#ifndef PLAYBACK_MEDIA_SCHEDULER_H
#define PLAYBACK_MEDIA_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "playback_domain.h"
#include "playback_http_range.h"
#include "playback_media_package.h"
#include "playback_video_output.h"
#include "playback_wired_speaker.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_SCHEDULER_VIDEO_QUEUE_CAPACITY (3U)

typedef enum
{
    PLAYBACK_SCHEDULER_OK = 0,
    PLAYBACK_SCHEDULER_INVALID_ARGUMENT,
    PLAYBACK_SCHEDULER_ALREADY_PREPARED,
    PLAYBACK_SCHEDULER_NOT_PREPARED,
    PLAYBACK_SCHEDULER_NO_MEMORY,
    PLAYBACK_SCHEDULER_AUDIO_FAILED,
    PLAYBACK_SCHEDULER_VIDEO_FAILED,
    PLAYBACK_SCHEDULER_SPEAKER_FAILED,
    PLAYBACK_SCHEDULER_OUTPUT_FAILED,
    PLAYBACK_SCHEDULER_ERROR_STATE,
} playback_media_scheduler_result_t;

typedef struct
{
    playback_http_config_t http;
    const uint8_t *audio_memory;
    size_t audio_memory_length;
    const uint8_t *video_memory;
    size_t video_memory_length;
    playback_wired_speaker_config_t wired_speaker;
    playback_video_output_config_t video_output;
    playback_video_sink_present_fn present_video;
    void *video_context;
} playback_media_scheduler_config_t;

typedef struct
{
    bool prepared;
    playback_state_t state;
    playback_intent_t intent;
    playback_error_t error;
    uint32_t position_ms;
    uint32_t duration_ms;
    uint32_t buffered_audio_ms;
    uint32_t next_video_sample;
    uint8_t queued_video_frames;
    uint32_t dropped_video_frames;
    bool speaker_started;
    uint32_t queued_speaker_ms;
    uint64_t submitted_audio_frames;
    bool audio_exhausted;
    bool video_exhausted;
} playback_media_scheduler_snapshot_t;

typedef struct
{
    void *state;
} playback_media_scheduler_t;

playback_media_scheduler_result_t playback_media_scheduler_prepare(
    playback_media_scheduler_t *scheduler,
    const playback_media_package_t *package,
    const playback_audio_index_t *audio_index,
    const playback_media_scheduler_config_t *config
);

playback_media_scheduler_result_t playback_media_scheduler_play(
    playback_media_scheduler_t *scheduler
);

playback_media_scheduler_result_t playback_media_scheduler_pause(
    playback_media_scheduler_t *scheduler
);

playback_media_scheduler_result_t playback_media_scheduler_seek(
    playback_media_scheduler_t *scheduler,
    uint32_t position_ms
);

playback_media_scheduler_result_t playback_media_scheduler_stop(
    playback_media_scheduler_t *scheduler
);

/**
 * Advance bounded prefetch, decode, speaker and presentation work once.
 * Call from one engine worker; this function may perform HTTP Range reads.
 */
playback_media_scheduler_result_t playback_media_scheduler_pump(
    playback_media_scheduler_t *scheduler
);

playback_media_scheduler_result_t playback_media_scheduler_get_snapshot(
    playback_media_scheduler_t *scheduler,
    playback_media_scheduler_snapshot_t *snapshot
);

void playback_media_scheduler_close(playback_media_scheduler_t *scheduler);
playback_error_t playback_media_scheduler_result_to_error(
    playback_media_scheduler_result_t result
);
const char *playback_media_scheduler_result_name(
    playback_media_scheduler_result_t result
);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_MEDIA_SCHEDULER_H */
