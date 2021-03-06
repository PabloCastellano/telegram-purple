#ifndef __TELEGRAM_H__
#define __TELEGRAM_H__

#define MAX_PACKED_SIZE (1 << 24)
#define MAX_DC_NUM 9
#define MAX_PEER_NUM 100000

#ifndef PROG_NAME
#define PROG_NAME "telegram-purple"
#endif

#include <sys/types.h>
#include "glib.h"
#include "loop.h"
#include "tree.h"
#include "queries.h"
#include <openssl/bn.h>

// forward declarations
struct message;
struct protocol_state;
struct authorization_state;
struct tree_query;
struct tree_timer;


/*
 * telegram states
 */

#define STATE_INITIALISED 0
#define STATE_DISCONNECTED 1
#define STATE_ERROR 2
#define STATE_AUTHORIZED 6


// dc discovery
#define STATE_CONFIG_REQUESTED 7
#define STATE_EXPORTING_CONFIG 8
#define STATE_DISCONNECTED_SWITCH_DC 9
#define STATE_CONFIG_RECEIVED 11

// login

// - Phone Registration
#define STATE_PHONE_NOT_REGISTERED 13
#define STATE_PHONE_CODE_REQUESTED 14
#define STATE_PHONE_CODE_NOT_ENTERED 15
#define STATE_PHONE_CODE_ENTERED 16

// - Client Registration
#define STATE_CLIENT_IS_REGISTERED_SENT 17
#define STATE_CLIENT_NOT_REGISTERED 18
#define STATE_CLIENT_CODE_REQUESTED 19
#define STATE_CLIENT_CODE_NOT_ENTERED 20
#define STATE_CLIENT_CODE_ENTERED 21

// Ready for sending and receiving messages
#define STATE_READY 22

struct tree_peer;
struct tree_peer_by_name;
struct tree_message;

#define BINLOG_BUFFER_SIZE (1 << 20)

/**
 * Binary log
 */
struct binlog {
  int binlog_buffer[BINLOG_BUFFER_SIZE];
  int *rptr;
  int *wptr;
  int test_dc; // = 0
  int in_replay_log;
  int binlog_enabled; // = 0;
  int binlog_fd;
  long long binlog_pos;

  int s[1000];

  // 
  struct tree_peer *peer_tree;
  struct tree_peer_by_name *peer_by_name_tree;
  struct tree_message *message_tree;
  struct tree_message *message_unsent_tree;
  
  int users_allocated;
  int chats_allocated;
  int messages_allocated;
  int peer_num;
  int encr_chats_allocated;
  int geo_chats_allocated;

  peer_t *Peers[MAX_PEER_NUM];
};

#define REQ_CONNECTION 1
#define REQ_DOWNLOAD 2
struct proxy_request {
    struct telegram *tg;
    struct dc *DC;
    struct mtproto_connection *conn;
    int type;
    void *data;
    void (*done) (struct proxy_request *req);
    void *extra;
};

struct telegram;
struct download;

/*
 * Events 1 arg
 */

#define DEFINE_EVENT_LISTENER(E_NAME, D_TYPE) void (*on_ ## E_NAME) (struct telegram *tg, D_TYPE *data);

#define DECLARE_EVENT_HANDLER(E_NAME, D_TYPE) \
void event_ ## E_NAME (struct telegram *tg, D_TYPE *data)

#define DEFINE_EVENT_HANDLER(E_NAME, D_TYPE) \
void event_ ## E_NAME (struct telegram *tg, D_TYPE *data) \
{ \
    if (tg->config->on_ ## E_NAME) { \
        tg->config->on_ ## E_NAME (tg, data); \
    } else { \
        warning ("Trying to execute non-existing event listener %s\n", "E_NAME"); \
    } \
}

/*
 * Events 3 args
 */

#define DEFINE_EVENT_LISTENER_3(E_NAME, D_TYPE, D_TYPE2, D_TYPE3) void (*on_ ## E_NAME) (struct telegram *tg, D_TYPE data, D_TYPE2 data2, D_TYPE3 data3);

#define DECLARE_EVENT_HANDLER_3(E_NAME, D_TYPE, D_TYPE2, D_TYPE3) \
void event_ ## E_NAME (struct telegram *tg, D_TYPE data, D_TYPE2 data2, D_TYPE3 data3)

#define DEFINE_EVENT_HANDLER_3(E_NAME, D_TYPE, D_TYPE2, D_TYPE3) \
void event_ ## E_NAME (struct telegram *tg, D_TYPE data, D_TYPE2 data2, D_TYPE3 data3) \
{ \
    if (tg->config->on_ ## E_NAME) { \
        tg->config->on_ ## E_NAME (tg, data, data2, data3); \
    } else { \
    warning ("Trying to execute non-existing event listener %s\n", "E_NAME"); \
    } \
}


/**
 * Contains all options and pointer to callback functions required by telegram
 */
struct telegram_config {

    /**
     * The base path containing the telegram configuration
     */
    char* base_config_path;
    
    /**
     * Called when there is pending network output
     */
    void (*on_output)(void *handle);

    /**
     * A callback function that delivers a connections to the given hostname
     * and port by calling telegram_set_proxy. This is useful for tunelling
     * the connection through a proxy server.
     */
    void (*proxy_request_cb) (struct telegram *instance, struct proxy_request *req);
    
    /**
     * A callback function that is called once the proxy connection is no longer
     * needed. This is useful for freeing all used resources.
     */
     void (*proxy_close_cb) (void *handle);

    /**
     * A callback function that is called when a phone registration is required. 
     *
     * This callback must query first name, last name and the 
     * authentication code from the user and call do_send_code_result_auth once done
     */
    void (*on_phone_registration_required) (struct telegram *instance);

    /**
     * A callback function that is called when a client registration is required. 
     *
     * This callback must query the authentication code from the user and
     * call do_send_code_result once done
     */
    void (*on_client_registration_required) (struct telegram *instance);

    /** 
     * A callback function that is called when telegram is ready
     */
    void (*on_ready) (struct telegram *instance);

    /** 
     * A callback function that is called when telegram is disconnected
     */
    void (*on_error) (struct telegram *instance, const char *err);

    /**
     * A callback function that is called when a new peer was allocated. This is useful
     * for populating the GUI with new peers.
     */
    DEFINE_EVENT_LISTENER(peer_allocated, void);
    
    /**
     * A callback function that is called when a user's status has changed
     */ 
    DEFINE_EVENT_LISTENER(update_user_status, void);

    /**
     * A callback function that is called when a user starts or stops typing
     */
    DEFINE_EVENT_LISTENER(update_user_typing, void);
    DEFINE_EVENT_LISTENER(update_user_name, peer_t);
    DEFINE_EVENT_LISTENER(update_user_photo, peer_t);
    DEFINE_EVENT_LISTENER(update_user_registered, peer_t);
    
    DEFINE_EVENT_LISTENER(update_chat_participants, peer_t);
    
    /**
     * A new user is added to a chat
     *
     * @param data1 The chat
     * @param data2 The added user
     * @param data3 The inviter
     */
    DEFINE_EVENT_LISTENER_3(update_chat_add_participant, peer_t *, peer_id_t, peer_id_t);
    
    /**
     * A user is deleted from a chat
     *
     * @param data1 The chat
     * @param data2 The added user
     * @param data3 NULL
     */
    DEFINE_EVENT_LISTENER_3(update_chat_del_participant, peer_t *, peer_id_t, void *);
    
    /**
     * A user in a chat is typing
     *
     * @param data1 The chat
     * @param data2 The user
     * @param data3 NULL
     */
    DEFINE_EVENT_LISTENER_3(update_chat_user_typing, peer_t *, peer_t *, void *);
    
    /**
     * A new device was registered for @location
     * 
     * @param tg
     * @param location
     */
    DEFINE_EVENT_LISTENER(update_auth_new, char);
    
    /**
     * A callback function that is called when a new message was allocated. This is useful
     * for adding new messages to the GUI.
     */
    DEFINE_EVENT_LISTENER(update_new_message, struct message);
    
    /**
     * A callback function that is called when a download is completed. This is useful
     * for populating the GUI with new user photos.
     */
    DEFINE_EVENT_LISTENER(download_finished, struct download);
    
    /**
     * A callback function that is called when a peer user info was received. This is useful
     * for populating the GUI with new user photos.
     */
    void (*on_user_info_received_handler) (struct telegram *instance, struct tgl_user *peer, int showInfo);
    
    /**
     * A callback function that is called when chat info is received
     */
    void (*on_chat_info_received) (struct telegram *instance, peer_id_t chatid); 
};

DECLARE_EVENT_HANDLER (peer_allocated, void);
DECLARE_EVENT_HANDLER (update_user_status, void);
DECLARE_EVENT_HANDLER (update_user_typing, void);

DECLARE_EVENT_HANDLER (update_user_name, peer_t);
DECLARE_EVENT_HANDLER (update_user_photo, peer_t);
DECLARE_EVENT_HANDLER (update_user_registered, peer_t);

DECLARE_EVENT_HANDLER (update_chat_participants, peer_t);
DECLARE_EVENT_HANDLER_3 (update_chat_add_participant, peer_t *, peer_id_t, peer_id_t);
DECLARE_EVENT_HANDLER_3 (update_chat_del_participant, peer_t *, peer_id_t, void *);
DECLARE_EVENT_HANDLER_3 (update_chat_user_typing, peer_t *, peer_t *, void *);
DECLARE_EVENT_HANDLER (update_auth_new, char);

DECLARE_EVENT_HANDLER (update_new_message, struct message);
DECLARE_EVENT_HANDLER (download_finished, struct download);

#define MSG_STORE_SIZE 10000

/**
 * A telegram session
 *
 * Contains all globals from the telegram-cli application is passed to every
 * query call
 */
struct telegram {
    void *protocol_data; 
    //int curr_dc;

    char *login;
    char *config_path;
    char *download_path;
    char *auth_path;
    char *state_path;
    char *secret_path;

    int session_state;
    struct telegram_config *config;
    
    /*
     * protocol state
     */
    struct protocol_state proto;
    struct authorization_state auth;

    /*
     * connection
     */
    struct mtproto_connection *connection;

    /*
     * binlog
     */
    struct binlog *bl;

    // TODO: Bind this to the current data center, since the code hash is only
    // valid in its context
    char *phone_code_hash;

    int unread_messages;
    long long cur_uploading_bytes;
    long long cur_uploaded_bytes;
    long long cur_downloading_bytes;
    long long cur_downloaded_bytes;
    int our_id;
    struct tgl_user User;
    BN_CTX *ctx;
    int encr_root;
    unsigned char *encr_prime;
    int encr_param_version;
    int max_chat_size;
    int max_bcast_size;
    int want_dc_num;
    int new_dc_num;
    int out_message_num;
    char *suser;
    int nearest_dc_num;
    int packed_buffer[MAX_PACKED_SIZE / 4];
    struct tree_query *queries_tree;
    struct tree_timer *timer_tree;
    char *export_auth_str;
    int export_auth_str_len;
    char g_a[256];
    // do_get_difference
    int get_difference_active;
    struct message *ML[MSG_STORE_SIZE];

    /*
     * All active MtProto connections
     */
    int cs;
    struct mtproto_connection *Cs[100]; 

    /*
     * Downloads
     */
    GQueue *dl_queue;
    struct download *dl_curr;

    /*
     * additional user data
     */
    void *extra;
};

/**
 * Create a new telegram application
 *
 * @param login         The phone number to use as login name
 * @param config        Contains all callbacks used for the telegram instance
 */
struct telegram *telegram_new(const char* login, struct telegram_config *config);

void telegram_restore_session(struct telegram *instance);
void telegram_store_session(struct telegram *instance);
void telegram_destroy(struct telegram *instance);

/** 
 * Get the currently active connection
 */
struct connection *telegram_get_connection(struct telegram *instance);

/**
 * Return the current working dc
 */
struct dc *telegram_get_working_dc(struct telegram *instance);

/* 
 * Events
 */

/**
 * Change the state of the given telegram instance and execute all event handlers
 *
 * @param instance  The telegram instance that changed its state
 * @param state     The changed state
 * @param data      Extra data that depends on switched state
 */
void telegram_change_state(struct telegram *instance, int state, void *data);

/**
 * Connect to the telegram network with the given configuration
 */
void telegram_connect(struct telegram *instance);

/**
 * Read and process all available input from the network
 */
void mtp_read_input (struct mtproto_connection *mtp);

/**
 * Write all available output to the network
 */
int mtp_write_output (struct mtproto_connection *mtp);

/**
 * Try to interpret RPC calls and apply the changes to the current telegram state
 */
void try_rpc_interpret(struct telegram *instance, int op, int len);

/**
 * Request a registration code
 */
char* network_request_registration();

/**
 * Verify the registration with the given registration code
 */
int network_verify_registration(const char *code, const char *sms_hash);

/**
 * Verify the registration with the given registration code
 */
int network_verify_phone_registration(const char *code, const char *sms_hash, 
	const char *first, const char *last);

/*
 * Load known users and chats on connect
 */
void event_user_info_received_handler(struct telegram *instance, struct tgl_user *peer, int showInfo);

/**
 * Set the connection after a proxy_request_cb 
 *
 * @param fd      The file-descriptor of the acquired connection
 * @param handle  A handle that will be passed back on output and close callbacks
 */
struct mtproto_connection *telegram_add_proxy(struct telegram *tg, struct proxy_request *req, int fd, void *handle);

/**
 * Return wether telegram is authenticated with the currently active data center
 */
int telegram_authenticated (struct telegram *instance);

void telegram_flush (struct telegram *instance);
void telegram_dl_add (struct telegram *instance, struct download *dl);
void telegram_dl_next (struct telegram *instance);

#endif
