/**
 * @file playback_video_output.c
 * @brief Replaceable YUV420P to RGB565 video output adapter.
 */

#include "playback_video_output.h"

#include <limits.h>
#include <string.h>

#include "tal_api.h"

typedef struct
{
    playback_video_output_config_t config;
    playback_video_sink_present_fn present;
    void *sink_context;
    uint16_t *buffers[PLAYBACK_VIDEO_OUTPUT_BUFFER_COUNT];
    size_t pixel_count;
    uint16_t output_width;
    uint16_t output_height;
    uint32_t next_surface_id;
    uint8_t active_buffer;
    bool has_active_surface;
    playback_rgb565_surface_t active_surface;
} playback_video_output_state_t;

static playback_video_output_state_t *playback_video_output_get_state(const playback_video_output_t *output)
{
    return (output == NULL) ? NULL : (playback_video_output_state_t *)output->state;
}

static uint8_t playback_video_clamp_u8(int32_t value)
{
    if (value < 0)
    {
        return 0U;
    }
    if (value > 255)
    {
        return 255U;
    }
    return (uint8_t)value;
}

static uint16_t playback_video_pack_rgb565(uint8_t red, uint8_t green, uint8_t blue, bool swap_bytes)
{
    uint16_t pixel = (uint16_t)(((uint16_t)(red & 0xF8U) << 8U) |
                                ((uint16_t)(green & 0xFCU) << 3U) |
                                ((uint16_t)blue >> 3U));

    if (swap_bytes)
    {
        pixel = (uint16_t)((pixel << 8U) | (pixel >> 8U));
    }
    return pixel;
}

static uint16_t playback_video_yuv_to_rgb565(uint8_t y, uint8_t u, uint8_t v, bool swap_bytes)
{
    int32_t luminance = (int32_t)y - 16;
    int32_t chroma_u = (int32_t)u - 128;
    int32_t chroma_v = (int32_t)v - 128;
    int32_t red;
    int32_t green;
    int32_t blue;

    if (luminance < 0)
    {
        luminance = 0;
    }

    red = (298 * luminance + 409 * chroma_v + 128) / 256;
    green = (298 * luminance - 100 * chroma_u - 208 * chroma_v + 128) / 256;
    blue = (298 * luminance + 516 * chroma_u + 128) / 256;
    return playback_video_pack_rgb565(
        playback_video_clamp_u8(red),
        playback_video_clamp_u8(green),
        playback_video_clamp_u8(blue),
        swap_bytes
    );
}

static void playback_video_map_destination(
    const playback_video_output_state_t *state,
    uint16_t source_x,
    uint16_t source_y,
    uint16_t *destination_x,
    uint16_t *destination_y
)
{
    switch (state->config.rotation)
    {
        case PLAYBACK_VIDEO_ROTATION_0:
            *destination_x = source_x;
            *destination_y = source_y;
            break;
        case PLAYBACK_VIDEO_ROTATION_90:
            *destination_x = (uint16_t)(state->config.source_height - 1U - source_y);
            *destination_y = source_x;
            break;
        case PLAYBACK_VIDEO_ROTATION_180:
            *destination_x = (uint16_t)(state->config.source_width - 1U - source_x);
            *destination_y = (uint16_t)(state->config.source_height - 1U - source_y);
            break;
        case PLAYBACK_VIDEO_ROTATION_270:
            *destination_x = source_y;
            *destination_y = (uint16_t)(state->config.source_width - 1U - source_x);
            break;
        default:
            *destination_x = 0U;
            *destination_y = 0U;
            break;
    }
}

static bool playback_video_plane_contains(
    size_t plane_length,
    uint32_t stride,
    uint16_t width,
    uint16_t height
)
{
    size_t last_row_offset;
    size_t required;

    if ((stride < width) || (width == 0U) || (height == 0U))
    {
        return false;
    }

    last_row_offset = (size_t)stride * (height - 1U);
    if ((SIZE_MAX - last_row_offset < width) || (last_row_offset + width > plane_length))
    {
        return false;
    }

    required = last_row_offset + width;
    return required <= plane_length;
}

static playback_video_output_result_t playback_video_validate_frame(
    const playback_video_output_state_t *state,
    const playback_h264_frame_t *frame
)
{
    uint16_t chroma_width;
    uint16_t chroma_height;

    if ((state == NULL) || (frame == NULL))
    {
        return PLAYBACK_VIDEO_OUTPUT_INVALID_ARGUMENT;
    }
    if ((frame->pixel_format != PLAYBACK_VIDEO_PIXEL_FORMAT_YUV420P) ||
        (frame->width != state->config.source_width) || (frame->height != state->config.source_height) ||
        ((frame->width & 1U) != 0U) || ((frame->height & 1U) != 0U) || (frame->timescale == 0U) ||
        (frame->duration == 0U) || (frame->generation == 0U) || (frame->lease_id == 0U) ||
        (frame->planes[0] == NULL) || (frame->planes[1] == NULL) || (frame->planes[2] == NULL))
    {
        return PLAYBACK_VIDEO_OUTPUT_INVALID_FRAME;
    }

    chroma_width = frame->width / 2U;
    chroma_height = frame->height / 2U;
    if (!playback_video_plane_contains(
            frame->plane_lengths[0],
            frame->strides[0],
            frame->width,
            frame->height
        ) ||
        !playback_video_plane_contains(
            frame->plane_lengths[1],
            frame->strides[1],
            chroma_width,
            chroma_height
        ) ||
        !playback_video_plane_contains(
            frame->plane_lengths[2],
            frame->strides[2],
            chroma_width,
            chroma_height
        ))
    {
        return PLAYBACK_VIDEO_OUTPUT_INVALID_FRAME;
    }

    if (state->has_active_surface &&
        ((frame->generation < state->active_surface.generation) ||
         ((frame->generation == state->active_surface.generation) && (frame->pts <= state->active_surface.pts))))
    {
        return PLAYBACK_VIDEO_OUTPUT_STALE_FRAME;
    }

    return PLAYBACK_VIDEO_OUTPUT_OK;
}

static void playback_video_convert_frame(
    const playback_video_output_state_t *state,
    const playback_h264_frame_t *frame,
    uint16_t *destination
)
{
    uint16_t source_y;

    for (source_y = 0U; source_y < frame->height; source_y += 2U)
    {
        const uint8_t *y_row_0 = frame->planes[0] + ((size_t)source_y * frame->strides[0]);
        const uint8_t *y_row_1 = y_row_0 + frame->strides[0];
        const uint8_t *u_row = frame->planes[1] + ((size_t)(source_y / 2U) * frame->strides[1]);
        const uint8_t *v_row = frame->planes[2] + ((size_t)(source_y / 2U) * frame->strides[2]);
        uint16_t source_x;

        for (source_x = 0U; source_x < frame->width; source_x += 2U)
        {
            uint8_t u = u_row[source_x / 2U];
            uint8_t v = v_row[source_x / 2U];
            uint16_t block_y;

            for (block_y = 0U; block_y < 2U; ++block_y)
            {
                const uint8_t *y_row = (block_y == 0U) ? y_row_0 : y_row_1;
                uint16_t block_x;

                for (block_x = 0U; block_x < 2U; ++block_x)
                {
                    uint16_t destination_x;
                    uint16_t destination_y;
                    size_t destination_index;

                    playback_video_map_destination(
                        state,
                        (uint16_t)(source_x + block_x),
                        (uint16_t)(source_y + block_y),
                        &destination_x,
                        &destination_y
                    );
                    destination_index = ((size_t)destination_y * state->output_width) + destination_x;
                    destination[destination_index] = playback_video_yuv_to_rgb565(
                        y_row[source_x + block_x],
                        u,
                        v,
                        state->config.swap_rgb565_bytes
                    );
                }
            }
        }
    }
}

playback_video_output_result_t playback_video_output_open(
    playback_video_output_t *output,
    const playback_video_output_config_t *config,
    playback_video_sink_present_fn present,
    void *sink_context
)
{
    playback_video_output_state_t *state;
    size_t buffer_bytes;
    uint8_t index;

    if ((output == NULL) || (config == NULL) || (present == NULL) || (output->state != NULL))
    {
        return PLAYBACK_VIDEO_OUTPUT_INVALID_ARGUMENT;
    }
    if ((config->source_width == 0U) || (config->source_height == 0U) ||
        ((config->source_width & 1U) != 0U) || ((config->source_height & 1U) != 0U) ||
        (config->source_width > PLAYBACK_VIDEO_WIDTH) || (config->source_height > PLAYBACK_VIDEO_HEIGHT) ||
        (config->rotation > PLAYBACK_VIDEO_ROTATION_270))
    {
        return PLAYBACK_VIDEO_OUTPUT_UNSUPPORTED_CONFIG;
    }

    state = tal_calloc(1U, sizeof(*state));
    if (state == NULL)
    {
        return PLAYBACK_VIDEO_OUTPUT_NO_MEMORY;
    }

    state->config = *config;
    state->present = present;
    state->sink_context = sink_context;
    if ((config->rotation == PLAYBACK_VIDEO_ROTATION_90) ||
        (config->rotation == PLAYBACK_VIDEO_ROTATION_270))
    {
        state->output_width = config->source_height;
        state->output_height = config->source_width;
    }
    else
    {
        state->output_width = config->source_width;
        state->output_height = config->source_height;
    }

    state->pixel_count = (size_t)state->output_width * state->output_height;
    if ((state->pixel_count == 0U) || (state->pixel_count > (SIZE_MAX / sizeof(uint16_t))))
    {
        tal_free(state);
        return PLAYBACK_VIDEO_OUTPUT_UNSUPPORTED_CONFIG;
    }
    buffer_bytes = state->pixel_count * sizeof(uint16_t);

    for (index = 0U; index < PLAYBACK_VIDEO_OUTPUT_BUFFER_COUNT; ++index)
    {
        state->buffers[index] = tal_psram_malloc(buffer_bytes);
        if (state->buffers[index] == NULL)
        {
            playback_video_output_t temporary = {.state = state};
            playback_video_output_close(&temporary);
            return PLAYBACK_VIDEO_OUTPUT_NO_MEMORY;
        }
        memset(state->buffers[index], 0, buffer_bytes);
    }

    state->next_surface_id = 1U;
    output->state = state;
    return PLAYBACK_VIDEO_OUTPUT_OK;
}

playback_video_output_result_t playback_video_output_present(
    playback_video_output_t *output,
    const playback_h264_frame_t *frame
)
{
    playback_video_output_state_t *state = playback_video_output_get_state(output);
    playback_rgb565_surface_t candidate;
    playback_video_output_result_t result;
    uint8_t staging_buffer;

    result = playback_video_validate_frame(state, frame);
    if (result != PLAYBACK_VIDEO_OUTPUT_OK)
    {
        return result;
    }

    staging_buffer = state->has_active_surface ? (uint8_t)(state->active_buffer ^ 1U) : 0U;
    playback_video_convert_frame(state, frame, state->buffers[staging_buffer]);

    memset(&candidate, 0, sizeof(candidate));
    candidate.pixels = state->buffers[staging_buffer];
    candidate.pixel_count = state->pixel_count;
    candidate.width = state->output_width;
    candidate.height = state->output_height;
    candidate.stride_bytes = (uint32_t)state->output_width * sizeof(uint16_t);
    candidate.timescale = frame->timescale;
    candidate.pts = frame->pts;
    candidate.duration = frame->duration;
    candidate.generation = frame->generation;
    candidate.surface_id = state->next_surface_id;
    candidate.is_sync = frame->is_sync;

    result = state->present(state->sink_context, &candidate);
    if (result != PLAYBACK_VIDEO_OUTPUT_OK)
    {
        return PLAYBACK_VIDEO_OUTPUT_SINK_FAILED;
    }

    state->active_buffer = staging_buffer;
    state->active_surface = candidate;
    state->has_active_surface = true;
    state->next_surface_id++;
    if (state->next_surface_id == 0U)
    {
        state->next_surface_id = 1U;
    }
    return PLAYBACK_VIDEO_OUTPUT_OK;
}

playback_video_output_result_t playback_video_output_get_current_surface(
    const playback_video_output_t *output,
    playback_rgb565_surface_t *surface
)
{
    const playback_video_output_state_t *state = playback_video_output_get_state(output);

    if ((state == NULL) || (surface == NULL))
    {
        return PLAYBACK_VIDEO_OUTPUT_INVALID_ARGUMENT;
    }
    if (!state->has_active_surface)
    {
        return PLAYBACK_VIDEO_OUTPUT_NO_SURFACE;
    }

    *surface = state->active_surface;
    return PLAYBACK_VIDEO_OUTPUT_OK;
}

void playback_video_output_close(playback_video_output_t *output)
{
    playback_video_output_state_t *state;
    uint8_t index;

    if (output == NULL)
    {
        return;
    }

    state = playback_video_output_get_state(output);
    output->state = NULL;
    if (state == NULL)
    {
        return;
    }

    for (index = 0U; index < PLAYBACK_VIDEO_OUTPUT_BUFFER_COUNT; ++index)
    {
        if (state->buffers[index] != NULL)
        {
            tal_psram_free(state->buffers[index]);
        }
    }
    tal_free(state);
}

playback_error_t playback_video_output_result_to_error(playback_video_output_result_t result)
{
    switch (result)
    {
        case PLAYBACK_VIDEO_OUTPUT_OK:
        case PLAYBACK_VIDEO_OUTPUT_NO_SURFACE:
        case PLAYBACK_VIDEO_OUTPUT_STALE_FRAME:
            return PLAYBACK_ERROR_NONE;
        case PLAYBACK_VIDEO_OUTPUT_UNSUPPORTED_CONFIG:
        case PLAYBACK_VIDEO_OUTPUT_NO_MEMORY:
        case PLAYBACK_VIDEO_OUTPUT_INVALID_FRAME:
        case PLAYBACK_VIDEO_OUTPUT_SINK_FAILED:
            return PLAYBACK_ERROR_H264_DECODE_FAILED;
        case PLAYBACK_VIDEO_OUTPUT_INVALID_ARGUMENT:
        default:
            return PLAYBACK_ERROR_INTERNAL;
    }
}

const char *playback_video_output_result_name(playback_video_output_result_t result)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "UNSUPPORTED_CONFIG",
        "NO_MEMORY",
        "INVALID_FRAME",
        "NO_SURFACE",
        "STALE_FRAME",
        "SINK_FAILED",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0]))) ? names[result]
                                                                      : "UNKNOWN_VIDEO_OUTPUT_RESULT";
}
