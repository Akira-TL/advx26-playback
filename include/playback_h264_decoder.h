#ifndef PLAYBACK_H264_DECODER_H
#define PLAYBACK_H264_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "playback_domain.h"
#include "playback_mp4_demux.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_H264_FRAME_PLANE_COUNT (3U)

typedef enum
{
    PLAYBACK_H264_OK = 0,
    PLAYBACK_H264_INVALID_ARGUMENT,
    PLAYBACK_H264_UNSUPPORTED_CONFIG,
    PLAYBACK_H264_NO_MEMORY,
    PLAYBACK_H264_NEED_SYNC,
    PLAYBACK_H264_FRAME_PENDING,
    PLAYBACK_H264_NO_FRAME,
    PLAYBACK_H264_STALE_FRAME,
    PLAYBACK_H264_INVALID_ACCESS_UNIT,
    PLAYBACK_H264_RESET_REQUIRED,
    PLAYBACK_H264_DECODE_FAILED,
    PLAYBACK_H264_OUTPUT_INVALID,
} playback_h264_result_t;

typedef enum
{
    PLAYBACK_VIDEO_PIXEL_FORMAT_YUV420P = 0,
} playback_video_pixel_format_t;

typedef struct
{
    playback_video_pixel_format_t pixel_format;
    const uint8_t *planes[PLAYBACK_H264_FRAME_PLANE_COUNT];
    uint32_t strides[PLAYBACK_H264_FRAME_PLANE_COUNT];
    size_t plane_lengths[PLAYBACK_H264_FRAME_PLANE_COUNT];
    uint16_t width;
    uint16_t height;
    uint32_t timescale;
    uint64_t pts;
    uint32_t duration;
    uint32_t generation;
    uint32_t lease_id;
    bool is_sync;
} playback_h264_frame_t;

typedef struct
{
    void *state;
} playback_h264_decoder_t;

playback_h264_result_t playback_h264_decoder_open(
    playback_h264_decoder_t *decoder,
    const playback_video_descriptor_t *descriptor,
    const playback_mp4_codec_config_t *codec_config,
    const uint8_t *parameter_sets_annex_b,
    size_t parameter_sets_length
);

playback_h264_result_t playback_h264_decoder_submit_access_unit(
    playback_h264_decoder_t *decoder,
    const uint8_t *access_unit_annex_b,
    size_t access_unit_length,
    uint64_t pts,
    uint32_t duration,
    bool is_sync
);

playback_h264_result_t playback_h264_decoder_poll_frame(
    const playback_h264_decoder_t *decoder,
    playback_h264_frame_t *frame
);

playback_h264_result_t playback_h264_decoder_release_frame(
    playback_h264_decoder_t *decoder,
    const playback_h264_frame_t *frame
);

playback_h264_result_t playback_h264_decoder_reset_at_sync(playback_h264_decoder_t *decoder);
void playback_h264_decoder_close(playback_h264_decoder_t *decoder);
playback_error_t playback_h264_result_to_error(playback_h264_result_t result);
const char *playback_h264_result_name(playback_h264_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_H264_DECODER_H */
