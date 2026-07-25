/**
 * @file playback_media_package.c
 * @brief Validation for the constrained MP4/H.264 and MP3 Media Package.
 */

#include "playback_media_package.h"

#include <stdbool.h>
#include <string.h>

static uint16_t playback_read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static uint32_t playback_read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) | ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static bool playback_string_valid(const char *value, size_t capacity, bool allow_empty)
{
    size_t length;

    if ((value == NULL) || (capacity == 0U))
    {
        return false;
    }

    length = strnlen(value, capacity);
    if (length >= capacity)
    {
        return false;
    }

    return allow_empty || (length > 0U);
}

static bool playback_url_valid(const char *url)
{
    if (!playback_string_valid(url, PLAYBACK_URL_MAX_LEN + 1U, false))
    {
        return false;
    }

    return (strncmp(url, "http://", 7U) == 0) || (strncmp(url, "https://", 8U) == 0);
}

static bool playback_sha256_valid(const char *sha256)
{
    size_t index;

    if (!playback_string_valid(sha256, PLAYBACK_SHA256_HEX_LEN + 1U, false) ||
        (strlen(sha256) != PLAYBACK_SHA256_HEX_LEN))
    {
        return false;
    }

    for (index = 0U; index < PLAYBACK_SHA256_HEX_LEN; ++index)
    {
        const char value = sha256[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
        {
            return false;
        }
    }

    return true;
}

static bool playback_etag_valid(const char *etag)
{
    if (!playback_string_valid(etag, PLAYBACK_ETAG_MAX_LEN + 1U, false))
    {
        return false;
    }

    return strncmp(etag, "W/", 2U) != 0;
}

static bool playback_asset_valid(const playback_asset_t *asset)
{
    if (asset == NULL)
    {
        return false;
    }

    return playback_url_valid(asset->url) && (asset->byte_length > 0U) && playback_sha256_valid(asset->sha256) &&
           playback_etag_valid(asset->etag);
}

static playback_package_result_t playback_validate_session_identity(const playback_session_t *session)
{
    if (!playback_string_valid(session->session_id, sizeof(session->session_id), false) ||
        !playback_string_valid(session->content_id, sizeof(session->content_id), false) || (session->revision == 0U) ||
        (session->duration_ms == 0U) || (session->duration_ms > PLAYBACK_MAX_DURATION_MS) ||
        (session->end_behavior != PLAYBACK_END_HOLD_LAST_FRAME))
    {
        return PLAYBACK_PACKAGE_INVALID_SESSION;
    }

    if (!playback_session_profile_supported(session))
    {
        return PLAYBACK_PACKAGE_UNSUPPORTED_PROFILE;
    }

    return PLAYBACK_PACKAGE_OK;
}

static playback_package_result_t playback_validate_video(const playback_video_descriptor_t *video)
{
    uint32_t rounded_fps;

    if (!playback_asset_valid(&video->asset))
    {
        return PLAYBACK_PACKAGE_INVALID_ASSET;
    }
    if ((video->width != PLAYBACK_VIDEO_WIDTH) || (video->height != PLAYBACK_VIDEO_HEIGHT) ||
        (video->fps_num == 0U) || (video->fps_den == 0U))
    {
        return PLAYBACK_PACKAGE_INVALID_VIDEO;
    }

    rounded_fps = ((uint32_t)video->fps_num + (uint32_t)video->fps_den - 1U) / (uint32_t)video->fps_den;
    if ((rounded_fps == 0U) || (rounded_fps > PLAYBACK_VIDEO_MAX_FPS) ||
        (video->h264_profile_idc != PLAYBACK_H264_BASELINE_PROFILE_IDC) || !video->yuv420p || video->has_b_frames ||
        (video->max_keyframe_interval_ms == 0U) ||
        (video->max_keyframe_interval_ms > PLAYBACK_H264_MAX_KEYFRAME_INTERVAL_MS))
    {
        return PLAYBACK_PACKAGE_INVALID_VIDEO;
    }

    return PLAYBACK_PACKAGE_OK;
}

static playback_package_result_t playback_validate_audio(const playback_audio_descriptor_t *audio)
{
    if (!playback_asset_valid(&audio->asset) || !playback_asset_valid(&audio->index_asset))
    {
        return PLAYBACK_PACKAGE_INVALID_ASSET;
    }
    if ((audio->sample_rate != PLAYBACK_MP3_SAMPLE_RATE) || (audio->bitrate_kbps != PLAYBACK_MP3_BITRATE_KBPS) ||
        (audio->channels == 0U) || (audio->channels > 2U) ||
        (audio->index_version != PLAYBACK_AUDIO_INDEX_VERSION))
    {
        return PLAYBACK_PACKAGE_INVALID_AUDIO;
    }

    return PLAYBACK_PACKAGE_OK;
}

playback_package_result_t playback_media_package_validate(
    const playback_session_t *session,
    playback_media_package_t *package
)
{
    playback_package_result_t result;

    if ((session == NULL) || (package == NULL))
    {
        return PLAYBACK_PACKAGE_INVALID_ARGUMENT;
    }

    result = playback_validate_session_identity(session);
    if (result != PLAYBACK_PACKAGE_OK)
    {
        return result;
    }

    result = playback_validate_video(&session->video);
    if (result != PLAYBACK_PACKAGE_OK)
    {
        return result;
    }

    result = playback_validate_audio(&session->audio);
    if (result != PLAYBACK_PACKAGE_OK)
    {
        return result;
    }

    memset(package, 0, sizeof(*package));
    package->session = *session;
    package->expected_pcm_samples = ((uint64_t)session->duration_ms * (uint64_t)session->audio.sample_rate) / 1000ULL;
    return PLAYBACK_PACKAGE_OK;
}

playback_package_result_t playback_audio_index_parse(
    const uint8_t *payload,
    size_t payload_length,
    uint32_t audio_byte_length,
    uint32_t sample_rate,
    uint32_t duration_ms,
    playback_audio_index_t *index
)
{
    uint16_t version;
    uint16_t record_size;
    uint32_t record_count;
    uint32_t reserved;
    uint64_t expected_samples;
    uint64_t previous_end = 0ULL;
    uint32_t previous_sample = 0U;
    size_t record_index;

    if ((payload == NULL) || (index == NULL) || (index->records == NULL) || (index->capacity == 0U) ||
        (audio_byte_length == 0U) || (sample_rate == 0U) || (duration_ms == 0U))
    {
        return PLAYBACK_PACKAGE_INVALID_ARGUMENT;
    }
    if (payload_length < PLAYBACK_AUDIO_INDEX_HEADER_SIZE)
    {
        return PLAYBACK_PACKAGE_INVALID_INDEX;
    }

    if (playback_read_le32(payload) != PLAYBACK_AUDIO_INDEX_MAGIC)
    {
        return PLAYBACK_PACKAGE_INVALID_INDEX;
    }

    version = playback_read_le16(payload + 4U);
    record_size = playback_read_le16(payload + 6U);
    record_count = playback_read_le32(payload + 8U);
    reserved = playback_read_le32(payload + 12U);

    if ((version != PLAYBACK_AUDIO_INDEX_VERSION) || (record_size != PLAYBACK_AUDIO_INDEX_RECORD_SIZE) ||
        (record_count == 0U) || (record_count > PLAYBACK_AUDIO_INDEX_MAX_RECORDS) || (reserved != 0U))
    {
        return PLAYBACK_PACKAGE_INVALID_INDEX;
    }
    if ((size_t)record_count > index->capacity)
    {
        return PLAYBACK_PACKAGE_INDEX_CAPACITY;
    }
    if (payload_length != (PLAYBACK_AUDIO_INDEX_HEADER_SIZE + ((size_t)record_count * PLAYBACK_AUDIO_INDEX_RECORD_SIZE)))
    {
        return PLAYBACK_PACKAGE_INVALID_INDEX;
    }

    expected_samples = ((uint64_t)duration_ms * (uint64_t)sample_rate) / 1000ULL;
    for (record_index = 0U; record_index < (size_t)record_count; ++record_index)
    {
        const uint8_t *record_data = payload + PLAYBACK_AUDIO_INDEX_HEADER_SIZE +
                                     (record_index * PLAYBACK_AUDIO_INDEX_RECORD_SIZE);
        playback_audio_index_record_t record;
        uint64_t record_end;

        record.pcm_sample_position = playback_read_le32(record_data);
        record.byte_offset = playback_read_le32(record_data + 4U);
        record.byte_length = playback_read_le32(record_data + 8U);
        record.crc32 = playback_read_le32(record_data + 12U);
        record_end = (uint64_t)record.byte_offset + (uint64_t)record.byte_length;

        if ((record.byte_length == 0U) || (record_end > audio_byte_length) ||
            ((record_index == 0U) && (record.pcm_sample_position != 0U)) ||
            ((record_index > 0U) && (record.pcm_sample_position <= previous_sample)) ||
            ((record_index > 0U) && ((uint64_t)record.byte_offset < previous_end)))
        {
            index->count = 0U;
            return PLAYBACK_PACKAGE_INVALID_INDEX;
        }

        if ((uint64_t)record.pcm_sample_position >= expected_samples)
        {
            break;
        }

        index->records[record_index] = record;
        previous_sample = record.pcm_sample_position;
        previous_end = record_end;
    }

    if (record_index == 0U)
    {
        return PLAYBACK_PACKAGE_INVALID_INDEX;
    }
    index->count = record_index;
    return PLAYBACK_PACKAGE_OK;
}

const playback_audio_index_record_t *playback_audio_index_find(
    const playback_audio_index_t *index,
    uint32_t pcm_sample_position
)
{
    size_t low;
    size_t high;

    if ((index == NULL) || (index->records == NULL) || (index->count == 0U))
    {
        return NULL;
    }

    low = 0U;
    high = index->count;
    while (low < high)
    {
        const size_t middle = low + ((high - low) / 2U);
        if (index->records[middle].pcm_sample_position <= pcm_sample_position)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }

    return (low == 0U) ? &index->records[0] : &index->records[low - 1U];
}

const char *playback_package_result_name(playback_package_result_t result)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "UNSUPPORTED_PROFILE",
        "INVALID_SESSION",
        "INVALID_VIDEO",
        "INVALID_AUDIO",
        "INVALID_ASSET",
        "INVALID_INDEX",
        "INDEX_CAPACITY",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0]))) ? names[result] : "UNKNOWN_PACKAGE_RESULT";
}
