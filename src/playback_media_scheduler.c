/**
 * @file playback_media_scheduler.c
 * @brief PCM-authoritative MP3/H.264 scheduler for the fixed A2DP speaker.
 */

#include "playback_media_scheduler.h"

#include <limits.h>
#include <string.h>

#include "playback_h264_decoder.h"
#include "playback_mp3_audio.h"
#include "playback_mp4_demux.h"
#include "tal_api.h"

#define PLAYBACK_SCHEDULER_VIDEO_PREROLL_FRAMES (2U)
#define PLAYBACK_SCHEDULER_ANNEX_B_OVERHEAD_BYTES (4U * PLAYBACK_MP4_MAX_NALS_PER_ACCESS_UNIT)

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

    playback_mp3_audio_t audio;
    playback_mp4_demux_t demux;
    playback_h264_decoder_t decoder;
    playback_video_output_t video_output;
    playback_speaker_link_t speaker;
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
    bool video_exhausted;
    bool seeking;
    uint64_t seek_target_ticks;

    playback_state_t state;
    playback_intent_t intent;
    playback_error_t error;
    uint32_t position_ms;
    bool audio_output_enabled;
} playback_media_scheduler_state_t;

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
    playback_scheduler_set_runtime(state, PLAYBACK_STATE_ERROR, error);
    (void)playback_mp3_audio_pause(&state->audio, PLAYBACK_MP3_PAUSE_OUTPUT);
    state->audio_output_enabled = false;
    (void)playback_speaker_link_stop(&state->speaker);
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
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_VIDEO_FAILED,
            playback_h264_result_to_error(h264_result)
        );
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
        state->video_count++;
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
    while ((state->video_count < PLAYBACK_SCHEDULER_VIDEO_QUEUE_CAPACITY) &&
           !state->video_exhausted)
    {
        playback_media_scheduler_result_t result = playback_scheduler_decode_one(state);
        if (result != PLAYBACK_SCHEDULER_OK)
        {
            return result;
        }
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
    uint64_t adjusted = (uint64_t)media_position_ms + state->config.speaker_latency_ms;
    uint32_t presentation_clock = (adjusted > state->package.session.duration_ms)
                                      ? state->package.session.duration_ms
                                      : (uint32_t)adjusted;
    uint8_t due = playback_scheduler_due_video_count(state, presentation_clock);

    while (due > 1U)
    {
        state->video_read = (uint8_t)((state->video_read + 1U) % PLAYBACK_SCHEDULER_VIDEO_QUEUE_CAPACITY);
        state->video_count--;
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
        state->video_count--;
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

static bool playback_scheduler_video_ready(const playback_media_scheduler_state_t *state)
{
    return (state->video_count >= PLAYBACK_SCHEDULER_VIDEO_PREROLL_FRAMES) ||
           (state->video_exhausted && (state->video_count > 0U));
}

static void playback_scheduler_release_state(playback_media_scheduler_state_t *state)
{
    uint8_t index;

    if (state == NULL)
    {
        return;
    }

    playback_speaker_link_close(&state->speaker);
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
    playback_speaker_link_config_t speaker_config;
    playback_mp4_result_t mp4_result;
    playback_h264_result_t h264_result;
    playback_mp3_result_t mp3_result;
    playback_video_output_result_t output_result;
    playback_speaker_link_result_t speaker_result;
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
    if (state->config.speaker_latency_ms == 0U)
    {
        state->config.speaker_latency_ms = PLAYBACK_SCHEDULER_DEFAULT_SPEAKER_LATENCY_MS;
    }
    state->state = PLAYBACK_STATE_LOADING;
    state->intent = package->session.autoplay ? PLAYBACK_INTENT_PLAYING : PLAYBACK_INTENT_PAUSED;
    state->error = PLAYBACK_ERROR_NONE;

    if (tal_mutex_create_init(&state->status_mutex) != OPRT_OK)
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_NO_MEMORY;
    }

    mp3_result = playback_mp3_audio_prepare(
        &state->audio,
        &state->package.session.audio,
        audio_index,
        state->package.session.duration_ms,
        &state->config.http
    );
    if (mp3_result != PLAYBACK_MP3_OK)
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_AUDIO_FAILED;
    }
    (void)playback_mp3_audio_pause(&state->audio, PLAYBACK_MP3_PAUSE_OUTPUT);

    mp4_result = playback_mp4_demux_open(
        &state->demux,
        &state->package.session.video.asset,
        &state->config.http
    );
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

    memset(&speaker_config, 0, sizeof(speaker_config));
    memcpy(
        speaker_config.target_address,
        state->config.speaker_address,
        sizeof(speaker_config.target_address)
    );
    speaker_config.sample_rate = state->package.session.audio.sample_rate;
    speaker_config.channels = state->package.session.audio.channels;
    speaker_config.read_pcm = playback_scheduler_read_pcm;
    speaker_config.context = state;
    speaker_result = playback_speaker_link_init(&state->speaker, &speaker_config);
    if (speaker_result != PLAYBACK_SPEAKER_LINK_OK)
    {
        playback_scheduler_release_state(state);
        return PLAYBACK_SCHEDULER_SPEAKER_FAILED;
    }

    state->audio_output_enabled = false;
    state->state = package->session.autoplay ? PLAYBACK_STATE_BUFFERING : PLAYBACK_STATE_PAUSED;
    scheduler->state = state;
    return PLAYBACK_SCHEDULER_OK;
}

playback_media_scheduler_result_t playback_media_scheduler_play(
    playback_media_scheduler_t *scheduler
)
{
    playback_media_scheduler_state_t *state = playback_scheduler_get_state(scheduler);

    if (state == NULL)
    {
        return PLAYBACK_SCHEDULER_NOT_PREPARED;
    }
    if (state->state == PLAYBACK_STATE_ERROR)
    {
        return PLAYBACK_SCHEDULER_ERROR_STATE;
    }
    if (state->state == PLAYBACK_STATE_COMPLETED)
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
    (void)playback_mp3_audio_resume(&state->audio, PLAYBACK_MP3_PAUSE_FETCH);
    (void)playback_speaker_link_start(&state->speaker);
    return PLAYBACK_SCHEDULER_OK;
}

playback_media_scheduler_result_t playback_media_scheduler_pause(
    playback_media_scheduler_t *scheduler
)
{
    playback_media_scheduler_state_t *state = playback_scheduler_get_state(scheduler);

    if (state == NULL)
    {
        return PLAYBACK_SCHEDULER_NOT_PREPARED;
    }
    if (state->state == PLAYBACK_STATE_ERROR)
    {
        return PLAYBACK_SCHEDULER_ERROR_STATE;
    }

    tal_mutex_lock(state->status_mutex);
    state->intent = PLAYBACK_INTENT_PAUSED;
    state->state = PLAYBACK_STATE_PAUSED;
    tal_mutex_unlock(state->status_mutex);
    playback_scheduler_set_audio_output(state, false);
    (void)playback_speaker_link_stop(&state->speaker);
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
    if (state->state == PLAYBACK_STATE_ERROR)
    {
        return PLAYBACK_SCHEDULER_ERROR_STATE;
    }

    playback_scheduler_set_runtime(state, PLAYBACK_STATE_SEEKING, PLAYBACK_ERROR_NONE);
    playback_scheduler_set_audio_output(state, false);
    (void)playback_speaker_link_stop(&state->speaker);
    (void)playback_mp3_audio_pause(&state->audio, PLAYBACK_MP3_PAUSE_FETCH);

    target_pcm_frame = ((uint64_t)position_ms * state->package.session.audio.sample_rate) / 1000ULL;
    mp3_result = playback_mp3_audio_seek(&state->audio, target_pcm_frame);
    if (mp3_result != PLAYBACK_MP3_OK)
    {
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_AUDIO_FAILED,
            playback_mp3_result_to_error(mp3_result)
        );
    }

    mp4_result = playback_mp4_demux_find_sync_at_or_before(&state->demux, position_ms, &sync_sample);
    if (mp4_result != PLAYBACK_MP4_OK)
    {
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_VIDEO_FAILED,
            playback_mp4_result_to_error(mp4_result)
        );
    }
    h264_result = playback_h264_decoder_reset_at_sync(&state->decoder);
    if (h264_result != PLAYBACK_H264_OK)
    {
        return playback_scheduler_fail(
            state,
            PLAYBACK_SCHEDULER_VIDEO_FAILED,
            playback_h264_result_to_error(h264_result)
        );
    }

    playback_scheduler_clear_video_queue(state);
    state->next_video_sample = sync_sample.index;
    state->video_exhausted = false;
    state->seeking = true;
    state->seek_target_ticks = ((uint64_t)position_ms * state->codec.timescale) / 1000ULL;
    state->position_ms = position_ms;
    (void)playback_mp3_audio_resume(&state->audio, PLAYBACK_MP3_PAUSE_FETCH);

    tal_mutex_lock(state->status_mutex);
    state->state = (state->intent == PLAYBACK_INTENT_PLAYING)
                       ? PLAYBACK_STATE_BUFFERING
                       : PLAYBACK_STATE_PAUSED;
    tal_mutex_unlock(state->status_mutex);
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

playback_media_scheduler_result_t playback_media_scheduler_pump(
    playback_media_scheduler_t *scheduler
)
{
    playback_media_scheduler_state_t *state = playback_scheduler_get_state(scheduler);
    playback_mp3_status_t audio_status;
    playback_speaker_link_status_t speaker_status;
    playback_mp3_result_t mp3_result;
    playback_media_scheduler_result_t result;
    playback_intent_t intent;
    bool audio_complete;

    if (state == NULL)
    {
        return PLAYBACK_SCHEDULER_NOT_PREPARED;
    }

    tal_mutex_lock(state->status_mutex);
    if (state->state == PLAYBACK_STATE_ERROR)
    {
        tal_mutex_unlock(state->status_mutex);
        return PLAYBACK_SCHEDULER_ERROR_STATE;
    }
    intent = state->intent;
    tal_mutex_unlock(state->status_mutex);

    if (playback_mp3_audio_get_status(&state->audio, &audio_status) != PLAYBACK_MP3_OK)
    {
        return playback_scheduler_fail(state, PLAYBACK_SCHEDULER_AUDIO_FAILED, PLAYBACK_ERROR_INTERNAL);
    }
    if (!audio_status.fetch_paused &&
        (audio_status.available_pcm_frames < audio_status.high_water_frames) &&
        !audio_status.source_exhausted)
    {
        mp3_result = playback_mp3_audio_fill(&state->audio);
        if ((mp3_result != PLAYBACK_MP3_OK) && (mp3_result != PLAYBACK_MP3_END_OF_STREAM) &&
            (mp3_result != PLAYBACK_MP3_BUFFER_FULL))
        {
            return playback_scheduler_fail(
                state,
                PLAYBACK_SCHEDULER_AUDIO_FAILED,
                playback_mp3_result_to_error(mp3_result)
            );
        }
    }

    result = playback_scheduler_fill_video(state);
    if (result != PLAYBACK_SCHEDULER_OK)
    {
        return result;
    }
    if (playback_mp3_audio_get_status(&state->audio, &audio_status) != PLAYBACK_MP3_OK)
    {
        return playback_scheduler_fail(state, PLAYBACK_SCHEDULER_AUDIO_FAILED, PLAYBACK_ERROR_INTERNAL);
    }
    if (playback_speaker_link_get_status(&state->speaker, &speaker_status) != PLAYBACK_SPEAKER_LINK_OK)
    {
        return playback_scheduler_fail(state, PLAYBACK_SCHEDULER_SPEAKER_FAILED, PLAYBACK_ERROR_INTERNAL);
    }

    state->position_ms = playback_scheduler_frames_to_ms(
        audio_status.consumed_pcm_frames,
        audio_status.sample_rate
    );
    if (state->position_ms > state->package.session.duration_ms)
    {
        state->position_ms = state->package.session.duration_ms;
    }

    audio_complete = audio_status.source_exhausted &&
                     (audio_status.consumed_pcm_frames >= audio_status.expected_pcm_frames);

    if (intent == PLAYBACK_INTENT_PAUSED)
    {
        playback_scheduler_set_audio_output(state, false);
        if (speaker_status.desired_streaming)
        {
            (void)playback_speaker_link_stop(&state->speaker);
        }
        playback_scheduler_set_runtime(state, PLAYBACK_STATE_PAUSED, PLAYBACK_ERROR_NONE);
        return PLAYBACK_SCHEDULER_OK;
    }

    if (audio_complete)
    {
        playback_scheduler_set_audio_output(state, false);
        if (speaker_status.desired_streaming)
        {
            (void)playback_speaker_link_stop(&state->speaker);
        }
        result = playback_scheduler_present_due(state, state->package.session.duration_ms);
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
        return PLAYBACK_SCHEDULER_OK;
    }

    if (!playback_scheduler_audio_ready(&audio_status) || !playback_scheduler_video_ready(state))
    {
        playback_scheduler_set_audio_output(state, false);
        if (speaker_status.desired_streaming)
        {
            (void)playback_speaker_link_stop(&state->speaker);
        }
        playback_scheduler_set_runtime(state, PLAYBACK_STATE_BUFFERING, PLAYBACK_ERROR_NONE);
        return PLAYBACK_SCHEDULER_OK;
    }

    if (!speaker_status.desired_streaming)
    {
        (void)playback_speaker_link_start(&state->speaker);
    }
    if (speaker_status.state != PLAYBACK_SPEAKER_STREAMING)
    {
        playback_scheduler_set_audio_output(state, false);
        playback_scheduler_set_runtime(state, PLAYBACK_STATE_WAITING_SPEAKER, PLAYBACK_ERROR_NONE);
        return PLAYBACK_SCHEDULER_OK;
    }

    playback_scheduler_set_audio_output(state, true);
    playback_scheduler_set_runtime(state, PLAYBACK_STATE_PLAYING, PLAYBACK_ERROR_NONE);
    result = playback_scheduler_present_due(state, state->position_ms);
    return result;
}

playback_media_scheduler_result_t playback_media_scheduler_get_snapshot(
    playback_media_scheduler_t *scheduler,
    playback_media_scheduler_snapshot_t *snapshot
)
{
    playback_media_scheduler_state_t *state = playback_scheduler_get_state(scheduler);
    playback_mp3_status_t audio_status;
    playback_speaker_link_status_t speaker_status;

    if ((state == NULL) || (snapshot == NULL))
    {
        return (scheduler == NULL || snapshot == NULL)
                   ? PLAYBACK_SCHEDULER_INVALID_ARGUMENT
                   : PLAYBACK_SCHEDULER_NOT_PREPARED;
    }
    if (playback_mp3_audio_get_status(&state->audio, &audio_status) != PLAYBACK_MP3_OK)
    {
        return PLAYBACK_SCHEDULER_AUDIO_FAILED;
    }
    if (playback_speaker_link_get_status(&state->speaker, &speaker_status) != PLAYBACK_SPEAKER_LINK_OK)
    {
        return PLAYBACK_SCHEDULER_SPEAKER_FAILED;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    tal_mutex_lock(state->status_mutex);
    snapshot->prepared = true;
    snapshot->state = state->state;
    snapshot->intent = state->intent;
    snapshot->error = state->error;
    snapshot->position_ms = playback_scheduler_frames_to_ms(
        audio_status.consumed_pcm_frames,
        audio_status.sample_rate
    );
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
    snapshot->speaker_state = speaker_status.state;
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
