#ifndef PLAYBACK_ENGINE_H
#define PLAYBACK_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "playback_domain.h"
#include "playback_media_scheduler.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_ENGINE_COMMAND_QUEUE_DEPTH (4U)
#define PLAYBACK_ENGINE_RESULT_CACHE_SIZE (8U)
#define PLAYBACK_ENGINE_PROGRESS_INTERVAL_MS (500U)

typedef enum
{
    PLAYBACK_ENGINE_OK = 0,
    PLAYBACK_ENGINE_INVALID_ARGUMENT,
    PLAYBACK_ENGINE_ALREADY_INITIALIZED,
    PLAYBACK_ENGINE_NOT_INITIALIZED,
    PLAYBACK_ENGINE_QUEUE_FULL,
    PLAYBACK_ENGINE_NO_MEMORY,
    PLAYBACK_ENGINE_PLATFORM_ERROR,
} playback_engine_result_t;

/** Runs on the engine worker. The report is valid only during the callback. */
typedef void (*playback_engine_report_sink_t)(
    void *context,
    const playback_report_t *report
);

typedef struct
{
    char boot_id[PLAYBACK_BOOT_ID_MAX_LEN + 1U];
    playback_media_scheduler_config_t scheduler;
    playback_engine_report_sink_t report_sink;
    void *report_context;
} playback_engine_config_t;

typedef struct
{
    void *state;
} playback_engine_t;

playback_engine_result_t playback_engine_init(
    playback_engine_t *engine,
    const playback_engine_config_t *config
);

/**
 * Copy and enqueue one decoded Board Link command.
 * This function is safe to call from the Board Link worker and never performs
 * media or network work on that caller thread.
 */
playback_engine_result_t playback_engine_submit(
    playback_engine_t *engine,
    const playback_command_t *command
);

playback_engine_result_t playback_engine_get_snapshot(
    playback_engine_t *engine,
    playback_snapshot_t *snapshot
);

/** Update the speaker selected by the local Bluetooth page for future sessions. */
playback_engine_result_t playback_engine_set_speaker_address(
    playback_engine_t *engine,
    const uint8_t address[PLAYBACK_SPEAKER_LINK_ADDRESS_BYTES]
);

void playback_engine_close(playback_engine_t *engine);
const char *playback_engine_result_name(playback_engine_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_ENGINE_H */
