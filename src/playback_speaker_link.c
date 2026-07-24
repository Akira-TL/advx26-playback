#include "playback_speaker_link.h"

#include <stdint.h>
#include <string.h>

#include "components/bluetooth/bk_dm_a2dp.h"
#include "modules/sbc_encoder.h"
#include "tal_api.h"

#define PLAYBACK_SPEAKER_EVENT_QUEUE_DEPTH       (24)
#define PLAYBACK_SPEAKER_WORKER_STACK_SIZE       (6U * 1024U)
#define PLAYBACK_SPEAKER_WORKER_POLL_MS          (250U)
#define PLAYBACK_SPEAKER_RECONNECT_INITIAL_MS    (1000U)
#define PLAYBACK_SPEAKER_RECONNECT_MAX_MS        (5000U)
#define PLAYBACK_SPEAKER_CONNECT_TIMEOUT_MS      (10000U)
#define PLAYBACK_SPEAKER_MEDIA_TIMEOUT_MS        (3000U)
#define PLAYBACK_SPEAKER_SBC_CODEC_TYPE          (0U)
#define PLAYBACK_SPEAKER_SBC_MAX_PCM_FRAMES      (128U)
#define PLAYBACK_SPEAKER_SBC_MAX_PCM_SAMPLES     (PLAYBACK_SPEAKER_SBC_MAX_PCM_FRAMES * 2U)
#define PLAYBACK_SPEAKER_SBC_MAX_FRAME_BYTES     (128U)
#define PLAYBACK_SPEAKER_INVALID_PLATFORM_ERROR  (-1)

typedef enum
{
    PLAYBACK_SPEAKER_EVENT_WAKE = 0,
    PLAYBACK_SPEAKER_EVENT_FORCE_RECONNECT,
    PLAYBACK_SPEAKER_EVENT_PROFILE,
    PLAYBACK_SPEAKER_EVENT_CONNECTION,
    PLAYBACK_SPEAKER_EVENT_AUDIO,
    PLAYBACK_SPEAKER_EVENT_CODEC,
    PLAYBACK_SPEAKER_EVENT_MEDIA_ACK,
    PLAYBACK_SPEAKER_EVENT_STOP,
} playback_speaker_event_kind_t;

typedef enum
{
    PLAYBACK_SPEAKER_MEDIA_NONE = 0,
    PLAYBACK_SPEAKER_MEDIA_START,
    PLAYBACK_SPEAKER_MEDIA_SUSPEND,
} playback_speaker_media_request_t;

typedef struct
{
    playback_speaker_event_kind_t kind;
    union
    {
        struct
        {
            uint8_t action;
            uint8_t role;
            uint8_t status;
            uint8_t reason;
        } profile;
        struct
        {
            bk_a2dp_connection_state_t state;
            uint8_t address[PLAYBACK_SPEAKER_LINK_ADDRESS_BYTES];
            bk_a2dp_disc_rsn_t reason;
        } connection;
        struct
        {
            bk_a2dp_audio_state_t state;
            uint8_t address[PLAYBACK_SPEAKER_LINK_ADDRESS_BYTES];
        } audio;
        struct
        {
            uint8_t address[PLAYBACK_SPEAKER_LINK_ADDRESS_BYTES];
            uint16_t mtu;
            bk_a2dp_mcc_t mcc;
        } codec;
        struct
        {
            bk_a2dp_media_ctrl_t command;
            bk_a2dp_media_ctrl_ack_t status;
        } media_ack;
    } data;
} playback_speaker_event_t;

typedef struct
{
    playback_speaker_link_config_t config;
    playback_speaker_link_status_t status;
    MUTEX_HANDLE status_mutex;
    MUTEX_HANDLE pcm_mutex;
    QUEUE_HANDLE event_queue;
    SEM_HANDLE worker_stopped;
    THREAD_HANDLE worker_thread;
    SbcEncoderContext encoder;
    int16_t encode_pcm[PLAYBACK_SPEAKER_SBC_MAX_PCM_SAMPLES];
    bool encoder_ready;
    bool source_init_requested;
    bool closing;
    playback_speaker_media_request_t media_request;
    uint32_t media_request_ms;
    uint32_t connect_request_ms;
    uint32_t next_reconnect_ms;
    uint32_t reconnect_delay_ms;
} playback_speaker_link_state_t;

static playback_speaker_link_state_t *playback_speaker_active_state = NULL;
static MUTEX_HANDLE playback_speaker_callback_mutex = NULL;

static bool playback_speaker_time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static bool playback_speaker_address_equal(const uint8_t *left, const uint8_t *right)
{
    return memcmp(left, right, PLAYBACK_SPEAKER_LINK_ADDRESS_BYTES) == 0;
}

static bool playback_speaker_address_valid(const uint8_t *address)
{
    bool any_nonzero = false;
    bool any_not_ff = false;
    size_t index;

    if (address == NULL)
    {
        return false;
    }
    for (index = 0U; index < PLAYBACK_SPEAKER_LINK_ADDRESS_BYTES; ++index)
    {
        any_nonzero = any_nonzero || (address[index] != 0U);
        any_not_ff = any_not_ff || (address[index] != 0xFFU);
    }
    return any_nonzero && any_not_ff;
}

static void playback_speaker_status_snapshot(
    playback_speaker_link_state_t *state,
    playback_speaker_link_status_t *status
)
{
    tal_mutex_lock(state->status_mutex);
    *status = state->status;
    tal_mutex_unlock(state->status_mutex);
}

static void playback_speaker_publish_status(playback_speaker_link_state_t *state)
{
    playback_speaker_link_status_t status;

    if (state->config.on_status == NULL)
    {
        return;
    }
    playback_speaker_status_snapshot(state, &status);
    state->config.on_status(state->config.context, &status);
}

static void playback_speaker_set_error(
    playback_speaker_link_state_t *state,
    playback_speaker_link_result_t result,
    int32_t platform_error
)
{
    tal_mutex_lock(state->status_mutex);
    state->status.last_result = result;
    state->status.last_platform_error = platform_error;
    tal_mutex_unlock(state->status_mutex);
}

static playback_speaker_link_result_t playback_speaker_enqueue(
    playback_speaker_link_state_t *state,
    const playback_speaker_event_t *event
)
{
    if ((state == NULL) || (event == NULL) || state->closing)
    {
        return PLAYBACK_SPEAKER_LINK_NOT_INITIALIZED;
    }
    if (tal_queue_post(state->event_queue, (void *)event, 0U) != OPRT_OK)
    {
        playback_speaker_set_error(
            state,
            PLAYBACK_SPEAKER_LINK_QUEUE_FULL,
            PLAYBACK_SPEAKER_INVALID_PLATFORM_ERROR
        );
        return PLAYBACK_SPEAKER_LINK_QUEUE_FULL;
    }
    return PLAYBACK_SPEAKER_LINK_OK;
}

static playback_speaker_link_state_t *playback_speaker_callback_enter(void)
{
    playback_speaker_link_state_t *state;

    if (playback_speaker_callback_mutex == NULL)
    {
        return NULL;
    }
    tal_mutex_lock(playback_speaker_callback_mutex);
    state = playback_speaker_active_state;
    if ((state == NULL) || state->closing)
    {
        tal_mutex_unlock(playback_speaker_callback_mutex);
        return NULL;
    }
    return state;
}

static void playback_speaker_callback_leave(void)
{
    tal_mutex_unlock(playback_speaker_callback_mutex);
}

static void playback_speaker_a2dp_callback(
    bk_a2dp_cb_event_t event_kind,
    bk_a2dp_cb_param_t *parameter
)
{
    playback_speaker_link_state_t *state;
    playback_speaker_event_t event;

    if (parameter == NULL)
    {
        return;
    }
    state = playback_speaker_callback_enter();
    if (state == NULL)
    {
        return;
    }

    memset(&event, 0, sizeof(event));
    switch (event_kind)
    {
        case BK_A2DP_PROF_STATE_EVT:
            event.kind = PLAYBACK_SPEAKER_EVENT_PROFILE;
            event.data.profile.action = parameter->a2dp_prof_stat.action;
            event.data.profile.role = parameter->a2dp_prof_stat.role;
            event.data.profile.status = parameter->a2dp_prof_stat.status;
            event.data.profile.reason = parameter->a2dp_prof_stat.reason;
            (void)playback_speaker_enqueue(state, &event);
            break;

        case BK_A2DP_CONNECTION_STATE_EVT:
            event.kind = PLAYBACK_SPEAKER_EVENT_CONNECTION;
            event.data.connection.state = parameter->conn_state.state;
            memcpy(
                event.data.connection.address,
                parameter->conn_state.remote_bda,
                sizeof(event.data.connection.address)
            );
            event.data.connection.reason = parameter->conn_state.disc_rsn;
            (void)playback_speaker_enqueue(state, &event);
            break;

        case BK_A2DP_AUDIO_STATE_EVT:
            event.kind = PLAYBACK_SPEAKER_EVENT_AUDIO;
            event.data.audio.state = parameter->audio_state.state;
            memcpy(
                event.data.audio.address,
                parameter->audio_state.remote_bda,
                sizeof(event.data.audio.address)
            );
            (void)playback_speaker_enqueue(state, &event);
            break;

        case BK_A2DP_AUDIO_SOURCE_CFG_EVT:
            event.kind = PLAYBACK_SPEAKER_EVENT_CODEC;
            memcpy(
                event.data.codec.address,
                parameter->audio_source_cfg.remote_bda,
                sizeof(event.data.codec.address)
            );
            event.data.codec.mtu = parameter->audio_source_cfg.mtu;
            event.data.codec.mcc = parameter->audio_source_cfg.mcc;
            (void)playback_speaker_enqueue(state, &event);
            break;

        case BK_A2DP_MEDIA_CTRL_ACK_EVT:
            event.kind = PLAYBACK_SPEAKER_EVENT_MEDIA_ACK;
            event.data.media_ack.command = parameter->media_ctrl_stat.cmd;
            event.data.media_ack.status = parameter->media_ctrl_stat.status;
            (void)playback_speaker_enqueue(state, &event);
            break;

        case BK_A2DP_AUDIO_CFG_EVT:
        case BK_A2DP_SNK_SET_DELAY_VALUE_EVT:
        case BK_A2DP_SNK_GET_DELAY_VALUE_EVT:
        default:
            break;
    }

    playback_speaker_callback_leave();
}

static int32_t playback_speaker_data_callback(uint8_t *buffer, int32_t length)
{
    playback_speaker_link_state_t *state;
    playback_speaker_link_read_pcm_cb read_pcm;
    playback_speaker_link_flush_pcm_cb flush_pcm;
    void *context;
    size_t frame_bytes;
    size_t frame_capacity;
    size_t frame_count;
    bool enabled;

    state = playback_speaker_callback_enter();
    if (state == NULL)
    {
        return 0;
    }

    tal_mutex_lock(state->pcm_mutex);
    read_pcm = state->config.read_pcm;
    flush_pcm = state->config.flush_pcm;
    context = state->config.context;

    if (length == -1)
    {
        if (flush_pcm != NULL)
        {
            flush_pcm(context);
        }
        tal_mutex_unlock(state->pcm_mutex);
        playback_speaker_callback_leave();
        return 0;
    }
    if ((buffer == NULL) || (length <= 0) || (((uintptr_t)buffer & 1U) != 0U))
    {
        tal_mutex_unlock(state->pcm_mutex);
        playback_speaker_callback_leave();
        return 0;
    }

    tal_mutex_lock(state->status_mutex);
    enabled = state->status.desired_streaming && state->status.codec_ready &&
              ((state->status.state == PLAYBACK_SPEAKER_CONNECTED) ||
               (state->status.state == PLAYBACK_SPEAKER_STREAMING));
    tal_mutex_unlock(state->status_mutex);
    if (!enabled)
    {
        tal_mutex_unlock(state->pcm_mutex);
        playback_speaker_callback_leave();
        return 0;
    }

    frame_bytes = (size_t)state->config.channels * sizeof(int16_t);
    frame_capacity = (size_t)length / frame_bytes;
    if ((frame_capacity == 0U) || (((size_t)length % frame_bytes) != 0U))
    {
        tal_mutex_unlock(state->pcm_mutex);
        playback_speaker_callback_leave();
        return 0;
    }

    frame_count = read_pcm(context, (int16_t *)buffer, frame_capacity);
    if (frame_count > frame_capacity)
    {
        playback_speaker_set_error(
            state,
            PLAYBACK_SPEAKER_LINK_INVALID_ARGUMENT,
            PLAYBACK_SPEAKER_INVALID_PLATFORM_ERROR
        );
        tal_mutex_unlock(state->pcm_mutex);
        playback_speaker_callback_leave();
        return 0;
    }

    tal_mutex_lock(state->status_mutex);
    state->status.consumed_pcm_frames += frame_count;
    tal_mutex_unlock(state->status_mutex);

    tal_mutex_unlock(state->pcm_mutex);
    playback_speaker_callback_leave();
    return (int32_t)(frame_count * frame_bytes);
}

static int16_t playback_speaker_load_i16(const uint8_t *source)
{
    int16_t value;
    memcpy(&value, source, sizeof(value));
    return value;
}

static void playback_speaker_store_i16(uint8_t *destination, int16_t value)
{
    memcpy(destination, &value, sizeof(value));
}

static int32_t playback_speaker_resample_callback(
    uint8_t *input,
    uint32_t *input_length,
    uint8_t *output,
    uint32_t *output_length
)
{
    playback_speaker_link_state_t *state;
    uint8_t input_channels;
    uint8_t output_channels;
    size_t input_frame_bytes;
    size_t output_frame_bytes;
    size_t input_frames;
    size_t output_frames;
    size_t frames;
    size_t index;

    if ((input == NULL) || (input_length == NULL) || (output == NULL) || (output_length == NULL))
    {
        return -1;
    }
    state = playback_speaker_callback_enter();
    if (state == NULL)
    {
        return -1;
    }
    tal_mutex_lock(state->pcm_mutex);

    input_channels = state->config.channels;
    tal_mutex_lock(state->status_mutex);
    output_channels = state->status.negotiated_channels;
    const uint32_t negotiated_sample_rate = state->status.negotiated_sample_rate;
    tal_mutex_unlock(state->status_mutex);
    if (!state->encoder_ready ||
        (negotiated_sample_rate != state->config.sample_rate) ||
        ((input_channels != 1U) && (input_channels != 2U)) ||
        ((output_channels != 1U) && (output_channels != 2U)))
    {
        tal_mutex_unlock(state->pcm_mutex);
        playback_speaker_callback_leave();
        return -1;
    }

    input_frame_bytes = (size_t)input_channels * sizeof(int16_t);
    output_frame_bytes = (size_t)output_channels * sizeof(int16_t);
    if (((*input_length % input_frame_bytes) != 0U) ||
        ((*output_length % output_frame_bytes) != 0U))
    {
        tal_mutex_unlock(state->pcm_mutex);
        playback_speaker_callback_leave();
        return -1;
    }

    input_frames = *input_length / input_frame_bytes;
    output_frames = *output_length / output_frame_bytes;
    frames = (input_frames < output_frames) ? input_frames : output_frames;

    if (input_channels == output_channels)
    {
        memmove(output, input, frames * input_frame_bytes);
    }
    else if ((input_channels == 1U) && (output_channels == 2U))
    {
        for (index = frames; index > 0U; --index)
        {
            const size_t source_index = index - 1U;
            const int16_t sample = playback_speaker_load_i16(
                input + (source_index * sizeof(int16_t))
            );
            playback_speaker_store_i16(
                output + ((source_index * 2U) * sizeof(int16_t)),
                sample
            );
            playback_speaker_store_i16(
                output + (((source_index * 2U) + 1U) * sizeof(int16_t)),
                sample
            );
        }
    }
    else
    {
        for (index = 0U; index < frames; ++index)
        {
            const int16_t left = playback_speaker_load_i16(
                input + ((index * 2U) * sizeof(int16_t))
            );
            const int16_t right = playback_speaker_load_i16(
                input + (((index * 2U) + 1U) * sizeof(int16_t))
            );
            const int16_t mixed = (int16_t)(((int32_t)left + (int32_t)right) / 2);
            playback_speaker_store_i16(output + (index * sizeof(int16_t)), mixed);
        }
    }

    *input_length = (uint32_t)(frames * input_frame_bytes);
    *output_length = (uint32_t)(frames * output_frame_bytes);

    tal_mutex_unlock(state->pcm_mutex);
    playback_speaker_callback_leave();
    return 0;
}

static int32_t playback_speaker_encode_callback(
    uint8_t type,
    uint8_t *input,
    uint32_t *input_length,
    uint8_t *output,
    uint32_t *output_length
)
{
    playback_speaker_link_state_t *state;
    size_t pcm_frame_bytes;
    size_t consumed = 0U;
    size_t produced = 0U;

    if ((type != PLAYBACK_SPEAKER_SBC_CODEC_TYPE) || (input == NULL) ||
        (input_length == NULL) || (output == NULL) || (output_length == NULL))
    {
        return -1;
    }
    state = playback_speaker_callback_enter();
    if (state == NULL)
    {
        return -1;
    }
    tal_mutex_lock(state->pcm_mutex);

    if (!state->encoder_ready || (state->encoder.pcm_length <= 0) ||
        ((state->encoder.num_channels != 1) && (state->encoder.num_channels != 2)))
    {
        tal_mutex_unlock(state->pcm_mutex);
        playback_speaker_callback_leave();
        return -1;
    }

    pcm_frame_bytes = (size_t)state->encoder.pcm_length *
                      (size_t)state->encoder.num_channels * sizeof(int16_t);
    if ((pcm_frame_bytes > sizeof(state->encode_pcm)) ||
        ((*input_length % ((size_t)state->encoder.num_channels * sizeof(int16_t))) != 0U))
    {
        tal_mutex_unlock(state->pcm_mutex);
        playback_speaker_callback_leave();
        return -1;
    }

    while (((size_t)*input_length - consumed) >= pcm_frame_bytes &&
           ((size_t)*output_length - produced) >= PLAYBACK_SPEAKER_SBC_MAX_FRAME_BYTES)
    {
        int32_t encoded_length;

        memcpy(state->encode_pcm, input + consumed, pcm_frame_bytes);
        encoded_length = sbc_encoder_encode(&state->encoder, state->encode_pcm);
        if ((encoded_length <= 0) ||
            ((size_t)encoded_length > PLAYBACK_SPEAKER_SBC_MAX_FRAME_BYTES) ||
            ((size_t)encoded_length > ((size_t)*output_length - produced)))
        {
            tal_mutex_unlock(state->pcm_mutex);
            playback_speaker_callback_leave();
            return -1;
        }

        memcpy(output + produced, state->encoder.stream, (size_t)encoded_length);
        consumed += pcm_frame_bytes;
        produced += (size_t)encoded_length;
    }

    *input_length = (uint32_t)consumed;
    *output_length = (uint32_t)produced;

    tal_mutex_unlock(state->pcm_mutex);
    playback_speaker_callback_leave();
    return 0;
}

static playback_speaker_link_result_t playback_speaker_configure_encoder(
    playback_speaker_link_state_t *state,
    const bk_a2dp_mcc_t *mcc,
    uint16_t mtu
)
{
    uint8_t allocation_method;
    uint8_t block_mode;
    uint8_t channel_mode;
    uint8_t sample_rate_index;
    uint8_t subband_mode;
    int32_t result;

    if ((mcc == NULL) || (mcc->type != PLAYBACK_SPEAKER_SBC_CODEC_TYPE) || (mtu == 0U) ||
        (mcc->cie.sbc_codec.sample_rate != PLAYBACK_SPEAKER_LINK_SAMPLE_RATE) ||
        ((mcc->cie.sbc_codec.channels != 1U) && (mcc->cie.sbc_codec.channels != 2U)) ||
        (mcc->cie.sbc_codec.bit_pool < 2U))
    {
        return PLAYBACK_SPEAKER_LINK_UNSUPPORTED_FORMAT;
    }

    switch (mcc->cie.sbc_codec.alloc_mode)
    {
        case 1U:
            allocation_method = BT_SBC_ALLOCATION_METHOD_LOUDNESS;
            break;
        case 2U:
            allocation_method = BT_SBC_ALLOCATION_METHOD_SNR;
            break;
        default:
            return PLAYBACK_SPEAKER_LINK_UNSUPPORTED_FORMAT;
    }

    switch (mcc->cie.sbc_codec.block_len)
    {
        case 4U:
            block_mode = BT_SBC_BLOCKS_4;
            break;
        case 8U:
            block_mode = BT_SBC_BLOCKS_8;
            break;
        case 12U:
            block_mode = BT_SBC_BLOCKS_12;
            break;
        case 16U:
            block_mode = BT_SBC_BLOCKS_16;
            break;
        default:
            return PLAYBACK_SPEAKER_LINK_UNSUPPORTED_FORMAT;
    }

    switch (mcc->cie.sbc_codec.channel_mode)
    {
        case 8U:
            channel_mode = BT_SBC_CH_MODE_MONO;
            if (mcc->cie.sbc_codec.channels != 1U)
            {
                return PLAYBACK_SPEAKER_LINK_UNSUPPORTED_FORMAT;
            }
            break;
        case 4U:
            channel_mode = BT_SBC_CH_MODE_DUAL_CHANNEL;
            if (mcc->cie.sbc_codec.channels != 2U)
            {
                return PLAYBACK_SPEAKER_LINK_UNSUPPORTED_FORMAT;
            }
            break;
        case 2U:
            channel_mode = BT_SBC_CH_MODE_STEREO;
            if (mcc->cie.sbc_codec.channels != 2U)
            {
                return PLAYBACK_SPEAKER_LINK_UNSUPPORTED_FORMAT;
            }
            break;
        case 1U:
            channel_mode = BT_SBC_CH_MODE_JOINT_STEREO;
            if (mcc->cie.sbc_codec.channels != 2U)
            {
                return PLAYBACK_SPEAKER_LINK_UNSUPPORTED_FORMAT;
            }
            break;
        default:
            return PLAYBACK_SPEAKER_LINK_UNSUPPORTED_FORMAT;
    }

    sample_rate_index = BT_SBC_SAMPLE_RATE_44100;
    switch (mcc->cie.sbc_codec.subbands)
    {
        case 4U:
            subband_mode = BT_SBC_SUBBANDS_4;
            break;
        case 8U:
            subband_mode = BT_SBC_SUBBANDS_8;
            break;
        default:
            return PLAYBACK_SPEAKER_LINK_UNSUPPORTED_FORMAT;
    }

    tal_mutex_lock(state->pcm_mutex);
    memset(&state->encoder, 0, sizeof(state->encoder));
    state->encoder_ready = false;

    result = sbc_encoder_init(
        &state->encoder,
        (int32_t)mcc->cie.sbc_codec.sample_rate,
        (int32_t)mcc->cie.sbc_codec.channels
    );
    if (result == SBC_ENCODER_ERROR_OK)
    {
        result = sbc_encoder_ctrl(
            &state->encoder,
            SBC_ENCODER_CTRL_CMD_SET_ALLOCATION_METHOD,
            allocation_method
        );
    }
    if (result == SBC_ENCODER_ERROR_OK)
    {
        result = sbc_encoder_ctrl(
            &state->encoder,
            SBC_ENCODER_CTRL_CMD_SET_BITPOOL,
            mcc->cie.sbc_codec.bit_pool
        );
    }
    if (result == SBC_ENCODER_ERROR_OK)
    {
        result = sbc_encoder_ctrl(
            &state->encoder,
            SBC_ENCODER_CTRL_CMD_SET_BLOCK_MODE,
            block_mode
        );
    }
    if (result == SBC_ENCODER_ERROR_OK)
    {
        result = sbc_encoder_ctrl(
            &state->encoder,
            SBC_ENCODER_CTRL_CMD_SET_CHANNEL_MODE,
            channel_mode
        );
    }
    if (result == SBC_ENCODER_ERROR_OK)
    {
        result = sbc_encoder_ctrl(
            &state->encoder,
            SBC_ENCODER_CTRL_CMD_SET_SAMPLE_RATE_INDEX,
            sample_rate_index
        );
    }
    if (result == SBC_ENCODER_ERROR_OK)
    {
        result = sbc_encoder_ctrl(
            &state->encoder,
            SBC_ENCODER_CTRL_CMD_SET_SUBBAND_MODE,
            subband_mode
        );
    }
    if ((result == SBC_ENCODER_ERROR_OK) && (state->encoder.pcm_length > 0) &&
        ((uint32_t)state->encoder.pcm_length <= PLAYBACK_SPEAKER_SBC_MAX_PCM_FRAMES))
    {
        state->encoder_ready = true;
    }
    tal_mutex_unlock(state->pcm_mutex);

    if (!state->encoder_ready)
    {
        return PLAYBACK_SPEAKER_LINK_UNSUPPORTED_FORMAT;
    }

    tal_mutex_lock(state->status_mutex);
    state->status.codec_ready = true;
    state->status.negotiated_sample_rate = mcc->cie.sbc_codec.sample_rate;
    state->status.negotiated_channels = mcc->cie.sbc_codec.channels;
    state->status.negotiated_mtu = mtu;
    state->status.negotiated_bit_pool = mcc->cie.sbc_codec.bit_pool;
    state->status.last_result = PLAYBACK_SPEAKER_LINK_OK;
    state->status.last_platform_error = 0;
    tal_mutex_unlock(state->status_mutex);
    return PLAYBACK_SPEAKER_LINK_OK;
}

static void playback_speaker_schedule_retry(playback_speaker_link_state_t *state, uint32_t now)
{
    state->next_reconnect_ms = now + state->reconnect_delay_ms;
    if (state->reconnect_delay_ms < PLAYBACK_SPEAKER_RECONNECT_MAX_MS)
    {
        uint32_t next_delay = state->reconnect_delay_ms * 2U;
        state->reconnect_delay_ms = (next_delay > PLAYBACK_SPEAKER_RECONNECT_MAX_MS)
                                            ? PLAYBACK_SPEAKER_RECONNECT_MAX_MS
                                            : next_delay;
    }
}

static void playback_speaker_reconcile(playback_speaker_link_state_t *state)
{
    playback_speaker_link_status_t status;
    uint32_t now = (uint32_t)tal_system_get_millisecond();

    playback_speaker_status_snapshot(state, &status);
    if (!status.profile_ready || state->closing)
    {
        return;
    }

    if ((status.state == PLAYBACK_SPEAKER_CONNECTING) &&
        playback_speaker_time_reached(now, state->connect_request_ms + PLAYBACK_SPEAKER_CONNECT_TIMEOUT_MS))
    {
        tal_mutex_lock(state->status_mutex);
        state->status.state = PLAYBACK_SPEAKER_DISCONNECTED;
        memset(state->status.remote_address, 0, sizeof(state->status.remote_address));
        state->status.last_result = PLAYBACK_SPEAKER_LINK_PLATFORM_ERROR;
        state->status.last_platform_error = PLAYBACK_SPEAKER_INVALID_PLATFORM_ERROR;
        tal_mutex_unlock(state->status_mutex);
        playback_speaker_schedule_retry(state, now);
        playback_speaker_status_snapshot(state, &status);
    }

    if ((state->media_request != PLAYBACK_SPEAKER_MEDIA_NONE) &&
        playback_speaker_time_reached(now, state->media_request_ms + PLAYBACK_SPEAKER_MEDIA_TIMEOUT_MS))
    {
        state->media_request = PLAYBACK_SPEAKER_MEDIA_NONE;
        playback_speaker_set_error(
            state,
            PLAYBACK_SPEAKER_LINK_PLATFORM_ERROR,
            PLAYBACK_SPEAKER_INVALID_PLATFORM_ERROR
        );
        playback_speaker_status_snapshot(state, &status);
    }

    if (!status.desired_connected)
    {
        if (status.state == PLAYBACK_SPEAKER_STREAMING &&
            (state->media_request == PLAYBACK_SPEAKER_MEDIA_NONE))
        {
            const int32_t result = bk_a2dp_media_ctrl(BK_A2DP_MEDIA_CTRL_SUSPEND);
            if (result == 0)
            {
                state->media_request = PLAYBACK_SPEAKER_MEDIA_SUSPEND;
                state->media_request_ms = now;
            }
            else
            {
                playback_speaker_set_error(state, PLAYBACK_SPEAKER_LINK_PLATFORM_ERROR, result);
            }
            return;
        }
        if ((status.state == PLAYBACK_SPEAKER_CONNECTED) ||
            (status.state == PLAYBACK_SPEAKER_STREAMING) ||
            (status.state == PLAYBACK_SPEAKER_CONNECTING))
        {
            const int32_t result = bk_bt_a2dp_source_disconnect(state->config.target_address);
            if (result != 0)
            {
                playback_speaker_set_error(state, PLAYBACK_SPEAKER_LINK_PLATFORM_ERROR, result);
            }
        }
        return;
    }

    if ((status.state == PLAYBACK_SPEAKER_DISCONNECTED) &&
        playback_speaker_time_reached(now, state->next_reconnect_ms))
    {
        const int32_t result = bk_bt_a2dp_source_connect(state->config.target_address);

        tal_mutex_lock(state->status_mutex);
        if (result == 0)
        {
            state->status.state = PLAYBACK_SPEAKER_CONNECTING;
            state->status.reconnect_attempts++;
            state->status.last_result = PLAYBACK_SPEAKER_LINK_OK;
            state->status.last_platform_error = 0;
            state->connect_request_ms = now;
        }
        else
        {
            state->status.last_result = PLAYBACK_SPEAKER_LINK_PLATFORM_ERROR;
            state->status.last_platform_error = result;
        }
        tal_mutex_unlock(state->status_mutex);
        if (result != 0)
        {
            playback_speaker_schedule_retry(state, now);
        }
        playback_speaker_publish_status(state);
        return;
    }

    if ((status.state == PLAYBACK_SPEAKER_CONNECTED) && status.desired_streaming &&
        status.codec_ready && (state->media_request == PLAYBACK_SPEAKER_MEDIA_NONE))
    {
        const int32_t result = bk_a2dp_media_ctrl(BK_A2DP_MEDIA_CTRL_START);
        if (result == 0)
        {
            state->media_request = PLAYBACK_SPEAKER_MEDIA_START;
            state->media_request_ms = now;
        }
        else
        {
            playback_speaker_set_error(state, PLAYBACK_SPEAKER_LINK_PLATFORM_ERROR, result);
        }
    }
    else if ((status.state == PLAYBACK_SPEAKER_STREAMING) && !status.desired_streaming &&
             (state->media_request == PLAYBACK_SPEAKER_MEDIA_NONE))
    {
        const int32_t result = bk_a2dp_media_ctrl(BK_A2DP_MEDIA_CTRL_SUSPEND);
        if (result == 0)
        {
            state->media_request = PLAYBACK_SPEAKER_MEDIA_SUSPEND;
            state->media_request_ms = now;
        }
        else
        {
            playback_speaker_set_error(state, PLAYBACK_SPEAKER_LINK_PLATFORM_ERROR, result);
        }
    }
}

static void playback_speaker_process_profile(
    playback_speaker_link_state_t *state,
    const playback_speaker_event_t *event
)
{
    if (event->data.profile.role != 0U)
    {
        return;
    }

    tal_mutex_lock(state->status_mutex);
    if (event->data.profile.action == 0U)
    {
        state->status.profile_ready = (event->data.profile.status == 0U);
        state->status.last_result = state->status.profile_ready
                                        ? PLAYBACK_SPEAKER_LINK_OK
                                        : PLAYBACK_SPEAKER_LINK_PLATFORM_ERROR;
        state->status.last_platform_error = event->data.profile.status;
    }
    else
    {
        state->status.profile_ready = false;
        state->status.codec_ready = false;
        state->status.state = PLAYBACK_SPEAKER_DISCONNECTED;
    }
    tal_mutex_unlock(state->status_mutex);
    playback_speaker_publish_status(state);
}

static void playback_speaker_process_connection(
    playback_speaker_link_state_t *state,
    const playback_speaker_event_t *event
)
{
    const uint32_t now = (uint32_t)tal_system_get_millisecond();

    if (!playback_speaker_address_equal(
            event->data.connection.address,
            state->config.target_address
        ))
    {
        if (event->data.connection.state != BK_A2DP_CONNECTION_STATE_DISCONNECTED)
        {
            uint8_t unexpected_address[PLAYBACK_SPEAKER_LINK_ADDRESS_BYTES];
            memcpy(
                unexpected_address,
                event->data.connection.address,
                sizeof(unexpected_address)
            );
            (void)bk_bt_a2dp_source_disconnect(unexpected_address);
        }
        return;
    }

    tal_mutex_lock(state->pcm_mutex);
    if (event->data.connection.state == BK_A2DP_CONNECTION_STATE_DISCONNECTED)
    {
        state->encoder_ready = false;
    }
    tal_mutex_unlock(state->pcm_mutex);

    tal_mutex_lock(state->status_mutex);
    memcpy(
        state->status.remote_address,
        event->data.connection.address,
        sizeof(state->status.remote_address)
    );
    switch (event->data.connection.state)
    {
        case BK_A2DP_CONNECTION_STATE_DISCONNECTED:
            state->status.state = PLAYBACK_SPEAKER_DISCONNECTED;
            state->status.codec_ready = false;
            state->status.negotiated_sample_rate = 0U;
            state->status.negotiated_channels = 0U;
            state->status.negotiated_mtu = 0U;
            state->status.negotiated_bit_pool = 0U;
            memset(state->status.remote_address, 0, sizeof(state->status.remote_address));
            state->media_request = PLAYBACK_SPEAKER_MEDIA_NONE;
            if (state->status.desired_connected)
            {
                playback_speaker_schedule_retry(state, now);
            }
            break;

        case BK_A2DP_CONNECTION_STATE_CONNECTING:
            state->status.state = PLAYBACK_SPEAKER_CONNECTING;
            state->connect_request_ms = now;
            break;

        case BK_A2DP_CONNECTION_STATE_CONNECTED:
            state->status.state = PLAYBACK_SPEAKER_CONNECTED;
            state->status.reconnect_attempts = 0U;
            state->status.last_result = PLAYBACK_SPEAKER_LINK_OK;
            state->status.last_platform_error = 0;
            state->reconnect_delay_ms = PLAYBACK_SPEAKER_RECONNECT_INITIAL_MS;
            state->next_reconnect_ms = now;
            break;

        case BK_A2DP_CONNECTION_STATE_DISCONNECTING:
        default:
            break;
    }
    tal_mutex_unlock(state->status_mutex);
    playback_speaker_publish_status(state);
}

static void playback_speaker_process_audio(
    playback_speaker_link_state_t *state,
    const playback_speaker_event_t *event
)
{
    if (!playback_speaker_address_equal(event->data.audio.address, state->config.target_address))
    {
        return;
    }

    tal_mutex_lock(state->status_mutex);
    if (event->data.audio.state == BK_A2DP_AUDIO_STATE_STARTED)
    {
        state->status.state = PLAYBACK_SPEAKER_STREAMING;
    }
    else if (state->status.state != PLAYBACK_SPEAKER_DISCONNECTED)
    {
        state->status.state = PLAYBACK_SPEAKER_CONNECTED;
    }
    state->status.last_result = PLAYBACK_SPEAKER_LINK_OK;
    state->status.last_platform_error = 0;
    state->media_request = PLAYBACK_SPEAKER_MEDIA_NONE;
    tal_mutex_unlock(state->status_mutex);
    playback_speaker_publish_status(state);
}

static void playback_speaker_process_codec(
    playback_speaker_link_state_t *state,
    const playback_speaker_event_t *event
)
{
    playback_speaker_link_result_t result;

    if (!playback_speaker_address_equal(event->data.codec.address, state->config.target_address))
    {
        return;
    }

    result = playback_speaker_configure_encoder(
        state,
        &event->data.codec.mcc,
        event->data.codec.mtu
    );
    if (result != PLAYBACK_SPEAKER_LINK_OK)
    {
        tal_mutex_lock(state->status_mutex);
        state->status.codec_ready = false;
        state->status.desired_connected = false;
        state->status.desired_streaming = false;
        state->status.last_result = result;
        state->status.last_platform_error = PLAYBACK_SPEAKER_INVALID_PLATFORM_ERROR;
        tal_mutex_unlock(state->status_mutex);
        (void)bk_bt_a2dp_source_disconnect(state->config.target_address);
    }
    playback_speaker_publish_status(state);
}

static void playback_speaker_process_media_ack(
    playback_speaker_link_state_t *state,
    const playback_speaker_event_t *event
)
{
    const playback_speaker_media_request_t expected =
        (event->data.media_ack.command == BK_A2DP_MEDIA_CTRL_START)
            ? PLAYBACK_SPEAKER_MEDIA_START
            : PLAYBACK_SPEAKER_MEDIA_SUSPEND;

    if (state->media_request == expected)
    {
        state->media_request = PLAYBACK_SPEAKER_MEDIA_NONE;
    }
    if (event->data.media_ack.status != BK_A2DP_MEDIA_CTRL_ACK_SUCCESS)
    {
        playback_speaker_set_error(
            state,
            PLAYBACK_SPEAKER_LINK_PLATFORM_ERROR,
            (int32_t)event->data.media_ack.status
        );
    }
    else
    {
        playback_speaker_set_error(state, PLAYBACK_SPEAKER_LINK_OK, 0);
    }
    playback_speaker_publish_status(state);
}

static void playback_speaker_worker(void *argument)
{
    playback_speaker_link_state_t *state = argument;
    playback_speaker_event_t event;

    for (;;)
    {
        if (tal_queue_fetch(
                state->event_queue,
                &event,
                PLAYBACK_SPEAKER_WORKER_POLL_MS
            ) != OPRT_OK)
        {
            playback_speaker_reconcile(state);
            continue;
        }

        switch (event.kind)
        {
            case PLAYBACK_SPEAKER_EVENT_WAKE:
                playback_speaker_publish_status(state);
                break;

            case PLAYBACK_SPEAKER_EVENT_FORCE_RECONNECT:
            {
                playback_speaker_link_status_t status;
                state->next_reconnect_ms = (uint32_t)tal_system_get_millisecond();
                state->reconnect_delay_ms = PLAYBACK_SPEAKER_RECONNECT_INITIAL_MS;
                playback_speaker_status_snapshot(state, &status);
                if (status.state != PLAYBACK_SPEAKER_DISCONNECTED)
                {
                    (void)bk_bt_a2dp_source_disconnect(state->config.target_address);
                }
                break;
            }

            case PLAYBACK_SPEAKER_EVENT_PROFILE:
                playback_speaker_process_profile(state, &event);
                break;

            case PLAYBACK_SPEAKER_EVENT_CONNECTION:
                playback_speaker_process_connection(state, &event);
                break;

            case PLAYBACK_SPEAKER_EVENT_AUDIO:
                playback_speaker_process_audio(state, &event);
                break;

            case PLAYBACK_SPEAKER_EVENT_CODEC:
                playback_speaker_process_codec(state, &event);
                break;

            case PLAYBACK_SPEAKER_EVENT_MEDIA_ACK:
                playback_speaker_process_media_ack(state, &event);
                break;

            case PLAYBACK_SPEAKER_EVENT_STOP:
                tal_semaphore_post(state->worker_stopped);
                return;

            default:
                break;
        }
        playback_speaker_reconcile(state);
    }
}

static void playback_speaker_release_state(playback_speaker_link_state_t *state)
{
    if (state == NULL)
    {
        return;
    }
    if (state->event_queue != NULL)
    {
        tal_queue_free(state->event_queue);
    }
    if (state->worker_stopped != NULL)
    {
        tal_semaphore_release(state->worker_stopped);
    }
    if (state->pcm_mutex != NULL)
    {
        tal_mutex_release(state->pcm_mutex);
    }
    if (state->status_mutex != NULL)
    {
        tal_mutex_release(state->status_mutex);
    }
    tal_psram_free(state);
}

static playback_speaker_link_result_t playback_speaker_set_desired(
    playback_speaker_link_t *link,
    bool connected,
    bool streaming,
    playback_speaker_event_kind_t wake_kind
)
{
    playback_speaker_link_state_t *state;
    playback_speaker_event_t event;

    if ((link == NULL) || (link->state == NULL))
    {
        return PLAYBACK_SPEAKER_LINK_NOT_INITIALIZED;
    }
    state = link->state;

    tal_mutex_lock(state->status_mutex);
    state->status.desired_connected = connected;
    state->status.desired_streaming = streaming;
    state->status.last_result = PLAYBACK_SPEAKER_LINK_OK;
    state->status.last_platform_error = 0;
    tal_mutex_unlock(state->status_mutex);

    memset(&event, 0, sizeof(event));
    event.kind = wake_kind;
    return playback_speaker_enqueue(state, &event);
}

playback_speaker_link_result_t playback_speaker_link_init(
    playback_speaker_link_t *link,
    const playback_speaker_link_config_t *config
)
{
    playback_speaker_link_state_t *state;
    THREAD_CFG_T worker_config;
    playback_speaker_event_t wake_event;
    int32_t platform_result;

    if ((link == NULL) || (config == NULL) || (config->read_pcm == NULL) ||
        !playback_speaker_address_valid(config->target_address) ||
        (config->sample_rate != PLAYBACK_SPEAKER_LINK_SAMPLE_RATE) ||
        ((config->channels != 1U) && (config->channels != 2U)))
    {
        return PLAYBACK_SPEAKER_LINK_INVALID_ARGUMENT;
    }
    if ((link->state != NULL) || (playback_speaker_active_state != NULL))
    {
        return PLAYBACK_SPEAKER_LINK_ALREADY_INITIALIZED;
    }
    if ((playback_speaker_callback_mutex == NULL) &&
        (tal_mutex_create_init(&playback_speaker_callback_mutex) != OPRT_OK))
    {
        return PLAYBACK_SPEAKER_LINK_NO_MEMORY;
    }

    state = tal_psram_calloc(1U, sizeof(*state));
    if (state == NULL)
    {
        return PLAYBACK_SPEAKER_LINK_NO_MEMORY;
    }
    state->config = *config;
    state->reconnect_delay_ms = PLAYBACK_SPEAKER_RECONNECT_INITIAL_MS;
    state->status.initialized = true;
    state->status.state = PLAYBACK_SPEAKER_DISCONNECTED;
    state->status.input_sample_rate = config->sample_rate;
    state->status.input_channels = config->channels;
    state->status.last_result = PLAYBACK_SPEAKER_LINK_OK;
    memcpy(
        state->status.target_address,
        config->target_address,
        sizeof(state->status.target_address)
    );

    if ((tal_mutex_create_init(&state->status_mutex) != OPRT_OK) ||
        (tal_mutex_create_init(&state->pcm_mutex) != OPRT_OK) ||
        (tal_semaphore_create_init(&state->worker_stopped, 0U, 1U) != OPRT_OK) ||
        (tal_queue_create_init(
             &state->event_queue,
             sizeof(playback_speaker_event_t),
             PLAYBACK_SPEAKER_EVENT_QUEUE_DEPTH
         ) != OPRT_OK))
    {
        playback_speaker_release_state(state);
        return PLAYBACK_SPEAKER_LINK_NO_MEMORY;
    }

    memset(&worker_config, 0, sizeof(worker_config));
    worker_config.stackDepth = PLAYBACK_SPEAKER_WORKER_STACK_SIZE;
    worker_config.priority = THREAD_PRIO_2;
    worker_config.thrdname = "speaker_link";
    worker_config.psram_mode = 1U;
    if (tal_thread_create_and_start(
            &state->worker_thread,
            NULL,
            NULL,
            playback_speaker_worker,
            state,
            &worker_config
        ) != OPRT_OK)
    {
        playback_speaker_release_state(state);
        return PLAYBACK_SPEAKER_LINK_PLATFORM_ERROR;
    }

    tal_mutex_lock(playback_speaker_callback_mutex);
    playback_speaker_active_state = state;
    tal_mutex_unlock(playback_speaker_callback_mutex);
    link->state = state;

    platform_result = bk_bt_a2dp_register_callback(playback_speaker_a2dp_callback);
    if (platform_result == 0)
    {
        platform_result = bk_a2dp_source_register_data_callback(playback_speaker_data_callback);
    }
    if (platform_result == 0)
    {
        platform_result = bk_a2dp_source_set_pcm_data_format(
            config->sample_rate,
            PLAYBACK_SPEAKER_LINK_BITS_PER_SAMPLE,
            config->channels
        );
    }
    if (platform_result == 0)
    {
        platform_result = bk_a2dp_source_register_pcm_encode_callback(
            playback_speaker_encode_callback
        );
    }
    if (platform_result == 0)
    {
        platform_result = bk_a2dp_source_register_pcm_resample_callback(
            playback_speaker_resample_callback
        );
    }
    if (platform_result == 0)
    {
        platform_result = bk_bt_a2dp_source_init();
    }
    if (platform_result != 0)
    {
        playback_speaker_set_error(
            state,
            PLAYBACK_SPEAKER_LINK_PLATFORM_ERROR,
            platform_result
        );
        playback_speaker_link_close(link);
        return PLAYBACK_SPEAKER_LINK_PLATFORM_ERROR;
    }
    state->source_init_requested = true;

    memset(&wake_event, 0, sizeof(wake_event));
    wake_event.kind = PLAYBACK_SPEAKER_EVENT_WAKE;
    (void)playback_speaker_enqueue(state, &wake_event);
    return PLAYBACK_SPEAKER_LINK_OK;
}

playback_speaker_link_result_t playback_speaker_link_connect(playback_speaker_link_t *link)
{
    return playback_speaker_set_desired(
        link,
        true,
        false,
        PLAYBACK_SPEAKER_EVENT_WAKE
    );
}

playback_speaker_link_result_t playback_speaker_link_reconnect(playback_speaker_link_t *link)
{
    playback_speaker_link_state_t *state;
    playback_speaker_event_t event;

    if ((link == NULL) || (link->state == NULL))
    {
        return PLAYBACK_SPEAKER_LINK_NOT_INITIALIZED;
    }
    state = link->state;

    tal_mutex_lock(state->status_mutex);
    state->status.desired_connected = true;
    state->status.last_result = PLAYBACK_SPEAKER_LINK_OK;
    state->status.last_platform_error = 0;
    tal_mutex_unlock(state->status_mutex);

    memset(&event, 0, sizeof(event));
    event.kind = PLAYBACK_SPEAKER_EVENT_FORCE_RECONNECT;
    return playback_speaker_enqueue(state, &event);
}

playback_speaker_link_result_t playback_speaker_link_disconnect(playback_speaker_link_t *link)
{
    return playback_speaker_set_desired(
        link,
        false,
        false,
        PLAYBACK_SPEAKER_EVENT_WAKE
    );
}

playback_speaker_link_result_t playback_speaker_link_start(playback_speaker_link_t *link)
{
    return playback_speaker_set_desired(
        link,
        true,
        true,
        PLAYBACK_SPEAKER_EVENT_WAKE
    );
}

playback_speaker_link_result_t playback_speaker_link_stop(playback_speaker_link_t *link)
{
    playback_speaker_link_state_t *state;
    playback_speaker_event_t event;

    if ((link == NULL) || (link->state == NULL))
    {
        return PLAYBACK_SPEAKER_LINK_NOT_INITIALIZED;
    }
    state = link->state;

    tal_mutex_lock(state->status_mutex);
    state->status.desired_streaming = false;
    state->status.last_result = PLAYBACK_SPEAKER_LINK_OK;
    state->status.last_platform_error = 0;
    tal_mutex_unlock(state->status_mutex);

    memset(&event, 0, sizeof(event));
    event.kind = PLAYBACK_SPEAKER_EVENT_WAKE;
    return playback_speaker_enqueue(state, &event);
}

playback_speaker_link_result_t playback_speaker_link_get_status(
    playback_speaker_link_t *link,
    playback_speaker_link_status_t *status
)
{
    if ((link == NULL) || (status == NULL))
    {
        return PLAYBACK_SPEAKER_LINK_INVALID_ARGUMENT;
    }
    if (link->state == NULL)
    {
        return PLAYBACK_SPEAKER_LINK_NOT_INITIALIZED;
    }
    playback_speaker_status_snapshot(link->state, status);
    return PLAYBACK_SPEAKER_LINK_OK;
}

void playback_speaker_link_close(playback_speaker_link_t *link)
{
    playback_speaker_link_state_t *state;
    playback_speaker_link_status_t status;
    playback_speaker_event_t stop_event;

    if ((link == NULL) || (link->state == NULL))
    {
        return;
    }
    state = link->state;
    state->closing = true;

    tal_mutex_lock(playback_speaker_callback_mutex);
    if (playback_speaker_active_state == state)
    {
        playback_speaker_active_state = NULL;
    }
    tal_mutex_unlock(playback_speaker_callback_mutex);

    playback_speaker_status_snapshot(state, &status);
    if (status.state == PLAYBACK_SPEAKER_STREAMING)
    {
        (void)bk_a2dp_media_ctrl(BK_A2DP_MEDIA_CTRL_SUSPEND);
    }
    if ((status.state == PLAYBACK_SPEAKER_CONNECTED) ||
        (status.state == PLAYBACK_SPEAKER_STREAMING) ||
        (status.state == PLAYBACK_SPEAKER_CONNECTING))
    {
        (void)bk_bt_a2dp_source_disconnect(state->config.target_address);
    }
    if (state->source_init_requested)
    {
        (void)bk_bt_a2dp_source_deinit();
        state->source_init_requested = false;
    }

    memset(&stop_event, 0, sizeof(stop_event));
    stop_event.kind = PLAYBACK_SPEAKER_EVENT_STOP;
    (void)tal_queue_post(state->event_queue, &stop_event, QUEUE_WAIT_FOREVER);
    (void)tal_semaphore_wait(state->worker_stopped, 3000U);
    if (state->worker_thread != NULL)
    {
        (void)tal_thread_delete(state->worker_thread);
    }

    link->state = NULL;
    playback_speaker_release_state(state);
}

const char *playback_speaker_link_result_name(playback_speaker_link_result_t result)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "ALREADY_INITIALIZED",
        "NOT_INITIALIZED",
        "NOT_READY",
        "NOT_CONNECTED",
        "UNSUPPORTED_FORMAT",
        "QUEUE_FULL",
        "NO_MEMORY",
        "PLATFORM_ERROR",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0])))
               ? names[result]
               : "UNKNOWN_SPEAKER_LINK_RESULT";
}
