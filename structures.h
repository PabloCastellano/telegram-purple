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
#ifndef __STRUCTURES_H__
#define __STRUCTURES_H__
#pragma once

// forward-declrataions
struct mtproto_connection;
struct binlog;
typedef struct { int type; int id; } peer_id_t;

#include <assert.h>

//#define FLAG_EMPTY 1
#define FLAG_MESSAGE_EMPTY 1
#define FLAG_DELETED 2
#define FLAG_FORBIDDEN 4
#define FLAG_HAS_PHOTO 8
#define FLAG_CREATED 16

#define FLAG_USER_SELF 128
#define FLAG_USER_FOREIGN 256
#define FLAG_USER_CONTACT 512
#define FLAG_USER_IN_CONTACT 1024
#define FLAG_USER_OUT_CONTACT 2048

#define FLAG_CHAT_IN_CHAT 128

#define FLAG_ENCRYPTED 4096
#define FLAG_PENDING 8192


struct file_location {
  int dc;
  long long volume;
  int local_id;
  long long secret;
};

struct photo_size {
  char *type;
  struct file_location loc;
  int w;
  int h;
  int size;
  char *data;
};

struct geo {
  double longitude;
  double latitude;
};

struct photo {
  long long id;
  long long access_hash;
  int user_id;
  int date;
  char *caption;
  struct geo geo;
  int sizes_num;
  struct photo_size *sizes;
};

struct encr_photo {
  long long id;
  long long access_hash;
  int dc_id;
  int size;
  int key_fingerprint;

  unsigned char *key;
  unsigned char *iv;
  int w;
  int h;
};

struct encr_video {
  long long id;
  long long access_hash;
  int dc_id;
  int size;
  int key_fingerprint;
  
  unsigned char *key;
  unsigned char *iv;
  int w;
  int h;
  int duration;
};

struct encr_audio {
  long long id;
  long long access_hash;
  int dc_id;
  int size;
  int key_fingerprint;
  
  unsigned char *key;
  unsigned char *iv;
  int duration;
};

struct encr_document {
  long long id;
  long long access_hash;
  int dc_id;
  int size;
  int key_fingerprint;
  
  unsigned char *key;
  unsigned char *iv;
  char *file_name;
  char *mime_type;
};

struct encr_file {
  char *filename;
  unsigned char *key;
  unsigned char *iv;
};


struct user_status {
  int online;
  int when;
};

struct tgl_user {
  peer_id_t id;
  int flags;
  struct message *last;
  char *print_name;
  int structure_version;
  struct file_location photo_big;
  struct file_location photo_small;
  long long photo_id;
  struct photo photo;
  char *first_name;
  char *last_name;
  char *phone;
  long long access_hash;
  struct user_status status;
  int blocked;
  char *real_first_name;
  char *real_last_name;
};


struct chat_user {
  int user_id;
  int inviter_id;
  int date;
};

struct chat {
  peer_id_t id;
  int flags;
  struct message *last;
  char *print_title;
  int structure_version;
  struct file_location photo_big;
  struct file_location photo_small;
  struct photo photo;
  char *title;
  int users_num;
  int user_list_size;
  int user_list_version;
  struct chat_user *user_list;
  int date;
  int version;
  int admin_id;
};

enum secret_chat_state {
  sc_none,
  sc_waiting,
  sc_request,
  sc_ok,
  sc_deleted
};

struct secret_chat {
  peer_id_t id;
  int flags;
  struct message *last;
  char *print_name;
  int structure_version;
  struct file_location photo_big;
  struct file_location photo_small;
  struct photo photo;
  int user_id;
  int admin_id;
  int date;
  int ttl;
  long long access_hash;
  unsigned char *g_key;
  unsigned char *nonce;

  enum secret_chat_state state;
  int key[64];
  long long key_fingerprint;
};

typedef union peer {
  struct {
    peer_id_t id;
    int flags;
    struct message *last;
    char *print_name;
    int structure_version;
    struct file_location photo_big;
    struct file_location photo_small;
    struct photo photo;
  };
  struct tgl_user user;
  struct chat chat;
  struct secret_chat encr_chat;
} peer_t;

struct video {
  long long id;
  long long access_hash;
  int user_id;
  int date;
  int size;
  int dc_id;
  struct photo_size thumb;
  char *caption;
  int duration;
  int w;
  int h;
};

struct audio {
  long long id;
  long long access_hash;
  int user_id;
  int date;
  int size;
  int dc_id;
  int duration;
};

struct document {
  long long id;
  long long access_hash;
  int user_id;
  int date;
  int size;
  int dc_id;
  struct photo_size thumb;
  char *caption;
  char *mime_type;
};

struct message_action {
  unsigned type;
  union {
    struct {
      char *title;
      int user_num;
      int *users;
    };
    char *new_title;
    struct photo photo;
    int user;
    int ttl;
  };
};

struct message_media {
  unsigned type;
  union {
    struct photo photo;
    struct video video;
    struct audio audio;
    struct document document;
    struct geo geo;
    struct {
      char *phone;
      char *first_name;
      char *last_name;
      int user_id;
    };
    struct encr_photo encr_photo;
    struct encr_video encr_video;
    struct encr_audio encr_audio;
    struct encr_document encr_document;
    struct encr_file encr_file;
    struct {
      void *data;
      int data_size;
    };
  };
};

struct message {
  struct message *next_use, *prev_use;
  struct message *next, *prev;
  long long id;
  int flags;
  peer_id_t fwd_from_id;
  int fwd_date;
  peer_id_t from_id;
  peer_id_t to_id;
  int out;
  int unread;
  int date;
  int service;
  struct telegram *instance;
  union {
    struct message_action action;
    struct {
      char *message;
      int message_len;
      struct message_media media;
    };
  };
};

int fetch_file_location (struct mtproto_connection *mtp, struct file_location *loc);
int fetch_user_status (struct mtproto_connection *mtp, struct user_status *S);
int fetch_user (struct mtproto_connection *mtp, struct tgl_user *U);
struct tgl_user *fetch_alloc_user (struct mtproto_connection *mtp);
struct tgl_user *fetch_alloc_user_full (struct mtproto_connection *mtp);
struct chat *fetch_alloc_chat (struct mtproto_connection *mtp);
struct chat *fetch_alloc_chat_full (struct mtproto_connection *mtp);
struct secret_chat *fetch_alloc_encrypted_chat (struct mtproto_connection *mtp);
struct message *fetch_alloc_message (struct mtproto_connection *self, struct telegram *instance);
struct message *fetch_alloc_geo_message (struct mtproto_connection *mtp, struct telegram *instance);
struct message *fetch_alloc_message_short (struct mtproto_connection *mtp, struct telegram *instance);
struct message *fetch_alloc_message_short_chat (struct mtproto_connection *self, struct telegram *instance);
struct message *fetch_alloc_encrypted_message (struct mtproto_connection *mtp, struct telegram *instance);
void fetch_encrypted_message_file (struct mtproto_connection *mtp, struct message_media *M);
void fetch_skip_encrypted_message_file (struct mtproto_connection *mtp);
void fetch_message_action_encrypted (struct mtproto_connection *mtp, struct message_action *M);
peer_id_t fetch_peer_id (struct mtproto_connection *mtp);

void fetch_message_media (struct mtproto_connection *mtp, struct message_media *M);
void fetch_message_media_encrypted (struct mtproto_connection *mtp, struct message_media *M);
void fetch_message_action (struct mtproto_connection *mtp, struct message_action *M);
void message_insert_tree (struct message *M);

void free_user (struct tgl_user *U);
void free_chat (struct chat *U);

char *create_print_name (struct binlog *bl, peer_id_t id, const char *a1, const char *a2, const char *a3, const char *a4);

int print_stat (struct binlog *bl, char *s, int len);
peer_t *user_chat_get (struct binlog *bl, peer_id_t id);
struct message *message_get (struct binlog *bl, long long id);
void update_message_id (struct message *M, long long id);
void message_insert (struct message *M);
void fetch_photo (struct mtproto_connection *mtp, struct photo *P);
void insert_encrypted_chat (struct binlog *bl, peer_t *P);
void insert_user (struct binlog *bl, peer_t *P);
void insert_chat (struct binlog *bl, peer_t *P);
void free_photo (struct photo *P);
void message_insert_unsent (struct message *M);
void message_remove_unsent (struct message *M);
void send_all_unsent (struct binlog *bl);
void message_remove_tree (struct message *M);
void message_add_peer (struct message *M);
void message_del_peer (struct message *M);
void free_message (struct message *M);
void message_del_use (struct message *M);
void peer_insert_name (struct binlog *bl, peer_t *P);
void peer_delete_name (struct binlog *bl, peer_t *P);
peer_t *peer_lookup_name (struct binlog *bl, const char *s);

int user_get_alias(peer_t *user, char *buffer, int maxlen);

#define PEER_USER 1
#define PEER_CHAT 2
#define PEER_GEO_CHAT 3
#define PEER_ENCR_CHAT 4
#define PEER_UNKNOWN 0

#define MK_USER(id) set_peer_id (PEER_USER,id)
#define MK_CHAT(id) set_peer_id (PEER_CHAT,id)
#define MK_GEO_CHAT(id) set_peer_id (PEER_GEO_CHAT,id)
#define MK_ENCR_CHAT(id) set_peer_id (PEER_ENCR_CHAT,id)

static inline int get_peer_type (peer_id_t id) {
  return id.type;
}

static inline int get_peer_id (peer_id_t id) {
  return id.id;
}

static inline peer_id_t set_peer_id (int type, int id) {
  peer_id_t ID;
  ID.id = id;
  ID.type = type;
  return ID;
}

static inline int cmp_peer_id (peer_id_t a, peer_id_t b) {
  return memcmp (&a, &b, sizeof (a));
}

void free_messages (struct binlog *bl);
void free_peers (struct binlog *bl);
#endif

