#ifndef PLAYBACK_BOARD_LINK_TCP_H
#define PLAYBACK_BOARD_LINK_TCP_H

#include <stdbool.h>
#include <stdint.h>

#include "playback_domain.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_BOARD_LINK_TCP_PORT (8787U)
#define PLAYBACK_BOARD_LINK_TCP_IPV4_MAX_LEN (15U)

typedef enum
{
    PLAYBACK_BOARD_LINK_TCP_OK = 0,
    PLAYBACK_BOARD_LINK_TCP_INVALID_ARGUMENT,
    PLAYBACK_BOARD_LINK_TCP_ALREADY_INITIALIZED,
    PLAYBACK_BOARD_LINK_TCP_NOT_INITIALIZED,
    PLAYBACK_BOARD_LINK_TCP_ALREADY_STARTED,
    PLAYBACK_BOARD_LINK_TCP_NOT_STARTED,
    PLAYBACK_BOARD_LINK_TCP_PEER_NOT_CONFIGURED,
    PLAYBACK_BOARD_LINK_TCP_QUEUE_FULL,
    PLAYBACK_BOARD_LINK_TCP_NO_MEMORY,
    PLAYBACK_BOARD_LINK_TCP_PLATFORM_ERROR,
} playback_board_link_tcp_result_t;

typedef struct
{
    bool initialized;
    bool started;
    bool peer_configured;
    char peer_ip[PLAYBACK_BOARD_LINK_TCP_IPV4_MAX_LEN + 1U];
    uint32_t received_command_count;
    uint32_t rejected_message_count;
    uint32_t sent_report_count;
    uint32_t failed_report_count;
} playback_board_link_tcp_status_t;

typedef void (*playback_board_link_tcp_command_callback_t)(
    void *context,
    const playback_command_t *command
);

typedef void (*playback_board_link_tcp_status_callback_t)(
    void *context,
    const playback_board_link_tcp_status_t *status
);

typedef bool (*playback_board_link_tcp_snapshot_callback_t)(
    void *context,
    playback_report_t *report
);

typedef struct
{
    const char *boot_id;
    playback_board_link_tcp_command_callback_t on_command;
    playback_board_link_tcp_status_callback_t on_status;
    playback_board_link_tcp_snapshot_callback_t get_snapshot;
    void *context;
} playback_board_link_tcp_config_t;

typedef struct
{
    void *state;
} playback_board_link_tcp_t;

playback_board_link_tcp_result_t playback_board_link_tcp_init(
    playback_board_link_tcp_t *tcp,
    const playback_board_link_tcp_config_t *config
);

playback_board_link_tcp_result_t playback_board_link_tcp_start(
    playback_board_link_tcp_t *tcp
);

playback_board_link_tcp_result_t playback_board_link_tcp_set_peer_ip(
    playback_board_link_tcp_t *tcp,
    const char *peer_ip
);

playback_board_link_tcp_result_t playback_board_link_tcp_send_report(
    playback_board_link_tcp_t *tcp,
    const playback_report_t *report
);

playback_board_link_tcp_result_t playback_board_link_tcp_get_status(
    playback_board_link_tcp_t *tcp,
    playback_board_link_tcp_status_t *status
);

void playback_board_link_tcp_close(playback_board_link_tcp_t *tcp);
const char *playback_board_link_tcp_result_name(playback_board_link_tcp_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_BOARD_LINK_TCP_H */
