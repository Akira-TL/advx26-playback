#ifndef PLAYBACK_BOARD_LINK_GATT_H
#define PLAYBACK_BOARD_LINK_GATT_H

#include <stdbool.h>
#include <stdint.h>

#include "playback_domain.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_BOARD_LINK_GATT_DEVICE_NAME "SoundPola Playback"
#define PLAYBACK_BOARD_LINK_GATT_ATT_MTU_DEFAULT (23U)
#define PLAYBACK_BOARD_LINK_GATT_ATT_MTU_MAX (247U)
#define PLAYBACK_BOARD_LINK_GATT_ATT_VALUE_MAX (244U)

typedef enum
{
    PLAYBACK_BOARD_LINK_GATT_OK = 0,
    PLAYBACK_BOARD_LINK_GATT_INVALID_ARGUMENT,
    PLAYBACK_BOARD_LINK_GATT_ALREADY_INITIALIZED,
    PLAYBACK_BOARD_LINK_GATT_NOT_INITIALIZED,
    PLAYBACK_BOARD_LINK_GATT_ALREADY_STARTED,
    PLAYBACK_BOARD_LINK_GATT_NOT_CONNECTED,
    PLAYBACK_BOARD_LINK_GATT_NOT_SUBSCRIBED,
    PLAYBACK_BOARD_LINK_GATT_QUEUE_FULL,
    PLAYBACK_BOARD_LINK_GATT_SERIALIZE_FAILED,
    PLAYBACK_BOARD_LINK_GATT_FRAGMENT_FAILED,
    PLAYBACK_BOARD_LINK_GATT_NOTIFY_FAILED,
    PLAYBACK_BOARD_LINK_GATT_NO_MEMORY,
    PLAYBACK_BOARD_LINK_GATT_PLATFORM_ERROR,
} playback_board_link_gatt_result_t;

typedef struct
{
    bool initialized;
    bool started;
    bool advertising;
    bool connected;
    bool subscribed;
    bool handshake_complete;
    uint16_t connection_handle;
    uint16_t att_mtu;
    uint16_t att_value_capacity;
    uint32_t dropped_event_count;
    char boot_id[PLAYBACK_BOOT_ID_MAX_LEN + 1U];
} playback_board_link_gatt_status_t;

/**
 * @brief Called on the Board Link worker thread for a complete decoded command.
 *
 * The command pointer is valid only for the duration of the callback. HELLO is
 * handled by the transport and is also delivered after HELLO_ACK succeeds.
 */
typedef void (*playback_board_link_command_callback_t)(
    void *context,
    const playback_command_t *command
);

/**
 * @brief Called on the Board Link worker thread after a visible link-state change.
 */
typedef void (*playback_board_link_status_callback_t)(
    void *context,
    const playback_board_link_gatt_status_t *status
);

typedef struct
{
    playback_board_link_command_callback_t on_command;
    playback_board_link_status_callback_t on_status;
    void *context;
} playback_board_link_gatt_config_t;

typedef struct
{
    void *state;
} playback_board_link_gatt_t;

playback_board_link_gatt_result_t playback_board_link_gatt_init(
    playback_board_link_gatt_t *gatt,
    const playback_board_link_gatt_config_t *config
);

playback_board_link_gatt_result_t playback_board_link_gatt_start(
    playback_board_link_gatt_t *gatt
);

playback_board_link_gatt_result_t playback_board_link_gatt_send_report(
    playback_board_link_gatt_t *gatt,
    const playback_report_t *report
);

playback_board_link_gatt_result_t playback_board_link_gatt_get_status(
    playback_board_link_gatt_t *gatt,
    playback_board_link_gatt_status_t *status
);

void playback_board_link_gatt_close(playback_board_link_gatt_t *gatt);
const char *playback_board_link_gatt_result_name(playback_board_link_gatt_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_BOARD_LINK_GATT_H */
