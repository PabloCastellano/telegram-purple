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
#ifndef __NET_H__
#define __NET_H__

#pragma once

#include <poll.h>
struct dc;
#include "mtproto-client.h"
#include "telegram.h"
#include "queries.h"

#define TG_SERVER "149.154.167.50" 
#define TG_DC_NUM 2
#define TG_SERVER_TEST "149.154.167.40"
#define TG_TEST_DC_NUM 2
#define TG_APP_HASH "99428c722d0ed59b9cd844e4577cb4bb"
#define TG_APP_ID 16154
#define TG_PORT 443

#define ACK_TIMEOUT 1
#define MAX_DC_ID 10

// typedef struct mtproto_connection not available right now
struct mtproto_connection;

struct connection;
struct connection_methods {
  int (*ready) (struct connection *c);
  int (*close) (struct connection *c);
  int (*execute) (struct connection *c, int op, int len);
};


#define MAX_DC_SESSIONS 3

struct session {
  struct dc *dc;
  long long session_id;
  int seq_no;
  struct connection *c;
  struct tree_long *ack_tree;
  struct event_timer ev;
};

struct dc {
  int id;
  int port;
  int flags;
  char *ip;
  char *user;
  struct session *sessions[MAX_DC_SESSIONS];
  char auth_key[256];
  long long auth_key_id;
  long long server_salt;

  int server_time_delta;
  double server_time_udelta;
  int has_auth;
};

#define DC_SERIALIZED_MAGIC 0x64582faa
#define DC_SERIALIZED_MAGIC_V2 0x94032abb
#define STATE_FILE_MAGIC 0x84217a0d
#define SECRET_CHAT_FILE_MAGIC 0xa9840add

struct dc_serialized {
  int magic;
  int port;
  char ip[64];
  char user[64];
  char auth_key[256];
  long long auth_key_id, server_salt;
  int authorized;
};

struct connection_buffer {
  unsigned char *start;
  unsigned char *end;
  unsigned char *rptr;
  unsigned char *wptr;
  struct connection_buffer *next;
};

enum conn_state {
  conn_none,
  conn_connecting,
  conn_ready,
  conn_failed,
  conn_stopped
};

struct connection {
  int fd;
  char *ip;
  int port;
  int flags;
  enum conn_state state;
  int ipv6[4];
  struct connection_buffer *in_head;
  struct connection_buffer *in_tail;
  struct connection_buffer *out_head;
  struct connection_buffer *out_tail;
  int in_bytes;
  int out_bytes;
  int packet_num;
  int out_packet_num;
  int last_connect_time;
  int in_fail_timer;
  struct connection_methods *methods;
  struct session *session;
  void *extra;
  struct event_timer ev;
  double last_receive_time;
  struct telegram *instance;
  struct mtproto_connection *mtconnection;
};

extern struct connection *Connections[];

int write_out (struct connection *c, const void *data, int len);
void flush_out (struct connection *c);
int read_in (struct connection *c, void *data, int len);

void create_all_outbound_connections (void);

void dc_create_session (struct dc *DC);
void insert_msg_id (struct session *S, long long id);
struct dc *alloc_dc (struct dc* DC_list[], int id, char *ip, int port);

#define GET_DC(c) (c->session->dc)

// export read and write methods to redirect network control
void try_read (struct connection *c);
void try_rpc_read (struct connection *c);
int try_write (struct connection *c);

struct connection *fd_create_connection (struct dc *DC, int fd, struct telegram *instance, 
    struct connection_methods *methods, struct mtproto_connection *mtp);
void fd_close_connection(struct connection *c);

void start_ping_timer (struct connection *c);
void stop_ping_timer (struct connection *c);
int send_all_acks (struct session *S);

#endif
