/**
 * @file demo_master_link.c
 * @brief NDJSON TCP control server for the master board (port 8788).
 *
 * Protocol (one JSON object per '\n'-terminated line):
 *   master -> slave: {"type":"load","content_id":"..."}
 *                    {"type":"start"} {"type":"pause"} {"type":"resume"}
 *                    {"type":"stop"}  {"type":"state"} {"type":"status"}
 *   slave -> master: {"type":"ack","state":"accepted"}
 *                    {"type":"state","state":"ready"|"playing"|...,
 *                     "position_ms":N,"duration_ms":M}
 *                    {"type":"error","detail":"..."}
 * Unknown types are ignored.
 */

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tal_network.h"
#include "cJSON.h"

#include "demo_cloud.h"
#include "demo_master_link.h"

#define MLINK_LINE_MAX      512
#define MLINK_RX_BUF        1024
#define MLINK_SELECT_MS     200U
#define MLINK_REPORT_MS     1000U
#define MLINK_LOAD_WAIT_MS  30000U
/* Extra grace after PAUSED so the video worker can prefill the frame queue. */
#define MLINK_PREFETCH_MS   400U

static playback_engine_t *sg_engine;
static THREAD_HANDLE sg_thread;
static MUTEX_HANDLE sg_status_mutex;
static demo_master_link_status_t sg_status;

static int sg_client_fd = -1;
static char sg_session_id[PLAYBACK_SESSION_ID_MAX_LEN + 1U];
static uint32_t sg_sequence_id;
static bool sg_session_active;

static void mlink_set_phase(const char *phase)
{
    tal_mutex_lock(sg_status_mutex);
    snprintf(sg_status.phase, sizeof(sg_status.phase), "%s", phase);
    tal_mutex_unlock(sg_status_mutex);
}

static void mlink_set_client(bool connected, const char *ip)
{
    tal_mutex_lock(sg_status_mutex);
    sg_status.client_connected = connected;
    snprintf(sg_status.peer_ip, sizeof(sg_status.peer_ip), "%s",
             (ip != NULL) ? ip : "");
    tal_mutex_unlock(sg_status_mutex);
}

void demo_master_link_get_status(demo_master_link_status_t *status)
{
    if (status == NULL) {
        return;
    }
    if (sg_status_mutex == NULL) {
        memset(status, 0, sizeof(*status));
        return;
    }
    tal_mutex_lock(sg_status_mutex);
    *status = sg_status;
    tal_mutex_unlock(sg_status_mutex);
}

static bool mlink_send_line(const char *line)
{
    size_t len = strlen(line);
    size_t sent = 0;

    if (sg_client_fd < 0) {
        return false;
    }
    while (sent < len) {
        int n = tal_net_send(sg_client_fd, (const uint8_t *)line + sent,
                             (uint32_t)(len - sent));
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static void mlink_send_error(const char *detail)
{
    char buf[192];

    snprintf(buf, sizeof(buf), "{\"type\":\"error\",\"detail\":\"%s\"}\n",
             detail);
    (void)mlink_send_line(buf);
}

static const char *mlink_state_word(playback_state_t state)
{
    switch (state) {
    case PLAYBACK_STATE_PLAYING:
        return "playing";
    case PLAYBACK_STATE_PAUSED:
        return "paused";
    case PLAYBACK_STATE_COMPLETED:
        return "completed";
    case PLAYBACK_STATE_ERROR:
        return "error";
    case PLAYBACK_STATE_LOADING:
    case PLAYBACK_STATE_BUFFERING:
    case PLAYBACK_STATE_WAITING_SPEAKER:
    case PLAYBACK_STATE_SEEKING:
        return "buffering";
    default:
        return "idle";
    }
}

static void mlink_send_state_report(void)
{
    playback_snapshot_t snap;
    char buf[192];

    if (playback_engine_get_snapshot(sg_engine, &snap) != PLAYBACK_ENGINE_OK) {
        return;
    }
    snprintf(buf, sizeof(buf),
             "{\"type\":\"state\",\"state\":\"%s\",\"position_ms\":%u,"
             "\"duration_ms\":%u}\n",
             mlink_state_word(snap.state), (unsigned)snap.position_ms,
             (unsigned)(snap.has_session ? snap.session.duration_ms : 0U));
    (void)mlink_send_line(buf);
}

static bool mlink_submit_control(playback_command_kind_t kind)
{
    playback_command_t command;

    if (!sg_session_active) {
        return false;
    }
    memset(&command, 0, sizeof(command));
    command.schema_version = PLAYBACK_PROTOCOL_SCHEMA_VERSION;
    command.kind = kind;
    snprintf(command.session_id, sizeof(command.session_id), "%s",
             sg_session_id);
    command.sequence_id = ++sg_sequence_id;
    return playback_engine_submit(sg_engine, &command) == PLAYBACK_ENGINE_OK;
}

static void mlink_handle_load(const char *content_id)
{
    /* Too large for this thread's stack next to mbedTLS frames; this worker
     * is the only writer. */
    static playback_session_t session;
    static playback_command_t command;
    demo_cloud_result_t cloud_result;
    SYS_TIME_T deadline;
    SYS_TIME_T t0;

    (void)mlink_send_line("{\"type\":\"ack\",\"state\":\"accepted\"}\n");
    mlink_set_phase("loading");
    PR_NOTICE("mlink: load %s", content_id);
    t0 = tal_system_get_millisecond();

    memset(&session, 0, sizeof(session));
    cloud_result = demo_cloud_fetch_session(content_id, &session);
    if (cloud_result != DEMO_CLOUD_OK) {
        PR_ERR("mlink: fetch_session failed: %d", cloud_result);
        mlink_send_error("fetch_session failed");
        mlink_set_phase("load failed");
        return;
    }
    session.autoplay = false;
    PR_NOTICE("mlink: fetch_session took %u ms",
              (unsigned)(tal_system_get_millisecond() - t0));

    memset(&command, 0, sizeof(command));
    command.schema_version = PLAYBACK_PROTOCOL_SCHEMA_VERSION;
    command.kind = PLAYBACK_COMMAND_LOAD_SESSION;
    snprintf(command.session_id, sizeof(command.session_id), "%s",
             session.session_id);
    command.sequence_id = ++sg_sequence_id;
    command.payload.session = session;

    if (playback_engine_submit(sg_engine, &command) != PLAYBACK_ENGINE_OK) {
        mlink_send_error("engine submit failed");
        mlink_set_phase("load failed");
        return;
    }
    snprintf(sg_session_id, sizeof(sg_session_id), "%s", session.session_id);
    sg_session_active = true;

    /* Wait for prepare to finish (state PAUSED), then let the prefetch
     * worker fill the video queue before declaring ready. */
    t0 = tal_system_get_millisecond();
    deadline = t0 + MLINK_LOAD_WAIT_MS;
    for (;;) {
        playback_snapshot_t snap;

        if (tal_system_get_millisecond() >= deadline) {
            mlink_send_error("load timeout");
            mlink_set_phase("load timeout");
            return;
        }
        if (playback_engine_get_snapshot(sg_engine, &snap) ==
            PLAYBACK_ENGINE_OK) {
            if (snap.state == PLAYBACK_STATE_ERROR) {
                mlink_send_error("load failed");
                mlink_set_phase("load failed");
                return;
            }
            if (snap.state == PLAYBACK_STATE_PAUSED) {
                break;
            }
        }
        tal_system_sleep(50);
    }
    PR_NOTICE("mlink: prepare took %u ms",
              (unsigned)(tal_system_get_millisecond() - t0));
    tal_system_sleep(MLINK_PREFETCH_MS);

    (void)mlink_send_line("{\"type\":\"state\",\"state\":\"ready\"}\n");
    mlink_set_phase("ready");
    PR_NOTICE("mlink: ready (%s)", sg_session_id);
}

static void mlink_handle_line(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    cJSON *type;
    const char *t;

    if (root == NULL) {
        return;
    }
    type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return;
    }
    t = type->valuestring;

    if (strcmp(t, "load") == 0) {
        cJSON *cid = cJSON_GetObjectItem(root, "content_id");

        if (cJSON_IsString(cid) && cid->valuestring != NULL &&
            cid->valuestring[0] != '\0') {
            char content_id[PLAYBACK_CONTENT_ID_MAX_LEN + 1U];

            snprintf(content_id, sizeof(content_id), "%s", cid->valuestring);
            cJSON_Delete(root);
            mlink_handle_load(content_id);
            return;
        }
        mlink_send_error("load requires content_id");
    } else if (strcmp(t, "start") == 0 || strcmp(t, "resume") == 0) {
        if (mlink_submit_control(PLAYBACK_COMMAND_PLAY)) {
            (void)mlink_send_line("{\"type\":\"ack\",\"state\":\"accepted\"}\n");
            mlink_set_phase("playing");
        } else {
            mlink_send_error("no session");
        }
    } else if (strcmp(t, "pause") == 0) {
        if (mlink_submit_control(PLAYBACK_COMMAND_PAUSE)) {
            (void)mlink_send_line("{\"type\":\"ack\",\"state\":\"accepted\"}\n");
            mlink_set_phase("paused");
        } else {
            mlink_send_error("no session");
        }
    } else if (strcmp(t, "stop") == 0) {
        if (mlink_submit_control(PLAYBACK_COMMAND_STOP)) {
            (void)mlink_send_line("{\"type\":\"ack\",\"state\":\"accepted\"}\n");
            mlink_set_phase("stopped");
        } else {
            mlink_send_error("no session");
        }
    } else if (strcmp(t, "state") == 0 || strcmp(t, "status") == 0) {
        cJSON *state_field = cJSON_GetObjectItem(root, "state");

        /* A bare {"type":"state"} is a query; one carrying a state value is
         * a peer report and is ignored. */
        if (state_field == NULL) {
            mlink_send_state_report();
        }
    }
    /* Unknown types: ignore for forward compatibility. */
    cJSON_Delete(root);
}

static void mlink_serve_client(void)
{
    char rx[MLINK_RX_BUF];
    char line[MLINK_LINE_MAX];
    size_t line_len = 0;
    SYS_TIME_T last_report = 0;

    for (;;) {
        TUYA_FD_SET_T read_fds;
        int ready;
        int n;

        tal_net_fd_zero(&read_fds);
        tal_net_fd_set(sg_client_fd, &read_fds);
        ready = tal_net_select(sg_client_fd + 1, &read_fds, NULL, NULL,
                               MLINK_SELECT_MS);

        if (ready > 0 && tal_net_fd_isset(sg_client_fd, &read_fds)) {
            n = tal_net_recv(sg_client_fd, (uint8_t *)rx, sizeof(rx));
            if (n <= 0) {
                return; /* peer closed / error */
            }
            for (int i = 0; i < n; i++) {
                char c = rx[i];

                if (c == '\n') {
                    if (line_len > 0) {
                        line[line_len] = '\0';
                        mlink_handle_line(line);
                        line_len = 0;
                    }
                } else if (c != '\r') {
                    if (line_len < sizeof(line) - 1) {
                        line[line_len++] = c;
                    } else {
                        line_len = 0; /* oversized: drop */
                    }
                }
            }
        }

        if (sg_session_active &&
            (tal_system_get_millisecond() - last_report) >= MLINK_REPORT_MS) {
            last_report = tal_system_get_millisecond();
            mlink_send_state_report();
        }
    }
}

static void mlink_worker(void *arg)
{
    int listen_fd;

    (void)arg;

    listen_fd = tal_net_socket_create(PROTOCOL_TCP);
    if (listen_fd < 0) {
        PR_ERR("mlink: socket create failed");
        return;
    }
    (void)tal_net_set_reuse(listen_fd);
    if (tal_net_bind(listen_fd, TY_IPADDR_ANY, DEMO_MASTER_LINK_PORT) != 0 ||
        tal_net_listen(listen_fd, 2) != 0) {
        PR_ERR("mlink: bind/listen :%u failed", DEMO_MASTER_LINK_PORT);
        tal_net_close(listen_fd);
        return;
    }
    PR_NOTICE("mlink: listening on :%u", DEMO_MASTER_LINK_PORT);
    mlink_set_phase("waiting");

    for (;;) {
        TUYA_FD_SET_T read_fds;
        TUYA_IP_ADDR_T client_address = 0U;
        uint16_t client_port = 0U;
        int client_fd;
        int ready;

        tal_net_fd_zero(&read_fds);
        tal_net_fd_set(listen_fd, &read_fds);
        ready = tal_net_select(listen_fd + 1, &read_fds, NULL, NULL, 500U);
        if (ready <= 0 || !tal_net_fd_isset(listen_fd, &read_fds)) {
            continue;
        }

        client_fd = tal_net_accept(listen_fd, &client_address, &client_port);
        if (client_fd < 0) {
            continue;
        }
        sg_client_fd = client_fd;
        mlink_set_client(true, tal_net_addr2str(client_address));
        mlink_set_phase("connected");
        PR_NOTICE("mlink: master connected: %s:%u",
                  tal_net_addr2str(client_address), (unsigned)client_port);

        mlink_serve_client();

        tal_net_close(client_fd);
        sg_client_fd = -1;
        mlink_set_client(false, NULL);
        mlink_set_phase("waiting");
        PR_NOTICE("mlink: master disconnected");
    }
}

OPERATE_RET demo_master_link_start(playback_engine_t *engine)
{
    THREAD_CFG_T cfg = {
        /* fetch_session (HTTP/mbedTLS) runs on this thread. */
        .stackDepth = 1024 * 20,
        .priority = THREAD_PRIO_2,
        .thrdname = "mlink",
    };
    OPERATE_RET rt;

    if (engine == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (sg_engine != NULL) {
        return OPRT_OK;
    }

    rt = tal_mutex_create_init(&sg_status_mutex);
    if (rt != OPRT_OK) {
        return rt;
    }
    snprintf(sg_status.phase, sizeof(sg_status.phase), "starting");

    sg_engine = engine;
    rt = tal_thread_create_and_start(&sg_thread, NULL, NULL, mlink_worker,
                                     NULL, &cfg);
    if (rt != OPRT_OK) {
        sg_engine = NULL;
        PR_ERR("mlink: worker create failed: %d", rt);
    }
    return rt;
}
