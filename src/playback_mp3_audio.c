/**
 * @file playback_mp3_audio.c
 * @brief Indexed MP3 Range reader, decoder, recovery policy and bounded PCM ring.
 */

#include "playback_mp3_audio.h"

#include <limits.h>
#include <string.h>

#include <modules/mp3dec.h>

#include "tal_api.h"

typedef struct
{
    playback_audio_descriptor_t descriptor;
    playback_audio_index_record_t *records;
    size_t record_count;
    uint64_t expected_pcm_frames;
    playback_http_range_reader_t reader;
    HMP3Decoder decoder;
    uint8_t *compressed_frame;
    size_t compressed_capacity;
    int16_t *decoded_pcm;
    int16_t *pcm_ring;
    MUTEX_HANDLE pcm_mutex;
    size_t pcm_ring_capacity_samples;
    size_t pcm_ring_read;
    size_t pcm_ring_write;
    size_t pcm_ring_count;
    uint32_t low_water_frames;
    uint32_t high_water_frames;
    size_t next_record;
    uint64_t discard_before_frame;
    uint64_t consumed_pcm_frames;
    bool fetch_paused;
    bool output_paused;
    bool source_exhausted;
    bool fatal;
    uint8_t consecutive_failures;
    uint32_t failure_started_ms;
    uint32_t recovered_frame_count;
    playback_mp3_result_t last_failure;
} playback_mp3_audio_state_t;

static void *playback_mp3_decoder_alloc(size_t size)
{
    return tal_psram_malloc(size);
}

static void playback_mp3_decoder_free(void *buffer)
{
    tal_psram_free(buffer);
}

static void *playback_mp3_decoder_memset(void *buffer, unsigned char value, size_t length)
{
    return memset(buffer, (int)value, length);
}

static uint32_t playback_mp3_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    size_t index;

    for (index = 0U; index < length; ++index)
    {
        unsigned int bit;
        crc ^= data[index];
        for (bit = 0U; bit < 8U; ++bit)
        {
            const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320UL & mask);
        }
    }

    return crc ^ 0xFFFFFFFFUL;
}

static playback_mp3_result_t playback_mp3_http_result(playback_http_result_t result)
{
    switch (result)
    {
        case PLAYBACK_HTTP_OK:
            return PLAYBACK_MP3_OK;
        case PLAYBACK_HTTP_RESOURCE_MISMATCH:
        case PLAYBACK_HTTP_RANGE_INVALID:
            return PLAYBACK_MP3_RESOURCE_MISMATCH;
        case PLAYBACK_HTTP_INVALID_ARGUMENT:
        case PLAYBACK_HTTP_INVALID_URL:
            return PLAYBACK_MP3_INVALID_ARGUMENT;
        case PLAYBACK_HTTP_CANCELLED:
            return PLAYBACK_MP3_FETCH_PAUSED;
        case PLAYBACK_HTTP_NETWORK_ERROR:
        case PLAYBACK_HTTP_STATUS_ERROR:
        case PLAYBACK_HTTP_RESPONSE_TOO_LARGE:
        default:
            return PLAYBACK_MP3_HTTP_FAILED;
    }
}

static uint64_t playback_mp3_record_end_frame(
    const playback_mp3_audio_state_t *state,
    size_t record_index
)
{
    if ((record_index + 1U) < state->record_count)
    {
        return state->records[record_index + 1U].pcm_sample_position;
    }
    return state->expected_pcm_frames;
}

static uint32_t playback_mp3_available_frames(const playback_mp3_audio_state_t *state)
{
    uint32_t frames;

    tal_mutex_lock(state->pcm_mutex);
    frames = (uint32_t)(state->pcm_ring_count / state->descriptor.channels);
    tal_mutex_unlock(state->pcm_mutex);
    return frames;
}

static size_t playback_mp3_free_samples(const playback_mp3_audio_state_t *state)
{
    return state->pcm_ring_capacity_samples - state->pcm_ring_count;
}

static bool playback_mp3_ring_write_samples(
    playback_mp3_audio_state_t *state,
    const int16_t *samples,
    size_t sample_count
)
{
    size_t first;
    size_t second;

    if (sample_count > playback_mp3_free_samples(state))
    {
        return false;
    }
    if (sample_count == 0U)
    {
        return true;
    }

    first = state->pcm_ring_capacity_samples - state->pcm_ring_write;
    if (first > sample_count)
    {
        first = sample_count;
    }
    second = sample_count - first;

    if (samples == NULL)
    {
        memset(state->pcm_ring + state->pcm_ring_write, 0, first * sizeof(int16_t));
        if (second > 0U)
        {
            memset(state->pcm_ring, 0, second * sizeof(int16_t));
        }
    }
    else
    {
        memcpy(state->pcm_ring + state->pcm_ring_write, samples, first * sizeof(int16_t));
        if (second > 0U)
        {
            memcpy(state->pcm_ring, samples + first, second * sizeof(int16_t));
        }
    }

    state->pcm_ring_write = (state->pcm_ring_write + sample_count) % state->pcm_ring_capacity_samples;
    state->pcm_ring_count += sample_count;
    return true;
}

static size_t playback_mp3_ring_read_samples(
    playback_mp3_audio_state_t *state,
    int16_t *destination,
    size_t sample_count
)
{
    size_t first;
    size_t second;

    if (sample_count > state->pcm_ring_count)
    {
        sample_count = state->pcm_ring_count;
    }
    if (sample_count == 0U)
    {
        return 0U;
    }

    first = state->pcm_ring_capacity_samples - state->pcm_ring_read;
    if (first > sample_count)
    {
        first = sample_count;
    }
    second = sample_count - first;

    memcpy(destination, state->pcm_ring + state->pcm_ring_read, first * sizeof(int16_t));
    if (second > 0U)
    {
        memcpy(destination + first, state->pcm_ring, second * sizeof(int16_t));
    }

    state->pcm_ring_read = (state->pcm_ring_read + sample_count) % state->pcm_ring_capacity_samples;
    state->pcm_ring_count -= sample_count;
    return sample_count;
}

static void playback_mp3_ring_clear(playback_mp3_audio_state_t *state)
{
    state->pcm_ring_read = 0U;
    state->pcm_ring_write = 0U;
    state->pcm_ring_count = 0U;
}

static bool playback_mp3_decoder_reset(playback_mp3_audio_state_t *state)
{
    if (state->decoder != NULL)
    {
        MP3FreeDecoder(state->decoder);
        state->decoder = NULL;
    }

    if (MP3SetBuffMethodAlwaysFourAlignedAccess(
            playback_mp3_decoder_alloc,
            playback_mp3_decoder_free,
            playback_mp3_decoder_memset
        ) != 0)
    {
        return false;
    }

    state->decoder = MP3InitDecoder();
    return state->decoder != NULL;
}

static void playback_mp3_release_state(playback_mp3_audio_state_t *state)
{
    if (state == NULL)
    {
        return;
    }
    if (state->decoder != NULL)
    {
        MP3FreeDecoder(state->decoder);
    }
    if (state->pcm_mutex != NULL)
    {
        tal_mutex_release(state->pcm_mutex);
    }
    if (state->pcm_ring != NULL)
    {
        tal_psram_free(state->pcm_ring);
    }
    if (state->decoded_pcm != NULL)
    {
        tal_psram_free(state->decoded_pcm);
    }
    if (state->compressed_frame != NULL)
    {
        tal_psram_free(state->compressed_frame);
    }
    if (state->records != NULL)
    {
        tal_psram_free(state->records);
    }
    tal_free(state);
}

static playback_mp3_result_t playback_mp3_validate_index(
    const playback_audio_descriptor_t *descriptor,
    const playback_audio_index_t *index,
    uint64_t expected_pcm_frames,
    size_t *maximum_frame_bytes,
    uint32_t *maximum_frame_span
)
{
    size_t record_index;
    uint64_t previous_end = 0ULL;
    uint32_t max_span = 0U;
    size_t max_bytes = 0U;

    if ((index == NULL) || (index->records == NULL) || (index->count == 0U) ||
        (index->count > PLAYBACK_AUDIO_INDEX_MAX_RECORDS))
    {
        return PLAYBACK_MP3_INDEX_INVALID;
    }

    for (record_index = 0U; record_index < index->count; ++record_index)
    {
        const playback_audio_index_record_t *record = &index->records[record_index];
        const uint64_t byte_end = (uint64_t)record->byte_offset + record->byte_length;
        const uint64_t frame_end = ((record_index + 1U) < index->count)
                                       ? index->records[record_index + 1U].pcm_sample_position
                                       : expected_pcm_frames;
        uint64_t span;

        if ((record_index == 0U) && (record->pcm_sample_position != 0U))
        {
            return PLAYBACK_MP3_INDEX_INVALID;
        }
        if ((record->byte_length == 0U) || (record->byte_length > PLAYBACK_MP3_MAX_COMPRESSED_FRAME_BYTES) ||
            (byte_end > descriptor->asset.byte_length) ||
            ((record_index > 0U) &&
             ((record->pcm_sample_position <= index->records[record_index - 1U].pcm_sample_position) ||
              ((uint64_t)record->byte_offset < previous_end))) ||
            (frame_end <= record->pcm_sample_position) || (frame_end > expected_pcm_frames))
        {
            return (record->byte_length > PLAYBACK_MP3_MAX_COMPRESSED_FRAME_BYTES)
                       ? PLAYBACK_MP3_FRAME_TOO_LARGE
                       : PLAYBACK_MP3_INDEX_INVALID;
        }

        span = frame_end - record->pcm_sample_position;
        if (span > 1152ULL)
        {
            return PLAYBACK_MP3_INDEX_INVALID;
        }
        if ((uint32_t)span > max_span)
        {
            max_span = (uint32_t)span;
        }
        if (record->byte_length > max_bytes)
        {
            max_bytes = record->byte_length;
        }
        previous_end = byte_end;
    }

    *maximum_frame_bytes = max_bytes;
    *maximum_frame_span = max_span;
    return PLAYBACK_MP3_OK;
}

static playback_mp3_result_t playback_mp3_append_timeline(
    playback_mp3_audio_state_t *state,
    uint64_t record_start,
    uint64_t record_end,
    const int16_t *decoded,
    uint32_t decoded_frames
)
{
    const uint64_t append_start = (state->discard_before_frame > record_start)
                                      ? state->discard_before_frame
                                      : record_start;
    uint64_t append_frames_u64;
    uint32_t source_offset_frames;
    uint32_t append_frames;
    uint32_t decoded_available_frames;
    uint32_t decoded_copy_frames;
    size_t decoded_copy_samples;
    size_t silence_samples;

    if (append_start >= record_end)
    {
        return PLAYBACK_MP3_OK;
    }

    append_frames_u64 = record_end - append_start;
    if (append_frames_u64 > UINT32_MAX)
    {
        return PLAYBACK_MP3_INDEX_INVALID;
    }
    append_frames = (uint32_t)append_frames_u64;
    source_offset_frames = (uint32_t)(append_start - record_start);
    decoded_available_frames = (decoded_frames > source_offset_frames)
                                   ? decoded_frames - source_offset_frames
                                   : 0U;
    decoded_copy_frames = (decoded_available_frames < append_frames)
                              ? decoded_available_frames
                              : append_frames;

    decoded_copy_samples = (size_t)decoded_copy_frames * state->descriptor.channels;
    silence_samples = (size_t)(append_frames - decoded_copy_frames) * state->descriptor.channels;

    tal_mutex_lock(state->pcm_mutex);
    if ((decoded_copy_samples + silence_samples) > playback_mp3_free_samples(state))
    {
        tal_mutex_unlock(state->pcm_mutex);
        return PLAYBACK_MP3_BUFFER_FULL;
    }

    if ((decoded_copy_samples > 0U) &&
        !playback_mp3_ring_write_samples(
            state,
            decoded + ((size_t)source_offset_frames * state->descriptor.channels),
            decoded_copy_samples
        ))
    {
        tal_mutex_unlock(state->pcm_mutex);
        return PLAYBACK_MP3_BUFFER_FULL;
    }
    if ((silence_samples > 0U) && !playback_mp3_ring_write_samples(state, NULL, silence_samples))
    {
        tal_mutex_unlock(state->pcm_mutex);
        return PLAYBACK_MP3_BUFFER_FULL;
    }
    tal_mutex_unlock(state->pcm_mutex);

    return PLAYBACK_MP3_OK;
}

static playback_mp3_result_t playback_mp3_append_silence(
    playback_mp3_audio_state_t *state,
    uint64_t record_start,
    uint64_t record_end
)
{
    const uint64_t append_start = (state->discard_before_frame > record_start)
                                      ? state->discard_before_frame
                                      : record_start;
    uint64_t frame_count;
    size_t sample_count;

    if (append_start >= record_end)
    {
        return PLAYBACK_MP3_OK;
    }
    frame_count = record_end - append_start;
    if (frame_count > (SIZE_MAX / state->descriptor.channels))
    {
        return PLAYBACK_MP3_INDEX_INVALID;
    }
    sample_count = (size_t)frame_count * state->descriptor.channels;
    tal_mutex_lock(state->pcm_mutex);
    if (!playback_mp3_ring_write_samples(state, NULL, sample_count))
    {
        tal_mutex_unlock(state->pcm_mutex);
        return PLAYBACK_MP3_BUFFER_FULL;
    }
    tal_mutex_unlock(state->pcm_mutex);
    return PLAYBACK_MP3_OK;
}

static playback_mp3_result_t playback_mp3_decode_current_record(
    playback_mp3_audio_state_t *state
)
{
    const playback_audio_index_record_t *record = &state->records[state->next_record];
    const uint64_t record_start = record->pcm_sample_position;
    const uint64_t record_end = playback_mp3_record_end_frame(state, state->next_record);
    playback_http_result_t http_result;
    playback_mp3_result_t append_result;
    MP3FrameInfo frame_info;
    unsigned char *cursor;
    int bytes_left;
    int decode_result;
    uint32_t decoded_frames;

    http_result = playback_http_range_read_at(
        &state->reader,
        record->byte_offset,
        record->byte_length,
        state->compressed_frame,
        state->compressed_capacity
    );
    if (http_result != PLAYBACK_HTTP_OK)
    {
        return playback_mp3_http_result(http_result);
    }
    if (playback_mp3_crc32(state->compressed_frame, record->byte_length) != record->crc32)
    {
        return PLAYBACK_MP3_CRC_MISMATCH;
    }
    if ((record->byte_length > (uint32_t)INT_MAX) ||
        (MP3FindSyncWord(state->compressed_frame, (int)record->byte_length) != 0))
    {
        return PLAYBACK_MP3_DECODE_FAILED;
    }

    memset(&frame_info, 0, sizeof(frame_info));
    if (MP3GetNextFrameInfo(state->decoder, &frame_info, state->compressed_frame) != ERR_MP3_NONE)
    {
        return PLAYBACK_MP3_DECODE_FAILED;
    }
    if ((frame_info.nChans != state->descriptor.channels) ||
        (frame_info.samprate != (int)state->descriptor.sample_rate) ||
        (frame_info.bitrate != ((int)state->descriptor.bitrate_kbps * 1000)) ||
        (frame_info.bitsPerSample != 16) || (frame_info.layer != 3) ||
        (frame_info.version != MPEG1))
    {
        return PLAYBACK_MP3_UNSUPPORTED_FORMAT;
    }

    cursor = state->compressed_frame;
    bytes_left = (int)record->byte_length;
    decode_result = MP3Decode(state->decoder, &cursor, &bytes_left, state->decoded_pcm, 0);
    if (decode_result != ERR_MP3_NONE)
    {
        return PLAYBACK_MP3_DECODE_FAILED;
    }
    if (bytes_left != 0)
    {
        return PLAYBACK_MP3_INDEX_INVALID;
    }

    memset(&frame_info, 0, sizeof(frame_info));
    MP3GetLastFrameInfo(state->decoder, &frame_info);
    if ((frame_info.nChans != state->descriptor.channels) ||
        (frame_info.samprate != (int)state->descriptor.sample_rate) ||
        (frame_info.bitrate != ((int)state->descriptor.bitrate_kbps * 1000)) ||
        (frame_info.bitsPerSample != 16) || (frame_info.layer != 3) ||
        (frame_info.version != MPEG1) || (frame_info.outputSamps <= 0) ||
        (frame_info.outputSamps > (int)PLAYBACK_MP3_MAX_DECODED_SAMPLES) ||
        ((frame_info.outputSamps % state->descriptor.channels) != 0))
    {
        return PLAYBACK_MP3_UNSUPPORTED_FORMAT;
    }

    decoded_frames = (uint32_t)frame_info.outputSamps / state->descriptor.channels;
    if ((((state->next_record + 1U) < state->record_count) &&
         ((record_end - record_start) != decoded_frames)) ||
        (((state->next_record + 1U) == state->record_count) &&
         ((record_end - record_start) > decoded_frames)))
    {
        return PLAYBACK_MP3_INDEX_INVALID;
    }
    append_result = playback_mp3_append_timeline(
        state,
        record_start,
        record_end,
        state->decoded_pcm,
        decoded_frames
    );
    if (append_result != PLAYBACK_MP3_OK)
    {
        return append_result;
    }

    state->next_record++;
    state->consecutive_failures = 0U;
    state->failure_started_ms = 0U;
    state->last_failure = PLAYBACK_MP3_OK;
    return PLAYBACK_MP3_OK;
}

static playback_mp3_result_t playback_mp3_recover_record(
    playback_mp3_audio_state_t *state,
    playback_mp3_result_t failure
)
{
    const uint32_t now = (uint32_t)tal_system_get_millisecond();
    const uint64_t record_start = state->records[state->next_record].pcm_sample_position;
    const uint64_t record_end = playback_mp3_record_end_frame(state, state->next_record);
    playback_mp3_result_t append_result;

    if ((failure == PLAYBACK_MP3_RESOURCE_MISMATCH) ||
        (failure == PLAYBACK_MP3_INDEX_INVALID) ||
        (failure == PLAYBACK_MP3_FRAME_TOO_LARGE) ||
        (failure == PLAYBACK_MP3_UNSUPPORTED_FORMAT) ||
        (failure == PLAYBACK_MP3_NO_MEMORY))
    {
        tal_mutex_lock(state->pcm_mutex);
        state->fatal = true;
        state->last_failure = failure;
        tal_mutex_unlock(state->pcm_mutex);
        return failure;
    }

    if (state->consecutive_failures == 0U)
    {
        state->failure_started_ms = now;
    }
    state->consecutive_failures++;
    state->last_failure = failure;

    if ((state->consecutive_failures >= PLAYBACK_MP3_MAX_CONSECUTIVE_FAILURES) ||
        ((uint32_t)(now - state->failure_started_ms) > PLAYBACK_MP3_RECOVERY_DEADLINE_MS))
    {
        tal_mutex_lock(state->pcm_mutex);
        state->fatal = true;
        state->last_failure = PLAYBACK_MP3_RECOVERY_FAILED;
        tal_mutex_unlock(state->pcm_mutex);
        return PLAYBACK_MP3_RECOVERY_FAILED;
    }

    append_result = playback_mp3_append_silence(state, record_start, record_end);
    if (append_result != PLAYBACK_MP3_OK)
    {
        return append_result;
    }
    state->next_record++;
    state->recovered_frame_count++;

    if (!playback_mp3_decoder_reset(state))
    {
        tal_mutex_lock(state->pcm_mutex);
        state->fatal = true;
        state->last_failure = PLAYBACK_MP3_NO_MEMORY;
        tal_mutex_unlock(state->pcm_mutex);
        return PLAYBACK_MP3_NO_MEMORY;
    }

    return PLAYBACK_MP3_OK;
}

static size_t playback_mp3_find_record_index(
    const playback_mp3_audio_state_t *state,
    uint64_t target_pcm_frame
)
{
    size_t low = 0U;
    size_t high = state->record_count;

    while (low < high)
    {
        const size_t middle = low + ((high - low) / 2U);
        if ((uint64_t)state->records[middle].pcm_sample_position <= target_pcm_frame)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }

    return (low == 0U) ? 0U : low - 1U;
}

playback_mp3_result_t playback_mp3_audio_prepare(
    playback_mp3_audio_t *audio,
    const playback_audio_descriptor_t *descriptor,
    const playback_audio_index_t *index,
    uint32_t duration_ms,
    const playback_http_config_t *http_config
)
{
    playback_mp3_audio_state_t *state;
    playback_http_metadata_t metadata;
    playback_http_result_t http_result;
    playback_mp3_result_t result;
    size_t maximum_frame_bytes;
    uint32_t maximum_frame_span;
    uint64_t expected_pcm_frames;
    uint64_t ring_capacity_frames;
    uint64_t ring_capacity_samples;

    if ((audio == NULL) || (descriptor == NULL) || (index == NULL) || (duration_ms == 0U) ||
        (descriptor->asset.byte_length == 0U) ||
        ((descriptor->channels != 1U) && (descriptor->channels != 2U)) ||
        (descriptor->sample_rate != PLAYBACK_MP3_SAMPLE_RATE) ||
        (descriptor->bitrate_kbps != PLAYBACK_MP3_BITRATE_KBPS) ||
        (descriptor->index_version != PLAYBACK_AUDIO_INDEX_VERSION))
    {
        return PLAYBACK_MP3_INVALID_ARGUMENT;
    }
    if (audio->state != NULL)
    {
        return PLAYBACK_MP3_ALREADY_OPEN;
    }

    expected_pcm_frames = ((uint64_t)duration_ms * descriptor->sample_rate) / 1000ULL;
    if ((expected_pcm_frames == 0ULL) || (expected_pcm_frames > UINT32_MAX))
    {
        return PLAYBACK_MP3_INVALID_ARGUMENT;
    }

    result = playback_mp3_validate_index(
        descriptor,
        index,
        expected_pcm_frames,
        &maximum_frame_bytes,
        &maximum_frame_span
    );
    if (result != PLAYBACK_MP3_OK)
    {
        return result;
    }

    state = tal_malloc(sizeof(*state));
    if (state == NULL)
    {
        return PLAYBACK_MP3_NO_MEMORY;
    }
    memset(state, 0, sizeof(*state));
    state->descriptor = *descriptor;
    state->record_count = index->count;
    state->expected_pcm_frames = expected_pcm_frames;
    state->compressed_capacity = maximum_frame_bytes;
    state->low_water_frames = (descriptor->sample_rate * PLAYBACK_MP3_PCM_LOW_WATER_MS) / 1000U;
    state->high_water_frames = (descriptor->sample_rate * PLAYBACK_MP3_PCM_HIGH_WATER_MS) / 1000U;
    state->last_failure = PLAYBACK_MP3_OK;
    if (tal_mutex_create_init(&state->pcm_mutex) != OPRT_OK)
    {
        playback_mp3_release_state(state);
        return PLAYBACK_MP3_NO_MEMORY;
    }

    ring_capacity_frames = (uint64_t)state->high_water_frames + maximum_frame_span;
    ring_capacity_samples = ring_capacity_frames * descriptor->channels;
    if ((ring_capacity_samples == 0ULL) || (ring_capacity_samples > (SIZE_MAX / sizeof(int16_t))))
    {
        playback_mp3_release_state(state);
        return PLAYBACK_MP3_NO_MEMORY;
    }
    state->pcm_ring_capacity_samples = (size_t)ring_capacity_samples;

    state->records = tal_psram_malloc(index->count * sizeof(*state->records));
    state->compressed_frame = tal_psram_malloc(maximum_frame_bytes);
    state->decoded_pcm = tal_psram_malloc(PLAYBACK_MP3_MAX_DECODED_SAMPLES * sizeof(int16_t));
    state->pcm_ring = tal_psram_malloc(state->pcm_ring_capacity_samples * sizeof(int16_t));
    if ((state->records == NULL) || (state->compressed_frame == NULL) ||
        (state->decoded_pcm == NULL) || (state->pcm_ring == NULL))
    {
        playback_mp3_release_state(state);
        return PLAYBACK_MP3_NO_MEMORY;
    }
    memcpy(state->records, index->records, index->count * sizeof(*state->records));

    result = playback_mp3_decoder_reset(state) ? PLAYBACK_MP3_OK : PLAYBACK_MP3_NO_MEMORY;
    if (result != PLAYBACK_MP3_OK)
    {
        playback_mp3_release_state(state);
        return result;
    }

    http_result = playback_http_range_reader_init(&state->reader, &descriptor->asset, http_config);
    if (http_result != PLAYBACK_HTTP_OK)
    {
        result = playback_mp3_http_result(http_result);
        playback_mp3_release_state(state);
        return result;
    }
    http_result = playback_http_range_probe(&state->reader, &metadata);
    if (http_result != PLAYBACK_HTTP_OK)
    {
        result = playback_mp3_http_result(http_result);
        playback_mp3_release_state(state);
        return result;
    }

    audio->state = state;
    return PLAYBACK_MP3_OK;
}

playback_mp3_result_t playback_mp3_audio_fill(playback_mp3_audio_t *audio)
{
    playback_mp3_audio_state_t *state;
    bool progressed = false;

    if ((audio == NULL) || (audio->state == NULL))
    {
        return PLAYBACK_MP3_NOT_OPEN;
    }
    state = audio->state;
    tal_mutex_lock(state->pcm_mutex);
    if (state->fatal)
    {
        const playback_mp3_result_t failure = (state->last_failure == PLAYBACK_MP3_OK)
                                                  ? PLAYBACK_MP3_RECOVERY_FAILED
                                                  : state->last_failure;
        tal_mutex_unlock(state->pcm_mutex);
        return failure;
    }
    if (state->fetch_paused)
    {
        tal_mutex_unlock(state->pcm_mutex);
        return PLAYBACK_MP3_FETCH_PAUSED;
    }
    tal_mutex_unlock(state->pcm_mutex);
    if (playback_mp3_available_frames(state) >= state->high_water_frames)
    {
        return PLAYBACK_MP3_OK;
    }

    while ((playback_mp3_available_frames(state) < state->high_water_frames) &&
           (state->next_record < state->record_count))
    {
        playback_mp3_result_t result = playback_mp3_decode_current_record(state);
        if (result != PLAYBACK_MP3_OK)
        {
            result = playback_mp3_recover_record(state, result);
            if (result != PLAYBACK_MP3_OK)
            {
                return result;
            }
        }
        progressed = true;
    }

    if (state->next_record >= state->record_count)
    {
        tal_mutex_lock(state->pcm_mutex);
        state->source_exhausted = true;
        tal_mutex_unlock(state->pcm_mutex);
    }

    if (progressed || (playback_mp3_available_frames(state) > 0U))
    {
        return PLAYBACK_MP3_OK;
    }
    tal_mutex_lock(state->pcm_mutex);
    progressed = state->source_exhausted;
    tal_mutex_unlock(state->pcm_mutex);
    return progressed ? PLAYBACK_MP3_END_OF_STREAM : PLAYBACK_MP3_BUFFER_EMPTY;
}

playback_mp3_result_t playback_mp3_audio_consume(
    playback_mp3_audio_t *audio,
    int16_t *destination,
    size_t destination_frame_capacity,
    size_t *consumed_frames
)
{
    playback_mp3_audio_state_t *state;
    size_t available_frames;
    size_t selected_frames;
    size_t selected_samples;
    size_t read_samples;

    if (consumed_frames != NULL)
    {
        *consumed_frames = 0U;
    }
    if ((audio == NULL) || (audio->state == NULL))
    {
        return PLAYBACK_MP3_NOT_OPEN;
    }
    if ((destination == NULL) || (destination_frame_capacity == 0U))
    {
        return PLAYBACK_MP3_INVALID_ARGUMENT;
    }

    state = audio->state;
    tal_mutex_lock(state->pcm_mutex);
    if (state->output_paused)
    {
        tal_mutex_unlock(state->pcm_mutex);
        return PLAYBACK_MP3_OUTPUT_PAUSED;
    }
    if (state->fatal && (state->pcm_ring_count == 0U))
    {
        const playback_mp3_result_t failure = (state->last_failure == PLAYBACK_MP3_OK)
                                                  ? PLAYBACK_MP3_RECOVERY_FAILED
                                                  : state->last_failure;
        tal_mutex_unlock(state->pcm_mutex);
        return failure;
    }

    available_frames = state->pcm_ring_count / state->descriptor.channels;
    if (available_frames == 0U)
    {
        const playback_mp3_result_t empty_result = state->source_exhausted
                                                       ? PLAYBACK_MP3_END_OF_STREAM
                                                       : PLAYBACK_MP3_BUFFER_EMPTY;
        tal_mutex_unlock(state->pcm_mutex);
        return empty_result;
    }

    selected_frames = (available_frames < destination_frame_capacity)
                          ? available_frames
                          : destination_frame_capacity;
    selected_samples = selected_frames * state->descriptor.channels;
    read_samples = playback_mp3_ring_read_samples(state, destination, selected_samples);
    selected_frames = read_samples / state->descriptor.channels;
    state->consumed_pcm_frames += selected_frames;
    tal_mutex_unlock(state->pcm_mutex);
    if (consumed_frames != NULL)
    {
        *consumed_frames = selected_frames;
    }
    return PLAYBACK_MP3_OK;
}

playback_mp3_result_t playback_mp3_audio_seek(
    playback_mp3_audio_t *audio,
    uint64_t target_pcm_frame
)
{
    playback_mp3_audio_state_t *state;
    size_t record_index;

    if ((audio == NULL) || (audio->state == NULL))
    {
        return PLAYBACK_MP3_NOT_OPEN;
    }
    state = audio->state;
    if (target_pcm_frame > state->expected_pcm_frames)
    {
        return PLAYBACK_MP3_INVALID_ARGUMENT;
    }

    playback_http_range_reset_cancel(&state->reader);
    tal_mutex_lock(state->pcm_mutex);
    playback_mp3_ring_clear(state);
    state->consumed_pcm_frames = target_pcm_frame;
    state->discard_before_frame = target_pcm_frame;
    state->source_exhausted = false;
    state->fatal = false;
    state->consecutive_failures = 0U;
    state->failure_started_ms = 0U;
    state->last_failure = PLAYBACK_MP3_OK;
    tal_mutex_unlock(state->pcm_mutex);

    if (target_pcm_frame == state->expected_pcm_frames)
    {
        state->next_record = state->record_count;
        tal_mutex_lock(state->pcm_mutex);
        state->source_exhausted = true;
        tal_mutex_unlock(state->pcm_mutex);
        return playback_mp3_decoder_reset(state) ? PLAYBACK_MP3_OK : PLAYBACK_MP3_NO_MEMORY;
    }

    record_index = playback_mp3_find_record_index(state, target_pcm_frame);
    state->next_record = (record_index > PLAYBACK_MP3_SEEK_WARMUP_RECORDS)
                             ? record_index - PLAYBACK_MP3_SEEK_WARMUP_RECORDS
                             : 0U;
    if (!playback_mp3_decoder_reset(state))
    {
        tal_mutex_lock(state->pcm_mutex);
        state->fatal = true;
        state->last_failure = PLAYBACK_MP3_NO_MEMORY;
        tal_mutex_unlock(state->pcm_mutex);
        return PLAYBACK_MP3_NO_MEMORY;
    }
    return PLAYBACK_MP3_OK;
}

playback_mp3_result_t playback_mp3_audio_pause(
    playback_mp3_audio_t *audio,
    uint32_t flags
)
{
    playback_mp3_audio_state_t *state;

    if ((audio == NULL) || (audio->state == NULL))
    {
        return PLAYBACK_MP3_NOT_OPEN;
    }
    if ((flags == 0U) || ((flags & ~((uint32_t)PLAYBACK_MP3_PAUSE_ALL)) != 0U))
    {
        return PLAYBACK_MP3_INVALID_ARGUMENT;
    }

    state = audio->state;
    tal_mutex_lock(state->pcm_mutex);
    if ((flags & PLAYBACK_MP3_PAUSE_FETCH) != 0U)
    {
        state->fetch_paused = true;
    }
    if ((flags & PLAYBACK_MP3_PAUSE_OUTPUT) != 0U)
    {
        state->output_paused = true;
    }
    tal_mutex_unlock(state->pcm_mutex);
    if ((flags & PLAYBACK_MP3_PAUSE_FETCH) != 0U)
    {
        playback_http_range_cancel(&state->reader);
    }
    return PLAYBACK_MP3_OK;
}

playback_mp3_result_t playback_mp3_audio_resume(
    playback_mp3_audio_t *audio,
    uint32_t flags
)
{
    playback_mp3_audio_state_t *state;

    if ((audio == NULL) || (audio->state == NULL))
    {
        return PLAYBACK_MP3_NOT_OPEN;
    }
    if ((flags == 0U) || ((flags & ~((uint32_t)PLAYBACK_MP3_PAUSE_ALL)) != 0U))
    {
        return PLAYBACK_MP3_INVALID_ARGUMENT;
    }

    state = audio->state;
    if ((flags & PLAYBACK_MP3_PAUSE_FETCH) != 0U)
    {
        playback_http_range_reset_cancel(&state->reader);
    }
    tal_mutex_lock(state->pcm_mutex);
    if ((flags & PLAYBACK_MP3_PAUSE_FETCH) != 0U)
    {
        state->fetch_paused = false;
    }
    if ((flags & PLAYBACK_MP3_PAUSE_OUTPUT) != 0U)
    {
        state->output_paused = false;
    }
    tal_mutex_unlock(state->pcm_mutex);
    return PLAYBACK_MP3_OK;
}

playback_mp3_result_t playback_mp3_audio_get_status(
    const playback_mp3_audio_t *audio,
    playback_mp3_status_t *status
)
{
    const playback_mp3_audio_state_t *state;

    if ((audio == NULL) || (audio->state == NULL))
    {
        return PLAYBACK_MP3_NOT_OPEN;
    }
    if (status == NULL)
    {
        return PLAYBACK_MP3_INVALID_ARGUMENT;
    }

    state = audio->state;
    memset(status, 0, sizeof(*status));
    tal_mutex_lock(state->pcm_mutex);
    status->open = true;
    status->fetch_paused = state->fetch_paused;
    status->output_paused = state->output_paused;
    status->source_exhausted = state->source_exhausted;
    status->fatal = state->fatal;
    status->channels = state->descriptor.channels;
    status->sample_rate = state->descriptor.sample_rate;
    status->low_water_frames = state->low_water_frames;
    status->high_water_frames = state->high_water_frames;
    status->available_pcm_frames = (uint32_t)(state->pcm_ring_count / state->descriptor.channels);
    status->consumed_pcm_frames = state->consumed_pcm_frames;
    status->expected_pcm_frames = state->expected_pcm_frames;
    status->next_index_record = state->next_record;
    status->consecutive_failures = state->consecutive_failures;
    status->recovered_frame_count = state->recovered_frame_count;
    status->last_failure = state->last_failure;
    tal_mutex_unlock(state->pcm_mutex);
    return PLAYBACK_MP3_OK;
}

void playback_mp3_audio_close(playback_mp3_audio_t *audio)
{
    playback_mp3_audio_state_t *state;

    if ((audio == NULL) || (audio->state == NULL))
    {
        return;
    }

    state = audio->state;
    audio->state = NULL;
    playback_http_range_cancel(&state->reader);
    playback_mp3_release_state(state);
}

playback_error_t playback_mp3_result_to_error(playback_mp3_result_t result)
{
    switch (result)
    {
        case PLAYBACK_MP3_OK:
        case PLAYBACK_MP3_BUFFER_FULL:
        case PLAYBACK_MP3_BUFFER_EMPTY:
        case PLAYBACK_MP3_FETCH_PAUSED:
        case PLAYBACK_MP3_OUTPUT_PAUSED:
        case PLAYBACK_MP3_END_OF_STREAM:
            return PLAYBACK_ERROR_NONE;
        case PLAYBACK_MP3_HTTP_FAILED:
            return PLAYBACK_ERROR_NETWORK_TIMEOUT;
        case PLAYBACK_MP3_RESOURCE_MISMATCH:
        case PLAYBACK_MP3_CRC_MISMATCH:
            return PLAYBACK_ERROR_CONTENT_INVALID;
        case PLAYBACK_MP3_INDEX_INVALID:
        case PLAYBACK_MP3_FRAME_TOO_LARGE:
            return PLAYBACK_ERROR_INDEX_INVALID;
        case PLAYBACK_MP3_DECODE_FAILED:
        case PLAYBACK_MP3_UNSUPPORTED_FORMAT:
        case PLAYBACK_MP3_RECOVERY_FAILED:
            return PLAYBACK_ERROR_MP3_DECODE_FAILED;
        case PLAYBACK_MP3_INVALID_ARGUMENT:
        case PLAYBACK_MP3_ALREADY_OPEN:
        case PLAYBACK_MP3_NOT_OPEN:
        case PLAYBACK_MP3_NO_MEMORY:
        default:
            return PLAYBACK_ERROR_INTERNAL;
    }
}

const char *playback_mp3_result_name(playback_mp3_result_t result)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "ALREADY_OPEN",
        "NOT_OPEN",
        "NO_MEMORY",
        "HTTP_FAILED",
        "RESOURCE_MISMATCH",
        "INDEX_INVALID",
        "FRAME_TOO_LARGE",
        "CRC_MISMATCH",
        "DECODE_FAILED",
        "UNSUPPORTED_FORMAT",
        "BUFFER_FULL",
        "BUFFER_EMPTY",
        "FETCH_PAUSED",
        "OUTPUT_PAUSED",
        "END_OF_STREAM",
        "RECOVERY_FAILED",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0])))
               ? names[result]
               : "UNKNOWN_MP3_RESULT";
}
