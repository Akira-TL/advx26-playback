#ifndef PLAYBACK_MEDIA_PACKAGE_H
#define PLAYBACK_MEDIA_PACKAGE_H

#include <stddef.h>
#include <stdint.h>

#include "playback_domain.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_AUDIO_INDEX_VERSION (1U)
#define PLAYBACK_AUDIO_INDEX_HEADER_SIZE (16U)
#define PLAYBACK_AUDIO_INDEX_RECORD_SIZE (16U)
#define PLAYBACK_AUDIO_INDEX_MAGIC (0x31584941UL) /* AIX1, little-endian */
#define PLAYBACK_AUDIO_INDEX_MAX_RECORDS (1200U)
#define PLAYBACK_H264_BASELINE_PROFILE_IDC (66U)
#define PLAYBACK_H264_MAX_KEYFRAME_INTERVAL_MS (1000U)
#define PLAYBACK_VIDEO_MAX_FPS (15U)
#define PLAYBACK_MP3_SAMPLE_RATE (44100U)
#define PLAYBACK_MP3_BITRATE_KBPS (128U)

typedef enum
{
    PLAYBACK_PACKAGE_OK = 0,
    PLAYBACK_PACKAGE_INVALID_ARGUMENT,
    PLAYBACK_PACKAGE_UNSUPPORTED_PROFILE,
    PLAYBACK_PACKAGE_INVALID_SESSION,
    PLAYBACK_PACKAGE_INVALID_VIDEO,
    PLAYBACK_PACKAGE_INVALID_AUDIO,
    PLAYBACK_PACKAGE_INVALID_ASSET,
    PLAYBACK_PACKAGE_INVALID_INDEX,
    PLAYBACK_PACKAGE_INDEX_CAPACITY,
} playback_package_result_t;

typedef struct
{
    uint32_t pcm_sample_position;
    uint32_t byte_offset;
    uint32_t byte_length;
    uint32_t crc32;
} playback_audio_index_record_t;

typedef struct
{
    playback_audio_index_record_t *records;
    size_t capacity;
    size_t count;
} playback_audio_index_t;

typedef struct
{
    playback_session_t session;
    uint64_t expected_pcm_samples;
} playback_media_package_t;

playback_package_result_t playback_media_package_validate(
    const playback_session_t *session,
    playback_media_package_t *package
);

playback_package_result_t playback_audio_index_parse(
    const uint8_t *payload,
    size_t payload_length,
    uint32_t audio_byte_length,
    uint32_t sample_rate,
    uint32_t duration_ms,
    playback_audio_index_t *index
);

const playback_audio_index_record_t *playback_audio_index_find(
    const playback_audio_index_t *index,
    uint32_t pcm_sample_position
);

const char *playback_package_result_name(playback_package_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_MEDIA_PACKAGE_H */
