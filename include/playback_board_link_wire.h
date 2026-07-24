#ifndef PLAYBACK_BOARD_LINK_WIRE_H
#define PLAYBACK_BOARD_LINK_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_BOARD_LINK_MAGIC (0xA7U)
#define PLAYBACK_BOARD_LINK_PROTOCOL_MAJOR (1U)
#define PLAYBACK_BOARD_LINK_HEADER_SIZE (16U)
#define PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES (4096U)
#define PLAYBACK_BOARD_LINK_MAX_FRAGMENT_COUNT (PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES)
#define PLAYBACK_BOARD_LINK_ASSEMBLY_TIMEOUT_MS (5000U)

typedef enum
{
    PLAYBACK_BOARD_LINK_MESSAGE_COMMAND = 0x01,
    PLAYBACK_BOARD_LINK_MESSAGE_REPORT = 0x02,
} playback_board_link_message_kind_t;

typedef enum
{
    PLAYBACK_BOARD_LINK_WIRE_OK = 0,
    PLAYBACK_BOARD_LINK_WIRE_INCOMPLETE,
    PLAYBACK_BOARD_LINK_WIRE_COMPLETE,
    PLAYBACK_BOARD_LINK_WIRE_INVALID_ARGUMENT,
    PLAYBACK_BOARD_LINK_WIRE_BUFFER_TOO_SMALL,
    PLAYBACK_BOARD_LINK_WIRE_INVALID_MAGIC,
    PLAYBACK_BOARD_LINK_WIRE_UNSUPPORTED_PROTOCOL,
    PLAYBACK_BOARD_LINK_WIRE_INVALID_MESSAGE_KIND,
    PLAYBACK_BOARD_LINK_WIRE_INVALID_FLAGS,
    PLAYBACK_BOARD_LINK_WIRE_INVALID_MESSAGE_ID,
    PLAYBACK_BOARD_LINK_WIRE_INVALID_FRAGMENT,
    PLAYBACK_BOARD_LINK_WIRE_LENGTH_MISMATCH,
    PLAYBACK_BOARD_LINK_WIRE_MESSAGE_TOO_LARGE,
    PLAYBACK_BOARD_LINK_WIRE_NO_MEMORY,
    PLAYBACK_BOARD_LINK_WIRE_ASSEMBLY_CONFLICT,
    PLAYBACK_BOARD_LINK_WIRE_CONFLICTING_DUPLICATE,
} playback_board_link_wire_result_t;

typedef struct
{
    uint8_t protocol_major;
    playback_board_link_message_kind_t message_kind;
    uint8_t flags;
    uint32_t message_id;
    uint16_t fragment_index;
    uint16_t fragment_count;
    uint16_t payload_length;
    uint16_t reserved;
} playback_board_link_fragment_header_t;

typedef struct
{
    playback_board_link_message_kind_t message_kind;
    uint32_t message_id;
    size_t body_length;
} playback_board_link_message_t;

typedef struct
{
    void *state;
} playback_board_link_reassembler_t;

playback_board_link_wire_result_t playback_board_link_fragment_header_encode(
    const playback_board_link_fragment_header_t *header,
    uint8_t *destination,
    size_t destination_capacity
);

playback_board_link_wire_result_t playback_board_link_fragment_header_decode(
    const uint8_t *fragment,
    size_t fragment_length,
    playback_board_link_fragment_header_t *header
);

playback_board_link_wire_result_t playback_board_link_fragment_plan(
    size_t body_length,
    size_t att_value_capacity,
    size_t *fragment_payload_capacity,
    uint16_t *fragment_count
);

playback_board_link_wire_result_t playback_board_link_fragment_encode(
    playback_board_link_message_kind_t message_kind,
    uint32_t message_id,
    const uint8_t *body,
    size_t body_length,
    size_t att_value_capacity,
    uint16_t fragment_index,
    uint8_t *destination,
    size_t destination_capacity,
    size_t *written
);

playback_board_link_wire_result_t playback_board_link_reassembler_init(
    playback_board_link_reassembler_t *reassembler
);

playback_board_link_wire_result_t playback_board_link_reassembler_push(
    playback_board_link_reassembler_t *reassembler,
    uint32_t now_ms,
    const uint8_t *fragment,
    size_t fragment_length,
    uint8_t *body_destination,
    size_t body_destination_capacity,
    playback_board_link_message_t *message
);

size_t playback_board_link_reassembler_expire(
    playback_board_link_reassembler_t *reassembler,
    uint32_t now_ms
);

void playback_board_link_reassembler_reset(playback_board_link_reassembler_t *reassembler);
void playback_board_link_reassembler_close(playback_board_link_reassembler_t *reassembler);
const char *playback_board_link_wire_result_name(playback_board_link_wire_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_BOARD_LINK_WIRE_H */
