/*
    This file is part of telegram-client.

    struct telegram-client is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    struct telegram-client is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this telegram-client.  If not, see <http://www.gnu.org/licenses/>.

    Copyright Vitaly Valtman 2013
*/

#define _FILE_OFFSET_BITS 64
#include <string.h>
#include <memory.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/utsname.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "include.h"
#include "mtproto-client.h"
#include "queries.h"
#include "tree.h"
#include "loop.h"
#include "structures.h"
#include "net.h"
#include <openssl/bn.h>
#include <openssl/rand.h>
#include <openssl/aes.h>
#include <openssl/sha.h>
#include <openssl/md5.h>

#include "no-preview.h"
#include "binlog.h"
#include "telegram.h"
#include "msglog.h"
#include "purple-plugin/telegram-purple.h"

#define sha1 SHA1

#ifdef __APPLE__
#define OPEN_BIN "open %s"
#else
#define OPEN_BIN "xdg-open %s"
#endif

char *get_downloads_directory (void);
int verbosity;
int offline_mode = 0;

#define memcmp8(a,b) memcmp ((a), (b), 8)
DEFINE_TREE (query, struct query *, memcmp8, 0) ;

#define event_timer_cmp(a,b) ((a)->timeout > (b)->timeout ? 1 : ((a)->timeout < (b)->timeout ? -1 : (memcmp (a, b, sizeof (struct event_timer)))))
DEFINE_TREE (timer, struct event_timer *, event_timer_cmp, 0)

void out_peer_id (struct mtproto_connection *self, peer_id_t id);
#define QUERY_TIMEOUT 6.0

/**
 * Get the struct mtproto_connection connection this connection was attached to
 */
struct mtproto_connection *query_get_mtproto(struct query *q) { 
  return q->DC->sessions[0]->c->mtconnection; 
}

double get_double_time (void) {
  struct timespec tv;
  my_clock_gettime (CLOCK_REALTIME, &tv);
  return tv.tv_sec + 1e-9 * tv.tv_nsec;
}

struct query *query_get (struct telegram *instance, long long id) {
  return tree_lookup_query (instance->queries_tree, (void *)&id);
}

int alarm_query (struct query *q) {
  assert (q);
  struct mtproto_connection *mtp = query_get_mtproto(q);
  debug ("Alarm query %lld\n", q->msg_id);
  q->ev.timeout = get_double_time () + QUERY_TIMEOUT;
  insert_event_timer (mtp->connection->instance, &q->ev);

  if (q->session->c->out_bytes >= 100000) {
    return 0;
  }
  
  clear_packet (mtp);
  out_int (mtp, CODE_msg_container);
  out_int (mtp, 1);
  out_long (mtp, q->msg_id);
  out_int (mtp, q->seq_no);
  out_int (mtp, 4 * q->data_len);
  out_ints (mtp, q->data, q->data_len);
  
  encrypt_send_message (mtp, mtp->packet_buffer, mtp->packet_ptr - mtp->packet_buffer, 0);
  return 0;
}

void query_restart (struct telegram *instance, long long id) {
  struct query *q = query_get (instance, id);
  if (q) {
    remove_event_timer (instance, &q->ev);
    alarm_query (q);
  }
}

struct query *send_query (struct telegram *instance, struct dc *DC, int ints, void *data, struct query_methods *methods, void *extra) {
  info ("SEND_QUERY() size %d to DC %d(%s:%d)\n", 4 * ints, DC->id, DC->ip, DC->port);
  struct query *q = talloc0 (sizeof (*q));
  q->data_len = ints;
  q->data = talloc (4 * ints);
  memcpy (q->data, data, 4 * ints);
  q->msg_id = encrypt_send_message (DC->sessions[0]->c->mtconnection, data, ints, 1);
  q->session = DC->sessions[0];
  q->seq_no = DC->sessions[0]->seq_no - 1; 
  //debug ( "Msg_id is %lld %p\n", q->msg_id, q);
  q->methods = methods;
  q->DC = DC;
  if (instance->queries_tree) {
    if (verbosity >= 2) {
      debug ( "%lld %lld\n", q->msg_id, instance->queries_tree->x->msg_id);
    }
  }

  instance->queries_tree = tree_insert_query (instance->queries_tree, q, lrand48 ());
  struct mtproto_connection *mtp = query_get_mtproto(q);
  ++ mtp->queries_num;

  q->ev.alarm = (void *)alarm_query;
  q->ev.timeout = get_double_time () + QUERY_TIMEOUT;
  q->ev.self = (void *)q;
  insert_event_timer (instance, &q->ev);

  q->extra = extra;
  return q;
}

void query_ack (struct telegram *instance, long long id) {
  struct query *q = query_get (instance, id);
  if (q && !(q->flags & QUERY_ACK_RECEIVED)) { 
    assert (q->msg_id == id);
    q->flags |= QUERY_ACK_RECEIVED; 
    remove_event_timer (instance, &q->ev);
  }
}

void query_error (struct telegram *instance, long long id) {
  struct query *q = query_get (instance, id);
  struct mtproto_connection *mtp = query_get_mtproto(q);

  assert (fetch_int (mtp) == CODE_rpc_error);
  int error_code = fetch_int (mtp);
  int error_len = prefetch_strlen (mtp);
  char *err = fetch_str (mtp, error_len);
  failure ( "error for query #%lld: #%d :%.*s\n", id, error_code, error_len, err);
  if (!q) {
    failure ( "No such query\n");
  } else {
    if (!(q->flags & QUERY_ACK_RECEIVED)) {
      remove_event_timer (instance, &q->ev);
    }
    instance->queries_tree = tree_delete_query (instance->queries_tree, q);
    -- mtp->queries_num;

    if (q->methods && q->methods->on_error) {
      q->methods->on_error (q, error_code, error_len, err);
    } else {
      failure ( "error for query #%lld: #%d :%.*s\n", id, error_code, error_len, err);
    }
    tfree (q->data, q->data_len * 4);
    tfree (q, sizeof (*q));
    return;
  }

}

void query_result (struct telegram *instance, long long id UU) {
  struct query *q = query_get (instance, id);
  struct mtproto_connection *mtp = query_get_mtproto(q);

  debug ( "result for query #%lld\n", id);
  if (verbosity  >= 4) {
    debug ( "result: ");
    hexdump_in (mtp);
  }

  int op = prefetch_int (mtp);
  int *end = 0;
  int *eend = 0;
  if (op == CODE_gzip_packed) {
    fetch_int (mtp);
    int l = prefetch_strlen (mtp);
    char *s = fetch_str (mtp, l);
    int total_out = tinflate (s, l, instance->packed_buffer, MAX_PACKED_SIZE);
    end = mtp->in_ptr;
    eend = mtp->in_end;
    //assert (total_out % 4 == 0);
    mtp->in_ptr = instance->packed_buffer;
    mtp->in_end = mtp->in_ptr + total_out / 4;
    if (verbosity >= 4) {
      debug ( "Unzipped data: ");
      hexdump_in (mtp);
    }
  }
  if (!q) {
    warning ( "No such query\n");
    mtp->in_ptr = mtp->in_end;
  } else {
    if (!(q->flags & QUERY_ACK_RECEIVED)) {
      remove_event_timer (instance, &q->ev);
    }
    instance->queries_tree = tree_delete_query (instance->queries_tree, q);
    debug("queries_num: %d\n", -- mtp->queries_num);

    if (q->methods && q->methods->on_answer) {
      q->methods->on_answer (q);
      assert (mtp->in_ptr == mtp->in_end);
    }
    tfree (q->data, 4 * q->data_len);
    tfree (q, sizeof (*q));
  }
  if (end) {
    mtp->in_ptr = end;
    mtp->in_end = eend;
  }
} 


void insert_event_timer (struct telegram *instance, struct event_timer *ev) {
  //  debug ( "INSERT: %lf %p %p\n", ev->timeout, ev->self, ev->alarm);
  instance->timer_tree = tree_insert_timer (instance->timer_tree, ev, lrand48 ());
}

void remove_event_timer (struct telegram *instance, struct event_timer *ev) {
  //  debug ( "REMOVE: %lf %p %p\n", ev->timeout, ev->self, ev->alarm);
  instance->timer_tree = tree_delete_timer (instance->timer_tree, ev);
}

double next_timer_in (struct telegram *instance) {
  if (!instance->timer_tree) { return 1e100; }
  return tree_get_min_timer (instance->timer_tree)->timeout;
}

void work_timers (struct telegram *instance) {
  debug ("work_timers ()\n");
  double t = get_double_time ();
  while (instance->timer_tree) {
    struct event_timer *ev = tree_get_min_timer (instance->timer_tree);
    assert (ev);
    if (ev->timeout > t) { break; }
    remove_event_timer (instance, ev);
    assert (ev->alarm);
    debug ("Alarm\n");
    ev->alarm (ev->self);
  }
}

void free_timers (struct telegram *instance)
{
  while (instance->timer_tree) {
    struct event_timer *ev = tree_get_min_timer (instance->timer_tree);
    assert (ev);
    debug ("freeing event timer with timeout: %d\n", ev->timeout);
    remove_event_timer (instance, ev);
    //tfree (ev, sizeof(struct event_timer));
  }
}

void free_queries (struct telegram *instance)
{
  while (instance->queries_tree) {
    struct query *q = tree_get_min_query (instance->queries_tree);
    assert (q);
    debug ("freeing query with msg_id %d and len\n", q->msg_id, q->data_len);
    tfree (q->data, 4 * q->data_len);
    instance->queries_tree = tree_delete_query (instance->queries_tree, q);
    //tfree (q, sizeof (struct query));
  }
}

//extern struct dc *DC_list[];
//extern struct dc *DC_working;

void out_random (struct mtproto_connection *mtp, int n) {
  assert (n <= 32);
  static char buf[32];
  secure_random (buf, n);
  out_cstring (mtp, buf, n);
}

int allow_send_linux_version;
void do_insert_header (struct mtproto_connection *mtp) {
  out_int (mtp, CODE_invoke_with_layer12);  
  out_int (mtp, CODE_init_connection);
  out_int (mtp, TG_APP_ID);
  if (allow_send_linux_version) {
    struct utsname st;
    uname (&st);
    out_string (mtp, st.machine);
    static char buf[4096];
    tsnprintf (buf, sizeof (buf), "%.999s %.999s %.999s\n", st.sysname, st.release, st.version);
    out_string (mtp, buf);
    out_string (mtp, TG_VERSION " (build " TG_BUILD ")");
    out_string (mtp, "En");
  } else { 
    out_string (mtp, "x86");
    out_string (mtp, "Linux");
    out_string (mtp, TG_VERSION);
    out_string (mtp, "en");
  }
}

/* {{{ Get config */

void fetch_dc_option (struct telegram *instance) {
  info ("fetch_dc_option()\n");
  struct mtproto_connection *mtp = instance->connection;
  
  assert (fetch_int (mtp) == CODE_dc_option);
  int id = fetch_int (mtp);
  int l1 = prefetch_strlen (mtp);
  char *name = fetch_str (mtp, l1);
  int l2 = prefetch_strlen (mtp);
  char *ip = fetch_str (mtp, l2);
  int port = fetch_int (mtp);
  debug ( "id = %d, name = %.*s ip = %.*s port = %d\n", id, l1, name, l2, ip, port);

  bl_do_dc_option (mtp->bl, mtp, id, l1, name, l2, ip, port, instance);
}

int help_get_config_on_answer (struct query *q UU) {
  struct telegram *instance = q->extra;
  struct mtproto_connection *mtp = query_get_mtproto(q);

  unsigned op = fetch_int (mtp);
  assert (op == CODE_config || op == CODE_config_old);
  fetch_int (mtp);

  unsigned test_mode = fetch_int (mtp);
  assert (test_mode == CODE_bool_true || test_mode == CODE_bool_false);
  assert (test_mode == CODE_bool_false || test_mode == CODE_bool_true);
  int this_dc = fetch_int (mtp);
  debug ( "this_dc = %d\n", this_dc);
  assert (fetch_int (mtp) == CODE_vector);
  int n = fetch_int (mtp);
  assert (n <= 10);
  int i;
  for (i = 0; i < n; i++) {
    fetch_dc_option (instance);
  }
  instance->max_chat_size = fetch_int (mtp);
  if (op == CODE_config) {
    instance->max_bcast_size = fetch_int (mtp);
  }
  debug ( "max_chat_size = %d\n", instance->max_chat_size);

  telegram_change_state(instance, STATE_CONFIG_RECEIVED, NULL);
  return 0;
}

struct query_methods help_get_config_methods  = {
  .on_answer = help_get_config_on_answer
};

void do_help_get_config (struct telegram *instance) {
  info ("do_help_get_config()\n");
  struct mtproto_connection *mtp = instance->connection;

  debug ("mtp: %p:%p\n", mtp->packet_ptr, mtp->packet_buffer);
  clear_packet (mtp);  
  out_int (mtp, CODE_help_get_config);
  struct dc *DC_working = telegram_get_working_dc(instance);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, 
    mtp->packet_buffer, &help_get_config_methods, instance);
}
/* }}} */

/* {{{ Send code */
int send_code_on_answer (struct query *q UU) {
  struct telegram *instance = q->extra;
  struct mtproto_connection *mtp = query_get_mtproto(q);

  assert (fetch_int (mtp) == (int)CODE_auth_sent_code);
  fetch_bool (mtp);
  int l = prefetch_strlen (mtp);
  char *phone_code_hash = tstrndup (fetch_str (mtp, l), l);
  instance->phone_code_hash = phone_code_hash;
  debug("telegram: phone_code_hash: %s\n", phone_code_hash);
  fetch_int (mtp); 
  fetch_bool (mtp);
  instance->want_dc_num = -1;
  if (instance->session_state == STATE_PHONE_CODE_REQUESTED) {
    telegram_change_state(instance, STATE_PHONE_CODE_NOT_ENTERED, NULL); 
  } else if (instance->session_state == STATE_CLIENT_CODE_REQUESTED) {
    telegram_change_state(instance, STATE_CLIENT_CODE_NOT_ENTERED, NULL); 
  } else {
    debug("send_code_on_answer(): Invalid State %d ", instance->session_state);
    telegram_change_state(instance, STATE_ERROR, NULL);
  }
  return 0;
}

int send_code_on_error (struct query *q UU, int error_code, int l, char *error) {
  struct telegram *tg = q->extra;

  int s = strlen ("PHONE_MIGRATE_");
  int s2 = strlen ("NETWORK_MIGRATE_");
  if (l >= s && !memcmp (error, "PHONE_MIGRATE_", s)) {
    int want_dc_num = error[s] - '0';
    tg->auth.dc_working_num = want_dc_num;
    telegram_change_state(tg, STATE_DISCONNECTED_SWITCH_DC, error);
  } else if (l >= s2 && !memcmp (error, "NETWORK_MIGRATE_", s2)) {
    int want_dc_num = error[s2] - '0';
    tg->auth.dc_working_num = want_dc_num;
    telegram_change_state(tg, STATE_DISCONNECTED_SWITCH_DC, error);
  } else {
    fatal ( "error_code = %d, error = %.*s\n", error_code, l, error);
    telegram_change_state(tg, STATE_ERROR, error);
  }
  return 0;
}

struct query_methods send_code_methods  = {
  .on_answer = send_code_on_answer,
  .on_error = send_code_on_error
};

void do_send_code (struct telegram *instance, const char *user) {
  info ("do_send_code()\n");
  struct mtproto_connection *mtp = instance->connection;

  instance->suser = tstrdup (user);
  instance->want_dc_num = 0;
  clear_packet (mtp);
  do_insert_header (mtp);
  out_int (mtp, CODE_auth_send_code);
  out_string (mtp, user);
  out_int (mtp, 0);
  out_int (mtp, TG_APP_ID);
  out_string (mtp, TG_APP_HASH);
  out_string (mtp, "en");

  debug ("send_code: dc_num = %d\n", instance->auth.dc_working_num);
  send_query (instance, telegram_get_working_dc(instance), mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &send_code_methods, instance);
  if (instance->session_state == STATE_PHONE_NOT_REGISTERED) {
    telegram_change_state(instance, STATE_PHONE_CODE_REQUESTED, NULL);
  } else if (instance->session_state == STATE_CLIENT_NOT_REGISTERED) {
    telegram_change_state(instance, STATE_CLIENT_CODE_REQUESTED, NULL);
  } else {
    fatal ("do_send_code() Invalid State %d, erroring\n", instance->session_state);
    telegram_change_state(instance, STATE_ERROR, NULL);
  }
}


int phone_call_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);

  fetch_bool (mtp);
  return 0;
}

int phone_call_on_error (struct query *q UU, int error_code, int l, char *error) {
  fatal ( "error_code = %d, error = %.*s\n", error_code, l, error);
  telegram_change_state(q->data, STATE_ERROR, error);
  return 0;
}

struct query_methods phone_call_methods  = {
  .on_answer = phone_call_on_answer,
  .on_error = phone_call_on_error
};

void do_phone_call (struct telegram *instance, const char *user) {
  struct mtproto_connection *mtp = instance->connection;
 
  debug ("calling user\n");
  instance->suser = tstrdup (user);
  instance->want_dc_num = 0;
  clear_packet (mtp);
  do_insert_header (mtp);
  out_int (mtp, CODE_auth_send_call);
  out_string (mtp, user);
  out_string (mtp, instance->phone_code_hash);

  info ("do_phone_call: dc_num = %d\n", instance->auth.dc_working_num);
  send_query (instance, telegram_get_working_dc(instance), mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &phone_call_methods, instance);
}
/* }}} */

/* {{{ Check phone */
int check_phone_result;
int cr_f (void) {
  return check_phone_result >= 0;
}

int check_phone_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);

  assert (fetch_int (mtp) == (int)CODE_auth_checked_phone);
  check_phone_result = fetch_bool (mtp);
  fetch_bool (mtp);

  assert (mtp->connection->instance->session_state == STATE_CONFIG_RECEIVED);
  debug ("check_phone_result=%d\n", check_phone_result);
  telegram_change_state (mtp->connection->instance, 
     check_phone_result ? STATE_CLIENT_NOT_REGISTERED : STATE_PHONE_NOT_REGISTERED, NULL);
  return 0;
}

int check_phone_on_error (struct query *q UU, int error_code, int l, char *error) {
  int s = strlen ("PHONE_MIGRATE_");
  int s2 = strlen ("NETWORK_MIGRATE_");
  struct telegram* instance = q->extra;

  if (l >= s && !memcmp (error, "PHONE_MIGRATE_", s)) {
    // update used data centre
    int i = error[s] - '0';
    instance->auth.dc_working_num = i;

    //bl_do_set_working_dc (i);
    //check_phone_result = 1;
  } else if (l >= s2 && !memcmp (error, "NETWORK_MIGRATE_", s2)) {
    // update used data centre
    int i = error[s2] - '0';
    instance->auth.dc_working_num = i;
    //bl_do_set_working_dc (i);

    //check_phone_result = 1;
  } else {
    failure ( "error_code = %d, error = %.*s\n", error_code, l, error);
    telegram_change_state(instance, STATE_ERROR, error);
    return -1;
  }
  telegram_change_state(instance,
    STATE_DISCONNECTED_SWITCH_DC, &instance->auth.dc_working_num);
  return 0;
}

struct query_methods check_phone_methods = {
  .on_answer = check_phone_on_answer,
  .on_error = check_phone_on_error
};

void do_auth_check_phone (struct telegram *instance, const char *user) {
  struct mtproto_connection *mtp = instance->connection;

  instance->suser = tstrdup (user);
  clear_packet (mtp);
  out_int (mtp, CODE_auth_check_phone);
  out_string (mtp, user);
  check_phone_result = -1;
  struct dc *DC_working = telegram_get_working_dc(instance);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, 
    &check_phone_methods, instance);
}
/* }}} */

/* {{{ Nearest DC */

int nearest_dc_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
  struct telegram *instance = mtp->connection->instance;

  assert (fetch_int (mtp) == (int)CODE_nearest_dc);
  char *country = fetch_str_dup (mtp);
  debug ("Server thinks that you are in %s\n", country);
  fetch_int (mtp); // this_dc
  instance->nearest_dc_num = fetch_int (mtp);
  assert (instance->nearest_dc_num >= 0);
  return 0;
}

int fail_on_error (struct query *q UU, int error_code UU, int l UU, char *error UU) {
  fatal ("error #%d: %.*s\n", error_code, l, error);
  telegram_change_state(q->data, STATE_ERROR, error);
  return 0;
}

struct query_methods nearest_dc_methods = {
  .on_answer = nearest_dc_on_answer,
  .on_error = fail_on_error
};

void do_get_nearest_dc (struct telegram *instance) {
  struct mtproto_connection *mtp = instance->connection;
  struct dc *DC_working = telegram_get_working_dc(instance);
  clear_packet (mtp);
  out_int (mtp, CODE_help_get_nearest_dc);
  instance->nearest_dc_num = -1;
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &nearest_dc_methods, instance);
  //net_loop (0, nr_f);
  //return nearest_dc_num;
}
/* }}} */

/* {{{ Sign in / Sign up */

int sign_in_on_answer (struct query *q) {
  info ("sign_in_on_answer()\n");
  struct mtproto_connection *mtp = query_get_mtproto(q);
  struct telegram *instance = mtp->connection->instance;

  struct dc *DC_working = telegram_get_working_dc(mtp->connection->instance);
  assert (fetch_int (mtp) == (int)CODE_auth_authorization);
  int expires = fetch_int (mtp);
  fetch_user (mtp, &instance->User);
  if (!instance->our_id) {
    instance->our_id = get_peer_id (instance->User.id);
    bl_do_set_our_id (mtp->bl, mtp, instance->our_id);
  }
  debug ( "telegram: authorized successfully: name = '%s %s', phone = '%s', expires = %d\n", 
    instance->User.first_name, instance->User.last_name, instance->User.phone, (int)(expires - get_double_time ()));
  DC_working->has_auth = 1;

  bl_do_dc_signed (mtp->bl, mtp, DC_working->id);
  telegram_change_state (mtp->connection->instance, STATE_READY, NULL);
  return 0;
}

int sign_in_on_error (struct query *q UU, int error_code, int l, char *error) {
  info ("sign_in_on_error()\n");
  struct mtproto_connection *mtp = query_get_mtproto(q);
  failure ( "error_code = %d, error = %.*s\n", error_code, l, error);
  int state = STATE_CLIENT_CODE_NOT_ENTERED;
  if (mtp->instance->session_state == STATE_PHONE_CODE_NOT_ENTERED) {
     state = STATE_PHONE_CODE_NOT_ENTERED;
  }
  telegram_change_state (mtp->connection->instance, state, NULL);
  return 0;
}

struct query_methods sign_in_methods  = {
  .on_answer = sign_in_on_answer,
  .on_error = sign_in_on_error
};

void do_send_code_result (struct telegram *instance, const char *code) {
  info ("do_send_code_result()\n");
  struct mtproto_connection *mtp = instance->connection;
  assert (instance->session_state == STATE_CLIENT_CODE_NOT_ENTERED);
 
  struct dc *DC_working = telegram_get_working_dc(instance);
  clear_packet (mtp);
  out_int (mtp, CODE_auth_sign_in);
  out_string (mtp, instance->suser);
  out_string(mtp, instance->phone_code_hash);
  out_string (mtp, code);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, 
    &sign_in_methods, instance);
}

void do_send_code_result_auth (struct telegram *instance, const char *code, const char *first_name, const char *last_name) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;

  clear_packet (mtp);
  out_int (mtp, CODE_auth_sign_up);
  out_string (mtp, instance->suser);
  out_string (mtp, instance->phone_code_hash);
  out_string (mtp, code);
  out_string (mtp, first_name);
  out_string (mtp, last_name);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &sign_in_methods, instance);
}
/* }}} */

/* {{{ Get contacts */
extern char *user_list[];

int get_contacts_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
  int i;
  assert (fetch_int (mtp) == (int)CODE_contacts_contacts);
  assert (fetch_int (mtp) == CODE_vector);
  int n = fetch_int (mtp);
  for (i = 0; i < n; i++) {
    assert (fetch_int (mtp) == (int)CODE_contact);
    fetch_int (mtp); // id
    fetch_int (mtp); // mutual
  }
  assert (fetch_int (mtp) == CODE_vector);
  n = fetch_int (mtp);
  for (i = 0; i < n; i++) {
    fetch_alloc_user (mtp);
  }
  return 0;
}

struct query_methods get_contacts_methods = {
  .on_answer = get_contacts_on_answer,
};

void do_update_contact_list (struct telegram *instance) {
  struct mtproto_connection *mtp = instance->connection;
  struct dc *DC_working = telegram_get_working_dc(instance);

  clear_packet (mtp);
  out_int (mtp, CODE_contacts_get_contacts);
  out_string (mtp, "");
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &get_contacts_methods, instance);
}


/* }}} */

/* {{{ Encrypt decrypted */

char *encrypt_decrypted_message (struct mtproto_connection *mtp, struct secret_chat *E) {
  static int msg_key[4];
  static unsigned char sha1a_buffer[20];
  static unsigned char sha1b_buffer[20];
  static unsigned char sha1c_buffer[20];
  static unsigned char sha1d_buffer[20];
  int x = *(mtp->encr_ptr);  
  assert (x >= 0 && !(x & 3));
  sha1 ((void *)mtp->encr_ptr, 4 + x, sha1a_buffer);
  memcpy (msg_key, sha1a_buffer + 4, 16);
 
  static unsigned char buf[64];
  memcpy (buf, msg_key, 16);
  memcpy (buf + 16, E->key, 32);
  sha1 (buf, 48, sha1a_buffer);
  
  memcpy (buf, E->key + 8, 16);
  memcpy (buf + 16, msg_key, 16);
  memcpy (buf + 32, E->key + 12, 16);
  sha1 (buf, 48, sha1b_buffer);
  
  memcpy (buf, E->key + 16, 32);
  memcpy (buf + 32, msg_key, 16);
  sha1 (buf, 48, sha1c_buffer);
  
  memcpy (buf, msg_key, 16);
  memcpy (buf + 16, E->key + 24, 32);
  sha1 (buf, 48, sha1d_buffer);

  static unsigned char key[32];
  memcpy (key, sha1a_buffer + 0, 8);
  memcpy (key + 8, sha1b_buffer + 8, 12);
  memcpy (key + 20, sha1c_buffer + 4, 12);

  static unsigned char iv[32];
  memcpy (iv, sha1a_buffer + 8, 12);
  memcpy (iv + 12, sha1b_buffer + 0, 8);
  memcpy (iv + 20, sha1c_buffer + 16, 4);
  memcpy (iv + 24, sha1d_buffer + 0, 8);

  AES_KEY aes_key;
  AES_set_encrypt_key (key, 256, &aes_key);
  AES_ige_encrypt ((void *)mtp->encr_ptr, (void *)mtp->encr_ptr, 4 * (mtp->encr_end - mtp->encr_ptr), &aes_key, iv, 1);
  memset (&aes_key, 0, sizeof (aes_key));

  return (void *)msg_key;
}

void encr_start (struct mtproto_connection *mtp) {
  mtp->encr_extra = mtp->packet_ptr;
  mtp->packet_ptr += 1; // str len
  mtp->packet_ptr += 2; // fingerprint
  mtp->packet_ptr += 4; // msg_key
  mtp->packet_ptr += 1; // len
}


void encr_finish (struct mtproto_connection *mtp, struct secret_chat *E) {
  int l = mtp->packet_ptr - (mtp->encr_extra +  8);
  while (((mtp->packet_ptr - mtp->encr_extra) - 3) & 3) {  
    int t;
    secure_random (&t, 4);
    out_int (mtp, t);
  }

  *mtp->encr_extra = ((mtp->packet_ptr - mtp->encr_extra) - 1) * 4 * 256 + 0xfe;
  mtp->encr_extra ++;
  *(long long *)mtp->encr_extra = E->key_fingerprint;
  mtp->encr_extra += 2;
  mtp->encr_extra[4] = l * 4;
  mtp->encr_ptr = mtp->encr_extra + 4;
  mtp->encr_end = mtp->packet_ptr;
  memcpy (mtp->encr_extra, encrypt_decrypted_message (mtp, E), 16);
}
/* }}} */

/* {{{ Seng msg (plain text) */
int msg_send_encr_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
  assert (fetch_int (mtp) == CODE_messages_sent_encrypted_message);
  debug ("Sent\n");
  struct message *M = q->extra;
  //M->date = fetch_int (mtp);
  fetch_int (mtp);
  bl_do_set_message_sent (mtp->bl, mtp, M);
  return 0;
}

int msg_send_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
 
  unsigned x = fetch_int (mtp);
  assert (x == CODE_messages_sent_message || x == CODE_messages_sent_message_link);
  int id = fetch_int (mtp); // id
  struct message *M = q->extra;
  bl_do_set_msg_id (mtp->bl, mtp, M, id);
  fetch_date (mtp);
  fetch_pts (mtp);
  fetch_seq (mtp);
  if (x == CODE_messages_sent_message_link) {
    assert (fetch_int (mtp) == CODE_vector);
    int n = fetch_int (mtp);
    int i;
    unsigned a, b;
    for (i = 0; i < n; i++) {
      assert (fetch_int (mtp) == (int)CODE_contacts_link);
      a = fetch_int (mtp);
      assert (a == CODE_contacts_my_link_empty || a == CODE_contacts_my_link_requested || a == CODE_contacts_my_link_contact);
      if (a == CODE_contacts_my_link_requested) {
        fetch_bool (mtp);
      }
      b = fetch_int (mtp);
      assert (b == CODE_contacts_foreign_link_unknown || b == CODE_contacts_foreign_link_requested || b == CODE_contacts_foreign_link_mutual);
      if (b == CODE_contacts_foreign_link_requested) {
        fetch_bool (mtp);
      }
      struct tgl_user *U = fetch_alloc_user (mtp);
  
      U->flags &= ~(FLAG_USER_IN_CONTACT | FLAG_USER_OUT_CONTACT);
      if (a == CODE_contacts_my_link_contact) {
        U->flags |= FLAG_USER_IN_CONTACT; 
      }
      U->flags &= ~(FLAG_USER_IN_CONTACT | FLAG_USER_OUT_CONTACT);
      if (b == CODE_contacts_foreign_link_mutual) {
        U->flags |= FLAG_USER_IN_CONTACT | FLAG_USER_OUT_CONTACT; 
      }
      if (b == CODE_contacts_foreign_link_requested) {
        U->flags |= FLAG_USER_OUT_CONTACT;
      }
      //print_start ();
      //push_color (COLOR_YELLOW);
      debug ("Link with user ");
      //print_user_name (U->id, (void *)U);
      debug (" changed\n");
      //pop_color ();
      //print_end ();
    }
  }
  debug ("Sent: id = %d\n", id);
  bl_do_set_message_sent (mtp->bl, mtp, M);
  return 0;
}

int msg_send_on_error (struct query *q, int error_code, int error_len, char *error) {
  struct mtproto_connection *mtp = query_get_mtproto(q);

  debug ( "error for query #%lld: #%d :%.*s\n", q->msg_id, error_code, error_len, error);
  struct message *M = q->extra;
  bl_do_delete_msg (mtp->bl, mtp, M);
  return 0;
}

struct query_methods msg_send_methods = {
  .on_answer = msg_send_on_answer,
  .on_error = msg_send_on_error
};

struct query_methods msg_send_encr_methods = {
  .on_answer = msg_send_encr_on_answer
};

void do_send_encr_msg (struct telegram *instance, struct message *M) {
  struct mtproto_connection *mtp = instance->connection;
  struct dc *DC_working = telegram_get_working_dc(instance);

  peer_t *P = user_chat_get (mtp->bl, M->to_id);
  if (!P || P->encr_chat.state != sc_ok) { return; }
  
  clear_packet (mtp);
  out_int (mtp, CODE_messages_send_encrypted);
  out_int (mtp, CODE_input_encrypted_chat);
  out_int (mtp, get_peer_id (M->to_id));
  out_long (mtp, P->encr_chat.access_hash);
  out_long (mtp, M->id);
  encr_start (mtp);
  out_int (mtp, CODE_decrypted_message);
  out_long (mtp, M->id);
  static int buf[4];
  secure_random (buf, 16);
  out_cstring (mtp, (void *)buf, 16);
  out_cstring (mtp, (void *)M->message, M->message_len);
  out_int (mtp, CODE_decrypted_message_media_empty);
  encr_finish (mtp, &P->encr_chat);
  
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, 
    &msg_send_encr_methods, M);
}

void do_send_msg (struct telegram *instance, struct message *M) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;

  if (get_peer_type (M->to_id) == PEER_ENCR_CHAT) {
    do_send_encr_msg (instance ,M);
    return;
  }
  clear_packet (mtp);
  out_int (mtp, CODE_messages_send_message);
  out_peer_id (mtp, M->to_id);
  out_cstring (mtp, M->message, M->message_len);
  out_long (mtp, M->id);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &msg_send_methods, M);
}

void do_send_message (struct telegram *instance, peer_id_t id, const char *msg, int len) {
  struct mtproto_connection *mtp = instance->connection;
  if (get_peer_type (id) == PEER_ENCR_CHAT) {
    peer_t *P = user_chat_get (mtp->bl, id);
    if (!P) {
      warning ("Can not send to unknown encrypted chat\n");
      return;
    }
    if (P->encr_chat.state != sc_ok) {
      warning ("Chat is not yet initialized\n");
      return;
    }
  }
  long long t;
  secure_random (&t, 8);
  debug ("t = %lld, len = %d\n", t, len);
  bl_do_send_message_text (mtp->bl, mtp, t, instance->our_id, get_peer_type (id), get_peer_id (id), time (0), len, msg);
  struct message *M = message_get (mtp->bl, t);
  assert (M);
  do_send_msg (instance, M);
  //print_message (M);
}
/* }}} */

/* {{{ Send text file */
void do_send_text (struct telegram *instance, peer_id_t id, char *file_name) {
  int fd = open (file_name, O_RDONLY);
  if (fd < 0) {
    warning ("No such file '%s'\n", file_name);
    tfree_str (file_name);
    return;
  }
  static char buf[(1 << 20) + 1];
  int x = read (fd, buf, (1 << 20) + 1);
  assert (x >= 0);
  if (x == (1 << 20) + 1) {
    warning ("Too big file '%s'\n", file_name);
    tfree_str (file_name);
    close (fd);
  } else {
    buf[x] = 0;
    do_send_message (instance, id, buf, x);
    tfree_str (file_name);
    close (fd);
  }
}
/* }}} */

/* {{{ Mark read */
int mark_read_on_receive (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);

  assert (fetch_int (mtp) == (int)CODE_messages_affected_history);
  fetch_pts (mtp);
  fetch_seq (mtp);
  fetch_int (mtp); // offset
  return 0;
}

int mark_read_encr_on_receive (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
  fetch_bool (mtp);
  return 0;
}

struct query_methods mark_read_methods = {
  .on_answer = mark_read_on_receive
};

struct query_methods mark_read_encr_methods = {
  .on_answer = mark_read_encr_on_receive
};

void do_messages_mark_read (struct telegram *instance, peer_id_t id, int max_id) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;

  clear_packet (mtp);
  out_int (mtp, CODE_messages_read_history);
  out_peer_id (mtp, id);
  out_int (mtp, max_id);
  out_int (mtp, 0);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &mark_read_methods, 0);
}

void do_messages_mark_read_encr (struct telegram *instance, peer_id_t id, long long access_hash, int last_time) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;

  clear_packet (mtp);
  out_int (mtp, CODE_messages_read_encrypted_history);
  out_int (mtp, CODE_input_encrypted_chat);
  out_int (mtp, get_peer_id (id));
  out_long (mtp, access_hash);
  out_int (mtp, last_time);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &mark_read_encr_methods, 0);
}

void do_mark_read (struct telegram *instance, peer_id_t id) {
  struct mtproto_connection *mtp = instance->connection;

  peer_t *P = user_chat_get (mtp->bl, id);
  if (!P) {
    debug ("Unknown peer\n");
    return;
  }
  if (get_peer_type (id) == PEER_USER || get_peer_type (id) == PEER_CHAT) {
    if (!P->last) {
      debug ("Unknown last peer message\n");
      return;
    }
    do_messages_mark_read (instance, id, P->last->id);
    return;
  }
  assert (get_peer_type (id) == PEER_ENCR_CHAT);
  if (P->last) {
    do_messages_mark_read_encr (instance, id, P->encr_chat.access_hash, P->last->date);
  } else {
    do_messages_mark_read_encr (instance, id, P->encr_chat.access_hash, time (0) - 10);
    
  }
}
/* }}} */

struct get_hist_extra {
  struct telegram *instance;
  peer_id_t peer_id;
};

/* {{{ Get history */
int get_history_on_answer (struct query *q UU) {
  struct get_hist_extra *extra = q->extra;
  struct telegram *instance = extra->instance;
  struct mtproto_connection *mtp = query_get_mtproto(q);
  peer_id_t peer_id = extra->peer_id;

  static struct message *ML[10000];
  int i;
  int x = fetch_int (mtp);
  if (x == (int)CODE_messages_messages_slice) {
    fetch_int (mtp);
    debug ("...\n");
  } else {
    assert (x == (int)CODE_messages_messages);
  }
  assert (fetch_int (mtp) == CODE_vector);
  int n = fetch_int (mtp);
  for (i = 0; i < n; i++) {
    struct message *M = fetch_alloc_message (mtp, instance);
    if (i <= 9999) {
      ML[i] = M;
    }
  }
  if (n > 10000) { n = 10000; }
  int sn = n;
  for (i = n - 1; i >= 0; i--) {
    //print_message (ML[i]);
  }
  assert (fetch_int (mtp) == CODE_vector);
  n = fetch_int (mtp);
  for (i = 0; i < n; i++) {
    fetch_alloc_chat (mtp);
  }
  assert (fetch_int (mtp) == CODE_vector);
  n = fetch_int (mtp);
  for (i = 0; i < n; i++) {
    fetch_alloc_user (mtp);
  }

  if (sn > 0 && q->extra) {
    do_messages_mark_read (instance, peer_id, ML[0]->id);
  }
  free(extra);
  return 0;
}

struct query_methods get_history_methods = {
  .on_answer = get_history_on_answer,
};

void do_get_local_history (struct telegram *instance, peer_id_t id, int limit) {
  struct mtproto_connection *mtp = instance->connection;
  peer_t *P = user_chat_get (mtp->bl, id);
  if (!P || !P->last) { return; }
  struct message *M = P->last;
  int count = 1;
  assert (!M->prev);
  while (count < limit && M->next) {
    M = M->next;
    count ++;
  }
  while (M) {
    //print_message (M);
    M = M->prev;
  }
}

void do_get_history (struct telegram *instance, peer_id_t id, int limit) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  if (get_peer_type (id) == PEER_ENCR_CHAT || offline_mode) {
    do_get_local_history (instance, id, limit);
    do_mark_read (instance, id);
    return;
  }
  clear_packet (mtp);
  out_int (mtp, CODE_messages_get_history);
  out_peer_id (mtp, id);
  out_int (mtp, 0);
  out_int (mtp, 0);
  out_int (mtp, limit);

  struct get_hist_extra *extra = malloc(sizeof(struct get_hist_extra));
  extra->instance = instance;
  extra->peer_id = id;

  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &get_history_methods, extra);
}
/* }}} */

/* {{{ Get dialogs */
int dialog_list_got;
int get_dialogs_on_answer (struct query *q UU) {
  struct telegram *instance = q->extra;
  struct mtproto_connection *mtp = query_get_mtproto(q);
  unsigned x = fetch_int (mtp); 
  assert (x == CODE_messages_dialogs || x == CODE_messages_dialogs_slice);
  if (x == CODE_messages_dialogs_slice) {
    fetch_int (mtp); // total_count
  }
  assert (fetch_int (mtp) == CODE_vector);
  int n, i;
  n = fetch_int (mtp);
  static int dlist[2 * 100];
  static peer_id_t plist[100];
  int dl_size = n;
  for (i = 0; i < n; i++) {
    assert (fetch_int (mtp) == CODE_dialog);
    if (i < 100) {
      plist[i] = fetch_peer_id (mtp);
      dlist[2 * i + 0] = fetch_int (mtp);
      dlist[2 * i + 1] = fetch_int (mtp);
    } else {
      fetch_peer_id (mtp);
      fetch_int (mtp);
      fetch_int (mtp);
    }
  }
  assert (fetch_int (mtp) == CODE_vector);
  n = fetch_int (mtp);
  for (i = 0; i < n; i++) {
    fetch_alloc_message (mtp, instance);
  }
  assert (fetch_int (mtp) == CODE_vector);
  n = fetch_int (mtp);
  for (i = 0; i < n; i++) {
    fetch_alloc_chat (mtp);
  }
  assert (fetch_int (mtp) == CODE_vector);
  n = fetch_int (mtp);
  for (i = 0; i < n; i++) {
    fetch_alloc_user (mtp);
  }
  //print_start ();
  //push_color (COLOR_YELLOW);
  for (i = dl_size - 1; i >= 0; i--) {

    // TODO: use peer
    peer_t *UC UU;
    switch (get_peer_type (plist[i])) {
    case PEER_USER:
      UC = user_chat_get (mtp->bl, plist[i]);
      debug ("User ");
      //print_user_name (plist[i], UC);
      debug (": %d unread\n", dlist[2 * i + 1]);
      break;
    case PEER_CHAT:
      UC = user_chat_get (mtp->bl, plist[i]);
      debug ("Chat ");
      //print_chat_name (plist[i], UC);
      debug (": %d unread\n", dlist[2 * i + 1]);
      break;
    }
  }
  //pop_color ();
  //print_end ();

  dialog_list_got = 1;
  return 0;
}

struct query_methods get_dialogs_methods = {
  .on_answer = get_dialogs_on_answer,
};


void do_get_dialog_list (struct telegram *instance) {
  struct mtproto_connection *mtp = instance->connection;
  struct dc *DC_working = telegram_get_working_dc(instance);
  clear_packet (mtp);
  out_int (mtp, CODE_messages_get_dialogs);
  out_int (mtp, 0);
  out_int (mtp, 0);
  out_int (mtp, 1000);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &get_dialogs_methods, instance);
}
/* }}} */

int allow_send_linux_version = 1;

/* {{{ Send photo/video file */
struct send_file {
  int fd;
  long long size;
  long long offset;
  int part_num;
  int part_size;
  long long id;
  long long thumb_id;
  peer_id_t to_id;
  unsigned media_type;
  char *file_name;
  int encr;
  unsigned char *iv;
  unsigned char *init_iv;
  unsigned char *key;
};

void out_peer_id (struct mtproto_connection *self, peer_id_t id) {
  peer_t *U;
  switch (get_peer_type (id)) {
  case PEER_CHAT:
    out_int (self, CODE_input_peer_chat);
    out_int (self, get_peer_id (id));
    break;
  case PEER_USER:
    U = user_chat_get (self->bl, id);
    if (U && U->user.access_hash) {
      out_int (self, CODE_input_peer_foreign);
      out_int (self, get_peer_id (id));
      out_long (self, U->user.access_hash);
    } else {
      out_int (self, CODE_input_peer_contact);
      out_int (self, get_peer_id (id));
    }
    break;
  default:
    assert (0);
  }
}

struct send_file_extra {
  struct telegram *instance;
  struct send_file *file;
};

void send_part (struct telegram *instance, struct send_file *f);

int send_file_part_on_answer (struct query *q) {
  struct mtproto_connection *mtp = query_get_mtproto (q);
  struct send_file_extra *extra = q->extra;
  assert (fetch_int (mtp) == (int)CODE_bool_true);
  send_part (extra->instance, extra->file);
  return 0;
}

int send_file_on_answer (struct query *q UU) {
  struct telegram *instance = q->extra; 
  struct mtproto_connection *mtp = query_get_mtproto(q);
  assert (fetch_int (mtp) == (int)CODE_messages_stated_message);
 
  // TODO: use message
  struct message *M UU = fetch_alloc_message (mtp, instance);

  assert (fetch_int (mtp) == CODE_vector);
  int n, i;
  n = fetch_int (mtp);
  for (i = 0; i < n; i++) {
    fetch_alloc_chat (mtp);
  }
  assert (fetch_int (mtp) == CODE_vector);
  n = fetch_int (mtp);
  for (i = 0; i < n; i++) {
    fetch_alloc_user (mtp);
  }
  fetch_pts (mtp);
  fetch_seq (mtp);
  //print_message (M);
  return 0;
}

int send_encr_file_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
  if (prefetch_int (mtp) != (int)CODE_messages_sent_encrypted_file) {
    hexdump_in (mtp);
  }
  assert (fetch_int (mtp) == (int)CODE_messages_sent_encrypted_file);
  struct message *M = q->extra;
  M->date = fetch_int (mtp);
  assert (fetch_int (mtp) == CODE_encrypted_file);
  M->media.encr_photo.id = fetch_long (mtp);
  M->media.encr_photo.access_hash = fetch_long (mtp);
  //M->media.encr_photo.size = fetch_int (mtp);
  fetch_int (mtp);
  M->media.encr_photo.dc_id = fetch_int (mtp);
  assert (fetch_int (mtp) == M->media.encr_photo.key_fingerprint);
  //print_message (M);
  message_insert (M);
  return 0;
}

struct query_methods send_file_part_methods = {
  .on_answer = send_file_part_on_answer
};

struct query_methods send_file_methods = {
  .on_answer = send_file_on_answer
};

struct query_methods send_encr_file_methods = {
  .on_answer = send_encr_file_on_answer
};

void send_part (struct telegram *instance, struct send_file *f) {
  struct mtproto_connection *mtp = instance->connection;
  struct dc *DC_working = telegram_get_working_dc(instance);
  if (f->fd >= 0) {
    if (!f->part_num) {
      instance->cur_uploading_bytes += f->size;
    }
    clear_packet (mtp);
    if (f->size < (16 << 20)) {
      out_int (mtp, CODE_upload_save_file_part);      
      out_long (mtp, f->id);
      out_int (mtp, f->part_num ++);
    } else {
      out_int (mtp, CODE_upload_save_big_file_part);      
      out_long (mtp, f->id);
      out_int (mtp, f->part_num ++);
      out_int (mtp, (f->size + f->part_size - 1) / f->part_size);
    }
    static char buf[512 << 10];
    int x = read (f->fd, buf, f->part_size);
    assert (x > 0);
    f->offset += x;
    instance->cur_uploaded_bytes += x;
    
    if (f->encr) {
      if (x & 15) {
        assert (f->offset == f->size);
        secure_random (buf + x, (-x) & 15);
        x = (x + 15) & ~15;
      }
      
      AES_KEY aes_key;
      AES_set_encrypt_key (f->key, 256, &aes_key);
      AES_ige_encrypt ((void *)buf, (void *)buf, x, &aes_key, f->iv, 1);
      memset (&aes_key, 0, sizeof (aes_key));
    }
    out_cstring (mtp, buf, x);
    if (verbosity >= 2) {
      debug ("offset=%lld size=%lld\n", f->offset, f->size);
    }
    if (f->offset == f->size) {
      close (f->fd);
      f->fd = -1;
    } else {
      assert (f->part_size == x);
    }
    //update_prompt ();

    struct send_file_extra *extra = malloc(sizeof(struct send_file_extra));
    extra->instance = instance;
    extra->file = f;
    send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &send_file_part_methods, extra);
  } else {
    instance->cur_uploaded_bytes -= f->size;
    instance->cur_uploading_bytes -= f->size;
    //update_prompt ();
    clear_packet (mtp);
    assert (f->media_type == CODE_input_media_uploaded_photo || f->media_type == CODE_input_media_uploaded_video || f->media_type == CODE_input_media_uploaded_thumb_video || f->media_type == CODE_input_media_uploaded_audio || f->media_type == CODE_input_media_uploaded_document || f->media_type == CODE_input_media_uploaded_thumb_document);
    if (!f->encr) {
      out_int (mtp, CODE_messages_send_media);
      out_peer_id (mtp, f->to_id);
      out_int (mtp, f->media_type);
      if (f->size < (16 << 20)) {
        out_int (mtp, CODE_input_file);
      } else {
        out_int (mtp, CODE_input_file_big);
      }
      out_long (mtp, f->id);
      out_int (mtp, f->part_num);
      char *s = f->file_name + strlen (f->file_name);
      while (s >= f->file_name && *s != '/') { s --;}
      out_string (mtp, s + 1);
      if (f->size < (16 << 20)) {
        out_string (mtp, "");
      }
      if (f->media_type == CODE_input_media_uploaded_thumb_video || f->media_type == CODE_input_media_uploaded_thumb_document) {
        out_int (mtp, CODE_input_file);
        out_long (mtp, f->thumb_id);
        out_int (mtp, 1);
        out_string (mtp, "thumb.jpg");
        out_string (mtp, "");
      }
      if (f->media_type == CODE_input_media_uploaded_video || f->media_type == CODE_input_media_uploaded_thumb_video) {
        out_int (mtp, 100);
        out_int (mtp, 100);
        out_int (mtp, 100);
      }
      if (f->media_type == CODE_input_media_uploaded_document || f->media_type == CODE_input_media_uploaded_thumb_document) {
        out_string (mtp, s + 1);
        out_string (mtp, "text");
      }
      if (f->media_type == CODE_input_media_uploaded_audio) {
        out_int (mtp, 60);
      }

      out_long (mtp, -lrand48 () * (1ll << 32) - lrand48 ());
      send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &send_file_methods, instance);
    } else {
      struct message *M = talloc0 (sizeof (*M));

      out_int (mtp, CODE_messages_send_encrypted_file);
      out_int (mtp, CODE_input_encrypted_chat);
      out_int (mtp, get_peer_id (f->to_id));
      peer_t *P = user_chat_get (mtp->bl, f->to_id);
      assert (P);
      out_long (mtp, P->encr_chat.access_hash);
      long long r = -lrand48 () * (1ll << 32) - lrand48 (); 
      out_long (mtp, r);
      encr_start (mtp);
      out_int (mtp, CODE_decrypted_message);
      out_long (mtp, r);
      out_random (mtp, 15 + 4 * (lrand48 () % 3));
      out_string (mtp, "");
      if (f->media_type == CODE_input_media_uploaded_photo) {
        out_int (mtp, CODE_decrypted_message_media_photo);
        M->media.type = CODE_decrypted_message_media_photo;
      } else if (f->media_type == CODE_input_media_uploaded_video) {
        out_int (mtp, CODE_decrypted_message_media_video);
        M->media.type = CODE_decrypted_message_media_video;
      } else if (f->media_type == CODE_input_media_uploaded_audio) {
        out_int (mtp, CODE_decrypted_message_media_audio);
        M->media.type = CODE_decrypted_message_media_audio;
      } else if (f->media_type == CODE_input_media_uploaded_document) {
        out_int (mtp, CODE_decrypted_message_media_document);
        M->media.type = CODE_decrypted_message_media_document;;
      } else {
        assert (0);
      }
      if (f->media_type != CODE_input_media_uploaded_audio) {
        out_cstring (mtp, (void *)thumb_file, thumb_file_size);
        out_int (mtp, 90);
        out_int (mtp, 90);
      }
      if (f->media_type == CODE_input_media_uploaded_video) {
        out_int (mtp, 0);
      }
      if (f->media_type == CODE_input_media_uploaded_document) {
        out_string (mtp, f->file_name);
        out_string (mtp, "text");
      }
      if (f->media_type == CODE_input_media_uploaded_audio) {
        out_int (mtp, 60);
      }
      if (f->media_type == CODE_input_media_uploaded_video || f->media_type == CODE_input_media_uploaded_photo) {
        out_int (mtp, 100);
        out_int (mtp, 100);
      }
      out_int (mtp, f->size);
      out_cstring (mtp, (void *)f->key, 32);
      out_cstring (mtp, (void *)f->init_iv, 32);
      encr_finish (mtp, &P->encr_chat);
      if (f->size < (16 << 20)) {
        out_int (mtp, CODE_input_encrypted_file_uploaded);
      } else {
        out_int (mtp, CODE_input_encrypted_file_big_uploaded);
      }
      out_long (mtp, f->id);
      out_int (mtp, f->part_num);
      if (f->size < (16 << 20)) {
        out_string (mtp, "");
      }
 
      unsigned char md5[16];
      unsigned char str[64];
      memcpy (str, f->key, 32);
      memcpy (str + 32, f->init_iv, 32);
      MD5 (str, 64, md5);
      out_int (mtp, (*(int *)md5) ^ (*(int *)(md5 + 4)));

      tfree_secure (f->iv, 32);
      
      M->media.encr_photo.key = f->key;
      M->media.encr_photo.iv = f->init_iv;
      M->media.encr_photo.key_fingerprint = (*(int *)md5) ^ (*(int *)(md5 + 4)); 
      M->media.encr_photo.size = f->size;
  
      M->flags = FLAG_ENCRYPTED;
      M->from_id = MK_USER (instance->our_id);
      M->to_id = f->to_id;
      M->unread = 1;
      M->message = tstrdup ("");
      M->out = 1;
      M->id = r;
      M->date = time (0);
      
      send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &send_encr_file_methods, M);
    }
    tfree_str (f->file_name);
    tfree (f, sizeof (*f));
  }
}

void send_file_thumb (struct telegram *instance, struct send_file *f) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  clear_packet (mtp);
  f->thumb_id = lrand48 () * (1ll << 32) + lrand48 ();
  out_int (mtp, CODE_upload_save_file_part);
  out_long (mtp, f->thumb_id);
  out_int (mtp, 0);
  out_cstring (mtp, (void *)thumb_file, thumb_file_size);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &send_file_part_methods, f);
}

void do_send_photo (struct telegram *instance, int type, peer_id_t to_id, char *file_name) {
  int fd = open (file_name, O_RDONLY);
  if (fd < 0) {
    warning ("No such file '%s'\n", file_name);
    tfree_str (file_name);
    return;
  }
  struct stat buf;
  fstat (fd, &buf);
  long long size = buf.st_size;
  if (size <= 0) {
    debug ("File has zero length\n");
    tfree_str (file_name);
    close (fd);
    return;
  }
  struct send_file *f = talloc0 (sizeof (*f));
  f->fd = fd;
  f->size = size;
  f->offset = 0;
  f->part_num = 0;
  int tmp = ((size + 2999) / 3000);
  f->part_size = (1 << 10);
  while (f->part_size < tmp) {
    f->part_size *= 2;
  }

  if (f->part_size > (512 << 10)) {
    close (fd);
    failure ("Too big file. Maximal supported size is %d.\n", (512 << 10) * 1000);
    tfree (f, sizeof (*f));
    tfree_str (file_name);
    return;
  }

  f->id = lrand48 () * (1ll << 32) + lrand48 ();
  f->to_id = to_id;
  f->media_type = type;
  f->file_name = file_name;
  if (get_peer_type (f->to_id) == PEER_ENCR_CHAT) {
    f->encr = 1;
    f->iv = talloc (32);
    secure_random (f->iv, 32);
    f->init_iv = talloc (32);
    memcpy (f->init_iv, f->iv, 32);
    f->key = talloc (32);
    secure_random (f->key, 32);
  }
  if (f->media_type == CODE_input_media_uploaded_video && !f->encr) {
    f->media_type = CODE_input_media_uploaded_thumb_video;
    send_file_thumb (instance, f);
  } else if (f->media_type == CODE_input_media_uploaded_document && !f->encr) {
    f->media_type = CODE_input_media_uploaded_thumb_document;
    send_file_thumb (instance, f);
  } else {
    send_part (instance, f);
  }
}
/* }}} */

/* {{{ Forward */
int fwd_msg_on_answer (struct query *q UU) {
  struct telegram *instance = q->extra;
  struct mtproto_connection *mtp = query_get_mtproto(q);

  assert (fetch_int (mtp) == (int)CODE_messages_stated_message);

  // TODO: use message
  struct message *M UU = fetch_alloc_message (mtp, instance);
  assert (fetch_int (mtp) == CODE_vector);
  int n, i;
  n = fetch_int (mtp);
  for (i = 0; i < n; i++) {
    fetch_alloc_chat (mtp);
  }
  assert (fetch_int (mtp) == CODE_vector);
  n = fetch_int (mtp);
  for (i = 0; i < n; i++) {
    fetch_alloc_user (mtp);
  }
  fetch_pts (mtp);
  fetch_seq (mtp);
  //print_message (M);
  return 0;
}

struct query_methods fwd_msg_methods = {
  .on_answer = fwd_msg_on_answer
};

void do_forward_message (struct telegram *instance, peer_id_t id, int n) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  if (get_peer_type (id) == PEER_ENCR_CHAT) {
    warning ("Can not forward messages from secret chat\n");
    return;
  }
  clear_packet (mtp);
  out_int (mtp, CODE_messages_forward_message);
  out_peer_id (mtp, id);
  out_int (mtp, n);
  out_long (mtp, lrand48 () * (1ll << 32) + lrand48 ());
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &fwd_msg_methods, instance);
}
/* }}} */

/* {{{ Rename chat */
int rename_chat_on_answer (struct query *q UU) {
  struct telegram *instance = q->extra; 
  struct mtproto_connection *mtp = query_get_mtproto(q);

  assert (fetch_int (mtp) == (int)CODE_messages_stated_message);

  // TODO: use message
  struct message *M UU = fetch_alloc_message (mtp, instance);
  assert (fetch_int (mtp) == CODE_vector);
  int n, i;
  n = fetch_int (mtp);
  for (i = 0; i < n; i++) {
    fetch_alloc_chat (mtp);
  }
  assert (fetch_int (mtp) == CODE_vector);
  n = fetch_int (mtp);
  for (i = 0; i < n; i++) {
    fetch_alloc_user (mtp);
  }
  fetch_pts (mtp);
  fetch_seq (mtp);
  //print_message (M);
  return 0;
}

struct query_methods rename_chat_methods = {
  .on_answer = rename_chat_on_answer
};

void do_rename_chat (struct telegram *instance, peer_id_t id, char *name UU) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  clear_packet (mtp);
  out_int (mtp, CODE_messages_edit_chat_title);
  assert (get_peer_type (id) == PEER_CHAT);
  out_int (mtp, get_peer_id (id));
  out_string (mtp, name);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &rename_chat_methods, instance);
}
/* }}} */

/* {{{ Chat info */
void print_chat_info (struct chat *C) {

  // TODO: use peer_t
  peer_t *U UU = (void *)C;

  //print_start ();
  //push_color (COLOR_YELLOW);
  debug ("Chat ");
  //print_chat_name (U->id, U);
  debug (" members:\n");
  int i;
  for (i = 0; i < C->user_list_size; i++) {
    debug ("\t\t");
    //print_user_name (MK_USER (C->user_list[i].user_id), user_chat_get (mtp->bl, MK_USER (C->user_list[i].user_id)));
    debug (" invited by ");
    //print_user_name (MK_USER (C->user_list[i].inviter_id), user_chat_get (mtp->bl, MK_USER (C->user_list[i].inviter_id)));
    debug (" at ");
    //print_date_full (C->user_list[i].date);
    if (C->user_list[i].user_id == C->admin_id) {
      debug (" admin");
    }
    debug ("\n");
  }
  //pop_color ();
  //print_end ();
}

int chat_info_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
  struct chat *C = fetch_alloc_chat_full (mtp);
  mtp->instance->config->on_chat_info_received (mtp->instance, C->id);
  return 0;
}

struct query_methods chat_info_methods = {
  .on_answer = chat_info_on_answer
};

void do_get_chat_info (struct telegram *instance, peer_id_t id) {
  debug ("do_get_chat_info (peer_id=%d)", id.id);
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  if (offline_mode) {
    peer_t *C = user_chat_get (mtp->bl, id);
    if (!C) {
      warning ("No such chat\n");
    } else {
      //print_chat_info (&C->chat);
    }
    return;
  }
  clear_packet (mtp);
  out_int (mtp, CODE_messages_get_full_chat);
  assert (get_peer_type (id) == PEER_CHAT);
  out_int (mtp, get_peer_id (id));
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &chat_info_methods, 0);
}
/* }}} */

/* {{{ User info */

void print_user_info (struct tgl_user *U) {
  // TODO: use peer
  peer_t *C UU = (void *)U;

  //print_start ();
  //push_color (COLOR_YELLOW);
  debug ("User ");
  //print_user_name (U->id, C);
  debug (":\n");
  debug ("\treal name: %s %s\n", U->real_first_name, U->real_last_name);
  debug ("\tphone: %s\n", U->phone);
  if (U->status.online > 0) {
    debug ("\tonline\n");
  } else {
    debug ("\toffline (was online ");
    //print_date_full (U->status.when);
    debug (")\n");
  }
  //pop_color ();
  //print_end ();
}

struct show_info_extra {
    int show_info;
};

int user_info_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
  struct show_info_extra *extra = q->extra;

  struct tgl_user *U = fetch_alloc_user_full (mtp);
  event_user_info_received_handler (mtp->instance, U, extra->show_info);
  tfree (extra, sizeof(struct show_info_extra));
  //print_user_info (U);
  return 0;
}

struct query_methods user_info_methods = {
  .on_answer = user_info_on_answer
};

void do_get_user_info (struct telegram *instance, peer_id_t id, int showInfo) {
  info ("do_get_user_info\n");
  struct show_info_extra *extra = talloc(sizeof(struct show_info_extra));
  extra->show_info = showInfo;
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  clear_packet (mtp);
  out_int (mtp, CODE_users_get_full_user);
  assert (get_peer_type (id) == PEER_USER);
  peer_t *U = user_chat_get (mtp->bl, id);
  if (U && U->user.access_hash) {
    out_int (mtp, CODE_input_user_foreign);
    out_int (mtp, get_peer_id (id));
    out_long (mtp, U->user.access_hash);
  } else {
    out_int (mtp, CODE_input_user_contact);
    out_int (mtp, get_peer_id (id));
  }
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &user_info_methods, extra);
  debug ("do_get_user_info ready\n");
}
/* }}} */

/* {{{ Get user info silently */
int user_list_info_silent_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
  assert (fetch_int (mtp) == CODE_vector);
  int n = fetch_int (mtp);
  int i;
  for (i = 0; i < n; i++) {
    fetch_alloc_user (mtp);
  }
  return 0;
}

struct query_methods user_list_info_silent_methods = {
  .on_answer = user_list_info_silent_on_answer
};

void do_get_user_list_info_silent (struct telegram *instance, int num, int *list) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  clear_packet (mtp);
  out_int (mtp, CODE_users_get_users);
  out_int (mtp, CODE_vector);
  out_int (mtp, num);
  int i;
  for (i = 0; i < num; i++) {
    out_int (mtp, CODE_input_user_contact);
    out_int (mtp, list[i]);
    //out_long (0);
  }
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &user_list_info_silent_methods, 0);
}
/* }}} */


void end_load (struct telegram *instance, struct download *D) {
  instance->cur_downloading_bytes -= D->size;
  instance->cur_downloaded_bytes -= D->size;
  //update_prompt ();
  close (D->fd);
  debug ("Done: %s\n", D->name);
  event_download_finished (instance, D);
  instance->dl_curr = 0;
  if (D->dc != telegram_get_working_dc(instance)->id) {
    debug ("%d Not the working dc %d, closing...\n", D->dc, 
        telegram_get_working_dc(instance)->id);
  }
  if (D->iv) {
    tfree_secure (D->iv, 32);
  }
  tfree_str (D->name);
  tfree (D, sizeof (*D));
  telegram_dl_next (instance);
}

struct download_extra {
  struct telegram *instance;
  struct download *dl;
};

void load_next_part (struct telegram *instance, struct download *D);
int download_on_answer (struct query *q) {
  struct download_extra *extra = q->extra;
  struct telegram *instance = extra->instance;
  struct mtproto_connection *mtp = query_get_mtproto(q);

  struct download *D = extra->dl;
  free(extra);

  assert (fetch_int (mtp) == (int)CODE_upload_file);
  unsigned x = fetch_int (mtp);
  assert (x);
  if (D->fd == -1) {
    D->fd = open (D->name, O_CREAT | O_WRONLY, 0640);
  }
  fetch_int (mtp); // mtime
  int len = prefetch_strlen (mtp);
  assert (len >= 0);
  instance->cur_downloaded_bytes += len;
  //update_prompt ();
  if (D->iv) {
    unsigned char *ptr = (void *)fetch_str (mtp, len);
    assert (!(len & 15));
    AES_KEY aes_key;
    AES_set_decrypt_key (D->key, 256, &aes_key);
    AES_ige_encrypt (ptr, ptr, len, &aes_key, D->iv, 0);
    memset (&aes_key, 0, sizeof (aes_key));
    if (len > D->size - D->offset) {
      len = D->size - D->offset;
    }
    assert (write (D->fd, ptr, len) == len);
  } else {
    assert (write (D->fd, fetch_str (mtp, len), len) == len);
  }
  D->offset += len;
  if (D->offset < D->size) {
    load_next_part (instance, D);
    return 0;
  } else {
    end_load (instance, D);
    return 0;
  }
}

struct query_methods download_methods = {
  .on_answer = download_on_answer
};

void load_next_part (struct telegram *instance, struct download *D) {
  struct mtproto_connection *mtp = instance->connection;
  if (!D->offset) {
    static char buf[PATH_MAX];
    int l;
  
    if (!D->id) {
      l = tsnprintf (buf, sizeof (buf), "%s/download_%lld_%d", instance->download_path, D->volume, D->local_id);
    } else {
      l = tsnprintf (buf, sizeof (buf), "%s/download_%lld", instance->download_path, D->id);
    }
    if (l >= (int) sizeof (buf)) {
      fatal ("Download filename is too long");
      exit (1);
    }
    D->name = tstrdup (buf);
    struct stat st;
    if (stat (buf, &st) >= 0) {
      D->offset = st.st_size;      
      if (D->offset >= D->size) {
        instance->cur_downloading_bytes += D->size;
        instance->cur_downloaded_bytes += D->offset;
        info ("Already downloaded\n");
        end_load (instance, D);
        return;
      }
    }
    
    instance->cur_downloading_bytes += D->size;
    instance->cur_downloaded_bytes += D->offset;
    //update_prompt ();
  }
  info ("do_upload_get_file()\n");
  clear_packet (mtp);
  out_int (mtp, CODE_upload_get_file);
  if (!D->id) {
    out_int (mtp, CODE_input_file_location);
    out_long (mtp, D->volume);
    out_int (mtp, D->local_id);
    out_long (mtp, D->secret);
  } else {
    if (D->iv) {
      out_int (mtp, CODE_input_encrypted_file_location);
    } else {
      out_int (mtp, D->type);
    }
    out_long (mtp, D->id);
    out_long (mtp, D->access_hash);
  }
  out_int (mtp, D->offset);
  out_int (mtp, 1 << 14);

  struct download_extra *extra = malloc(sizeof(struct download_extra));
  extra->instance = instance;
  extra->dl = D;

  send_query (instance, instance->auth.DC_list[D->dc], mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &download_methods, extra);
  //send_query (instance, DC_working, packet_ptr - packet_buffer, packet_buffer, &download_methods, D);
}

void do_load_photo_size (struct telegram *instance, struct photo_size *P, void *extra) {
  if (!P->loc.dc) {
    failure ("Bad video thumb\n");
    return;
  }
  assert (P);
  struct download *D = talloc0 (sizeof (*D));
  D->id = 0;
  D->offset = 0;
  D->size = P->size;
  D->volume = P->loc.volume;
  D->dc = P->loc.dc;
  D->local_id = P->loc.local_id;
  D->secret = P->loc.secret;
  D->extra = extra;
  D->name = 0;
  D->fd = -1;

  telegram_dl_add (instance, D);
  telegram_dl_next (instance);
}

void do_load_photo (struct telegram *instance, struct photo *photo, int photoBig, void *extra) {
  if (!photo->sizes_num) { return; }
  int size = -1;
  int sizei = 0;
  int i;
  for (i = 0; i < photo->sizes_num; i++) {
    if (photoBig == 0)
    {
        if (photo->sizes[i].w + photo->sizes[i].h < size) {
          size = photo->sizes[i].w + photo->sizes[i].h;
          sizei = i;
        }
    } else {
        if (photo->sizes[i].w + photo->sizes[i].h > size) {
          size = photo->sizes[i].w + photo->sizes[i].h;
          sizei = i;
        }
    }
  }
  do_load_photo_size (instance, &photo->sizes[sizei], extra);
}

void do_load_video_thumb (struct telegram *instance, struct video *video, void *extra) {
  do_load_photo_size (instance, &video->thumb, extra);
}

void do_load_document_thumb (struct telegram *instance, struct document *video, void *extra) {
  do_load_photo_size (instance, &video->thumb, extra);
}

void do_load_video (struct telegram *instance, struct video *V, void *extra) {
  assert (V);
  struct download *D = talloc0 (sizeof (*D));
  D->offset = 0;
  D->size = V->size;
  D->id = V->id;
  D->access_hash = V->access_hash;
  D->dc = V->dc_id;
  D->extra = extra;
  D->name = 0;
  D->fd = -1;
  D->type = CODE_input_video_file_location;
  load_next_part (instance, D);
}

void do_load_audio (struct telegram *instance, struct video *V, void *extra) {
  assert (V);
  struct download *D = talloc0 (sizeof (*D));
  D->offset = 0;
  D->size = V->size;
  D->id = V->id;
  D->access_hash = V->access_hash;
  D->dc = V->dc_id;
  D->extra = extra;
  D->name = 0;
  D->fd = -1;
  D->type = CODE_input_audio_file_location;
  load_next_part (instance, D);
}

void do_load_document (struct telegram *instance, struct document *V, void *extra) {
  assert (V);
  struct download *D = talloc0 (sizeof (*D));
  D->offset = 0;
  D->size = V->size;
  D->id = V->id;
  D->access_hash = V->access_hash;
  D->dc = V->dc_id;
  D->extra = extra;
  D->name = 0;
  D->fd = -1;
  D->type = CODE_input_document_file_location;
  load_next_part (instance, D);
}

void do_load_encr_video (struct telegram *instance, struct encr_video *V, void *extra) {
  assert (V);
  struct download *D = talloc0 (sizeof (*D));
  D->offset = 0;
  D->size = V->size;
  D->id = V->id;
  D->access_hash = V->access_hash;
  D->dc = V->dc_id;
  D->extra = extra;
  D->name = 0;
  D->fd = -1;
  D->key = V->key;
  D->iv = talloc (32);
  memcpy (D->iv, V->iv, 32);
  load_next_part (instance, D);
      
  unsigned char md5[16];
  unsigned char str[64];
  memcpy (str, V->key, 32);
  memcpy (str + 32, V->iv, 32);
  MD5 (str, 64, md5);
  assert (V->key_fingerprint == ((*(int *)md5) ^ (*(int *)(md5 + 4))));
}
/* }}} */

/* {{{ Export auth */

struct export_info {
    void *extra;
    void (*cb)(char *export_auth_str, int len, void *extra);
};

int export_auth_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
  struct telegram *instance = mtp->connection->instance;

  assert (fetch_int (mtp) == (int)CODE_auth_exported_authorization);
  int l = fetch_int (mtp);
  if (!instance->our_id) {
    instance->our_id = l;
  } else {
    assert (instance->our_id == l);
  }
  l = prefetch_strlen (mtp);
  char *s = talloc (l);
  memcpy (s, fetch_str (mtp, l), l);
  instance->export_auth_str_len = l;
  instance->export_auth_str = s;

  struct export_info *info = q->extra;
  info->cb(instance->export_auth_str, instance->export_auth_str_len, info->extra);
  tfree(info, sizeof(struct export_info));
  return 0;
}

struct query_methods export_auth_methods = {
  .on_answer = export_auth_on_answer,
  .on_error = fail_on_error
};

void do_export_auth (struct telegram *instance, int num, void (*cb)(char *export_auth_str, int len, void *extra), void *extra) {
  info ("do_export_auth(num=%d)\n", num);
  instance->export_auth_str = 0;
  struct mtproto_connection *mtp = instance->connection;
  clear_packet (mtp);
  out_int (mtp, CODE_auth_export_authorization);
  out_int (mtp, num);

  struct export_info *info = talloc0(sizeof(struct export_info));
  info->cb = cb;
  info->extra = extra;

  send_query (instance, telegram_get_working_dc(instance), 
    mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &export_auth_methods, info);
}
/* }}} */

struct import_info {
    void *extra;
    void (*cb)(void* extra);
};

/* {{{ Import auth */
int import_auth_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
  struct telegram *instance = mtp->connection->instance;
  struct import_info *info = q->extra;

  assert (fetch_int (mtp) == (int)CODE_auth_authorization);
  fetch_int (mtp); // expires
  fetch_alloc_user (mtp);
  tfree_str (instance->export_auth_str);
  instance->export_auth_str = 0;
  info->cb(info->extra);
  tfree (info, sizeof(struct import_info));
  return 0;
}

struct query_methods import_auth_methods = {
  .on_answer = import_auth_on_answer,
  .on_error = fail_on_error
};

void do_import_auth (struct telegram *instance, int num, void (*cb)(void *extra), void *extra) {
  info ("do_import_auth(num=%d, our_id=%d, export_auth_str=)\n", num, instance->our_id);
  struct import_info *info = talloc0(sizeof (struct import_info));
  info->cb = cb;
  info->extra = extra;

  struct dc *target_dc = instance->auth.DC_list[num];
  assert (target_dc);
  struct mtproto_connection *dc_conn = target_dc->sessions[0]->c->mtconnection;
  assert (dc_conn);

  clear_packet (dc_conn);
  out_int (dc_conn, CODE_auth_import_authorization);
  out_int (dc_conn, instance->our_id);
  out_cstring (dc_conn, instance->export_auth_str, instance->export_auth_str_len);

  send_query (instance, target_dc, dc_conn->packet_ptr - dc_conn->packet_buffer, 
     dc_conn->packet_buffer, &import_auth_methods, info);
}
/* }}} */

/* {{{ Add contact */
int add_contact_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);

  assert (fetch_int (mtp) == (int)CODE_contacts_imported_contacts);
  assert (fetch_int (mtp) == CODE_vector);
  int n = fetch_int (mtp);
  if (n > 0) {
    debug ("Added successfully");
  } else {
    debug ("Not added");
  }
  int i;
  for (i = 0; i < n ; i++) {
    assert (fetch_int (mtp) == (int)CODE_imported_contact);
    fetch_int (mtp); // uid
    fetch_long (mtp); // client_id
  }
  assert (fetch_int (mtp) == CODE_vector);
  n = fetch_int (mtp);
  for (i = 0; i < n ; i++) {
    struct tgl_user *U = fetch_alloc_user (mtp);
    //print_start ();
    //push_color (COLOR_YELLOW);
    debug ("User #%d: ", get_peer_id (U->id));
    //print_user_name (U->id, (peer_t *)U);
    //push_color (COLOR_GREEN);
    debug (" (");
    debug ("%s", U->print_name);
    if (U->phone) {
      debug (" ");
      debug ("%s", U->phone);
    }
    debug (") ");
    //pop_color ();
    if (U->status.online > 0) {
      debug ("online\n");
    } else {
      if (U->status.online < 0) {
        debug ("offline. Was online ");
        //print_date_full (U->status.when);
      } else {
        debug ("offline permanent");
      }
      debug ("\n");
    }
    //pop_color ();
    //print_end ();

  }
  return 0;
}

struct query_methods add_contact_methods = {
  .on_answer = add_contact_on_answer,
};

void do_add_contact (struct telegram *instance, const char *phone, int phone_len, const char *first_name, int first_name_len, const char *last_name, int last_name_len, int force) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  clear_packet (mtp);
  out_int (mtp, CODE_contacts_import_contacts);
  out_int (mtp, CODE_vector);
  out_int (mtp, 1);
  out_int (mtp, CODE_input_phone_contact);
  out_long (mtp, lrand48 () * (1ll << 32) + lrand48 ());
  out_cstring (mtp, phone, phone_len);
  out_cstring (mtp, first_name, first_name_len);
  out_cstring (mtp, last_name, last_name_len);
  out_int (mtp, force ? CODE_bool_true : CODE_bool_false);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &add_contact_methods, 0);
}
/* }}} */

/* {{{ Msg search */
int msg_search_on_answer (struct query *q UU) {
  return get_history_on_answer (q);
}

struct query_methods msg_search_methods = {
  .on_answer = msg_search_on_answer
};

void do_msg_search (struct telegram *instance, peer_id_t id, int from, int to, int limit, const char *s) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  if (get_peer_type (id) == PEER_ENCR_CHAT) {
    warning ("Can not search in secure chat\n");
    return;
  }
  clear_packet (mtp);
  out_int (mtp, CODE_messages_search);
  if (get_peer_type (id) == PEER_UNKNOWN) {
    out_int (mtp, CODE_input_peer_empty);
  } else {
    out_peer_id (mtp, id);
  }
  out_string (mtp, s);
  out_int (mtp, CODE_input_messages_filter_empty);
  out_int (mtp, from);
  out_int (mtp, to);
  out_int (mtp, 0); // offset
  out_int (mtp, 0); // max_id
  out_int (mtp, limit);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &msg_search_methods, 0);
}
/* }}} */

/* {{{ Contacts search */
int contacts_search_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
 
  assert (fetch_int (mtp) == CODE_contacts_found);
  assert (fetch_int (mtp) == CODE_vector);
  int n = fetch_int (mtp);
  int i;
  for (i = 0; i < n; i++) {
    assert (fetch_int (mtp) == (int)CODE_contact_found);
    fetch_int (mtp);
  }
  assert (fetch_int (mtp) == CODE_vector);
  n = fetch_int (mtp);
  //print_start ();
  //push_color (COLOR_YELLOW);
  for (i = 0; i < n; i++) {
    struct tgl_user *U = fetch_alloc_user (mtp);
    debug ("User ");
    //push_color  (COLOR_RED);
    debug ("%s %s", U->first_name, U->last_name); 
    //pop_color ();
    debug (". Phone %s\n", U->phone);
  }
  //pop_color ();
  //print_end ();
  return 0;
}

struct query_methods contacts_search_methods = {
  .on_answer = contacts_search_on_answer
};

void do_contacts_search (struct telegram *instance, int limit, const char *s) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
   
  clear_packet (mtp);
  out_int (mtp, CODE_contacts_search);
  out_string (mtp, s);
  out_int (mtp, limit);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &contacts_search_methods, 0);
}
/* }}} */

/* {{{ Encr accept */
int send_encr_accept_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
  struct secret_chat *E = fetch_alloc_encrypted_chat (mtp);

  if (E->state == sc_ok) {
    //print_start ();
    //push_color (COLOR_YELLOW);
    debug ("Encrypted connection with ");
    ////print_encr_chat_name (E->id, (void *)E);
    debug (" established\n");
    //pop_color ();
    //print_end ();
  } else {
    //print_start ();
    //push_color (COLOR_YELLOW);
    debug ("Encrypted connection with ");
    ////print_encr_chat_name (E->id, (void *)E);
    debug (" failed\n");
    //pop_color ();
    //print_end ();
  }
  return 0;
}

int send_encr_request_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
  struct secret_chat *E = fetch_alloc_encrypted_chat (mtp);
  if (E->state == sc_deleted) {
    //print_start ();
    //push_color (COLOR_YELLOW);
    debug ("Encrypted connection with ");
    //print_encr_chat_name (E->id, (void *)E);
    debug (" can not be established\n");
    //pop_color ();
    //print_end ();
  } else {
    //print_start ();
    //push_color (COLOR_YELLOW);
    debug ("Establishing connection with ");
    //print_encr_chat_name (E->id, (void *)E);
    debug ("\n");
    //pop_color ();
    //print_end ();

    assert (E->state == sc_waiting);
  }
  return 0;
}

struct query_methods send_encr_accept_methods  = {
  .on_answer = send_encr_accept_on_answer
};

struct query_methods send_encr_request_methods  = {
  .on_answer = send_encr_request_on_answer
};

void do_send_accept_encr_chat (struct telegram *instance, struct secret_chat *E, unsigned char *random) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  int i;
  int ok = 0;
  for (i = 0; i < 64; i++) {
    if (E->key[i]) {
      ok = 1;
      break;
    }
  }
  if (ok) { return; } // Already generated key for this chat
  unsigned char random_here[256];
  secure_random (random_here, 256);
  for (i = 0; i < 256; i++) {
    random[i] ^= random_here[i];
  }
  BIGNUM *b = BN_bin2bn (random, 256, 0);
  ensure_ptr (b);
  BIGNUM *g_a = BN_bin2bn (E->g_key, 256, 0);
  ensure_ptr (g_a);
  assert (check_g (instance->encr_prime, g_a) >= 0);
  if (!instance->ctx) {
    instance->ctx = BN_CTX_new ();
    ensure_ptr (instance->ctx);
  }
  BIGNUM *p = BN_bin2bn (instance->encr_prime, 256, 0); 
  ensure_ptr (p);
  BIGNUM *r = BN_new ();
  ensure_ptr (r);
  ensure (BN_mod_exp (r, g_a, b, p, instance->ctx));
  static unsigned char kk[256];
  memset (kk, 0, sizeof (kk));
  BN_bn2bin (r, kk);
  for (i = 0; i < 256; i++) {
    kk[i] ^= E->nonce[i];
  }
  static unsigned char sha_buffer[20];
  sha1 (kk, 256, sha_buffer);

  bl_do_set_encr_chat_key (mtp->bl, mtp, E, kk, *(long long *)(sha_buffer + 12));

  clear_packet (mtp);
  out_int (mtp, CODE_messages_accept_encryption);
  out_int (mtp, CODE_input_encrypted_chat);
  out_int (mtp, get_peer_id (E->id));
  out_long (mtp, E->access_hash);
  
  ensure (BN_set_word (g_a, instance->encr_root));
  ensure (BN_mod_exp (r, g_a, b, p, instance->ctx));
  static unsigned char buf[256];
  memset (buf, 0, sizeof (buf));
  BN_bn2bin (r, buf);
  out_cstring (mtp, (void *)buf, 256);

  out_long (mtp, E->key_fingerprint);
  BN_clear_free (b);
  BN_clear_free (g_a);
  BN_clear_free (p);
  BN_clear_free (r);

  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &send_encr_accept_methods, E);
}

void do_create_keys_end (struct telegram *instance, struct secret_chat *U) {
  assert (instance->encr_prime);
  BIGNUM *g_b = BN_bin2bn (U->g_key, 256, 0);
  ensure_ptr (g_b);
  assert (check_g (instance->encr_prime, g_b) >= 0);
  if (!instance->ctx) {
    instance->ctx = BN_CTX_new ();
    ensure_ptr (instance->ctx);
  }
  BIGNUM *p = BN_bin2bn (instance->encr_prime, 256, 0); 
  ensure_ptr (p);
  BIGNUM *r = BN_new ();
  ensure_ptr (r);
  BIGNUM *a = BN_bin2bn ((void *)U->key, 256, 0);
  ensure_ptr (a);
  ensure (BN_mod_exp (r, g_b, a, p, instance->ctx));

  unsigned char *t = talloc (256);
  memcpy (t, U->key, 256);
  
  memset (U->key, 0, sizeof (U->key));
  BN_bn2bin (r, (void *)U->key);
  int i;
  for (i = 0; i < 64; i++) {
    U->key[i] ^= *(((int *)U->nonce) + i);
  }
  
  static unsigned char sha_buffer[20];
  sha1 ((void *)U->key, 256, sha_buffer);
  long long k = *(long long *)(sha_buffer + 12);
  if (k != U->key_fingerprint) {
    debug ("version = %d\n", instance->encr_param_version);
    hexdump ((void *)U->nonce, (void *)(U->nonce + 256));
    hexdump ((void *)U->g_key, (void *)(U->g_key + 256));
    hexdump ((void *)U->key, (void *)(U->key + 64));
    hexdump ((void *)t, (void *)(t + 256));
    hexdump ((void *)sha_buffer, (void *)(sha_buffer + 20));
    failure ("!!Key fingerprint mismatch (my 0x%llx 0x%llx)\n", (unsigned long long)k, (unsigned long long)U->key_fingerprint);
    U->state = sc_deleted;
  }

  tfree_secure (t, 256);
  
  BN_clear_free (p);
  BN_clear_free (g_b);
  BN_clear_free (r);
  BN_clear_free (a);
}

void do_send_create_encr_chat (struct telegram *instance, void *x, unsigned char *random) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
   
  int user_id = (long)x;
  int i;
  unsigned char random_here[256];
  secure_random (random_here, 256);
  for (i = 0; i < 256; i++) {
    random[i] ^= random_here[i];
  }
  if (!instance->ctx) {
    instance->ctx = BN_CTX_new ();
    ensure_ptr (instance->ctx);
  }
  BIGNUM *a = BN_bin2bn (random, 256, 0);
  ensure_ptr (a);
  BIGNUM *p = BN_bin2bn (instance->encr_prime, 256, 0); 
  ensure_ptr (p);
 
  BIGNUM *g = BN_new ();
  ensure_ptr (g);

  ensure (BN_set_word (g, instance->encr_root));

  BIGNUM *r = BN_new ();
  ensure_ptr (r);

  ensure (BN_mod_exp (r, g, a, p, instance->ctx));

  BN_clear_free (a);

  memset (instance->g_a, 0, 256);

  BN_bn2bin (r, (void *)instance->g_a);
  
  int t = lrand48 ();
  while (user_chat_get (mtp->bl, MK_ENCR_CHAT (t))) {
    t = lrand48 ();
  }

  bl_do_encr_chat_init (mtp->bl, mtp, t, user_id, (void *)random, (void *)instance->g_a);
  peer_t *_E = user_chat_get (mtp->bl, MK_ENCR_CHAT (t));
  assert (_E);
  struct secret_chat *E = &_E->encr_chat;
  
  clear_packet (mtp);
  out_int (mtp, CODE_messages_request_encryption);
  peer_t *U = user_chat_get (mtp->bl, MK_USER (E->user_id));
  assert (U);
  if (U && U->user.access_hash) {
    out_int (mtp, CODE_input_user_foreign);
    out_int (mtp, E->user_id);
    out_long (mtp, U->user.access_hash);
  } else {
    out_int (mtp, CODE_input_user_contact);
    out_int (mtp, E->user_id);
  }
  out_int (mtp, get_peer_id (E->id));
  out_cstring (mtp, instance->g_a, 256);
  write_secret_chat_file (instance, instance->secret_path);
  
  BN_clear_free (g);
  BN_clear_free (p);
  BN_clear_free (r);

  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &send_encr_request_methods, E);
}

struct create_encr_chat_extra {
   void (*callback) (struct telegram *instance, struct secret_chat *E, unsigned char *random);
   void *data;
   struct telegram *instance;
};

int get_dh_config_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);

  unsigned x = fetch_int (mtp);
  assert (x == CODE_messages_dh_config || x == CODE_messages_dh_config_not_modified || LOG_DH_CONFIG);
  if (x == CODE_messages_dh_config || x == LOG_DH_CONFIG)  {
    int a = fetch_int (mtp);
    int l = prefetch_strlen (mtp);
    assert (l == 256);
    char *s = fetch_str (mtp, l);
    int v = fetch_int (mtp);
    bl_do_set_dh_params (mtp->bl, mtp, a, (void *)s, v);

    BIGNUM *p = BN_bin2bn ((void *)s, 256, 0);
    ensure_ptr (p);
    assert (check_DH_params (mtp, p, a) >= 0);
    BN_free (p);      
  }
  if (x == LOG_DH_CONFIG) { return 0; }
  int l = prefetch_strlen (mtp);
  assert (l == 256);
  unsigned char *random = talloc (256);
  memcpy (random, fetch_str (mtp, 256), 256);
  if (q->extra) {
    //((void (*)(void *, void *))(*x))(x[1], random);

    struct create_encr_chat_extra *extra = q->extra;
    extra->callback(extra->instance, extra->data, random);
    free(extra);
    //tfree (x, 2 * sizeof (void *));

    tfree_secure (random, 256);
  } else {
    tfree_secure (random, 256);
  }
  return 0;
}

struct query_methods get_dh_config_methods  = {
  .on_answer = get_dh_config_on_answer
};

void do_accept_encr_chat_request (struct telegram *instance, struct secret_chat *E) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  assert (E->state == sc_request);
  
  clear_packet (mtp);
  out_int (mtp, CODE_messages_get_dh_config);
  out_int (mtp, instance->encr_param_version);
  out_int (mtp, 256);

  struct create_encr_chat_extra *extra = malloc(sizeof(struct create_encr_chat_extra));
  extra->callback = do_send_accept_encr_chat;
  extra->instance = instance;
  extra->data = (void*)E;
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &get_dh_config_methods, extra);
}

void do_create_encr_chat_request (struct telegram *instance, int user_id) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;

  clear_packet (mtp);
  out_int (mtp, CODE_messages_get_dh_config);
  out_int (mtp, instance->encr_param_version);
  out_int (mtp, 256);

  struct create_encr_chat_extra *extra = malloc(sizeof(struct create_encr_chat_extra));
  extra->callback = do_send_accept_encr_chat;
  extra->instance = instance;
  extra->data = (void *)(long)user_id;
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &get_dh_config_methods, extra);
}
/* }}} */

/* {{{ Get difference */
//int difference_got;
//int seq, pts, qts, last_date;
int get_state_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
  struct telegram *instance = q->extra;

  debug("get_state_on_answer()\n");
  assert (fetch_int (mtp) == (int)CODE_updates_state);
  bl_do_set_pts (mtp->bl, mtp, fetch_int (mtp));
  bl_do_set_qts (mtp->bl, mtp, fetch_int (mtp));
  bl_do_set_date (mtp->bl, mtp, fetch_int (mtp));
  bl_do_set_seq (mtp->bl, mtp, fetch_int (mtp));
  instance->unread_messages = fetch_int (mtp);
  //write_state_file ();
  telegram_store_session (instance);
  return 0;
}

int get_difference_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);
  struct telegram *instance = q->extra;

  debug("get_difference_on_answer()\n");
  instance->get_difference_active = 0;
  unsigned x = fetch_int (mtp);
  if (x == CODE_updates_difference_empty) {
    bl_do_set_date (mtp->bl, mtp, fetch_int (mtp));
    bl_do_set_seq (mtp->bl, mtp, fetch_int (mtp));
  } else if (x == CODE_updates_difference || x == CODE_updates_difference_slice) {
    int n, i;
    assert (fetch_int (mtp) == CODE_vector);
    n = fetch_int (mtp);
    int ml_pos = 0;
    for (i = 0; i < n; i++) {
      if (ml_pos < 10000) {
        instance->ML[ml_pos ++] = fetch_alloc_message (mtp, instance);
      } else {
        fetch_alloc_message (mtp, instance);
      }
    }
    assert (fetch_int (mtp) == CODE_vector);
    n = fetch_int (mtp);
    for (i = 0; i < n; i++) {
      if (ml_pos < 10000) {
        instance->ML[ml_pos ++] = fetch_alloc_encrypted_message (mtp, instance);
      } else {
        fetch_alloc_encrypted_message (mtp, instance);
      }
    }
    assert (fetch_int (mtp) == CODE_vector);
    n = fetch_int (mtp);
    for (i = 0; i < n; i++) {
      work_update (mtp, 0);
    }
    assert (fetch_int (mtp) == CODE_vector);
    n = fetch_int (mtp);
    debug("Found %d chats\n", n);
    for (i = 0; i < n; i++) {
      fetch_alloc_chat (mtp);
    }
    assert (fetch_int (mtp) == CODE_vector);
    n = fetch_int (mtp);
    debug("Found %d users\n", n);
    for (i = 0; i < n; i++) {
      fetch_alloc_user (mtp);
    }
    assert (fetch_int (mtp) == (int)CODE_updates_state);
    bl_do_set_pts (mtp->bl, mtp, fetch_int (mtp));
    bl_do_set_qts (mtp->bl, mtp, fetch_int (mtp));
    bl_do_set_date (mtp->bl, mtp, fetch_int (mtp));
    bl_do_set_seq (mtp->bl, mtp, fetch_int (mtp));
    instance->unread_messages = fetch_int (mtp);
    debug ("UNREAD MESSAGES: %d\n", ml_pos);
    //write_state_file ();
    for (i = 0; i < ml_pos; i++) {
      event_update_new_message (instance, instance->ML[i]);
      ////print_message (ML[i]);
    }
    if (x == CODE_updates_difference_slice) {
      do_get_difference (instance, 0);
    } else {
      //difference_got = 1;
    }
  } else {
    assert (0);
  }
  telegram_store_session (instance);
  return 0;   
}

struct query_methods get_state_methods = {
  .on_answer = get_state_on_answer
};

struct query_methods get_difference_methods = {
  .on_answer = get_difference_on_answer
};

void do_get_difference (struct telegram *instance, int sync_from_start) {
  info ("do_get_difference()\n");
  struct mtproto_connection *mtp = instance->connection;
  struct dc *DC_working = telegram_get_working_dc(instance);

  instance->get_difference_active = 1;
  //difference_got = 0;
  clear_packet (mtp);
  do_insert_header (mtp);
  debug("do_get_difference(pts:%d, last_date:%d, qts: %d)\n", instance->proto.pts, instance->proto.last_date, instance->proto.qts);
  if (instance->proto.seq > 0 || sync_from_start) {
    if (instance->proto.pts == 0) { instance->proto.pts = 1; }
    if (instance->proto.qts == 0) { instance->proto.qts = 1; }
    if (instance->proto.last_date == 0) { instance->proto.last_date = 1; }
    
    out_int (mtp, CODE_updates_get_difference);
    out_int (mtp, instance->proto.pts);
    out_int (mtp, instance->proto.last_date);
    out_int (mtp, instance->proto.qts);
    send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &get_difference_methods, instance);
  } else {
    debug("do_updates_get_state()\n", 
        instance->proto.pts, instance->proto.last_date, instance->proto.qts);
    out_int (mtp, CODE_updates_get_state);
    send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &get_state_methods, instance);
  }
}
/* }}} */

/* {{{ Visualize key */
//char *colors[4] = {COLOR_GREY, COLOR_CYAN, COLOR_BLUE, COLOR_GREEN};

void do_visualize_key (struct binlog *bl, peer_id_t id) {
  assert (get_peer_type (id) == PEER_ENCR_CHAT);
  peer_t *P = user_chat_get (bl, id);
  assert (P);
  if (P->encr_chat.state != sc_ok) {
    warning ("Chat is not initialized yet\n");
    return;
  }
  unsigned char buf[20];
  SHA1 ((void *)P->encr_chat.key, 256, buf);
  //print_start ();
  int i;
  for (i = 0; i < 16; i++) {
    int x = buf[i];
    int j;
    for (j = 0; j < 4; j ++) {    
      ////push_color (colors[x & 3]);
      ////push_color (COLOR_INVERSE);
      //debug ("  ");
      ////pop_color ();
      ////pop_color ();
      x = x >> 2;
    }
    if (i & 1) { debug ("\n"); }
  }
  //print_end ();
}
/* }}} */

/* {{{ Get suggested */
int get_suggested_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);

  assert (fetch_int (mtp) == CODE_contacts_suggested);
  assert (fetch_int (mtp) == CODE_vector);
  int n = fetch_int (mtp);
  debug ("n = %d\n", n);
  assert (n <= 200);
  int l[400];
  int i;
  for (i = 0; i < n; i++) {
    assert (fetch_int (mtp) == CODE_contact_suggested);
    l[2 * i] = fetch_int (mtp);
    l[2 * i + 1] = fetch_int (mtp);
  }
  assert (fetch_int (mtp) == CODE_vector);
  int m = fetch_int (mtp);
  assert (n == m);
  //print_start ();
  //push_color (COLOR_YELLOW);
  for (i = 0; i < m; i++) {
    peer_t *U = (void *)fetch_alloc_user (mtp);
    assert (get_peer_id (U->id) == l[2 * i]);
    //print_user_name (U->id, U);
    debug (" phone %s: %d mutual friends\n", U->user.phone, l[2 * i + 1]);
  }
  //pop_color ();
  //print_end ();
  return 0;
}

struct query_methods get_suggested_methods = {
  .on_answer = get_suggested_on_answer
};

void do_get_suggested (struct telegram *instance) {
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  clear_packet (mtp);
  out_int (mtp, CODE_contacts_get_suggested);
  out_int (mtp, 100);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &get_suggested_methods, 0);
}
/* }}} */

/* {{{ Add user to chat */

struct query_methods add_user_to_chat_methods = {
  .on_answer = fwd_msg_on_answer
};

void do_add_user_to_chat (struct telegram *instance, peer_id_t chat_id, peer_id_t id, int limit) {
  info ("do_add_user_to_chat()\n");
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  clear_packet (mtp);
  out_int (mtp, CODE_messages_add_chat_user);
  out_int (mtp, get_peer_id (chat_id));
  
  assert (get_peer_type (id) == PEER_USER);
  peer_t *U = user_chat_get (mtp->bl, id);
  if (U && U->user.access_hash) {
    out_int (mtp, CODE_input_user_foreign);
    out_int (mtp, get_peer_id (id));
    out_long (mtp, U->user.access_hash);
  } else {
    out_int (mtp, CODE_input_user_contact);
    out_int (mtp, get_peer_id (id));
  }
  out_int (mtp, limit);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &add_user_to_chat_methods, 0);
}

void do_del_user_from_chat (struct telegram *instance, peer_id_t chat_id, peer_id_t id) {
  info ("do_del_user_from_chat()\n");
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  clear_packet (mtp);
  out_int (mtp, CODE_messages_delete_chat_user);
  out_int (mtp, get_peer_id (chat_id));
  
  assert (get_peer_type (id) == PEER_USER);
  peer_t *U = user_chat_get (mtp->bl, id);
  if (U && U->user.access_hash) {
    out_int (mtp, CODE_input_user_foreign);
    out_int (mtp, get_peer_id (id));
    out_long (mtp, U->user.access_hash);
  } else {
    out_int (mtp, CODE_input_user_contact);
    out_int (mtp, get_peer_id (id));
  }
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &add_user_to_chat_methods, 0);
}
/* }}} */

/* {{{ Create secret chat */
char *create_print_name (struct binlog *bl, peer_id_t id, const char *a1, const char *a2, const char *a3, const char *a4);

void do_create_secret_chat (struct telegram *instance, peer_id_t id) {
  info ("do_create_secret_chat()\n");
  struct mtproto_connection *mtp = instance->connection;
  assert (get_peer_type (id) == PEER_USER);
  peer_t *U = user_chat_get (mtp->bl, id);
  if (!U) { 
    warning ("Can not create chat with unknown user\n");
    return;
  }

  do_create_encr_chat_request (instance, get_peer_id (id)); 
}
/* }}} */

/* {{{ Create group chat */
struct query_methods create_group_chat_methods = {
  .on_answer = fwd_msg_on_answer
};

void do_create_group_chat (struct telegram *instance, peer_id_t id, char *chat_topic) {
  info ("do_create_group_chat()\n");
  struct mtproto_connection *mtp = instance->connection;
  assert (get_peer_type (id) == PEER_USER);
  peer_t *U = user_chat_get (mtp->bl, id);
  if (!U) { 
    warning ("Can not create chat with unknown user\n");
    return;
  }
  clear_packet (mtp);
  out_int (mtp, CODE_messages_create_chat);
  out_int (mtp, CODE_vector);
  out_int (mtp, 1); // Number of users, currently we support only 1 user.
  if (U && U->user.access_hash) {
    out_int (mtp, CODE_input_user_foreign);
    out_int (mtp, get_peer_id (id));
    out_long (mtp, U->user.access_hash);
  } else {
    out_int (mtp, CODE_input_user_contact);
    out_int (mtp, get_peer_id (id));
  }
  out_string (mtp, chat_topic);
  send_query (instance, telegram_get_working_dc(instance), mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &create_group_chat_methods, 0);
}
/* }}} */


/* {{{ Delete msg */

int delete_msg_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);

  assert (fetch_int (mtp) == CODE_vector);
  int n = fetch_int (mtp);
  fetch_skip (mtp, n);
  debug ("Deleted %d messages\n", n);
  return 0;
}

struct query_methods delete_msg_methods = {
  .on_answer = delete_msg_on_answer
};

void do_delete_msg (struct telegram *instance, long long id) {
  info ("do_delete_msg()\n");
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  clear_packet (mtp);
  out_int (mtp, CODE_messages_delete_messages);
  out_int (mtp, CODE_vector);
  out_int (mtp, 1);
  out_int (mtp, id);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &delete_msg_methods, 0);
}
/* }}} */

/* {{{ Restore msg */

int restore_msg_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);

  assert (fetch_int (mtp) == CODE_vector);
  int n = fetch_int (mtp);
  fetch_skip (mtp, n);
  debug ("Restored %d messages\n", n);
  return 0;
}

struct query_methods restore_msg_methods = {
  .on_answer = restore_msg_on_answer
};

void do_restore_msg (struct telegram *instance, long long id) {
  info ("do_restore_msg()\n");
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  clear_packet (mtp);
  out_int (mtp, CODE_messages_restore_messages);
  out_int (mtp, CODE_vector);
  out_int (mtp, 1);
  out_int (mtp, id);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &restore_msg_methods, 0);
}
/* }}} */
int update_status_on_answer (struct query *q UU) {
  struct mtproto_connection *mtp = query_get_mtproto(q);

  fetch_bool (mtp);
  return 0;
}

struct query_methods update_status_methods = {
  .on_answer = update_status_on_answer
};

void do_update_status (struct telegram *instance, int online UU) {
  info ("do_update_status()\n");
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;
  clear_packet (mtp);
  out_int (mtp, CODE_account_update_status);
  out_int (mtp, online ? CODE_bool_false : CODE_bool_true);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &update_status_methods, 0);
}

int update_typing_on_answer (struct query *q UU) {
  fetch_bool (query_get_mtproto (q));
  return 0;
}

struct query_methods update_typing_methods = {
  .on_answer = update_typing_on_answer
};

void do_update_typing (struct telegram *instance, peer_id_t id) {
  info ("do_update_typing()\n");
  struct dc *DC_working = telegram_get_working_dc(instance);
  struct mtproto_connection *mtp = instance->connection;

  clear_packet (mtp);
  out_int (mtp, CODE_messages_set_typing);
  out_peer_id (mtp, id);
  out_int (mtp, CODE_bool_true);
  send_query (instance, DC_working, mtp->packet_ptr - mtp->packet_buffer, mtp->packet_buffer, &update_typing_methods, 0);
}

