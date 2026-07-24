#ifndef PLAYBACK_SPEAKER_LINK_H
#define PLAYBACK_SPEAKER_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_SPEAKER_LINK_ADDRESS_BYTES (6U)
#define PLAYBACK_SPEAKER_LINK_SAMPLE_RATE   (44100U)
#define PLAYBACK_SPEAKER_LINK_BITS_PER_SAMPLE (16U)

typedef enum
{
    PLAYBACK_SPEAKER_LINK_OK = 0,
    PLAYBACK_SPEAKER_LINK_INVALID_ARGUMENT,
    PLAYBACK_SPEAKER_LINK_ALREADY_INITIALIZED,
    PLAYBACK_SPEAKER_LINK_NOT_INITIALIZED,
    PLAYBACK_SPEAKER_LINK_NOT_READY,
    PLAYBACK_SPEAKER_LINK_NOT_CONNECTED,
    PLAYBACK_SPEAKER_LINK_UNSUPPORTED_FORMAT,
    PLAYBACK_SPEAKER_LINK_QUEUE_FULL,
    PLAYBACK_SPEAKER_LINK_NO_MEMORY,
    PLAYBACK_SPEAKER_LINK_PLATFORM_ERROR,
} playback_speaker_link_result_t;

typedef enum
{
    PLAYBACK_SPEAKER_DISCONNECTED = 0,
    PLAYBACK_SPEAKER_CONNECTING,
    PLAYBACK_SPEAKER_CONNECTED,
    PLAYBACK_SPEAKER_STREAMING,
} playback_speaker_state_t;

typedef struct
{
    bool initialized;
    bool profile_ready;
    bool desired_connected;
    bool desired_streaming;
    bool codec_ready;
    playback_speaker_state_t state;
    uint8_t target_address[PLAYBACK_SPEAKER_LINK_ADDRESS_BYTES];
    uint8_t remote_address[PLAYBACK_SPEAKER_LINK_ADDRESS_BYTES];
    uint32_t input_sample_rate;
    uint8_t input_channels;
    uint32_t negotiated_sample_rate;
    uint8_t negotiated_channels;
    uint16_t negotiated_mtu;
    uint8_t negotiated_bit_pool;
    uint32_t reconnect_attempts;
    uint64_t consumed_pcm_frames;
    playback_speaker_link_result_t last_result;
    int32_t last_platform_error;
} playback_speaker_link_status_t;

/**
 * Pull interleaved signed 16-bit PCM frames.
 *
 * The callback returns the number of complete PCM frames written. It must not
 * return more than frame_capacity. Returning zero keeps the media clock still.
 * It runs on the Bluetooth media task and must remain bounded and non-blocking;
 * it must not close the Speaker Link from inside the callback.
 */
typedef size_t (*playback_speaker_link_read_pcm_cb)(
    void *context,
    int16_t *destination,
    size_t frame_capacity
);

typedef void (*playback_speaker_link_flush_pcm_cb)(void *context);

/* Runs on the Speaker Link worker; it must not close the link synchronously. */
typedef void (*playback_speaker_link_status_cb)(
    void *context,
    const playback_speaker_link_status_t *status
);

typedef struct
{
    uint8_t target_address[PLAYBACK_SPEAKER_LINK_ADDRESS_BYTES];
    uint32_t sample_rate;
    uint8_t channels;
    playback_speaker_link_read_pcm_cb read_pcm;
    playback_speaker_link_flush_pcm_cb flush_pcm;
    playback_speaker_link_status_cb on_status;
    void *context;
} playback_speaker_link_config_t;

typedef struct
{
    void *state;
} playback_speaker_link_t;

playback_speaker_link_result_t playback_speaker_link_init(
    playback_speaker_link_t *link,
    const playback_speaker_link_config_t *config
);

playback_speaker_link_result_t playback_speaker_link_connect(playback_speaker_link_t *link);
playback_speaker_link_result_t playback_speaker_link_reconnect(playback_speaker_link_t *link);
playback_speaker_link_result_t playback_speaker_link_disconnect(playback_speaker_link_t *link);
playback_speaker_link_result_t playback_speaker_link_start(playback_speaker_link_t *link);
playback_speaker_link_result_t playback_speaker_link_stop(playback_speaker_link_t *link);

playback_speaker_link_result_t playback_speaker_link_get_status(
    playback_speaker_link_t *link,
    playback_speaker_link_status_t *status
);

void playback_speaker_link_close(playback_speaker_link_t *link);
const char *playback_speaker_link_result_name(playback_speaker_link_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_SPEAKER_LINK_H */
