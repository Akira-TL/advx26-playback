#ifndef PLAYBACK_MP3_AUDIO_H
#define PLAYBACK_MP3_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "playback_domain.h"
#include "playback_http_range.h"
#include "playback_media_package.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_MP3_PCM_LOW_WATER_MS (120U)
#define PLAYBACK_MP3_PCM_HIGH_WATER_MS (500U)
#define PLAYBACK_MP3_SEEK_WARMUP_RECORDS (10U)
#define PLAYBACK_MP3_MAX_COMPRESSED_FRAME_BYTES (16U * 1024U)
#define PLAYBACK_MP3_MAX_DECODED_SAMPLES (1152U * 2U)
#define PLAYBACK_MP3_MAX_CONSECUTIVE_FAILURES (3U)
#define PLAYBACK_MP3_RECOVERY_DEADLINE_MS (500U)

typedef enum
{
    PLAYBACK_MP3_OK = 0,
    PLAYBACK_MP3_INVALID_ARGUMENT,
    PLAYBACK_MP3_ALREADY_OPEN,
    PLAYBACK_MP3_NOT_OPEN,
    PLAYBACK_MP3_NO_MEMORY,
    PLAYBACK_MP3_HTTP_FAILED,
    PLAYBACK_MP3_RESOURCE_MISMATCH,
    PLAYBACK_MP3_INDEX_INVALID,
    PLAYBACK_MP3_FRAME_TOO_LARGE,
    PLAYBACK_MP3_CRC_MISMATCH,
    PLAYBACK_MP3_DECODE_FAILED,
    PLAYBACK_MP3_UNSUPPORTED_FORMAT,
    PLAYBACK_MP3_BUFFER_FULL,
    PLAYBACK_MP3_BUFFER_EMPTY,
    PLAYBACK_MP3_FETCH_PAUSED,
    PLAYBACK_MP3_OUTPUT_PAUSED,
    PLAYBACK_MP3_END_OF_STREAM,
    PLAYBACK_MP3_RECOVERY_FAILED,
} playback_mp3_result_t;

typedef enum
{
    PLAYBACK_MP3_PAUSE_FETCH = 1U << 0,
    PLAYBACK_MP3_PAUSE_OUTPUT = 1U << 1,
    PLAYBACK_MP3_PAUSE_ALL = PLAYBACK_MP3_PAUSE_FETCH | PLAYBACK_MP3_PAUSE_OUTPUT,
} playback_mp3_pause_flags_t;

typedef struct
{
    bool open;
    bool fetch_paused;
    bool output_paused;
    bool source_exhausted;
    bool fatal;
    uint8_t channels;
    uint32_t sample_rate;
    uint32_t low_water_frames;
    uint32_t high_water_frames;
    uint32_t available_pcm_frames;
    uint64_t consumed_pcm_frames;
    uint64_t expected_pcm_frames;
    size_t next_index_record;
    uint8_t consecutive_failures;
    uint32_t recovered_frame_count;
    playback_mp3_result_t last_failure;
} playback_mp3_status_t;

typedef struct
{
    void *state;
} playback_mp3_audio_t;

playback_mp3_result_t playback_mp3_audio_prepare(
    playback_mp3_audio_t *audio,
    const playback_audio_descriptor_t *descriptor,
    const playback_audio_index_t *index,
    uint32_t duration_ms,
    const playback_http_config_t *http_config
);

/**
 * Prepare the indexed decoder from a caller-owned complete MP3 image.
 * The memory must remain valid until playback_mp3_audio_close().
 */
playback_mp3_result_t playback_mp3_audio_prepare_memory(
    playback_mp3_audio_t *audio,
    const playback_audio_descriptor_t *descriptor,
    const playback_audio_index_t *index,
    uint32_t duration_ms,
    const uint8_t *data,
    size_t data_length
);

/**
 * @brief Fill the bounded PCM ring toward its high-water mark.
 *
 * One call may issue several indexed HTTP Range reads. Recoverable isolated
 * frame failures are replaced by equivalent-duration silence. The function
 * returns OK when useful progress was made or the high-water mark is already
 * satisfied.
 */
playback_mp3_result_t playback_mp3_audio_fill(playback_mp3_audio_t *audio);

/**
 * @brief Consume up to destination_frame_capacity PCM sample frames.
 *
 * PCM is signed 16-bit interleaved audio. A frame contains one sample per
 * channel. consumed_frames is always written when non-NULL.
 */
playback_mp3_result_t playback_mp3_audio_consume(
    playback_mp3_audio_t *audio,
    int16_t *destination,
    size_t destination_frame_capacity,
    size_t *consumed_frames
);

playback_mp3_result_t playback_mp3_audio_seek(
    playback_mp3_audio_t *audio,
    uint64_t target_pcm_frame
);

playback_mp3_result_t playback_mp3_audio_pause(
    playback_mp3_audio_t *audio,
    uint32_t flags
);

playback_mp3_result_t playback_mp3_audio_resume(
    playback_mp3_audio_t *audio,
    uint32_t flags
);

playback_mp3_result_t playback_mp3_audio_get_status(
    const playback_mp3_audio_t *audio,
    playback_mp3_status_t *status
);

void playback_mp3_audio_close(playback_mp3_audio_t *audio);
playback_error_t playback_mp3_result_to_error(playback_mp3_result_t result);
const char *playback_mp3_result_name(playback_mp3_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_MP3_AUDIO_H */
