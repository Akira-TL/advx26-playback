#ifndef PLAYBACK_VIDEO_TEST_H
#define PLAYBACK_VIDEO_TEST_H

#include "playback_video_output.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
    PLAYBACK_VIDEO_TEST_OK = 0,
    PLAYBACK_VIDEO_TEST_INVALID_ARGUMENT,
    PLAYBACK_VIDEO_TEST_NETWORK_ERROR,
    PLAYBACK_VIDEO_TEST_NO_MEMORY,
    PLAYBACK_VIDEO_TEST_MP4_ERROR,
    PLAYBACK_VIDEO_TEST_UNSUPPORTED_VIDEO,
    PLAYBACK_VIDEO_TEST_DECODE_ERROR,
    PLAYBACK_VIDEO_TEST_OUTPUT_ERROR,
} playback_video_test_result_t;

/**
 * Download one H.264 MP4 into PSRAM, demux it from memory, and present frames.
 * This is a blocking hardware-acceptance path and must run on a worker thread.
 */
playback_video_test_result_t playback_video_test_run(
    const char *url,
    playback_video_sink_present_fn present,
    void *present_context
);

const char *playback_video_test_result_name(playback_video_test_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_VIDEO_TEST_H */
