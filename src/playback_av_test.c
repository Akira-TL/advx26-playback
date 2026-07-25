/**
 * @file playback_av_test.c
 * @brief Production-scheduler audio/video hardware acceptance path.
 */

#include "playback_av_test.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "playback_http_download.h"
#include "playback_media_package.h"
#include "tal_api.h"

#define PLAYBACK_AV_TEST_INDEX_MAX_BYTES \
    (PLAYBACK_AUDIO_INDEX_HEADER_SIZE + \
     (PLAYBACK_AUDIO_INDEX_MAX_RECORDS * PLAYBACK_AUDIO_INDEX_RECORD_SIZE))
#define PLAYBACK_AV_TEST_PUMP_DELAY_MS (10U)
#define PLAYBACK_AV_TEST_LOG_INTERVAL_MS (1000U)
#define PLAYBACK_AV_TEST_WALL_TIMEOUT_MS (180000U)

typedef struct
{
    playback_audio_index_t index;
    playback_audio_index_record_t *records;
} playback_av_test_index_t;

static playback_av_test_result_t playback_av_test_load_index(
    const playback_session_t *session,
    playback_av_test_index_t *loaded
)
{
    playback_http_download_file_t payload;
    playback_http_download_result_t download_result;
    playback_package_result_t package_result;
    const uint32_t byte_length = session != NULL
                                     ? session->audio.index_asset.byte_length
                                     : 0U;

    if ((session == NULL) || (loaded == NULL))
    {
        return PLAYBACK_AV_TEST_INVALID_ARGUMENT;
    }
    memset(loaded, 0, sizeof(*loaded));
    memset(&payload, 0, sizeof(payload));
    if ((byte_length < PLAYBACK_AUDIO_INDEX_HEADER_SIZE) ||
        (byte_length > PLAYBACK_AV_TEST_INDEX_MAX_BYTES))
    {
        return PLAYBACK_AV_TEST_INDEX_ERROR;
    }

    download_result = playback_http_download_file(
        session->audio.index_asset.url,
        byte_length,
        "INDEX",
        &payload
    );
    if (download_result != PLAYBACK_HTTP_DOWNLOAD_OK)
    {
        PR_ERR(
            "AV: audio index download failed: %s",
            playback_http_download_result_name(download_result)
        );
        return (download_result == PLAYBACK_HTTP_DOWNLOAD_NO_MEMORY)
                   ? PLAYBACK_AV_TEST_NO_MEMORY
                   : PLAYBACK_AV_TEST_INDEX_ERROR;
    }

    loaded->records = tal_psram_malloc(
        PLAYBACK_AUDIO_INDEX_MAX_RECORDS * sizeof(*loaded->records)
    );
    if (loaded->records == NULL)
    {
        playback_http_download_release(&payload);
        return PLAYBACK_AV_TEST_NO_MEMORY;
    }
    loaded->index.records = loaded->records;
    loaded->index.capacity = PLAYBACK_AUDIO_INDEX_MAX_RECORDS;
    package_result = playback_audio_index_parse(
        payload.data,
        payload.length,
        session->audio.asset.byte_length,
        session->audio.sample_rate,
        session->duration_ms,
        &loaded->index
    );
    playback_http_download_release(&payload);
    if (package_result != PLAYBACK_PACKAGE_OK)
    {
        PR_ERR(
            "AV: audio index parse failed: %s",
            playback_package_result_name(package_result)
        );
        tal_psram_free(loaded->records);
        memset(loaded, 0, sizeof(*loaded));
        return PLAYBACK_AV_TEST_INDEX_ERROR;
    }

    PR_NOTICE(
        "AV: audio index ready records=%u",
        (unsigned int)loaded->index.count
    );
    return PLAYBACK_AV_TEST_OK;
}

static void playback_av_test_release_index(playback_av_test_index_t *loaded)
{
    if ((loaded != NULL) && (loaded->records != NULL))
    {
        tal_psram_free(loaded->records);
        memset(loaded, 0, sizeof(*loaded));
    }
}

playback_av_test_result_t playback_av_test_run(
    const playback_session_t *session,
    const playback_media_scheduler_config_t *config
)
{
    playback_media_package_t package;
    playback_media_scheduler_t scheduler;
    playback_media_scheduler_snapshot_t snapshot;
    playback_media_scheduler_config_t scheduler_config;
    playback_av_test_index_t loaded_index;
    playback_http_download_file_t audio_file;
    playback_http_download_file_t video_file;
    playback_http_download_result_t download_result;
    playback_media_scheduler_result_t scheduler_result;
    playback_av_test_result_t result;
    playback_state_t last_state = PLAYBACK_STATE_IDLE;
    uint32_t last_log_ms = 0U;
    uint64_t started_ms;
    uint64_t perf_window_started_ms = 0ULL;
    uint64_t perf_last_loop_started_ms = 0ULL;
    uint64_t perf_loop_gap_total_ms = 0ULL;
    uint64_t perf_pump_total_ms = 0ULL;
    uint64_t perf_sleep_total_ms = 0ULL;
    uint32_t perf_loop_count = 0U;
    uint64_t perf_loop_gap_max_ms = 0ULL;
    uint64_t perf_pump_max_ms = 0ULL;
    uint64_t perf_sleep_max_ms = 0ULL;
    bool scheduler_open = false;
    bool has_state = false;

    if ((session == NULL) || (config == NULL) || (config->present_video == NULL))
    {
        return PLAYBACK_AV_TEST_INVALID_ARGUMENT;
    }
    memset(&package, 0, sizeof(package));
    memset(&scheduler, 0, sizeof(scheduler));
    memset(&scheduler_config, 0, sizeof(scheduler_config));
    memset(&loaded_index, 0, sizeof(loaded_index));
    memset(&audio_file, 0, sizeof(audio_file));
    memset(&video_file, 0, sizeof(video_file));
    scheduler_config = *config;

    if (playback_media_package_validate(session, &package) != PLAYBACK_PACKAGE_OK)
    {
        PR_ERR("AV: debug media package validation failed");
        return PLAYBACK_AV_TEST_PACKAGE_ERROR;
    }

    result = playback_av_test_load_index(session, &loaded_index);
    if (result != PLAYBACK_AV_TEST_OK)
    {
        goto cleanup;
    }

    download_result = playback_http_download_file(
        session->audio.asset.url,
        session->audio.asset.byte_length,
        "AUDIO",
        &audio_file
    );
    if (download_result != PLAYBACK_HTTP_DOWNLOAD_OK)
    {
        PR_ERR(
            "AV: audio download failed: %s",
            playback_http_download_result_name(download_result)
        );
        result = (download_result == PLAYBACK_HTTP_DOWNLOAD_NO_MEMORY)
                     ? PLAYBACK_AV_TEST_NO_MEMORY
                     : PLAYBACK_AV_TEST_SCHEDULER_ERROR;
        goto cleanup;
    }

    download_result = playback_http_download_file(
        session->video.asset.url,
        session->video.asset.byte_length,
        "VIDEO",
        &video_file
    );
    if (download_result != PLAYBACK_HTTP_DOWNLOAD_OK)
    {
        PR_ERR(
            "AV: video download failed: %s",
            playback_http_download_result_name(download_result)
        );
        result = (download_result == PLAYBACK_HTTP_DOWNLOAD_NO_MEMORY)
                     ? PLAYBACK_AV_TEST_NO_MEMORY
                     : PLAYBACK_AV_TEST_SCHEDULER_ERROR;
        goto cleanup;
    }

    scheduler_config.audio_memory = audio_file.data;
    scheduler_config.audio_memory_length = audio_file.length;
    scheduler_config.video_memory = video_file.data;
    scheduler_config.video_memory_length = video_file.length;
    PR_NOTICE(
        "AV: staged media audio=%u video=%u; playback network disabled",
        (unsigned int)audio_file.length,
        (unsigned int)video_file.length
    );

    scheduler_result = playback_media_scheduler_prepare(
        &scheduler,
        &package,
        &loaded_index.index,
        &scheduler_config
    );
    playback_av_test_release_index(&loaded_index);
    if (scheduler_result != PLAYBACK_SCHEDULER_OK)
    {
        PR_ERR(
            "AV: scheduler prepare failed: %s",
            playback_media_scheduler_result_name(scheduler_result)
        );
        result = PLAYBACK_AV_TEST_SCHEDULER_ERROR;
        goto cleanup;
    }
    scheduler_open = true;

    if (!session->autoplay)
    {
        scheduler_result = playback_media_scheduler_play(&scheduler);
        if (scheduler_result != PLAYBACK_SCHEDULER_OK)
        {
            PR_ERR(
                "AV: scheduler play failed: %s",
                playback_media_scheduler_result_name(scheduler_result)
            );
            result = PLAYBACK_AV_TEST_SCHEDULER_ERROR;
            goto cleanup;
        }
    }

    PR_NOTICE(
        "AV: scheduler started duration=%u wired_latency=%u",
        (unsigned int)session->duration_ms,
        (unsigned int)(
            scheduler_config.wired_speaker.output_latency_ms != 0U
                ? scheduler_config.wired_speaker.output_latency_ms
                : PLAYBACK_WIRED_SPEAKER_DEFAULT_LATENCY_MS
        )
    );
    started_ms = tal_system_get_millisecond();
    result = PLAYBACK_AV_TEST_SCHEDULER_ERROR;

    for (;;)
    {
        const uint64_t loop_started_ms = tal_system_get_millisecond();
        const uint64_t now_ms = loop_started_ms;
        uint64_t pump_call_started_ms;
        uint64_t pump_call_ms;
        uint64_t loop_gap_ms = 0ULL;

        if (perf_window_started_ms == 0ULL)
        {
            perf_window_started_ms = loop_started_ms;
        }
        if (perf_last_loop_started_ms != 0ULL &&
            loop_started_ms >= perf_last_loop_started_ms)
        {
            loop_gap_ms = loop_started_ms - perf_last_loop_started_ms;
            perf_loop_gap_total_ms += loop_gap_ms;
            if (loop_gap_ms > perf_loop_gap_max_ms)
            {
                perf_loop_gap_max_ms = loop_gap_ms;
            }
        }
        perf_last_loop_started_ms = loop_started_ms;

        pump_call_started_ms = tal_system_get_millisecond();
        scheduler_result = playback_media_scheduler_pump(&scheduler);
        pump_call_ms = tal_system_get_millisecond() - pump_call_started_ms;
        perf_pump_total_ms += pump_call_ms;
        if (pump_call_ms > perf_pump_max_ms)
        {
            perf_pump_max_ms = pump_call_ms;
        }
        perf_loop_count++;
        if (scheduler_result != PLAYBACK_SCHEDULER_OK)
        {
            PR_ERR(
                "AV: scheduler pump failed: %s",
                playback_media_scheduler_result_name(scheduler_result)
            );
            break;
        }
        scheduler_result = playback_media_scheduler_get_snapshot(&scheduler, &snapshot);
        if (scheduler_result != PLAYBACK_SCHEDULER_OK)
        {
            PR_ERR(
                "AV: scheduler snapshot failed: %s",
                playback_media_scheduler_result_name(scheduler_result)
            );
            break;
        }

        if (!has_state || (snapshot.state != last_state) ||
            ((uint32_t)(now_ms - last_log_ms) >= PLAYBACK_AV_TEST_LOG_INTERVAL_MS))
        {
            PR_NOTICE(
                "AV: state=%s position=%u/%u audio_buffer=%u speaker_queue=%u video_queue=%u dropped=%u wired=%u submitted=%llu",
                playback_state_name(snapshot.state),
                (unsigned int)snapshot.position_ms,
                (unsigned int)snapshot.duration_ms,
                (unsigned int)snapshot.buffered_audio_ms,
                (unsigned int)snapshot.queued_speaker_ms,
                (unsigned int)snapshot.queued_video_frames,
                (unsigned int)snapshot.dropped_video_frames,
                snapshot.speaker_started ? 1U : 0U,
                (unsigned long long)snapshot.submitted_audio_frames
            );
            last_state = snapshot.state;
            last_log_ms = (uint32_t)now_ms;
            has_state = true;
        }

        if (snapshot.state == PLAYBACK_STATE_COMPLETED)
        {
            result = PLAYBACK_AV_TEST_OK;
            break;
        }
        if (snapshot.state == PLAYBACK_STATE_ERROR)
        {
            PR_ERR("AV: scheduler entered error=%u", (unsigned int)snapshot.error);
            break;
        }
        if ((now_ms - started_ms) >= PLAYBACK_AV_TEST_WALL_TIMEOUT_MS)
        {
            PR_ERR("AV: wall-clock timeout");
            result = PLAYBACK_AV_TEST_TIMEOUT;
            break;
        }

        {
            const uint64_t sleep_started_ms = tal_system_get_millisecond();
            uint64_t sleep_ms;
            uint64_t perf_now_ms;
            uint64_t perf_wall_ms;

            tal_system_sleep(PLAYBACK_AV_TEST_PUMP_DELAY_MS);
            perf_now_ms = tal_system_get_millisecond();
            sleep_ms = perf_now_ms - sleep_started_ms;
            perf_sleep_total_ms += sleep_ms;
            if (sleep_ms > perf_sleep_max_ms)
            {
                perf_sleep_max_ms = sleep_ms;
            }

            perf_wall_ms = perf_now_ms - perf_window_started_ms;
            if (perf_wall_ms >= PLAYBACK_AV_TEST_LOG_INTERVAL_MS)
            {
                PR_NOTICE(
                    "[DEBUG-avperf] av_loop wall=%llu loops=%u gap_total=%llu gap_max=%llu pump_total=%llu pump_max=%llu sleep_total=%llu sleep_max=%llu requested_sleep=%u",
                    (unsigned long long)perf_wall_ms,
                    (unsigned int)perf_loop_count,
                    (unsigned long long)perf_loop_gap_total_ms,
                    (unsigned long long)perf_loop_gap_max_ms,
                    (unsigned long long)perf_pump_total_ms,
                    (unsigned long long)perf_pump_max_ms,
                    (unsigned long long)perf_sleep_total_ms,
                    (unsigned long long)perf_sleep_max_ms,
                    (unsigned int)PLAYBACK_AV_TEST_PUMP_DELAY_MS
                );
                perf_window_started_ms = perf_now_ms;
                perf_last_loop_started_ms = 0ULL;
                perf_loop_gap_total_ms = 0ULL;
                perf_pump_total_ms = 0ULL;
                perf_sleep_total_ms = 0ULL;
                perf_loop_count = 0U;
                perf_loop_gap_max_ms = 0ULL;
                perf_pump_max_ms = 0ULL;
                perf_sleep_max_ms = 0ULL;
            }
        }
    }

cleanup:
    if (scheduler_open)
    {
        playback_media_scheduler_close(&scheduler);
    }
    playback_av_test_release_index(&loaded_index);
    playback_http_download_release(&video_file);
    playback_http_download_release(&audio_file);
    return result;
}

const char *playback_av_test_result_name(playback_av_test_result_t result)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "NO_MEMORY",
        "INDEX_ERROR",
        "PACKAGE_ERROR",
        "SCHEDULER_ERROR",
        "TIMEOUT",
    };

    return (result < (sizeof(names) / sizeof(names[0]))) ? names[result] : "UNKNOWN";
}
