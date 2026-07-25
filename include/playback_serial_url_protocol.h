#ifndef PLAYBACK_SERIAL_URL_PROTOCOL_H
#define PLAYBACK_SERIAL_URL_PROTOCOL_H

/*
 * Deprecated compatibility header.
 *
 * The URL-only P06/P07 protocol was never hardware-valid on BK7258 and has
 * been replaced by the full Board Link UART adapter. New code must include
 * playback_board_link_uart.h directly and send the same Board Link JSON
 * commands used by the BLE transport.
 */
#include "playback_board_link_uart.h"

#define PLAYBACK_SERIAL_URL_BAUDRATE PLAYBACK_BOARD_LINK_UART_BAUDRATE
#define PLAYBACK_SERIAL_URL_P11_RX_PIN PLAYBACK_BOARD_LINK_UART_P11_RX_PIN
#define PLAYBACK_SERIAL_URL_P11_TX_PIN PLAYBACK_BOARD_LINK_UART_P11_TX_PIN
#define PLAYBACK_SERIAL_URL_RX_GPIO PLAYBACK_BOARD_LINK_UART_RX_GPIO
#define PLAYBACK_SERIAL_URL_TX_GPIO PLAYBACK_BOARD_LINK_UART_TX_GPIO

#endif /* PLAYBACK_SERIAL_URL_PROTOCOL_H */
