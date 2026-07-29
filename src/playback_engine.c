/**
 * @file playback_engine.c
 * @brief Serialized Playback command policy, idempotency and report publishing.
 */

#include "playback_engine.h"

#include <string.h>

#include "playback_http_download.h"
#include "playback_http_range.h"
#include "playback_media_package.h"
#include "tal_api.h"

#define PLAYBACK_ENGINE_WORKER_POLL_MS (20U)
#define PLAYBACK_ENGINE_WORKER_STACK_SIZE (12U * 1024U)
#define PLAYBACK_ENGINE_INDEX_MAX_BYTES \
    (PLAYBACK_AUDIO_INDEX_HEADER_SIZE + \
     (PLAYBACK_AUDIO_INDEX_MAX_RECORDS * PLAYBACK_AUDIO_INDEX_RECORD_SIZE))

typedef enum
{
    PLAYBACK_ENGINE_EVENT_COMMAND = 0,
    PLAYBACK_ENGINE_EVENT_STOP,
} playback_engine_event_kind_t;

typedef struct
{
    playback_engine_event_kind_t kind;
    playback_command_t *command;
} playback_engine_event_t;

typedef struct
{
    bool valid;
    uint32_t sequence_id;
    uint64_t fingerprint;
    playback_report_t final_report;
} playback_engine_cache_entry_t;

typedef struct
{
    playback_engine_config_t config;
    playback_media_scheduler_t scheduler;
    bool scheduler_active;
    /* Whole-file PSRAM staging for the video asset (memory-source demux). */
    playback_http_download_file_t video_file;
    playback_snapshot_t snapshot;
    /* Worker-owned PSRAM scratch avoids multi-kilobyte command stack frames. */
    playback_snapshot_t scratch_snapshot;
    playback_media_package_t scratch_package;
    playback_report_t scratch_report;
    MUTEX_HANDLE snapshot_mutex;
    QUEUE_HANDLE command_queue;
    SEM_HANDLE worker_stopped;
    THREAD_HANDLE worker_thread;
    playback_engine_cache_entry_t cache[PLAYBACK_ENGINE_RESULT_CACHE_SIZE];
    uint8_t next_cache_entry;
    uint32_t highest_sequence_id;
    char controller_boot_id[PLAYBACK_BOOT_ID_MAX_LEN + 1U];
    uint32_t last_progress_ms;
    playback_state_t published_state;
    playback_intent_t published_intent;
    playback_error_t published_error;
    bool has_published_state;
    volatile bool closing;
} playback_engine_state_t;

static size_t playback_engine_bounded_length(const char *text, size_t capacity)
{
    size_t length = 0U;

    if (text == NULL)
    {
        return 0U;
    }
    while ((length < capacity) && (text[length] != '\0'))
    {
        ++length;
    }
    return length;
}

static void playback_engine_copy_string(char *destination, size_t capacity, const char *source)
{
    size_t length;

    if ((destination == NULL) || (capacity == 0U))
    {
        return;
    }
    length = playback_engine_bounded_length(source, capacity - 1U);
    if ((source != NULL) && (length > 0U))
    {
        memcpy(destination, source, length);
    }
    destination[length] = '\0';
}

static uint64_t playback_engine_hash_bytes(uint64_t hash, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    size_t index;

    for (index = 0U; index < length; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t playback_engine_hash_u32(uint64_t hash, uint32_t value)
{
    uint8_t bytes[4U];

    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
    return playback_engine_hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t playback_engine_hash_string(uint64_t hash, const char *text, size_t capacity)
{
    const size_t length = playback_engine_bounded_length(text, capacity);
    hash = playback_engine_hash_u32(hash, (uint32_t)length);
    return playback_engine_hash_bytes(hash, text, length);
}

static uint64_t playback_engine_hash_asset(uint64_t hash, const playback_asset_t *asset)
{
    hash = playback_engine_hash_string(hash, asset->url, sizeof(asset->url));
    hash = playback_engine_hash_u32(hash, asset->byte_length);
    hash = playback_engine_hash_string(hash, asset->sha256, sizeof(asset->sha256));
    return playback_engine_hash_string(hash, asset->etag, sizeof(asset->etag));
}

static uint64_t playback_engine_hash_session(uint64_t hash, const playback_session_t *session)
{
    hash = playback_engine_hash_string(hash, session->session_id, sizeof(session->session_id));
    hash = playback_engine_hash_string(hash, session->content_id, sizeof(session->content_id));
    hash = playback_engine_hash_u32(hash, session->revision);
    hash = playback_engine_hash_u32(hash, session->duration_ms);
    hash = playback_engine_hash_string(hash, session->profile, sizeof(session->profile));
    hash = playback_engine_hash_asset(hash, &session->video.asset);
    hash = playback_engine_hash_u32(hash, session->video.width);
    hash = playback_engine_hash_u32(hash, session->video.height);
    hash = playback_engine_hash_u32(hash, session->video.fps_num);
    hash = playback_engine_hash_u32(hash, session->video.fps_den);
    hash = playback_engine_hash_u32(hash, session->video.max_keyframe_interval_ms);
    hash = playback_engine_hash_u32(hash, session->video.h264_profile_idc);
    hash = playback_engine_hash_u32(hash, session->video.h264_level_idc);
    hash = playback_engine_hash_u32(hash, session->video.yuv420p ? 1U : 0U);
    hash = playback_engine_hash_u32(hash, session->video.has_b_frames ? 1U : 0U);
    hash = playback_engine_hash_asset(hash, &session->audio.asset);
    hash = playback_engine_hash_asset(hash, &session->audio.index_asset);
    hash = playback_engine_hash_u32(hash, session->audio.sample_rate);
    hash = playback_engine_hash_u32(hash, session->audio.bitrate_kbps);
    hash = playback_engine_hash_u32(hash, session->audio.channels);
    hash = playback_engine_hash_u32(hash, session->audio.index_version);
    hash = playback_engine_hash_u32(hash, session->autoplay ? 1U : 0U);
    return playback_engine_hash_u32(hash, (uint32_t)session->end_behavior);
}

static uint64_t playback_engine_command_fingerprint(const playback_command_t *command)
{
    uint64_t hash = 1469598103934665603ULL;

    hash = playback_engine_hash_u32(hash, command->schema_version);
    hash = playback_engine_hash_u32(hash, (uint32_t)command->kind);
    hash = playback_engine_hash_string(hash, command->session_id, sizeof(command->session_id));
    switch (command->kind)
    {
        case PLAYBACK_COMMAND_LOAD_SESSION:
            hash = playback_engine_hash_session(hash, &command->payload.session);
            break;
        case PLAYBACK_COMMAND_SEEK_MS:
            hash = playback_engine_hash_u32(hash, command->payload.seek_position_ms);
            break;
        default:
            break;
    }
    return hash;
}

static void playback_engine_snapshot_copy(
    playback_engine_state_t *state,
    playback_snapshot_t *snapshot
)
{
    tal_mutex_lock(state->snapshot_mutex);
    *snapshot = state->snapshot;
    tal_mutex_unlock(state->snapshot_mutex);
}

static void playback_engine_snapshot_store(
    playback_engine_state_t *state,
    const playback_snapshot_t *snapshot
)
{
    tal_mutex_lock(state->snapshot_mutex);
    state->snapshot = *snapshot;
    tal_mutex_unlock(state->snapshot_mutex);
}

static void playback_engine_emit(playback_engine_state_t *state, const playback_report_t *report)
{
    if (state->config.report_sink != NULL)
    {
        state->config.report_sink(state->config.report_context, report);
    }
}

static void playback_engine_fill_report_identity(
    playback_report_t *report,
    const playback_command_t *command
)
{
    memset(report, 0, sizeof(*report));
    report->sequence_id = command->sequence_id;
    playback_engine_copy_string(
        report->session_id,
        sizeof(report->session_id),
        command->session_id
    );
}

static playback_engine_cache_entry_t *playback_engine_find_cache(
    playback_engine_state_t *state,
    uint32_t sequence_id
)
{
    uint8_t index;

    for (index = 0U; index < PLAYBACK_ENGINE_RESULT_CACHE_SIZE; ++index)
    {
        if (state->cache[index].valid &&
            (state->cache[index].sequence_id == sequence_id))
        {
            return &state->cache[index];
        }
    }
    return NULL;
}

static void playback_engine_cache_report(
    playback_engine_state_t *state,
    uint32_t sequence_id,
    uint64_t fingerprint,
    const playback_report_t *report
)
{
    playback_engine_cache_entry_t *entry = &state->cache[state->next_cache_entry];

    entry->valid = true;
    entry->sequence_id = sequence_id;
    entry->fingerprint = fingerprint;
    entry->final_report = *report;
    state->next_cache_entry = (uint8_t)(
        (state->next_cache_entry + 1U) % PLAYBACK_ENGINE_RESULT_CACHE_SIZE
    );
}

static void playback_engine_make_ack(
    playback_report_t *report,
    const playback_command_t *command
)
{
    playback_engine_fill_report_identity(report, command);
    report->kind = PLAYBACK_REPORT_ACK;
    report->acknowledged_command = command->kind;
}

static void playback_engine_make_nack(
    playback_report_t *report,
    const playback_command_t *command,
    playback_nack_t nack,
    const char *diagnostic
)
{
    playback_engine_fill_report_identity(report, command);
    report->kind = PLAYBACK_REPORT_NACK;
    report->acknowledged_command = command->kind;
    report->nack = nack;
    playback_engine_copy_string(
        report->diagnostic,
        sizeof(report->diagnostic),
        diagnostic
    );
}

static void playback_engine_publish_snapshot(
    playback_engine_state_t *state,
    bool force,
    bool allow_progress
)
{
    playback_snapshot_t *snapshot = &state->scratch_snapshot;
    playback_report_t *report = &state->scratch_report;
    const uint32_t now = (uint32_t)tal_system_get_millisecond();
    bool state_changed;

    playback_engine_snapshot_copy(state, snapshot);
    state_changed = !state->has_published_state ||
                    (snapshot->state != state->published_state) ||
                    (snapshot->intent != state->published_intent) ||
                    (snapshot->error != state->published_error);

    if (force || state_changed)
    {
        memset(report, 0, sizeof(*report));
        report->kind = (snapshot->state == PLAYBACK_STATE_COMPLETED)
                           ? PLAYBACK_REPORT_COMPLETED
                           : ((snapshot->state == PLAYBACK_STATE_ERROR)
                                  ? PLAYBACK_REPORT_ERROR
                                  : PLAYBACK_REPORT_STATE);
        playback_engine_copy_string(
            report->session_id,
            sizeof(report->session_id),
            snapshot->has_session ? snapshot->session.session_id : ""
        );
        report->state = snapshot->state;
        report->intent = snapshot->intent;
        report->error = snapshot->error;
        report->retryable = snapshot->retryable;
        report->position_ms = snapshot->position_ms;
        report->duration_ms = snapshot->has_session ? snapshot->session.duration_ms : 0U;
        playback_engine_copy_string(
            report->diagnostic,
            sizeof(report->diagnostic),
            snapshot->diagnostic
        );
        playback_engine_emit(state, report);
        state->published_state = snapshot->state;
        state->published_intent = snapshot->intent;
        state->published_error = snapshot->error;
        state->has_published_state = true;
        if (snapshot->state == PLAYBACK_STATE_PLAYING)
        {
            state->last_progress_ms = now;
        }
    }

    if (allow_progress && (snapshot->state == PLAYBACK_STATE_PLAYING) &&
        ((uint32_t)(now - state->last_progress_ms) >= PLAYBACK_ENGINE_PROGRESS_INTERVAL_MS))
    {
        memset(report, 0, sizeof(*report));
        report->kind = PLAYBACK_REPORT_PROGRESS;
        playback_engine_copy_string(
            report->session_id,
            sizeof(report->session_id),
            snapshot->session.session_id
        );
        report->state = snapshot->state;
        report->intent = snapshot->intent;
        report->position_ms = snapshot->position_ms;
        report->duration_ms = snapshot->session.duration_ms;
        playback_engine_emit(state, report);
        state->last_progress_ms = now;
    }
}

#if !PLAYBACK_SCHEDULER_VIDEO_ONLY
static playback_error_t playback_engine_http_error(playback_http_result_t result)
{
    switch (result)
    {
        case PLAYBACK_HTTP_RANGE_INVALID:
            return PLAYBACK_ERROR_NETWORK_RANGE_INVALID;
        case PLAYBACK_HTTP_RESOURCE_MISMATCH:
            return PLAYBACK_ERROR_CONTENT_INVALID;
        case PLAYBACK_HTTP_INVALID_ARGUMENT:
        case PLAYBACK_HTTP_INVALID_URL:
            return PLAYBACK_ERROR_INDEX_INVALID;
        case PLAYBACK_HTTP_OK:
            return PLAYBACK_ERROR_NONE;
        default:
            return PLAYBACK_ERROR_NETWORK_TIMEOUT;
    }
}
#endif /* !PLAYBACK_SCHEDULER_VIDEO_ONLY */

static void playback_engine_set_error(
    playback_engine_state_t *state,
    playback_error_t error,
    bool retryable,
    const char *diagnostic
)
{
    playback_snapshot_t *snapshot = &state->scratch_snapshot;

    playback_engine_snapshot_copy(state, snapshot);
    snapshot->state = PLAYBACK_STATE_ERROR;
    snapshot->error = error;
    snapshot->retryable = retryable;
    playback_engine_copy_string(
        snapshot->diagnostic,
        sizeof(snapshot->diagnostic),
        diagnostic
    );
    playback_engine_snapshot_store(state, snapshot);
}

#if !PLAYBACK_SCHEDULER_VIDEO_ONLY
static playback_engine_result_t playback_engine_load_index(
    playback_engine_state_t *state,
    const playback_session_t *session,
    playback_audio_index_t *index,
    playback_audio_index_record_t **records_out,
    playback_error_t *error_out
)
{
    playback_http_range_reader_t reader;
    playback_http_metadata_t metadata;
    playback_http_result_t http_result;
    playback_package_result_t package_result;
    uint8_t *payload = NULL;
    playback_audio_index_record_t *records = NULL;
    uint32_t byte_length = session->audio.index_asset.byte_length;

    *records_out = NULL;
    *error_out = PLAYBACK_ERROR_NONE;
    if ((byte_length < PLAYBACK_AUDIO_INDEX_HEADER_SIZE) ||
        (byte_length > PLAYBACK_ENGINE_INDEX_MAX_BYTES))
    {
        *error_out = PLAYBACK_ERROR_INDEX_INVALID;
        return PLAYBACK_ENGINE_INVALID_ARGUMENT;
    }

    payload = tal_psram_malloc(byte_length);
    records = tal_psram_malloc(
        PLAYBACK_AUDIO_INDEX_MAX_RECORDS * sizeof(*records)
    );
    if ((payload == NULL) || (records == NULL))
    {
        if (payload != NULL)
        {
            tal_psram_free(payload);
        }
        if (records != NULL)
        {
            tal_psram_free(records);
        }
        *error_out = PLAYBACK_ERROR_INTERNAL;
        return PLAYBACK_ENGINE_NO_MEMORY;
    }

    http_result = playback_http_range_reader_init(
        &reader,
        &session->audio.index_asset,
        &state->config.scheduler.http
    );
    if (http_result == PLAYBACK_HTTP_OK)
    {
        http_result = playback_http_range_probe(&reader, &metadata);
    }
    if (http_result == PLAYBACK_HTTP_OK)
    {
        http_result = playback_http_range_read_at(
            &reader,
            0U,
            byte_length,
            payload,
            byte_length
        );
    }
    if (http_result != PLAYBACK_HTTP_OK)
    {
        *error_out = playback_engine_http_error(http_result);
        tal_psram_free(records);
        tal_psram_free(payload);
        return PLAYBACK_ENGINE_PLATFORM_ERROR;
    }

    memset(index, 0, sizeof(*index));
    index->records = records;
    index->capacity = PLAYBACK_AUDIO_INDEX_MAX_RECORDS;
    package_result = playback_audio_index_parse(
        payload,
        byte_length,
        session->audio.asset.byte_length,
        session->audio.sample_rate,
        session->duration_ms,
        index
    );
    tal_psram_free(payload);
    if (package_result != PLAYBACK_PACKAGE_OK)
    {
        tal_psram_free(records);
        memset(index, 0, sizeof(*index));
        *error_out = PLAYBACK_ERROR_INDEX_INVALID;
        return PLAYBACK_ENGINE_INVALID_ARGUMENT;
    }

    *records_out = records;
    return PLAYBACK_ENGINE_OK;
}
#endif /* !PLAYBACK_SCHEDULER_VIDEO_ONLY */

static bool playback_engine_session_matches(
    playback_engine_state_t *state,
    const playback_command_t *command
)
{
    playback_snapshot_t *snapshot = &state->scratch_snapshot;

    playback_engine_snapshot_copy(state, snapshot);
    return snapshot->has_session &&
           (strcmp(snapshot->session.session_id, command->session_id) == 0);
}

static void playback_engine_sync_scheduler(playback_engine_state_t *state)
{
    playback_media_scheduler_snapshot_t scheduler_snapshot;
    playback_snapshot_t *snapshot = &state->scratch_snapshot;
    playback_media_scheduler_result_t result;

    if (!state->scheduler_active)
    {
        return;
    }
    result = playback_media_scheduler_get_snapshot(
        &state->scheduler,
        &scheduler_snapshot
    );
    if (result != PLAYBACK_SCHEDULER_OK)
    {
        playback_engine_set_error(
            state,
            playback_media_scheduler_result_to_error(result),
            false,
            playback_media_scheduler_result_name(result)
        );
        return;
    }

    playback_engine_snapshot_copy(state, snapshot);
    snapshot->state = scheduler_snapshot.state;
    snapshot->intent = scheduler_snapshot.intent;
    snapshot->error = scheduler_snapshot.error;
    snapshot->position_ms = scheduler_snapshot.position_ms;
    if (scheduler_snapshot.state != PLAYBACK_STATE_ERROR)
    {
        snapshot->retryable = false;
        snapshot->diagnostic[0] = '\0';
    }
    playback_engine_snapshot_store(state, snapshot);
}

static bool playback_engine_handle_load(
    playback_engine_state_t *state,
    const playback_command_t *command,
    playback_nack_t *nack
)
{
    playback_media_package_t *package = &state->scratch_package;
    playback_audio_index_t index;
    playback_audio_index_record_t *records = NULL;
    playback_media_scheduler_result_t scheduler_result;
    playback_media_scheduler_config_t scheduler_config;
    playback_engine_result_t index_result;
    playback_error_t load_error;

    if (playback_media_package_validate(&command->payload.session, package) !=
        PLAYBACK_PACKAGE_OK)
    {
        *nack = PLAYBACK_NACK_UNSUPPORTED_PROFILE;
        return false;
    }

    if (state->scheduler_active)
    {
        playback_media_scheduler_close(&state->scheduler);
        state->scheduler_active = false;
    }
    playback_http_download_release(&state->video_file);

#if PLAYBACK_SCHEDULER_VIDEO_ONLY
    /* Video-only: audio index is unused by the scheduler; skip its HTTP fetch. */
    memset(&index, 0, sizeof(index));
    (void)index_result;
    (void)load_error;
#else
    index_result = playback_engine_load_index(
        state,
        &command->payload.session,
        &index,
        &records,
        &load_error
    );
    if (index_result != PLAYBACK_ENGINE_OK)
    {
        playback_engine_set_error(
            state,
            load_error,
            load_error == PLAYBACK_ERROR_NETWORK_TIMEOUT,
            "audio index load failed"
        );
        return true;
    }
#endif

    tal_mutex_lock(state->snapshot_mutex);
    scheduler_config = state->config.scheduler;
    tal_mutex_unlock(state->snapshot_mutex);

    if (scheduler_config.video_memory == NULL)
    {
        playback_http_download_result_t download_result;
        SYS_TIME_T download_start = tal_system_get_millisecond();

        download_result = playback_http_download_file(
            package->session.video.asset.url,
            scheduler_config.http.authorization,
            package->session.video.asset.byte_length,
            "VIDEO",
            &state->video_file
        );
        if (download_result != PLAYBACK_HTTP_DOWNLOAD_OK)
        {
            if (records != NULL)
            {
                tal_psram_free(records);
            }
            playback_engine_set_error(
                state,
                (download_result == PLAYBACK_HTTP_DOWNLOAD_NO_MEMORY)
                    ? PLAYBACK_ERROR_INTERNAL
                    : PLAYBACK_ERROR_NETWORK_TIMEOUT,
                download_result == PLAYBACK_HTTP_DOWNLOAD_NETWORK_ERROR,
                playback_http_download_result_name(download_result)
            );
            return true;
        }
        PR_NOTICE(
            "engine: video staged %u bytes in %u ms",
            (unsigned int)state->video_file.length,
            (unsigned int)(tal_system_get_millisecond() - download_start)
        );
        scheduler_config.video_memory = state->video_file.data;
        scheduler_config.video_memory_length = state->video_file.length;
    }

    scheduler_result = playback_media_scheduler_prepare(
        &state->scheduler,
        package,
        &index,
        &scheduler_config
    );
    if (records != NULL)
    {
        tal_psram_free(records);
    }
    if (scheduler_result != PLAYBACK_SCHEDULER_OK)
    {
        playback_http_download_release(&state->video_file);
        playback_engine_set_error(
            state,
            playback_media_scheduler_result_to_error(scheduler_result),
            false,
            playback_media_scheduler_result_name(scheduler_result)
        );
        return true;
    }

    state->scheduler_active = true;
    (void)playback_media_scheduler_pump(&state->scheduler);
    playback_engine_sync_scheduler(state);
    return true;
}

static bool playback_engine_handle_control(
    playback_engine_state_t *state,
    const playback_command_t *command,
    playback_nack_t *nack
)
{
    playback_media_scheduler_result_t result = PLAYBACK_SCHEDULER_OK;
    playback_snapshot_t *snapshot = &state->scratch_snapshot;

    if (command->kind == PLAYBACK_COMMAND_STOP)
    {
        if (!playback_engine_session_matches(state, command))
        {
            *nack = PLAYBACK_NACK_STALE_SESSION;
            return false;
        }
        if (state->scheduler_active)
        {
            playback_media_scheduler_close(&state->scheduler);
            state->scheduler_active = false;
        }
        playback_http_download_release(&state->video_file);
        playback_engine_snapshot_copy(state, snapshot);
        memset(&snapshot->session, 0, sizeof(snapshot->session));
        snapshot->has_session = false;
        snapshot->state = PLAYBACK_STATE_IDLE;
        snapshot->intent = PLAYBACK_INTENT_PAUSED;
        snapshot->position_ms = 0U;
        snapshot->error = PLAYBACK_ERROR_NONE;
        snapshot->retryable = false;
        snapshot->diagnostic[0] = '\0';
        playback_engine_snapshot_store(state, snapshot);
        return true;
    }

    if (!state->scheduler_active || !playback_engine_session_matches(state, command))
    {
        *nack = PLAYBACK_NACK_STALE_SESSION;
        return false;
    }

    switch (command->kind)
    {
        case PLAYBACK_COMMAND_PLAY:
            result = playback_media_scheduler_play(&state->scheduler);
            break;
        case PLAYBACK_COMMAND_PAUSE:
            result = playback_media_scheduler_pause(&state->scheduler);
            break;
        case PLAYBACK_COMMAND_SEEK_MS:
            playback_engine_snapshot_copy(state, snapshot);
            if (command->payload.seek_position_ms > snapshot->session.duration_ms)
            {
                *nack = PLAYBACK_NACK_INVALID_STATE;
                return false;
            }
            result = playback_media_scheduler_seek(
                &state->scheduler,
                command->payload.seek_position_ms
            );
            break;
        case PLAYBACK_COMMAND_STOP:
            *nack = PLAYBACK_NACK_INVALID_STATE;
            return false;
        default:
            *nack = PLAYBACK_NACK_UNKNOWN_TYPE;
            return false;
    }

    if (result != PLAYBACK_SCHEDULER_OK)
    {
        if ((result == PLAYBACK_SCHEDULER_NOT_PREPARED) ||
            (result == PLAYBACK_SCHEDULER_ERROR_STATE) ||
            (result == PLAYBACK_SCHEDULER_INVALID_ARGUMENT))
        {
            *nack = PLAYBACK_NACK_INVALID_STATE;
            return false;
        }
        playback_engine_set_error(
            state,
            playback_media_scheduler_result_to_error(result),
            false,
            playback_media_scheduler_result_name(result)
        );
        return true;
    }

    playback_engine_sync_scheduler(state);
    return true;
}

static bool playback_engine_execute(
    playback_engine_state_t *state,
    const playback_command_t *command,
    playback_nack_t *nack
)
{
    *nack = PLAYBACK_NACK_NONE;
    switch (command->kind)
    {
        case PLAYBACK_COMMAND_LOAD_SESSION:
            *nack = PLAYBACK_NACK_INVALID_STATE;
            return false;
        case PLAYBACK_COMMAND_PLAY:
        case PLAYBACK_COMMAND_PAUSE:
        case PLAYBACK_COMMAND_SEEK_MS:
        case PLAYBACK_COMMAND_STOP:
            return playback_engine_handle_control(state, command, nack);
        case PLAYBACK_COMMAND_GET_STATUS:
            return true;
        case PLAYBACK_COMMAND_HELLO:
            return true;
        default:
            *nack = PLAYBACK_NACK_UNKNOWN_TYPE;
            return false;
    }
}

static void playback_engine_process_command(
    playback_engine_state_t *state,
    const playback_command_t *command
)
{
    playback_engine_cache_entry_t *cached;
    playback_report_t *final_report = &state->scratch_report;
    playback_nack_t nack;
    uint64_t fingerprint;
    bool accepted;

    if ((command->kind == PLAYBACK_COMMAND_HELLO) &&
        (command->payload.hello.boot_id[0] != '\0') &&
        (strcmp(state->controller_boot_id, command->payload.hello.boot_id) != 0))
    {
        playback_engine_copy_string(
            state->controller_boot_id,
            sizeof(state->controller_boot_id),
            command->payload.hello.boot_id
        );
        memset(state->cache, 0, sizeof(state->cache));
        state->next_cache_entry = 0U;
        state->highest_sequence_id = 0U;
    }

    fingerprint = playback_engine_command_fingerprint(command);
    cached = playback_engine_find_cache(state, command->sequence_id);
    if (cached != NULL)
    {
        if (cached->fingerprint == fingerprint)
        {
            playback_engine_emit(state, &cached->final_report);
        }
        else
        {
            playback_engine_make_nack(
                final_report,
                command,
                PLAYBACK_NACK_DUPLICATE_SEQUENCE_CONFLICT,
                "sequence id reused with different command"
            );
            playback_engine_emit(state, final_report);
        }
        return;
    }

    if ((command->sequence_id == 0U) ||
        ((state->highest_sequence_id != 0U) &&
         (command->sequence_id <= state->highest_sequence_id)))
    {
        playback_engine_make_nack(
            final_report,
            command,
            PLAYBACK_NACK_DUPLICATE_SEQUENCE_CONFLICT,
            "sequence id is not monotonic"
        );
        playback_engine_cache_report(
            state,
            command->sequence_id,
            fingerprint,
            final_report
        );
        playback_engine_emit(state, final_report);
        return;
    }
    state->highest_sequence_id = command->sequence_id;

    if (command->kind == PLAYBACK_COMMAND_LOAD_SESSION)
    {
        playback_media_package_t *validated_package = &state->scratch_package;
        playback_snapshot_t *snapshot = &state->scratch_snapshot;
        playback_event_t event;

        if (playback_media_package_validate(
                &command->payload.session,
                validated_package
            ) != PLAYBACK_PACKAGE_OK)
        {
            playback_engine_make_nack(
                final_report,
                command,
                PLAYBACK_NACK_UNSUPPORTED_PROFILE,
                "invalid media package"
            );
            playback_engine_cache_report(
                state,
                command->sequence_id,
                fingerprint,
                final_report
            );
            playback_engine_emit(state, final_report);
            return;
        }

        playback_engine_snapshot_copy(state, snapshot);
        memset(&event, 0, sizeof(event));
        event.kind = PLAYBACK_EVENT_LOAD_ACCEPTED;
        event.session = &command->payload.session;
        if (playback_snapshot_apply(snapshot, &event) != PLAYBACK_RESULT_OK)
        {
            playback_engine_make_nack(
                final_report,
                command,
                PLAYBACK_NACK_INVALID_STATE,
                "load transition rejected"
            );
            playback_engine_cache_report(
                state,
                command->sequence_id,
                fingerprint,
                final_report
            );
            playback_engine_emit(state, final_report);
            return;
        }

        playback_engine_snapshot_store(state, snapshot);
        playback_engine_make_ack(final_report, command);
        playback_engine_cache_report(
            state,
            command->sequence_id,
            fingerprint,
            final_report
        );
        playback_engine_emit(state, final_report);
        playback_engine_publish_snapshot(state, true, false);
        (void)playback_engine_handle_load(state, command, &nack);
        playback_engine_publish_snapshot(state, true, false);
        return;
    }

    accepted = playback_engine_execute(state, command, &nack);
    if (accepted)
    {
        playback_engine_make_ack(final_report, command);
    }
    else
    {
        playback_engine_make_nack(
            final_report,
            command,
            nack,
            playback_nack_name(nack)
        );
    }
    playback_engine_cache_report(
        state,
        command->sequence_id,
        fingerprint,
        final_report
    );
    playback_engine_emit(state, final_report);
    if (accepted)
    {
        playback_engine_publish_snapshot(state, true, false);
    }
}

static void playback_engine_tick(playback_engine_state_t *state)
{
    playback_media_scheduler_result_t result;

    if (!state->scheduler_active)
    {
        return;
    }
    result = playback_media_scheduler_pump(&state->scheduler);
    if ((result != PLAYBACK_SCHEDULER_OK) &&
        (result != PLAYBACK_SCHEDULER_ERROR_STATE))
    {
        playback_engine_set_error(
            state,
            playback_media_scheduler_result_to_error(result),
            false,
            playback_media_scheduler_result_name(result)
        );
    }
    playback_engine_sync_scheduler(state);
    playback_engine_publish_snapshot(state, false, true);
}

static void playback_engine_worker(void *argument)
{
    playback_engine_state_t *state = argument;
    playback_engine_event_t event;

    for (;;)
    {
        if (tal_queue_fetch(
                state->command_queue,
                &event,
                PLAYBACK_ENGINE_WORKER_POLL_MS
            ) != OPRT_OK)
        {
            playback_engine_tick(state);
            continue;
        }

        if (event.kind == PLAYBACK_ENGINE_EVENT_STOP)
        {
            if (event.command != NULL)
            {
                tal_psram_free(event.command);
            }
            if (state->scheduler_active)
            {
                playback_media_scheduler_close(&state->scheduler);
                state->scheduler_active = false;
            }
            playback_http_download_release(&state->video_file);
            tal_semaphore_post(state->worker_stopped);
            return;
        }
        if ((event.kind == PLAYBACK_ENGINE_EVENT_COMMAND) &&
            (event.command != NULL))
        {
            playback_engine_process_command(state, event.command);
            tal_psram_free(event.command);
        }
        playback_engine_tick(state);
    }
}

static void playback_engine_release_state(playback_engine_state_t *state)
{
    playback_engine_event_t pending;

    if (state == NULL)
    {
        return;
    }
    if (state->command_queue != NULL)
    {
        while (tal_queue_fetch(state->command_queue, &pending, 0U) == OPRT_OK)
        {
            if (pending.command != NULL)
            {
                tal_psram_free(pending.command);
            }
        }
        tal_queue_free(state->command_queue);
    }
    if (state->worker_stopped != NULL)
    {
        tal_semaphore_release(state->worker_stopped);
    }
    if (state->snapshot_mutex != NULL)
    {
        tal_mutex_release(state->snapshot_mutex);
    }
    tal_psram_free(state);
}

playback_engine_result_t playback_engine_init(
    playback_engine_t *engine,
    const playback_engine_config_t *config
)
{
    playback_engine_state_t *state;
    THREAD_CFG_T worker_config;

    if ((engine == NULL) || (config == NULL) ||
        (config->boot_id[0] == '\0') ||
        (config->report_sink == NULL) ||
        (config->scheduler.present_video == NULL))
    {
        return PLAYBACK_ENGINE_INVALID_ARGUMENT;
    }
    if (engine->state != NULL)
    {
        return PLAYBACK_ENGINE_ALREADY_INITIALIZED;
    }

    state = tal_psram_calloc(1U, sizeof(*state));
    if (state == NULL)
    {
        return PLAYBACK_ENGINE_NO_MEMORY;
    }
    state->config = *config;
    playback_snapshot_init(&state->snapshot, config->boot_id);
    state->published_state = PLAYBACK_STATE_IDLE;
    state->published_intent = PLAYBACK_INTENT_PAUSED;
    state->published_error = PLAYBACK_ERROR_NONE;

    if ((tal_mutex_create_init(&state->snapshot_mutex) != OPRT_OK) ||
        (tal_semaphore_create_init(&state->worker_stopped, 0U, 1U) != OPRT_OK) ||
        (tal_queue_create_init(
             &state->command_queue,
             sizeof(playback_engine_event_t),
             PLAYBACK_ENGINE_COMMAND_QUEUE_DEPTH
         ) != OPRT_OK))
    {
        playback_engine_release_state(state);
        return PLAYBACK_ENGINE_NO_MEMORY;
    }

    memset(&worker_config, 0, sizeof(worker_config));
    worker_config.stackDepth = PLAYBACK_ENGINE_WORKER_STACK_SIZE;
    worker_config.priority = THREAD_PRIO_2;
    worker_config.thrdname = "playback_engine";
    worker_config.psram_mode = 1U;
    if (tal_thread_create_and_start(
            &state->worker_thread,
            NULL,
            NULL,
            playback_engine_worker,
            state,
            &worker_config
        ) != OPRT_OK)
    {
        playback_engine_release_state(state);
        return PLAYBACK_ENGINE_PLATFORM_ERROR;
    }

    engine->state = state;
    return PLAYBACK_ENGINE_OK;
}

playback_engine_result_t playback_engine_submit(
    playback_engine_t *engine,
    const playback_command_t *command
)
{
    playback_engine_state_t *state;
    playback_engine_event_t event;
    playback_command_t *copy;

    if ((engine == NULL) || (command == NULL))
    {
        return PLAYBACK_ENGINE_INVALID_ARGUMENT;
    }
    if (engine->state == NULL)
    {
        return PLAYBACK_ENGINE_NOT_INITIALIZED;
    }
    state = engine->state;
    if (state->closing)
    {
        return PLAYBACK_ENGINE_NOT_INITIALIZED;
    }
    if (command->kind == PLAYBACK_COMMAND_HELLO)
    {
        return PLAYBACK_ENGINE_OK;
    }

    copy = tal_psram_malloc(sizeof(*copy));
    if (copy == NULL)
    {
        return PLAYBACK_ENGINE_NO_MEMORY;
    }
    *copy = *command;
    memset(&event, 0, sizeof(event));
    event.kind = PLAYBACK_ENGINE_EVENT_COMMAND;
    event.command = copy;
    if (tal_queue_post(state->command_queue, &event, 0U) != OPRT_OK)
    {
        tal_psram_free(copy);
        return PLAYBACK_ENGINE_QUEUE_FULL;
    }
    return PLAYBACK_ENGINE_OK;
}

playback_engine_result_t playback_engine_get_snapshot(
    playback_engine_t *engine,
    playback_snapshot_t *snapshot
)
{
    if ((engine == NULL) || (snapshot == NULL))
    {
        return PLAYBACK_ENGINE_INVALID_ARGUMENT;
    }
    if (engine->state == NULL)
    {
        return PLAYBACK_ENGINE_NOT_INITIALIZED;
    }
    playback_engine_snapshot_copy(engine->state, snapshot);
    return PLAYBACK_ENGINE_OK;
}

void playback_engine_close(playback_engine_t *engine)
{
    playback_engine_state_t *state;
    playback_engine_event_t event;

    if ((engine == NULL) || (engine->state == NULL))
    {
        return;
    }
    state = engine->state;
    state->closing = true;
    memset(&event, 0, sizeof(event));
    event.kind = PLAYBACK_ENGINE_EVENT_STOP;
    (void)tal_queue_post(state->command_queue, &event, QUEUE_WAIT_FOREVER);
    (void)tal_semaphore_wait(state->worker_stopped, 5000U);
    if (state->worker_thread != NULL)
    {
        (void)tal_thread_delete(state->worker_thread);
    }
    engine->state = NULL;
    playback_engine_release_state(state);
}

const char *playback_engine_result_name(playback_engine_result_t result)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "ALREADY_INITIALIZED",
        "NOT_INITIALIZED",
        "QUEUE_FULL",
        "NO_MEMORY",
        "PLATFORM_ERROR",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0])))
               ? names[result]
               : "UNKNOWN_ENGINE_RESULT";
}
