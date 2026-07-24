#include "playback_board_link_wire.h"

#include <stdbool.h>
#include <string.h>

#include "tal_api.h"

typedef struct
{
    uint16_t offset;
    uint16_t length;
} playback_board_link_fragment_record_t;

typedef struct
{
    bool active;
    playback_board_link_message_kind_t message_kind;
    uint32_t message_id;
    uint32_t started_ms;
    uint16_t fragment_count;
    uint16_t received_count;
    size_t used_bytes;
    playback_board_link_fragment_record_t *records;
    uint8_t *payload_arena;
} playback_board_link_assembly_t;

typedef struct
{
    playback_board_link_assembly_t command;
    playback_board_link_assembly_t report;
} playback_board_link_reassembler_state_t;

static uint16_t playback_board_link_read_u16_le(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

static uint32_t playback_board_link_read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static void playback_board_link_write_u16_le(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static void playback_board_link_write_u32_le(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)((value >> 8U) & 0xFFU);
    data[2] = (uint8_t)((value >> 16U) & 0xFFU);
    data[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static bool playback_board_link_message_kind_valid(playback_board_link_message_kind_t message_kind)
{
    return (message_kind == PLAYBACK_BOARD_LINK_MESSAGE_COMMAND) ||
           (message_kind == PLAYBACK_BOARD_LINK_MESSAGE_REPORT);
}

static playback_board_link_wire_result_t playback_board_link_header_validate(
    const playback_board_link_fragment_header_t *header
)
{
    if (header == NULL)
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_ARGUMENT;
    }
    if (header->protocol_major != PLAYBACK_BOARD_LINK_PROTOCOL_MAJOR)
    {
        return PLAYBACK_BOARD_LINK_WIRE_UNSUPPORTED_PROTOCOL;
    }
    if (!playback_board_link_message_kind_valid(header->message_kind))
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_MESSAGE_KIND;
    }
    if ((header->flags != 0U) || (header->reserved != 0U))
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_FLAGS;
    }
    if (header->message_id == 0U)
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_MESSAGE_ID;
    }
    if ((header->fragment_count == 0U) ||
        (header->fragment_count > PLAYBACK_BOARD_LINK_MAX_FRAGMENT_COUNT) ||
        (header->fragment_index >= header->fragment_count) ||
        (header->payload_length == 0U))
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_FRAGMENT;
    }
    return PLAYBACK_BOARD_LINK_WIRE_OK;
}

static playback_board_link_assembly_t *playback_board_link_get_assembly(
    playback_board_link_reassembler_state_t *state,
    playback_board_link_message_kind_t message_kind
)
{
    if (message_kind == PLAYBACK_BOARD_LINK_MESSAGE_COMMAND)
    {
        return &state->command;
    }
    if (message_kind == PLAYBACK_BOARD_LINK_MESSAGE_REPORT)
    {
        return &state->report;
    }
    return NULL;
}

static void playback_board_link_assembly_clear(playback_board_link_assembly_t *assembly)
{
    if (assembly == NULL)
    {
        return;
    }
    if (assembly->records != NULL)
    {
        tal_free(assembly->records);
    }
    if (assembly->payload_arena != NULL)
    {
        tal_free(assembly->payload_arena);
    }
    memset(assembly, 0, sizeof(*assembly));
}

static bool playback_board_link_assembly_expired(
    const playback_board_link_assembly_t *assembly,
    uint32_t now_ms
)
{
    return assembly->active &&
           ((uint32_t)(now_ms - assembly->started_ms) >= PLAYBACK_BOARD_LINK_ASSEMBLY_TIMEOUT_MS);
}

static playback_board_link_wire_result_t playback_board_link_assembly_start(
    playback_board_link_assembly_t *assembly,
    const playback_board_link_fragment_header_t *header,
    uint32_t now_ms
)
{
    playback_board_link_fragment_record_t *records;
    uint8_t *payload_arena;

    records = tal_calloc(header->fragment_count, sizeof(*records));
    if (records == NULL)
    {
        return PLAYBACK_BOARD_LINK_WIRE_NO_MEMORY;
    }

    payload_arena = tal_malloc(PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES);
    if (payload_arena == NULL)
    {
        tal_free(records);
        return PLAYBACK_BOARD_LINK_WIRE_NO_MEMORY;
    }

    memset(assembly, 0, sizeof(*assembly));
    assembly->active = true;
    assembly->message_kind = header->message_kind;
    assembly->message_id = header->message_id;
    assembly->started_ms = now_ms;
    assembly->fragment_count = header->fragment_count;
    assembly->records = records;
    assembly->payload_arena = payload_arena;
    return PLAYBACK_BOARD_LINK_WIRE_OK;
}

playback_board_link_wire_result_t playback_board_link_fragment_header_encode(
    const playback_board_link_fragment_header_t *header,
    uint8_t *destination,
    size_t destination_capacity
)
{
    playback_board_link_wire_result_t result;

    if ((destination == NULL) || (destination_capacity < PLAYBACK_BOARD_LINK_HEADER_SIZE))
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_ARGUMENT;
    }

    result = playback_board_link_header_validate(header);
    if (result != PLAYBACK_BOARD_LINK_WIRE_OK)
    {
        return result;
    }

    destination[0] = PLAYBACK_BOARD_LINK_MAGIC;
    destination[1] = header->protocol_major;
    destination[2] = (uint8_t)header->message_kind;
    destination[3] = header->flags;
    playback_board_link_write_u32_le(&destination[4], header->message_id);
    playback_board_link_write_u16_le(&destination[8], header->fragment_index);
    playback_board_link_write_u16_le(&destination[10], header->fragment_count);
    playback_board_link_write_u16_le(&destination[12], header->payload_length);
    playback_board_link_write_u16_le(&destination[14], header->reserved);
    return PLAYBACK_BOARD_LINK_WIRE_OK;
}

playback_board_link_wire_result_t playback_board_link_fragment_header_decode(
    const uint8_t *fragment,
    size_t fragment_length,
    playback_board_link_fragment_header_t *header
)
{
    playback_board_link_wire_result_t result;

    if ((fragment == NULL) || (header == NULL) || (fragment_length < PLAYBACK_BOARD_LINK_HEADER_SIZE))
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_ARGUMENT;
    }
    if (fragment[0] != PLAYBACK_BOARD_LINK_MAGIC)
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_MAGIC;
    }

    memset(header, 0, sizeof(*header));
    header->protocol_major = fragment[1];
    header->message_kind = (playback_board_link_message_kind_t)fragment[2];
    header->flags = fragment[3];
    header->message_id = playback_board_link_read_u32_le(&fragment[4]);
    header->fragment_index = playback_board_link_read_u16_le(&fragment[8]);
    header->fragment_count = playback_board_link_read_u16_le(&fragment[10]);
    header->payload_length = playback_board_link_read_u16_le(&fragment[12]);
    header->reserved = playback_board_link_read_u16_le(&fragment[14]);

    result = playback_board_link_header_validate(header);
    if (result != PLAYBACK_BOARD_LINK_WIRE_OK)
    {
        return result;
    }
    if ((size_t)header->payload_length != (fragment_length - PLAYBACK_BOARD_LINK_HEADER_SIZE))
    {
        return PLAYBACK_BOARD_LINK_WIRE_LENGTH_MISMATCH;
    }
    return PLAYBACK_BOARD_LINK_WIRE_OK;
}

playback_board_link_wire_result_t playback_board_link_fragment_plan(
    size_t body_length,
    size_t att_value_capacity,
    size_t *fragment_payload_capacity,
    uint16_t *fragment_count
)
{
    size_t payload_capacity;
    size_t count;

    if ((fragment_payload_capacity == NULL) || (fragment_count == NULL) ||
        (body_length == 0U) || (att_value_capacity <= PLAYBACK_BOARD_LINK_HEADER_SIZE))
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_ARGUMENT;
    }
    if (body_length > PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES)
    {
        return PLAYBACK_BOARD_LINK_WIRE_MESSAGE_TOO_LARGE;
    }

    payload_capacity = att_value_capacity - PLAYBACK_BOARD_LINK_HEADER_SIZE;
    if (payload_capacity > UINT16_MAX)
    {
        payload_capacity = UINT16_MAX;
    }
    count = (body_length + payload_capacity - 1U) / payload_capacity;
    if ((count == 0U) || (count > PLAYBACK_BOARD_LINK_MAX_FRAGMENT_COUNT) || (count > UINT16_MAX))
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_FRAGMENT;
    }

    *fragment_payload_capacity = payload_capacity;
    *fragment_count = (uint16_t)count;
    return PLAYBACK_BOARD_LINK_WIRE_OK;
}

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
)
{
    playback_board_link_fragment_header_t header;
    playback_board_link_wire_result_t result;
    size_t fragment_payload_capacity;
    uint16_t fragment_count;
    size_t payload_offset;
    size_t payload_length;
    size_t fragment_length;

    if ((body == NULL) || (destination == NULL) || (written == NULL) || (message_id == 0U) ||
        !playback_board_link_message_kind_valid(message_kind))
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_ARGUMENT;
    }

    result = playback_board_link_fragment_plan(
        body_length,
        att_value_capacity,
        &fragment_payload_capacity,
        &fragment_count
    );
    if (result != PLAYBACK_BOARD_LINK_WIRE_OK)
    {
        return result;
    }
    if (fragment_index >= fragment_count)
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_FRAGMENT;
    }

    payload_offset = (size_t)fragment_index * fragment_payload_capacity;
    payload_length = body_length - payload_offset;
    if (payload_length > fragment_payload_capacity)
    {
        payload_length = fragment_payload_capacity;
    }
    fragment_length = PLAYBACK_BOARD_LINK_HEADER_SIZE + payload_length;
    if (destination_capacity < fragment_length)
    {
        return PLAYBACK_BOARD_LINK_WIRE_BUFFER_TOO_SMALL;
    }

    memset(&header, 0, sizeof(header));
    header.protocol_major = PLAYBACK_BOARD_LINK_PROTOCOL_MAJOR;
    header.message_kind = message_kind;
    header.message_id = message_id;
    header.fragment_index = fragment_index;
    header.fragment_count = fragment_count;
    header.payload_length = (uint16_t)payload_length;

    result = playback_board_link_fragment_header_encode(&header, destination, destination_capacity);
    if (result != PLAYBACK_BOARD_LINK_WIRE_OK)
    {
        return result;
    }
    memcpy(&destination[PLAYBACK_BOARD_LINK_HEADER_SIZE], &body[payload_offset], payload_length);
    *written = fragment_length;
    return PLAYBACK_BOARD_LINK_WIRE_OK;
}

playback_board_link_wire_result_t playback_board_link_reassembler_init(
    playback_board_link_reassembler_t *reassembler
)
{
    playback_board_link_reassembler_state_t *state;

    if ((reassembler == NULL) || (reassembler->state != NULL))
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_ARGUMENT;
    }

    state = tal_calloc(1U, sizeof(*state));
    if (state == NULL)
    {
        return PLAYBACK_BOARD_LINK_WIRE_NO_MEMORY;
    }
    reassembler->state = state;
    return PLAYBACK_BOARD_LINK_WIRE_OK;
}

playback_board_link_wire_result_t playback_board_link_reassembler_push(
    playback_board_link_reassembler_t *reassembler,
    uint32_t now_ms,
    const uint8_t *fragment,
    size_t fragment_length,
    uint8_t *body_destination,
    size_t body_destination_capacity,
    playback_board_link_message_t *message
)
{
    playback_board_link_reassembler_state_t *state;
    playback_board_link_fragment_header_t header;
    playback_board_link_assembly_t *assembly;
    playback_board_link_fragment_record_t *record;
    playback_board_link_wire_result_t result;
    const uint8_t *payload;
    size_t output_offset;
    uint16_t index;

    if ((reassembler == NULL) || (reassembler->state == NULL) || (fragment == NULL) ||
        (body_destination == NULL) || (body_destination_capacity == 0U) || (message == NULL))
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_ARGUMENT;
    }

    memset(message, 0, sizeof(*message));
    result = playback_board_link_fragment_header_decode(fragment, fragment_length, &header);
    if (result != PLAYBACK_BOARD_LINK_WIRE_OK)
    {
        return result;
    }

    state = reassembler->state;
    assembly = playback_board_link_get_assembly(state, header.message_kind);
    if (assembly == NULL)
    {
        return PLAYBACK_BOARD_LINK_WIRE_INVALID_MESSAGE_KIND;
    }
    if (playback_board_link_assembly_expired(assembly, now_ms))
    {
        playback_board_link_assembly_clear(assembly);
    }

    if (!assembly->active)
    {
        result = playback_board_link_assembly_start(assembly, &header, now_ms);
        if (result != PLAYBACK_BOARD_LINK_WIRE_OK)
        {
            return result;
        }
    }
    else if (assembly->message_id != header.message_id)
    {
        return PLAYBACK_BOARD_LINK_WIRE_ASSEMBLY_CONFLICT;
    }
    else if (assembly->fragment_count != header.fragment_count)
    {
        playback_board_link_assembly_clear(assembly);
        return PLAYBACK_BOARD_LINK_WIRE_ASSEMBLY_CONFLICT;
    }

    payload = &fragment[PLAYBACK_BOARD_LINK_HEADER_SIZE];
    record = &assembly->records[header.fragment_index];
    if (record->length != 0U)
    {
        if ((record->length != header.payload_length) ||
            (memcmp(&assembly->payload_arena[record->offset], payload, header.payload_length) != 0))
        {
            playback_board_link_assembly_clear(assembly);
            return PLAYBACK_BOARD_LINK_WIRE_CONFLICTING_DUPLICATE;
        }
    }
    else
    {
        if ((assembly->used_bytes + header.payload_length) > PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES)
        {
            playback_board_link_assembly_clear(assembly);
            return PLAYBACK_BOARD_LINK_WIRE_MESSAGE_TOO_LARGE;
        }

        record->offset = (uint16_t)assembly->used_bytes;
        record->length = header.payload_length;
        memcpy(&assembly->payload_arena[assembly->used_bytes], payload, header.payload_length);
        assembly->used_bytes += header.payload_length;
        assembly->received_count++;
    }

    if (assembly->received_count != assembly->fragment_count)
    {
        return PLAYBACK_BOARD_LINK_WIRE_INCOMPLETE;
    }
    if (body_destination_capacity <= assembly->used_bytes)
    {
        return PLAYBACK_BOARD_LINK_WIRE_BUFFER_TOO_SMALL;
    }

    output_offset = 0U;
    for (index = 0U; index < assembly->fragment_count; ++index)
    {
        record = &assembly->records[index];
        if (record->length == 0U)
        {
            playback_board_link_assembly_clear(assembly);
            return PLAYBACK_BOARD_LINK_WIRE_INVALID_FRAGMENT;
        }
        memcpy(
            &body_destination[output_offset],
            &assembly->payload_arena[record->offset],
            record->length
        );
        output_offset += record->length;
    }
    body_destination[output_offset] = 0U;

    message->message_kind = assembly->message_kind;
    message->message_id = assembly->message_id;
    message->body_length = output_offset;
    playback_board_link_assembly_clear(assembly);
    return PLAYBACK_BOARD_LINK_WIRE_COMPLETE;
}

size_t playback_board_link_reassembler_expire(
    playback_board_link_reassembler_t *reassembler,
    uint32_t now_ms
)
{
    playback_board_link_reassembler_state_t *state;
    size_t expired = 0U;

    if ((reassembler == NULL) || (reassembler->state == NULL))
    {
        return 0U;
    }

    state = reassembler->state;
    if (playback_board_link_assembly_expired(&state->command, now_ms))
    {
        playback_board_link_assembly_clear(&state->command);
        expired++;
    }
    if (playback_board_link_assembly_expired(&state->report, now_ms))
    {
        playback_board_link_assembly_clear(&state->report);
        expired++;
    }
    return expired;
}

void playback_board_link_reassembler_reset(playback_board_link_reassembler_t *reassembler)
{
    playback_board_link_reassembler_state_t *state;

    if ((reassembler == NULL) || (reassembler->state == NULL))
    {
        return;
    }

    state = reassembler->state;
    playback_board_link_assembly_clear(&state->command);
    playback_board_link_assembly_clear(&state->report);
}

void playback_board_link_reassembler_close(playback_board_link_reassembler_t *reassembler)
{
    if (reassembler == NULL)
    {
        return;
    }
    if (reassembler->state != NULL)
    {
        playback_board_link_reassembler_reset(reassembler);
        tal_free(reassembler->state);
    }
    reassembler->state = NULL;
}

const char *playback_board_link_wire_result_name(playback_board_link_wire_result_t result)
{
    static const char *const names[] = {
        "OK",
        "INCOMPLETE",
        "COMPLETE",
        "INVALID_ARGUMENT",
        "BUFFER_TOO_SMALL",
        "INVALID_MAGIC",
        "UNSUPPORTED_PROTOCOL",
        "INVALID_MESSAGE_KIND",
        "INVALID_FLAGS",
        "INVALID_MESSAGE_ID",
        "INVALID_FRAGMENT",
        "LENGTH_MISMATCH",
        "MESSAGE_TOO_LARGE",
        "NO_MEMORY",
        "ASSEMBLY_CONFLICT",
        "CONFLICTING_DUPLICATE",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0]))) ? names[result]
                                                                      : "UNKNOWN_BOARD_LINK_WIRE_RESULT";
}
