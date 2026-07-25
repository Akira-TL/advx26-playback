#ifndef PLAYBACK_WIRED_SPEAKER_H
#define PLAYBACK_WIRED_SPEAKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_WIRED_SPEAKER_DEFAULT_VOLUME (60U)
#define PLAYBACK_WIRED_SPEAKER_DEFAULT_GPIO (28U)
#define PLAYBACK_WIRED_SPEAKER_DEFAULT_GPIO_POLARITY (0U)
#define PLAYBACK_WIRED_SPEAKER_DEFAULT_LATENCY_MS (40U)
#define PLAYBACK_WIRED_SPEAKER_WRITE_MS (20U)

typedef enum
{
    PLAYBACK_WIRED_SPEAKER_OK = 0,
    PLAYBACK_WIRED_SPEAKER_INVALID_ARGUMENT,
    PLAYBACK_WIRED_SPEAKER_ALREADY_OPEN,
    PLAYBACK_WIRED_SPEAKER_NOT_OPEN,
    PLAYBACK_WIRED_SPEAKER_NO_MEMORY,
    PLAYBACK_WIRED_SPEAKER_NO_DATA,
    PLAYBACK_WIRED_SPEAKER_PLATFORM_ERROR,
} playback_wired_speaker_result_t;

typedef size_t (*playback_wired_speaker_read_pcm_cb)(
    void *context,
    int16_t *destination,
    size_t frame_capacity
);

typedef struct
{
    uint32_t sample_rate;
    uint8_t source_channels;
    uint8_t volume;
    uint8_t amplifier_gpio;
    uint8_t amplifier_gpio_polarity;
    uint32_t output_latency_ms;
    playback_wired_speaker_read_pcm_cb read_pcm;
    void *context;
} playback_wired_speaker_config_t;

typedef struct
{
    bool open;
    bool started;
    bool paused;
    uint32_t sample_rate;
    uint8_t source_channels;
    uint32_t output_latency_ms;
    uint64_t submitted_pcm_frames;
    uint64_t audible_pcm_frames;
} playback_wired_speaker_status_t;

typedef struct
{
    void *state;
} playback_wired_speaker_t;

playback_wired_speaker_result_t playback_wired_speaker_open(
    playback_wired_speaker_t *speaker,
    const playback_wired_speaker_config_t *config
);

playback_wired_speaker_result_t playback_wired_speaker_start(
    playback_wired_speaker_t *speaker
);

playback_wired_speaker_result_t playback_wired_speaker_pause(
    playback_wired_speaker_t *speaker
);

playback_wired_speaker_result_t playback_wired_speaker_resume(
    playback_wired_speaker_t *speaker
);

/** Consume and submit at most one 20 ms PCM block. */
playback_wired_speaker_result_t playback_wired_speaker_pump(
    playback_wired_speaker_t *speaker
);

/** Reset the hardware queue and clock to an absolute PCM frame position. */
playback_wired_speaker_result_t playback_wired_speaker_reset(
    playback_wired_speaker_t *speaker,
    uint64_t pcm_frame_position
);

playback_wired_speaker_result_t playback_wired_speaker_get_status(
    playback_wired_speaker_t *speaker,
    playback_wired_speaker_status_t *status
);

void playback_wired_speaker_close(playback_wired_speaker_t *speaker);
const char *playback_wired_speaker_result_name(
    playback_wired_speaker_result_t result
);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_WIRED_SPEAKER_H */
