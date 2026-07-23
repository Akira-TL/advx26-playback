#ifndef PLAYBACK_DOMAIN_H
#define PLAYBACK_DOMAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_PROTOCOL_SCHEMA_VERSION (1U)
#define PLAYBACK_PROFILE_H264_MP3 "t5ai-h264-mp3-v1"

#define PLAYBACK_SESSION_ID_MAX_LEN (64U)
#define PLAYBACK_CONTENT_ID_MAX_LEN (128U)
#define PLAYBACK_BOOT_ID_MAX_LEN (64U)
#define PLAYBACK_PROFILE_MAX_LEN (32U)
#define PLAYBACK_URL_MAX_LEN (1024U)
#define PLAYBACK_SHA256_HEX_LEN (64U)
#define PLAYBACK_ETAG_MAX_LEN (96U)
#define PLAYBACK_DIAGNOSTIC_MAX_LEN (160U)
#define PLAYBACK_CAPABILITY_COUNT_MAX (8U)
#define PLAYBACK_CAPABILITY_MAX_LEN (32U)

#define PLAYBACK_MAX_DURATION_MS (30000U)
#define PLAYBACK_VIDEO_WIDTH (480U)
#define PLAYBACK_VIDEO_HEIGHT (320U)

typedef enum
{
    PLAYBACK_RESULT_OK = 0,
    PLAYBACK_RESULT_INVALID_ARGUMENT,
    PLAYBACK_RESULT_INVALID_TRANSITION,
    PLAYBACK_RESULT_OUT_OF_RANGE,
    PLAYBACK_RESULT_UNSUPPORTED_PROFILE,
} playback_result_t;

typedef enum
{
    PLAYBACK_STATE_IDLE = 0,
    PLAYBACK_STATE_LOADING,
    PLAYBACK_STATE_WAITING_SPEAKER,
    PLAYBACK_STATE_BUFFERING,
    PLAYBACK_STATE_PLAYING,
    PLAYBACK_STATE_PAUSED,
    PLAYBACK_STATE_SEEKING,
    PLAYBACK_STATE_COMPLETED,
    PLAYBACK_STATE_ERROR,
} playback_state_t;

typedef enum
{
    PLAYBACK_INTENT_PAUSED = 0,
    PLAYBACK_INTENT_PLAYING,
} playback_intent_t;

typedef enum
{
    PLAYBACK_ERROR_NONE = 0,
    PLAYBACK_ERROR_NETWORK_TIMEOUT,
    PLAYBACK_ERROR_NETWORK_RANGE_INVALID,
    PLAYBACK_ERROR_CONTENT_INVALID,
    PLAYBACK_ERROR_INDEX_INVALID,
    PLAYBACK_ERROR_MP4_DEMUX_FAILED,
    PLAYBACK_ERROR_H264_DECODE_FAILED,
    PLAYBACK_ERROR_MP3_DECODE_FAILED,
    PLAYBACK_ERROR_PROTOCOL_INCOMPATIBLE,
    PLAYBACK_ERROR_INTERNAL,
} playback_error_t;

typedef enum
{
    PLAYBACK_COMMAND_HELLO = 0,
    PLAYBACK_COMMAND_GET_STATUS,
    PLAYBACK_COMMAND_LOAD_SESSION,
    PLAYBACK_COMMAND_PLAY,
    PLAYBACK_COMMAND_PAUSE,
    PLAYBACK_COMMAND_SEEK_MS,
    PLAYBACK_COMMAND_STOP,
} playback_command_kind_t;

typedef enum
{
    PLAYBACK_REPORT_HELLO_ACK = 0,
    PLAYBACK_REPORT_ACK,
    PLAYBACK_REPORT_NACK,
    PLAYBACK_REPORT_STATE,
    PLAYBACK_REPORT_PROGRESS,
    PLAYBACK_REPORT_COMPLETED,
    PLAYBACK_REPORT_ERROR,
} playback_report_kind_t;

typedef enum
{
    PLAYBACK_NACK_NONE = 0,
    PLAYBACK_NACK_MALFORMED_MESSAGE,
    PLAYBACK_NACK_UNKNOWN_TYPE,
    PLAYBACK_NACK_INVALID_STATE,
    PLAYBACK_NACK_STALE_SESSION,
    PLAYBACK_NACK_DUPLICATE_SEQUENCE_CONFLICT,
    PLAYBACK_NACK_UNSUPPORTED_PROFILE,
    PLAYBACK_NACK_PROTOCOL_INCOMPATIBLE,
} playback_nack_t;

typedef enum
{
    PLAYBACK_END_HOLD_LAST_FRAME = 0,
} playback_end_behavior_t;

typedef enum
{
    PLAYBACK_EVENT_LOAD_ACCEPTED = 0,
    PLAYBACK_EVENT_LOADING,
    PLAYBACK_EVENT_BUFFERING,
    PLAYBACK_EVENT_SPEAKER_UNAVAILABLE,
    PLAYBACK_EVENT_SPEAKER_AVAILABLE,
    PLAYBACK_EVENT_READY,
    PLAYBACK_EVENT_PLAY_REQUESTED,
    PLAYBACK_EVENT_PAUSE_REQUESTED,
    PLAYBACK_EVENT_SEEK_STARTED,
    PLAYBACK_EVENT_SEEK_READY,
    PLAYBACK_EVENT_POSITION_CHANGED,
    PLAYBACK_EVENT_COMPLETED,
    PLAYBACK_EVENT_FAILED,
    PLAYBACK_EVENT_STOPPED,
} playback_event_kind_t;

typedef struct
{
    char url[PLAYBACK_URL_MAX_LEN + 1U];
    uint32_t byte_length;
    char sha256[PLAYBACK_SHA256_HEX_LEN + 1U];
    char etag[PLAYBACK_ETAG_MAX_LEN + 1U];
} playback_asset_t;

typedef struct
{
    playback_asset_t asset;
    uint16_t width;
    uint16_t height;
    uint16_t fps_num;
    uint16_t fps_den;
    uint16_t max_keyframe_interval_ms;
    uint8_t h264_profile_idc;
    uint8_t h264_level_idc;
    bool yuv420p;
    bool has_b_frames;
} playback_video_descriptor_t;

typedef struct
{
    playback_asset_t asset;
    playback_asset_t index_asset;
    uint32_t sample_rate;
    uint16_t bitrate_kbps;
    uint8_t channels;
    uint8_t index_version;
} playback_audio_descriptor_t;

typedef struct
{
    char session_id[PLAYBACK_SESSION_ID_MAX_LEN + 1U];
    char content_id[PLAYBACK_CONTENT_ID_MAX_LEN + 1U];
    uint32_t revision;
    uint32_t duration_ms;
    char profile[PLAYBACK_PROFILE_MAX_LEN + 1U];
    playback_video_descriptor_t video;
    playback_audio_descriptor_t audio;
    bool autoplay;
    playback_end_behavior_t end_behavior;
} playback_session_t;

typedef struct
{
    uint16_t protocol_major;
    uint16_t protocol_minor;
    char boot_id[PLAYBACK_BOOT_ID_MAX_LEN + 1U];
    uint16_t max_message_bytes;
    uint8_t capability_count;
    char capabilities[PLAYBACK_CAPABILITY_COUNT_MAX][PLAYBACK_CAPABILITY_MAX_LEN + 1U];
} playback_hello_t;

typedef struct
{
    uint32_t schema_version;
    playback_command_kind_t kind;
    char session_id[PLAYBACK_SESSION_ID_MAX_LEN + 1U];
    uint32_t sequence_id;
    union
    {
        playback_hello_t hello;
        playback_session_t session;
        uint32_t seek_position_ms;
    } payload;
} playback_command_t;

typedef struct
{
    playback_report_kind_t kind;
    char session_id[PLAYBACK_SESSION_ID_MAX_LEN + 1U];
    uint32_t sequence_id;
    playback_command_kind_t acknowledged_command;
    playback_nack_t nack;
    playback_state_t state;
    playback_intent_t intent;
    playback_error_t error;
    bool retryable;
    uint32_t position_ms;
    uint32_t duration_ms;
    char diagnostic[PLAYBACK_DIAGNOSTIC_MAX_LEN + 1U];
} playback_report_t;

typedef struct
{
    bool has_session;
    playback_session_t session;
    playback_state_t state;
    playback_intent_t intent;
    uint32_t position_ms;
    char boot_id[PLAYBACK_BOOT_ID_MAX_LEN + 1U];
    playback_error_t error;
    bool retryable;
    char diagnostic[PLAYBACK_DIAGNOSTIC_MAX_LEN + 1U];
} playback_snapshot_t;

typedef struct
{
    playback_event_kind_t kind;
    const playback_session_t *session;
    uint32_t position_ms;
    playback_error_t error;
    bool retryable;
    const char *diagnostic;
} playback_event_t;

void playback_snapshot_init(playback_snapshot_t *snapshot, const char *boot_id);
playback_result_t playback_snapshot_apply(playback_snapshot_t *snapshot, const playback_event_t *event);
bool playback_snapshot_has_active_session(const playback_snapshot_t *snapshot);
bool playback_session_profile_supported(const playback_session_t *session);

const char *playback_state_name(playback_state_t state);
const char *playback_error_name(playback_error_t error);
const char *playback_command_name(playback_command_kind_t command);
const char *playback_report_name(playback_report_kind_t report);
const char *playback_nack_name(playback_nack_t nack);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_DOMAIN_H */
