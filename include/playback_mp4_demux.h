#ifndef PLAYBACK_MP4_DEMUX_H
#define PLAYBACK_MP4_DEMUX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "playback_domain.h"
#include "playback_http_range.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_MP4_MAX_MOOV_BYTES (128U * 1024U)
#define PLAYBACK_MP4_MAX_SAMPLES (512U)
#define PLAYBACK_MP4_MAX_CODEC_CONFIG_BYTES (4096U)
#define PLAYBACK_MP4_MAX_NALS_PER_ACCESS_UNIT (128U)

typedef enum
{
    PLAYBACK_MP4_OK = 0,
    PLAYBACK_MP4_INVALID_ARGUMENT,
    PLAYBACK_MP4_CANCELLED,
    PLAYBACK_MP4_NETWORK_ERROR,
    PLAYBACK_MP4_RANGE_INVALID,
    PLAYBACK_MP4_RESOURCE_MISMATCH,
    PLAYBACK_MP4_UNSUPPORTED_CONTAINER,
    PLAYBACK_MP4_INVALID_CONTAINER,
    PLAYBACK_MP4_CONTENT_INVALID,
    PLAYBACK_MP4_NO_MEMORY,
    PLAYBACK_MP4_SAMPLE_OUT_OF_RANGE,
    PLAYBACK_MP4_BUFFER_TOO_SMALL,
} playback_mp4_result_t;

typedef struct
{
    uint8_t profile_idc;
    uint8_t profile_compatibility;
    uint8_t level_idc;
    uint8_t nal_length_size;
    uint16_t width;
    uint16_t height;
    uint32_t timescale;
    uint64_t duration_ticks;
    uint32_t duration_ms;
    uint32_t sample_count;
    uint32_t max_sample_size;
    size_t parameter_sets_annex_b_length;
} playback_mp4_codec_config_t;

typedef struct
{
    uint32_t index;
    uint32_t byte_offset;
    uint32_t byte_length;
    uint64_t dts;
    uint64_t pts;
    uint32_t duration;
    bool is_sync;
} playback_mp4_sample_t;

typedef struct
{
    void *state;
} playback_mp4_demux_t;

playback_mp4_result_t playback_mp4_demux_open(
    playback_mp4_demux_t *demux,
    const playback_asset_t *asset,
    const playback_http_config_t *http_config
);

/** Open a non-owning in-memory MP4 source. The buffer must outlive the demux. */
playback_mp4_result_t playback_mp4_demux_open_memory(
    playback_mp4_demux_t *demux,
    const uint8_t *data,
    size_t length
);

playback_mp4_result_t playback_mp4_demux_get_codec_config(
    const playback_mp4_demux_t *demux,
    playback_mp4_codec_config_t *config,
    uint8_t *parameter_sets_annex_b,
    size_t parameter_sets_capacity,
    size_t *parameter_sets_length
);

playback_mp4_result_t playback_mp4_demux_get_sample(
    const playback_mp4_demux_t *demux,
    uint32_t sample_index,
    playback_mp4_sample_t *sample
);

playback_mp4_result_t playback_mp4_demux_find_sample_at_or_after(
    const playback_mp4_demux_t *demux,
    uint32_t position_ms,
    playback_mp4_sample_t *sample
);

playback_mp4_result_t playback_mp4_demux_find_sync_at_or_before(
    const playback_mp4_demux_t *demux,
    uint32_t position_ms,
    playback_mp4_sample_t *sample
);

playback_mp4_result_t playback_mp4_demux_read_access_unit(
    playback_mp4_demux_t *demux,
    uint32_t sample_index,
    uint8_t *destination,
    size_t destination_capacity,
    size_t *access_unit_length
);

void playback_mp4_demux_cancel(playback_mp4_demux_t *demux);
void playback_mp4_demux_close(playback_mp4_demux_t *demux);
playback_error_t playback_mp4_result_to_error(playback_mp4_result_t result);
const char *playback_mp4_result_name(playback_mp4_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_MP4_DEMUX_H */
