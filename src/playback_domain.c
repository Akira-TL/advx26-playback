/**
 * @file playback_domain.c
 * @brief Pure Playback domain values and state-transition rules.
 */

#include "playback_domain.h"

#include <string.h>

static void playback_copy_string(char *destination, size_t capacity, const char *source)
{
    if ((destination == NULL) || (capacity == 0U))
    {
        return;
    }

    destination[0] = '\0';
    if (source == NULL)
    {
        return;
    }

    strncpy(destination, source, capacity - 1U);
    destination[capacity - 1U] = '\0';
}

static void playback_snapshot_clear_error(playback_snapshot_t *snapshot)
{
    snapshot->error = PLAYBACK_ERROR_NONE;
    snapshot->retryable = false;
    snapshot->diagnostic[0] = '\0';
}

static void playback_snapshot_clear_session(playback_snapshot_t *snapshot)
{
    memset(&snapshot->session, 0, sizeof(snapshot->session));
    snapshot->has_session = false;
    snapshot->position_ms = 0U;
    snapshot->intent = PLAYBACK_INTENT_PAUSED;
    playback_snapshot_clear_error(snapshot);
}

static bool playback_state_accepts_intent(playback_state_t state)
{
    switch (state)
    {
    case PLAYBACK_STATE_LOADING:
    case PLAYBACK_STATE_WAITING_SPEAKER:
    case PLAYBACK_STATE_BUFFERING:
    case PLAYBACK_STATE_PLAYING:
    case PLAYBACK_STATE_PAUSED:
    case PLAYBACK_STATE_SEEKING:
        return true;

    case PLAYBACK_STATE_IDLE:
    case PLAYBACK_STATE_COMPLETED:
    case PLAYBACK_STATE_ERROR:
    default:
        return false;
    }
}

static bool playback_state_can_seek(playback_state_t state)
{
    switch (state)
    {
    case PLAYBACK_STATE_WAITING_SPEAKER:
    case PLAYBACK_STATE_BUFFERING:
    case PLAYBACK_STATE_PLAYING:
    case PLAYBACK_STATE_PAUSED:
    case PLAYBACK_STATE_COMPLETED:
        return true;

    case PLAYBACK_STATE_IDLE:
    case PLAYBACK_STATE_LOADING:
    case PLAYBACK_STATE_SEEKING:
    case PLAYBACK_STATE_ERROR:
    default:
        return false;
    }
}

static playback_result_t playback_snapshot_require_session(const playback_snapshot_t *snapshot)
{
    return playback_snapshot_has_active_session(snapshot) ? PLAYBACK_RESULT_OK : PLAYBACK_RESULT_INVALID_TRANSITION;
}

static playback_result_t playback_snapshot_set_position(playback_snapshot_t *snapshot, uint32_t position_ms)
{
    if (!playback_snapshot_has_active_session(snapshot))
    {
        return PLAYBACK_RESULT_INVALID_TRANSITION;
    }
    if (position_ms > snapshot->session.duration_ms)
    {
        return PLAYBACK_RESULT_OUT_OF_RANGE;
    }

    snapshot->position_ms = position_ms;
    return PLAYBACK_RESULT_OK;
}

void playback_snapshot_init(playback_snapshot_t *snapshot, const char *boot_id)
{
    if (snapshot == NULL)
    {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = PLAYBACK_STATE_IDLE;
    snapshot->intent = PLAYBACK_INTENT_PAUSED;
    snapshot->error = PLAYBACK_ERROR_NONE;
    playback_copy_string(snapshot->boot_id, sizeof(snapshot->boot_id), boot_id);
}

bool playback_session_profile_supported(const playback_session_t *session)
{
    if (session == NULL)
    {
        return false;
    }

    return strcmp(session->profile, PLAYBACK_PROFILE_H264_MP3) == 0;
}

bool playback_snapshot_has_active_session(const playback_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return false;
    }

    return snapshot->has_session && (snapshot->state != PLAYBACK_STATE_IDLE);
}

playback_result_t playback_snapshot_apply(playback_snapshot_t *snapshot, const playback_event_t *event)
{
    playback_result_t result;

    if ((snapshot == NULL) || (event == NULL))
    {
        return PLAYBACK_RESULT_INVALID_ARGUMENT;
    }

    switch (event->kind)
    {
    case PLAYBACK_EVENT_LOAD_ACCEPTED:
        if (event->session == NULL)
        {
            return PLAYBACK_RESULT_INVALID_ARGUMENT;
        }
        if (!playback_session_profile_supported(event->session))
        {
            return PLAYBACK_RESULT_UNSUPPORTED_PROFILE;
        }
        if ((event->session->duration_ms == 0U) || (event->session->duration_ms > PLAYBACK_MAX_DURATION_MS))
        {
            return PLAYBACK_RESULT_OUT_OF_RANGE;
        }

        snapshot->session = *event->session;
        snapshot->has_session = true;
        snapshot->state = PLAYBACK_STATE_LOADING;
        snapshot->intent = event->session->autoplay ? PLAYBACK_INTENT_PLAYING : PLAYBACK_INTENT_PAUSED;
        snapshot->position_ms = 0U;
        playback_snapshot_clear_error(snapshot);
        return PLAYBACK_RESULT_OK;

    case PLAYBACK_EVENT_LOADING:
        result = playback_snapshot_require_session(snapshot);
        if (result != PLAYBACK_RESULT_OK)
        {
            return result;
        }
        if (snapshot->state == PLAYBACK_STATE_ERROR)
        {
            return PLAYBACK_RESULT_INVALID_TRANSITION;
        }
        snapshot->state = PLAYBACK_STATE_LOADING;
        return PLAYBACK_RESULT_OK;

    case PLAYBACK_EVENT_BUFFERING:
        result = playback_snapshot_require_session(snapshot);
        if (result != PLAYBACK_RESULT_OK)
        {
            return result;
        }
        if ((snapshot->state == PLAYBACK_STATE_ERROR) || (snapshot->state == PLAYBACK_STATE_COMPLETED))
        {
            return PLAYBACK_RESULT_INVALID_TRANSITION;
        }
        snapshot->state = PLAYBACK_STATE_BUFFERING;
        return PLAYBACK_RESULT_OK;

    case PLAYBACK_EVENT_SPEAKER_UNAVAILABLE:
        result = playback_snapshot_require_session(snapshot);
        if (result != PLAYBACK_RESULT_OK)
        {
            return result;
        }
        if ((snapshot->state == PLAYBACK_STATE_ERROR) || (snapshot->state == PLAYBACK_STATE_COMPLETED))
        {
            return PLAYBACK_RESULT_INVALID_TRANSITION;
        }
        snapshot->state = PLAYBACK_STATE_WAITING_SPEAKER;
        return PLAYBACK_RESULT_OK;

    case PLAYBACK_EVENT_SPEAKER_AVAILABLE:
        if ((snapshot->state != PLAYBACK_STATE_WAITING_SPEAKER) || !snapshot->has_session)
        {
            return PLAYBACK_RESULT_INVALID_TRANSITION;
        }
        snapshot->state = PLAYBACK_STATE_BUFFERING;
        return PLAYBACK_RESULT_OK;

    case PLAYBACK_EVENT_READY:
        result = playback_snapshot_require_session(snapshot);
        if (result != PLAYBACK_RESULT_OK)
        {
            return result;
        }
        if ((snapshot->state != PLAYBACK_STATE_LOADING) && (snapshot->state != PLAYBACK_STATE_BUFFERING) &&
            (snapshot->state != PLAYBACK_STATE_SEEKING) && (snapshot->state != PLAYBACK_STATE_WAITING_SPEAKER))
        {
            return PLAYBACK_RESULT_INVALID_TRANSITION;
        }
        snapshot->state = (snapshot->intent == PLAYBACK_INTENT_PLAYING) ? PLAYBACK_STATE_PLAYING : PLAYBACK_STATE_PAUSED;
        return PLAYBACK_RESULT_OK;

    case PLAYBACK_EVENT_PLAY_REQUESTED:
        result = playback_snapshot_require_session(snapshot);
        if (result != PLAYBACK_RESULT_OK)
        {
            return result;
        }
        if (!playback_state_accepts_intent(snapshot->state))
        {
            return PLAYBACK_RESULT_INVALID_TRANSITION;
        }
        snapshot->intent = PLAYBACK_INTENT_PLAYING;
        if (snapshot->state == PLAYBACK_STATE_PAUSED)
        {
            snapshot->state = PLAYBACK_STATE_PLAYING;
        }
        return PLAYBACK_RESULT_OK;

    case PLAYBACK_EVENT_PAUSE_REQUESTED:
        result = playback_snapshot_require_session(snapshot);
        if (result != PLAYBACK_RESULT_OK)
        {
            return result;
        }
        if (!playback_state_accepts_intent(snapshot->state))
        {
            return PLAYBACK_RESULT_INVALID_TRANSITION;
        }
        snapshot->intent = PLAYBACK_INTENT_PAUSED;
        if (snapshot->state == PLAYBACK_STATE_PLAYING)
        {
            snapshot->state = PLAYBACK_STATE_PAUSED;
        }
        return PLAYBACK_RESULT_OK;

    case PLAYBACK_EVENT_SEEK_STARTED:
        result = playback_snapshot_require_session(snapshot);
        if (result != PLAYBACK_RESULT_OK)
        {
            return result;
        }
        if (!playback_state_can_seek(snapshot->state))
        {
            return PLAYBACK_RESULT_INVALID_TRANSITION;
        }
        result = playback_snapshot_set_position(snapshot, event->position_ms);
        if (result != PLAYBACK_RESULT_OK)
        {
            return result;
        }
        snapshot->state = PLAYBACK_STATE_SEEKING;
        return PLAYBACK_RESULT_OK;

    case PLAYBACK_EVENT_SEEK_READY:
        if ((snapshot->state != PLAYBACK_STATE_SEEKING) || !snapshot->has_session)
        {
            return PLAYBACK_RESULT_INVALID_TRANSITION;
        }
        result = playback_snapshot_set_position(snapshot, event->position_ms);
        if (result != PLAYBACK_RESULT_OK)
        {
            return result;
        }
        snapshot->state = (snapshot->intent == PLAYBACK_INTENT_PLAYING) ? PLAYBACK_STATE_PLAYING : PLAYBACK_STATE_PAUSED;
        return PLAYBACK_RESULT_OK;

    case PLAYBACK_EVENT_POSITION_CHANGED:
        return playback_snapshot_set_position(snapshot, event->position_ms);

    case PLAYBACK_EVENT_COMPLETED:
        result = playback_snapshot_require_session(snapshot);
        if (result != PLAYBACK_RESULT_OK)
        {
            return result;
        }
        if ((snapshot->state != PLAYBACK_STATE_PLAYING) && (snapshot->state != PLAYBACK_STATE_BUFFERING))
        {
            return PLAYBACK_RESULT_INVALID_TRANSITION;
        }
        snapshot->position_ms = snapshot->session.duration_ms;
        snapshot->intent = PLAYBACK_INTENT_PAUSED;
        snapshot->state = PLAYBACK_STATE_COMPLETED;
        return PLAYBACK_RESULT_OK;

    case PLAYBACK_EVENT_FAILED:
        if (event->error == PLAYBACK_ERROR_NONE)
        {
            return PLAYBACK_RESULT_INVALID_ARGUMENT;
        }
        snapshot->state = PLAYBACK_STATE_ERROR;
        snapshot->error = event->error;
        snapshot->retryable = event->retryable;
        playback_copy_string(snapshot->diagnostic, sizeof(snapshot->diagnostic), event->diagnostic);
        return PLAYBACK_RESULT_OK;

    case PLAYBACK_EVENT_STOPPED:
        playback_snapshot_clear_session(snapshot);
        snapshot->state = PLAYBACK_STATE_IDLE;
        return PLAYBACK_RESULT_OK;

    default:
        return PLAYBACK_RESULT_INVALID_ARGUMENT;
    }
}

const char *playback_state_name(playback_state_t state)
{
    static const char *const names[] = {
        "IDLE", "LOADING", "WAITING_SPEAKER", "BUFFERING", "PLAYING",
        "PAUSED", "SEEKING", "COMPLETED", "ERROR",
    };

    return ((unsigned int)state < (sizeof(names) / sizeof(names[0]))) ? names[state] : "UNKNOWN_STATE";
}

const char *playback_error_name(playback_error_t error)
{
    static const char *const names[] = {
        "NONE",
        "NETWORK_TIMEOUT",
        "NETWORK_RANGE_INVALID",
        "CONTENT_INVALID",
        "INDEX_INVALID",
        "MP4_DEMUX_FAILED",
        "H264_DECODE_FAILED",
        "MP3_DECODE_FAILED",
        "PROTOCOL_INCOMPATIBLE",
        "INTERNAL_ERROR",
    };

    return ((unsigned int)error < (sizeof(names) / sizeof(names[0]))) ? names[error] : "UNKNOWN_ERROR";
}

const char *playback_command_name(playback_command_kind_t command)
{
    static const char *const names[] = {
        "HELLO", "GET_STATUS", "LOAD_SESSION", "PLAY", "PAUSE", "SEEK_MS", "STOP",
    };

    return ((unsigned int)command < (sizeof(names) / sizeof(names[0]))) ? names[command] : "UNKNOWN_COMMAND";
}

const char *playback_report_name(playback_report_kind_t report)
{
    static const char *const names[] = {
        "HELLO_ACK", "ACK", "NACK", "STATE", "PROGRESS", "COMPLETED", "ERROR",
    };

    return ((unsigned int)report < (sizeof(names) / sizeof(names[0]))) ? names[report] : "UNKNOWN_REPORT";
}

const char *playback_nack_name(playback_nack_t nack)
{
    static const char *const names[] = {
        "NONE",
        "MALFORMED_MESSAGE",
        "UNKNOWN_TYPE",
        "INVALID_STATE",
        "STALE_SESSION",
        "DUPLICATE_SEQUENCE_CONFLICT",
        "UNSUPPORTED_PROFILE",
        "PROTOCOL_INCOMPATIBLE",
    };

    return ((unsigned int)nack < (sizeof(names) / sizeof(names[0]))) ? names[nack] : "UNKNOWN_NACK";
}
