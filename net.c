/*
    This file is part of telegram-client.

    Telegram-client is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Telegram-client is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this telegram-client.  If not, see <http://www.gnu.org/licenses/>.

    Copyright Vitaly Valtman 2013
*/

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <openssl/rand.h>
#include <arpa/inet.h>

#include "net.h"
#include "include.h"
#include "mtproto-client.h"
#include "tree.h"

#ifndef POLLRDHUP
#define POLLRDHUP 0
#endif

#define long_cmp(a,b) ((a) > (b) ? 1 : (a) == (b) ? 0 : -1)
DEFINE_TREE(long,long long,long_cmp,0)
double get_utime (int clock_id);

int verbosity;
extern struct connection_methods auth_methods;
//extern FILE *log_net_f;
FILE *log_net_f = 0;

void fail_connection (struct connection *c);

/*
 *
 */

#define PING_TIMEOUT 10

void start_ping_timer (struct connection *c);
int ping_alarm (struct connection *c) {
  assert (c->state == conn_ready || c->state == conn_connecting);
  if (get_double_time () - c->last_receive_time > 20 * PING_TIMEOUT) {
    warning ( "fail connection: reason: ping timeout\n");
    c->state = conn_failed;
    fail_connection (c);
  } else if (get_double_time () - c->last_receive_time > 5 * PING_TIMEOUT && c->state == conn_ready) {
    debug ("sending PING...\n");
    int x[3];
    x[0] = CODE_ping;
    *(long long *)(x + 1) = lrand48 () * (1ll << 32) + lrand48 ();
    encrypt_send_message (c->mtconnection, x, 3, 0);
    start_ping_timer (c);
  } else {
    start_ping_timer (c);
  }
  return 0;
}

void stop_ping_timer (struct connection *c) {
  if (c->ev.self) {
    remove_event_timer (c->instance, &c->ev);
  } else {
    warning ("trying to stop non-existing ping timer fd: #%d\n", c->fd);
  }
}

void start_ping_timer (struct connection *c) {
  c->ev.timeout = get_double_time () + PING_TIMEOUT;
  c->ev.alarm = (void *)ping_alarm;
  c->ev.self = c;
  insert_event_timer (c->instance, &c->ev);
}

void restart_connection (struct connection *c);
int fail_alarm (void *ev) {
  ((struct connection *)ev)->in_fail_timer = 0;
  restart_connection (ev);
  return 0;
}
void start_fail_timer (struct connection *c) {
  if (c->in_fail_timer) { return; }
  c->in_fail_timer = 1;  
  c->ev.timeout = get_double_time () + 10;
  c->ev.alarm = (void *)fail_alarm;
  c->ev.self = c;
  insert_event_timer (c->instance, &c->ev);
}

struct connection_buffer *new_connection_buffer (int size) {
  struct connection_buffer *b = talloc0 (sizeof (*b));
  b->start = talloc (size);
  b->end = b->start + size;
  b->rptr = b->wptr = b->start;
  return b;
}

void delete_connection_buffer (struct connection_buffer *b) {
  tfree (b->start, b->end - b->start);
  tfree (b, sizeof (*b));
}

int write_out (struct connection *c, const void *_data, int len) {
  const unsigned char *data = _data;
  if (!len) { return 0; }
  assert (len > 0);
  int x = 0;
  if (!c->out_head) {
    struct connection_buffer *b = new_connection_buffer (1 << 20);
    c->out_head = c->out_tail = b;
  }
  while (len) {
    if (c->out_tail->end - c->out_tail->wptr >= len) {
      memcpy (c->out_tail->wptr, data, len);
      c->out_tail->wptr += len;
      c->out_bytes += len;
      return x + len;
    } else {
      int y = c->out_tail->end - c->out_tail->wptr;
      assert (y < len);
      memcpy (c->out_tail->wptr, data, y);
      x += y;
      len -= y;
      data += y;
      struct connection_buffer *b = new_connection_buffer (1 << 20);
      c->out_tail->next = b;
      b->next = 0;
      c->out_tail = b;
      c->out_bytes += y;
    }
  }
  return x;
}

int read_in (struct connection *c, void *_data, int len) {
  unsigned char *data = _data;
  if (!len) { return 0; }
  assert (len > 0);
  if (len > c->in_bytes) {
    len = c->in_bytes;
  }
  int x = 0;
  while (len) {
    int y = c->in_head->wptr - c->in_head->rptr;
    if (y > len) {
      memcpy (data, c->in_head->rptr, len);
      c->in_head->rptr += len;
      c->in_bytes -= len;
      return x + len;
    } else {
      memcpy (data, c->in_head->rptr, y);
      c->in_bytes -= y;
      x += y;
      data += y;
      len -= y;
      void *old = c->in_head;
      c->in_head = c->in_head->next;
      if (!c->in_head) {
        c->in_tail = 0;
      }
      delete_connection_buffer (old);
    }
  }
  return x;
}

int read_in_lookup (struct connection *c, void *_data, int len) {
  unsigned char *data = _data;
  if (!len || !c->in_bytes) { return 0; }
  assert (len > 0);
  if (len > c->in_bytes) {
    len = c->in_bytes;
  }
  int x = 0;
  struct connection_buffer *b = c->in_head;
  while (len) {
    int y = b->wptr - b->rptr;
    if (y >= len) {
      memcpy (data, b->rptr, len);
      return x + len;
    } else {
      memcpy (data, b->rptr, y);
      x += y;
      data += y;
      len -= y;
      b = b->next;
    }
  }
  return x;
}

void flush_out (struct connection *c UU) {
}

#define MAX_CONNECTIONS 100
struct connection *Connections[MAX_CONNECTIONS];
int max_connection_fd;

void rotate_port (struct connection *c) {
  switch (c->port) {
  case 443:
    c->port = 80;
    break;
  case 80:
    c->port = 25;
    break;
  case 25:
    c->port = 443;
    break;
  }
}

void restart_connection (struct connection *c) {
  if (c->last_connect_time == time (0)) {
    start_fail_timer (c);
    return;
  }
  
  c->last_connect_time = time (0);
  int fd = socket (AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    debug ("Can not create socket: %m\n");
    exit (1);
  }
  assert (fd >= 0 && fd < MAX_CONNECTIONS);
  if (fd > max_connection_fd) {
    max_connection_fd = fd;
  }
  int flags = -1;
  setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof (flags));
  setsockopt (fd, SOL_SOCKET, SO_KEEPALIVE, &flags, sizeof (flags));
  setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof (flags));

  struct sockaddr_in addr;
  addr.sin_family = AF_INET; 
  addr.sin_port = htons (c->port);
  addr.sin_addr.s_addr = inet_addr (c->ip);


  fcntl (fd, F_SETFL, O_NONBLOCK);

  if (connect (fd, (struct sockaddr *) &addr, sizeof (addr)) == -1) {
    if (errno != EINPROGRESS) {
      debug ( "Can not connect to %s:%d %m\n", c->ip, c->port);
      start_fail_timer (c);
      close (fd);
      return;
    }
  }

  c->fd = fd;
  c->state = conn_connecting;
  c->last_receive_time = get_double_time ();
  start_ping_timer (c);
  Connections[fd] = c;
  
  char byte = 0xef;
  assert (write_out (c, &byte, 1) == 1);
  flush_out (c);
}

void fail_connection (struct connection *c) {
  warning ("Lost connection to server... %s:%d\n", c->ip, c->port);
  telegram_change_state(c->mtconnection->instance, STATE_ERROR, "Lost connection to server\n");
}

extern FILE *log_net_f;
int try_write (struct connection *c) {
  debug ( "try write: fd = %d\n", c->fd);
  int x = 0;
  while (c->out_head) {
    int r = write (c->fd, c->out_head->rptr, c->out_head->wptr - c->out_head->rptr);

	// Log all written packages
    if (r > 0 && log_net_f) {
      // fprintf (log_net_f, "%.02lf %d OUT %s:%d", get_utime (CLOCK_REALTIME), r, c->ip, c->port);
      int i;
      for (i = 0; i < r; i++) {
        // fprintf (log_net_f, " %02x", *(unsigned char *)(c->out_head->rptr + i));
      }
      fprintf (log_net_f, "\n");
      fflush (log_net_f);
    }
	
    if (r >= 0) {
      x += r;
      c->out_head->rptr += r;
      if (c->out_head->rptr != c->out_head->wptr) {
        break;
      }
      struct connection_buffer *b = c->out_head;
      c->out_head = b->next;
      if (!c->out_head) {
        c->out_tail = 0;
      }
      delete_connection_buffer (b);
    } else {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        debug ("fail_connection: write_error %m\n");
        fail_connection (c);
        return 0;
      } else {
        break;
      }
    }
  }
  debug ( "Sent %d bytes to %d\n", x, c->fd);
  c->out_bytes -= x;
  return x;
}

void hexdump_buf (struct connection_buffer *b) {
  // TODO: figure out how to log hexdumps to purple log
  int pos = 0;
  int rem = 8;
  while (b) { 
    unsigned char *c = b->rptr;
    while (c != b->wptr) {
      if (rem == 8) {
        //if (pos) { printf ("\n"); }
        //printf ("%04d", pos);
      }
      //printf (" %02x", (int)*c);
      rem --;
      pos ++;
      if (!rem) {
        rem = 8;
      }
      c ++;
    }
    b = b->next;
  }
  //printf ("\n");
    
}

void try_rpc_read (struct connection *c) {
  assert (c->in_head);
  if (verbosity >= 3) {
    hexdump_buf (c->in_head);
  }

  while (1) {
    if (c->in_bytes < 1) { return; }
    unsigned len = 0;
    unsigned t = 0;
    assert (read_in_lookup (c, &len, 1) == 1);
    if (len >= 1 && len <= 0x7e) {
      if (c->in_bytes < (int)(1 + 4 * len)) { return; }
    } else {
      if (c->in_bytes < 4) { return; }
      assert (read_in_lookup (c, &len, 4) == 4);
      len = (len >> 8);
      if (c->in_bytes < (int)(4 + 4 * len)) { return; }
      len = 0x7f;
    }

    if (len >= 1 && len <= 0x7e) {
      assert (read_in (c, &t, 1) == 1);    
      assert (t == len);
      assert (len >= 1);
    } else {
      assert (len == 0x7f);
      assert (read_in (c, &len, 4) == 4);
      len = (len >> 8);
      assert (len >= 1);
    }
    len *= 4;
    int op;
    assert (read_in_lookup (c, &op, 4) == 4);
    c->methods->execute (c, op, len);
  }
}

void try_read (struct connection *c) {
  debug ( "try read: fd = %d\n", c->fd);
  if (!c->in_tail) {
    c->in_head = c->in_tail = new_connection_buffer (1 << 20);
  }
  int x = 0;
  while (1) {
    int r = read (c->fd, c->in_tail->wptr, c->in_tail->end - c->in_tail->wptr);
    if (r > 0 && log_net_f) {
      fprintf (log_net_f, "%.02lf %d IN %s:%d", get_utime (CLOCK_REALTIME), r, c->ip, c->port);
      int i;
      for (i = 0; i < r; i++) {
        fprintf (log_net_f, " %02x", *(unsigned char *)(c->in_tail->wptr + i));
      }
      fprintf (log_net_f, "\n");
      fflush (log_net_f);
    }
    if (r > 0) {
      c->last_receive_time = get_double_time ();
      stop_ping_timer (c);
      start_ping_timer (c);
    }
    if (r >= 0) {
      c->in_tail->wptr += r;
      x += r;
      if (c->in_tail->wptr != c->in_tail->end) {
        break;
      }
      struct connection_buffer *b = new_connection_buffer (1 << 20);
      c->in_tail->next = b;
      c->in_tail = b;
    } else {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        debug ("fail_connection: read_error %m\n");
        fail_connection (c);
        return;
      } else {
        break;
      }
    }
  }
  debug ( "Received %d bytes from fd=#%d and DC %d(%s, %d)\n", x, 
    c->fd, c->session->dc->id, c->session->dc->ip, c->session->dc->port);
  c->in_bytes += x;
  if (x) {
    try_rpc_read (c);
  }
}

int send_all_acks (struct session *S) {
  info ("send_all_acks(dc=%d)\n", S->dc->id);
  if (!S->c) {
    warning ("WARNING: cannot send acks, session has no active connection");
    return -1;
  }
  struct mtproto_connection *mt = S->c->mtconnection;
  
  clear_packet (mt);
  out_int (mt, CODE_msgs_ack);
  out_int (mt, CODE_vector);
  out_int (mt, tree_count_long (S->ack_tree));
  while (S->ack_tree) {
    long long x = tree_get_min_long (S->ack_tree); 
    out_long (mt, x);
    S->ack_tree = tree_delete_long (S->ack_tree, x);
  }
  encrypt_send_message (mt, mt->packet_buffer, mt->packet_ptr - mt->packet_buffer, 0);
  return 0;
}

void insert_msg_id (struct session *S, long long id) {
  if (!S->ack_tree) {
    debug ("Creating ack_tree pointing to session %p\n");
    S->ev.alarm = (void *)send_all_acks;
    S->ev.self = (void *)S;
    S->ev.timeout = get_double_time () + ACK_TIMEOUT;
    insert_event_timer (S->c->instance, &S->ev);
  }
  if (!tree_lookup_long (S->ack_tree, id)) {
    S->ack_tree = tree_insert_long (S->ack_tree, id, lrand48 ());
  }
}

struct dc *alloc_dc (struct dc* DC_list[], int id, char *ip, int port UU) {
  assert (!DC_list[id]);
  struct dc *DC = talloc0 (sizeof (*DC));
  DC->id = id;
  DC->ip = ip;
  DC->port = port;
  DC_list[id] = DC;
  return DC;
}

/** 
 * Wrap an existing socket file descriptor and make it usable as a connection,
 */
struct connection *fd_create_connection (struct dc *DC, int fd, 
     struct telegram *instance, struct connection_methods *methods, 
     struct mtproto_connection *mtp) {
  
  // create a connection
  struct connection *c = talloc0 (sizeof (*c));
  c->fd = fd; 
  c->ip = tstrdup (DC->ip);
  c->flags = 0;
  c->state = conn_ready;
  c->port = DC->port;
  c->methods = methods;
  c->instance = instance;
  c->last_receive_time = get_double_time ();
  debug ( "connect to %s:%d successful\n", DC->ip, DC->port);

  if (!DC->sessions[0]) {
    struct session *S = talloc0 (sizeof (*S));
    assert (RAND_pseudo_bytes ((unsigned char *) &S->session_id, 8) >= 0);
    S->dc = DC;
    S->c = c;
    DC->sessions[0] = S;
  }
  if (!DC->sessions[0]->c) {
    DC->sessions[0]->c = c;
  }
  // add backreference to session
  c->session = DC->sessions[0];

  // add backreference to used mtproto-connection
  c->mtconnection = mtp;
  return c;
}

/** 
 * Close the connection by freeing all attached buffers and setting
 * the state to conn_stopped, but does NOT close the attached file descriptor
 */
void fd_close_connection(struct connection *c) {
  struct connection_buffer *b = c->out_head;
  while (b) {
    struct connection_buffer *d = b;
    b = b->next;
    delete_connection_buffer (d);
  }
  while (b) {
    struct connection_buffer *d = b;
    b = b->next;
    delete_connection_buffer (d);
  }
  c->out_head = c->out_tail = c->in_head = c->in_tail = 0;
  c->state = conn_stopped;
  c->out_bytes = c->in_bytes = 0;
  tfree(c, sizeof(struct connection));
}

