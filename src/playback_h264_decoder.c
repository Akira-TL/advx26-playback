/**
 * @file playback_h264_decoder.c
 * @brief Replaceable Baseline H.264 decoder adapter backed by tinyh264.
 */

#include "playback_h264_decoder.h"

#include <limits.h>
#include <string.h>

#include "h264bsd_decoder.h"
#include "h264bsd_util.h"
#include "playback_media_package.h"
#include "tal_api.h"

#define PLAYBACK_H264_ANNEX_B_START_CODE_LENGTH (4U)
#define PLAYBACK_H264_NAL_TYPE_NON_IDR (1U)
#define PLAYBACK_H264_NAL_TYPE_IDR (5U)
#define PLAYBACK_H264_NAL_TYPE_SEI (6U)
#define PLAYBACK_H264_NAL_TYPE_SPS (7U)
#define PLAYBACK_H264_NAL_TYPE_PPS (8U)
#define PLAYBACK_H264_NAL_TYPE_AUD (9U)
#define PLAYBACK_H264_NAL_TYPE_END_SEQUENCE (10U)
#define PLAYBACK_H264_NAL_TYPE_END_STREAM (11U)
#define PLAYBACK_H264_NAL_TYPE_FILLER (12U)

typedef struct
{
    bool has_sps;
    bool has_pps;
    bool has_vcl;
    bool has_idr;
    bool has_non_idr;
    uint32_t nal_count;
} playback_h264_annex_b_info_t;

typedef struct
{
    storage_t *backend;
    uint8_t *parameter_sets;
    size_t parameter_sets_length;
    uint8_t *decode_buffer;
    size_t decode_buffer_capacity;
    size_t max_access_unit_length;
    uint16_t width;
    uint16_t height;
    uint8_t profile_idc;
    uint8_t level_idc;
    uint32_t timescale;
    uint32_t generation;
    uint32_t next_lease_id;
    bool prepend_parameter_sets;
    bool require_sync;
    bool reset_required;
    bool frame_pending;
    playback_h264_frame_t pending_frame;
} playback_h264_state_t;

static bool playback_h264_size_add(size_t left, size_t right, size_t *result)
{
    if ((result == NULL) || (SIZE_MAX - left < right))
    {
        return false;
    }

    *result = left + right;
    return true;
}

static playback_h264_state_t *playback_h264_get_state(const playback_h264_decoder_t *decoder)
{
    return (decoder == NULL) ? NULL : (playback_h264_state_t *)decoder->state;
}

static bool playback_h264_has_start_code(const uint8_t *data, size_t length, size_t offset)
{
    return (data != NULL) && (offset <= length) && (length - offset >= PLAYBACK_H264_ANNEX_B_START_CODE_LENGTH) &&
           (data[offset] == 0U) && (data[offset + 1U] == 0U) && (data[offset + 2U] == 0U) &&
           (data[offset + 3U] == 1U);
}

static bool playback_h264_access_unit_nal_allowed(uint8_t nal_type)
{
    switch (nal_type)
    {
        case PLAYBACK_H264_NAL_TYPE_NON_IDR:
        case PLAYBACK_H264_NAL_TYPE_IDR:
        case PLAYBACK_H264_NAL_TYPE_SPS:
        case PLAYBACK_H264_NAL_TYPE_PPS:
        case PLAYBACK_H264_NAL_TYPE_SEI:
        case PLAYBACK_H264_NAL_TYPE_AUD:
        case PLAYBACK_H264_NAL_TYPE_END_SEQUENCE:
        case PLAYBACK_H264_NAL_TYPE_END_STREAM:
        case PLAYBACK_H264_NAL_TYPE_FILLER:
            return true;
        default:
            return false;
    }
}

static playback_h264_result_t playback_h264_inspect_annex_b(
    const uint8_t *data,
    size_t length,
    bool parameter_sets_only,
    playback_h264_annex_b_info_t *info
)
{
    size_t cursor = 0U;

    if ((data == NULL) || (info == NULL) || (length < (PLAYBACK_H264_ANNEX_B_START_CODE_LENGTH + 1U)) ||
        !playback_h264_has_start_code(data, length, 0U))
    {
        return PLAYBACK_H264_INVALID_ACCESS_UNIT;
    }

    memset(info, 0, sizeof(*info));
    while (cursor < length)
    {
        size_t nal_start;
        size_t next_start;
        size_t nal_end;
        uint8_t nal_header;
        uint8_t nal_type;

        if (!playback_h264_has_start_code(data, length, cursor))
        {
            return PLAYBACK_H264_INVALID_ACCESS_UNIT;
        }
        nal_start = cursor + PLAYBACK_H264_ANNEX_B_START_CODE_LENGTH;
        if (nal_start >= length)
        {
            return PLAYBACK_H264_INVALID_ACCESS_UNIT;
        }

        next_start = nal_start + 1U;
        while ((next_start < length) && !playback_h264_has_start_code(data, length, next_start))
        {
            next_start++;
        }
        nal_end = playback_h264_has_start_code(data, length, next_start) ? next_start : length;
        if (nal_end <= nal_start)
        {
            return PLAYBACK_H264_INVALID_ACCESS_UNIT;
        }

        nal_header = data[nal_start];
        nal_type = (uint8_t)(nal_header & 0x1FU);
        if (((nal_header & 0x80U) != 0U) || (nal_type == 0U) || (nal_type >= 24U))
        {
            return PLAYBACK_H264_INVALID_ACCESS_UNIT;
        }

        if (parameter_sets_only)
        {
            if ((nal_type != PLAYBACK_H264_NAL_TYPE_SPS) && (nal_type != PLAYBACK_H264_NAL_TYPE_PPS))
            {
                return PLAYBACK_H264_INVALID_ACCESS_UNIT;
            }
        }
        else if (!playback_h264_access_unit_nal_allowed(nal_type))
        {
            return PLAYBACK_H264_INVALID_ACCESS_UNIT;
        }

        info->has_sps = info->has_sps || (nal_type == PLAYBACK_H264_NAL_TYPE_SPS);
        info->has_pps = info->has_pps || (nal_type == PLAYBACK_H264_NAL_TYPE_PPS);
        info->has_vcl = info->has_vcl || (nal_type == PLAYBACK_H264_NAL_TYPE_NON_IDR) ||
                        (nal_type == PLAYBACK_H264_NAL_TYPE_IDR);
        info->has_idr = info->has_idr || (nal_type == PLAYBACK_H264_NAL_TYPE_IDR);
        info->has_non_idr = info->has_non_idr || (nal_type == PLAYBACK_H264_NAL_TYPE_NON_IDR);
        info->nal_count++;
        if (info->nal_count > PLAYBACK_MP4_MAX_NALS_PER_ACCESS_UNIT)
        {
            return PLAYBACK_H264_INVALID_ACCESS_UNIT;
        }

        cursor = nal_end;
    }

    if (parameter_sets_only)
    {
        return (info->has_sps && info->has_pps && !info->has_vcl) ? PLAYBACK_H264_OK
                                                                 : PLAYBACK_H264_INVALID_ACCESS_UNIT;
    }

    return (info->has_vcl && !(info->has_idr && info->has_non_idr)) ? PLAYBACK_H264_OK
                                                                    : PLAYBACK_H264_INVALID_ACCESS_UNIT;
}

static playback_h264_result_t playback_h264_validate_config(
    const playback_video_descriptor_t *descriptor,
    const playback_mp4_codec_config_t *codec_config,
    const uint8_t *parameter_sets_annex_b,
    size_t parameter_sets_length,
    size_t *max_access_unit_length,
    size_t *decode_buffer_capacity
)
{
    playback_h264_annex_b_info_t parameter_info;
    size_t annex_b_overhead;
    uint32_t max_fps_numerator;

    if ((descriptor == NULL) || (codec_config == NULL) || (parameter_sets_annex_b == NULL) ||
        (max_access_unit_length == NULL) || (decode_buffer_capacity == NULL) ||
        (parameter_sets_length == 0U) || (parameter_sets_length > PLAYBACK_MP4_MAX_CODEC_CONFIG_BYTES))
    {
        return PLAYBACK_H264_INVALID_ARGUMENT;
    }

    max_fps_numerator = (uint32_t)descriptor->fps_den * PLAYBACK_VIDEO_MAX_FPS;
    if ((descriptor->width != PLAYBACK_VIDEO_WIDTH) || (descriptor->height != PLAYBACK_VIDEO_HEIGHT) ||
        (descriptor->fps_num == 0U) || (descriptor->fps_den == 0U) ||
        ((uint32_t)descriptor->fps_num > max_fps_numerator) ||
        (descriptor->h264_profile_idc != PLAYBACK_H264_BASELINE_PROFILE_IDC) || !descriptor->yuv420p ||
        descriptor->has_b_frames || (descriptor->max_keyframe_interval_ms == 0U) ||
        (descriptor->max_keyframe_interval_ms > PLAYBACK_H264_MAX_KEYFRAME_INTERVAL_MS))
    {
        return PLAYBACK_H264_UNSUPPORTED_CONFIG;
    }

    if ((codec_config->width != descriptor->width) || (codec_config->height != descriptor->height) ||
        (codec_config->profile_idc != descriptor->h264_profile_idc) ||
        (codec_config->level_idc != descriptor->h264_level_idc) ||
        (codec_config->profile_idc != PLAYBACK_H264_BASELINE_PROFILE_IDC) || (codec_config->timescale == 0U) ||
        (codec_config->duration_ticks == 0U) || (codec_config->sample_count == 0U) ||
        (codec_config->max_sample_size == 0U) ||
        (codec_config->max_sample_size > PLAYBACK_HTTP_RANGE_MAX_LENGTH) ||
        (codec_config->parameter_sets_annex_b_length != parameter_sets_length))
    {
        return PLAYBACK_H264_UNSUPPORTED_CONFIG;
    }

    if (playback_h264_inspect_annex_b(
            parameter_sets_annex_b,
            parameter_sets_length,
            true,
            &parameter_info
        ) != PLAYBACK_H264_OK)
    {
        return PLAYBACK_H264_UNSUPPORTED_CONFIG;
    }

    annex_b_overhead = (size_t)PLAYBACK_MP4_MAX_NALS_PER_ACCESS_UNIT * PLAYBACK_H264_ANNEX_B_START_CODE_LENGTH;
    if (!playback_h264_size_add(codec_config->max_sample_size, annex_b_overhead, max_access_unit_length) ||
        !playback_h264_size_add(*max_access_unit_length, parameter_sets_length, decode_buffer_capacity))
    {
        return PLAYBACK_H264_UNSUPPORTED_CONFIG;
    }

    return PLAYBACK_H264_OK;
}

static playback_h264_result_t playback_h264_backend_create(playback_h264_state_t *state)
{
    if (state == NULL)
    {
        return PLAYBACK_H264_INVALID_ARGUMENT;
    }

    state->backend = h264bsdAlloc();
    if (state->backend == NULL)
    {
        return PLAYBACK_H264_NO_MEMORY;
    }
    if (h264bsdInit(state->backend, 1U) != HANTRO_OK)
    {
        h264bsdFree(state->backend);
        state->backend = NULL;
        return PLAYBACK_H264_NO_MEMORY;
    }

    return PLAYBACK_H264_OK;
}

static void playback_h264_backend_destroy(playback_h264_state_t *state)
{
    if ((state == NULL) || (state->backend == NULL))
    {
        return;
    }

    h264bsdShutdown(state->backend);
    h264bsdFree(state->backend);
    state->backend = NULL;
}

static void playback_h264_state_release(playback_h264_state_t *state)
{
    if (state == NULL)
    {
        return;
    }

    playback_h264_backend_destroy(state);
    if (state->decode_buffer != NULL)
    {
        tal_psram_free(state->decode_buffer);
    }
    if (state->parameter_sets != NULL)
    {
        tal_psram_free(state->parameter_sets);
    }
    tal_free(state);
}

static void playback_h264_mark_reset_required(playback_h264_state_t *state)
{
    if (state == NULL)
    {
        return;
    }

    state->reset_required = true;
    state->require_sync = true;
    state->prepend_parameter_sets = true;
    state->frame_pending = false;
    memset(&state->pending_frame, 0, sizeof(state->pending_frame));
}

playback_h264_result_t playback_h264_decoder_open(
    playback_h264_decoder_t *decoder,
    const playback_video_descriptor_t *descriptor,
    const playback_mp4_codec_config_t *codec_config,
    const uint8_t *parameter_sets_annex_b,
    size_t parameter_sets_length
)
{
    playback_h264_state_t *state;
    playback_h264_result_t result;
    size_t max_access_unit_length;
    size_t decode_buffer_capacity;

    if ((decoder == NULL) || (decoder->state != NULL))
    {
        return PLAYBACK_H264_INVALID_ARGUMENT;
    }

    result = playback_h264_validate_config(
        descriptor,
        codec_config,
        parameter_sets_annex_b,
        parameter_sets_length,
        &max_access_unit_length,
        &decode_buffer_capacity
    );
    if (result != PLAYBACK_H264_OK)
    {
        return result;
    }

    state = tal_calloc(1U, sizeof(*state));
    if (state == NULL)
    {
        return PLAYBACK_H264_NO_MEMORY;
    }

    state->parameter_sets = tal_psram_malloc(parameter_sets_length);
    state->decode_buffer = tal_psram_malloc(decode_buffer_capacity);
    if ((state->parameter_sets == NULL) || (state->decode_buffer == NULL))
    {
        playback_h264_state_release(state);
        return PLAYBACK_H264_NO_MEMORY;
    }

    memcpy(state->parameter_sets, parameter_sets_annex_b, parameter_sets_length);
    state->parameter_sets_length = parameter_sets_length;
    state->decode_buffer_capacity = decode_buffer_capacity;
    state->max_access_unit_length = max_access_unit_length;
    state->width = codec_config->width;
    state->height = codec_config->height;
    state->profile_idc = codec_config->profile_idc;
    state->level_idc = codec_config->level_idc;
    state->timescale = codec_config->timescale;
    state->generation = 1U;
    state->next_lease_id = 1U;
    state->prepend_parameter_sets = true;
    state->require_sync = true;

    result = playback_h264_backend_create(state);
    if (result != PLAYBACK_H264_OK)
    {
        playback_h264_state_release(state);
        return result;
    }

    decoder->state = state;
    return PLAYBACK_H264_OK;
}

playback_h264_result_t playback_h264_decoder_submit_access_unit(
    playback_h264_decoder_t *decoder,
    const uint8_t *access_unit_annex_b,
    size_t access_unit_length,
    uint64_t pts,
    uint32_t duration,
    bool is_sync
)
{
    playback_h264_state_t *state = playback_h264_get_state(decoder);
    playback_h264_annex_b_info_t access_unit_info;
    size_t input_length = access_unit_length;
    size_t frame_y_length;
    size_t frame_chroma_length;
    uint8_t *picture = NULL;
    u32 decoded_width = 0U;
    u32 decoded_height = 0U;
    u32 backend_result;
    playback_h264_result_t result;

    if ((state == NULL) || (access_unit_annex_b == NULL) || (access_unit_length == 0U) || (duration == 0U))
    {
        return PLAYBACK_H264_INVALID_ARGUMENT;
    }
    if (state->frame_pending)
    {
        return PLAYBACK_H264_FRAME_PENDING;
    }
    if (state->reset_required)
    {
        return PLAYBACK_H264_RESET_REQUIRED;
    }
    if (state->require_sync && !is_sync)
    {
        return PLAYBACK_H264_NEED_SYNC;
    }
    if (access_unit_length > state->max_access_unit_length)
    {
        return PLAYBACK_H264_INVALID_ACCESS_UNIT;
    }

    result = playback_h264_inspect_annex_b(
        access_unit_annex_b,
        access_unit_length,
        false,
        &access_unit_info
    );
    if ((result != PLAYBACK_H264_OK) || (access_unit_info.has_idr != is_sync))
    {
        return PLAYBACK_H264_INVALID_ACCESS_UNIT;
    }

    if (state->prepend_parameter_sets &&
        !(access_unit_info.has_sps && access_unit_info.has_pps))
    {
        if (!playback_h264_size_add(state->parameter_sets_length, access_unit_length, &input_length) ||
            (input_length > state->decode_buffer_capacity))
        {
            return PLAYBACK_H264_INVALID_ACCESS_UNIT;
        }
        memcpy(state->decode_buffer, state->parameter_sets, state->parameter_sets_length);
        memcpy(
            state->decode_buffer + state->parameter_sets_length,
            access_unit_annex_b,
            access_unit_length
        );
    }
    else
    {
        if (access_unit_length > state->decode_buffer_capacity)
        {
            return PLAYBACK_H264_INVALID_ACCESS_UNIT;
        }
        memcpy(state->decode_buffer, access_unit_annex_b, access_unit_length);
    }

    backend_result = h264bsdDecode(
        state->backend,
        state->decode_buffer,
        (uint32_t)input_length,
        &picture,
        &decoded_width,
        &decoded_height
    );
    if (backend_result != H264BSD_PIC_RDY)
    {
        playback_h264_mark_reset_required(state);
        return (backend_result == H264BSD_MEMALLOC_ERROR) ? PLAYBACK_H264_NO_MEMORY
                                                          : PLAYBACK_H264_DECODE_FAILED;
    }

    if ((picture == NULL) || (decoded_width != state->width) || (decoded_height != state->height) ||
        ((decoded_width & 1U) != 0U) || ((decoded_height & 1U) != 0U) ||
        (state->backend->activeSps == NULL) ||
        (state->backend->activeSps->profileIdc != state->profile_idc) ||
        (state->backend->activeSps->levelIdc != state->level_idc))
    {
        playback_h264_mark_reset_required(state);
        return PLAYBACK_H264_OUTPUT_INVALID;
    }

    frame_y_length = (size_t)decoded_width * decoded_height;
    frame_chroma_length = frame_y_length / 4U;
    memset(&state->pending_frame, 0, sizeof(state->pending_frame));
    state->pending_frame.pixel_format = PLAYBACK_VIDEO_PIXEL_FORMAT_YUV420P;
    state->pending_frame.planes[0] = picture;
    state->pending_frame.planes[1] = picture + frame_y_length;
    state->pending_frame.planes[2] = picture + frame_y_length + frame_chroma_length;
    state->pending_frame.strides[0] = decoded_width;
    state->pending_frame.strides[1] = decoded_width / 2U;
    state->pending_frame.strides[2] = decoded_width / 2U;
    state->pending_frame.plane_lengths[0] = frame_y_length;
    state->pending_frame.plane_lengths[1] = frame_chroma_length;
    state->pending_frame.plane_lengths[2] = frame_chroma_length;
    state->pending_frame.width = (uint16_t)decoded_width;
    state->pending_frame.height = (uint16_t)decoded_height;
    state->pending_frame.timescale = state->timescale;
    state->pending_frame.pts = pts;
    state->pending_frame.duration = duration;
    state->pending_frame.generation = state->generation;
    state->pending_frame.lease_id = state->next_lease_id;
    state->pending_frame.is_sync = is_sync;

    state->next_lease_id++;
    if (state->next_lease_id == 0U)
    {
        state->next_lease_id = 1U;
    }
    state->prepend_parameter_sets = false;
    state->require_sync = false;
    state->frame_pending = true;
    return PLAYBACK_H264_OK;
}

playback_h264_result_t playback_h264_decoder_poll_frame(
    const playback_h264_decoder_t *decoder,
    playback_h264_frame_t *frame
)
{
    const playback_h264_state_t *state = playback_h264_get_state(decoder);

    if ((state == NULL) || (frame == NULL))
    {
        return PLAYBACK_H264_INVALID_ARGUMENT;
    }
    if (!state->frame_pending)
    {
        return PLAYBACK_H264_NO_FRAME;
    }

    *frame = state->pending_frame;
    return PLAYBACK_H264_OK;
}

playback_h264_result_t playback_h264_decoder_release_frame(
    playback_h264_decoder_t *decoder,
    const playback_h264_frame_t *frame
)
{
    playback_h264_state_t *state = playback_h264_get_state(decoder);

    if ((state == NULL) || (frame == NULL))
    {
        return PLAYBACK_H264_INVALID_ARGUMENT;
    }
    if (!state->frame_pending || (frame->generation != state->pending_frame.generation) ||
        (frame->lease_id != state->pending_frame.lease_id) ||
        (frame->planes[0] != state->pending_frame.planes[0]))
    {
        return PLAYBACK_H264_STALE_FRAME;
    }

    state->frame_pending = false;
    memset(&state->pending_frame, 0, sizeof(state->pending_frame));
    return PLAYBACK_H264_OK;
}

playback_h264_result_t playback_h264_decoder_reset_at_sync(playback_h264_decoder_t *decoder)
{
    playback_h264_state_t *state = playback_h264_get_state(decoder);
    playback_h264_result_t result;

    if (state == NULL)
    {
        return PLAYBACK_H264_INVALID_ARGUMENT;
    }
    if (state->frame_pending)
    {
        return PLAYBACK_H264_FRAME_PENDING;
    }

    playback_h264_backend_destroy(state);
    result = playback_h264_backend_create(state);
    state->generation++;
    if (state->generation == 0U)
    {
        state->generation = 1U;
    }
    state->prepend_parameter_sets = true;
    state->require_sync = true;
    state->reset_required = result != PLAYBACK_H264_OK;
    return result;
}

void playback_h264_decoder_close(playback_h264_decoder_t *decoder)
{
    playback_h264_state_t *state;

    if (decoder == NULL)
    {
        return;
    }

    state = playback_h264_get_state(decoder);
    decoder->state = NULL;
    playback_h264_state_release(state);
}

playback_error_t playback_h264_result_to_error(playback_h264_result_t result)
{
    switch (result)
    {
        case PLAYBACK_H264_OK:
        case PLAYBACK_H264_NEED_SYNC:
        case PLAYBACK_H264_FRAME_PENDING:
        case PLAYBACK_H264_NO_FRAME:
        case PLAYBACK_H264_STALE_FRAME:
        case PLAYBACK_H264_RESET_REQUIRED:
            return PLAYBACK_ERROR_NONE;
        case PLAYBACK_H264_UNSUPPORTED_CONFIG:
        case PLAYBACK_H264_NO_MEMORY:
        case PLAYBACK_H264_INVALID_ACCESS_UNIT:
        case PLAYBACK_H264_DECODE_FAILED:
        case PLAYBACK_H264_OUTPUT_INVALID:
            return PLAYBACK_ERROR_H264_DECODE_FAILED;
        case PLAYBACK_H264_INVALID_ARGUMENT:
        default:
            return PLAYBACK_ERROR_INTERNAL;
    }
}

const char *playback_h264_result_name(playback_h264_result_t result)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "UNSUPPORTED_CONFIG",
        "NO_MEMORY",
        "NEED_SYNC",
        "FRAME_PENDING",
        "NO_FRAME",
        "STALE_FRAME",
        "INVALID_ACCESS_UNIT",
        "RESET_REQUIRED",
        "DECODE_FAILED",
        "OUTPUT_INVALID",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0]))) ? names[result] : "UNKNOWN_H264_RESULT";
}
