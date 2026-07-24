#ifndef PLAYBACK_VIDEO_OUTPUT_H
#define PLAYBACK_VIDEO_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "playback_domain.h"
#include "playback_h264_decoder.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_VIDEO_OUTPUT_BUFFER_COUNT (2U)

typedef enum
{
    PLAYBACK_VIDEO_OUTPUT_OK = 0,
    PLAYBACK_VIDEO_OUTPUT_INVALID_ARGUMENT,
    PLAYBACK_VIDEO_OUTPUT_UNSUPPORTED_CONFIG,
    PLAYBACK_VIDEO_OUTPUT_NO_MEMORY,
    PLAYBACK_VIDEO_OUTPUT_INVALID_FRAME,
    PLAYBACK_VIDEO_OUTPUT_NO_SURFACE,
    PLAYBACK_VIDEO_OUTPUT_STALE_FRAME,
    PLAYBACK_VIDEO_OUTPUT_SINK_FAILED,
} playback_video_output_result_t;

typedef enum
{
    PLAYBACK_VIDEO_ROTATION_0 = 0,
    PLAYBACK_VIDEO_ROTATION_90,
    PLAYBACK_VIDEO_ROTATION_180,
    PLAYBACK_VIDEO_ROTATION_270,
} playback_video_rotation_t;

typedef struct
{
    uint16_t source_width;
    uint16_t source_height;
    playback_video_rotation_t rotation;
    bool swap_rgb565_bytes;
} playback_video_output_config_t;

typedef struct
{
    const uint16_t *pixels;
    size_t pixel_count;
    uint16_t width;
    uint16_t height;
    uint32_t stride_bytes;
    uint32_t timescale;
    uint64_t pts;
    uint32_t duration;
    uint32_t generation;
    uint32_t surface_id;
    bool is_sync;
} playback_rgb565_surface_t;

typedef playback_video_output_result_t (*playback_video_sink_present_fn)(
    void *context,
    const playback_rgb565_surface_t *surface
);

typedef struct
{
    void *state;
} playback_video_output_t;

playback_video_output_result_t playback_video_output_open(
    playback_video_output_t *output,
    const playback_video_output_config_t *config,
    playback_video_sink_present_fn present,
    void *sink_context
);

playback_video_output_result_t playback_video_output_present(
    playback_video_output_t *output,
    const playback_h264_frame_t *frame
);

playback_video_output_result_t playback_video_output_get_current_surface(
    const playback_video_output_t *output,
    playback_rgb565_surface_t *surface
);

void playback_video_output_close(playback_video_output_t *output);
playback_error_t playback_video_output_result_to_error(playback_video_output_result_t result);
const char *playback_video_output_result_name(playback_video_output_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_VIDEO_OUTPUT_H */
