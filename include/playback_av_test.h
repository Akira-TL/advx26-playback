#ifndef PLAYBACK_AV_TEST_H
#define PLAYBACK_AV_TEST_H

#include "playback_media_scheduler.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
    PLAYBACK_AV_TEST_OK = 0,
    PLAYBACK_AV_TEST_INVALID_ARGUMENT,
    PLAYBACK_AV_TEST_NO_MEMORY,
    PLAYBACK_AV_TEST_INDEX_ERROR,
    PLAYBACK_AV_TEST_PACKAGE_ERROR,
    PLAYBACK_AV_TEST_SCHEDULER_ERROR,
    PLAYBACK_AV_TEST_TIMEOUT,
} playback_av_test_result_t;

/**
 * Run one blocking audio/video acceptance session through the production
 * media scheduler. The caller must execute this function on a worker thread.
 */
playback_av_test_result_t playback_av_test_run(
    const playback_session_t *session,
    const playback_media_scheduler_config_t *config
);

const char *playback_av_test_result_name(playback_av_test_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_AV_TEST_H */
