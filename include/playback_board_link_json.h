#ifndef PLAYBACK_BOARD_LINK_JSON_H
#define PLAYBACK_BOARD_LINK_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "playback_domain.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
    PLAYBACK_BOARD_LINK_JSON_OK = 0,
    PLAYBACK_BOARD_LINK_JSON_INVALID_ARGUMENT,
    PLAYBACK_BOARD_LINK_JSON_MALFORMED,
    PLAYBACK_BOARD_LINK_JSON_UNKNOWN_TYPE,
    PLAYBACK_BOARD_LINK_JSON_UNSUPPORTED_PROFILE,
    PLAYBACK_BOARD_LINK_JSON_PROTOCOL_INCOMPATIBLE,
    PLAYBACK_BOARD_LINK_JSON_MESSAGE_TOO_LARGE,
    PLAYBACK_BOARD_LINK_JSON_BUFFER_TOO_SMALL,
    PLAYBACK_BOARD_LINK_JSON_NO_MEMORY,
} playback_board_link_json_result_t;

playback_board_link_json_result_t playback_board_link_command_parse(
    const uint8_t *body,
    size_t body_length,
    playback_command_t *command
);

playback_board_link_json_result_t playback_board_link_report_serialize(
    const playback_report_t *report,
    const playback_hello_t *hello_ack,
    char *destination,
    size_t destination_capacity,
    size_t *written
);

playback_nack_t playback_board_link_json_result_to_nack(playback_board_link_json_result_t result);
const char *playback_board_link_json_result_name(playback_board_link_json_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_BOARD_LINK_JSON_H */
