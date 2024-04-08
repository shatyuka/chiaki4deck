/*
 * SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
 *
 * This file is part of Chiaki - A Free and Open Source PS4 Remote Play Client
 *
 * UDP Hole Punching Implementation
 * --------------------------------
 *
 * "Remote Play over Internet" uses a custom UDP-based protocol for communication between the
 * console and the client (see `rudp.h` for details on that). The protocol is designed to work
 * even if both the console and the client are behind NATs, by using UDP hole punching via
 * an intermediate server. This file implements the hole punching logic using PSN APIs.
 *
 * The hole punching process itself i
 */

// TODO: Make portable for Windows
// TODO: Make portable for MacOS
// TODO: Make portable for Switch

#include <string.h>
#include <time.h>
#include <linux/if_arp.h>
#include <linux/sockios.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <ifaddrs.h>

#include <curl/curl.h>
#include <json-c/json_object.h>
#include <json-c/json_tokener.h>
#include <json-c/json_pointer.h>
// TODO: Make UPnP optional
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>

#include <chiaki/remote/holepunch.h>
#include <chiaki/stoppipe.h>
#include <chiaki/thread.h>
#include <chiaki/base64.h>
#include <chiaki/random.h>
#include <chiaki/sock.h>

#include "../utils.h"
#include "stun.h"

#define UUIDV4_STR_LEN 37
#define SECOND_NS 1000000000L
#define MILLISECONDS_NS 1000000L
#define WEBSOCKET_PING_INTERVAL_SEC 5
// Maximum WebSocket frame size currently supported by libcurl
#define WEBSOCKET_MAX_FRAME_SIZE 64 * 1024
#define SESSION_CREATION_TIMEOUT_SEC 30
#define SESSION_START_TIMEOUT_SEC 30
#define MSG_TYPE_REQ 6
#define MSG_TYPE_RESP 7

static const char oauth_header_fmt[] = "Authorization: Bearer %s";

// Endpoints we're using
static const char device_list_url_fmt[] = "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/clients?platform=%s&includeFields=device&limit=10&offset=0";
static const char ws_fqdn_api_url[] = "https://mobile-pushcl.np.communication.playstation.net/np/serveraddr?version=2.1&fields=keepAliveStatus&keepAliveStatusType=3";
static const char session_create_url[] = "https://web.np.playstation.com/api/sessionManager/v1/remotePlaySessions";
static const char session_command_url[] = "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/commands";
static const char session_message_url_fmt[] = "https://web.np.playstation.com/api/sessionManager/v1/remotePlaySessions/%s/sessionMessage";

// JSON payloads for requests.
// Implemented as string templates due to the broken JSON used by the official app, which we're
// trying to emulate.
static const char session_create_json_fmt[] =
    "{\"remotePlaySessions\":["
        "{\"members\":["
            "{\"accountId\":\"me\","
             "\"deviceUniqueId\":\"me\","
             "\"platform\":\"me\","
             "\"pushContexts\":["
                "{\"pushContextId\":\"%s\"}]}]}]}";  // 0: push context ID
static const char session_start_envelope_fmt[] =
    "{\"commandDetail\":"
        "{\"commandType\":\"remotePlay\","
         "\"duid\":\"%s\","                            // 0: device identifier, lowercase hex string
         "\"messageDestination\":\"SQS\","
         "\"parameters\":{\"initialParams\":\"%s\"},"  // 1: string containing escaped JSON data
         "\"platform\":\"%s\"}}";                      // 2: PS4/PS5
static const char session_message_envelope_fmt[] =
    "{\"channel\":\"remote_play:1\","
     "\"payload\":\"ver=1.0, type=text, body=%s\","  // 0: message body as JSON string
     "\"to\":["
       "{\"accountId\":\"%lu\","                     // 1: PSN account ID
        "\"deviceUniqueId\":\"%s\","                 // 2: device UID, lowercase hex string
        "\"platform\":\"%s\"}]}";                    // 3: PS4/PS5

// NOTE: These payloads are JSON-escaped, since they're going to be embedded in a JSON string
static const char session_start_payload_fmt[] =
    "{\\\"accountId\\\":%lu,"             // 0: PSN account ID, integer
     "\\\"roomId\\\":0,"
     "\\\"sessionId\\\":\\\"%s\\\","      // 1: session identifier (lowercase UUIDv4)
     "\\\"clientType\\\":\\\"Windows\\\","
     "\\\"data1\\\":\\\"%s\\\","          // 2: 16 byte data1, base64 encoded
     "\\\"data2\\\":\\\"%s\\\"}";         // 3: 16 byte data2, base64 encoded
static const char session_message_fmt[] =
    "{\\\"action\\\":\\\"%s\\\"," // 0: OFFER/RESULT/ACCEPT
     "\\\"reqId\\\":%d,"          // 1: request ID, integer
     "\\\"error\\\":%d,"          // 2: error id, integer
     "\\\"connRequest\\\":%s}";   // 3: connRequest JSON object
static const char session_connrequest_fmt[] =
    "{\\\"sid\\\":%d,"                         // 0: sid
     "\\\"peerSid\\\":%d,"                     // 1: peer sid
     "\\\"skey\\\":\\\"%s\\\","                // 2: skey, 16 byte buffer, base64 encoded
     "\\\"natType\\\":%d,"                     // 3: NAT type
     "\\\"candidate\\\":%s,"                   // 4: Candidates, JSON array of objects
     "\\\"defaultRouteMacAddr\\\":\\\"%s\\\"," // 5: colon-separated lowercase values
     // NOTE: Needs to be an empty string if local peer address is not submitted
     //       This leads to broken JSON, but the official app does it this way as well ¯\_(ツ)_/¯
     "\\\"localPeerAddr\\\":%s,"               // 6: JSON object or **empty string**
     "\\\"localHashedId\\\":\\\"%s\\\"}";      // 7: 16 byte buffer, base64 encoded
static const char session_conrequest_candidate_fmt[] =
    "{\\\"type\\\":\\\"%s\\\","        // 0: STATIC/LOCAL
     "\\\"addr\\\":\\\"%s\\\","        // 1: IPv4 address
     "\\\"mappedAddr\\\":\\\"%s\\\","  // 2: IPv4 address
     "\\\"port\\\":%d,"                // 3: Port
     "\\\"mappedPort\\\":%d}";         // 4: Mapped Port
static const char session_localpeeraddr_fmt[] =
    "{\\\"accountId\\\":\\\"%lu\\\","   // 0: PSN account ID
     "\\\"platform\\\":\\\"%s\\\"}";    // 1: "PROSPERO" for PS5, "ORBIS" for PS4, "REMOTE_PLAY" for client

typedef enum notification_type_t
{
    NOTIFICATION_TYPE_UNKNOWN = 0,
    // psn:sessionManager:sys:remotePlaySession:created
    NOTIFICATION_TYPE_SESSION_CREATED = 1 << 0,
    // psn:sessionManager:sys:rps:members:created
    NOTIFICATION_TYPE_MEMBER_CREATED = 1 << 1,
    // psn:sessionManager:sys:rps:members:deleted
    NOTIFICATION_TYPE_MEMBER_DELETED = 1 << 2,
    // psn:sessionManager:sys:rps:customData1:updated
    NOTIFICATION_TYPE_CUSTOM_DATA1_UPDATED = 1 << 3,
    // psn:sessionManager:sys:rps:sessionMessage:created
    NOTIFICATION_TYPE_SESSION_MESSAGE_CREATED = 1 << 4
} NotificationType;

typedef struct notification_queue_t
{
    struct notification_queue_t *previous;

    NotificationType type;
    json_object* json;
    char* json_buf;
    size_t json_buf_size;
} Notification;

typedef enum session_state_t
{
    SESSION_STATE_INIT = 0,
    SESSION_STATE_WS_OPEN = 1 << 0,
    SESSION_STATE_CREATED = 1 << 1,
    SESSION_STATE_STARTED = 1 << 2,
    SESSION_STATE_CLIENT_JOINED = 1 << 3,
    SESSION_STATE_DATA_SENT = 1 << 4,
    SESSION_STATE_CONSOLE_JOINED = 1 << 5,
    SESSION_STATE_CUSTOMDATA1_RECEIVED = 1 << 6,
    SESSION_STATE_CTRL_OFFER_RECEIVED = 1 << 7,
    SESSION_STATE_CTRL_OFFER_SENT = 1 << 8,
    SESSION_STATE_CTRL_CONSOLE_ACCEPTED = 1 << 9,
    SESSION_STATE_CTRL_CLIENT_ACCEPTED = 1 << 10,
    SESSION_STATE_CTRL_ESTABLISHED = 1 << 11,
    SESSION_STATE_DATA_OFFER_RECEIVED = 1 << 12,
    SESSION_STATE_DATA_OFFER_SENT = 1 << 13,
    SESSION_STATE_DATA_CONSOLE_ACCEPTED = 1 << 14,
    SESSION_STATE_DATA_CLIENT_ACCEPTED = 1 << 15,
    SESSION_STATE_DATA_ESTABLISHED = 1 << 16
} SessionState;

typedef struct session_t
{
    // TODO: Clean this up, how much of this stuff do we really need?
    char* oauth_header;
    uint8_t console_uid[32];
    ChiakiHolepunchConsoleType console_type;

    chiaki_socket_t sock;

    uint64_t account_id;
    char session_id[UUIDV4_STR_LEN];
    char pushctx_id[UUIDV4_STR_LEN];

    uint16_t sid_local;
    uint16_t sid_console;
    uint8_t hashed_id_local[20];
    uint8_t hashed_id_console[20];

    uint8_t data1[16];
    uint8_t data2[16];
    uint8_t custom_data1[16];

    CURLSH* curl_share;

    char* ws_fqdn;
    ChiakiThread ws_thread;
    Notification* ws_notification_queue;
    bool ws_thread_should_stop;

    ChiakiStopPipe notif_pipe;
    ChiakiMutex notif_mutex;
    ChiakiCond notif_cond;

    SessionState state;
    ChiakiMutex state_mutex;
    ChiakiCond state_cond;

    char* client_addr_static;
    char* client_addr_local;
    chiaki_socket_t client_sock;
    chiaki_socket_t ctrl_sock;
    chiaki_socket_t data_sock;

    ChiakiLog *log;
} Session;

typedef enum session_message_action_t
{
    SESSION_MESSAGE_ACTION_UNKNOWN = 0,
    SESSION_MESSAGE_ACTION_OFFER = 1,
    SESSION_MESSAGE_ACTION_RESULT = 1 << 2,
    SESSION_MESSAGE_ACTION_ACCEPT = 1 << 3,
    SESSION_MESSAGE_ACTION_TERMINATE = 1 << 4,
} SessionMessageAction;

typedef struct http_response_data_t
{
    char* data;
    size_t size;
} HttpResponseData;

typedef struct upnp_gateway_info_t
{
    char lan_ip[16];
    struct UPNPUrls *urls;
    struct IGDdatas *data;
} UPNPGatewayInfo;

typedef enum candidate_type_t
{
    CANDIDATE_TYPE_STATIC = 0,
    CANDIDATE_TYPE_LOCAL = 1,
} CandidateType;

typedef struct candidate_t
{
    CandidateType type;
    char addr[INET6_ADDRSTRLEN];
    char addr_mapped[INET6_ADDRSTRLEN];
    uint16_t port;
    uint16_t port_mapped;
} Candidate;

typedef struct connection_request_t
{
    uint32_t sid;
    uint32_t peer_sid;
    uint8_t skey[16];
    uint8_t nat_type;
    Candidate *candidates;
    size_t num_candidates;
    uint8_t default_route_mac_addr[6];
    uint8_t local_hashed_id[20];
} ConnectionRequest;

typedef struct session_message_t
{
    SessionMessageAction action;
    uint16_t req_id;
    uint16_t error;
    ConnectionRequest *conn_request;
    Notification *notification;
} SessionMessage;

static void make_oauth2_header(char** out, const char* token);
static ChiakiErrorCode get_websocket_fqdn(
    Session *session, char **fqdn);
static inline size_t curl_write_cb(
    void* ptr, size_t size, size_t nmemb, void* userdata);
static void hex_to_bytes(const char* hex_str, uint8_t* bytes, size_t max_len);
static void bytes_to_hex(const uint8_t* bytes, size_t len, char* hex_str, size_t max_len);
static void random_uuidv4(char* out);
static void *websocket_thread_func(void *user);
static NotificationType parse_notification_type(ChiakiLog *log, json_object* json);
static ChiakiErrorCode send_offer(Session *session, int req_id, Candidate *local_console_candidate);
static ChiakiErrorCode send_accept(Session *session, int req_id, Candidate *selected_candidate);
static ChiakiErrorCode http_create_session(Session *session);
static ChiakiErrorCode http_start_session(Session *session);
static ChiakiErrorCode http_send_session_message(Session *session, SessionMessage *msg);
static ChiakiErrorCode get_client_addr_local(Session *session, Candidate *local_console_candidate, char *out, size_t out_len);
static ChiakiErrorCode upnp_get_gateway_info(ChiakiLog *log, UPNPGatewayInfo *info);
static bool get_client_addr_remote_upnp(UPNPGatewayInfo *gw_info, char *out);
static bool upnp_add_udp_port_mapping(UPNPGatewayInfo *gw_info, uint16_t port_internal, uint16_t port_external, char* ip_local);
static bool upnp_delete_udp_port_mapping(UPNPGatewayInfo *gw_info, uint16_t port_external);
static bool get_client_addr_remote_stun(ChiakiLog *log, char *out);
static bool get_mac_addr(struct addrinfo *ip_addr, uint8_t *mac_addr);
static void log_session_state(Session *session);
static ChiakiErrorCode decode_customdata1(const char *customdata1, uint8_t *out, size_t out_len);
static ChiakiErrorCode check_candidates(
    Session *session, Candidate *candidate, size_t num_candidates, chiaki_socket_t *out,
    uint16_t *out_port, Candidate **out_candidate);

static json_object* session_message_get_payload(ChiakiLog *log, json_object *session_message);
static SessionMessageAction get_session_message_action(json_object *payload);
static ChiakiErrorCode wait_for_notification(
    Session *session, Notification** out,
    uint16_t types, uint64_t timeout_ms);
static ChiakiErrorCode clear_notification(
    Session *session, Notification *notification);
static void notification_queue_free(Notification* queue);

static ChiakiErrorCode wait_for_session_message(
    Session *session, SessionMessage** out,
    uint16_t types, uint64_t timeout_ms);
static ChiakiErrorCode wait_for_session_message_ack(
    Session *session, int req_id, uint64_t timeout_ms);
static ChiakiErrorCode session_message_parse(
    ChiakiLog *log, json_object *message_json, SessionMessage **out);
static ChiakiErrorCode session_message_serialize(
    Session *session, SessionMessage *message, char **out, size_t *out_len);
static ChiakiErrorCode session_message_free(SessionMessage *message);
static void print_session_request(ChiakiLog *log, ConnectionRequest *req);
static void print_candidate(ChiakiLog *log, Candidate *candidate);

CHIAKI_EXPORT ChiakiErrorCode chiaki_holepunch_list_devices(
    const char* psn_oauth2_token, ChiakiHolepunchConsoleType console_type,
    ChiakiHolepunchDeviceInfo **devices, size_t *device_count,
    ChiakiLog *log)
{
    CURL *curl = curl_easy_init();
    char url[133];

    char *platform;
    if (console_type == CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS4) {
        platform = "PS4";
    } else {
        platform = "PS5";
    }
    snprintf(url, sizeof(url), device_list_url_fmt, platform);

    char* oauth_header = NULL;
    make_oauth2_header(&oauth_header, psn_oauth2_token);

    HttpResponseData response_data = {
        .data = malloc(0),
        .size = 0,
    };

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept-Language: jp");
    headers = curl_slist_append(headers, oauth_header);

    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response_data);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
    if (res != CURLE_OK)
    {
        if (res == CURLE_HTTP_RETURNED_ERROR)
        {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            CHIAKI_LOGE(log, "chiaki_holepunch_list_devices: Fetching device list from %s failed with HTTP code %ld", url, http_code);
            CHIAKI_LOGD(log, "Response Body: %.*s.", response_data.size, response_data.data);
            err = CHIAKI_ERR_HTTP_NONOK;
        } else {
            CHIAKI_LOGE(log, "chiaki_holepunch_list_devices: Fetching device list from %s failed with CURL error %d", url, res);
            err = CHIAKI_ERR_NETWORK;
        }
        goto cleanup;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200)
    {
        CHIAKI_LOGE(log, "chiaki_holepunch_list_devices: Fetching device list from %s failed with HTTP code %ld", url, http_code);
        err = CHIAKI_ERR_HTTP_NONOK;
        goto cleanup;
    }
    json_tokener *tok = json_tokener_new();
    json_object *json = json_tokener_parse_ex(tok, response_data.data, response_data.size);
    if (json == NULL)
    {
        CHIAKI_LOGE(log, "chiaki_holepunch_list_devices: Parsing JSON failed");
        err = CHIAKI_ERR_UNKNOWN;
        goto cleanup;
    }

    json_object *clients;
    if (!json_object_object_get_ex(json, "clients", &clients))
    {
        CHIAKI_LOGE(log, "chiaki_holepunch_list_devices: JSON does not contain \"clients\" field");
        err = CHIAKI_ERR_UNKNOWN;
        goto cleanup_json;
    } else if (!json_object_is_type(clients, json_type_array))
    {
        CHIAKI_LOGE(log, "chiaki_holepunch_list_devices: JSON \"clients\" field is not an array");
        err = CHIAKI_ERR_UNKNOWN;
        goto cleanup_json;
    }

    size_t num_clients = json_object_array_length(clients);
    *devices = malloc(sizeof(ChiakiHolepunchDeviceInfo) * num_clients);
    *device_count = num_clients;
    for (size_t i = 0; i < num_clients; i++)
    {
        ChiakiHolepunchDeviceInfo *device = devices[i];
        device->type = console_type;

        json_object *client = json_object_array_get_idx(clients, i);
        json_object *duid;
        if (!json_object_object_get_ex(client, "duid", &duid))
        {
            CHIAKI_LOGE(log, "chiaki_holepunch_list_devices: JSON does not contain \"duid\" field");
            err = CHIAKI_ERR_UNKNOWN;
            goto cleanup_devices;
        } else if (!json_object_is_type(duid, json_type_string))
        {
            CHIAKI_LOGE(log, "chiaki_holepunch_list_devices: JSON \"duid\" field is not a string");
            err = CHIAKI_ERR_UNKNOWN;
            goto cleanup_devices;
        }
        hex_to_bytes(json_object_get_string(duid), device->device_uid, sizeof(device->device_uid));

        json_object *device_json;
        if (!json_object_object_get_ex(client, "device", &device_json))
        {
            CHIAKI_LOGE(log, "chiaki_holepunch_list_devices: JSON does not contain \"device\" field");
            err = CHIAKI_ERR_UNKNOWN;
            goto cleanup_devices;
        } else if (!json_object_is_type(device_json, json_type_object))
        {
            CHIAKI_LOGE(log, "chiaki_holepunch_list_devices: JSON \"device\" field is not an object");
            err = CHIAKI_ERR_UNKNOWN;
            goto cleanup_devices;
        }

        json_object *enabled_features;
        if (!json_object_object_get_ex(device_json, "enabledFeatures", &enabled_features))
        {
            CHIAKI_LOGE(log, "chiaki_holepunch_list_devices: JSON does not contain \"enabledFeatures\" field");
            err = CHIAKI_ERR_UNKNOWN;
            goto cleanup_devices;
        } else if (!json_object_is_type(enabled_features, json_type_array))
        {
            CHIAKI_LOGE(log, "chiaki_holepunch_list_devices: JSON \"enabledFeatures\" field is not an array");
            err = CHIAKI_ERR_UNKNOWN;
            goto cleanup_devices;
        }
        device->remoteplay_enabled = false;
        size_t num_enabled_features = json_object_array_length(enabled_features);
        for (size_t j = 0; j < num_enabled_features; j++)
        {
            json_object *feature = json_object_array_get_idx(enabled_features, j);
            if (json_object_is_type(feature, json_type_string) && strcmp(json_object_get_string(feature), "remotePlay") == 0)
            {
                device->remoteplay_enabled = true;
                break;
            }
        }

        json_object *device_name;
        if (!json_object_object_get_ex(device_json, "name", &device_name))
        {
            CHIAKI_LOGE(log, "chiaki_holepunch_list_devices: JSON does not contain \"name\" field");
            err = CHIAKI_ERR_UNKNOWN;
            goto cleanup_devices;
        } else if (!json_object_is_type(device_name, json_type_string))
        {
            CHIAKI_LOGE(log, "chiaki_holepunch_list_devices: JSON \"name\" field is not a string");
            err = CHIAKI_ERR_UNKNOWN;
            goto cleanup_devices;
        }
        strncpy(device->device_name, json_object_get_string(device_name), sizeof(device->device_name));
    }

cleanup_devices:
if (err != CHIAKI_ERR_SUCCESS)
    chiaki_holepunch_free_device_list(*devices);
cleanup_json:
    json_object_put(json);
    json_tokener_free(tok);
cleanup:
    free(oauth_header);
    free(response_data.data);
    curl_easy_cleanup(curl);
    return err;
}

CHIAKI_EXPORT void chiaki_holepunch_free_device_list(ChiakiHolepunchDeviceInfo* devices)
{
    free(devices);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_holepunch_generate_client_device_uid(
    char *out, size_t *out_size)
{
    if (*out_size < CHIAKI_DUID_STR_SIZE)
    {
        return CHIAKI_ERR_BUF_TOO_SMALL;
    }
    uint8_t random_bytes[16];
    ChiakiErrorCode err = chiaki_random_bytes_crypt(random_bytes, sizeof(random_bytes));
    if (err != CHIAKI_ERR_SUCCESS)
        return err;

    *out_size += sprintf(out, "%s", DUID_PREFIX);
    for (int i = 0; i < sizeof(random_bytes); i++)
    {
        *out_size += sprintf(out + strlen(out), "%02x", random_bytes[i]);
    }

    return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT Session* chiaki_holepunch_session_init(
    const char* psn_oauth2_token, ChiakiLog *log)
{
    Session *session = malloc(sizeof(Session));
    make_oauth2_header(&session->oauth_header, psn_oauth2_token);
    session->log = log;

    session->ws_fqdn = NULL;
    session->ws_notification_queue = NULL;
    session->ws_thread_should_stop = false;
    session->client_addr_static = NULL;
    session->client_addr_local = NULL;
    memset(&session->session_id, 0, sizeof(session->session_id));
    memset(&session->console_uid, 0, sizeof(session->console_uid));
    memset(&session->hashed_id_console, 0, sizeof(session->hashed_id_console));
    memset(&session->custom_data1, 0, sizeof(session->custom_data1));
    session->client_sock = -1;
    session->ctrl_sock = -1;
    session->data_sock = -1;
    session->sock = -1;
    session->sid_console = 0;

    ChiakiErrorCode err;
    err = chiaki_mutex_init(&session->notif_mutex, false);
    assert(err == CHIAKI_ERR_SUCCESS);
    err = chiaki_cond_init(&session->notif_cond);
    assert(err == CHIAKI_ERR_SUCCESS);
    err = chiaki_stop_pipe_init(&session->notif_pipe);
    assert(err == CHIAKI_ERR_SUCCESS);
    err = chiaki_mutex_init(&session->state_mutex, false);
    assert(err == CHIAKI_ERR_SUCCESS);
    err = chiaki_cond_init(&session->state_cond);
    assert(err == CHIAKI_ERR_SUCCESS);

    session->curl_share = curl_share_init();
    assert(session->curl_share != NULL);

    chiaki_mutex_lock(&session->state_mutex);
    session->state = SESSION_STATE_INIT;
    log_session_state(session);
    chiaki_mutex_unlock(&session->state_mutex);

    random_uuidv4(session->pushctx_id);
    session->sid_local = chiaki_random_32();
    chiaki_random_bytes_crypt(session->hashed_id_local, sizeof(session->hashed_id_local));
    chiaki_random_bytes_crypt(session->data1, sizeof(session->data1));
    chiaki_random_bytes_crypt(session->data2, sizeof(session->data2));

    return session;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_holepunch_session_create(Session* session)
{
    ChiakiErrorCode err = get_websocket_fqdn(session, &session->ws_fqdn);
    if (err != CHIAKI_ERR_SUCCESS)
        goto cleanup_curlsh;

    err = chiaki_thread_create(&session->ws_thread, websocket_thread_func, session);
    if (err != CHIAKI_ERR_SUCCESS)
        goto cleanup_curlsh;
    chiaki_thread_set_name(&session->ws_thread, "Chiaki Holepunch WS");
    CHIAKI_LOGD(session->log, "chiaki_holepunch_session_create: Created websocket thread");

    chiaki_mutex_lock(&session->state_mutex);
    while (!(session->state & SESSION_STATE_WS_OPEN))
    {
        CHIAKI_LOGD(session->log, "chiaki_holepunch_session_create: Waiting for websocket to open...");
        err = chiaki_cond_wait(&session->state_cond, &session->state_mutex);
        assert(err == CHIAKI_ERR_SUCCESS);
    }
    chiaki_mutex_unlock(&session->state_mutex);

    err = http_create_session(session);
    if (err != CHIAKI_ERR_SUCCESS)
        goto cleanup_thread;
    CHIAKI_LOGD(session->log, "chiaki_holepunch_session_create: Sent session creation request");


    // FIXME: We're currently not using a shared timeout for both  notifications, i.e. if the first one
    //        takes 29 secs arrive, and the second one takes 15 secs, we're not going to time out despite
    //        exceeing SESSION_CREATION_TIMEOUT_SEC.
    bool finished = false;
    Notification *notif = NULL;
    int notif_query = NOTIFICATION_TYPE_SESSION_CREATED
                    | NOTIFICATION_TYPE_MEMBER_CREATED;
    while (!finished)
    {
        err = wait_for_notification(session, &notif, notif_query, SESSION_CREATION_TIMEOUT_SEC * 1000);
        if (err == CHIAKI_ERR_TIMEOUT)
        {
            CHIAKI_LOGE(session->log, "chiaki_holepunch_session_create: Timed out waiting for session creation notifications.");
            break;
        }
        else if (err != CHIAKI_ERR_SUCCESS)
        {
            CHIAKI_LOGE(session->log, "chiaki_holepunch_session_create: Failed to wait for session creation notifications.");
            break;
        }

        chiaki_mutex_lock(&session->state_mutex);
        if (notif->type == NOTIFICATION_TYPE_SESSION_CREATED)
        {
            session->state |= SESSION_STATE_CREATED;
            CHIAKI_LOGD(session->log, "chiaki_holepunch_session_create: Session created.");
        }
        else if (notif->type == NOTIFICATION_TYPE_MEMBER_CREATED)
        {
            session->state |= SESSION_STATE_CLIENT_JOINED;
            CHIAKI_LOGD(session->log, "chiaki_holepunch_session_create: Client joined.");
        }
        else
        {
            CHIAKI_LOGE(session->log, "chiaki_holepunch_session_create: Got unexpected notification of type %d", notif->type);
            err = CHIAKI_ERR_UNKNOWN;
            break;
        }
        log_session_state(session);
        finished = (session->state & SESSION_STATE_CREATED) &&
                        (session->state & SESSION_STATE_CLIENT_JOINED);
        chiaki_mutex_unlock(&session->state_mutex);
        clear_notification(session, notif);
    }
    return CHIAKI_ERR_SUCCESS;

cleanup_thread:
    session->ws_thread_should_stop = true;
    chiaki_thread_join(&session->ws_thread, NULL);
cleanup_curlsh:
    curl_share_cleanup(session->curl_share);
    return err;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_holepunch_session_start(
    Session* session, const uint8_t* device_uid,
    ChiakiHolepunchConsoleType console_type)
{
    if (!(session->state & SESSION_STATE_CREATED))
    {
        CHIAKI_LOGE(session->log, "chiaki_holepunch_session_start: Session not created yet");
        return CHIAKI_ERR_UNINITIALIZED;
    }
    if (session->state & SESSION_STATE_STARTED)
    {
        CHIAKI_LOGE(session->log, "chiaki_holepunch_session_start: Session already started");
        return CHIAKI_ERR_UNKNOWN;
    }
    char duid_str[64];
    bytes_to_hex(device_uid, 32, duid_str, sizeof(duid_str));
    CHIAKI_LOGD(session->log, "chiaki_holepunch_session_start: Starting session %s for device %s", session->session_id, duid_str);
    memcpy(session->console_uid, device_uid, sizeof(session->console_uid));
    session->console_type = console_type;
    ChiakiErrorCode err = http_start_session(session);
    if (err != CHIAKI_ERR_SUCCESS)
    {
        CHIAKI_LOGE(session->log, "chiaki_holepunch_session_start: Starting session failed with error %d", err);
        return err;
    }

    // FIXME: We're currently not using a shared timeout for both  notifications, i.e. if the first one
    //        takes 29 secs arrive, and the second one takes 15 secs, we're not going to time out despite
    //        exceeding SESSION_START_TIMEOUT_SEC.
    bool finished = false;
    Notification *notif = NULL;
    int notif_query = NOTIFICATION_TYPE_MEMBER_CREATED
                    | NOTIFICATION_TYPE_CUSTOM_DATA1_UPDATED;
    while (!finished)
    {
        err = wait_for_notification(session, &notif, notif_query, SESSION_START_TIMEOUT_SEC * 1000);
        if (err == CHIAKI_ERR_TIMEOUT)
        {
            CHIAKI_LOGE(session->log, "chiaki_holepunch_session_start: Timed out waiting for session start notifications.");
            break;
        }
        else if (err != CHIAKI_ERR_SUCCESS)
        {
            CHIAKI_LOGE(session->log, "chiaki_holepunch_session_start: Failed to wait for session start notifications.");
            break;
        }

        assert(notif != NULL);

        chiaki_mutex_lock(&session->state_mutex);
        if (notif->type == NOTIFICATION_TYPE_MEMBER_CREATED)
        {
            // Check if the session now contains the console we requested
            json_object *member_duid_json = NULL;
            json_pointer_get(notif->json, "/body/data/members/0/deviceUniqueId", &member_duid_json);
            if (!member_duid_json || !json_object_is_type(member_duid_json, json_type_string))
            {
                CHIAKI_LOGE(session->log, "chiaki_holepunch_session_start: JSON does not contain member with a deviceUniqueId string field!");
                const char *json_str = json_object_to_json_string_ext(notif->json, JSON_C_TO_STRING_PRETTY);
                CHIAKI_LOGD(session->log, "chiaki_holepunch_session_start: JSON was:\n%s", json_str);
                err = CHIAKI_ERR_UNKNOWN;
                break;
            }
            const char *member_duid = json_object_get_string(member_duid_json);
            if (strlen(member_duid) != 64)
            {
                CHIAKI_LOGE(session->log, "chiaki_holepunch_session_start: \"deviceUniqueId\" has unexpected length, got %d, expected 64", strlen(member_duid));
                err = CHIAKI_ERR_UNKNOWN;
                break;
            }

            uint8_t duid_bytes[32];
            hex_to_bytes(member_duid, duid_bytes, sizeof(duid_bytes));
            if (memcmp(duid_bytes, session->console_uid, sizeof(session->console_uid)) != 0)
            {
                CHIAKI_LOGE(session->log, "chiaki_holepunch_session_start: Session does not contain console");
                err = CHIAKI_ERR_UNKNOWN;
                break;
            }

            session->state |= SESSION_STATE_CONSOLE_JOINED;
        } else if (notif->type == NOTIFICATION_TYPE_CUSTOM_DATA1_UPDATED)
        {
            json_object *custom_data1_json = NULL;
            json_pointer_get(notif->json, "/body/data/customData1", &custom_data1_json);
            if (!custom_data1_json || !json_object_is_type(custom_data1_json, json_type_string))
            {
                CHIAKI_LOGE(session->log, "chiaki_holepunch_session_start: JSON does not contain \"customData1\" string field");
                const char *json_str = json_object_to_json_string_ext(notif->json, JSON_C_TO_STRING_PRETTY);
                CHIAKI_LOGD(session->log, "chiaki_holepunch_session_start: JSON was:\n%s", json_str);
                err = CHIAKI_ERR_UNKNOWN;
                break;
            }
            const char *custom_data1 = json_object_get_string(custom_data1_json);
            if (strlen(custom_data1) != 32)
            {
                CHIAKI_LOGE(session->log, "chiaki_holepunch_session_start: \"customData1\" has unexpected length, got %d, expected 32", strlen(custom_data1));
                err = CHIAKI_ERR_UNKNOWN;
                break;
            }
            err = decode_customdata1(custom_data1, session->custom_data1, sizeof(session->custom_data1));
            if (err != CHIAKI_ERR_SUCCESS)
            {
                CHIAKI_LOGE(session->log, "chiaki_holepunch_session_start: Failed to decode \"customData1\": '%s'", custom_data1);
                break;
            }
            session->state |= SESSION_STATE_CUSTOMDATA1_RECEIVED;
        }
        else
        {
            CHIAKI_LOGE(session->log, "chiaki_holepunch_session_start: Got unexpected notification of type %d", notif->type);
            err = CHIAKI_ERR_UNKNOWN;
            break;
        }
        clear_notification(session, notif);
        finished = (session->state & SESSION_STATE_CONSOLE_JOINED) &&
                   (session->state & SESSION_STATE_CUSTOMDATA1_RECEIVED);
        log_session_state(session);
        chiaki_mutex_unlock(&session->state_mutex);
    }
    return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_holepunch_session_punch_hole(Session* session, ChiakiHolepunchPortType port_type, chiaki_socket_t *out_sock)
{
    if (port_type == CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL
        && !(session->state & SESSION_STATE_CUSTOMDATA1_RECEIVED))
    {
        CHIAKI_LOGE(session->log, "chiaki_holepunch_session_punch_holes: customData1 not received yet.");
        return CHIAKI_ERR_UNKNOWN;
    }
    else if (port_type == CHIAKI_HOLEPUNCH_PORT_TYPE_DATA
        && !(session->state & SESSION_STATE_CTRL_ESTABLISHED))
    {
        CHIAKI_LOGE(session->log, "chiaki_holepunch_session_punch_holes: Control port not open yet.");
        return CHIAKI_ERR_UNKNOWN;
    }

    ChiakiErrorCode err;

    // NOTE: Needs to be kept around until the end, we're using the candidates in the message later on
    SessionMessage *console_offer_msg = NULL;
    err = wait_for_session_message(session, &console_offer_msg, SESSION_MESSAGE_ACTION_OFFER, SESSION_START_TIMEOUT_SEC * 1000);
    if (err == CHIAKI_ERR_TIMEOUT)
    {
        CHIAKI_LOGE(session->log, "chiaki_holepunch_session_punch_holes: Timed out waiting for OFFER session message.");
        return err;
    }
    else if (err != CHIAKI_ERR_SUCCESS)
    {
        CHIAKI_LOGE(session->log, "chiaki_holepunch_session_punch_holes: Failed to wait for OFFER session message.");
        return err;
    }
    ConnectionRequest *console_req = console_offer_msg->conn_request;
    memcpy(session->hashed_id_console, console_req->local_hashed_id, sizeof(session->hashed_id_console));
    session->sid_console = console_req->sid;

    chiaki_mutex_lock(&session->state_mutex);
    session->state |= SESSION_STATE_CTRL_OFFER_RECEIVED;
    chiaki_mutex_unlock(&session->state_mutex);

    Candidate *console_candidate_local;
    for (int i=0; i < console_req->num_candidates; i++)
    {
        Candidate *candidate = &console_req->candidates[i];
        if (candidate->type == CANDIDATE_TYPE_LOCAL)
        {
            console_candidate_local = candidate;
            break;
        }
    }

    // ACK the message
    SessionMessage ack_msg = {
        .action = SESSION_MESSAGE_ACTION_RESULT,
        .req_id = console_offer_msg->req_id,
        .error = 0,
        .conn_request = calloc(1, sizeof(ConnectionRequest)),
    };
    ack_msg.conn_request->num_candidates = 0;
    http_send_session_message(session, &ack_msg);
    free(ack_msg.conn_request);

    // Send our own OFFER
    const int our_offer_req_id = 1;
    send_offer(session, our_offer_req_id, console_candidate_local);

    // Wait for ACK of OFFER, ignore other OFFERs, simply ACK them
    err = wait_for_session_message_ack(
        session, our_offer_req_id, SESSION_START_TIMEOUT_SEC * 1000);
    if (err == CHIAKI_ERR_TIMEOUT)
    {
        CHIAKI_LOGE(session->log, "chiaki_holepunch_session_punch_holes: Timed out waiting for ACK of our connection offer.");
        goto cleanup;
    }
    else if (err != CHIAKI_ERR_SUCCESS)
    {
        CHIAKI_LOGE(session->log, "chiaki_holepunch_session_punch_holes: Failed to wait for ACK of our connection offer.");
        goto cleanup;
    }

    // Find candidate that we can use to connect to the console
    chiaki_socket_t sock = -1;
    uint16_t local_port = 0;
    Candidate *selected_candidate = NULL;
    err = check_candidates(session, console_req->candidates, console_req->num_candidates, &sock, &local_port, &selected_candidate);
    if (err != CHIAKI_ERR_SUCCESS)
    {
        CHIAKI_LOGE(
            session->log, "chiaki_holepunch_session_punch_holes: Failed to find reachable candidate for %s connection.",
            port_type == CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL ? "control" : "data");
        goto cleanup;
    }

    *out_sock = sock;

    err = send_accept(session, 2 /* FIXME: Base on previous reqid */, selected_candidate);
    if (err != CHIAKI_ERR_SUCCESS)
    {
        CHIAKI_LOGE(session->log, "chiaki_holepunch_session_punch_holes: Failed to send ACCEPT message.");
        goto cleanup;
    }

    // bool finished = false;
    SessionMessage *msg = NULL;
        err = wait_for_session_message(session, &msg, SESSION_MESSAGE_ACTION_ACCEPT, SESSION_START_TIMEOUT_SEC * 1000);
        if (err == CHIAKI_ERR_TIMEOUT)
        {
            CHIAKI_LOGE(session->log, "chiaki_holepunch_session_punch_holes: Timed out waiting for ACCEPT session message.");
            goto cleanup;
        }
        else if (err != CHIAKI_ERR_SUCCESS)
        {
            CHIAKI_LOGE(session->log, "chiaki_holepunch_session_punch_holes: Failed to wait for ACCEPT or OFFER session message.");
            goto cleanup;
        }

        if (msg->action == SESSION_MESSAGE_ACTION_ACCEPT)
        {
            chiaki_mutex_lock(&session->state_mutex);
            session->state |= SESSION_STATE_CTRL_ESTABLISHED;
            CHIAKI_LOGD(session->log, "chiaki_holepunch_session_punch_holes: Control connection established.");
            chiaki_mutex_unlock(&session->state_mutex);
        }
        // else if (msg->action == SESSION_MESSAGE_ACTION_OFFER)
        // {
        //     CHIAKI_LOGD(session->log, "chiaki_holepunch_session_punch_holes: Got unexpected OFFER session message.");
        //     // ACK the message
        //     SessionMessage ack_msg = {
        //         .action = SESSION_MESSAGE_ACTION_RESULT,
        //         .req_id = msg->req_id,
        //         .error = 0,
        //         .conn_request = calloc(1, sizeof(ConnectionRequest)),
        //     };
        //     ack_msg.conn_request->num_candidates = 0;
        //     http_send_session_message(session, &ack_msg);
        //     free(ack_msg.conn_request);
        // }
        log_session_state(session);

cleanup:
    session_message_free(console_offer_msg);

    return err;
}

CHIAKI_EXPORT void chiaki_holepunch_session_fini(Session* session)
{
    // TODO: Send delete request for session to PSN
    if (session->oauth_header)
        free(session->oauth_header);
    if (session->curl_share)
        curl_share_cleanup(session->curl_share);
    if (session->ws_fqdn)
        free(session->ws_fqdn);
    if (session->ws_notification_queue)
        notification_queue_free(session->ws_notification_queue);
    chiaki_stop_pipe_fini(&session->notif_pipe);
    chiaki_mutex_fini(&session->notif_mutex);
    chiaki_cond_fini(&session->notif_cond);
    chiaki_mutex_fini(&session->state_mutex);
    chiaki_cond_fini(&session->state_cond);
}

void notification_queue_free(Notification* queue)
{
    Notification* previous;
    while (queue != NULL)
    {
        previous = queue->previous;
        json_object_put(queue->json);
        queue->json = NULL;
        free(queue->json_buf);
        queue->json_buf = NULL;
        free(queue);
        queue = previous;
    }
}

static void make_oauth2_header(char** out, const char* token)
{
    size_t oauth_header_len = sizeof(oauth_header_fmt) + strlen(token);
    *out = malloc(oauth_header_len);
    snprintf(*out, oauth_header_len, oauth_header_fmt, token);
}

/**
 * Get the fully qualified domain name of the websocket server that we can
 * get PSN notifications from.
 *
 * @param[in] session Session instance
 * @param[out] fqdn Pointer to a char* that will be set to the FQDN of the
 *                 websocket server. Must be freed by the caller.
 * @return CHIAKI_ERR_SUCCESS on success, otherwise an error code
*/
static ChiakiErrorCode get_websocket_fqdn(Session *session, char **fqdn)
{
    HttpResponseData response_data = {
        .data = malloc(0),
        .size = 0,
    };

    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, session->oauth_header);

    curl_easy_setopt(curl, CURLOPT_SHARE, session->curl_share);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, ws_fqdn_api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response_data);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
    if (res != CURLE_OK)
    {
        if (res == CURLE_HTTP_RETURNED_ERROR)
        {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            CHIAKI_LOGE(session->log, "get_websocket_fqdn: Fetching websocket FQDN from %s failed with HTTP code %ld", ws_fqdn_api_url, http_code);
            err = CHIAKI_ERR_HTTP_NONOK;
        } else {
            CHIAKI_LOGE(session->log, "get_websocket_fqdn: Fetching websocket FQDN from %s failed with CURL error %d", ws_fqdn_api_url, res);
            err = CHIAKI_ERR_NETWORK;
        }
        goto cleanup;
    }

    json_tokener *tok = json_tokener_new();
    json_object *json = json_tokener_parse_ex(tok, response_data.data, response_data.size);
    if (json == NULL)
    {
        CHIAKI_LOGE(session->log, "get_websocket_fqdn: Parsing JSON failed");
        err = CHIAKI_ERR_UNKNOWN;
        goto cleanup;
    }
    json_object *fqdn_json;
    if (!json_object_object_get_ex(json, "fqdn", &fqdn_json))
    {
        CHIAKI_LOGE(session->log, "get_websocket_fqdn: JSON does not contain \"fqdn\" field");
        err = CHIAKI_ERR_UNKNOWN;
        goto cleanup_json;
    } else if (!json_object_is_type(fqdn_json, json_type_string))
    {
        CHIAKI_LOGE(session->log, "get_websocket_fqdn: JSON \"fqdn\" field is not a string");
        err = CHIAKI_ERR_UNKNOWN;
        goto cleanup_json;
    }
    *fqdn = strdup(json_object_get_string(fqdn_json));

cleanup_json:
    json_object_put(json);
    json_tokener_free(tok);
cleanup:
    curl_easy_cleanup(curl);
    free(response_data.data);
    return err;
}

/**
 * Write callback for CURL calls.
 *
 * @param[in] ptr Pointer to the data to write
 * @param[in] size Size of each element
 * @param[in] nmemb Number of elements
 * @param[in] userdata Pointer to a HttpResponseData struct
 * @return Number of bytes written
*/
static inline size_t curl_write_cb(
    void* ptr, size_t size, size_t nmemb, void* userdata)
{
    size_t realsize = size * nmemb;
    HttpResponseData* response_data = (HttpResponseData*) userdata;

    response_data->data = realloc(response_data->data, response_data->size + realsize + 1);
    if (response_data->data == NULL)
    {
        // TODO: Use Chiaki logger!
        fprintf(stderr, "curl_write_cb: realloc failed\n");
        return 0;
    }
    memcpy(&(response_data->data[response_data->size]), ptr, realsize);
    response_data->size += realsize;
    response_data->data[response_data->size] = 0;

    return realsize;
}

static void hex_to_bytes(const char* hex_str, uint8_t* bytes, size_t max_len) {
    size_t len = strlen(hex_str);
    if (len > max_len * 2) {
        len = max_len * 2;
    }
    for (size_t i = 0; i < len; i += 2) {
        sscanf(hex_str + i, "%2hhx", &bytes[i / 2]);
    }
}

static void bytes_to_hex(const uint8_t* bytes, size_t len, char* hex_str, size_t max_len) {
    if (len > max_len * 2) {
        len = max_len * 2;
    }
    for (size_t i = 0; i < len; i++) {
        snprintf(hex_str + i * 2, 3, "%02x", bytes[i]);
    }
}

/**
 * Generate a random UUIDv4
 *
 * @param[out] out Buffer to write the UUID to, must be at least 37 bytes long
 */
static void random_uuidv4(char* out)
{
    srand((unsigned int)time(NULL));
    const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            out[i] = '-';
        } else if (i == 14) {
            out[i] = '4';
        } else if (i == 19) {
            out[i] = hex[(rand() % 4) + 8];
        } else {
            out[i] = hex[rand() % 16];
        }
    }
    out[36] = '\0';
}

static void* websocket_thread_func(void *user) {
    Session* session = (Session*) user;

    char ws_url[128] = {0};
    snprintf(ws_url, sizeof(ws_url), "wss://%s/np/pushNotification", session->ws_fqdn);

    CURL* curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, session->oauth_header);
    headers = curl_slist_append(headers, "Sec-WebSocket-Protocol: np-pushpacket");
    headers = curl_slist_append(headers, "User-Agent: WebSocket++/0.8.2");
    headers = curl_slist_append(headers, "X-PSN-APP-TYPE: REMOTE_PLAY");
    headers = curl_slist_append(headers, "X-PSN-APP-VER: RemotePlay/1.0");
    headers = curl_slist_append(headers, "X-PSN-KEEP-ALIVE-STATUS-TYPE: 3");
    headers = curl_slist_append(headers, "X-PSN-OS-VER: Windows/10.0");
    headers = curl_slist_append(headers, "X-PSN-PROTOCOL-VERSION: 2.1");
    headers = curl_slist_append(headers, "X-PSN-RECONNECTION: false");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_SHARE, session->curl_share);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, ws_url);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    if (res != CURLE_OK)
    {
        if (res == CURLE_HTTP_RETURNED_ERROR)
        {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            CHIAKI_LOGE(session->log, "websocket_thread_func: Connecting to push notification WebSocket %s failed with HTTP code %ld", ws_url, http_code);
        } else {
            CHIAKI_LOGE(session->log, "websocket_thread_func: Connecting to push notification WebSocket %s failed with CURL error %d", ws_url, res);
        }
        goto cleanup;
    }
    CHIAKI_LOGV(session->log, "websocket_thread_func: Connected to push notification WebSocket %s", ws_url);
    ChiakiErrorCode err = chiaki_mutex_lock(&session->state_mutex);
    assert(err == CHIAKI_ERR_SUCCESS);
    session->state |= SESSION_STATE_WS_OPEN;
    log_session_state(session);
    err = chiaki_cond_signal(&session->state_cond);
    assert(err == CHIAKI_ERR_SUCCESS);
    err = chiaki_mutex_unlock(&session->state_mutex);
    assert(err == CHIAKI_ERR_SUCCESS);

    // Get the socket curl uses for the websocket so we can select() on it
    curl_socket_t sockfd;
    res = curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);
    if(res != CURLE_OK) {
        CHIAKI_LOGE(session->log, "websocket_thread_func: Getting active socket failed with CURL error %d", res);
        goto cleanup;
    }
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sockfd, &fds);

    // Need to send a ping every 5secs
    struct timeval timeout = { .tv_sec = WEBSOCKET_PING_INTERVAL_SEC, .tv_usec = 0 };
    struct timespec ts;
    uint64_t now;
    uint64_t last_ping_sent;

    json_tokener *tok = json_tokener_new();

    const struct curl_ws_frame *meta;
    char *buf = malloc(WEBSOCKET_MAX_FRAME_SIZE);
    size_t rlen;
    size_t wlen;
    char pingbuf[0];
    bool expecting_pong = false;
    while (!session->ws_thread_should_stop)
    {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = ts.tv_sec * SECOND_NS + ts.tv_nsec;

        if (expecting_pong && now - last_ping_sent > 5L * SECOND_NS)
        {
            CHIAKI_LOGE(session->log, "websocket_thread_func: Did not receive PONG in time.");
            goto cleanup_json;
        }

        if (now - last_ping_sent > 5L * SECOND_NS)
        {
            res = curl_ws_send(curl, buf, 0, &wlen, 0, CURLWS_PING);
            if (res != CURLE_OK)
            {
                CHIAKI_LOGE(session->log, "websocket_thread_func: Sending WebSocket PING failed with CURL error %d.", res);
                goto cleanup_json;
            }
            CHIAKI_LOGD(session->log, "websocket_thread_func: PING.");
            last_ping_sent = now;
            expecting_pong = true;
        }

        memset(buf, 0, WEBSOCKET_MAX_FRAME_SIZE);
        res = curl_ws_recv(curl, buf, WEBSOCKET_MAX_FRAME_SIZE, &rlen, &meta);
        if (res != CURLE_OK)
        {
            if (res == CURLE_AGAIN)
            {
                if (select(sockfd + 1, &fds, NULL, NULL, &timeout) == -1)
                {
                    CHIAKI_LOGE(session->log, "websocket_thread_func: Select failed.");
                    goto cleanup_json;
                }
                else
                    continue;
            } else
            {
                CHIAKI_LOGE(session->log, "websocket_thread_func: Receiving WebSocket frame failed with CURL error %d", res);
                goto cleanup_json;
            }
        }

        CHIAKI_LOGV(session->log, "websocket_thread_func: Received WebSocket frame of length %zu with flags %d", rlen, meta->flags);
        if (meta->flags & CURLWS_PONG)
        {
            CHIAKI_LOGD(session->log, "websocket_thread_func: Received PONG.");
            expecting_pong = false;
        }
        if (meta->flags & CURLWS_PING)
        {
            CHIAKI_LOGD(session->log, "websocket_thread_func: Received PING.");
            res = curl_ws_send(curl, buf, rlen, &wlen, 0, CURLWS_PONG);
            if (res != CURLE_OK)
            {
                CHIAKI_LOGE(session->log, "websocket_thread_func: Sending WebSocket PONG failed with CURL error %d", res);
                goto cleanup_json;
            }
            CHIAKI_LOGD(session->log, "websocket_thread_func: Sent PONG.");
        }
        if (meta->flags & CURLWS_CLOSE)
        {
            CHIAKI_LOGE(session->log, "websocket_thread_func: WebSocket closed");
            goto cleanup_json;
        }
        if (meta->flags & CURLWS_TEXT || meta->flags & CURLWS_BINARY)
        {
            CHIAKI_LOGV(session->log, "websocket_thread_func: Received WebSocket frame with %d bytes of payload.", rlen);
            json_object *json = json_tokener_parse_ex(tok, buf, rlen);
            if (json == NULL)
            {
                CHIAKI_LOGE(session->log, "websocket_thread_func: Parsing JSON from payload failed");
                CHIAKI_LOGD(session->log, "websocket_thread_func: Payload was:\n%s", buf);
                continue;
            }
            CHIAKI_LOGV(session->log, json_object_to_json_string_ext(json, JSON_C_TO_STRING_PRETTY));

            Notification *notif = malloc(sizeof(Notification));
            notif->previous = session->ws_notification_queue;
            notif->json = json;
            notif->json_buf = malloc(rlen);
            memcpy(notif->json_buf, buf, rlen);
            notif->json_buf_size = rlen;
            notif->type = parse_notification_type(session->log, json);

            // Automatically ACK OFFER session messages if we're not currently explicitly
            // waiting on offers
            chiaki_mutex_lock(&session->state_mutex);
            bool should_ack_offers =
                // We're not expecting any offers after receiving one for the control port and before it's established, afterwards we expect
                // one for the data port, so we don't auto-ACK in between
                (session->state & SESSION_STATE_CTRL_OFFER_RECEIVED
                 && !(session->state & SESSION_STATE_CTRL_ESTABLISHED))
                 // At this point all offers were received and we don't care for new ones anymore
                || session->state & SESSION_STATE_DATA_OFFER_RECEIVED;
            chiaki_mutex_unlock(&session->state_mutex);
            if (should_ack_offers && notif->type == NOTIFICATION_TYPE_SESSION_MESSAGE_CREATED)
            {
                SessionMessage *msg;
                json_object *payload = session_message_get_payload(session->log, notif->json);
                err = session_message_parse(session->log, payload, &msg);
                json_object_put(payload);
                if (err != CHIAKI_ERR_SUCCESS)
                {
                    CHIAKI_LOGE(session->log, "websocket_thread_func: Failed to parse session message for ACKing.");
                    continue;
                }
                if (msg->action == SESSION_MESSAGE_ACTION_OFFER)
                {
                    SessionMessage ack_msg = {
                        .action = SESSION_MESSAGE_ACTION_RESULT,
                        .req_id = msg->req_id,
                        .error = 0,
                        .conn_request = calloc(1, sizeof(ConnectionRequest)),
                    };
                    ack_msg.conn_request->num_candidates = 0;
                    http_send_session_message(session, &ack_msg);
                    free(ack_msg.conn_request);
                }
                session_message_free(msg);
            }


            ChiakiErrorCode mutex_err = chiaki_mutex_lock(&session->notif_mutex);
            assert(mutex_err == CHIAKI_ERR_SUCCESS);
            session->ws_notification_queue = notif;
            chiaki_cond_signal(&session->notif_cond);
            chiaki_mutex_unlock(&session->notif_mutex);
        }
    }

cleanup_json:
    json_tokener_free(tok);
cleanup:
    curl_easy_cleanup(curl);

    return NULL;
}

static NotificationType parse_notification_type(
    ChiakiLog *log, json_object* json
) {
    json_object* datatype;
    if (!json_object_object_get_ex(json, "dataType", &datatype))
    {
        CHIAKI_LOGE(log, "parse_notification_type: JSON does not contain \"datatype\" field\n");
        return NOTIFICATION_TYPE_UNKNOWN;
    } else if (!json_object_is_type(datatype, json_type_string))
    {
        CHIAKI_LOGE(log, "parse_notification_type: JSON \"datatype\" field is not a string\n");
        return NOTIFICATION_TYPE_UNKNOWN;
    }
    const char* datatype_str = json_object_get_string(datatype);

    if (strcmp(datatype_str, "psn:sessionManager:sys:remotePlaySession:created") == 0)
    {
        return NOTIFICATION_TYPE_SESSION_CREATED;
    } else if (strcmp(datatype_str, "psn:sessionManager:sys:rps:members:created") == 0)
    {
        return NOTIFICATION_TYPE_MEMBER_CREATED;
    } else if (strcmp(datatype_str, "psn:sessionManager:sys:rps:customData1:updated") == 0)
    {
        return NOTIFICATION_TYPE_CUSTOM_DATA1_UPDATED;
    } else if (strcmp(datatype_str, "psn:sessionManager:sys:rps:sessionMessage:created") == 0)
    {
        return NOTIFICATION_TYPE_SESSION_MESSAGE_CREATED;
    } else
    {
        CHIAKI_LOGW(log, "parse_notification_type: Unknown notification type \"%s\"", datatype_str);
        CHIAKI_LOGD(log, "parse_notification_type: JSON was:\n%s", json_object_to_json_string_ext(json, JSON_C_TO_STRING_PRETTY));
        return NOTIFICATION_TYPE_UNKNOWN;
    }
}


/** Sends an OFFER connection request session message to the console via PSN.
 *
 * @param session The Session instance.
 * @return CHIAKI_ERR_SUCCESS on success, or an error code on failure.
 */
static ChiakiErrorCode send_offer(Session *session, int req_id, Candidate *local_console_candidate)
{
    // Create listening socket that the console can reach us on
    session->client_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (session->client_sock < 0)
    {
        CHIAKI_LOGE(session->log, "send_offer: Creating socket failed");
        return CHIAKI_ERR_UNKNOWN;
    }
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = INADDR_ANY;
    client_addr.sin_port = 0;
    socklen_t client_addr_len = sizeof(client_addr);
    bind(session->client_sock, (struct sockaddr*)&client_addr, client_addr_len);
    if (getsockname(session->client_sock, (struct sockaddr*)&client_addr, &client_addr_len) < 0)
    {
        CHIAKI_LOGE(session->log, "send_offer: Getting socket port failed");
        close(session->client_sock);
        return CHIAKI_ERR_UNKNOWN;
    }

    ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
    uint16_t local_port = ntohs(client_addr.sin_port);
    SessionMessage msg = {
        .action = SESSION_MESSAGE_ACTION_OFFER,
        .req_id = 0,
        .error = 0,
        .conn_request = malloc(sizeof(ConnectionRequest)),
    };
    msg.conn_request->sid = session->sid_local;
    msg.conn_request->nat_type = 2;
    memset(msg.conn_request->skey, 0, sizeof(msg.conn_request->skey));
    memcpy(msg.conn_request->local_hashed_id, session->hashed_id_local, sizeof(session->hashed_id_local));
    msg.conn_request->num_candidates = 2;
    msg.conn_request->candidates = calloc(2, sizeof(Candidate));

    Candidate *candidate_local = &msg.conn_request->candidates[0];
    candidate_local->type = CANDIDATE_TYPE_LOCAL;
    memcpy(candidate_local->addr_mapped, "0.0.0.0", 8);
    candidate_local->port = local_port;
    candidate_local->port_mapped = 0;

    bool have_addr = false;
    Candidate *candidate_remote = &msg.conn_request->candidates[1];
    candidate_remote->type = CANDIDATE_TYPE_STATIC;
    struct addrinfo *local_addr;
    UPNPGatewayInfo upnp_gw;
    upnp_gw.data = calloc(1, sizeof(struct IGDdatas));
    upnp_gw.urls = calloc(1, sizeof(struct UPNPUrls));
    err = upnp_get_gateway_info(session->log, &upnp_gw);
    if (err == CHIAKI_ERR_SUCCESS) {
        memcpy(candidate_local->addr, upnp_gw.lan_ip, sizeof(upnp_gw.lan_ip));
        have_addr = get_client_addr_remote_upnp(&upnp_gw, candidate_remote->addr);
        upnp_add_udp_port_mapping(&upnp_gw, local_port, local_port, "Chiaki UDP holepunch");
    }
    else {
        get_client_addr_local(session, candidate_local, candidate_local->addr, sizeof(candidate_local->addr));
    }
    getaddrinfo(candidate_local->addr, NULL, NULL, &local_addr);
    get_mac_addr(local_addr, msg.conn_request->default_route_mac_addr);
    if (!have_addr) {
        have_addr = get_client_addr_remote_stun(session->log, candidate_remote->addr);
    }
    if (!have_addr) {
        CHIAKI_LOGE(session->log, "send_offer: Could not get remote address");
        err = CHIAKI_ERR_UNKNOWN;
        close(session->client_sock);
        goto cleanup;
    }
    memcpy(candidate_remote->addr_mapped, "0.0.0.0", 8);
    candidate_remote->port = local_port;
    candidate_remote->port_mapped = 0;
    print_session_request(session->log, msg.conn_request);
    err = http_send_session_message(session, &msg);
    if (err != CHIAKI_ERR_SUCCESS)
    {
        CHIAKI_LOGE(session->log, "send_offer: Sending session message failed");
        close(session->client_sock);
    }

cleanup:
    free(msg.conn_request->candidates);
    free(msg.conn_request);
    free(upnp_gw.data);
    free(upnp_gw.urls);
    freeaddrinfo(local_addr);

    return err;
}


static ChiakiErrorCode send_accept(Session *session, int req_id, Candidate *selected_candidate)
{
    SessionMessage msg = {
        .action = SESSION_MESSAGE_ACTION_ACCEPT,
        .req_id = req_id,
        .error = 0,
        .conn_request = calloc(1, sizeof(ConnectionRequest)),
    };
    msg.conn_request->sid = session->sid_local;
    msg.conn_request->peer_sid = session->sid_console;
    msg.conn_request->nat_type = selected_candidate->type == CANDIDATE_TYPE_LOCAL ? 0 : 2;
    msg.conn_request->num_candidates = 1;
    msg.conn_request->candidates = calloc(1, sizeof(Candidate));
    memcpy(&msg.conn_request->candidates[0], selected_candidate, sizeof(Candidate));

    ChiakiErrorCode err = http_send_session_message(session, &msg);
    free(msg.conn_request->candidates);
    free(msg.conn_request);
    return err;
}

/**
 * Creates a session on the PSN server.
 *
 * @param session The Session instance.
 * @return CHIAKI_ERR_SUCCESS on success, or an error code on failure.
*/
static ChiakiErrorCode http_create_session(Session *session)
{
    size_t session_create_json_len = sizeof(session_create_json_fmt) + strlen(session->pushctx_id);
    char* session_create_json = malloc(session_create_json_len);
    snprintf(session_create_json, session_create_json_len, session_create_json_fmt, session->pushctx_id);
    CHIAKI_LOGD(session->log, "http_create_session: Sending JSON:\n%s", session_create_json);

    HttpResponseData response_data = {
        .data = malloc(0),
        .size = 0,
    };

    CURL* curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, session->oauth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

    curl_easy_setopt(curl, CURLOPT_SHARE, session->curl_share);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, session_create_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, session_create_json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response_data);

    ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    if (res != CURLE_OK)
    {
        if (res == CURLE_HTTP_RETURNED_ERROR)
        {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            CHIAKI_LOGE(session->log, "http_create_session: Creating session failed with HTTP code %ld", http_code);
            err = CHIAKI_ERR_HTTP_NONOK;
        } else {
            CHIAKI_LOGE(session->log, "http_create_session: Creating session failed with CURL error %d", res);
            err = CHIAKI_ERR_NETWORK;
        }
        goto cleanup;
    }

    json_tokener *tok = json_tokener_new();
    CHIAKI_LOGD(session->log, "http_create_session: Received JSON:\n%s", response_data.data);
    json_object *json = json_tokener_parse_ex(tok, response_data.data, response_data.size);
    if (json == NULL)
    {
        CHIAKI_LOGE(session->log, "http_create_session: Parsing JSON failed");
        err = CHIAKI_ERR_UNKNOWN;
        goto cleanup;
    }

    json_object* session_id_json;
    json_pointer_get(json, "/remotePlaySessions/0/sessionId", &session_id_json);
    json_object* account_id_json;
    json_pointer_get(json, "/remotePlaySessions/0/members/0/accountId", &account_id_json);

    bool schema_bad =
        session_id_json == NULL
        || !json_object_is_type(session_id_json, json_type_string)
        || account_id_json == NULL
        || (!json_object_is_type(account_id_json, json_type_string)
            && !json_object_is_type(account_id_json, json_type_int));
    if (schema_bad)
    {
        CHIAKI_LOGE(session->log, "http_create_session: Unexpected JSON schema, could not parse sessionId and accountId.");
        CHIAKI_LOGD(session->log, json_object_to_json_string_ext(json, JSON_C_TO_STRING_PRETTY));
        err = CHIAKI_ERR_UNKNOWN;
        goto cleanup_json;
    }
    const char* session_id = json_object_get_string(session_id_json);
    if (strlen(session_id) != 36)
    {
        CHIAKI_LOGE(session->log, "http_create_session: Unexpected JSON schema, sessionId is not a UUIDv4, was '%s'.", session_id);
        CHIAKI_LOGD(session->log, json_object_to_json_string_ext(json, JSON_C_TO_STRING_PRETTY));
        err = CHIAKI_ERR_UNKNOWN;
        goto cleanup_json;
    }
    memcpy(session->session_id, json_object_get_string(session_id_json), 36);

    session->account_id = json_object_get_int64(account_id_json);

cleanup_json:
    json_object_put(json);
    json_tokener_free(tok);
cleanup:
    free(session_create_json);
    free(response_data.data);
    curl_easy_cleanup(curl);

    return err;
}

/**
 * Starts a session on the PSN server. Session must have been created before.
 *
 * @param session The Session instance.
 * @return CHIAKI_ERR_SUCCESS on success, or an error code on failure.
*/
static ChiakiErrorCode http_start_session(Session *session)
{
    char payload_buf[sizeof(session_start_payload_fmt) * 3] = {0};
    char envelope_buf[sizeof(session_start_envelope_fmt) * 2 + sizeof(payload_buf)] = {0};

    char data1_base64[25] = {0};
    char data2_base64[25] = {0};
    chiaki_base64_encode(session->data1, sizeof(session->data1), data1_base64, sizeof(data1_base64));
    chiaki_base64_encode(session->data2, sizeof(session->data1), data2_base64, sizeof(data2_base64));
    size_t payload_len = snprintf(payload_buf, sizeof(payload_buf), session_start_payload_fmt,
        session->account_id,
        session->session_id,
        data1_base64,
        data2_base64);

    char device_uid_str[sizeof(session->console_uid) * 2 + 1];
    bytes_to_hex(session->console_uid, sizeof(session->console_uid), device_uid_str, sizeof(device_uid_str));

    size_t envelope_len = snprintf(
        envelope_buf, sizeof(envelope_buf), session_start_envelope_fmt,
        device_uid_str,
        payload_buf,
        session->console_type == CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS4 ? "PS4" : "PS5");

    HttpResponseData response_data = {
        .data = malloc(0),
        .size = 0,
    };

    CURL *curl = curl_easy_init();

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, session->oauth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    headers = curl_slist_append(headers, "User-Agent: RpNetHttpUtilImpl");

    curl_easy_setopt(curl, CURLOPT_SHARE, session->curl_share);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, session_command_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, envelope_buf);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response_data);

    CHIAKI_LOGV(session->log, "http_start_session: Sending JSON:\n%s", envelope_buf);

    ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    CHIAKI_LOGV(session->log, "http_start_session: Received JSON:\n%.*s", response_data.size, response_data.data);
    if (res != CURLE_OK)
    {
        if (res == CURLE_HTTP_RETURNED_ERROR)
        {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            CHIAKI_LOGE(session->log, "http_start_session: Starting session failed with HTTP code %ld.", http_code);
            CHIAKI_LOGD(session->log, "Request Body: %s.", envelope_buf);
            CHIAKI_LOGD(session->log, "Response Body: %.*s.", response_data.size, response_data.data);
            err = CHIAKI_ERR_HTTP_NONOK;
        } else {
            CHIAKI_LOGE(session->log, "http_start_session: Starting session failed with CURL error %d.", res);
            err = CHIAKI_ERR_NETWORK;
        }
        goto cleanup;
    }

    chiaki_mutex_lock(&session->state_mutex);
    session->state |= SESSION_STATE_DATA_SENT;
    log_session_state(session);
    chiaki_mutex_unlock(&session->state_mutex);

cleanup:
    curl_easy_cleanup(curl);
    free(response_data.data);

    return err;
}

/**
 * Sends a session message to the PSN server.
 *
 * @param session The Session instance.
 * @param message The session message to send, will be addressed to the console defined in the session
 * @return CHIAKI_ERR_SUCCESS on success, or an error code on failure.
*/
static ChiakiErrorCode http_send_session_message(Session *session, SessionMessage *message)
{
    ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;

    HttpResponseData response_data = {
        .data = malloc(0),
        .size = 0,
    };

    char url[128] = {0};
    size_t url_len = snprintf(url, sizeof(url), session_message_url_fmt, session->session_id);

    char console_uid_str[sizeof(session->console_uid) * 2 + 1] = {0};
    bytes_to_hex(session->console_uid, sizeof(session->console_uid), console_uid_str, sizeof(console_uid_str));

    char *payload_str = NULL;
    size_t payload_len = 0;
    session_message_serialize(session, message, &payload_str, &payload_len);

    char msg_buf[sizeof(session_message_envelope_fmt) * 2 + payload_len];
    size_t msg_len = snprintf(
        msg_buf, sizeof(msg_buf), session_message_envelope_fmt,
        payload_str, session->account_id, console_uid_str,
        session->console_type == CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS4 ? "PS4" : "PS5"
    );
    CHIAKI_LOGI("Message to send: %s", msg_buf);
    CURL *curl = curl_easy_init();

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, session->oauth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

    curl_easy_setopt(curl, CURLOPT_SHARE, session->curl_share);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, msg_buf);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response_data);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    if (res != CURLE_OK)
    {
        if (res == CURLE_HTTP_RETURNED_ERROR)
        {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            CHIAKI_LOGE(session->log, "http_send_session_message: Sending session message failed with HTTP code %ld.", http_code);
            CHIAKI_LOGD(session->log, "Request Body: %s.", msg_buf);
            CHIAKI_LOGD(session->log, "Response Body: %s.", response_data.data);
            err = CHIAKI_ERR_HTTP_NONOK;
        } else {
            CHIAKI_LOGE(session->log, "http_send_session_message: Sending session message failed with CURL error %d.", res);
            err = CHIAKI_ERR_NETWORK;
        }
        goto cleanup;
    }

cleanup:
    return err;
}

/**
 * Retrieves the IP address on the local network of the client.
 *
 * @param session The Session instance.
 * @param local_console_candidate A local console candidate.
 * @return The IP address of the client on the local network, or NULL if the IP address could not be retrieved. Needs to be freed by the caller.
*/
static ChiakiErrorCode get_client_addr_local(Session *session, Candidate *local_console_candidate, char *out, size_t out_len)
{
    ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
    struct ifaddrs *local_addrs, *current_addr;
    void *in_addr;

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *res;
    int status;

    if(getifaddrs(&local_addrs) != 0)
    {
        CHIAKI_LOGE(session->log, "Couldn't get local address");
        return CHIAKI_ERR_NETWORK;
    }
    for (current_addr = local_addrs; current_addr != NULL; current_addr = current_addr->ifa_next)
    {
        if (current_addr->ifa_addr == NULL)
            continue;
        if (!(current_addr->ifa_flags & IFF_UP))
            continue;
        if (0 != (current_addr->ifa_flags & IFF_LOOPBACK))
            continue;
        switch (current_addr->ifa_addr->sa_family)
        {
            case AF_INET:
                struct sockaddr_in *res4 = (struct sockaddr_in *)current_addr->ifa_addr;
                in_addr = &res4->sin_addr;
                break;

            case AF_INET6:
                struct sockaddr_in6 *res6 = (struct sockaddr_in6 *)current_addr->ifa_addr;
                in_addr = &res6->sin6_addr;
                break;

            default:
                continue;
        }
        if (!inet_ntop(current_addr->ifa_addr->sa_family, in_addr, local_console_candidate->addr, sizeof(local_console_candidate->addr)))
        {
            CHIAKI_LOGE(session->log, "%s: inet_ntop failed with error: %s\n", current_addr->ifa_name, strerror(errno));
            continue;
        }
        break;
    }

cleanup:
    freeifaddrs(local_addrs);
    return err;
}

/**
 * Retrieves the gateway information using UPnP.
 *
 * @param log The ChiakiLog instance for logging.
 * @param[out] info Pointer to the UPNPGatewayInfo structure to store the retrieved information.
 * @return true if the gateway information was successfully retrieved, false otherwise.
 */
static ChiakiErrorCode upnp_get_gateway_info(ChiakiLog *log, UPNPGatewayInfo *info)
{
    ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
    int success = 0;
    struct UPNPDev *devlist = upnpDiscover(
        2000 /** ms, delay*/, NULL, NULL, 0, 0, 2, &success);
    if (devlist == NULL || err != UPNPDISCOVER_SUCCESS) {
        CHIAKI_LOGI(log, "Failed to UPnP-capable devices on network: err=%d", err);
        return CHIAKI_ERR_NETWORK;
    }

    success = UPNP_GetValidIGD(devlist, info->urls, info->data, info->lan_ip, sizeof(info->lan_ip));
    if (success != 1) {
        CHIAKI_LOGI(log, "Failed to discover internet gateway via UPnP: err=%d", err);
        err = CHIAKI_ERR_NETWORK;
        goto cleanup;
    }

cleanup:
    freeUPNPDevlist(devlist);
    return err;
}

/**
 * Retrieves the external IP address of the gateway.
 *
 * @param gw_info The UPNPGatewayInfo structure containing the gateway information.
 * @param[out] out Pointer to the buffer where the external IP address will be stored, needs to be at least 16 bytes long.
 */
static bool get_client_addr_remote_upnp(UPNPGatewayInfo *gw_info, char* out)
{
    // TODO: Error checking, logging?
    return UPNP_GetExternalIPAddress(
        gw_info->urls->controlURL, gw_info->data->first.servicetype, out) == UPNPCOMMAND_SUCCESS;
}

/**
 * Adds a port mapping to the gateway.
 *
 * @param gw_info The UPNPGatewayInfo structure containing the gateway information.
 * @param port_internal The internal port to map.
 * @param port_external The external port to map.
 * @param ip_local The local IP address to map to.
 * @return true if the port mapping was successfully added, false otherwise.
*/
static bool upnp_add_udp_port_mapping(UPNPGatewayInfo *gw_info, uint16_t port_internal, uint16_t port_external, char* ip_local)
{
    char port_internal_str[6];
    snprintf(port_internal_str, sizeof(port_internal_str), "%d", port_internal);
    char port_external_str[6];
    snprintf(port_external_str, sizeof(port_external_str), "%d", port_external);

    int res = UPNP_AddPortMapping(
        gw_info->urls->controlURL, gw_info->data->first.servicetype,
        port_external_str, port_internal_str, ip_local, "Chiaki", "UDP", NULL, "0");
    return res == UPNPCOMMAND_SUCCESS;
}

/**
 * Deletes a port mapping from the gateway.
 *
 * @param gw_info The UPNPGatewayInfo structure containing the gateway information.
 * @param port_external The external port to delete the mapping for.
 * @return true if the port mapping was successfully deleted, false otherwise.
*/
static bool upnp_delete_udp_port_mapping(UPNPGatewayInfo *gw_info, uint16_t port_external)
{
    char port_external_str[6];
    snprintf(port_external_str, sizeof(port_external_str), "%d", port_external);

    int res = UPNP_DeletePortMapping(
        gw_info->urls->controlURL, gw_info->data->first.servicetype,
        port_external_str, "UDP", NULL);
    return res == UPNPCOMMAND_SUCCESS;
}

/**
 * Retrieves the external IP address (i.e. internet-visible) of the client using STUN.
 *
 * @param log The ChiakiLog instance for logging.
 * @return The external IP address of the client, or NULL if the IP address could not be retrieved. Needs to be freed by the caller.
*/
static bool get_client_addr_remote_stun(ChiakiLog *log, char *out)
{
    uint16_t port = 0;
    if (!stun_get_external_address(log, out, &port))
    {
        CHIAKI_LOGE(log, "get_client_addr_remote_stun: Failed to get external address");
        return false;
    }
    return true;
}

/**
 * Retrieves the MAC address associated with the given IP address.
 *
 * @param ip_addr The IP address for which to retrieve the MAC address.
 * @param mac_addr Pointer to the buffer where the MAC address will be stored, needs to be at least 6 bytes long.
 * @return True if the MAC address was successfully retrieved, false otherwise.
 */
static bool get_mac_addr(struct addrinfo *ip_addr, uint8_t *mac_addr)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return false;
    }

    struct arpreq req;
    memset(&req, 0, sizeof(req));
    req.arp_pa.sa_family = AF_INET;
    memcpy(&req.arp_pa, ip_addr->ai_addr, sizeof(struct sockaddr_in));

    int err = ioctl(sock, SIOCGARP, &req);
    close(sock);
    if (err < 0) {
        return false;
    }

    memcpy(mac_addr, req.arp_ha.sa_data, 6);
    return true;
}

static ChiakiErrorCode check_candidates(
    Session *session, Candidate *candidates, size_t num_candidates, chiaki_socket_t *out,
    uint16_t *out_port, Candidate **out_candidate)
{
    ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;

    // Set up request buffer
    uint32_t request_id = chiaki_random_32();
    uint8_t request_buf[88] = {0};
    *(uint32_t*)&request_buf[0x00] = ntohl(MSG_TYPE_REQ);
    memcpy(&request_buf[0x04], session->hashed_id_local, sizeof(session->hashed_id_local));
    memcpy(&request_buf[0x24], session->hashed_id_console, sizeof(session->hashed_id_console));
    *(uint16_t*)&request_buf[0x44] = ntohs(session->sid_local);
    *(uint16_t*)&request_buf[0x46] = ntohs(session->sid_console);
    *(uint32_t*)&request_buf[0x48] = ntohl(request_id);
    // TODO: Encode IP address (?? which one ??)
    // TODO: Encode local port


    // Set up sockets for candidates and send a request over each of them
    chiaki_socket_t sockets[num_candidates];
    for (int i=0; i < num_candidates; i++)
        sockets[i] = -1;
    fd_set fds;
    FD_ZERO(&fds);
    for (int i=0; i < num_candidates; i++)
    {
        chiaki_socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
        {
            CHIAKI_LOGE(session->log, "check_candidate: Creating socket failed");
            return CHIAKI_ERR_UNKNOWN;
        }
        sockets[i] = sock;
        FD_SET(sock, &fds);

        Candidate *candidate = &candidates[i];

        // Get addrinfo from string address
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        struct addrinfo *addr;
        if (getaddrinfo(candidate->addr, NULL, &hints, &addr) != 0)
        {
            CHIAKI_LOGE(session->log, "check_candidate: getaddrinfo failed for %s:%d", candidate->addr, candidate->port);
            err = CHIAKI_ERR_UNKNOWN;
            goto cleanup_sockets;
        }

        if (bind(sock, addr->ai_addr, addr->ai_addrlen) < 0)
        {
            CHIAKI_LOGE(session->log, "check_candidate: Binding socket failed for %s:%d", candidate->addr, candidate->port);
            err = CHIAKI_ERR_NETWORK;
            freeaddrinfo(addr);
            goto cleanup_sockets;
        }

        if (send(sock, request_buf, sizeof(request_buf), 0) < 0)
        {
            CHIAKI_LOGE(session->log, "check_candidate: Sending request failed for %s:%d", candidate->addr, candidate->port);
            err = CHIAKI_ERR_NETWORK;
            freeaddrinfo(addr);
            goto cleanup_sockets;
        }
        freeaddrinfo(addr);
    }

    // Wait for responses
    uint8_t response_buf[88];
    struct sockaddr response_addr;
    socklen_t response_addr_len = sizeof(response_addr);

    chiaki_socket_t maxfd = -1;
    for (int i=0; i < num_candidates; i++)
    {
        if (sockets[i] > maxfd)
            maxfd = sockets[i];
    }
    maxfd = maxfd + 1;

    struct timeval tv;
    tv.tv_sec = SESSION_START_TIMEOUT_SEC;
    tv.tv_usec = 0;

    chiaki_socket_t selected_sock = -1;
    Candidate *selected_candidate = NULL;

    while (true)
    {
        int ret = select(maxfd, &fds, NULL, NULL, &tv);
        if (ret < 0)
        {
            CHIAKI_LOGE(session->log, "check_candidate: Select failed");
            err = CHIAKI_ERR_NETWORK;
            goto cleanup_sockets;
        } else if (ret == 0)
        {
            // No responsive candidate within timeout, terminate with error
            if (selected_sock < 0)
            {
                CHIAKI_LOGE(session->log, "check_candidate: Select timed out");
                err = CHIAKI_ERR_TIMEOUT;
                goto cleanup_sockets;
            }
            // Otherwise, we have a responsive candidate, break out of loop
            break;
        }

        Candidate *candidate = NULL;
        chiaki_socket_t candidate_sock = -1;
        for (int i=0; i < num_candidates; i++)
        {
            if (sockets[i] >= 0 && FD_ISSET(sockets[i], &fds))
            {
                candidate_sock = sockets[i];
                candidate = &candidates[i];
                break;
            }
        }
        if (candidate_sock < 0)
        {
            CHIAKI_LOGE(session->log, "check_candidate: Select returned unexpected socket");
            err = CHIAKI_ERR_UNKNOWN;
            goto cleanup_sockets;
        }

        CHIAKI_LOGD(session->log, "check_candidate: Receiving data from %s:%d", candidate->addr, candidate->port);
        ssize_t response_len = recvfrom(candidate_sock, response_buf, sizeof(response_buf), 0, &response_addr, &response_addr_len);
        if (response_len < 0)
        {
            CHIAKI_LOGE(session->log, "check_candidate: Receiving response from %s:%d failed", candidate->addr, candidate->port);
            err = CHIAKI_ERR_NETWORK;
            goto cleanup_sockets;
        }
        if (response_len != sizeof(response_buf))
        {
            CHIAKI_LOGE(session->log, "check_candidate: Received response of unexpected size %ld from %s:%d", response_len, candidate->addr, candidate->port);
            err = CHIAKI_ERR_NETWORK;
            goto cleanup_sockets;
        }
        uint32_t msg_type = ntohl(*(uint32_t*)&response_buf[0]);
        if (msg_type != MSG_TYPE_RESP)
        {
            CHIAKI_LOGE(session->log, "check_candidate: Received response of unexpected type %d from %s:%d", msg_type, candidate->addr, candidate->port);
            err = CHIAKI_ERR_UNKNOWN;
            goto cleanup_sockets;
        }
        // TODO: More validation of localHashedIds, sids and the weird data at 0x4C?
        uint32_t resp_id = ntohl(*(uint32_t*)&response_buf[0x48]);
        if (resp_id != request_id)
        {
            CHIAKI_LOGE(session->log, "check_candidate: Received response with unexpected request ID %d from %s:%d", resp_id, candidate->addr, candidate->port);
            err = CHIAKI_ERR_UNKNOWN;
            goto cleanup_sockets;
        }

        selected_sock = candidate_sock;
        selected_candidate = candidate;

        // If we have a responsive local candidate, no need to wait for other candidates
        if (candidate->type == CANDIDATE_TYPE_LOCAL)
            break;
        // Otherwise wait a bit longer until a local candidate becomes responsive
        memset(response_buf, 0, sizeof(response_buf));
        memset(&response_addr, 0, sizeof(response_addr));
        response_addr_len = sizeof(response_addr);
    }

    *out = selected_sock;

    struct sockaddr_in addr_in;
    socklen_t addr_in_len = sizeof(addr_in);
    if (getsockname(selected_sock, (struct sockaddr*)&addr_in, &addr_in_len) < 0)
    {
        CHIAKI_LOGE(session->log, "check_candidate: getsockname failed");
        err = CHIAKI_ERR_NETWORK;
        goto cleanup_sockets;
    }
    *out_port = ntohs(addr_in.sin_port);
    *out_candidate = selected_candidate;

cleanup_sockets:
    for (int i=0; i < num_candidates; i++)
    {
        if (sockets[i] != *out && sockets[i] >= 0)
            close(sockets[i]);
    }

    return err;
}


static void log_session_state(Session *session)
{
    char state_str[1024];
    state_str[0] = '[';
    state_str[1] = '\0';
    if (session->state & SESSION_STATE_INIT)
        strcat(state_str, " ✅INIT");
    if (session->state & SESSION_STATE_WS_OPEN)
        strcat(state_str, " ✅WS_OPEN");
    if (session->state & SESSION_STATE_CREATED)
        strcat(state_str, " ✅CREATED");
    if (session->state & SESSION_STATE_STARTED)
        strcat(state_str, " ✅STARTED");
    if (session->state & SESSION_STATE_CLIENT_JOINED)
        strcat(state_str, " ✅CLIENT_JOINED");
    if (session->state & SESSION_STATE_DATA_SENT)
        strcat(state_str, " ✅DATA_SENT");
    if (session->state & SESSION_STATE_CONSOLE_JOINED)
        strcat(state_str, " ✅CONSOLE_JOINED");
    if (session->state & SESSION_STATE_CUSTOMDATA1_RECEIVED)
        strcat(state_str, " ✅CUSTOMDATA1_RECEIVED");
    if (session->state &SESSION_STATE_CTRL_OFFER_RECEIVED)
        strcat(state_str, " ✅CTRL_OFFER_RECEIVED");
    if (session->state & SESSION_STATE_CTRL_OFFER_SENT)
        strcat(state_str, " ✅CTRL_OFFER_SENT");
    if (session->state & SESSION_STATE_CTRL_CONSOLE_ACCEPTED)
        strcat(state_str, " ✅CTRL_CONSOLE_ACCEPTED");
    if (session->state & SESSION_STATE_CTRL_CLIENT_ACCEPTED)
        strcat(state_str, " ✅CTRL_CLIENT_ACCEPTED");
    if (session->state & SESSION_STATE_CTRL_ESTABLISHED)
        strcat(state_str, " ✅CTRL_ESTABLISHED");
    if (session->state & SESSION_STATE_DATA_OFFER_RECEIVED)
        strcat(state_str, " ✅DATA_OFFER_RECEIVED");
    if (session->state & SESSION_STATE_DATA_OFFER_SENT)
        strcat(state_str, " ✅DATA_OFFER_SENT");
    if (session->state & SESSION_STATE_DATA_CONSOLE_ACCEPTED)
        strcat(state_str, " ✅DATA_CONSOLE_ACCEPTED");
    if (session->state & SESSION_STATE_DATA_CLIENT_ACCEPTED)
        strcat(state_str, " ✅DATA_CLIENT_ACCEPTED");
    if (session->state & SESSION_STATE_DATA_ESTABLISHED)
        strcat(state_str, " ✅DATA_ESTABLISHED");
    strcat(state_str, " ]");
    CHIAKI_LOGD(session->log, "Session state: %d = %s", session->state, state_str);
}

static ChiakiErrorCode decode_customdata1(const char *customdata1, uint8_t *out, size_t out_len)
{

    uint8_t customdata1_round1[24];
    size_t decoded_len = sizeof(customdata1_round1);
    ChiakiErrorCode err = chiaki_base64_decode(customdata1, strlen(customdata1), customdata1_round1, &decoded_len);
    if (err != CHIAKI_ERR_SUCCESS)
        return err;
    err = chiaki_base64_decode((const char*)customdata1_round1, decoded_len, out, &decoded_len);
    if (err != CHIAKI_ERR_SUCCESS)
        return err;
    if (decoded_len != out_len)
        return CHIAKI_ERR_UNKNOWN;
    return CHIAKI_ERR_SUCCESS;
}

static json_object* session_message_get_payload(ChiakiLog *log, json_object *session_message)
{
    json_object *payload_json;
    if (json_pointer_get(session_message, "/body/data/sessionMessage/payload", &payload_json) < 0)
    {
        CHIAKI_LOGE(log, "session_message_get_payload: Failed to get payload");
        CHIAKI_LOGD(log, json_object_to_json_string_ext(session_message, JSON_C_TO_STRING_PRETTY));
        return NULL;
    }

    if (!json_object_is_type(payload_json, json_type_string))
    {
        CHIAKI_LOGE(log, "session_message_get_payload: Payload is not a string");
        CHIAKI_LOGD(log, json_object_to_json_string_ext(session_message, JSON_C_TO_STRING_PRETTY));
        return NULL;
    }

    const char* payload_str = json_object_get_string(payload_json);

    char* body = strstr(payload_str, "body=");
    if (body == NULL) {
        CHIAKI_LOGE(log, "session_message_get_payload: Failed to find body of payload");
        CHIAKI_LOGD(log, payload_str);
        return NULL;
    }


    char *json = body + 5;
    // The JSON for a session message is kind of peculiar, as it's sometimes invalid JSON.
    // This happens when there is no value for the `localPeerAddr` field. Instead of the value
    // being `undefined` or the empty object, the field simply doesn't have a value, i.e. the
    // colon is immediately followed by a comma. This obviously breaks our parser, so we fix
    // the JSON if the field value is missing.
    json_object *message_json;
    char *peeraddr_key = "\"localPeerAddr\":";
    char *peeraddr_start = strstr(json, peeraddr_key);

    // No localPeerAddr, nothing to fix
    if (peeraddr_start == NULL)
        return json_tokener_parse(json);

    char *peeraddr_end = peeraddr_start + strlen(peeraddr_key);
    if (*peeraddr_end == '{')
    {
        // Valid JSON, we can parse without modifications
        message_json = json_tokener_parse(json);
    }
    else
    {
        // Insert empty object as key for localPeerAddr key
        size_t prefix_len = peeraddr_end - json;
        size_t suffix_len = strlen(peeraddr_end);
        char fixed_json[strlen(json) + 3]; // {} + \0
        memset(fixed_json, 0, sizeof(fixed_json));
        strncpy(fixed_json, json, prefix_len);
        fixed_json[prefix_len + 1] = '{';
        fixed_json[prefix_len + 2] = '}';
        strncpy(fixed_json + prefix_len + 2, peeraddr_end, suffix_len);

        message_json = json_tokener_parse(fixed_json);
    }

    return message_json;
}

static SessionMessageAction get_session_message_action(json_object *payload)
{
    json_object *action_json;
    json_object_object_get_ex(payload, "action", &action_json);
    assert(json_object_is_type(action_json, json_type_string));
    const char *action = json_object_get_string(action_json);
    if (strcmp(action, "OFFER") == 0)
        return SESSION_MESSAGE_ACTION_OFFER;
    else if (strcmp(action, "ACCEPT") == 0)
        return SESSION_MESSAGE_ACTION_ACCEPT;
    else if (strcmp(action, "TERMINATE") == 0)
        return SESSION_MESSAGE_ACTION_TERMINATE;
    else if (strcmp(action, "RESULT") == 0)
        return SESSION_MESSAGE_ACTION_RESULT;
    else
        return SESSION_MESSAGE_ACTION_UNKNOWN;
}

static ChiakiErrorCode wait_for_notification(
    Session *session, Notification** out,
    uint16_t types, uint64_t timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t waiting_since = ts.tv_sec * SECOND_NS + ts.tv_nsec;
    uint64_t now = waiting_since;

    ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
    Notification *last_known = NULL;
    chiaki_mutex_lock(&session->notif_mutex);
    while (true) {
        while (session->ws_notification_queue == last_known)
        {
            CHIAKI_LOGD(session->log, "wait_for_notification: Waiting for notifications...");
            err = chiaki_cond_timedwait(&session->notif_cond, &session->notif_mutex, timeout_ms);
            if (err == CHIAKI_ERR_TIMEOUT)
            {
                clock_gettime(CLOCK_MONOTONIC, &ts);
                now = ts.tv_sec * SECOND_NS + ts.tv_nsec;
                if ((now - waiting_since) > (timeout_ms * MILLISECONDS_NS))
                {
                    CHIAKI_LOGE(session->log, "wait_for_notification: Timed out waiting for session messages");
                    err = CHIAKI_ERR_TIMEOUT;
                    goto cleanup;
                }
            }
            assert(err == CHIAKI_ERR_SUCCESS);
        }

        Notification *notif = session->ws_notification_queue;
        while (notif != NULL && notif != last_known)
        {
            if (notif->type & types)
            {
                CHIAKI_LOGD(session->log, "wait_for_notification: Found notification of type %d", notif->type);
                *out = notif;
                err = CHIAKI_ERR_SUCCESS;
                goto cleanup;
            }
            last_known = notif;
            notif = notif->previous;
        }
    }

cleanup:
    chiaki_mutex_unlock(&session->notif_mutex);
    return err;
}

static ChiakiErrorCode clear_notification(
    Session *session, Notification *notification)
{
    bool found = false;
    Notification *notif = session->ws_notification_queue;
    Notification *last = NULL;
    chiaki_mutex_lock(&session->notif_mutex);
    while (notif != NULL)
    {
        if (notif == notification)
        {
            if (last != NULL)
                last->previous = notif->previous;
            else
                session->ws_notification_queue = notif->previous;

            notif->previous = NULL;
            notification_queue_free(notif);
            found = true;
            break;
        }
        last = notif;
        notif = notif->previous;
    }
    chiaki_mutex_unlock(&session->notif_mutex);
    if (found)
        return CHIAKI_ERR_SUCCESS;
    else
        return CHIAKI_ERR_UNKNOWN;
}

static ChiakiErrorCode wait_for_session_message(
    Session *session, SessionMessage** out,
    uint16_t types, uint64_t timeout_ms)
{
    ChiakiErrorCode err;
    bool finished = false;
    Notification *notif = NULL;
    SessionMessage *msg = NULL;
    uint32_t notif_query = NOTIFICATION_TYPE_SESSION_MESSAGE_CREATED;
    while (!finished)
    {
        err = wait_for_notification(session, &notif, notif_query, SESSION_START_TIMEOUT_SEC * 1000);
        if (err == CHIAKI_ERR_TIMEOUT)
        {
            CHIAKI_LOGE(session->log, "Timed out waiting for session message notification.");
            return err;
        }
        else if (err != CHIAKI_ERR_SUCCESS)
        {
            CHIAKI_LOGE(session->log, "Failed to wait for session message notification.");
            return err;
        }
        json_object *payload = session_message_get_payload(session->log, notif->json);
        err = session_message_parse(session->log, payload, &msg);
        json_object_put(payload);
        if (err != CHIAKI_ERR_SUCCESS)
        {
            CHIAKI_LOGE(session->log, "Failed to parse session message");
            return err;
        }
        if (!(msg->action & types))
        {
            CHIAKI_LOGE(session->log, "Ignoring session message with action %d", msg->action);
            continue;
        }
        finished = true;
    }
    msg->notification = notif;
    *out = msg;
    return CHIAKI_ERR_SUCCESS;
}

static ChiakiErrorCode wait_for_session_message_ack(
    Session *session, int req_id, uint64_t timeout_ms)
{
    ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
    SessionMessage *msg = NULL;
    uint32_t msg_query = SESSION_MESSAGE_ACTION_RESULT;
    bool finished = false;
    while (!finished)
    {
        err = wait_for_session_message(session, &msg, msg_query, SESSION_START_TIMEOUT_SEC * 1000);
        if (err == CHIAKI_ERR_TIMEOUT)
        {
            CHIAKI_LOGE(session->log, "wait_for_session_message_ack: Timed out waiting for connection offer ACK notification.");
            return err;
        }
        else if (err != CHIAKI_ERR_SUCCESS)
        {
            CHIAKI_LOGE(session->log, "wait_for_session_message_ack: Failed to wait for session connection offer ACK notification.");
            return err;
        }
        if (msg->req_id != req_id)
        {
            CHIAKI_LOGE(session->log, "wait_for_session_message_ack: Got ACK for unexpected request ID %d", msg->req_id);
            continue;
        }
        finished = true;
        chiaki_mutex_lock(&session->notif_mutex);
        session_message_free(msg);
        chiaki_mutex_unlock(&session->notif_mutex);
    }

    return err;
}

static ChiakiErrorCode session_message_parse(
    ChiakiLog *log, json_object *message_json, SessionMessage **out)
{
    ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
    SessionMessage *msg = calloc(1, sizeof(SessionMessage));

    json_object *action_json;
    json_object_object_get_ex(message_json, "action", &action_json);
    if (action_json == NULL || !json_object_is_type(action_json, json_type_string))
        goto invalid_schema;
    const char *action = json_object_get_string(action_json);
    if (strcmp(action, "OFFER") == 0)
        msg->action = SESSION_MESSAGE_ACTION_OFFER;
    else if (strcmp(action, "ACCEPT") == 0)
        msg->action = SESSION_MESSAGE_ACTION_ACCEPT;
    else if (strcmp(action, "TERMINATE") == 0)
        msg->action = SESSION_MESSAGE_ACTION_TERMINATE;
    else if (strcmp(action, "RESULT") == 0)
        msg->action = SESSION_MESSAGE_ACTION_RESULT;
    else
        msg->action = SESSION_MESSAGE_ACTION_UNKNOWN;

    json_object *reqid_json = NULL;
    json_object_object_get_ex(message_json, "reqId", &reqid_json);
    if (reqid_json == NULL || !json_object_is_type(reqid_json, json_type_int))
        goto invalid_schema;
    msg->req_id = json_object_get_int(reqid_json);

    json_object *error_json = NULL;
    json_object_object_get_ex(message_json, "error", &error_json);
    if (error_json == NULL || !json_object_is_type(error_json, json_type_int))
        goto invalid_schema;
    msg->error = json_object_get_int(error_json);

    json_object *conn_request_json = NULL;
    json_object_object_get_ex(message_json, "connRequest", &conn_request_json);
    if (conn_request_json == NULL || !json_object_is_type(conn_request_json, json_type_object))
        goto invalid_schema;
    if (json_object_object_length(conn_request_json) > 0)
    {
        msg->conn_request = calloc(1, sizeof(ConnectionRequest));

        json_object *obj;
        json_object_object_get_ex(conn_request_json, "sid", &obj);
        if (obj == NULL || !json_object_is_type(obj, json_type_int))
            goto invalid_schema;
        msg->conn_request->sid = json_object_get_int(obj);

        json_object_object_get_ex(conn_request_json, "peerSid", &obj);
        if (obj == NULL || !json_object_is_type(obj, json_type_int))
            goto invalid_schema;
        msg->conn_request->peer_sid = json_object_get_int(obj);

        json_object_object_get_ex(conn_request_json, "skey", &obj);
        if (obj == NULL || !json_object_is_type(obj, json_type_string))
            goto invalid_schema;
        const char *skey_str = json_object_get_string(obj);
        size_t skey_len = strlen(skey_str);
        err = chiaki_base64_decode(skey_str, strlen(skey_str), msg->conn_request->skey, &skey_len);
        if (err != CHIAKI_ERR_SUCCESS)
        {
            CHIAKI_LOGE(log, "session_message_parse: Failed to decode skey: '%s'", skey_str);
            goto cleanup;
        }

        json_object_object_get_ex(conn_request_json, "natType", &obj);
        if (obj == NULL || !json_object_is_type(obj, json_type_int))
            goto invalid_schema;
        msg->conn_request->nat_type = json_object_get_int(obj);

        json_object_object_get_ex(conn_request_json, "defaultRouteMacAddr", &obj);
        if (obj == NULL || !json_object_is_type(obj, json_type_string))
            goto invalid_schema;
        const char *mac_str = json_object_get_string(obj);
        if (strlen(mac_str) == 17)
        {
            // Parse MAC address
            char *end;
            for (int i = 0; i < 6; i++)
            {
                msg->conn_request->default_route_mac_addr[i] = strtoul(mac_str, &end, 16);
                if (i < 5)
                {
                    if (*end != ':')
                        goto invalid_schema;
                    mac_str = end + 1;
                }
            }
        }

        json_object_object_get_ex(conn_request_json, "localHashedId", &obj);
        if (obj == NULL || !json_object_is_type(obj, json_type_string))
            goto invalid_schema;
        const char *local_hashed_id_str = json_object_get_string(obj);
        size_t local_hashed_id_len = sizeof(msg->conn_request->local_hashed_id);
        err = chiaki_base64_decode(
            local_hashed_id_str, strlen(local_hashed_id_str),
            msg->conn_request->local_hashed_id, &local_hashed_id_len);
        if (err != CHIAKI_ERR_SUCCESS)
        {
            CHIAKI_LOGE(log, "session_message_parse: Failed to decode localHashedId: '%s'", local_hashed_id_str);
            goto cleanup;
        }

        json_object_object_get_ex(conn_request_json, "candidate", &obj);
        if (obj == NULL || !json_object_is_type(obj, json_type_array))
            goto invalid_schema;
        size_t num_candidates = json_object_array_length(obj);
        msg->conn_request->num_candidates = num_candidates;
        msg->conn_request->candidates = calloc(num_candidates, sizeof(Candidate));
        for (size_t i = 0; i < num_candidates; i++)
        {
            Candidate candidate = msg->conn_request->candidates[i];
            json_object *candidate_json = json_object_array_get_idx(obj, i);

            json_object *jobj = NULL;
            json_object_object_get_ex(candidate_json, "type", &jobj);
            if (jobj == NULL || !json_object_is_type(jobj, json_type_string))
                goto invalid_schema;
            const char *type_str = json_object_get_string(jobj);
            if (strcmp(type_str, "LOCAL") == 0)
                candidate.type = CANDIDATE_TYPE_LOCAL;
            else if (strcmp(type_str, "STATIC") == 0)
                candidate.type = CANDIDATE_TYPE_STATIC;
            else
                goto invalid_schema;

            json_object_object_get_ex(candidate_json, "addr", &jobj);
            if (jobj == NULL || !json_object_is_type(jobj, json_type_string))
                goto invalid_schema;
            const char *addr_str = json_object_get_string(jobj);
            strncpy(candidate.addr, addr_str, sizeof(candidate.addr));

            json_object_object_get_ex(candidate_json, "mappedAddr", &jobj);
            if (jobj == NULL || !json_object_is_type(jobj, json_type_string))
                goto invalid_schema;
            const char *mapped_addr_str = json_object_get_string(jobj);
            strncpy(candidate.addr_mapped, mapped_addr_str, sizeof(candidate.addr_mapped));

            json_object_object_get_ex(candidate_json, "port", &jobj);
            if (jobj == NULL || !json_object_is_type(jobj, json_type_int))
                goto invalid_schema;
            candidate.port = json_object_get_int(jobj);

            json_object_object_get_ex(candidate_json, "mappedPort", &jobj);
            if (jobj == NULL || !json_object_is_type(jobj, json_type_int))
                goto invalid_schema;
            candidate.port_mapped = json_object_get_int(jobj);
        }
    }
    *out = msg;
    goto cleanup;

invalid_schema:
    CHIAKI_LOGE(log, "session_message_parse: Unexpected JSON schema for session message.");
    CHIAKI_LOGD(log, json_object_to_json_string_ext(message_json, JSON_C_TO_STRING_PRETTY));
    err = CHIAKI_ERR_UNKNOWN;

cleanup:
    if (msg != NULL && err != CHIAKI_ERR_SUCCESS)
    {
        session_message_free(msg);
        msg = NULL;
    }
    return err;
}

static ChiakiErrorCode session_message_serialize(
    Session *session, SessionMessage *message, char **out, size_t *out_len)
{
    ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;

    // Since the official remote play app doesn't send valid JSON half the time,
    // we can't use a proper JSON library to serialize the message. Instead, we
    // use snprintf to build the JSON string manually.
    char localpeeraddr_json[128] = {0};
    size_t localpeeraddr_len = snprintf(
        localpeeraddr_json, sizeof(localpeeraddr_json), session_localpeeraddr_fmt,
        session->account_id, "REMOTE_PLAY");

    char *candidates_json = calloc(1, 256 * message->conn_request->num_candidates);
    size_t candidates_len = 0;
    for (int i=0; i < message->conn_request->num_candidates; i++)
    {
        Candidate candidate = message->conn_request->candidates[i];
        char candidate_str[256] = {0};
        size_t candidate_len = snprintf(
            candidate_str, sizeof(candidate_str), session_conrequest_candidate_fmt,
            candidate.type == CANDIDATE_TYPE_LOCAL ? "LOCAL" : "STATIC",
            candidate.addr, candidate.addr_mapped, candidate.port, candidate.port_mapped);
        snprintf(candidates_json + candidates_len, candidate_len, "%s", candidate_str);
        *(candidates_json + candidates_len + candidate_len) = ',';
        candidates_len += candidate_len + 1;
    }

    char localhashedid_str[29] = {0};
    chiaki_base64_encode(
        message->conn_request->local_hashed_id, sizeof(message->conn_request->local_hashed_id),
        localhashedid_str, sizeof(localhashedid_str));
    char skey_str[25] = {0};
    chiaki_base64_encode (
        message->conn_request->skey, sizeof(message->conn_request->skey), skey_str, sizeof(skey_str));
    char *connreq_json = calloc(
        1, sizeof(session_connrequest_fmt) * 2 + localpeeraddr_len + candidates_len);
    size_t connreq_len = snprintf(
        connreq_json, sizeof(connreq_json), session_connrequest_fmt,
        message->conn_request->sid, message->conn_request->peer_sid,
        skey_str, message->conn_request->nat_type,
        candidates_json, message->conn_request->default_route_mac_addr,
        localpeeraddr_json, localhashedid_str);

    char* action_str;
    switch (message->action)
    {
        case SESSION_MESSAGE_ACTION_OFFER:
            action_str = "OFFER";
            break;
        case SESSION_MESSAGE_ACTION_ACCEPT:
            action_str = "ACCEPT";
            break;
        case SESSION_MESSAGE_ACTION_TERMINATE:
            action_str = "TERMINATE";
            break;
        case SESSION_MESSAGE_ACTION_RESULT:
            action_str = "RESULT";
            break;
        default:
            action_str = "UNKNOWN";
            break;
    }
    char *serialized_msg = calloc(1, sizeof(session_message_envelope_fmt) * 2 + connreq_len);
    size_t msg_len = snprintf(
        serialized_msg, sizeof(serialized_msg), session_message_fmt,
        action_str, message->req_id, message->error, connreq_json);

    *out = serialized_msg;
    *out_len = msg_len;

    free(candidates_json);
    free(connreq_json);

    return err;
}

static ChiakiErrorCode session_message_free(SessionMessage *message)
{
    ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
    if (message->conn_request != NULL)
    {
        if (message->conn_request->candidates != NULL)
            free(message->conn_request->candidates);
        free(message->conn_request);
    }
    if (message->notification != NULL)
    {
        message->notification->previous = NULL;
        notification_queue_free(message->notification);
    }
    free(message);
    return err;
}

static void print_session_request(ChiakiLog *log, ConnectionRequest *req)
{
    CHIAKI_LOGI(log, "-----------------CONNECTION REQUEST---------------------");
    CHIAKI_LOGI(log, "sid: %lu", req->sid);
    CHIAKI_LOGI(log, "peer_sid: %lu", req->peer_sid);
    char skey[25];
    ChiakiErrorCode err = chiaki_base64_encode(req->skey, sizeof(req->skey), skey, sizeof(skey));
    if(err != CHIAKI_ERR_SUCCESS)
    {
        char hex[32];
        bytes_to_hex(req->skey, sizeof(req->skey), hex, sizeof(hex));
        CHIAKI_LOGE(log, "Error with base64 encoding of string %s", hex);
    }
    CHIAKI_LOGI(log, "skey: %s", skey);
    CHIAKI_LOGI(log, "nat type %u", req->nat_type);
    CHIAKI_LOGI(log, "default_route_mac_addr %s", req->default_route_mac_addr);
    char local_hashed_id[29];
    chiaki_base64_encode(req->local_hashed_id, sizeof(req->local_hashed_id), local_hashed_id, sizeof(local_hashed_id));
    CHIAKI_LOGI(log, "local hashed id %s", local_hashed_id);
    for(size_t i = 0; i < req->num_candidates; i++)
    {
        Candidate *candidate = &req->candidates[i];
        print_candidate(log, candidate);
    }
}

static void print_candidate(ChiakiLog *log, Candidate *candidate)
{
    if(candidate->type == CANDIDATE_TYPE_LOCAL)
        CHIAKI_LOGI(log, "--------------LOCAL CANDIDATE---------------------");
    else if(candidate->type == CANDIDATE_TYPE_STATIC)
        CHIAKI_LOGI(log, "--------------REMOTE CANDIDATE--------------------");
    else
        CHIAKI_LOGI(log, "--------------CANDIDATE TYPE UNKNOWN--------------");
    CHIAKI_LOGI(log, "Address: %s", candidate->addr);
    CHIAKI_LOGI(log, "Mapped Address: %s", candidate->addr_mapped);
    CHIAKI_LOGI(log, "Port %u", candidate->port);
    CHIAKI_LOGI(log, "Mapped Port: %u", candidate->port_mapped);
}