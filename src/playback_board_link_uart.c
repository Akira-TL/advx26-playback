/**
 * @file playback_board_link_uart.c
 * @brief UART0 adapter for Board Link JSON over P10/P11.
 */

#include "playback_board_link_uart.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "playback_board_link_json.h"
#include "playback_board_link_wire.h"
#include "tal_api.h"

#define PLAYBACK_BOARD_LINK_UART_PORT TUYA_UART_NUM_0
#define PLAYBACK_BOARD_LINK_UART_RX_BUFFER_BYTES (PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES * 2U)
#define PLAYBACK_BOARD_LINK_UART_READ_BYTES (256U)
#define PLAYBACK_BOARD_LINK_UART_POLL_MS (5U)
#define PLAYBACK_BOARD_LINK_UART_WORKER_STACK_SIZE (12U * 1024U)

typedef struct
{
    playback_board_link_uart_config_t config;
    playback_board_link_uart_status_t status;
    playback_hello_t local_hello;
    THREAD_HANDLE worker_thread;
    SEM_HANDLE worker_stopped;
    MUTEX_HANDLE status_mutex;
    MUTEX_HANDLE send_mutex;
    uint8_t command_body[PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES];
    size_t command_length;
    bool discarding_line;
    char report_body[PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES + 2U];
    volatile bool closing;
} playback_board_link_uart_state_t;

static size_t playback_board_link_uart_bounded_length(
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

static bool playback_board_link_uart_copy_string(
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
    length = playback_board_link_uart_bounded_length(source, capacity);
    if ((length == 0U) || (length >= capacity))
    {
        return false;
    }
    memcpy(destination, source, length + 1U);
    return true;
}

static void playback_board_link_uart_status_snapshot(
    playback_board_link_uart_state_t *state,
    playback_board_link_uart_status_t *status
)
{
    tal_mutex_lock(state->status_mutex);
    *status = state->status;
    tal_mutex_unlock(state->status_mutex);
}

static void playback_board_link_uart_publish_status(
    playback_board_link_uart_state_t *state
)
{
    playback_board_link_uart_status_t status;

    if (state->config.on_status == NULL)
    {
        return;
    }
    playback_board_link_uart_status_snapshot(state, &status);
    state->config.on_status(state->config.context, &status);
}

static playback_board_link_uart_result_t playback_board_link_uart_send_report_state(
    playback_board_link_uart_state_t *state,
    const playback_report_t *report
)
{
    playback_board_link_uart_status_t status;
    playback_board_link_json_result_t json_result;
    size_t report_length = 0U;
    int written;

    if ((state == NULL) || (report == NULL))
    {
        return PLAYBACK_BOARD_LINK_UART_INVALID_ARGUMENT;
    }

    playback_board_link_uart_status_snapshot(state, &status);
    if (!status.started)
    {
        return PLAYBACK_BOARD_LINK_UART_NOT_STARTED;
    }
    if (!status.handshake_complete &&
        (report->kind != PLAYBACK_REPORT_HELLO_ACK) &&
        (report->kind != PLAYBACK_REPORT_NACK))
    {
        return PLAYBACK_BOARD_LINK_UART_NOT_HANDSHAKEN;
    }

    tal_mutex_lock(state->send_mutex);
    json_result = playback_board_link_report_serialize(
        report,
        (report->kind == PLAYBACK_REPORT_HELLO_ACK) ? &state->local_hello : NULL,
        state->report_body,
        PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES + 1U,
        &report_length
    );
    if (json_result != PLAYBACK_BOARD_LINK_JSON_OK)
    {
        tal_mutex_unlock(state->send_mutex);
        return PLAYBACK_BOARD_LINK_UART_SERIALIZE_FAILED;
    }

    state->report_body[report_length] = '\n';
    written = tal_uart_write(
        PLAYBACK_BOARD_LINK_UART_PORT,
        (const uint8_t *)state->report_body,
        (uint32_t)(report_length + 1U)
    );
    tal_mutex_unlock(state->send_mutex);

    return (written == (int)(report_length + 1U))
               ? PLAYBACK_BOARD_LINK_UART_OK
               : PLAYBACK_BOARD_LINK_UART_WRITE_FAILED;
}

static void playback_board_link_uart_send_nack(
    playback_board_link_uart_state_t *state,
    const playback_command_t *command,
    playback_nack_t nack,
    const char *diagnostic
)
{
    playback_report_t report;

    memset(&report, 0, sizeof(report));
    report.kind = PLAYBACK_REPORT_NACK;
    report.sequence_id = (command != NULL) ? command->sequence_id : 0U;
    report.nack = nack;
    if (command != NULL)
    {
        report.acknowledged_command = command->kind;
        if (command->session_id[0] != '\0')
        {
            (void)playback_board_link_uart_copy_string(
                report.session_id,
                sizeof(report.session_id),
                command->session_id
            );
        }
    }
    if ((diagnostic != NULL) && (diagnostic[0] != '\0'))
    {
        (void)playback_board_link_uart_copy_string(
            report.diagnostic,
            sizeof(report.diagnostic),
            diagnostic
        );
    }
    (void)playback_board_link_uart_send_report_state(state, &report);
}

static void playback_board_link_uart_handle_hello(
    playback_board_link_uart_state_t *state,
    const playback_command_t *command
)
{
    playback_report_t report;

    tal_mutex_lock(state->status_mutex);
    state->status.handshake_complete = false;
    tal_mutex_unlock(state->status_mutex);

    memset(&report, 0, sizeof(report));
    report.kind = PLAYBACK_REPORT_HELLO_ACK;
    report.sequence_id = command->sequence_id;
    if (playback_board_link_uart_send_report_state(state, &report) !=
        PLAYBACK_BOARD_LINK_UART_OK)
    {
        return;
    }

    tal_mutex_lock(state->status_mutex);
    state->status.handshake_complete = true;
    tal_mutex_unlock(state->status_mutex);
    playback_board_link_uart_publish_status(state);

    if (state->config.on_command != NULL)
    {
        state->config.on_command(state->config.context, command);
    }
}

static void playback_board_link_uart_handle_command(
    playback_board_link_uart_state_t *state,
    const uint8_t *body,
    size_t body_length
)
{
    playback_command_t command;
    playback_board_link_json_result_t json_result;
    playback_board_link_uart_status_t status;

    memset(&command, 0, sizeof(command));
    json_result = playback_board_link_command_parse(body, body_length, &command);
    if (json_result != PLAYBACK_BOARD_LINK_JSON_OK)
    {
        tal_mutex_lock(state->status_mutex);
        state->status.rejected_message_count++;
        tal_mutex_unlock(state->status_mutex);
        playback_board_link_uart_send_nack(
            state,
            &command,
            playback_board_link_json_result_to_nack(json_result),
            playback_board_link_json_result_name(json_result)
        );
        return;
    }

    tal_mutex_lock(state->status_mutex);
    state->status.received_message_count++;
    tal_mutex_unlock(state->status_mutex);

    if (command.kind == PLAYBACK_COMMAND_HELLO)
    {
        playback_board_link_uart_handle_hello(state, &command);
        return;
    }

    playback_board_link_uart_status_snapshot(state, &status);
    if (!status.handshake_complete)
    {
        playback_board_link_uart_send_nack(
            state,
            &command,
            PLAYBACK_NACK_PROTOCOL_INCOMPATIBLE,
            "HELLO required"
        );
        return;
    }

    if (state->config.on_command != NULL)
    {
        state->config.on_command(state->config.context, &command);
    }
}

static void playback_board_link_uart_process_byte(
    playback_board_link_uart_state_t *state,
    uint8_t value
)
{
    if (value == '\r')
    {
        return;
    }
    if (value == '\n')
    {
        if (state->discarding_line)
        {
            tal_mutex_lock(state->status_mutex);
            state->status.rejected_message_count++;
            tal_mutex_unlock(state->status_mutex);
            playback_board_link_uart_send_nack(
                state,
                NULL,
                PLAYBACK_NACK_MALFORMED_MESSAGE,
                "UART line too long"
            );
        }
        else if (state->command_length > 0U)
        {
            playback_board_link_uart_handle_command(
                state,
                state->command_body,
                state->command_length
            );
        }
        state->command_length = 0U;
        state->discarding_line = false;
        return;
    }

    if (state->discarding_line)
    {
        return;
    }
    if (state->command_length >= sizeof(state->command_body))
    {
        state->discarding_line = true;
        return;
    }
    state->command_body[state->command_length++] = value;
}

static void playback_board_link_uart_worker(void *context)
{
    playback_board_link_uart_state_t *state = context;
    uint8_t read_buffer[PLAYBACK_BOARD_LINK_UART_READ_BYTES];

    while (!state->closing)
    {
        int read_length = tal_uart_read(
            PLAYBACK_BOARD_LINK_UART_PORT,
            read_buffer,
            sizeof(read_buffer)
        );
        if (read_length > 0)
        {
            int index;
            for (index = 0; index < read_length; ++index)
            {
                playback_board_link_uart_process_byte(state, read_buffer[index]);
            }
        }
        else
        {
            tal_system_sleep(PLAYBACK_BOARD_LINK_UART_POLL_MS);
        }
    }

    tal_semaphore_post(state->worker_stopped);
}

static void playback_board_link_uart_release_state(
    playback_board_link_uart_state_t *state
)
{
    if (state == NULL)
    {
        return;
    }
    if (state->worker_stopped != NULL)
    {
        tal_semaphore_release(state->worker_stopped);
    }
    if (state->send_mutex != NULL)
    {
        tal_mutex_release(state->send_mutex);
    }
    if (state->status_mutex != NULL)
    {
        tal_mutex_release(state->status_mutex);
    }
    tal_psram_free(state);
}

playback_board_link_uart_result_t playback_board_link_uart_init(
    playback_board_link_uart_t *uart,
    const playback_board_link_uart_config_t *config
)
{
    playback_board_link_uart_state_t *state;

    if ((uart == NULL) || (config == NULL) || (config->boot_id == NULL) ||
        (config->on_command == NULL))
    {
        return PLAYBACK_BOARD_LINK_UART_INVALID_ARGUMENT;
    }
    if (uart->state != NULL)
    {
        return PLAYBACK_BOARD_LINK_UART_ALREADY_INITIALIZED;
    }

    state = tal_psram_malloc(sizeof(*state));
    if (state == NULL)
    {
        return PLAYBACK_BOARD_LINK_UART_NO_MEMORY;
    }
    memset(state, 0, sizeof(*state));
    state->config = *config;
    if (!playback_board_link_uart_copy_string(
            state->status.boot_id,
            sizeof(state->status.boot_id),
            config->boot_id
        ))
    {
        playback_board_link_uart_release_state(state);
        return PLAYBACK_BOARD_LINK_UART_INVALID_ARGUMENT;
    }
    state->status.initialized = true;

    memset(&state->local_hello, 0, sizeof(state->local_hello));
    state->local_hello.protocol_major = PLAYBACK_BOARD_LINK_PROTOCOL_MAJOR;
    state->local_hello.protocol_minor = 0U;
    state->local_hello.max_message_bytes = PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES;
    state->local_hello.capability_count = 2U;
    (void)playback_board_link_uart_copy_string(
        state->local_hello.boot_id,
        sizeof(state->local_hello.boot_id),
        state->status.boot_id
    );
    (void)playback_board_link_uart_copy_string(
        state->local_hello.capabilities[0],
        sizeof(state->local_hello.capabilities[0]),
        "BOARD_LINK_V1"
    );
    (void)playback_board_link_uart_copy_string(
        state->local_hello.capabilities[1],
        sizeof(state->local_hello.capabilities[1]),
        "T5AI_H264_MP3_V1"
    );

    if ((tal_mutex_create_init(&state->status_mutex) != OPRT_OK) ||
        (tal_mutex_create_init(&state->send_mutex) != OPRT_OK) ||
        (tal_semaphore_create_init(&state->worker_stopped, 0U, 1U) != OPRT_OK))
    {
        playback_board_link_uart_release_state(state);
        return PLAYBACK_BOARD_LINK_UART_NO_MEMORY;
    }

    uart->state = state;
    return PLAYBACK_BOARD_LINK_UART_OK;
}

playback_board_link_uart_result_t playback_board_link_uart_start(
    playback_board_link_uart_t *uart
)
{
    playback_board_link_uart_state_t *state;
    TAL_UART_CFG_T uart_config;
    THREAD_CFG_T worker_config;

    if ((uart == NULL) || (uart->state == NULL))
    {
        return PLAYBACK_BOARD_LINK_UART_NOT_INITIALIZED;
    }
    state = uart->state;
    if (state->status.started)
    {
        return PLAYBACK_BOARD_LINK_UART_ALREADY_STARTED;
    }

    memset(&uart_config, 0, sizeof(uart_config));
    uart_config.base_cfg.baudrate = PLAYBACK_BOARD_LINK_UART_BAUDRATE;
    uart_config.base_cfg.databits = TUYA_UART_DATA_LEN_8BIT;
    uart_config.base_cfg.stopbits = TUYA_UART_STOP_LEN_1BIT;
    uart_config.base_cfg.parity = TUYA_UART_PARITY_TYPE_NONE;
    uart_config.rx_buffer_size = PLAYBACK_BOARD_LINK_UART_RX_BUFFER_BYTES;
    uart_config.open_mode = 0;
    if (tal_uart_init(PLAYBACK_BOARD_LINK_UART_PORT, &uart_config) != OPRT_OK)
    {
        return PLAYBACK_BOARD_LINK_UART_PLATFORM_ERROR;
    }

    tal_mutex_lock(state->status_mutex);
    state->status.started = true;
    tal_mutex_unlock(state->status_mutex);

    memset(&worker_config, 0, sizeof(worker_config));
    worker_config.stackDepth = PLAYBACK_BOARD_LINK_UART_WORKER_STACK_SIZE;
    worker_config.priority = THREAD_PRIO_2;
    worker_config.thrdname = "board_uart";
    worker_config.psram_mode = 1U;
    if (tal_thread_create_and_start(
            &state->worker_thread,
            NULL,
            NULL,
            playback_board_link_uart_worker,
            state,
            &worker_config
        ) != OPRT_OK)
    {
        tal_mutex_lock(state->status_mutex);
        state->status.started = false;
        tal_mutex_unlock(state->status_mutex);
        (void)tal_uart_deinit(PLAYBACK_BOARD_LINK_UART_PORT);
        return PLAYBACK_BOARD_LINK_UART_PLATFORM_ERROR;
    }
    playback_board_link_uart_publish_status(state);

    PR_NOTICE(
        "Board Link UART0 ready: RX=P10/P11-1 TX=P11/P11-2 baud=%u",
        (unsigned int)PLAYBACK_BOARD_LINK_UART_BAUDRATE
    );
    return PLAYBACK_BOARD_LINK_UART_OK;
}

playback_board_link_uart_result_t playback_board_link_uart_send_report(
    playback_board_link_uart_t *uart,
    const playback_report_t *report
)
{
    if ((uart == NULL) || (uart->state == NULL))
    {
        return PLAYBACK_BOARD_LINK_UART_NOT_INITIALIZED;
    }
    return playback_board_link_uart_send_report_state(uart->state, report);
}

playback_board_link_uart_result_t playback_board_link_uart_get_status(
    playback_board_link_uart_t *uart,
    playback_board_link_uart_status_t *status
)
{
    if (status == NULL)
    {
        return PLAYBACK_BOARD_LINK_UART_INVALID_ARGUMENT;
    }
    if ((uart == NULL) || (uart->state == NULL))
    {
        return PLAYBACK_BOARD_LINK_UART_NOT_INITIALIZED;
    }
    playback_board_link_uart_status_snapshot(uart->state, status);
    return PLAYBACK_BOARD_LINK_UART_OK;
}

void playback_board_link_uart_close(playback_board_link_uart_t *uart)
{
    playback_board_link_uart_state_t *state;
    playback_board_link_uart_status_t status;

    if ((uart == NULL) || (uart->state == NULL))
    {
        return;
    }
    state = uart->state;
    playback_board_link_uart_status_snapshot(state, &status);
    state->closing = true;

    if (status.started)
    {
        (void)tal_semaphore_wait(state->worker_stopped, 3000U);
        if (state->worker_thread != NULL)
        {
            (void)tal_thread_delete(state->worker_thread);
        }
        (void)tal_uart_deinit(PLAYBACK_BOARD_LINK_UART_PORT);
    }

    uart->state = NULL;
    playback_board_link_uart_release_state(state);
}

const char *playback_board_link_uart_result_name(
    playback_board_link_uart_result_t result
)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "ALREADY_INITIALIZED",
        "NOT_INITIALIZED",
        "ALREADY_STARTED",
        "NOT_STARTED",
        "NOT_HANDSHAKEN",
        "SERIALIZE_FAILED",
        "WRITE_FAILED",
        "NO_MEMORY",
        "PLATFORM_ERROR",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0])))
               ? names[result]
               : "UNKNOWN_BOARD_LINK_UART_RESULT";
}
