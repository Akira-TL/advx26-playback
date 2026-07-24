#include "playback_board_link_json.h"

#include <stdbool.h>
#include <limits.h>
#include <string.h>

#include "cJSON.h"
#include "tal_api.h"

#include "playback_board_link_wire.h"
#include "playback_media_package.h"

#define PLAYBACK_BOARD_LINK_PROTOCOL_MINOR (0U)
#define PLAYBACK_BOARD_LINK_ROLE_TRIGGER "TRIGGER"
#define PLAYBACK_BOARD_LINK_ROLE_PLAYBACK "PLAYBACK"
#define PLAYBACK_BOARD_LINK_VIDEO_FORMAT "MP4_H264"
#define PLAYBACK_BOARD_LINK_VIDEO_CODEC "H264"
#define PLAYBACK_BOARD_LINK_VIDEO_CODEC_PROFILE "BASELINE"
#define PLAYBACK_BOARD_LINK_AUDIO_FORMAT "MP3_CBR"
#define PLAYBACK_BOARD_LINK_PIXEL_FORMAT "YUV420P"
#define PLAYBACK_BOARD_LINK_END_BEHAVIOR "HOLD_LAST_FRAME"

static bool playback_board_link_utf8_valid(const uint8_t *data, size_t length)
{
    size_t index = 0U;

    while (index < length)
    {
        const uint8_t first = data[index];
        uint32_t codepoint;
        size_t continuation_count;
        size_t continuation_index;

        if (first == 0U)
        {
            return false;
        }
        if (first <= 0x7FU)
        {
            index++;
            continue;
        }
        if ((first >= 0xC2U) && (first <= 0xDFU))
        {
            codepoint = (uint32_t)(first & 0x1FU);
            continuation_count = 1U;
        }
        else if ((first >= 0xE0U) && (first <= 0xEFU))
        {
            codepoint = (uint32_t)(first & 0x0FU);
            continuation_count = 2U;
        }
        else if ((first >= 0xF0U) && (first <= 0xF4U))
        {
            codepoint = (uint32_t)(first & 0x07U);
            continuation_count = 3U;
        }
        else
        {
            return false;
        }

        if ((index + continuation_count) >= length)
        {
            return false;
        }
        for (continuation_index = 1U; continuation_index <= continuation_count; ++continuation_index)
        {
            const uint8_t continuation = data[index + continuation_index];
            if ((continuation & 0xC0U) != 0x80U)
            {
                return false;
            }
            codepoint = (codepoint << 6U) | (uint32_t)(continuation & 0x3FU);
        }

        if (((continuation_count == 1U) && (codepoint < 0x80U)) ||
            ((continuation_count == 2U) && (codepoint < 0x800U)) ||
            ((continuation_count == 3U) && (codepoint < 0x10000U)) ||
            ((codepoint >= 0xD800U) && (codepoint <= 0xDFFFU)) ||
            (codepoint > 0x10FFFFU))
        {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

static bool playback_board_link_json_has_duplicate_keys(const cJSON *item)
{
    const cJSON *child;

    if (cJSON_IsObject(item))
    {
        for (child = item->child; child != NULL; child = child->next)
        {
            const cJSON *other;
            if (child->string == NULL)
            {
                return true;
            }
            for (other = child->next; other != NULL; other = other->next)
            {
                if ((other->string != NULL) && (strcmp(child->string, other->string) == 0))
                {
                    return true;
                }
            }
            if (playback_board_link_json_has_duplicate_keys(child))
            {
                return true;
            }
        }
    }
    else if (cJSON_IsArray(item))
    {
        cJSON_ArrayForEach(child, item)
        {
            if (playback_board_link_json_has_duplicate_keys(child))
            {
                return true;
            }
        }
    }
    return false;
}

static size_t playback_board_link_bounded_string_length(const char *value, size_t capacity)
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

static bool playback_board_link_copy_string(char *destination, size_t capacity, const char *source, bool allow_empty)
{
    size_t length;

    if ((destination == NULL) || (capacity == 0U) || (source == NULL))
    {
        return false;
    }
    length = playback_board_link_bounded_string_length(source, capacity);
    if ((length >= capacity) || (!allow_empty && (length == 0U)))
    {
        return false;
    }
    memcpy(destination, source, length + 1U);
    return true;
}

static bool playback_board_link_json_get_string(
    const cJSON *object,
    const char *name,
    char *destination,
    size_t capacity,
    bool allow_empty
)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

    return cJSON_IsString(item) && (item->valuestring != NULL) &&
           playback_board_link_copy_string(destination, capacity, item->valuestring, allow_empty);
}

static bool playback_board_link_json_get_u32(
    const cJSON *object,
    const char *name,
    uint32_t minimum,
    uint32_t maximum,
    uint32_t *value
)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    double number;
    uint32_t converted;

    if (!cJSON_IsNumber(item) || (value == NULL))
    {
        return false;
    }
    number = item->valuedouble;
    if ((number < (double)minimum) || (number > (double)maximum))
    {
        return false;
    }
    converted = (uint32_t)number;
    if ((double)converted != number)
    {
        return false;
    }
    *value = converted;
    return true;
}

static bool playback_board_link_json_get_u16(
    const cJSON *object,
    const char *name,
    uint16_t minimum,
    uint16_t maximum,
    uint16_t *value
)
{
    uint32_t converted;

    if (!playback_board_link_json_get_u32(object, name, minimum, maximum, &converted))
    {
        return false;
    }
    *value = (uint16_t)converted;
    return true;
}

static bool playback_board_link_json_get_u8(
    const cJSON *object,
    const char *name,
    uint8_t minimum,
    uint8_t maximum,
    uint8_t *value
)
{
    uint32_t converted;

    if (!playback_board_link_json_get_u32(object, name, minimum, maximum, &converted))
    {
        return false;
    }
    *value = (uint8_t)converted;
    return true;
}

static bool playback_board_link_json_get_bool(const cJSON *object, const char *name, bool *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

    if (!cJSON_IsBool(item) || (value == NULL))
    {
        return false;
    }
    *value = cJSON_IsTrue(item);
    return true;
}

static bool playback_board_link_json_string_equals(const cJSON *object, const char *name, const char *expected)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

    return cJSON_IsString(item) && (item->valuestring != NULL) && (strcmp(item->valuestring, expected) == 0);
}

static bool playback_board_link_json_parse_nullable_session_id(const cJSON *root, char *session_id)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "session_id");

    if (cJSON_IsNull(item))
    {
        session_id[0] = '\0';
        return true;
    }
    if (!cJSON_IsString(item) || (item->valuestring == NULL))
    {
        return false;
    }
    return playback_board_link_copy_string(
        session_id,
        PLAYBACK_SESSION_ID_MAX_LEN + 1U,
        item->valuestring,
        false
    );
}

static bool playback_board_link_json_parse_command_kind(const char *name, playback_command_kind_t *kind)
{
    playback_command_kind_t candidate;

    if ((name == NULL) || (kind == NULL))
    {
        return false;
    }
    for (candidate = PLAYBACK_COMMAND_HELLO; candidate <= PLAYBACK_COMMAND_STOP; ++candidate)
    {
        if (strcmp(name, playback_command_name(candidate)) == 0)
        {
            *kind = candidate;
            return true;
        }
    }
    return false;
}

static bool playback_board_link_json_parse_asset(const cJSON *object, playback_asset_t *asset)
{
    return cJSON_IsObject(object) &&
           playback_board_link_json_get_string(
               object,
               "url",
               asset->url,
               sizeof(asset->url),
               false
           ) &&
           playback_board_link_json_get_u32(object, "byte_length", 1U, UINT32_MAX, &asset->byte_length) &&
           playback_board_link_json_get_string(
               object,
               "sha256",
               asset->sha256,
               sizeof(asset->sha256),
               false
           ) &&
           playback_board_link_json_get_string(
               object,
               "etag",
               asset->etag,
               sizeof(asset->etag),
               false
           );
}

static bool playback_board_link_json_parse_video(
    const cJSON *object,
    playback_video_descriptor_t *video
)
{
    if (!cJSON_IsObject(object) ||
        !playback_board_link_json_string_equals(object, "format", PLAYBACK_BOARD_LINK_VIDEO_FORMAT) ||
        !playback_board_link_json_string_equals(object, "codec", PLAYBACK_BOARD_LINK_VIDEO_CODEC) ||
        !playback_board_link_json_string_equals(
            object,
            "codec_profile",
            PLAYBACK_BOARD_LINK_VIDEO_CODEC_PROFILE
        ) ||
        !playback_board_link_json_string_equals(object, "pixel_format", PLAYBACK_BOARD_LINK_PIXEL_FORMAT) ||
        !playback_board_link_json_parse_asset(object, &video->asset) ||
        !playback_board_link_json_get_u16(object, "width", 1U, UINT16_MAX, &video->width) ||
        !playback_board_link_json_get_u16(object, "height", 1U, UINT16_MAX, &video->height) ||
        !playback_board_link_json_get_u16(object, "fps", 1U, UINT16_MAX, &video->fps_num) ||
        !playback_board_link_json_get_u16(
            object,
            "max_keyframe_interval_ms",
            1U,
            UINT16_MAX,
            &video->max_keyframe_interval_ms
        ))
    {
        return false;
    }

    video->fps_den = 1U;
    video->h264_profile_idc = PLAYBACK_H264_BASELINE_PROFILE_IDC;
    video->h264_level_idc = 0U;
    video->yuv420p = true;
    video->has_b_frames = false;
    return true;
}

static bool playback_board_link_json_parse_audio(
    const cJSON *object,
    playback_audio_descriptor_t *audio
)
{
    if (!cJSON_IsObject(object) ||
        !playback_board_link_json_string_equals(object, "format", PLAYBACK_BOARD_LINK_AUDIO_FORMAT) ||
        !playback_board_link_json_parse_asset(object, &audio->asset) ||
        !playback_board_link_json_get_string(
            object,
            "index_url",
            audio->index_asset.url,
            sizeof(audio->index_asset.url),
            false
        ) ||
        !playback_board_link_json_get_u32(
            object,
            "index_byte_length",
            1U,
            UINT32_MAX,
            &audio->index_asset.byte_length
        ) ||
        !playback_board_link_json_get_string(
            object,
            "index_sha256",
            audio->index_asset.sha256,
            sizeof(audio->index_asset.sha256),
            false
        ) ||
        !playback_board_link_json_get_string(
            object,
            "index_etag",
            audio->index_asset.etag,
            sizeof(audio->index_asset.etag),
            false
        ))
    {
        return false;
    }

    return playback_board_link_json_get_u32(object, "sample_rate", 1U, UINT32_MAX, &audio->sample_rate) &&
           playback_board_link_json_get_u16(object, "bitrate_kbps", 1U, UINT16_MAX, &audio->bitrate_kbps) &&
           playback_board_link_json_get_u8(object, "channels", 1U, UINT8_MAX, &audio->channels) &&
           playback_board_link_json_get_u8(object, "index_version", 1U, UINT8_MAX, &audio->index_version);
}

static playback_board_link_json_result_t playback_board_link_json_parse_hello(
    const cJSON *payload,
    playback_hello_t *hello
)
{
    const cJSON *capabilities;
    const cJSON *capability;
    const cJSON *role;
    uint32_t protocol_major;
    uint32_t protocol_minor;
    uint32_t max_message_bytes;
    int capability_count;
    int index;

    if (!playback_board_link_json_get_u32(payload, "protocol_major", 0U, UINT16_MAX, &protocol_major) ||
        !playback_board_link_json_get_u32(payload, "protocol_minor", 0U, UINT16_MAX, &protocol_minor))
    {
        return PLAYBACK_BOARD_LINK_JSON_MALFORMED;
    }
    role = cJSON_GetObjectItemCaseSensitive(payload, "role");
    if (!cJSON_IsString(role) || (role->valuestring == NULL))
    {
        return PLAYBACK_BOARD_LINK_JSON_MALFORMED;
    }
    if ((strcmp(role->valuestring, PLAYBACK_BOARD_LINK_ROLE_TRIGGER) != 0) ||
        (protocol_major != PLAYBACK_BOARD_LINK_PROTOCOL_MAJOR))
    {
        return PLAYBACK_BOARD_LINK_JSON_PROTOCOL_INCOMPATIBLE;
    }
    if (!playback_board_link_json_get_string(
            payload,
            "boot_id",
            hello->boot_id,
            sizeof(hello->boot_id),
            false
        ) ||
        !playback_board_link_json_get_u32(
            payload,
            "max_message_bytes",
            1U,
            PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES,
            &max_message_bytes
        ))
    {
        return PLAYBACK_BOARD_LINK_JSON_MALFORMED;
    }

    capabilities = cJSON_GetObjectItemCaseSensitive(payload, "capabilities");
    if (!cJSON_IsArray(capabilities))
    {
        return PLAYBACK_BOARD_LINK_JSON_MALFORMED;
    }
    capability_count = cJSON_GetArraySize(capabilities);
    if ((capability_count < 0) || (capability_count > (int)PLAYBACK_CAPABILITY_COUNT_MAX))
    {
        return PLAYBACK_BOARD_LINK_JSON_MALFORMED;
    }

    hello->protocol_major = (uint16_t)protocol_major;
    hello->protocol_minor = (uint16_t)protocol_minor;
    hello->max_message_bytes = (uint16_t)max_message_bytes;
    hello->capability_count = (uint8_t)capability_count;
    for (index = 0; index < capability_count; ++index)
    {
        capability = cJSON_GetArrayItem(capabilities, index);
        if (!cJSON_IsString(capability) || (capability->valuestring == NULL) ||
            !playback_board_link_copy_string(
                hello->capabilities[index],
                sizeof(hello->capabilities[index]),
                capability->valuestring,
                false
            ))
        {
            return PLAYBACK_BOARD_LINK_JSON_MALFORMED;
        }
    }
    return PLAYBACK_BOARD_LINK_JSON_OK;
}

static playback_board_link_json_result_t playback_board_link_json_parse_session(
    const cJSON *payload,
    playback_session_t *session
)
{
    const cJSON *video;
    const cJSON *audio;
    const cJSON *autoplay;
    const cJSON *end_behavior;
    playback_media_package_t *package;
    playback_package_result_t package_result;

    if (!playback_board_link_json_get_string(
            payload,
            "content_id",
            session->content_id,
            sizeof(session->content_id),
            false
        ) ||
        !playback_board_link_json_get_u32(payload, "revision", 1U, UINT32_MAX, &session->revision) ||
        !playback_board_link_json_get_u32(
            payload,
            "duration_ms",
            1U,
            PLAYBACK_MAX_DURATION_MS,
            &session->duration_ms
        ) ||
        !playback_board_link_json_get_string(
            payload,
            "profile",
            session->profile,
            sizeof(session->profile),
            false
        ))
    {
        return PLAYBACK_BOARD_LINK_JSON_MALFORMED;
    }

    video = cJSON_GetObjectItemCaseSensitive(payload, "video");
    audio = cJSON_GetObjectItemCaseSensitive(payload, "audio");
    if (!playback_board_link_json_parse_video(video, &session->video) ||
        !playback_board_link_json_parse_audio(audio, &session->audio))
    {
        return PLAYBACK_BOARD_LINK_JSON_MALFORMED;
    }

    session->autoplay = true;
    autoplay = cJSON_GetObjectItemCaseSensitive(payload, "autoplay");
    if ((autoplay != NULL) && !playback_board_link_json_get_bool(payload, "autoplay", &session->autoplay))
    {
        return PLAYBACK_BOARD_LINK_JSON_MALFORMED;
    }

    session->end_behavior = PLAYBACK_END_HOLD_LAST_FRAME;
    end_behavior = cJSON_GetObjectItemCaseSensitive(payload, "end_behavior");
    if ((end_behavior != NULL) &&
        !playback_board_link_json_string_equals(payload, "end_behavior", PLAYBACK_BOARD_LINK_END_BEHAVIOR))
    {
        return PLAYBACK_BOARD_LINK_JSON_MALFORMED;
    }

    package = tal_malloc(sizeof(*package));
    if (package == NULL)
    {
        return PLAYBACK_BOARD_LINK_JSON_NO_MEMORY;
    }
    package_result = playback_media_package_validate(session, package);
    tal_free(package);

    if (package_result == PLAYBACK_PACKAGE_UNSUPPORTED_PROFILE)
    {
        return PLAYBACK_BOARD_LINK_JSON_UNSUPPORTED_PROFILE;
    }
    if (package_result != PLAYBACK_PACKAGE_OK)
    {
        return PLAYBACK_BOARD_LINK_JSON_MALFORMED;
    }
    return PLAYBACK_BOARD_LINK_JSON_OK;
}

playback_board_link_json_result_t playback_board_link_command_parse(
    const uint8_t *body,
    size_t body_length,
    playback_command_t *command
)
{
    char *json;
    const char *parse_end = NULL;
    cJSON *root;
    const cJSON *schema_version;
    const cJSON *type;
    const cJSON *payload;
    playback_board_link_json_result_t result = PLAYBACK_BOARD_LINK_JSON_MALFORMED;
    uint32_t schema;

    if ((body == NULL) || (command == NULL) || (body_length == 0U))
    {
        return PLAYBACK_BOARD_LINK_JSON_INVALID_ARGUMENT;
    }
    if (body_length > PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES)
    {
        return PLAYBACK_BOARD_LINK_JSON_MESSAGE_TOO_LARGE;
    }
    if (!playback_board_link_utf8_valid(body, body_length))
    {
        return PLAYBACK_BOARD_LINK_JSON_MALFORMED;
    }

    json = tal_malloc(body_length + 1U);
    if (json == NULL)
    {
        return PLAYBACK_BOARD_LINK_JSON_NO_MEMORY;
    }
    memcpy(json, body, body_length);
    json[body_length] = '\0';

    root = cJSON_ParseWithLengthOpts(json, body_length + 1U, &parse_end, true);
    if ((root == NULL) || (parse_end == NULL) || ((size_t)(parse_end - json) > body_length) ||
        !cJSON_IsObject(root) || playback_board_link_json_has_duplicate_keys(root))
    {
        cJSON_Delete(root);
        tal_free(json);
        return PLAYBACK_BOARD_LINK_JSON_MALFORMED;
    }

    memset(command, 0, sizeof(*command));
    schema_version = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    payload = cJSON_GetObjectItemCaseSensitive(root, "payload");

    if (!cJSON_IsNumber(schema_version) ||
        !playback_board_link_json_get_u32(root, "schema_version", 0U, UINT32_MAX, &schema))
    {
        goto cleanup;
    }
    if (schema != PLAYBACK_PROTOCOL_SCHEMA_VERSION)
    {
        result = PLAYBACK_BOARD_LINK_JSON_PROTOCOL_INCOMPATIBLE;
        goto cleanup;
    }
    if (!cJSON_IsString(type) || (type->valuestring == NULL))
    {
        goto cleanup;
    }
    if (!playback_board_link_json_parse_command_kind(type->valuestring, &command->kind))
    {
        result = PLAYBACK_BOARD_LINK_JSON_UNKNOWN_TYPE;
        goto cleanup;
    }
    if (!playback_board_link_json_parse_nullable_session_id(root, command->session_id) ||
        !playback_board_link_json_get_u32(root, "sequence_id", 0U, UINT32_MAX, &command->sequence_id) ||
        !cJSON_IsObject(payload))
    {
        goto cleanup;
    }

    command->schema_version = schema;
    switch (command->kind)
    {
        case PLAYBACK_COMMAND_HELLO:
            if (command->session_id[0] != '\0')
            {
                break;
            }
            result = playback_board_link_json_parse_hello(payload, &command->payload.hello);
            break;

        case PLAYBACK_COMMAND_GET_STATUS:
            result = (command->session_id[0] == '\0') ? PLAYBACK_BOARD_LINK_JSON_OK
                                                       : PLAYBACK_BOARD_LINK_JSON_MALFORMED;
            break;

        case PLAYBACK_COMMAND_LOAD_SESSION:
            if (command->session_id[0] == '\0')
            {
                break;
            }
            if (!playback_board_link_copy_string(
                    command->payload.session.session_id,
                    sizeof(command->payload.session.session_id),
                    command->session_id,
                    false
                ))
            {
                break;
            }
            result = playback_board_link_json_parse_session(payload, &command->payload.session);
            break;

        case PLAYBACK_COMMAND_SEEK_MS:
            if ((command->session_id[0] != '\0') &&
                playback_board_link_json_get_u32(
                    payload,
                    "position_ms",
                    0U,
                    PLAYBACK_MAX_DURATION_MS,
                    &command->payload.seek_position_ms
                ))
            {
                result = PLAYBACK_BOARD_LINK_JSON_OK;
            }
            break;

        case PLAYBACK_COMMAND_PLAY:
        case PLAYBACK_COMMAND_PAUSE:
        case PLAYBACK_COMMAND_STOP:
            result = (command->session_id[0] != '\0') ? PLAYBACK_BOARD_LINK_JSON_OK
                                                       : PLAYBACK_BOARD_LINK_JSON_MALFORMED;
            break;

        default:
            result = PLAYBACK_BOARD_LINK_JSON_UNKNOWN_TYPE;
            break;
    }

cleanup:
    if (result != PLAYBACK_BOARD_LINK_JSON_OK)
    {
        memset(command, 0, sizeof(*command));
    }
    cJSON_Delete(root);
    tal_free(json);
    return result;
}

static bool playback_board_link_json_add_string(cJSON *object, const char *name, const char *value)
{
    return cJSON_AddStringToObject(object, name, value) != NULL;
}

static bool playback_board_link_json_add_number(cJSON *object, const char *name, uint32_t value)
{
    return cJSON_AddNumberToObject(object, name, (double)value) != NULL;
}

static bool playback_board_link_json_add_bool(cJSON *object, const char *name, bool value)
{
    return cJSON_AddBoolToObject(object, name, value) != NULL;
}

static bool playback_board_link_json_add_nullable_session(cJSON *root, const char *session_id)
{
    if (session_id[0] == '\0')
    {
        return cJSON_AddNullToObject(root, "session_id") != NULL;
    }
    if (playback_board_link_bounded_string_length(session_id, PLAYBACK_SESSION_ID_MAX_LEN + 1U) >
        PLAYBACK_SESSION_ID_MAX_LEN)
    {
        return false;
    }
    return playback_board_link_json_add_string(root, "session_id", session_id);
}

static bool playback_board_link_json_add_hello_payload(cJSON *payload, const playback_hello_t *hello)
{
    cJSON *capabilities;
    uint8_t index;

    if ((hello == NULL) || (hello->protocol_major != PLAYBACK_BOARD_LINK_PROTOCOL_MAJOR) ||
        (hello->capability_count > PLAYBACK_CAPABILITY_COUNT_MAX) ||
        (hello->max_message_bytes == 0U) ||
        (hello->max_message_bytes > PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES) ||
        (playback_board_link_bounded_string_length(hello->boot_id, sizeof(hello->boot_id)) == 0U) ||
        (playback_board_link_bounded_string_length(hello->boot_id, sizeof(hello->boot_id)) >=
         sizeof(hello->boot_id)))
    {
        return false;
    }

    if (!playback_board_link_json_add_number(payload, "protocol_major", hello->protocol_major) ||
        !playback_board_link_json_add_number(payload, "protocol_minor", hello->protocol_minor) ||
        !playback_board_link_json_add_string(payload, "role", PLAYBACK_BOARD_LINK_ROLE_PLAYBACK) ||
        !playback_board_link_json_add_string(payload, "boot_id", hello->boot_id) ||
        !playback_board_link_json_add_number(payload, "max_message_bytes", hello->max_message_bytes))
    {
        return false;
    }

    capabilities = cJSON_AddArrayToObject(payload, "capabilities");
    if (capabilities == NULL)
    {
        return false;
    }
    for (index = 0U; index < hello->capability_count; ++index)
    {
        cJSON *capability;
        const size_t length = playback_board_link_bounded_string_length(
            hello->capabilities[index],
            sizeof(hello->capabilities[index])
        );
        if ((length == 0U) || (length >= sizeof(hello->capabilities[index])))
        {
            return false;
        }
        capability = cJSON_CreateString(hello->capabilities[index]);
        if ((capability == NULL) || !cJSON_AddItemToArray(capabilities, capability))
        {
            cJSON_Delete(capability);
            return false;
        }
    }
    return true;
}

static bool playback_board_link_report_kind_valid(playback_report_kind_t kind)
{
    return (unsigned int)kind <= (unsigned int)PLAYBACK_REPORT_ERROR;
}

static bool playback_board_link_command_kind_valid(playback_command_kind_t kind)
{
    return (unsigned int)kind <= (unsigned int)PLAYBACK_COMMAND_STOP;
}

static bool playback_board_link_nack_valid(playback_nack_t nack)
{
    return (nack > PLAYBACK_NACK_NONE) && (nack <= PLAYBACK_NACK_PROTOCOL_INCOMPATIBLE);
}

static bool playback_board_link_state_valid(playback_state_t state)
{
    return (unsigned int)state <= (unsigned int)PLAYBACK_STATE_ERROR;
}

static bool playback_board_link_intent_valid(playback_intent_t intent)
{
    return (intent == PLAYBACK_INTENT_PAUSED) || (intent == PLAYBACK_INTENT_PLAYING);
}

static const char *playback_board_link_intent_name(playback_intent_t intent)
{
    return (intent == PLAYBACK_INTENT_PLAYING) ? "PLAYING" : "PAUSED";
}

static bool playback_board_link_error_valid(playback_error_t error)
{
    return (error > PLAYBACK_ERROR_NONE) && (error <= PLAYBACK_ERROR_INTERNAL);
}

static bool playback_board_link_json_add_diagnostic(cJSON *payload, const char *diagnostic)
{
    const size_t length = playback_board_link_bounded_string_length(
        diagnostic,
        PLAYBACK_DIAGNOSTIC_MAX_LEN + 1U
    );

    if (length > PLAYBACK_DIAGNOSTIC_MAX_LEN)
    {
        return false;
    }
    return (length == 0U) || playback_board_link_json_add_string(payload, "diagnostic", diagnostic);
}

static bool playback_board_link_hello_valid(const playback_hello_t *hello)
{
    uint8_t index;
    size_t length;

    if ((hello == NULL) || (hello->protocol_major != PLAYBACK_BOARD_LINK_PROTOCOL_MAJOR) ||
        (hello->capability_count > PLAYBACK_CAPABILITY_COUNT_MAX) ||
        (hello->max_message_bytes == 0U) ||
        (hello->max_message_bytes > PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES))
    {
        return false;
    }

    length = playback_board_link_bounded_string_length(hello->boot_id, sizeof(hello->boot_id));
    if ((length == 0U) || (length >= sizeof(hello->boot_id)))
    {
        return false;
    }
    for (index = 0U; index < hello->capability_count; ++index)
    {
        length = playback_board_link_bounded_string_length(
            hello->capabilities[index],
            sizeof(hello->capabilities[index])
        );
        if ((length == 0U) || (length >= sizeof(hello->capabilities[index])))
        {
            return false;
        }
    }
    return true;
}

static bool playback_board_link_report_valid(
    const playback_report_t *report,
    const playback_hello_t *hello_ack
)
{
    size_t session_length;
    size_t diagnostic_length;

    if ((report == NULL) || !playback_board_link_report_kind_valid(report->kind))
    {
        return false;
    }
    session_length = playback_board_link_bounded_string_length(
        report->session_id,
        sizeof(report->session_id)
    );
    diagnostic_length = playback_board_link_bounded_string_length(
        report->diagnostic,
        sizeof(report->diagnostic)
    );
    if ((session_length >= sizeof(report->session_id)) ||
        (diagnostic_length >= sizeof(report->diagnostic)))
    {
        return false;
    }

    switch (report->kind)
    {
        case PLAYBACK_REPORT_HELLO_ACK:
            return (session_length == 0U) && playback_board_link_hello_valid(hello_ack);
        case PLAYBACK_REPORT_ACK:
            return playback_board_link_command_kind_valid(report->acknowledged_command);
        case PLAYBACK_REPORT_NACK:
            return playback_board_link_nack_valid(report->nack);
        case PLAYBACK_REPORT_STATE:
            return playback_board_link_state_valid(report->state) &&
                   playback_board_link_intent_valid(report->intent);
        case PLAYBACK_REPORT_PROGRESS:
        case PLAYBACK_REPORT_COMPLETED:
            return true;
        case PLAYBACK_REPORT_ERROR:
            return playback_board_link_error_valid(report->error);
        default:
            return false;
    }
}

static bool playback_board_link_json_build_report_payload(
    const playback_report_t *report,
    const playback_hello_t *hello_ack,
    cJSON *payload
)
{
    switch (report->kind)
    {
        case PLAYBACK_REPORT_HELLO_ACK:
            return playback_board_link_json_add_hello_payload(payload, hello_ack);

        case PLAYBACK_REPORT_ACK:
            return playback_board_link_command_kind_valid(report->acknowledged_command) &&
                   playback_board_link_json_add_string(
                       payload,
                       "command",
                       playback_command_name(report->acknowledged_command)
                   );

        case PLAYBACK_REPORT_NACK:
            return playback_board_link_nack_valid(report->nack) &&
                   playback_board_link_json_add_string(payload, "code", playback_nack_name(report->nack)) &&
                   playback_board_link_json_add_diagnostic(payload, report->diagnostic);

        case PLAYBACK_REPORT_STATE:
            return playback_board_link_state_valid(report->state) &&
                   playback_board_link_intent_valid(report->intent) &&
                   playback_board_link_json_add_string(payload, "state", playback_state_name(report->state)) &&
                   playback_board_link_json_add_number(payload, "position_ms", report->position_ms) &&
                   playback_board_link_json_add_number(payload, "duration_ms", report->duration_ms) &&
                   playback_board_link_json_add_string(
                       payload,
                       "intent",
                       playback_board_link_intent_name(report->intent)
                   );

        case PLAYBACK_REPORT_PROGRESS:
        case PLAYBACK_REPORT_COMPLETED:
            return playback_board_link_json_add_number(payload, "position_ms", report->position_ms) &&
                   playback_board_link_json_add_number(payload, "duration_ms", report->duration_ms);

        case PLAYBACK_REPORT_ERROR:
            return playback_board_link_error_valid(report->error) &&
                   playback_board_link_json_add_string(payload, "code", playback_error_name(report->error)) &&
                   playback_board_link_json_add_bool(payload, "retryable", report->retryable) &&
                   playback_board_link_json_add_number(payload, "position_ms", report->position_ms) &&
                   playback_board_link_json_add_diagnostic(payload, report->diagnostic);

        default:
            return false;
    }
}

playback_board_link_json_result_t playback_board_link_report_serialize(
    const playback_report_t *report,
    const playback_hello_t *hello_ack,
    char *destination,
    size_t destination_capacity,
    size_t *written
)
{
    cJSON *root;
    cJSON *payload;
    size_t length;
    bool built;
    bool payload_attached = false;

    if ((report == NULL) || (destination == NULL) || (written == NULL) || (destination_capacity == 0U))
    {
        return PLAYBACK_BOARD_LINK_JSON_INVALID_ARGUMENT;
    }
    *written = 0U;
    destination[0] = '\0';
    if (!playback_board_link_report_valid(report, hello_ack))
    {
        return PLAYBACK_BOARD_LINK_JSON_MALFORMED;
    }
    if (destination_capacity > (size_t)INT_MAX)
    {
        return PLAYBACK_BOARD_LINK_JSON_INVALID_ARGUMENT;
    }

    root = cJSON_CreateObject();
    if (root == NULL)
    {
        return PLAYBACK_BOARD_LINK_JSON_NO_MEMORY;
    }
    payload = cJSON_CreateObject();
    if (payload == NULL)
    {
        cJSON_Delete(root);
        return PLAYBACK_BOARD_LINK_JSON_NO_MEMORY;
    }

    built = playback_board_link_json_add_number(root, "schema_version", PLAYBACK_PROTOCOL_SCHEMA_VERSION) &&
            playback_board_link_json_add_string(root, "type", playback_report_name(report->kind)) &&
            playback_board_link_json_add_nullable_session(root, report->session_id) &&
            playback_board_link_json_add_number(root, "sequence_id", report->sequence_id);
    if (built)
    {
        payload_attached = cJSON_AddItemToObject(root, "payload", payload);
        built = payload_attached && playback_board_link_json_build_report_payload(report, hello_ack, payload);
    }
    if (!built)
    {
        if (!payload_attached)
        {
            cJSON_Delete(payload);
        }
        cJSON_Delete(root);
        return PLAYBACK_BOARD_LINK_JSON_NO_MEMORY;
    }

    if (!cJSON_PrintPreallocated(root, destination, (int)destination_capacity, false))
    {
        cJSON_Delete(root);
        destination[0] = '\0';
        return PLAYBACK_BOARD_LINK_JSON_BUFFER_TOO_SMALL;
    }
    length = strlen(destination);
    cJSON_Delete(root);

    if (length > PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES)
    {
        destination[0] = '\0';
        return PLAYBACK_BOARD_LINK_JSON_MESSAGE_TOO_LARGE;
    }
    *written = length;
    return PLAYBACK_BOARD_LINK_JSON_OK;
}

playback_nack_t playback_board_link_json_result_to_nack(playback_board_link_json_result_t result)
{
    switch (result)
    {
        case PLAYBACK_BOARD_LINK_JSON_OK:
            return PLAYBACK_NACK_NONE;
        case PLAYBACK_BOARD_LINK_JSON_UNKNOWN_TYPE:
            return PLAYBACK_NACK_UNKNOWN_TYPE;
        case PLAYBACK_BOARD_LINK_JSON_UNSUPPORTED_PROFILE:
            return PLAYBACK_NACK_UNSUPPORTED_PROFILE;
        case PLAYBACK_BOARD_LINK_JSON_PROTOCOL_INCOMPATIBLE:
            return PLAYBACK_NACK_PROTOCOL_INCOMPATIBLE;
        case PLAYBACK_BOARD_LINK_JSON_INVALID_ARGUMENT:
        case PLAYBACK_BOARD_LINK_JSON_MALFORMED:
        case PLAYBACK_BOARD_LINK_JSON_MESSAGE_TOO_LARGE:
        case PLAYBACK_BOARD_LINK_JSON_BUFFER_TOO_SMALL:
        case PLAYBACK_BOARD_LINK_JSON_NO_MEMORY:
        default:
            return PLAYBACK_NACK_MALFORMED_MESSAGE;
    }
}

const char *playback_board_link_json_result_name(playback_board_link_json_result_t result)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "MALFORMED",
        "UNKNOWN_TYPE",
        "UNSUPPORTED_PROFILE",
        "PROTOCOL_INCOMPATIBLE",
        "MESSAGE_TOO_LARGE",
        "BUFFER_TOO_SMALL",
        "NO_MEMORY",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0]))) ? names[result]
                                                                      : "UNKNOWN_BOARD_LINK_JSON_RESULT";
}
