/**
 * @file playback_media_scheduler.c
 * @brief PCM-authoritative MP3/H.264 scheduler for the onboard wired speaker.
 */

#include "playback_media_scheduler.h"

#include <limits.h>
#include <string.h>

#include "playback_h264_decoder.h"
#include "playback_mp3_audio.h"
#include "playback_mp4_demux.h"
#include "playback_wired_speaker.h"
#include "tal_api.h"

#define PLAYBACK_SCHEDULER_VIDEO_PREROLL_FRAMES (2U)
#define PLAYBACK_SCHEDULER_MAX_GOP_RECOVERY_FAILURES (2U)
#define PLAYBACK_SCHEDULER_SPEAKER_QUEUE_TARGET_MS (80U)
#define PLAYBACK_SCHEDULER_SPEAKER_MAX_WRITES_PER_PUMP \
    ((PLAYBACK_SCHEDULER_SPEAKER_QUEUE_TARGET_MS + PLAYBACK_WIRED_SPEAKER_WRITE_MS - 1U) / \
     PLAYBACK_WIRED_SPEAKER_WRITE_MS)
#define PLAYBACK_SCHEDULER_ANNEX_B_OVERHEAD_BYTES (4U * PLAYBACK_MP4_MAX_NALS_PER_ACCESS_UNIT)
#define PLAYBACK_SCHEDULER_PERF_LOG_INTERVAL_MS (1000U)
/* Bound decode work per pump so presents keep running while the queue rebuilds. */
#define PLAYBACK_SCHEDULER_VIDEO_FILL_MAX_PER_PUMP (2U)
/* Extrapolate the audio clock while the audio worker is blocked on a refill. */
#define PLAYBACK_SCHEDULER_POSITION_EXTRAPOLATE_MAX_MS (2000U)
#define PLAYBACK_SCHEDULER_COMPLETION_CHECK_WINDOW_MS (2000U)
/* Cap one clock step so a blocking window refill pauses playback instead of
 * making every queued frame due at once (mass frame drop). */
#define PLAYBACK_SCHEDULER_VIDEO_CLOCK_STEP_MAX_MS (120U)

typedef struct
{
    uint64_t window_started_ms;
    uint64_t submitted_start_frames;
    uint32_t position_start_ms;
    uint32_t pump_count;
    uint64_t pump_total_ms;
    uint32_t pump_max_ms;
    uint64_t mp3_fill_total_ms;
    uint32_t mp3_fill_max_ms;
    uint64_t video_fill_total_ms;
    uint32_t video_fill_max_ms;
    uint64_t speaker_total_ms;
    uint32_t speaker_write_count;
    uint32_t speaker_write_max_ms;
    uint64_t present_total_ms;
    uint32_t present_max_ms;
} playback_scheduler_perf_window_t;

typedef struct
{
    uint8_t *storage;
    playback_h264_frame_t frame;
} playback_scheduler_video_slot_t;

typedef struct
{
    playback_media_package_t package;
    playback_media_scheduler_config_t config;
    MUTEX_HANDLE status_mutex;
    MUTEX_HANDLE audio_mutex;

    playback_mp3_audio_t audio;
    playback_mp4_demux_t demux;
    playback_h264_decoder_t decoder;
    playback_video_output_t video_output;
    playback_wired_speaker_t speaker;
    playback_mp4_codec_config_t codec;

    uint8_t *parameter_sets;
    size_t parameter_sets_length;
    uint8_t *access_unit;
    size_t access_unit_capacity;

    playback_scheduler_video_slot_t video_slots[PLAYBACK_SCHEDULER_VIDEO_QUEUE_CAPACITY];
    size_t video_frame_bytes;
    uint8_t video_read;
    uint8_t video_write;
    uint8_t video_count;
    uint32_t next_video_sample;
    uint32_t dropped_video_frames;
    uint8_t h264_recovery_failures;
    bool h264_recovery_active;
    bool video_exhausted;
    bool seeking;
    uint64_t seek_target_ticks;

    playback_state_t state;
    playback_intent_t intent;
    playback_error_t error;
    uint32_t position_ms;
    uint64_t position_wall_ms;
    bool audio_output_enabled;
    playback_scheduler_perf_window_t perf;

    THREAD_HANDLE audio_thread;
    SEM_HANDLE audio_worker_stopped;
    volatile bool audio_worker_running;
} playback_media_scheduler_state_t;

static void playback_scheduler_audio_worker(void *context);

static playback_media_scheduler_state_t *playback_scheduler_get_state(
    const playback_media_scheduler_t *scheduler
)
{
    return (scheduler == NULL) ? NULL : (playback_media_scheduler_state_t *)scheduler->state;
}

static bool playback_scheduler_size_add(size_t left, size_t right, size_t *result)
{
    if ((result == NULL) || (SIZE_MAX - left < right))
    {
        return false;
    }
    *result = left + right;
    return true;
}

static uint32_t playback_scheduler_ticks_to_ms(uint64_t ticks, uint32_t timescale)
{
    uint64_t milliseconds;

    if (timescale == 0U)
    {
        return 0U;
    }
    milliseconds = (ticks * 1000ULL) / timescale;
    return (milliseconds > UINT32_MAX) ? UINT32_MAX : (uint32_t)milliseconds;
}

static uint32_t playback_scheduler_frames_to_ms(uint64_t frames, uint32_t sample_rate)
{
    uint64_t milliseconds;

    if (sample_rate == 0U)
    {
        return 0U;
    }
    milliseconds = (frames * 1000ULL) / sample_rate;
    return (milliseconds > UINT32_MAX) ? UINT32_MAX : (uint32_t)milliseconds;
}

static uint64_t playback_scheduler_ms_to_frames(uint32_t milliseconds, uint32_t sample_rate)
{
    return ((uint64_t)milliseconds * sample_rate) / 1000ULL;
}

static uint64_t playback_scheduler_speaker_queued_frames(
    const playback_wired_speaker_status_t *status
)
{
    if ((status == NULL) ||
        (status->submitted_pcm_frames <= status->audible_pcm_frames))
    {
        return 0ULL;
    }
    return status->submitted_pcm_frames - status->audible_pcm_frames;
}

static uint32_t playback_scheduler_elapsed_ms(uint64_t started_ms)
{
    const uint64_t now_ms = tal_system_get_millisecond();
    const uint64_t elapsed_ms = now_ms >= started_ms ? now_ms - started_ms : 0ULL;

    return elapsed_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_ms;
}

static void playback_scheduler_perf_add(
    uint64_t *total_ms,
    uint32_t *maximum_ms,
    uint32_t elapsed_ms
)
{
    *total_ms += elapsed_ms;
    if (elapsed_ms > *maximum_ms)
    {
        *maximum_ms = elapsed_ms;
    }
}

static void playback_scheduler_perf_record(
    playback_media_scheduler_state_t *state,
    const playback_mp3_status_t *audio_status,
    const playback_wired_speaker_status_t *speaker_status,
    uint32_t pump_ms,
    uint32_t mp3_fill_ms,
    uint32_t video_fill_ms,
    uint32_t speaker_ms,
    uint32_t speaker_write_count,
    uint32_t speaker_write_max_ms,
    uint32_t present_ms
)
{
    playback_scheduler_perf_window_t *perf;
    uint64_t now_ms;
    uint64_t wall_ms;
    uint64_t submitted_delta;
    uint64_t expected_frames;
    uint64_t queued_frames;
    uint32_t position_ms;
    uint32_t position_delta;
    uint32_t submitted_permille;
    uint32_t position_permille;

    if ((state == NULL) || (audio_status == NULL) || (speaker_status == NULL) ||
        (audio_status->sample_rate == 0U))
    {
        return;
    }

    perf = &state->perf;
    now_ms = tal_system_get_millisecond();
    position_ms = playback_scheduler_frames_to_ms(
        speaker_status->audible_pcm_frames,
        audio_status->sample_rate
    );
    if (perf->window_started_ms == 0ULL)
    {
        perf->window_started_ms = now_ms;
        perf->submitted_start_frames = speaker_status->submitted_pcm_frames;
        perf->position_start_ms = position_ms;
    }

    perf->pump_count++;
    playback_scheduler_perf_add(&perf->pump_total_ms, &perf->pump_max_ms, pump_ms);
    playback_scheduler_perf_add(
        &perf->mp3_fill_total_ms,
        &perf->mp3_fill_max_ms,
        mp3_fill_ms
    );
    playback_scheduler_perf_add(
        &perf->video_fill_total_ms,
        &perf->video_fill_max_ms,
        video_fill_ms
    );
    perf->speaker_total_ms += speaker_ms;
    perf->speaker_write_count += speaker_write_count;
    if (speaker_write_max_ms > perf->speaker_write_max_ms)
    {
        perf->speaker_write_max_ms = speaker_write_max_ms;
    }
    playback_scheduler_perf_add(
        &perf->present_total_ms,
        &perf->present_max_ms,
        present_ms
    );

    wall_ms = now_ms >= perf->window_started_ms
                  ? now_ms - perf->window_started_ms
                  : 0ULL;
    if (wall_ms < PLAYBACK_SCHEDULER_PERF_LOG_INTERVAL_MS)
    {
        return;
    }

    submitted_delta = speaker_status->submitted_pcm_frames >= perf->submitted_start_frames
                          ? speaker_status->submitted_pcm_frames - perf->submitted_start_frames
                          : 0ULL;
    expected_frames = ((uint64_t)audio_status->sample_rate * wall_ms) / 1000ULL;
    submitted_permille = expected_frames > 0ULL
                             ? (uint32_t)((submitted_delta * 1000ULL) / expected_frames)
                             : 0U;
    position_delta = position_ms >= perf->position_start_ms
                         ? position_ms - perf->position_start_ms
                         : 0U;
    position_permille = wall_ms > 0ULL
                            ? (uint32_t)(((uint64_t)position_delta * 1000ULL) / wall_ms)
                            : 0U;
    queued_frames = playback_scheduler_speaker_queued_frames(speaker_status);

    PR_NOTICE(
        "[DEBUG-avperf] scheduler wall=%llu pumps=%u pump_total=%llu pump_max=%u mp3_total=%llu mp3_max=%u video_total=%llu video_max=%u speaker_total=%llu writes=%u write_max=%u present_total=%llu present_max=%u submitted=%llu expected=%llu submit_permille=%u position_delta=%u position_permille=%u queue_frames=%llu",
        (unsigned long long)wall_ms,
        (unsigned int)perf->pump_count,
        (unsigned long long)perf->pump_total_ms,
        (unsigned int)perf->pump_max_ms,
        (unsigned long long)perf->mp3_fill_total_ms,
        (unsigned int)perf->mp3_fill_max_ms,
        (unsigned long long)perf->video_fill_total_ms,
        (unsigned int)perf->video_fill_max_ms,
        (unsigned long long)perf->speaker_total_ms,
        (unsigned int)perf->speaker_write_count,
        (unsigned int)perf->speaker_write_max_ms,
        (unsigned long long)perf->present_total_ms,
        (unsigned int)perf->present_max_ms,
        (unsigned long long)submitted_delta,
        (unsigned long long)expected_frames,
        (unsigned int)submitted_permille,
        (unsigned int)position_delta,
        (unsigned int)position_permille,
        (unsigned long long)queued_frames
    );

    memset(perf, 0, sizeof(*perf));
    perf->window_started_ms = now_ms;
    perf->submitted_start_frames = speaker_status->submitted_pcm_frames;
    perf->position_start_ms = position_ms;
}

static void playback_scheduler_set_runtime(
    playback_media_scheduler_state_t *state,
    playback_state_t playback_state,
    playback_error_t error
)
{
    tal_mutex_lock(state->status_mutex);
    state->state = playback_state;
    state->error = error;
    tal_mutex_unlock(state->status_mutex);
}

static playback_media_scheduler_result_t playback_scheduler_fail(
    playback_media_scheduler_state_t *state,
    playback_media_scheduler_result_t result,
    playback_error_t error
)
{
    tal_mutex_lock(state->status_mutex);
    state->state = PLAYBACK_STATE_ERROR;
    state->intent = PLAYBACK_INTENT_PAUSED;
    state->error = error;
    tal_mutex_unlock(state->status_mutex);
    return result;
}

static size_t playback_scheduler_read_pcm(
    void *context,
    int16_t *destination,
    size_t frame_capacity
)
{
    playback_media_scheduler_state_t *state = context;
    size_t consumed = 0U;
    playback_mp3_result_t result;

    if ((state == NULL) || (destination == NULL) || (frame_capacity == 0U))
    {
        return 0U;
    }

    result = playback_mp3_audio_consume(&state->audio, destination, frame_capacity, &consumed);
    return (result == PLAYBACK_MP3_OK) ? consumed : 0U;
}

static void playback_scheduler_clear_video_queue(playback_media_scheduler_state_t *state)
{
    state->video_read = 0U;
    state->video_write = 0U;
    state->video_count = 0U;
}

static bool playback_scheduler_copy_plane(
    uint8_t *destination,
    const uint8_t *source,
    uint32_t source_stride,
    uint16_t width,
    uint16_t height,
    size_t source_length
)
{
    uint16_t row;
    size_t required;

    if ((destination == NULL) || (source == NULL) || (source_stride < width) ||
        (width == 0U) || (height == 0U))
    {
        return false;
    }
    required = ((size_t)(height - 1U) * source_stride) + width;
    if (required > source_length)
    {
        return false;
    }

    for (row = 0U; row < height; ++row)
    {
        memcpy(destination + ((size_t)row * width), source + ((size_t)row * source_stride), width);
    }
    return true;
}

static bool playback_scheduler_copy_frame(
    playback_media_scheduler_state_t *state,
    playback_scheduler_video_slot_t *slot,
    const playback_h264_frame_t *frame
)
{
    size_t y_length;
    size_t chroma_length;
    uint16_t chroma_width;
    uint16_t chroma_height;

    if ((state == NULL) || (slot == NULL) || (slot->storage == NULL) || (frame == NULL) ||
        (frame->width != state->codec.width) || (frame->height != state->codec.height))
    {
        return false;
    }

    y_length = (size_t)frame->width * frame->height;
    chroma_length = y_length / 4U;
    if ((y_length + (2U * chroma_length)) > state->video_frame_bytes)
    {
        return false;
    }
    chroma_width = frame->width / 2U;
    chroma_height = frame->height / 2U;

    if (!playback_scheduler_copy_plane(
            slot->storage,
            frame->planes[0],
            frame->strides[0],
            frame->width,
            frame->height,
            frame->plane_lengths[0]
        ) ||
        !playback_scheduler_copy_plane(
            slot->storage + y_length,
            frame->planes[1],
            frame->strides[1],
            chroma_width,
            chroma_height,
            frame->plane_lengths[1]
        ) ||
        !playback_scheduler_copy_plane(
            slot->storage + y_length + chroma_length,
            frame->planes[2],
            frame->strides[2],
            chroma_width,
            chroma_height,
            frame->plane_lengths[2]
        ))
    {
        return false;
    }

    slot->frame = *frame;
    slot->frame.planes[0] = slot->storage;
    slot->frame.planes[1] = slot->storage + y_length;
    slot->frame.planes[2] = slot->storage + y_length + chroma_length;
    slot->frame.strides[0] = frame->width;
    slot->frame.strides[1] = chroma_width;
    slot->frame.strides[2] = chroma_width;
    slot->frame.plane_lengths[0] = y_length;
    slot->frame.plane_lengths[1] = chroma_length;
    slot->frame.plane_lengths[2] = chroma_length;
    return true;
}

static bool playback_scheduler_h264_recoverable(playback_h264_result_t result)
{
    return (result == PLAYBACK_H264_DECODE_FAILED) ||
           (result == PLAYBACK_H264_OUTPUT_INVALID) ||
           (result == PLAYBACK_H264_RESET_REQUIRED) ||
           (result == PLAYBACK_H264_NEED_SYNC);
}

static playback_media_scheduler_result_t playback_scheduler_recover_h264(
    playback_media_scheduler_state_t *state,
    const playback_mp4_sample_t *failed_sample,
    playback_h264_result_t failure
)
{
    playback_mp4_sample_t sync_sample;
    playback_mp4_result_t mp4_result;
    playback_h264_result_t reset_result;
    uint64_t maximum_gap_ticks;
    uint32_t sample_index;
    uint32_t position_ms;

    if (!playback_scheduler_h264_recoverable(failure))
    {
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_VIDEO_FAILED,
            playback_h264_result_to_error(failure)
        );
    }

    state->h264_recovery_failures = state->h264_recovery_active
                                        ? (uint8_t)(state->h264_recovery_failures + 1U)
                                        : 1U;
    if (state->h264_recovery_failures >= PLAYBACK_SCHEDULER_MAX_GOP_RECOVERY_FAILURES)
    {
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_VIDEO_FAILED,
            PLAYBACK_ERROR_H264_DECODE_FAILED
        );
    }

    maximum_gap_ticks =
        ((uint64_t)state->package.session.video.max_keyframe_interval_ms *
         state->codec.timescale) /
        1000ULL;
    for (sample_index = failed_sample->index + 1U;
         sample_index < state->codec.sample_count;
         ++sample_index)
    {
        mp4_result = playback_mp4_demux_get_sample(
            &state->demux,
            sample_index,
            &sync_sample
        );
        if (mp4_result != PLAYBACK_MP4_OK)
        {
            return playback_scheduler_fail(
                state,
                PLAYBACK_SCHEDULER_VIDEO_FAILED,
                playback_mp4_result_to_error(mp4_result)
            );
        }
        if (!sync_sample.is_sync)
        {
            continue;
        }
        if ((sync_sample.pts < failed_sample->pts) ||
            ((sync_sample.pts - failed_sample->pts) > maximum_gap_ticks))
        {
            return playback_scheduler_fail(
                state,
                PLAYBACK_SCHEDULER_VIDEO_FAILED,
                PLAYBACK_ERROR_H264_DECODE_FAILED
            );
        }

        state->dropped_video_frames += state->video_count;
        playback_scheduler_clear_video_queue(state);
        reset_result = playback_h264_decoder_reset_at_sync(&state->decoder);
        if (reset_result != PLAYBACK_H264_OK)
        {
            return playback_scheduler_fail(
                state,
                PLAYBACK_SCHEDULER_VIDEO_FAILED,
                playback_h264_result_to_error(reset_result)
            );
        }
        state->next_video_sample = sync_sample.index;
        state->video_exhausted = false;
        state->seeking = true;
        tal_mutex_lock(state->status_mutex);
        position_ms = state->position_ms;
        tal_mutex_unlock(state->status_mutex);
        state->seek_target_ticks =
            ((uint64_t)position_ms * state->codec.timescale) / 1000ULL;
        state->h264_recovery_active = true;
        return PLAYBACK_SCHEDULER_OK;
    }

    return playback_scheduler_fail(
        state,
        PLAYBACK_SCHEDULER_VIDEO_FAILED,
        PLAYBACK_ERROR_H264_DECODE_FAILED
    );
}

static playback_media_scheduler_result_t playback_scheduler_decode_one(
    playback_media_scheduler_state_t *state
)
{
    playback_mp4_sample_t sample;
    playback_h264_frame_t frame;
    playback_mp4_result_t mp4_result;
    playback_h264_result_t h264_result;
    size_t access_unit_length = 0U;
    bool keep_frame;

    if (state->next_video_sample >= state->codec.sample_count)
    {
        state->video_exhausted = true;
        return PLAYBACK_SCHEDULER_OK;
    }

    mp4_result = playback_mp4_demux_get_sample(&state->demux, state->next_video_sample, &sample);
    if (mp4_result != PLAYBACK_MP4_OK)
    {
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_VIDEO_FAILED,
            playback_mp4_result_to_error(mp4_result)
        );
    }
    mp4_result = playback_mp4_demux_read_access_unit(
        &state->demux,
        state->next_video_sample,
        state->access_unit,
        state->access_unit_capacity,
        &access_unit_length
    );
    if (mp4_result != PLAYBACK_MP4_OK)
    {
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_VIDEO_FAILED,
            playback_mp4_result_to_error(mp4_result)
        );
    }

    h264_result = playback_h264_decoder_submit_access_unit(
        &state->decoder,
        state->access_unit,
        access_unit_length,
        sample.pts,
        sample.duration,
        sample.is_sync
    );
    if (h264_result != PLAYBACK_H264_OK)
    {
        return playback_scheduler_recover_h264(state, &sample, h264_result);
    }
    h264_result = playback_h264_decoder_poll_frame(&state->decoder, &frame);
    if (h264_result != PLAYBACK_H264_OK)
    {
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_VIDEO_FAILED,
            playback_h264_result_to_error(h264_result)
        );
    }

    keep_frame = !state->seeking || ((frame.pts + frame.duration) > state->seek_target_ticks);
    if (keep_frame)
    {
        playback_scheduler_video_slot_t *slot = &state->video_slots[state->video_write];
        if ((state->video_count >= PLAYBACK_SCHEDULER_VIDEO_QUEUE_CAPACITY) ||
            !playback_scheduler_copy_frame(state, slot, &frame))
        {
            (void)playback_h264_decoder_release_frame(&state->decoder, &frame);
            return playback_scheduler_fail(
                state,
                PLAYBACK_SCHEDULER_VIDEO_FAILED,
                PLAYBACK_ERROR_INTERNAL
            );
        }
        state->video_write = (uint8_t)((state->video_write + 1U) % PLAYBACK_SCHEDULER_VIDEO_QUEUE_CAPACITY);
        /* Fill may run on the audio worker while presents pop on the engine
         * thread; the count update is the SPSC hand-off point. */
        tal_mutex_lock(state->status_mutex);
        state->video_count++;
        tal_mutex_unlock(state->status_mutex);
        state->seeking = false;
    }

    h264_result = playback_h264_decoder_release_frame(&state->decoder, &frame);
    if (h264_result != PLAYBACK_H264_OK)
    {
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_VIDEO_FAILED,
            playback_h264_result_to_error(h264_result)
        );
    }

    state->h264_recovery_active = false;
    state->h264_recovery_failures = 0U;
    state->next_video_sample++;
    if (state->next_video_sample >= state->codec.sample_count)
    {
        state->video_exhausted = true;
    }
    return PLAYBACK_SCHEDULER_OK;
}

static playback_media_scheduler_result_t playback_scheduler_fill_video(
    playback_media_scheduler_state_t *state
)
{
    uint8_t decoded = 0U;

    while ((state->video_count < PLAYBACK_SCHEDULER_VIDEO_QUEUE_CAPACITY) &&
           !state->video_exhausted &&
           (decoded < PLAYBACK_SCHEDULER_VIDEO_FILL_MAX_PER_PUMP))
    {
        playback_media_scheduler_result_t result = playback_scheduler_decode_one(state);
        if (result != PLAYBACK_SCHEDULER_OK)
        {
            return result;
        }
        decoded++;
    }
    return PLAYBACK_SCHEDULER_OK;
}

static uint8_t playback_scheduler_due_video_count(
    const playback_media_scheduler_state_t *state,
    uint32_t clock_ms
)
{
    uint8_t due = 0U;
    uint8_t index = state->video_read;

    while (due < state->video_count)
    {
        const playback_h264_frame_t *frame = &state->video_slots[index].frame;
        const uint32_t pts_ms = playback_scheduler_ticks_to_ms(frame->pts, frame->timescale);
        if (pts_ms > clock_ms)
        {
            break;
        }
        due++;
        index = (uint8_t)((index + 1U) % PLAYBACK_SCHEDULER_VIDEO_QUEUE_CAPACITY);
    }
    return due;
}

static playback_media_scheduler_result_t playback_scheduler_present_due(
    playback_media_scheduler_state_t *state,
    uint32_t media_position_ms
)
{
    const uint32_t presentation_clock =
        (media_position_ms > state->package.session.duration_ms)
            ? state->package.session.duration_ms
            : media_position_ms;
    uint8_t due = playback_scheduler_due_video_count(state, presentation_clock);

    while (due > 1U)
    {
        state->video_read = (uint8_t)((state->video_read + 1U) % PLAYBACK_SCHEDULER_VIDEO_QUEUE_CAPACITY);
        tal_mutex_lock(state->status_mutex);
        state->video_count--;
        tal_mutex_unlock(state->status_mutex);
        state->dropped_video_frames++;
        due--;
    }

    if (due == 1U)
    {
        playback_scheduler_video_slot_t *slot = &state->video_slots[state->video_read];
        const playback_video_output_result_t output_result = playback_video_output_present(
            &state->video_output,
            &slot->frame
        );
        if ((output_result != PLAYBACK_VIDEO_OUTPUT_OK) &&
            (output_result != PLAYBACK_VIDEO_OUTPUT_STALE_FRAME))
        {
            return playback_scheduler_fail(
                state,
                PLAYBACK_SCHEDULER_OUTPUT_FAILED,
                playback_video_output_result_to_error(output_result)
            );
        }
        state->video_read = (uint8_t)((state->video_read + 1U) % PLAYBACK_SCHEDULER_VIDEO_QUEUE_CAPACITY);
        tal_mutex_lock(state->status_mutex);
        state->video_count--;
        tal_mutex_unlock(state->status_mutex);
    }

    return PLAYBACK_SCHEDULER_OK;
}

static void playback_scheduler_set_audio_output(
    playback_media_scheduler_state_t *state,
    bool enabled
)
{
    if (state->audio_output_enabled == enabled)
    {
        return;
    }
    if (enabled)
    {
        if (playback_mp3_audio_resume(&state->audio, PLAYBACK_MP3_PAUSE_OUTPUT) == PLAYBACK_MP3_OK)
        {
            state->audio_output_enabled = true;
        }
    }
    else
    {
        if (playback_mp3_audio_pause(&state->audio, PLAYBACK_MP3_PAUSE_OUTPUT) == PLAYBACK_MP3_OK)
        {
            state->audio_output_enabled = false;
        }
    }
}

static bool playback_scheduler_audio_ready(const playback_mp3_status_t *audio)
{
    return (audio->available_pcm_frames >= audio->low_water_frames) ||
           (audio->source_exhausted && (audio->available_pcm_frames > 0U)) ||
           (audio->consumed_pcm_frames >= audio->expected_pcm_frames);
}

static void playback_scheduler_release_state(playback_media_scheduler_state_t *state)
{
    uint8_t index;

    if (state == NULL)
    {
        return;
    }

    playback_wired_speaker_close(&state->speaker);
    playback_video_output_close(&state->video_output);
    playback_h264_decoder_close(&state->decoder);
    playback_mp4_demux_close(&state->demux);
    playback_mp3_audio_close(&state->audio);

    for (index = 0U; index < PLAYBACK_SCHEDULER_VIDEO_QUEUE_CAPACITY; ++index)
    {
        if (state->video_slots[index].storage != NULL)
        {
            tal_psram_free(state->video_slots[index].storage);
        }
    }
    if (state->access_unit != NULL)
    {
        tal_psram_free(state->access_unit);
    }
    if (state->parameter_sets != NULL)
    {
        tal_psram_free(state->parameter_sets);
    }
    if (state->audio_worker_stopped != NULL)
    {
        tal_semaphore_release(state->audio_worker_stopped);
    }
    if (state->audio_mutex != NULL)
    {
        tal_mutex_release(state->audio_mutex);
    }
    if (state->status_mutex != NULL)
    {
        tal_mutex_release(state->status_mutex);
    }
    tal_free(state);
}

playback_media_scheduler_result_t playback_media_scheduler_prepare(
    playback_media_scheduler_t *scheduler,
    const playback_media_package_t *package,
    const playback_audio_index_t *audio_index,
    const playback_media_scheduler_config_t *config
)
{
    playback_media_scheduler_state_t *state;
    playback_media_package_t validated_package;
    playback_wired_speaker_config_t speaker_config;
    playback_mp4_result_t mp4_result;
    playback_h264_result_t h264_result;
    playback_mp3_result_t mp3_result;
    playback_video_output_result_t output_result;
    playback_wired_speaker_result_t speaker_result;
    playback_package_result_t package_result;
    size_t parameter_sets_length = 0U;
    size_t y_length;
    size_t chroma_length;
    uint8_t index;

    if ((scheduler == NULL) || (package == NULL) || (audio_index == NULL) || (config == NULL) ||
        (config->present_video == NULL))
    {
        return PLAYBACK_SCHEDULER_INVALID_ARGUMENT;
    }
    if (scheduler->state != NULL)
    {
        return PLAYBACK_SCHEDULER_ALREADY_PREPARED;
    }

    package_result = playback_media_package_validate(&package->session, &validated_package);
    if (package_result != PLAYBACK_PACKAGE_OK)
    {
        return PLAYBACK_SCHEDULER_INVALID_ARGUMENT;
    }

    state = tal_calloc(1U, sizeof(*state));
    if (state == NULL)
    {
        return PLAYBACK_SCHEDULER_NO_MEMORY;
    }
    state->package = validated_package;
    state->config = *config;
    state->state = PLAYBACK_STATE_LOADING;
    state->intent = package->session.autoplay ? PLAYBACK_INTENT_PLAYING : PLAYBACK_INTENT_PAUSED;
    state->error = PLAYBACK_ERROR_NONE;

    if ((tal_mutex_create_init(&state->status_mutex) != OPRT_OK) ||
        (tal_mutex_create_init(&state->audio_mutex) != OPRT_OK))
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_NO_MEMORY;
    }

#if PLAYBACK_SCHEDULER_VIDEO_ONLY
    /* Video-only: skip the MP3 prepare (audio index + metadata HTTP probes). */
    (void)audio_index;
    (void)mp3_result;
#else
    if (state->config.audio_memory != NULL)
    {
        mp3_result = playback_mp3_audio_prepare_memory(
            &state->audio,
            &state->package.session.audio,
            audio_index,
            state->package.session.duration_ms,
            state->config.audio_memory,
            state->config.audio_memory_length
        );
    }
    else
    {
        mp3_result = playback_mp3_audio_prepare(
            &state->audio,
            &state->package.session.audio,
            audio_index,
            state->package.session.duration_ms,
            &state->config.http
        );
    }
    if (mp3_result != PLAYBACK_MP3_OK)
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_AUDIO_FAILED;
    }
    (void)playback_mp3_audio_pause(&state->audio, PLAYBACK_MP3_PAUSE_OUTPUT);
#endif

    if (state->config.video_memory != NULL)
    {
        mp4_result = playback_mp4_demux_open_memory(
            &state->demux,
            state->config.video_memory,
            state->config.video_memory_length
        );
    }
    else
    {
        mp4_result = playback_mp4_demux_open(
            &state->demux,
            &state->package.session.video.asset,
            &state->config.http
        );
    }
    if (mp4_result != PLAYBACK_MP4_OK)
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_VIDEO_FAILED;
    }
    mp4_result = playback_mp4_demux_get_codec_config(
        &state->demux,
        &state->codec,
        NULL,
        0U,
        &parameter_sets_length
    );
    if ((mp4_result != PLAYBACK_MP4_BUFFER_TOO_SMALL) || (parameter_sets_length == 0U) ||
        (parameter_sets_length > PLAYBACK_MP4_MAX_CODEC_CONFIG_BYTES))
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_VIDEO_FAILED;
    }

    state->parameter_sets = tal_psram_malloc(parameter_sets_length);
    if (state->parameter_sets == NULL)
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_NO_MEMORY;
    }
    state->parameter_sets_length = parameter_sets_length;
    mp4_result = playback_mp4_demux_get_codec_config(
        &state->demux,
        &state->codec,
        state->parameter_sets,
        state->parameter_sets_length,
        &parameter_sets_length
    );
    if (mp4_result != PLAYBACK_MP4_OK)
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_VIDEO_FAILED;
    }

    if (!playback_scheduler_size_add(
            state->codec.max_sample_size,
            PLAYBACK_SCHEDULER_ANNEX_B_OVERHEAD_BYTES,
            &state->access_unit_capacity
        ))
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_NO_MEMORY;
    }
    state->access_unit = tal_psram_malloc(state->access_unit_capacity);
    if (state->access_unit == NULL)
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_NO_MEMORY;
    }

    h264_result = playback_h264_decoder_open(
        &state->decoder,
        &state->package.session.video,
        &state->codec,
        state->parameter_sets,
        state->parameter_sets_length
    );
    if (h264_result != PLAYBACK_H264_OK)
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_VIDEO_FAILED;
    }

    y_length = (size_t)state->codec.width * state->codec.height;
    chroma_length = y_length / 4U;
    if (!playback_scheduler_size_add(y_length, 2U * chroma_length, &state->video_frame_bytes))
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_NO_MEMORY;
    }
    for (index = 0U; index < PLAYBACK_SCHEDULER_VIDEO_QUEUE_CAPACITY; ++index)
    {
        state->video_slots[index].storage = tal_psram_malloc(state->video_frame_bytes);
        if (state->video_slots[index].storage == NULL)
        {
            playback_scheduler_release_state(state);
            return PLAYBACK_SCHEDULER_NO_MEMORY;
        }
    }

    state->config.video_output.source_width = state->codec.width;
    state->config.video_output.source_height = state->codec.height;
    output_result = playback_video_output_open(
        &state->video_output,
        &state->config.video_output,
        state->config.present_video,
        state->config.video_context
    );
    if (output_result != PLAYBACK_VIDEO_OUTPUT_OK)
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_OUTPUT_FAILED;
    }

    speaker_config = state->config.wired_speaker;
    speaker_config.sample_rate = state->package.session.audio.sample_rate;
    speaker_config.source_channels = state->package.session.audio.channels;
    speaker_config.read_pcm = playback_scheduler_read_pcm;
    speaker_config.context = state;
    speaker_result = playback_wired_speaker_open(&state->speaker, &speaker_config);
    if (speaker_result != PLAYBACK_WIRED_SPEAKER_OK)
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_SPEAKER_FAILED;
    }

    state->audio_output_enabled = false;
    state->audio_worker_running = false;

    if (tal_semaphore_create_init(&state->audio_worker_stopped, 0U, 1U) != OPRT_OK)
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_NO_MEMORY;
    }

    {
        THREAD_CFG_T audio_thread_config;
        memset(&audio_thread_config, 0, sizeof(audio_thread_config));
        /* Worker now runs HTTP fetch + H264 decode in video-only mode. */
        audio_thread_config.stackDepth = (16U * 1024U);
        audio_thread_config.priority = THREAD_PRIO_1;
        audio_thread_config.thrdname = "playback_audio";
        audio_thread_config.psram_mode = 1U;

        state->audio_worker_running = true;
        if (tal_thread_create_and_start(
                &state->audio_thread,
                NULL,
                NULL,
                playback_scheduler_audio_worker,
                state,
                &audio_thread_config
            ) != OPRT_OK)
        {
            state->audio_worker_running = false;
            playback_scheduler_release_state(state);
            return PLAYBACK_SCHEDULER_SPEAKER_FAILED;
        }
    }

    state->state = package->session.autoplay ? PLAYBACK_STATE_BUFFERING : PLAYBACK_STATE_PAUSED;
    scheduler->state = state;
    return PLAYBACK_SCHEDULER_OK;
}

playback_media_scheduler_result_t playback_media_scheduler_play(
    playback_media_scheduler_t *scheduler
)
{
    playback_media_scheduler_state_t *state = playback_scheduler_get_state(scheduler);
    playback_state_t current_state;

    if (state == NULL)
    {
        return PLAYBACK_SCHEDULER_NOT_PREPARED;
    }

    tal_mutex_lock(state->status_mutex);
    current_state = state->state;
    tal_mutex_unlock(state->status_mutex);
    if (current_state == PLAYBACK_STATE_ERROR)
    {
        return PLAYBACK_SCHEDULER_ERROR_STATE;
    }
    if (current_state == PLAYBACK_STATE_COMPLETED)
    {
        playback_media_scheduler_result_t seek_result = playback_media_scheduler_seek(scheduler, 0U);
        if (seek_result != PLAYBACK_SCHEDULER_OK)
        {
            return seek_result;
        }
    }

    tal_mutex_lock(state->status_mutex);
    state->intent = PLAYBACK_INTENT_PLAYING;
    state->state = PLAYBACK_STATE_BUFFERING;
    tal_mutex_unlock(state->status_mutex);

    tal_mutex_lock(state->audio_mutex);
    (void)playback_mp3_audio_resume(&state->audio, PLAYBACK_MP3_PAUSE_FETCH);
    tal_mutex_unlock(state->audio_mutex);
    return PLAYBACK_SCHEDULER_OK;
}

playback_media_scheduler_result_t playback_media_scheduler_pause(
    playback_media_scheduler_t *scheduler
)
{
    playback_media_scheduler_state_t *state = playback_scheduler_get_state(scheduler);
    playback_state_t current_state;
    playback_wired_speaker_result_t speaker_result;

    if (state == NULL)
    {
        return PLAYBACK_SCHEDULER_NOT_PREPARED;
    }

    tal_mutex_lock(state->status_mutex);
    current_state = state->state;
    if (current_state != PLAYBACK_STATE_ERROR)
    {
        state->intent = PLAYBACK_INTENT_PAUSED;
        state->state = PLAYBACK_STATE_PAUSED;
    }
    tal_mutex_unlock(state->status_mutex);
    if (current_state == PLAYBACK_STATE_ERROR)
    {
        return PLAYBACK_SCHEDULER_ERROR_STATE;
    }

    tal_mutex_lock(state->audio_mutex);
    playback_scheduler_set_audio_output(state, false);
    speaker_result = playback_wired_speaker_pause(&state->speaker);
    tal_mutex_unlock(state->audio_mutex);
    if (speaker_result != PLAYBACK_WIRED_SPEAKER_OK)
    {
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_SPEAKER_FAILED,
            PLAYBACK_ERROR_INTERNAL
        );
    }
    return PLAYBACK_SCHEDULER_OK;
}

playback_media_scheduler_result_t playback_media_scheduler_seek(
    playback_media_scheduler_t *scheduler,
    uint32_t position_ms
)
{
    playback_media_scheduler_state_t *state = playback_scheduler_get_state(scheduler);
    playback_mp4_sample_t sync_sample;
    playback_mp4_result_t mp4_result;
    playback_mp3_result_t mp3_result;
    playback_h264_result_t h264_result;
    uint64_t target_pcm_frame;

    if (state == NULL)
    {
        return PLAYBACK_SCHEDULER_NOT_PREPARED;
    }
    if (position_ms > state->package.session.duration_ms)
    {
        return PLAYBACK_SCHEDULER_INVALID_ARGUMENT;
    }
    tal_mutex_lock(state->status_mutex);
    if (state->state == PLAYBACK_STATE_ERROR)
    {
        tal_mutex_unlock(state->status_mutex);
        return PLAYBACK_SCHEDULER_ERROR_STATE;
    }
    state->state = PLAYBACK_STATE_SEEKING;
    state->error = PLAYBACK_ERROR_NONE;
    tal_mutex_unlock(state->status_mutex);

    tal_mutex_lock(state->audio_mutex);
    playback_scheduler_set_audio_output(state, false);
    (void)playback_mp3_audio_pause(&state->audio, PLAYBACK_MP3_PAUSE_FETCH);

    target_pcm_frame = ((uint64_t)position_ms * state->package.session.audio.sample_rate) / 1000ULL;
    if (playback_wired_speaker_reset(&state->speaker, target_pcm_frame) !=
        PLAYBACK_WIRED_SPEAKER_OK)
    {
        tal_mutex_unlock(state->audio_mutex);
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_SPEAKER_FAILED,
            PLAYBACK_ERROR_INTERNAL
        );
    }
#if !PLAYBACK_SCHEDULER_VIDEO_ONLY
    mp3_result = playback_mp3_audio_seek(&state->audio, target_pcm_frame);
    if (mp3_result != PLAYBACK_MP3_OK)
    {
        tal_mutex_unlock(state->audio_mutex);
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_AUDIO_FAILED,
            playback_mp3_result_to_error(mp3_result)
        );
    }
#else
    (void)mp3_result;
#endif

    mp4_result = playback_mp4_demux_find_sync_at_or_before(&state->demux, position_ms, &sync_sample);
    if (mp4_result != PLAYBACK_MP4_OK)
    {
        tal_mutex_unlock(state->audio_mutex);
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_VIDEO_FAILED,
            playback_mp4_result_to_error(mp4_result)
        );
    }
    h264_result = playback_h264_decoder_reset_at_sync(&state->decoder);
    if (h264_result != PLAYBACK_H264_OK)
    {
        tal_mutex_unlock(state->audio_mutex);
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_VIDEO_FAILED,
            playback_h264_result_to_error(h264_result)
        );
    }

    playback_scheduler_clear_video_queue(state);
    state->next_video_sample = sync_sample.index;
    state->h264_recovery_active = false;
    state->h264_recovery_failures = 0U;
    state->video_exhausted = false;
    state->seeking = true;
    state->seek_target_ticks = ((uint64_t)position_ms * state->codec.timescale) / 1000ULL;
    (void)playback_mp3_audio_resume(&state->audio, PLAYBACK_MP3_PAUSE_FETCH);

    tal_mutex_lock(state->status_mutex);
    state->position_ms = position_ms;
    state->position_wall_ms = 0ULL;
    state->state = (state->intent == PLAYBACK_INTENT_PLAYING)
                       ? PLAYBACK_STATE_BUFFERING
                       : PLAYBACK_STATE_PAUSED;
    tal_mutex_unlock(state->status_mutex);
    tal_mutex_unlock(state->audio_mutex);
    return PLAYBACK_SCHEDULER_OK;
}

playback_media_scheduler_result_t playback_media_scheduler_stop(
    playback_media_scheduler_t *scheduler
)
{
    playback_media_scheduler_state_t *state = playback_scheduler_get_state(scheduler);
    playback_media_scheduler_result_t result;

    if (state == NULL)
    {
        return PLAYBACK_SCHEDULER_NOT_PREPARED;
    }
    result = playback_media_scheduler_pause(scheduler);
    if (result != PLAYBACK_SCHEDULER_OK)
    {
        return result;
    }
    result = playback_media_scheduler_seek(scheduler, 0U);
    if (result != PLAYBACK_SCHEDULER_OK)
    {
        return result;
    }
    playback_scheduler_set_runtime(state, PLAYBACK_STATE_PAUSED, PLAYBACK_ERROR_NONE);
    return PLAYBACK_SCHEDULER_OK;
}

/* --- audio worker (high-priority thread) ---------------------------------- */

static void playback_scheduler_audio_pump_locked(playback_media_scheduler_state_t *state)
{
    playback_mp3_status_t audio_status;
    playback_wired_speaker_status_t speaker_status;
    playback_mp3_result_t mp3_result;
    playback_wired_speaker_result_t speaker_result;
    playback_intent_t intent;
    playback_state_t current_state;
    bool audio_submitted_complete;
    bool audio_audible_complete;
    uint64_t pump_started_ms = tal_system_get_millisecond();
    uint64_t phase_started_ms;
    uint32_t mp3_fill_ms = 0U;
    uint32_t speaker_ms = 0U;
    uint32_t speaker_write_count = 0U;
    uint32_t speaker_write_max_ms = 0U;
    uint32_t position_ms;

    tal_mutex_lock(state->status_mutex);
    current_state = state->state;
    intent = state->intent;
    tal_mutex_unlock(state->status_mutex);

    if ((current_state == PLAYBACK_STATE_ERROR) ||
        (current_state == PLAYBACK_STATE_COMPLETED) ||
        (current_state == PLAYBACK_STATE_LOADING) ||
        (current_state == PLAYBACK_STATE_SEEKING))
    {
        playback_scheduler_set_audio_output(state, false);
        (void)playback_wired_speaker_pause(&state->speaker);
        return;
    }

#if PLAYBACK_SCHEDULER_VIDEO_ONLY
    /* Video-only: this worker becomes the video prefetch+decode thread so the
     * engine-thread pump (clock + present) never blocks on HTTP. */
    (void)intent;
    (void)audio_status;
    (void)speaker_status;
    (void)mp3_result;
    (void)speaker_result;
    (void)audio_submitted_complete;
    (void)audio_audible_complete;
    (void)pump_started_ms;
    (void)phase_started_ms;
    (void)mp3_fill_ms;
    (void)speaker_ms;
    (void)speaker_write_count;
    (void)speaker_write_max_ms;
    (void)position_ms;
    (void)playback_scheduler_fill_video(state);
    return;
#endif

    /* -- MP3 fill -------------------------------------------------------- */
    if (playback_mp3_audio_get_status(&state->audio, &audio_status) != PLAYBACK_MP3_OK)
    {
        playback_scheduler_fail(state, PLAYBACK_SCHEDULER_AUDIO_FAILED, PLAYBACK_ERROR_INTERNAL);
        return;
    }
    if (!audio_status.fetch_paused &&
        (audio_status.available_pcm_frames < audio_status.high_water_frames) &&
        !audio_status.source_exhausted)
    {
        phase_started_ms = tal_system_get_millisecond();
        mp3_result = playback_mp3_audio_fill(&state->audio);
        mp3_fill_ms += playback_scheduler_elapsed_ms(phase_started_ms);
        if ((mp3_result != PLAYBACK_MP3_OK) && (mp3_result != PLAYBACK_MP3_END_OF_STREAM) &&
            (mp3_result != PLAYBACK_MP3_BUFFER_FULL))
        {
            playback_scheduler_fail(
                state,
                PLAYBACK_SCHEDULER_AUDIO_FAILED,
                playback_mp3_result_to_error(mp3_result)
            );
            return;
        }
    }

    /* -- Status and position -------------------------------------------- */
    if ((playback_mp3_audio_get_status(&state->audio, &audio_status) != PLAYBACK_MP3_OK) ||
        (playback_wired_speaker_get_status(&state->speaker, &speaker_status) !=
         PLAYBACK_WIRED_SPEAKER_OK))
    {
        playback_scheduler_fail(state, PLAYBACK_SCHEDULER_SPEAKER_FAILED, PLAYBACK_ERROR_INTERNAL);
        return;
    }

    position_ms = playback_scheduler_frames_to_ms(
        speaker_status.audible_pcm_frames,
        audio_status.sample_rate
    );
    if (position_ms > state->package.session.duration_ms)
    {
        position_ms = state->package.session.duration_ms;
    }
    tal_mutex_lock(state->status_mutex);
    state->position_ms = position_ms;
    state->position_wall_ms = tal_system_get_millisecond();
    tal_mutex_unlock(state->status_mutex);

    /* -- Pause ---------------------------------------------------------- */
    if (intent == PLAYBACK_INTENT_PAUSED)
    {
        playback_scheduler_set_audio_output(state, false);
        (void)playback_wired_speaker_pause(&state->speaker);
        playback_scheduler_set_runtime(state, PLAYBACK_STATE_PAUSED, PLAYBACK_ERROR_NONE);
        return;
    }

    /* -- Buffering (audio only, no video gate) -------------------------- */
    audio_submitted_complete = audio_status.source_exhausted &&
                               (audio_status.consumed_pcm_frames >=
                                audio_status.expected_pcm_frames);

    if (!audio_submitted_complete &&
        !playback_scheduler_audio_ready(&audio_status))
    {
        playback_scheduler_set_audio_output(state, false);
        (void)playback_wired_speaker_pause(&state->speaker);
        playback_scheduler_set_runtime(state, PLAYBACK_STATE_BUFFERING, PLAYBACK_ERROR_NONE);
        return;
    }

    /* -- Speaker start -------------------------------------------------- */
    speaker_result = playback_wired_speaker_start(&state->speaker);
    if (speaker_result != PLAYBACK_WIRED_SPEAKER_OK)
    {
        playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_SPEAKER_FAILED,
            PLAYBACK_ERROR_INTERNAL
        );
        return;
    }

    /* -- Speaker pump --------------------------------------------------- */
    if (!audio_submitted_complete)
    {
        const uint64_t target_queued_frames = playback_scheduler_ms_to_frames(
            PLAYBACK_SCHEDULER_SPEAKER_QUEUE_TARGET_MS,
            audio_status.sample_rate
        );
        uint8_t writes = 0U;

        playback_scheduler_set_audio_output(state, true);
        while ((writes < PLAYBACK_SCHEDULER_SPEAKER_MAX_WRITES_PER_PUMP) &&
               (playback_scheduler_speaker_queued_frames(&speaker_status) <
                target_queued_frames))
        {
            uint32_t write_ms;

            phase_started_ms = tal_system_get_millisecond();
            speaker_result = playback_wired_speaker_pump(&state->speaker);
            write_ms = playback_scheduler_elapsed_ms(phase_started_ms);
            speaker_ms += write_ms;
            speaker_write_count++;
            if (write_ms > speaker_write_max_ms)
            {
                speaker_write_max_ms = write_ms;
            }
            if ((speaker_result != PLAYBACK_WIRED_SPEAKER_OK) &&
                (speaker_result != PLAYBACK_WIRED_SPEAKER_NO_DATA))
            {
                playback_scheduler_fail(
                    state,
                    PLAYBACK_SCHEDULER_SPEAKER_FAILED,
                    PLAYBACK_ERROR_INTERNAL
                );
                return;
            }
            if ((playback_mp3_audio_get_status(&state->audio, &audio_status) !=
                 PLAYBACK_MP3_OK) ||
                (playback_wired_speaker_get_status(&state->speaker, &speaker_status) !=
                 PLAYBACK_WIRED_SPEAKER_OK))
            {
                playback_scheduler_fail(
                    state,
                    PLAYBACK_SCHEDULER_SPEAKER_FAILED,
                    PLAYBACK_ERROR_INTERNAL
                );
                return;
            }
            if (speaker_result == PLAYBACK_WIRED_SPEAKER_NO_DATA)
            {
                if (playback_scheduler_speaker_queued_frames(&speaker_status) == 0ULL)
                {
                    playback_scheduler_set_audio_output(state, false);
                    (void)playback_wired_speaker_pause(&state->speaker);
                    playback_scheduler_set_runtime(
                        state,
                        PLAYBACK_STATE_BUFFERING,
                        PLAYBACK_ERROR_NONE
                    );
                    return;
                }
                break;
            }

            writes++;
            audio_submitted_complete = audio_status.source_exhausted &&
                                       (audio_status.consumed_pcm_frames >=
                                        audio_status.expected_pcm_frames);
            if (audio_submitted_complete)
            {
                break;
            }
        }
    }
    else
    {
        playback_scheduler_set_audio_output(state, false);
    }

    /* -- Position update after pump ------------------------------------- */
    if ((playback_mp3_audio_get_status(&state->audio, &audio_status) != PLAYBACK_MP3_OK) ||
        (playback_wired_speaker_get_status(&state->speaker, &speaker_status) !=
         PLAYBACK_WIRED_SPEAKER_OK))
    {
        playback_scheduler_fail(state, PLAYBACK_SCHEDULER_SPEAKER_FAILED, PLAYBACK_ERROR_INTERNAL);
        return;
    }

    position_ms = playback_scheduler_frames_to_ms(
        speaker_status.audible_pcm_frames,
        audio_status.sample_rate
    );
    if (position_ms > state->package.session.duration_ms)
    {
        position_ms = state->package.session.duration_ms;
    }
    tal_mutex_lock(state->status_mutex);
    state->position_ms = position_ms;
    tal_mutex_unlock(state->status_mutex);

    /* -- Completion ----------------------------------------------------- */
    audio_submitted_complete = audio_status.source_exhausted &&
                               (audio_status.consumed_pcm_frames >=
                                audio_status.expected_pcm_frames);
    audio_audible_complete = audio_submitted_complete &&
                             (speaker_status.audible_pcm_frames >=
                              audio_status.expected_pcm_frames);
    if (audio_audible_complete)
    {
        playback_scheduler_set_audio_output(state, false);
        (void)playback_wired_speaker_pause(&state->speaker);
        /* The video pump owns final draining and the COMPLETED transition. */
        playback_scheduler_perf_record(
            state,
            &audio_status,
            &speaker_status,
            playback_scheduler_elapsed_ms(pump_started_ms),
            mp3_fill_ms,
            0U,
            speaker_ms,
            speaker_write_count,
            speaker_write_max_ms,
            0U
        );
        return;
    }

    playback_scheduler_set_runtime(state, PLAYBACK_STATE_PLAYING, PLAYBACK_ERROR_NONE);
    playback_scheduler_perf_record(
        state,
        &audio_status,
        &speaker_status,
        playback_scheduler_elapsed_ms(pump_started_ms),
        mp3_fill_ms,
        0U,
        speaker_ms,
        speaker_write_count,
        speaker_write_max_ms,
        0U
    );
}

static void playback_scheduler_audio_pump(playback_media_scheduler_state_t *state)
{
    tal_mutex_lock(state->audio_mutex);
    playback_scheduler_audio_pump_locked(state);
    tal_mutex_unlock(state->audio_mutex);
}

static void playback_scheduler_audio_worker(void *context)
{
    playback_media_scheduler_state_t *state = context;

    while (state->audio_worker_running)
    {
        playback_scheduler_audio_pump(state);
        tal_system_sleep(5);
    }

    tal_semaphore_post(state->audio_worker_stopped);
}

/* --- video pump (caller thread) ------------------------------------------ */

playback_media_scheduler_result_t playback_media_scheduler_pump(
    playback_media_scheduler_t *scheduler
)
{
    playback_media_scheduler_state_t *state = playback_scheduler_get_state(scheduler);
    playback_media_scheduler_result_t result;
    playback_intent_t intent;
    playback_state_t current_state;
    uint64_t pump_started_ms;
    uint64_t phase_started_ms;
    uint32_t video_fill_ms = 0U;
    uint32_t present_ms = 0U;
    uint32_t position_ms;

    if (state == NULL)
    {
        return PLAYBACK_SCHEDULER_NOT_PREPARED;
    }

    pump_started_ms = tal_system_get_millisecond();

    tal_mutex_lock(state->status_mutex);
    current_state = state->state;
    intent = state->intent;
    tal_mutex_unlock(state->status_mutex);

    if (current_state == PLAYBACK_STATE_ERROR)
    {
        return PLAYBACK_SCHEDULER_ERROR_STATE;
    }

#if PLAYBACK_SCHEDULER_VIDEO_ONLY
    if (current_state == PLAYBACK_STATE_COMPLETED)
    {
        return PLAYBACK_SCHEDULER_OK;
    }
    if (intent == PLAYBACK_INTENT_PAUSED)
    {
        tal_mutex_lock(state->status_mutex);
        state->position_wall_ms = 0ULL;
        tal_mutex_unlock(state->status_mutex);
        playback_scheduler_set_runtime(state, PLAYBACK_STATE_PAUSED, PLAYBACK_ERROR_NONE);
        return PLAYBACK_SCHEDULER_OK;
    }

    /* -- Video-master clock: freeze while the queue is empty ------------- */
    tal_mutex_lock(state->status_mutex);
    if ((state->video_count == 0U) && !state->video_exhausted)
    {
        state->position_wall_ms = 0ULL;
        position_ms = state->position_ms;
        tal_mutex_unlock(state->status_mutex);
        playback_scheduler_set_runtime(state, PLAYBACK_STATE_BUFFERING, PLAYBACK_ERROR_NONE);
    }
    else
    {
        uint64_t now_ms = tal_system_get_millisecond();
        if (state->position_wall_ms != 0ULL)
        {
            uint64_t elapsed_ms = (now_ms > state->position_wall_ms)
                                      ? (now_ms - state->position_wall_ms)
                                      : 0ULL;
            uint64_t next_ms;
            if (elapsed_ms > PLAYBACK_SCHEDULER_VIDEO_CLOCK_STEP_MAX_MS)
            {
                elapsed_ms = PLAYBACK_SCHEDULER_VIDEO_CLOCK_STEP_MAX_MS;
            }
            next_ms = (uint64_t)state->position_ms + elapsed_ms;
            if (next_ms > state->package.session.duration_ms)
            {
                next_ms = state->package.session.duration_ms;
            }
            state->position_ms = (uint32_t)next_ms;
        }
        state->position_wall_ms = now_ms;
        position_ms = state->position_ms;
        tal_mutex_unlock(state->status_mutex);
        playback_scheduler_set_runtime(state, PLAYBACK_STATE_PLAYING, PLAYBACK_ERROR_NONE);
    }
#else
    if (intent == PLAYBACK_INTENT_PAUSED)
    {
        return PLAYBACK_SCHEDULER_OK;
    }

    /* -- Present first so a slow network fill cannot starve the display -- */
    tal_mutex_lock(state->status_mutex);
    position_ms = state->position_ms;
    if ((current_state == PLAYBACK_STATE_PLAYING) && (state->position_wall_ms != 0ULL))
    {
        uint64_t now_ms = tal_system_get_millisecond();
        uint64_t elapsed_ms = (now_ms > state->position_wall_ms)
                                  ? (now_ms - state->position_wall_ms)
                                  : 0ULL;
        if (elapsed_ms > PLAYBACK_SCHEDULER_POSITION_EXTRAPOLATE_MAX_MS)
        {
            elapsed_ms = PLAYBACK_SCHEDULER_POSITION_EXTRAPOLATE_MAX_MS;
        }
        position_ms += (uint32_t)elapsed_ms;
    }
    tal_mutex_unlock(state->status_mutex);
#endif
    phase_started_ms = tal_system_get_millisecond();
    result = playback_scheduler_present_due(state, position_ms);
    present_ms = playback_scheduler_elapsed_ms(phase_started_ms);
    if (result != PLAYBACK_SCHEDULER_OK)
    {
        return result;
    }

    /* -- Video decode --------------------------------------------------- */
#if PLAYBACK_SCHEDULER_VIDEO_ONLY
    /* The audio worker thread owns fill (HTTP + decode); nothing to do here. */
#else
    phase_started_ms = tal_system_get_millisecond();
    result = playback_scheduler_fill_video(state);
    video_fill_ms = playback_scheduler_elapsed_ms(phase_started_ms);
    if (result != PLAYBACK_SCHEDULER_OK)
    {
        return result;
    }
#endif

    /* -- Completion: audio done, drain remaining video ------------------ */
    if (state->video_exhausted ||
        ((uint64_t)position_ms + PLAYBACK_SCHEDULER_COMPLETION_CHECK_WINDOW_MS >=
         state->package.session.duration_ms))
    {
        bool audio_audible_complete;

#if PLAYBACK_SCHEDULER_VIDEO_ONLY
        audio_audible_complete =
            ((uint64_t)position_ms >= state->package.session.duration_ms);
#else
        playback_mp3_status_t audio_status;
        playback_wired_speaker_status_t speaker_status;
        bool audio_submitted_complete;

        tal_mutex_lock(state->audio_mutex);
        if ((playback_mp3_audio_get_status(&state->audio, &audio_status) != PLAYBACK_MP3_OK) ||
            (playback_wired_speaker_get_status(&state->speaker, &speaker_status) !=
             PLAYBACK_WIRED_SPEAKER_OK))
        {
            tal_mutex_unlock(state->audio_mutex);
            return playback_scheduler_fail(
                state,
                PLAYBACK_SCHEDULER_SPEAKER_FAILED,
                PLAYBACK_ERROR_INTERNAL
            );
        }
        tal_mutex_unlock(state->audio_mutex);

        audio_submitted_complete = audio_status.source_exhausted &&
                                   (audio_status.consumed_pcm_frames >=
                                    audio_status.expected_pcm_frames);
        audio_audible_complete = audio_submitted_complete &&
                                 (speaker_status.audible_pcm_frames >=
                                  audio_status.expected_pcm_frames);
#endif
        if (audio_audible_complete)
        {
            phase_started_ms = tal_system_get_millisecond();
            result = playback_scheduler_present_due(
                state,
                state->package.session.duration_ms
            );
            present_ms += playback_scheduler_elapsed_ms(phase_started_ms);
            if (result != PLAYBACK_SCHEDULER_OK)
            {
                return result;
            }
            playback_scheduler_set_runtime(
                state,
                (state->video_exhausted && (state->video_count == 0U))
                    ? PLAYBACK_STATE_COMPLETED
                    : PLAYBACK_STATE_BUFFERING,
                PLAYBACK_ERROR_NONE
            );
        }
    }

    {
        static uint64_t video_perf_started_ms = 0ULL;
        static uint64_t video_perf_present_total_ms = 0ULL;
        static uint32_t video_perf_present_max_ms = 0U;
        static uint32_t video_perf_pump_count = 0U;
        uint64_t now_ms = tal_system_get_millisecond();

        if (video_perf_started_ms == 0ULL)
        {
            video_perf_started_ms = now_ms;
        }
        video_perf_present_total_ms += present_ms;
        if (present_ms > video_perf_present_max_ms)
        {
            video_perf_present_max_ms = present_ms;
        }
        video_perf_pump_count++;

        {
            uint64_t wall_ms = now_ms >= video_perf_started_ms
                                   ? now_ms - video_perf_started_ms
                                   : 0ULL;
            if (wall_ms >= PLAYBACK_SCHEDULER_PERF_LOG_INTERVAL_MS)
            {
                PR_NOTICE(
                    "[DEBUG-avperf] video_pump wall=%llu pumps=%u present_total=%llu present_max=%u video_fill=%u",
                    (unsigned long long)wall_ms,
                    (unsigned int)video_perf_pump_count,
                    (unsigned long long)video_perf_present_total_ms,
                    (unsigned int)video_perf_present_max_ms,
                    (unsigned int)video_fill_ms
                );
                video_perf_started_ms = now_ms;
                video_perf_present_total_ms = 0ULL;
                video_perf_present_max_ms = 0U;
                video_perf_pump_count = 0U;
            }
        }
    }

    (void)pump_started_ms;
    return PLAYBACK_SCHEDULER_OK;
}

playback_media_scheduler_result_t playback_media_scheduler_get_snapshot(
    playback_media_scheduler_t *scheduler,
    playback_media_scheduler_snapshot_t *snapshot
)
{
    playback_media_scheduler_state_t *state = playback_scheduler_get_state(scheduler);
    playback_mp3_status_t audio_status;
    playback_wired_speaker_status_t speaker_status;

    if ((state == NULL) || (snapshot == NULL))
    {
        return (scheduler == NULL || snapshot == NULL)
                   ? PLAYBACK_SCHEDULER_INVALID_ARGUMENT
                   : PLAYBACK_SCHEDULER_NOT_PREPARED;
    }
#if PLAYBACK_SCHEDULER_VIDEO_ONLY
    memset(&audio_status, 0, sizeof(audio_status));
    memset(&speaker_status, 0, sizeof(speaker_status));
#else
    tal_mutex_lock(state->audio_mutex);
    if (playback_mp3_audio_get_status(&state->audio, &audio_status) != PLAYBACK_MP3_OK)
    {
        tal_mutex_unlock(state->audio_mutex);
        return PLAYBACK_SCHEDULER_AUDIO_FAILED;
    }
    if (playback_wired_speaker_get_status(&state->speaker, &speaker_status) !=
        PLAYBACK_WIRED_SPEAKER_OK)
    {
        tal_mutex_unlock(state->audio_mutex);
        return PLAYBACK_SCHEDULER_SPEAKER_FAILED;
    }
    tal_mutex_unlock(state->audio_mutex);
#endif

    memset(snapshot, 0, sizeof(*snapshot));
    tal_mutex_lock(state->status_mutex);
    snapshot->prepared = true;
    snapshot->state = state->state;
    snapshot->intent = state->intent;
    snapshot->error = state->error;
#if PLAYBACK_SCHEDULER_VIDEO_ONLY
    snapshot->position_ms = state->position_ms;
#else
    snapshot->position_ms = playback_scheduler_frames_to_ms(
        speaker_status.audible_pcm_frames,
        audio_status.sample_rate
    );
#endif
    if (snapshot->position_ms > state->package.session.duration_ms)
    {
        snapshot->position_ms = state->package.session.duration_ms;
    }
    snapshot->duration_ms = state->package.session.duration_ms;
    snapshot->buffered_audio_ms = playback_scheduler_frames_to_ms(
        audio_status.available_pcm_frames,
        audio_status.sample_rate
    );
    snapshot->next_video_sample = state->next_video_sample;
    snapshot->queued_video_frames = state->video_count;
    snapshot->dropped_video_frames = state->dropped_video_frames;
    snapshot->speaker_started = speaker_status.started;
    snapshot->queued_speaker_ms = playback_scheduler_frames_to_ms(
        playback_scheduler_speaker_queued_frames(&speaker_status),
        audio_status.sample_rate
    );
    snapshot->submitted_audio_frames = speaker_status.submitted_pcm_frames;
    snapshot->audio_exhausted = audio_status.source_exhausted;
    snapshot->video_exhausted = state->video_exhausted;
    tal_mutex_unlock(state->status_mutex);
    return PLAYBACK_SCHEDULER_OK;
}

void playback_media_scheduler_close(playback_media_scheduler_t *scheduler)
{
    playback_media_scheduler_state_t *state;

    if (scheduler == NULL)
    {
        return;
    }
    state = playback_scheduler_get_state(scheduler);
    scheduler->state = NULL;

    if ((state != NULL) && state->audio_worker_running)
    {
        state->audio_worker_running = false;
        (void)tal_semaphore_wait(state->audio_worker_stopped, 5000U);
        if (state->audio_thread != NULL)
        {
            (void)tal_thread_delete(state->audio_thread);
        }
    }

    playback_scheduler_release_state(state);
}

playback_error_t playback_media_scheduler_result_to_error(
    playback_media_scheduler_result_t result
)
{
    switch (result)
    {
        case PLAYBACK_SCHEDULER_OK:
            return PLAYBACK_ERROR_NONE;
        case PLAYBACK_SCHEDULER_AUDIO_FAILED:
            return PLAYBACK_ERROR_MP3_DECODE_FAILED;
        case PLAYBACK_SCHEDULER_VIDEO_FAILED:
            return PLAYBACK_ERROR_H264_DECODE_FAILED;
        case PLAYBACK_SCHEDULER_INVALID_ARGUMENT:
        case PLAYBACK_SCHEDULER_ALREADY_PREPARED:
        case PLAYBACK_SCHEDULER_NOT_PREPARED:
        case PLAYBACK_SCHEDULER_NO_MEMORY:
        case PLAYBACK_SCHEDULER_SPEAKER_FAILED:
        case PLAYBACK_SCHEDULER_OUTPUT_FAILED:
        case PLAYBACK_SCHEDULER_ERROR_STATE:
        default:
            return PLAYBACK_ERROR_INTERNAL;
    }
}

const char *playback_media_scheduler_result_name(
    playback_media_scheduler_result_t result
)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "ALREADY_PREPARED",
        "NOT_PREPARED",
        "NO_MEMORY",
        "AUDIO_FAILED",
        "VIDEO_FAILED",
        "SPEAKER_FAILED",
        "OUTPUT_FAILED",
        "ERROR_STATE",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0])))
               ? names[result]
               : "UNKNOWN";
}
