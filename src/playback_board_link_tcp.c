/**
 * @file playback_board_link_tcp.c
 * @brief Raw TCP/NDJSON Board Link adapter on port 8787.
 */

#include "playback_board_link_tcp.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "playback_board_link_json.h"
#include "playback_board_link_wire.h"
#include "tal_api.h"
#include "tal_network.h"

#define PLAYBACK_TCP_REPORT_QUEUE_DEPTH (12U)
#define PLAYBACK_TCP_SERVER_BACKLOG (2)
#define PLAYBACK_TCP_IO_TIMEOUT_MS (1500)
#define PLAYBACK_TCP_SELECT_TIMEOUT_MS (200U)
#define PLAYBACK_TCP_STATUS_INTERVAL_MS (500U)
#define PLAYBACK_TCP_SERVER_STACK_SIZE (10U * 1024U)
#define PLAYBACK_TCP_SENDER_STACK_SIZE (10U * 1024U)

typedef struct
{
    playback_report_t report;
} playback_board_link_tcp_report_event_t;

typedef struct
{
    playback_board_link_tcp_config_t config;
    playback_board_link_tcp_status_t status;
    playback_hello_t local_hello;
    MUTEX_HANDLE status_mutex;
    QUEUE_HANDLE report_queue;
    SEM_HANDLE server_stopped;
    SEM_HANDLE sender_stopped;
    THREAD_HANDLE server_thread;
    THREAD_HANDLE sender_thread;
    volatile bool closing;
    volatile int listen_fd;
    uint8_t command_json[PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES + 2U];
    char report_json[PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES + 2U];
} playback_board_link_tcp_state_t;

static playback_board_link_tcp_state_t *playback_board_link_tcp_get_state(
    const playback_board_link_tcp_t *tcp
)
{
    return (tcp == NULL) ? NULL : (playback_board_link_tcp_state_t *)tcp->state;
}

static size_t playback_board_link_tcp_bounded_length(
    const char *value,
    size_t capacity
)
{
    size_t length = 0U;

    if (value == NULL)
    {
        return capacity;
    }
    while ((length < capacity) && (value[length] != '\0'))
    {
        length++;
    }
    return length;
}

static bool playback_board_link_tcp_copy_string(
    char *destination,
    size_t capacity,
    const char *source
)
{
    size_t length;

    if ((destination == NULL) || (capacity == 0U) || (source == NULL))
    {
        return false;
    }
    length = playback_board_link_tcp_bounded_length(source, capacity);
    if ((length == 0U) || (length >= capacity))
    {
        return false;
    }
    memcpy(destination, source, length + 1U);
    return true;
}

static bool playback_board_link_tcp_ipv4_valid(const char *value)
{
    unsigned int octet = 0U;
    unsigned int digits = 0U;
    unsigned int count = 0U;
    size_t index;
    size_t length;

    if (value == NULL)
    {
        return false;
    }
    length = playback_board_link_tcp_bounded_length(
        value,
        PLAYBACK_BOARD_LINK_TCP_IPV4_MAX_LEN + 2U
    );
    if ((length == 0U) || (length > PLAYBACK_BOARD_LINK_TCP_IPV4_MAX_LEN))
    {
        return false;
    }

    for (index = 0U; index <= length; ++index)
    {
        const char character = (index < length) ? value[index] : '.';

        if ((character >= '0') && (character <= '9'))
        {
            octet = (octet * 10U) + (unsigned int)(character - '0');
            digits++;
            if ((digits > 3U) || (octet > 255U))
            {
                return false;
            }
            continue;
        }
        if ((character != '.') || (digits == 0U) || (count >= 4U))
        {
            return false;
        }
        count++;
        octet = 0U;
        digits = 0U;
    }

    return (count == 4U) && (strcmp(value, "0.0.0.0") != 0) &&
           (strcmp(value, "255.255.255.255") != 0);
}

static void playback_board_link_tcp_status_snapshot(
    playback_board_link_tcp_state_t *state,
    playback_board_link_tcp_status_t *status
)
{
    tal_mutex_lock(state->status_mutex);
    *status = state->status;
    tal_mutex_unlock(state->status_mutex);
}

static void playback_board_link_tcp_publish_status(
    playback_board_link_tcp_state_t *state
)
{
    playback_board_link_tcp_status_t status;

    if ((state == NULL) || (state->config.on_status == NULL))
    {
        return;
    }
    playback_board_link_tcp_status_snapshot(state, &status);
    state->config.on_status(state->config.context, &status);
}

static int playback_board_link_tcp_send_all(
    int fd,
    const void *buffer,
    size_t length
)
{
    const uint8_t *bytes = buffer;
    size_t sent = 0U;

    while (sent < length)
    {
        const int result = tal_net_send(fd, bytes + sent, (uint32_t)(length - sent));
        if (result <= 0)
        {
            return -1;
        }
        sent += (size_t)result;
    }
    return 0;
}

static int playback_board_link_tcp_receive_json(
    playback_board_link_tcp_state_t *state,
    int fd,
    size_t *json_length
)
{
    size_t received = 0U;

    *json_length = 0U;
    while (received < (sizeof(state->command_json) - 1U))
    {
        size_t index;
        const int result = tal_net_recv(
            fd,
            state->command_json + received,
            (uint32_t)(sizeof(state->command_json) - 1U - received)
        );

        if (result < 0)
        {
            return -1;
        }
        if (result == 0)
        {
            break;
        }
        received += (size_t)result;

        for (index = 0U; index < received; ++index)
        {
            if (state->command_json[index] == '\n')
            {
                received = index;
                if ((received > 0U) &&
                    (state->command_json[received - 1U] == '\r'))
                {
                    received--;
                }
                *json_length = received;
                return (received > 0U) ? 0 : -1;
            }
        }
    }

    while ((received > 0U) &&
           ((state->command_json[received - 1U] == '\r') ||
            (state->command_json[received - 1U] == '\n')))
    {
        received--;
    }
    if ((received == 0U) || (received > PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES))
    {
        return -1;
    }
    *json_length = received;
    return 0;
}

static void playback_board_link_tcp_handle_client(
    playback_board_link_tcp_state_t *state,
    int client_fd
)
{
    playback_command_t command;
    playback_board_link_json_result_t json_result;
    size_t json_length = 0U;

    (void)tal_net_set_timeout(client_fd, PLAYBACK_TCP_IO_TIMEOUT_MS, TRANS_RECV);
    memset(&command, 0, sizeof(command));
    if (playback_board_link_tcp_receive_json(state, client_fd, &json_length) != 0)
    {
        tal_mutex_lock(state->status_mutex);
        state->status.rejected_message_count++;
        tal_mutex_unlock(state->status_mutex);
        PR_WARN("Board Link TCP rejected empty or oversized JSON message");
        return;
    }

    json_result = playback_board_link_command_parse(
        state->command_json,
        json_length,
        &command
    );
    if (json_result != PLAYBACK_BOARD_LINK_JSON_OK)
    {
        tal_mutex_lock(state->status_mutex);
        state->status.rejected_message_count++;
        tal_mutex_unlock(state->status_mutex);
        PR_WARN(
            "Board Link TCP rejected JSON: %s",
            playback_board_link_json_result_name(json_result)
        );
        return;
    }

    tal_mutex_lock(state->status_mutex);
    state->status.received_command_count++;
    tal_mutex_unlock(state->status_mutex);
    if (state->config.on_command != NULL)
    {
        state->config.on_command(state->config.context, &command);
    }
}

static void playback_board_link_tcp_server_worker(void *context)
{
    playback_board_link_tcp_state_t *state = context;

    while (!state->closing)
    {
        TUYA_FD_SET_T read_fds;
        int ready;

        tal_net_fd_zero(&read_fds);
        tal_net_fd_set(state->listen_fd, &read_fds);
        ready = tal_net_select(
            state->listen_fd + 1,
            &read_fds,
            NULL,
            NULL,
            PLAYBACK_TCP_SELECT_TIMEOUT_MS
        );
        if (state->closing)
        {
            break;
        }
        if ((ready <= 0) || !tal_net_fd_isset(state->listen_fd, &read_fds))
        {
            continue;
        }

        {
            TUYA_IP_ADDR_T client_address = 0U;
            uint16_t client_port = 0U;
            const int client_fd = tal_net_accept(
                state->listen_fd,
                &client_address,
                &client_port
            );
            if (client_fd >= 0)
            {
                PR_DEBUG(
                    "Board Link TCP JSON from %s:%u",
                    tal_net_addr2str(client_address),
                    (unsigned int)client_port
                );
                playback_board_link_tcp_handle_client(state, client_fd);
                tal_net_close(client_fd);
            }
        }
    }

    tal_semaphore_post(state->server_stopped);
}

static playback_board_link_tcp_result_t playback_board_link_tcp_send_report_now(
    playback_board_link_tcp_state_t *state,
    const playback_report_t *report
)
{
    playback_board_link_tcp_status_t status;
    playback_board_link_json_result_t json_result;
    TUYA_IP_ADDR_T address;
    size_t json_length = 0U;
    int fd;

    playback_board_link_tcp_status_snapshot(state, &status);
    if (!status.peer_configured)
    {
        return PLAYBACK_BOARD_LINK_TCP_PEER_NOT_CONFIGURED;
    }

    json_result = playback_board_link_report_serialize(
        report,
        (report->kind == PLAYBACK_REPORT_HELLO_ACK) ? &state->local_hello : NULL,
        state->report_json,
        PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES + 1U,
        &json_length
    );
    if (json_result != PLAYBACK_BOARD_LINK_JSON_OK)
    {
        return PLAYBACK_BOARD_LINK_TCP_PLATFORM_ERROR;
    }
    state->report_json[json_length++] = '\n';

    address = tal_net_str2addr(status.peer_ip);
    fd = tal_net_socket_create(PROTOCOL_TCP);
    if (fd < 0)
    {
        return PLAYBACK_BOARD_LINK_TCP_PLATFORM_ERROR;
    }
    (void)tal_net_set_timeout(fd, PLAYBACK_TCP_IO_TIMEOUT_MS, TRANS_SEND);
    if ((tal_net_connect(fd, address, PLAYBACK_BOARD_LINK_TCP_PORT) != 0) ||
        (playback_board_link_tcp_send_all(fd, state->report_json, json_length) != 0))
    {
        tal_net_close(fd);
        return PLAYBACK_BOARD_LINK_TCP_PLATFORM_ERROR;
    }

    tal_net_close(fd);
    return PLAYBACK_BOARD_LINK_TCP_OK;
}

static void playback_board_link_tcp_record_send_result(
    playback_board_link_tcp_state_t *state,
    playback_board_link_tcp_result_t result
)
{
    tal_mutex_lock(state->status_mutex);
    if (result == PLAYBACK_BOARD_LINK_TCP_OK)
    {
        state->status.sent_report_count++;
    }
    else if (result != PLAYBACK_BOARD_LINK_TCP_PEER_NOT_CONFIGURED)
    {
        state->status.failed_report_count++;
    }
    tal_mutex_unlock(state->status_mutex);
}

static void playback_board_link_tcp_sender_worker(void *context)
{
    playback_board_link_tcp_state_t *state = context;
    playback_board_link_tcp_report_event_t event;
    uint64_t next_snapshot_ms =
        tal_system_get_millisecond() + PLAYBACK_TCP_STATUS_INTERVAL_MS;

    while (!state->closing)
    {
        const uint64_t now = tal_system_get_millisecond();
        const uint32_t wait_ms = now >= next_snapshot_ms
                                     ? 0U
                                     : (uint32_t)(next_snapshot_ms - now);

        if (tal_queue_fetch(state->report_queue, &event, wait_ms) == OPRT_OK)
        {
            if (state->closing)
            {
                break;
            }
            playback_board_link_tcp_record_send_result(
                state,
                playback_board_link_tcp_send_report_now(state, &event.report)
            );
        }

        if (!state->closing &&
            (tal_system_get_millisecond() >= next_snapshot_ms))
        {
            playback_report_t snapshot_report;

            memset(&snapshot_report, 0, sizeof(snapshot_report));
            if ((state->config.get_snapshot != NULL) &&
                state->config.get_snapshot(state->config.context, &snapshot_report))
            {
                playback_board_link_tcp_record_send_result(
                    state,
                    playback_board_link_tcp_send_report_now(state, &snapshot_report)
                );
            }
            next_snapshot_ms =
                tal_system_get_millisecond() + PLAYBACK_TCP_STATUS_INTERVAL_MS;
        }
    }

    tal_semaphore_post(state->sender_stopped);
}

static void playback_board_link_tcp_release_state(
    playback_board_link_tcp_state_t *state
)
{
    if (state == NULL)
    {
        return;
    }
    if (state->report_queue != NULL)
    {
        tal_queue_free(state->report_queue);
    }
    if (state->server_stopped != NULL)
    {
        tal_semaphore_release(state->server_stopped);
    }
    if (state->sender_stopped != NULL)
    {
        tal_semaphore_release(state->sender_stopped);
    }
    if (state->status_mutex != NULL)
    {
        tal_mutex_release(state->status_mutex);
    }
    tal_psram_free(state);
}

playback_board_link_tcp_result_t playback_board_link_tcp_init(
    playback_board_link_tcp_t *tcp,
    const playback_board_link_tcp_config_t *config
)
{
    playback_board_link_tcp_state_t *state;

    if ((tcp == NULL) || (config == NULL) || (config->boot_id == NULL) ||
        (config->on_command == NULL) || (config->get_snapshot == NULL))
    {
        return PLAYBACK_BOARD_LINK_TCP_INVALID_ARGUMENT;
    }
    if (tcp->state != NULL)
    {
        return PLAYBACK_BOARD_LINK_TCP_ALREADY_INITIALIZED;
    }

    state = tal_psram_calloc(1U, sizeof(*state));
    if (state == NULL)
    {
        return PLAYBACK_BOARD_LINK_TCP_NO_MEMORY;
    }
    state->config = *config;
    state->listen_fd = -1;
    state->status.initialized = true;

    memset(&state->local_hello, 0, sizeof(state->local_hello));
    state->local_hello.protocol_major = PLAYBACK_BOARD_LINK_PROTOCOL_MAJOR;
    state->local_hello.protocol_minor = 0U;
    state->local_hello.max_message_bytes = PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES;
    state->local_hello.capability_count = 2U;
    if (!playback_board_link_tcp_copy_string(
            state->local_hello.boot_id,
            sizeof(state->local_hello.boot_id),
            config->boot_id
        ) ||
        !playback_board_link_tcp_copy_string(
            state->local_hello.capabilities[0],
            sizeof(state->local_hello.capabilities[0]),
            "BOARD_LINK_V1"
        ) ||
        !playback_board_link_tcp_copy_string(
            state->local_hello.capabilities[1],
            sizeof(state->local_hello.capabilities[1]),
            "T5AI_H264_MP3_V1"
        ))
    {
        playback_board_link_tcp_release_state(state);
        return PLAYBACK_BOARD_LINK_TCP_INVALID_ARGUMENT;
    }

    if ((tal_mutex_create_init(&state->status_mutex) != OPRT_OK) ||
        (tal_semaphore_create_init(&state->server_stopped, 0U, 1U) != OPRT_OK) ||
        (tal_semaphore_create_init(&state->sender_stopped, 0U, 1U) != OPRT_OK) ||
        (tal_queue_create_init(
             &state->report_queue,
             sizeof(playback_board_link_tcp_report_event_t),
             PLAYBACK_TCP_REPORT_QUEUE_DEPTH
         ) != OPRT_OK))
    {
        playback_board_link_tcp_release_state(state);
        return PLAYBACK_BOARD_LINK_TCP_NO_MEMORY;
    }

    tcp->state = state;
    return PLAYBACK_BOARD_LINK_TCP_OK;
}

playback_board_link_tcp_result_t playback_board_link_tcp_start(
    playback_board_link_tcp_t *tcp
)
{
    playback_board_link_tcp_state_t *state = playback_board_link_tcp_get_state(tcp);
    THREAD_CFG_T server_config;
    THREAD_CFG_T sender_config;
    int listen_fd;

    if (state == NULL)
    {
        return PLAYBACK_BOARD_LINK_TCP_NOT_INITIALIZED;
    }
    if (state->status.started)
    {
        return PLAYBACK_BOARD_LINK_TCP_ALREADY_STARTED;
    }

    listen_fd = tal_net_socket_create(PROTOCOL_TCP);
    if (listen_fd < 0)
    {
        return PLAYBACK_BOARD_LINK_TCP_PLATFORM_ERROR;
    }
    (void)tal_net_set_reuse(listen_fd);
    (void)tal_net_set_block(listen_fd, FALSE);
    if ((tal_net_bind(listen_fd, TY_IPADDR_ANY, PLAYBACK_BOARD_LINK_TCP_PORT) != 0) ||
        (tal_net_listen(listen_fd, PLAYBACK_TCP_SERVER_BACKLOG) != 0))
    {
        tal_net_close(listen_fd);
        return PLAYBACK_BOARD_LINK_TCP_PLATFORM_ERROR;
    }
    state->listen_fd = listen_fd;
    state->closing = false;

    memset(&sender_config, 0, sizeof(sender_config));
    sender_config.stackDepth = PLAYBACK_TCP_SENDER_STACK_SIZE;
    sender_config.priority = THREAD_PRIO_2;
    sender_config.thrdname = "board_tcp_tx";
    sender_config.psram_mode = 1U;
    if (tal_thread_create_and_start(
            &state->sender_thread,
            NULL,
            NULL,
            playback_board_link_tcp_sender_worker,
            state,
            &sender_config
        ) != OPRT_OK)
    {
        tal_net_close(listen_fd);
        state->listen_fd = -1;
        return PLAYBACK_BOARD_LINK_TCP_PLATFORM_ERROR;
    }

    memset(&server_config, 0, sizeof(server_config));
    server_config.stackDepth = PLAYBACK_TCP_SERVER_STACK_SIZE;
    server_config.priority = THREAD_PRIO_2;
    server_config.thrdname = "board_tcp_rx";
    server_config.psram_mode = 1U;
    if (tal_thread_create_and_start(
            &state->server_thread,
            NULL,
            NULL,
            playback_board_link_tcp_server_worker,
            state,
            &server_config
        ) != OPRT_OK)
    {
        state->closing = true;
        tal_net_close(listen_fd);
        state->listen_fd = -1;
        (void)tal_semaphore_wait(state->sender_stopped, 1000U);
        if (state->sender_thread != NULL)
        {
            (void)tal_thread_delete(state->sender_thread);
            state->sender_thread = NULL;
        }
        state->closing = false;
        return PLAYBACK_BOARD_LINK_TCP_PLATFORM_ERROR;
    }

    tal_mutex_lock(state->status_mutex);
    state->status.started = true;
    tal_mutex_unlock(state->status_mutex);
    playback_board_link_tcp_publish_status(state);
    PR_NOTICE(
        "Board Link TCP ready: 0.0.0.0:%u raw JSON status=%u ms",
        (unsigned int)PLAYBACK_BOARD_LINK_TCP_PORT,
        (unsigned int)PLAYBACK_TCP_STATUS_INTERVAL_MS
    );
    return PLAYBACK_BOARD_LINK_TCP_OK;
}

playback_board_link_tcp_result_t playback_board_link_tcp_set_peer_ip(
    playback_board_link_tcp_t *tcp,
    const char *peer_ip
)
{
    playback_board_link_tcp_state_t *state = playback_board_link_tcp_get_state(tcp);

    if (state == NULL)
    {
        return PLAYBACK_BOARD_LINK_TCP_NOT_INITIALIZED;
    }
    if (!playback_board_link_tcp_ipv4_valid(peer_ip))
    {
        return PLAYBACK_BOARD_LINK_TCP_INVALID_ARGUMENT;
    }

    tal_mutex_lock(state->status_mutex);
    (void)playback_board_link_tcp_copy_string(
        state->status.peer_ip,
        sizeof(state->status.peer_ip),
        peer_ip
    );
    state->status.peer_configured = true;
    tal_mutex_unlock(state->status_mutex);
    playback_board_link_tcp_publish_status(state);
    PR_NOTICE(
        "Board Link TCP peer: %s:%u",
        peer_ip,
        (unsigned int)PLAYBACK_BOARD_LINK_TCP_PORT
    );
    return PLAYBACK_BOARD_LINK_TCP_OK;
}

playback_board_link_tcp_result_t playback_board_link_tcp_send_report(
    playback_board_link_tcp_t *tcp,
    const playback_report_t *report
)
{
    playback_board_link_tcp_state_t *state = playback_board_link_tcp_get_state(tcp);
    playback_board_link_tcp_report_event_t event;
    playback_board_link_tcp_status_t status;

    if (report == NULL)
    {
        return PLAYBACK_BOARD_LINK_TCP_INVALID_ARGUMENT;
    }
    if (state == NULL)
    {
        return PLAYBACK_BOARD_LINK_TCP_NOT_INITIALIZED;
    }
    playback_board_link_tcp_status_snapshot(state, &status);
    if (!status.started)
    {
        return PLAYBACK_BOARD_LINK_TCP_NOT_STARTED;
    }
    if (!status.peer_configured)
    {
        return PLAYBACK_BOARD_LINK_TCP_PEER_NOT_CONFIGURED;
    }

    memset(&event, 0, sizeof(event));
    event.report = *report;
    if (tal_queue_post(state->report_queue, &event, 0U) != OPRT_OK)
    {
        tal_mutex_lock(state->status_mutex);
        state->status.failed_report_count++;
        tal_mutex_unlock(state->status_mutex);
        return PLAYBACK_BOARD_LINK_TCP_QUEUE_FULL;
    }
    return PLAYBACK_BOARD_LINK_TCP_OK;
}

playback_board_link_tcp_result_t playback_board_link_tcp_get_status(
    playback_board_link_tcp_t *tcp,
    playback_board_link_tcp_status_t *status
)
{
    playback_board_link_tcp_state_t *state = playback_board_link_tcp_get_state(tcp);

    if (status == NULL)
    {
        return PLAYBACK_BOARD_LINK_TCP_INVALID_ARGUMENT;
    }
    if (state == NULL)
    {
        return PLAYBACK_BOARD_LINK_TCP_NOT_INITIALIZED;
    }
    playback_board_link_tcp_status_snapshot(state, status);
    return PLAYBACK_BOARD_LINK_TCP_OK;
}

void playback_board_link_tcp_close(playback_board_link_tcp_t *tcp)
{
    playback_board_link_tcp_state_t *state = playback_board_link_tcp_get_state(tcp);

    if (state == NULL)
    {
        return;
    }
    tcp->state = NULL;
    state->closing = true;
    if (state->listen_fd >= 0)
    {
        tal_net_close(state->listen_fd);
        state->listen_fd = -1;
    }
    if (state->server_thread != NULL)
    {
        (void)tal_semaphore_wait(state->server_stopped, 2000U);
        (void)tal_thread_delete(state->server_thread);
    }
    if (state->sender_thread != NULL)
    {
        (void)tal_semaphore_wait(state->sender_stopped, 2000U);
        (void)tal_thread_delete(state->sender_thread);
    }
    playback_board_link_tcp_release_state(state);
}

const char *playback_board_link_tcp_result_name(
    playback_board_link_tcp_result_t result
)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "ALREADY_INITIALIZED",
        "NOT_INITIALIZED",
        "ALREADY_STARTED",
        "NOT_STARTED",
        "PEER_NOT_CONFIGURED",
        "QUEUE_FULL",
        "NO_MEMORY",
        "PLATFORM_ERROR",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0])))
               ? names[result]
               : "UNKNOWN_BOARD_LINK_TCP_RESULT";
}
