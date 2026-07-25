/**
 * @file playback_wired_speaker.c
 * @brief Mono onboard-speaker PCM sink with an audible media clock.
 */

#include "playback_wired_speaker.h"

#include <limits.h>
#include <string.h>

#include "tal_api.h"
#include "tkl_speaker.h"

#define PLAYBACK_WIRED_SPEAKER_WRITE_RESULT_COMPLETE(result, expected_bytes) \
    (((result) == OPRT_OK) || \
     (((result) > 0) && ((uint32_t)(result) == (expected_bytes))))

_Static_assert(
    PLAYBACK_WIRED_SPEAKER_WRITE_RESULT_COMPLETE(OPRT_OK, 1764U),
    "generic TKL success must be accepted"
);
_Static_assert(
    PLAYBACK_WIRED_SPEAKER_WRITE_RESULT_COMPLETE(1764, 1764U),
    "T5AI byte-count success must be accepted"
);
_Static_assert(
    !PLAYBACK_WIRED_SPEAKER_WRITE_RESULT_COMPLETE(882, 1764U),
    "partial speaker writes must be rejected"
);

static bool playback_wired_speaker_write_result_complete(
    int32_t result,
    uint32_t expected_bytes
)
{
    return PLAYBACK_WIRED_SPEAKER_WRITE_RESULT_COMPLETE(result, expected_bytes);
}

typedef struct
{
    playback_wired_speaker_config_t config;
    int16_t *pcm;
    size_t write_frames;
    uint64_t base_pcm_frame;
    uint64_t submitted_pcm_frames;
    uint64_t started_ms;
    uint64_t pause_started_ms;
    uint64_t paused_total_ms;
    bool initialized;
    bool started;
    bool paused;
} playback_wired_speaker_state_t;

static playback_wired_speaker_state_t *playback_wired_speaker_get_state(
    const playback_wired_speaker_t *speaker
)
{
    return (speaker == NULL) ? NULL : (playback_wired_speaker_state_t *)speaker->state;
}

static uint64_t playback_wired_speaker_audible_frames(
    const playback_wired_speaker_state_t *state
)
{
    uint64_t now_ms;
    uint64_t running_ms;
    uint64_t audible_ms;
    uint64_t audible_frames;

    if ((state == NULL) || !state->started)
    {
        return (state == NULL) ? 0ULL : state->base_pcm_frame;
    }

    now_ms = state->paused ? state->pause_started_ms : tal_system_get_millisecond();
    running_ms = (now_ms > state->started_ms)
                     ? now_ms - state->started_ms
                     : 0ULL;
    running_ms = (running_ms > state->paused_total_ms)
                     ? running_ms - state->paused_total_ms
                     : 0ULL;
    audible_ms = (running_ms > state->config.output_latency_ms)
                     ? running_ms - state->config.output_latency_ms
                     : 0ULL;
    audible_frames = state->base_pcm_frame +
                     ((audible_ms * state->config.sample_rate) / 1000ULL);
    return (audible_frames > state->submitted_pcm_frames)
               ? state->submitted_pcm_frames
               : audible_frames;
}

static void playback_wired_speaker_downmix(
    int16_t *pcm,
    size_t frame_count,
    uint8_t channels
)
{
    size_t frame;

    if ((pcm == NULL) || (channels != 2U))
    {
        return;
    }
    for (frame = 0U; frame < frame_count; ++frame)
    {
        const int32_t mixed = (int32_t)pcm[frame * 2U] +
                              (int32_t)pcm[(frame * 2U) + 1U];
        pcm[frame] = (int16_t)(mixed / 2);
    }
}

playback_wired_speaker_result_t playback_wired_speaker_open(
    playback_wired_speaker_t *speaker,
    const playback_wired_speaker_config_t *config
)
{
    playback_wired_speaker_state_t *state;
    TKL_SPK_CFG_T speaker_config;
    uint64_t sample_capacity;

    if ((speaker == NULL) || (config == NULL) || (config->read_pcm == NULL) ||
        (config->sample_rate == 0U) ||
        ((config->source_channels != 1U) && (config->source_channels != 2U)))
    {
        return PLAYBACK_WIRED_SPEAKER_INVALID_ARGUMENT;
    }
    if (speaker->state != NULL)
    {
        return PLAYBACK_WIRED_SPEAKER_ALREADY_OPEN;
    }

    state = tal_calloc(1U, sizeof(*state));
    if (state == NULL)
    {
        return PLAYBACK_WIRED_SPEAKER_NO_MEMORY;
    }
    state->config = *config;
    if (state->config.volume == 0U)
    {
        state->config.volume = PLAYBACK_WIRED_SPEAKER_DEFAULT_VOLUME;
    }
    if (state->config.amplifier_gpio == 0U)
    {
        state->config.amplifier_gpio = PLAYBACK_WIRED_SPEAKER_DEFAULT_GPIO;
    }
    if (state->config.output_latency_ms == 0U)
    {
        state->config.output_latency_ms = PLAYBACK_WIRED_SPEAKER_DEFAULT_LATENCY_MS;
    }
    state->write_frames =
        (state->config.sample_rate * PLAYBACK_WIRED_SPEAKER_WRITE_MS) / 1000U;
    if (state->write_frames == 0U)
    {
        tal_free(state);
        return PLAYBACK_WIRED_SPEAKER_INVALID_ARGUMENT;
    }

    sample_capacity = (uint64_t)state->write_frames * state->config.source_channels;
    if (sample_capacity > (SIZE_MAX / sizeof(int16_t)))
    {
        tal_free(state);
        return PLAYBACK_WIRED_SPEAKER_NO_MEMORY;
    }
    state->pcm = tal_psram_malloc((size_t)sample_capacity * sizeof(int16_t));
    if (state->pcm == NULL)
    {
        tal_free(state);
        return PLAYBACK_WIRED_SPEAKER_NO_MEMORY;
    }

    memset(&speaker_config, 0, sizeof(speaker_config));
    speaker_config.chl_num = 1U;
    speaker_config.sample_rate = state->config.sample_rate;
    speaker_config.datebits = TKL_SPK_DATABITS_16;
    speaker_config.volume = state->config.volume;
    speaker_config.card = TKL_SPK_TYPE_BOARD;
    speaker_config.codectype = TKL_CODEC_SPK_PCM;
    speaker_config.spk_gpio = state->config.amplifier_gpio;
    speaker_config.spk_gpio_polarity = state->config.amplifier_gpio_polarity;
    if (tkl_speaker_init(&speaker_config) != OPRT_OK)
    {
        tal_psram_free(state->pcm);
        tal_free(state);
        return PLAYBACK_WIRED_SPEAKER_PLATFORM_ERROR;
    }

    state->initialized = true;
    speaker->state = state;
    return PLAYBACK_WIRED_SPEAKER_OK;
}

playback_wired_speaker_result_t playback_wired_speaker_start(
    playback_wired_speaker_t *speaker
)
{
    playback_wired_speaker_state_t *state = playback_wired_speaker_get_state(speaker);

    if (state == NULL)
    {
        return PLAYBACK_WIRED_SPEAKER_NOT_OPEN;
    }
    if (state->started)
    {
        return state->paused
                   ? playback_wired_speaker_resume(speaker)
                   : PLAYBACK_WIRED_SPEAKER_OK;
    }
    if (tkl_speaker_start() != OPRT_OK)
    {
        return PLAYBACK_WIRED_SPEAKER_PLATFORM_ERROR;
    }
    state->started = true;
    state->paused = false;
    state->started_ms = tal_system_get_millisecond();
    state->pause_started_ms = 0ULL;
    state->paused_total_ms = 0ULL;
    return PLAYBACK_WIRED_SPEAKER_OK;
}

playback_wired_speaker_result_t playback_wired_speaker_pause(
    playback_wired_speaker_t *speaker
)
{
    playback_wired_speaker_state_t *state = playback_wired_speaker_get_state(speaker);

    if (state == NULL)
    {
        return PLAYBACK_WIRED_SPEAKER_NOT_OPEN;
    }
    if (!state->started || state->paused)
    {
        return PLAYBACK_WIRED_SPEAKER_OK;
    }
    if (tkl_speaker_pause() != OPRT_OK)
    {
        return PLAYBACK_WIRED_SPEAKER_PLATFORM_ERROR;
    }
    state->pause_started_ms = tal_system_get_millisecond();
    state->paused = true;
    return PLAYBACK_WIRED_SPEAKER_OK;
}

playback_wired_speaker_result_t playback_wired_speaker_resume(
    playback_wired_speaker_t *speaker
)
{
    playback_wired_speaker_state_t *state = playback_wired_speaker_get_state(speaker);
    uint64_t now_ms;

    if (state == NULL)
    {
        return PLAYBACK_WIRED_SPEAKER_NOT_OPEN;
    }
    if (!state->started)
    {
        return playback_wired_speaker_start(speaker);
    }
    if (!state->paused)
    {
        return PLAYBACK_WIRED_SPEAKER_OK;
    }
    if (tkl_speaker_resume() != OPRT_OK)
    {
        return PLAYBACK_WIRED_SPEAKER_PLATFORM_ERROR;
    }
    now_ms = tal_system_get_millisecond();
    if (now_ms > state->pause_started_ms)
    {
        state->paused_total_ms += now_ms - state->pause_started_ms;
    }
    state->pause_started_ms = 0ULL;
    state->paused = false;
    return PLAYBACK_WIRED_SPEAKER_OK;
}

playback_wired_speaker_result_t playback_wired_speaker_pump(
    playback_wired_speaker_t *speaker
)
{
    playback_wired_speaker_state_t *state = playback_wired_speaker_get_state(speaker);
    size_t consumed;
    uint32_t output_bytes;
    int32_t write_result;

    if (state == NULL)
    {
        return PLAYBACK_WIRED_SPEAKER_NOT_OPEN;
    }
    if (!state->started || state->paused)
    {
        return PLAYBACK_WIRED_SPEAKER_NO_DATA;
    }

    consumed = state->config.read_pcm(
        state->config.context,
        state->pcm,
        state->write_frames
    );
    if (consumed == 0U)
    {
        return PLAYBACK_WIRED_SPEAKER_NO_DATA;
    }
    if (consumed > state->write_frames)
    {
        return PLAYBACK_WIRED_SPEAKER_PLATFORM_ERROR;
    }

    playback_wired_speaker_downmix(
        state->pcm,
        consumed,
        state->config.source_channels
    );
    if (consumed > (UINT32_MAX / sizeof(int16_t)))
    {
        return PLAYBACK_WIRED_SPEAKER_PLATFORM_ERROR;
    }
    output_bytes = (uint32_t)(consumed * sizeof(int16_t));
    write_result = tkl_speaker_write((uint8_t *)state->pcm, output_bytes);
    if (!playback_wired_speaker_write_result_complete(write_result, output_bytes))
    {
        PR_ERR(
            "Wired speaker write failed: result=%d expected=%u",
            (int)write_result,
            (unsigned int)output_bytes
        );
        return PLAYBACK_WIRED_SPEAKER_PLATFORM_ERROR;
    }
    state->submitted_pcm_frames += consumed;
    return PLAYBACK_WIRED_SPEAKER_OK;
}

playback_wired_speaker_result_t playback_wired_speaker_reset(
    playback_wired_speaker_t *speaker,
    uint64_t pcm_frame_position
)
{
    playback_wired_speaker_state_t *state = playback_wired_speaker_get_state(speaker);

    if (state == NULL)
    {
        return PLAYBACK_WIRED_SPEAKER_NOT_OPEN;
    }
    if (state->started && (tkl_speaker_stop() != OPRT_OK))
    {
        return PLAYBACK_WIRED_SPEAKER_PLATFORM_ERROR;
    }
    state->base_pcm_frame = pcm_frame_position;
    state->submitted_pcm_frames = pcm_frame_position;
    state->started_ms = 0ULL;
    state->pause_started_ms = 0ULL;
    state->paused_total_ms = 0ULL;
    state->started = false;
    state->paused = false;
    return PLAYBACK_WIRED_SPEAKER_OK;
}

playback_wired_speaker_result_t playback_wired_speaker_get_status(
    playback_wired_speaker_t *speaker,
    playback_wired_speaker_status_t *status
)
{
    playback_wired_speaker_state_t *state = playback_wired_speaker_get_state(speaker);

    if ((state == NULL) || (status == NULL))
    {
        return (status == NULL)
                   ? PLAYBACK_WIRED_SPEAKER_INVALID_ARGUMENT
                   : PLAYBACK_WIRED_SPEAKER_NOT_OPEN;
    }
    memset(status, 0, sizeof(*status));
    status->open = true;
    status->started = state->started;
    status->paused = state->paused;
    status->sample_rate = state->config.sample_rate;
    status->source_channels = state->config.source_channels;
    status->output_latency_ms = state->config.output_latency_ms;
    status->submitted_pcm_frames = state->submitted_pcm_frames;
    status->audible_pcm_frames = playback_wired_speaker_audible_frames(state);
    return PLAYBACK_WIRED_SPEAKER_OK;
}

void playback_wired_speaker_close(playback_wired_speaker_t *speaker)
{
    playback_wired_speaker_state_t *state;

    if ((speaker == NULL) || (speaker->state == NULL))
    {
        return;
    }
    state = playback_wired_speaker_get_state(speaker);
    speaker->state = NULL;
    if (state->initialized)
    {
        /* T5AI deinit owns pipeline stop/wait/terminate; do not stop twice. */
        tkl_speaker_deinit();
    }
    if (state->pcm != NULL)
    {
        tal_psram_free(state->pcm);
    }
    tal_free(state);
}

const char *playback_wired_speaker_result_name(
    playback_wired_speaker_result_t result
)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "ALREADY_OPEN",
        "NOT_OPEN",
        "NO_MEMORY",
        "NO_DATA",
        "PLATFORM_ERROR",
    };

    return (result < (sizeof(names) / sizeof(names[0]))) ? names[result] : "UNKNOWN";
}
