#ifndef PLAYBACK_BOARD_LINK_UART_H
#define PLAYBACK_BOARD_LINK_UART_H

#include <stdbool.h>
#include <stdint.h>

#include "playback_domain.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_BOARD_LINK_UART_BAUDRATE (115200U)
#define PLAYBACK_BOARD_LINK_UART_RX_GPIO (10U)
#define PLAYBACK_BOARD_LINK_UART_TX_GPIO (11U)
#define PLAYBACK_BOARD_LINK_UART_P11_RX_PIN (1U)
#define PLAYBACK_BOARD_LINK_UART_P11_TX_PIN (2U)

typedef enum
{
    PLAYBACK_BOARD_LINK_UART_OK = 0,
    PLAYBACK_BOARD_LINK_UART_INVALID_ARGUMENT,
    PLAYBACK_BOARD_LINK_UART_ALREADY_INITIALIZED,
    PLAYBACK_BOARD_LINK_UART_NOT_INITIALIZED,
    PLAYBACK_BOARD_LINK_UART_ALREADY_STARTED,
    PLAYBACK_BOARD_LINK_UART_NOT_STARTED,
    PLAYBACK_BOARD_LINK_UART_NOT_HANDSHAKEN,
    PLAYBACK_BOARD_LINK_UART_SERIALIZE_FAILED,
    PLAYBACK_BOARD_LINK_UART_WRITE_FAILED,
    PLAYBACK_BOARD_LINK_UART_NO_MEMORY,
    PLAYBACK_BOARD_LINK_UART_PLATFORM_ERROR,
} playback_board_link_uart_result_t;

typedef struct
{
    bool initialized;
    bool started;
    bool handshake_complete;
    uint32_t received_message_count;
    uint32_t rejected_message_count;
    char boot_id[PLAYBACK_BOOT_ID_MAX_LEN + 1U];
} playback_board_link_uart_status_t;

typedef void (*playback_board_link_uart_command_callback_t)(
    void *context,
    const playback_command_t *command
);

typedef void (*playback_board_link_uart_status_callback_t)(
    void *context,
    const playback_board_link_uart_status_t *status
);

typedef struct
{
    const char *boot_id;
    playback_board_link_uart_command_callback_t on_command;
    playback_board_link_uart_status_callback_t on_status;
    void *context;
} playback_board_link_uart_config_t;

typedef struct
{
    void *state;
} playback_board_link_uart_t;

playback_board_link_uart_result_t playback_board_link_uart_init(
    playback_board_link_uart_t *uart,
    const playback_board_link_uart_config_t *config
);

playback_board_link_uart_result_t playback_board_link_uart_start(
    playback_board_link_uart_t *uart
);

playback_board_link_uart_result_t playback_board_link_uart_send_report(
    playback_board_link_uart_t *uart,
    const playback_report_t *report
);

playback_board_link_uart_result_t playback_board_link_uart_get_status(
    playback_board_link_uart_t *uart,
    playback_board_link_uart_status_t *status
);

void playback_board_link_uart_close(playback_board_link_uart_t *uart);
const char *playback_board_link_uart_result_name(playback_board_link_uart_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_BOARD_LINK_UART_H */
