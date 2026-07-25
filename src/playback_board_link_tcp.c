/**
 * @file playback_board_link_tcp.c
 * @brief HTTP/JSON Board Link adapter on TCP port 8787.
 */

#include "playback_board_link_tcp.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "playback_board_link_json.h"
#include "playback_board_link_wire.h"
#include "tal_api.h"
#include "tal_network.h"

#define PLAYBACK_HTTP_COMMAND_PATH "/api/v1/playback/commands"
#define PLAYBACK_HTTP_EVENT_PATH "/api/v1/playback/events"
#define PLAYBACK_HTTP_HEALTH_PATH "/api/v1/playback/health"
#define PLAYBACK_HTTP_HEADER_MAX_BYTES (1536U)
#define PLAYBACK_HTTP_REQUEST_MAX_BYTES \
    (PLAYBACK_HTTP_HEADER_MAX_BYTES + PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES + 1U)
#define PLAYBACK_HTTP_RESPONSE_MAX_BYTES (320U)
#define PLAYBACK_HTTP_REPORT_QUEUE_DEPTH (12U)
#define PLAYBACK_HTTP_SERVER_BACKLOG (2)
#define PLAYBACK_HTTP_IO_TIMEOUT_MS (1500)
#define PLAYBACK_HTTP_SELECT_TIMEOUT_MS (200U)
#define PLAYBACK_HTTP_STATUS_INTERVAL_MS (500U)
#define PLAYBACK_HTTP_SERVER_STACK_SIZE (10U * 1024U)
#define PLAYBACK_HTTP_SENDER_STACK_SIZE (10U * 1024U)

typedef struct
{
    playback_report_t report;
} playback_board_link_tcp_report_event_t;

typedef struct
{
    playback_board_link_tcp_config_t config;
    playback_board_link_tcp_status_t status;
    MUTEX_HANDLE status_mutex;
    QUEUE_HANDLE report_queue;
    SEM_HANDLE server_stopped;
    SEM_HANDLE sender_stopped;
    THREAD_HANDLE server_thread;
    THREAD_HANDLE sender_thread;
    volatile bool closing;
    volatile int listen_fd;
    char request_buffer[PLAYBACK_HTTP_REQUEST_MAX_BYTES];
    char report_json[PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES + 1U];
    char response_buffer[PLAYBACK_HTTP_RESPONSE_MAX_BYTES];
} playback_board_link_tcp_state_t;

typedef struct
{
    char method[8];
    char path[80];
    size_t header_length;
    size_t content_length;
    const uint8_t *body;
} playback_board_link_tcp_request_t;

static playback_board_link_tcp_state_t *playback_board_link_tcp_get_state(
    const playback_board_link_tcp_t *http
)
{
    return (http == NULL) ? NULL : (playback_board_link_tcp_state_t *)http->state;
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
    const char *source,
    bool allow_empty
)
{
    size_t length;

    if ((destination == NULL) || (capacity == 0U) || (source == NULL))
    {
        return false;
    }
    length = playback_board_link_tcp_bounded_length(source, capacity);
    if ((length >= capacity) || (!allow_empty && (length == 0U)))
    {
        return false;
    }
    memcpy(destination, source, length + 1U);
    return true;
}

static bool playback_board_link_tcp_ipv4_valid(const char *value)
{
    unsigned int octet = 0U;
    unsigned int octet_digits = 0U;
    unsigned int octet_count = 0U;
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
            octet_digits++;
            if ((octet_digits > 3U) || (octet > 255U))
            {
                return false;
            }
            continue;
        }
        if (character != '.')
        {
            return false;
        }
        if ((octet_digits == 0U) || (octet_count >= 4U))
        {
            return false;
        }
        octet_count++;
        octet = 0U;
        octet_digits = 0U;
    }

    return (octet_count == 4U) && (strcmp(value, "0.0.0.0") != 0) &&
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

static void playback_board_link_tcp_increment(
    playback_board_link_tcp_state_t *state,
    uint32_t *counter
)
{
    tal_mutex_lock(state->status_mutex);
    (*counter)++;
    tal_mutex_unlock(state->status_mutex);
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

static bool playback_board_link_tcp_ascii_prefix(
    const char *value,
    size_t value_length,
    const char *prefix
)
{
    size_t index;
    const size_t prefix_length = strlen(prefix);

    if (value_length < prefix_length)
    {
        return false;
    }
    for (index = 0U; index < prefix_length; ++index)
    {
        if (tolower((unsigned char)value[index]) !=
            tolower((unsigned char)prefix[index]))
        {
            return false;
        }
    }
    return true;
}

static size_t playback_board_link_tcp_find_header_end(
    const char *buffer,
    size_t length
)
{
    size_t index;

    if (length < 4U)
    {
        return 0U;
    }
    for (index = 0U; index <= (length - 4U); ++index)
    {
        if ((buffer[index] == '\r') && (buffer[index + 1U] == '\n') &&
            (buffer[index + 2U] == '\r') && (buffer[index + 3U] == '\n'))
        {
            return index + 4U;
        }
    }
    return 0U;
}

static bool playback_board_link_tcp_parse_content_length(
    const char *buffer,
    size_t header_length,
    size_t *content_length
)
{
    const char *cursor;
    const char *header_end;

    if ((buffer == NULL) || (content_length == NULL) || (header_length < 4U))
    {
        return false;
    }
    *content_length = 0U;
    cursor = strstr(buffer, "\r\n");
    if (cursor == NULL)
    {
        return false;
    }
    cursor += 2;
    header_end = buffer + header_length - 2U;

    while (cursor < header_end)
    {
        const char *line_end = strstr(cursor, "\r\n");
        const char *number;
        size_t line_length;
        size_t parsed = 0U;
        bool has_digit = false;

        if ((line_end == NULL) || (line_end > header_end))
        {
            return false;
        }
        line_length = (size_t)(line_end - cursor);
        if (playback_board_link_tcp_ascii_prefix(
                cursor,
                line_length,
                "Content-Length:"
            ))
        {
            number = cursor + strlen("Content-Length:");
            while ((number < line_end) && ((*number == ' ') || (*number == '\t')))
            {
                number++;
            }
            while (number < line_end)
            {
                if ((*number < '0') || (*number > '9'))
                {
                    return false;
                }
                has_digit = true;
                if (parsed > ((SIZE_MAX - 9U) / 10U))
                {
                    return false;
                }
                parsed = (parsed * 10U) + (size_t)(*number - '0');
                number++;
            }
            if (!has_digit)
            {
                return false;
            }
            *content_length = parsed;
            return true;
        }
        cursor = line_end + 2;
    }

    return true;
}

static bool playback_board_link_tcp_parse_request(
    char *buffer,
    size_t length,
    playback_board_link_tcp_request_t *request
)
{
    char version[16];
    char *request_line_end;
    size_t header_length;

    if ((buffer == NULL) || (request == NULL) || (length == 0U))
    {
        return false;
    }
    header_length = playback_board_link_tcp_find_header_end(buffer, length);
    if ((header_length == 0U) || (header_length > PLAYBACK_HTTP_HEADER_MAX_BYTES))
    {
        return false;
    }
    request_line_end = strstr(buffer, "\r\n");
    if (request_line_end == NULL)
    {
        return false;
    }

    *request_line_end = '\0';
    memset(request, 0, sizeof(*request));
    if (sscanf(
            buffer,
            "%7s %79s %15s",
            request->method,
            request->path,
            version
        ) != 3)
    {
        *request_line_end = '\r';
        return false;
    }
    *request_line_end = '\r';
    if (strncmp(version, "HTTP/1.", 7U) != 0)
    {
        return false;
    }
    if (!playback_board_link_tcp_parse_content_length(
            buffer,
            header_length,
            &request->content_length
        ))
    {
        return false;
    }
    if ((request->content_length > PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES) ||
        ((header_length + request->content_length) > length))
    {
        return false;
    }

    request->header_length = header_length;
    request->body = (const uint8_t *)(buffer + header_length);
    return true;
}

static int playback_board_link_tcp_send_json_response(
    playback_board_link_tcp_state_t *state,
    int fd,
    unsigned int status_code,
    const char *status_text,
    const char *body
)
{
    int header_length;
    size_t body_length;

    if ((state == NULL) || (status_text == NULL) || (body == NULL))
    {
        return -1;
    }
    body_length = strlen(body);
    header_length = snprintf(
        state->response_buffer,
        sizeof(state->response_buffer),
        "HTTP/1.1 %u %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n",
        status_code,
        status_text,
        (unsigned int)body_length
    );
    if ((header_length <= 0) || ((size_t)header_length >= sizeof(state->response_buffer)))
    {
        return -1;
    }
    if (playback_board_link_tcp_send_all(
            fd,
            state->response_buffer,
            (size_t)header_length
        ) != 0)
    {
        return -1;
    }
    return playback_board_link_tcp_send_all(fd, body, body_length);
}

static int playback_board_link_tcp_receive_request(
    playback_board_link_tcp_state_t *state,
    int fd,
    playback_board_link_tcp_request_t *request
)
{
    size_t received = 0U;
    size_t header_length = 0U;
    size_t expected_length = 0U;
    size_t content_length = 0U;

    while (received < (sizeof(state->request_buffer) - 1U))
    {
        const int result = tal_net_recv(
            fd,
            state->request_buffer + received,
            (uint32_t)(sizeof(state->request_buffer) - 1U - received)
        );
        if (result <= 0)
        {
            return -1;
        }
        received += (size_t)result;
        state->request_buffer[received] = '\0';

        if (header_length == 0U)
        {
            header_length = playback_board_link_tcp_find_header_end(
                state->request_buffer,
                received
            );
            if (header_length == 0U)
            {
                if (received >= PLAYBACK_HTTP_HEADER_MAX_BYTES)
                {
                    return -2;
                }
                continue;
            }
            if ((header_length > PLAYBACK_HTTP_HEADER_MAX_BYTES) ||
                !playback_board_link_tcp_parse_content_length(
                    state->request_buffer,
                    header_length,
                    &content_length
                ))
            {
                return -2;
            }
            if (content_length > PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES)
            {
                return -3;
            }
            expected_length = header_length + content_length;
            if (expected_length >= sizeof(state->request_buffer))
            {
                return -3;
            }
        }

        if ((expected_length > 0U) && (received >= expected_length))
        {
            state->request_buffer[expected_length] = '\0';
            return playback_board_link_tcp_parse_request(
                       state->request_buffer,
                       expected_length,
                       request
                   )
                       ? 0
                       : -2;
        }
    }

    return -3;
}

static void playback_board_link_tcp_handle_client(
    playback_board_link_tcp_state_t *state,
    int client_fd
)
{
    playback_board_link_tcp_request_t request;
    playback_command_t command;
    playback_board_link_json_result_t json_result;
    int receive_result;
    int body_length;

    (void)tal_net_set_timeout(client_fd, PLAYBACK_HTTP_IO_TIMEOUT_MS, TRANS_RECV);
    (void)tal_net_set_timeout(client_fd, PLAYBACK_HTTP_IO_TIMEOUT_MS, TRANS_SEND);
    memset(&request, 0, sizeof(request));
    receive_result = playback_board_link_tcp_receive_request(state, client_fd, &request);
    if (receive_result != 0)
    {
        playback_board_link_tcp_increment(state, &state->status.rejected_request_count);
        if (receive_result == -3)
        {
            (void)playback_board_link_tcp_send_json_response(
                state,
                client_fd,
                413U,
                "Payload Too Large",
                "{\"ok\":false,\"error\":\"MESSAGE_TOO_LARGE\"}"
            );
        }
        else
        {
            (void)playback_board_link_tcp_send_json_response(
                state,
                client_fd,
                400U,
                "Bad Request",
                "{\"ok\":false,\"error\":\"MALFORMED_HTTP\"}"
            );
        }
        return;
    }

    if ((strcmp(request.method, "GET") == 0) &&
        (strcmp(request.path, PLAYBACK_HTTP_HEALTH_PATH) == 0))
    {
        (void)playback_board_link_tcp_send_json_response(
            state,
            client_fd,
            200U,
            "OK",
            "{\"ok\":true,\"role\":\"PLAYBACK\",\"port\":8787}"
        );
        return;
    }

    if ((strcmp(request.method, "POST") != 0) ||
        (strcmp(request.path, PLAYBACK_HTTP_COMMAND_PATH) != 0))
    {
        playback_board_link_tcp_increment(state, &state->status.rejected_request_count);
        (void)playback_board_link_tcp_send_json_response(
            state,
            client_fd,
            404U,
            "Not Found",
            "{\"ok\":false,\"error\":\"ROUTE_NOT_FOUND\"}"
        );
        return;
    }

    memset(&command, 0, sizeof(command));
    json_result = playback_board_link_command_parse(
        request.body,
        request.content_length,
        &command
    );
    if (json_result != PLAYBACK_BOARD_LINK_JSON_OK)
    {
        playback_board_link_tcp_increment(state, &state->status.rejected_request_count);
        body_length = snprintf(
            state->response_buffer,
            sizeof(state->response_buffer),
            "{\"ok\":false,\"error\":\"%s\",\"sequence_id\":%u}",
            playback_board_link_json_result_name(json_result),
            (unsigned int)command.sequence_id
        );
        if ((body_length <= 0) || ((size_t)body_length >= sizeof(state->response_buffer)))
        {
            (void)playback_board_link_tcp_send_json_response(
                state,
                client_fd,
                400U,
                "Bad Request",
                "{\"ok\":false,\"error\":\"MALFORMED\"}"
            );
        }
        else
        {
            char response_body[PLAYBACK_HTTP_RESPONSE_MAX_BYTES];
            memcpy(response_body, state->response_buffer, (size_t)body_length + 1U);
            (void)playback_board_link_tcp_send_json_response(
                state,
                client_fd,
                400U,
                "Bad Request",
                response_body
            );
        }
        return;
    }

    playback_board_link_tcp_increment(state, &state->status.received_command_count);
    if (state->config.on_command != NULL)
    {
        state->config.on_command(state->config.context, &command);
    }

    body_length = snprintf(
        state->response_buffer,
        sizeof(state->response_buffer),
        "{\"ok\":true,\"accepted\":true,\"sequence_id\":%u}",
        (unsigned int)command.sequence_id
    );
    if ((body_length <= 0) || ((size_t)body_length >= sizeof(state->response_buffer)))
    {
        (void)playback_board_link_tcp_send_json_response(
            state,
            client_fd,
            202U,
            "Accepted",
            "{\"ok\":true,\"accepted\":true}"
        );
    }
    else
    {
        char response_body[PLAYBACK_HTTP_RESPONSE_MAX_BYTES];
        memcpy(response_body, state->response_buffer, (size_t)body_length + 1U);
        (void)playback_board_link_tcp_send_json_response(
            state,
            client_fd,
            202U,
            "Accepted",
            response_body
        );
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
            PLAYBACK_HTTP_SELECT_TIMEOUT_MS
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
            if (client_fd > 0)
            {
                PR_DEBUG(
                    "Board Link TCP request from %s:%u",
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

static playback_board_link_tcp_result_t playback_board_link_tcp_post_report(
    playback_board_link_tcp_state_t *state,
    const playback_report_t *report
)
{
    playback_board_link_tcp_status_t status;
    playback_board_link_json_result_t json_result;
    TUYA_IP_ADDR_T address;
    size_t json_length = 0U;
    char header[320];
    int header_length;
    int fd;

    playback_board_link_tcp_status_snapshot(state, &status);
    if (!status.peer_configured)
    {
        return PLAYBACK_BOARD_LINK_TCP_PEER_NOT_CONFIGURED;
    }

    json_result = playback_board_link_report_serialize(
        report,
        NULL,
        state->report_json,
        sizeof(state->report_json),
        &json_length
    );
    if (json_result != PLAYBACK_BOARD_LINK_JSON_OK)
    {
        return PLAYBACK_BOARD_LINK_TCP_PLATFORM_ERROR;
    }

    header_length = snprintf(
        header,
        sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n",
        PLAYBACK_HTTP_EVENT_PATH,
        status.peer_ip,
        (unsigned int)PLAYBACK_BOARD_LINK_TCP_PORT,
        (unsigned int)json_length
    );
    if ((header_length <= 0) || ((size_t)header_length >= sizeof(header)))
    {
        return PLAYBACK_BOARD_LINK_TCP_PLATFORM_ERROR;
    }

    address = tal_net_str2addr(status.peer_ip);
    fd = tal_net_socket_create(PROTOCOL_TCP);
    if (fd < 0)
    {
        return PLAYBACK_BOARD_LINK_TCP_PLATFORM_ERROR;
    }
    (void)tal_net_set_timeout(fd, PLAYBACK_HTTP_IO_TIMEOUT_MS, TRANS_SEND);
    (void)tal_net_set_timeout(fd, PLAYBACK_HTTP_IO_TIMEOUT_MS, TRANS_RECV);
    if ((tal_net_connect(fd, address, PLAYBACK_BOARD_LINK_TCP_PORT) != 0) ||
        (playback_board_link_tcp_send_all(fd, header, (size_t)header_length) != 0) ||
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
        tal_system_get_millisecond() + PLAYBACK_HTTP_STATUS_INTERVAL_MS;

    while (!state->closing)
    {
        const uint64_t before_wait_ms = tal_system_get_millisecond();
        const uint32_t wait_ms = before_wait_ms >= next_snapshot_ms
                                     ? 0U
                                     : (uint32_t)(next_snapshot_ms - before_wait_ms);

        if (tal_queue_fetch(state->report_queue, &event, wait_ms) == OPRT_OK)
        {
            if (state->closing)
            {
                break;
            }
            playback_board_link_tcp_record_send_result(
                state,
                playback_board_link_tcp_post_report(state, &event.report)
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
                    playback_board_link_tcp_post_report(state, &snapshot_report)
                );
            }
            next_snapshot_ms =
                tal_system_get_millisecond() + PLAYBACK_HTTP_STATUS_INTERVAL_MS;
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
    playback_board_link_tcp_t *http,
    const playback_board_link_tcp_config_t *config
)
{
    playback_board_link_tcp_state_t *state;

    if ((http == NULL) || (config == NULL) || (config->on_command == NULL) ||
        (config->get_snapshot == NULL))
    {
        return PLAYBACK_BOARD_LINK_TCP_INVALID_ARGUMENT;
    }
    if (http->state != NULL)
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

    if ((tal_mutex_create_init(&state->status_mutex) != OPRT_OK) ||
        (tal_semaphore_create_init(&state->server_stopped, 0U, 1U) != OPRT_OK) ||
        (tal_semaphore_create_init(&state->sender_stopped, 0U, 1U) != OPRT_OK) ||
        (tal_queue_create_init(
             &state->report_queue,
             sizeof(playback_board_link_tcp_report_event_t),
             PLAYBACK_HTTP_REPORT_QUEUE_DEPTH
         ) != OPRT_OK))
    {
        playback_board_link_tcp_release_state(state);
        return PLAYBACK_BOARD_LINK_TCP_NO_MEMORY;
    }

    http->state = state;
    return PLAYBACK_BOARD_LINK_TCP_OK;
}

playback_board_link_tcp_result_t playback_board_link_tcp_start(
    playback_board_link_tcp_t *http
)
{
    playback_board_link_tcp_state_t *state = playback_board_link_tcp_get_state(http);
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
        (tal_net_listen(listen_fd, PLAYBACK_HTTP_SERVER_BACKLOG) != 0))
    {
        tal_net_close(listen_fd);
        return PLAYBACK_BOARD_LINK_TCP_PLATFORM_ERROR;
    }
    state->listen_fd = listen_fd;
    state->closing = false;

    memset(&sender_config, 0, sizeof(sender_config));
    sender_config.stackDepth = PLAYBACK_HTTP_SENDER_STACK_SIZE;
    sender_config.priority = THREAD_PRIO_2;
    sender_config.thrdname = "board_http_tx";
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
    server_config.stackDepth = PLAYBACK_HTTP_SERVER_STACK_SIZE;
    server_config.priority = THREAD_PRIO_2;
    server_config.thrdname = "board_http_rx";
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
        "Board Link TCP ready: listen=0.0.0.0:%u command=%s events=%s",
        (unsigned int)PLAYBACK_BOARD_LINK_TCP_PORT,
        PLAYBACK_HTTP_COMMAND_PATH,
        PLAYBACK_HTTP_EVENT_PATH
    );
    return PLAYBACK_BOARD_LINK_TCP_OK;
}

playback_board_link_tcp_result_t playback_board_link_tcp_set_peer_ip(
    playback_board_link_tcp_t *http,
    const char *peer_ip
)
{
    playback_board_link_tcp_state_t *state = playback_board_link_tcp_get_state(http);

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
        peer_ip,
        false
    );
    state->status.peer_configured = true;
    tal_mutex_unlock(state->status_mutex);
    playback_board_link_tcp_publish_status(state);
    PR_NOTICE(
        "Board Link TCP peer configured: %s:%u",
        peer_ip,
        (unsigned int)PLAYBACK_BOARD_LINK_TCP_PORT
    );
    return PLAYBACK_BOARD_LINK_TCP_OK;
}

playback_board_link_tcp_result_t playback_board_link_tcp_send_report(
    playback_board_link_tcp_t *http,
    const playback_report_t *report
)
{
    playback_board_link_tcp_state_t *state = playback_board_link_tcp_get_state(http);
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
    playback_board_link_tcp_t *http,
    playback_board_link_tcp_status_t *status
)
{
    playback_board_link_tcp_state_t *state = playback_board_link_tcp_get_state(http);

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

void playback_board_link_tcp_close(playback_board_link_tcp_t *http)
{
    playback_board_link_tcp_state_t *state = playback_board_link_tcp_get_state(http);

    if (state == NULL)
    {
        return;
    }
    http->state = NULL;
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
        state->server_thread = NULL;
    }
    if (state->sender_thread != NULL)
    {
        (void)tal_semaphore_wait(state->sender_stopped, 2000U);
        (void)tal_thread_delete(state->sender_thread);
        state->sender_thread = NULL;
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
               : "UNKNOWN_BOARD_LINK_HTTP_RESULT";
}
