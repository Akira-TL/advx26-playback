/**
 * @file playback_mp4_demux.c
 * @brief Bounded fast-start MP4 demuxer for immutable H.264 media.
 */

#include "playback_mp4_demux.h"

#include <limits.h>
#include <string.h>

#include "tal_api.h"

#define PLAYBACK_MP4_FTYP_MAX_BYTES (256U)
#define PLAYBACK_MP4_MAX_STSC_ENTRIES (PLAYBACK_MP4_MAX_SAMPLES)
#define PLAYBACK_MP4_MAX_TIMESCALE (1000000U)

#define MP4_FOURCC(a, b, c, d)                                                                                     \
    (((uint32_t)(uint8_t)(a) << 24U) | ((uint32_t)(uint8_t)(b) << 16U) | ((uint32_t)(uint8_t)(c) << 8U) |         \
     (uint32_t)(uint8_t)(d))

#define MP4_BOX_FTYP MP4_FOURCC('f', 't', 'y', 'p')
#define MP4_BOX_MOOV MP4_FOURCC('m', 'o', 'o', 'v')
#define MP4_BOX_MDAT MP4_FOURCC('m', 'd', 'a', 't')
#define MP4_BOX_MOOF MP4_FOURCC('m', 'o', 'o', 'f')
#define MP4_BOX_MVEX MP4_FOURCC('m', 'v', 'e', 'x')
#define MP4_BOX_MVHD MP4_FOURCC('m', 'v', 'h', 'd')
#define MP4_BOX_TRAK MP4_FOURCC('t', 'r', 'a', 'k')
#define MP4_BOX_TKHD MP4_FOURCC('t', 'k', 'h', 'd')
#define MP4_BOX_EDTS MP4_FOURCC('e', 'd', 't', 's')
#define MP4_BOX_ELST MP4_FOURCC('e', 'l', 's', 't')
#define MP4_BOX_TREF MP4_FOURCC('t', 'r', 'e', 'f')
#define MP4_BOX_MDIA MP4_FOURCC('m', 'd', 'i', 'a')
#define MP4_BOX_MDHD MP4_FOURCC('m', 'd', 'h', 'd')
#define MP4_BOX_HDLR MP4_FOURCC('h', 'd', 'l', 'r')
#define MP4_BOX_MINF MP4_FOURCC('m', 'i', 'n', 'f')
#define MP4_BOX_STBL MP4_FOURCC('s', 't', 'b', 'l')
#define MP4_BOX_STSD MP4_FOURCC('s', 't', 's', 'd')
#define MP4_BOX_STTS MP4_FOURCC('s', 't', 't', 's')
#define MP4_BOX_CTTS MP4_FOURCC('c', 't', 't', 's')
#define MP4_BOX_STSC MP4_FOURCC('s', 't', 's', 'c')
#define MP4_BOX_STSZ MP4_FOURCC('s', 't', 's', 'z')
#define MP4_BOX_STZ2 MP4_FOURCC('s', 't', 'z', '2')
#define MP4_BOX_STCO MP4_FOURCC('s', 't', 'c', 'o')
#define MP4_BOX_CO64 MP4_FOURCC('c', 'o', '6', '4')
#define MP4_BOX_STSS MP4_FOURCC('s', 't', 's', 's')
#define MP4_BOX_AVC1 MP4_FOURCC('a', 'v', 'c', '1')
#define MP4_BOX_AVCC MP4_FOURCC('a', 'v', 'c', 'C')
#define MP4_HANDLER_VIDEO MP4_FOURCC('v', 'i', 'd', 'e')

#define MP4_BRAND_ISOM MP4_FOURCC('i', 's', 'o', 'm')
#define MP4_BRAND_ISO2 MP4_FOURCC('i', 's', 'o', '2')
#define MP4_BRAND_MP41 MP4_FOURCC('m', 'p', '4', '1')
#define MP4_BRAND_MP42 MP4_FOURCC('m', 'p', '4', '2')
#define MP4_BRAND_AVC1 MP4_FOURCC('a', 'v', 'c', '1')

#define MP4_NAL_TYPE_NON_IDR (1U)
#define MP4_NAL_TYPE_IDR (5U)
#define MP4_NAL_TYPE_SPS (7U)
#define MP4_NAL_TYPE_PPS (8U)

typedef struct
{
    uint32_t type;
    const uint8_t *data;
    size_t data_length;
    size_t size;
    size_t header_size;
} playback_mp4_memory_box_t;

typedef struct
{
    uint32_t type;
    uint32_t offset;
    uint32_t size;
    uint32_t header_size;
} playback_mp4_file_box_t;

typedef struct
{
    const uint8_t *data;
    size_t length;
    bool present;
} playback_mp4_box_view_t;

typedef struct
{
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
} playback_mp4_stsc_entry_t;

typedef struct
{
    bool mvhd_found;
    uint32_t movie_timescale;
    uint64_t movie_duration;

    bool tkhd_found;
    uint32_t track_id;
    uint64_t track_duration;
    uint16_t track_width;
    uint16_t track_height;

    bool edit_found;
    uint64_t edit_segment_duration;
    int64_t edit_media_time;
    int16_t edit_rate_integer;
    int16_t edit_rate_fraction;

    bool mdhd_found;
    uint32_t media_timescale;
    uint64_t media_duration;

    bool hdlr_found;
    uint32_t handler_type;

    playback_mp4_box_view_t stsd;
    playback_mp4_box_view_t stts;
    playback_mp4_box_view_t stsc;
    playback_mp4_box_view_t stsz;
    playback_mp4_box_view_t chunk_offsets;
    playback_mp4_box_view_t stss;
    bool chunk_offsets_are_64_bit;
} playback_mp4_builder_t;

typedef struct
{
    playback_http_range_reader_t reader;
    playback_http_metadata_t metadata;
    const uint8_t *memory_data;
    size_t memory_length;
    bool memory_source;
    playback_mp4_codec_config_t codec;
    playback_mp4_sample_t *samples;
    uint8_t *sample_scratch;
    size_t sample_scratch_capacity;
    uint8_t parameter_sets[PLAYBACK_MP4_MAX_CODEC_CONFIG_BYTES];
    size_t parameter_sets_length;
    uint32_t mdat_payload_start;
    uint32_t mdat_end;
} playback_mp4_state_t;

typedef enum
{
    PLAYBACK_MP4_BOX_OK = 0,
    PLAYBACK_MP4_BOX_END,
    PLAYBACK_MP4_BOX_INVALID,
} playback_mp4_box_result_t;

static uint16_t playback_mp4_read_be16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8U) | (uint16_t)data[1];
}

static int16_t playback_mp4_read_be_i16(const uint8_t *data)
{
    return (int16_t)playback_mp4_read_be16(data);
}

static uint32_t playback_mp4_read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) | ((uint32_t)data[2] << 8U) |
           (uint32_t)data[3];
}

static int32_t playback_mp4_read_be_i32(const uint8_t *data)
{
    return (int32_t)playback_mp4_read_be32(data);
}

static uint64_t playback_mp4_read_be64(const uint8_t *data)
{
    return ((uint64_t)playback_mp4_read_be32(data) << 32U) | (uint64_t)playback_mp4_read_be32(data + 4U);
}

static int64_t playback_mp4_read_be_i64(const uint8_t *data)
{
    return (int64_t)playback_mp4_read_be64(data);
}

static bool playback_mp4_u64_add(uint64_t left, uint64_t right, uint64_t *result)
{
    if ((result == NULL) || (UINT64_MAX - left < right))
    {
        return false;
    }

    *result = left + right;
    return true;
}

static bool playback_mp4_size_add(size_t left, size_t right, size_t *result)
{
    if ((result == NULL) || (SIZE_MAX - left < right))
    {
        return false;
    }

    *result = left + right;
    return true;
}

static playback_mp4_box_result_t playback_mp4_next_memory_box(
    const uint8_t *buffer,
    size_t buffer_length,
    size_t *cursor,
    playback_mp4_memory_box_t *box
)
{
    size_t offset;
    uint32_t size32;
    uint64_t size64;
    size_t box_size;
    size_t header_size = 8U;

    if ((buffer == NULL) || (cursor == NULL) || (box == NULL) || (*cursor > buffer_length))
    {
        return PLAYBACK_MP4_BOX_INVALID;
    }
    if (*cursor == buffer_length)
    {
        return PLAYBACK_MP4_BOX_END;
    }

    offset = *cursor;
    if (buffer_length - offset < 8U)
    {
        return PLAYBACK_MP4_BOX_INVALID;
    }

    size32 = playback_mp4_read_be32(buffer + offset);
    box->type = playback_mp4_read_be32(buffer + offset + 4U);
    if (size32 == 1U)
    {
        if (buffer_length - offset < 16U)
        {
            return PLAYBACK_MP4_BOX_INVALID;
        }
        size64 = playback_mp4_read_be64(buffer + offset + 8U);
        if (size64 > SIZE_MAX)
        {
            return PLAYBACK_MP4_BOX_INVALID;
        }
        box_size = (size_t)size64;
        header_size = 16U;
    }
    else if (size32 == 0U)
    {
        box_size = buffer_length - offset;
    }
    else
    {
        box_size = size32;
    }

    if ((box_size < header_size) || (box_size > buffer_length - offset))
    {
        return PLAYBACK_MP4_BOX_INVALID;
    }

    box->size = box_size;
    box->header_size = header_size;
    box->data = buffer + offset + header_size;
    box->data_length = box_size - header_size;
    *cursor = offset + box_size;
    return PLAYBACK_MP4_BOX_OK;
}

static playback_mp4_result_t playback_mp4_map_http_result(playback_http_result_t result)
{
    switch (result)
    {
        case PLAYBACK_HTTP_OK:
            return PLAYBACK_MP4_OK;
        case PLAYBACK_HTTP_CANCELLED:
            return PLAYBACK_MP4_CANCELLED;
        case PLAYBACK_HTTP_NETWORK_ERROR:
        case PLAYBACK_HTTP_STATUS_ERROR:
            return PLAYBACK_MP4_NETWORK_ERROR;
        case PLAYBACK_HTTP_RANGE_INVALID:
        case PLAYBACK_HTTP_RESPONSE_TOO_LARGE:
            return PLAYBACK_MP4_RANGE_INVALID;
        case PLAYBACK_HTTP_RESOURCE_MISMATCH:
            return PLAYBACK_MP4_RESOURCE_MISMATCH;
        case PLAYBACK_HTTP_INVALID_ARGUMENT:
        case PLAYBACK_HTTP_INVALID_URL:
        default:
            return PLAYBACK_MP4_INVALID_ARGUMENT;
    }
}

static playback_mp4_result_t playback_mp4_read_at(
    playback_mp4_state_t *state,
    uint32_t offset,
    uint32_t length,
    uint8_t *destination,
    size_t destination_capacity
)
{
    playback_http_result_t http_result;

    if ((state == NULL) || (destination == NULL) || (length == 0U) ||
        (length > destination_capacity))
    {
        return PLAYBACK_MP4_INVALID_ARGUMENT;
    }

    if (state->memory_source)
    {
        if (((uint64_t)offset + length > state->memory_length) ||
            (state->memory_data == NULL))
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
        memcpy(destination, state->memory_data + offset, length);
        return PLAYBACK_MP4_OK;
    }

    http_result = playback_http_range_read_at(
        &state->reader,
        offset,
        length,
        destination,
        destination_capacity
    );
    return playback_mp4_map_http_result(http_result);
}

static playback_mp4_result_t playback_mp4_read_file_box(
    playback_mp4_state_t *state,
    uint32_t offset,
    playback_mp4_file_box_t *box
)
{
    uint8_t header[16U];
    uint32_t size32;
    uint64_t size64;
    uint64_t box_end;
    playback_mp4_result_t read_result;

    if ((state == NULL) || (box == NULL) || (offset >= state->metadata.content_length) ||
        (state->metadata.content_length - offset < 8U))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    read_result = playback_mp4_read_at(state, offset, 8U, header, sizeof(header));
    if (read_result != PLAYBACK_MP4_OK)
    {
        return read_result;
    }

    memset(box, 0, sizeof(*box));
    size32 = playback_mp4_read_be32(header);
    box->type = playback_mp4_read_be32(header + 4U);
    box->offset = offset;
    box->header_size = 8U;

    if (size32 == 1U)
    {
        if (state->metadata.content_length - offset < 16U)
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
        read_result = playback_mp4_read_at(state, offset + 8U, 8U, header + 8U, 8U);
        if (read_result != PLAYBACK_MP4_OK)
        {
            return read_result;
        }
        size64 = playback_mp4_read_be64(header + 8U);
        if ((size64 > UINT32_MAX) || (size64 < 16U))
        {
            return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
        }
        box->size = (uint32_t)size64;
        box->header_size = 16U;
    }
    else if (size32 == 0U)
    {
        box->size = state->metadata.content_length - offset;
    }
    else
    {
        box->size = size32;
    }

    if (box->size < box->header_size)
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }
    box_end = (uint64_t)box->offset + (uint64_t)box->size;
    if (box_end > state->metadata.content_length)
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    return PLAYBACK_MP4_OK;
}

static bool playback_mp4_brand_supported(uint32_t brand)
{
    return (brand == MP4_BRAND_ISOM) || (brand == MP4_BRAND_ISO2) || (brand == MP4_BRAND_MP41) ||
           (brand == MP4_BRAND_MP42) || (brand == MP4_BRAND_AVC1);
}

static playback_mp4_result_t playback_mp4_validate_ftyp(
    const uint8_t *box,
    size_t box_size,
    size_t header_size
)
{
    const uint8_t *data;
    size_t data_length;
    size_t cursor;
    bool supported;

    if ((box == NULL) || (box_size < header_size) || (box_size - header_size < 8U))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    data = box + header_size;
    data_length = box_size - header_size;
    if (((data_length - 8U) % 4U) != 0U)
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    supported = playback_mp4_brand_supported(playback_mp4_read_be32(data));
    for (cursor = 8U; cursor < data_length; cursor += 4U)
    {
        supported = supported || playback_mp4_brand_supported(playback_mp4_read_be32(data + cursor));
    }

    return supported ? PLAYBACK_MP4_OK : PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
}

static playback_mp4_result_t playback_mp4_scan_file(
    playback_mp4_state_t *state,
    uint8_t **moov_buffer,
    size_t *moov_size,
    size_t *moov_header_size
)
{
    uint32_t offset = 0U;
    bool ftyp_found = false;
    bool moov_found = false;
    bool mdat_found = false;
    playback_mp4_result_t result;

    if ((state == NULL) || (moov_buffer == NULL) || (moov_size == NULL) || (moov_header_size == NULL))
    {
        return PLAYBACK_MP4_INVALID_ARGUMENT;
    }

    *moov_buffer = NULL;
    *moov_size = 0U;
    *moov_header_size = 0U;

    while (offset < state->metadata.content_length)
    {
        playback_mp4_file_box_t box;
        uint64_t next_offset;

        result = playback_mp4_read_file_box(state, offset, &box);
        if (result != PLAYBACK_MP4_OK)
        {
            return result;
        }

        if (box.type == MP4_BOX_FTYP)
        {
            uint8_t ftyp[PLAYBACK_MP4_FTYP_MAX_BYTES];

            if (ftyp_found || (offset != 0U) || (box.size > sizeof(ftyp)))
            {
                return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
            }
            result = playback_mp4_read_at(state, box.offset, box.size, ftyp, sizeof(ftyp));
            if (result != PLAYBACK_MP4_OK)
            {
                return result;
            }
            result = playback_mp4_validate_ftyp(ftyp, box.size, box.header_size);
            if (result != PLAYBACK_MP4_OK)
            {
                return result;
            }
            ftyp_found = true;
        }
        else if ((box.type == MP4_BOX_MOOF) || (box.type == MP4_BOX_MVEX))
        {
            return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
        }
        else if (box.type == MP4_BOX_MOOV)
        {
            if (!ftyp_found || moov_found || mdat_found || (box.size > PLAYBACK_MP4_MAX_MOOV_BYTES) ||
                (box.size > PLAYBACK_HTTP_RANGE_MAX_LENGTH))
            {
                return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
            }

            *moov_buffer = tal_malloc(box.size);
            if (*moov_buffer == NULL)
            {
                return PLAYBACK_MP4_NO_MEMORY;
            }
            result = playback_mp4_read_at(state, box.offset, box.size, *moov_buffer, box.size);
            if (result != PLAYBACK_MP4_OK)
            {
                tal_free(*moov_buffer);
                *moov_buffer = NULL;
                return result;
            }
            *moov_size = box.size;
            *moov_header_size = box.header_size;
            moov_found = true;
        }
        else if (box.type == MP4_BOX_MDAT)
        {
            uint64_t payload_start;
            uint64_t box_end;

            if (!moov_found || mdat_found)
            {
                return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
            }
            payload_start = (uint64_t)box.offset + (uint64_t)box.header_size;
            box_end = (uint64_t)box.offset + (uint64_t)box.size;
            if ((payload_start >= box_end) || (box_end > UINT32_MAX))
            {
                return PLAYBACK_MP4_INVALID_CONTAINER;
            }
            state->mdat_payload_start = (uint32_t)payload_start;
            state->mdat_end = (uint32_t)box_end;
            mdat_found = true;
        }

        next_offset = (uint64_t)offset + (uint64_t)box.size;
        if ((next_offset > UINT32_MAX) || ((uint32_t)next_offset <= offset))
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
        offset = (uint32_t)next_offset;
    }

    if (!ftyp_found || !moov_found || !mdat_found || (offset != state->metadata.content_length))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    return PLAYBACK_MP4_OK;
}

static bool playback_mp4_box_view_set(playback_mp4_box_view_t *view, const playback_mp4_memory_box_t *box)
{
    if ((view == NULL) || (box == NULL) || view->present)
    {
        return false;
    }

    view->data = box->data;
    view->length = box->data_length;
    view->present = true;
    return true;
}

static playback_mp4_result_t playback_mp4_parse_mvhd(
    const playback_mp4_memory_box_t *box,
    playback_mp4_builder_t *builder
)
{
    uint8_t version;

    if ((box == NULL) || (builder == NULL) || builder->mvhd_found || (box->data_length < 4U))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    version = box->data[0];
    if (version == 0U)
    {
        if (box->data_length < 20U)
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
        builder->movie_timescale = playback_mp4_read_be32(box->data + 12U);
        builder->movie_duration = playback_mp4_read_be32(box->data + 16U);
    }
    else if (version == 1U)
    {
        if (box->data_length < 32U)
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
        builder->movie_timescale = playback_mp4_read_be32(box->data + 20U);
        builder->movie_duration = playback_mp4_read_be64(box->data + 24U);
    }
    else
    {
        return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
    }

    if ((builder->movie_timescale == 0U) || (builder->movie_timescale > PLAYBACK_MP4_MAX_TIMESCALE) ||
        (builder->movie_duration == 0U))
    {
        return PLAYBACK_MP4_CONTENT_INVALID;
    }

    builder->mvhd_found = true;
    return PLAYBACK_MP4_OK;
}

static playback_mp4_result_t playback_mp4_parse_tkhd(
    const playback_mp4_memory_box_t *box,
    playback_mp4_builder_t *builder
)
{
    uint8_t version;
    uint32_t width_fixed;
    uint32_t height_fixed;

    if ((box == NULL) || (builder == NULL) || builder->tkhd_found || (box->data_length < 4U))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    version = box->data[0];
    if (version == 0U)
    {
        if (box->data_length < 84U)
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
        builder->track_id = playback_mp4_read_be32(box->data + 12U);
        builder->track_duration = playback_mp4_read_be32(box->data + 20U);
        width_fixed = playback_mp4_read_be32(box->data + 76U);
        height_fixed = playback_mp4_read_be32(box->data + 80U);
    }
    else if (version == 1U)
    {
        if (box->data_length < 96U)
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
        builder->track_id = playback_mp4_read_be32(box->data + 20U);
        builder->track_duration = playback_mp4_read_be64(box->data + 28U);
        width_fixed = playback_mp4_read_be32(box->data + 88U);
        height_fixed = playback_mp4_read_be32(box->data + 92U);
    }
    else
    {
        return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
    }

    if ((builder->track_id == 0U) || (builder->track_duration == 0U) || ((width_fixed & 0xFFFFU) != 0U) ||
        ((height_fixed & 0xFFFFU) != 0U) || ((width_fixed >> 16U) == 0U) || ((height_fixed >> 16U) == 0U))
    {
        return PLAYBACK_MP4_CONTENT_INVALID;
    }

    builder->track_width = (uint16_t)(width_fixed >> 16U);
    builder->track_height = (uint16_t)(height_fixed >> 16U);
    builder->tkhd_found = true;
    return PLAYBACK_MP4_OK;
}

static playback_mp4_result_t playback_mp4_parse_edts(
    const playback_mp4_memory_box_t *box,
    playback_mp4_builder_t *builder
)
{
    size_t cursor = 0U;
    playback_mp4_memory_box_t child;
    playback_mp4_box_result_t box_result;
    bool elst_found = false;

    if ((box == NULL) || (builder == NULL) || builder->edit_found)
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    while ((box_result = playback_mp4_next_memory_box(box->data, box->data_length, &cursor, &child)) ==
           PLAYBACK_MP4_BOX_OK)
    {
        uint8_t version;
        uint32_t entry_count;

        if ((child.type != MP4_BOX_ELST) || elst_found || (child.data_length < 8U))
        {
            return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
        }

        version = child.data[0];
        entry_count = playback_mp4_read_be32(child.data + 4U);
        if (entry_count != 1U)
        {
            return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
        }

        if (version == 0U)
        {
            if (child.data_length != 20U)
            {
                return PLAYBACK_MP4_INVALID_CONTAINER;
            }
            builder->edit_segment_duration = playback_mp4_read_be32(child.data + 8U);
            builder->edit_media_time = playback_mp4_read_be_i32(child.data + 12U);
            builder->edit_rate_integer = playback_mp4_read_be_i16(child.data + 16U);
            builder->edit_rate_fraction = playback_mp4_read_be_i16(child.data + 18U);
        }
        else if (version == 1U)
        {
            if (child.data_length != 28U)
            {
                return PLAYBACK_MP4_INVALID_CONTAINER;
            }
            builder->edit_segment_duration = playback_mp4_read_be64(child.data + 8U);
            builder->edit_media_time = playback_mp4_read_be_i64(child.data + 16U);
            builder->edit_rate_integer = playback_mp4_read_be_i16(child.data + 24U);
            builder->edit_rate_fraction = playback_mp4_read_be_i16(child.data + 26U);
        }
        else
        {
            return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
        }
        elst_found = true;
    }

    if ((box_result != PLAYBACK_MP4_BOX_END) || !elst_found)
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    if ((builder->edit_media_time != 0) || (builder->edit_rate_integer != 1) ||
        (builder->edit_rate_fraction != 0) || (builder->edit_segment_duration == 0U))
    {
        return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
    }

    builder->edit_found = true;
    return PLAYBACK_MP4_OK;
}

static playback_mp4_result_t playback_mp4_parse_mdhd(
    const playback_mp4_memory_box_t *box,
    playback_mp4_builder_t *builder
)
{
    uint8_t version;

    if ((box == NULL) || (builder == NULL) || builder->mdhd_found || (box->data_length < 4U))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    version = box->data[0];
    if (version == 0U)
    {
        if (box->data_length < 20U)
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
        builder->media_timescale = playback_mp4_read_be32(box->data + 12U);
        builder->media_duration = playback_mp4_read_be32(box->data + 16U);
    }
    else if (version == 1U)
    {
        if (box->data_length < 32U)
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
        builder->media_timescale = playback_mp4_read_be32(box->data + 20U);
        builder->media_duration = playback_mp4_read_be64(box->data + 24U);
    }
    else
    {
        return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
    }

    if ((builder->media_timescale == 0U) || (builder->media_timescale > PLAYBACK_MP4_MAX_TIMESCALE) ||
        (builder->media_duration == 0U))
    {
        return PLAYBACK_MP4_CONTENT_INVALID;
    }

    builder->mdhd_found = true;
    return PLAYBACK_MP4_OK;
}

static playback_mp4_result_t playback_mp4_parse_hdlr(
    const playback_mp4_memory_box_t *box,
    playback_mp4_builder_t *builder
)
{
    if ((box == NULL) || (builder == NULL) || builder->hdlr_found || (box->data_length < 12U) ||
        (box->data[0] != 0U))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    builder->handler_type = playback_mp4_read_be32(box->data + 8U);
    builder->hdlr_found = true;
    return (builder->handler_type == MP4_HANDLER_VIDEO) ? PLAYBACK_MP4_OK : PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
}

static playback_mp4_result_t playback_mp4_append_parameter_set(
    playback_mp4_state_t *state,
    const uint8_t *nal,
    size_t nal_length,
    uint8_t expected_type
)
{
    size_t required;

    if ((state == NULL) || (nal == NULL) || (nal_length == 0U) || ((nal[0] & 0x1FU) != expected_type) ||
        !playback_mp4_size_add(state->parameter_sets_length, 4U + nal_length, &required) ||
        (required > sizeof(state->parameter_sets)))
    {
        return PLAYBACK_MP4_CONTENT_INVALID;
    }

    state->parameter_sets[state->parameter_sets_length] = 0U;
    state->parameter_sets[state->parameter_sets_length + 1U] = 0U;
    state->parameter_sets[state->parameter_sets_length + 2U] = 0U;
    state->parameter_sets[state->parameter_sets_length + 3U] = 1U;
    memcpy(state->parameter_sets + state->parameter_sets_length + 4U, nal, nal_length);
    state->parameter_sets_length = required;
    return PLAYBACK_MP4_OK;
}

static playback_mp4_result_t playback_mp4_parse_avcc(
    const playback_mp4_memory_box_t *box,
    playback_mp4_state_t *state
)
{
    size_t cursor;
    uint8_t sps_count;
    uint8_t pps_count;
    uint8_t index;

    if ((box == NULL) || (state == NULL) || (box->data_length < 7U) || (box->data[0] != 1U) ||
        (state->parameter_sets_length != 0U))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    state->codec.profile_idc = box->data[1];
    state->codec.profile_compatibility = box->data[2];
    state->codec.level_idc = box->data[3];
    state->codec.nal_length_size = (uint8_t)((box->data[4] & 0x03U) + 1U);
    if (state->codec.nal_length_size == 3U)
    {
        return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
    }

    sps_count = (uint8_t)(box->data[5] & 0x1FU);
    if ((sps_count == 0U) || (sps_count > 8U))
    {
        return PLAYBACK_MP4_CONTENT_INVALID;
    }

    cursor = 6U;
    for (index = 0U; index < sps_count; ++index)
    {
        uint16_t nal_length;
        playback_mp4_result_t result;

        if (box->data_length - cursor < 2U)
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
        nal_length = playback_mp4_read_be16(box->data + cursor);
        cursor += 2U;
        if ((nal_length == 0U) || (nal_length > box->data_length - cursor))
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
        result = playback_mp4_append_parameter_set(state, box->data + cursor, nal_length, MP4_NAL_TYPE_SPS);
        if (result != PLAYBACK_MP4_OK)
        {
            return result;
        }
        cursor += nal_length;
    }

    if (box->data_length - cursor < 1U)
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }
    pps_count = box->data[cursor++];
    if ((pps_count == 0U) || (pps_count > 8U))
    {
        return PLAYBACK_MP4_CONTENT_INVALID;
    }

    for (index = 0U; index < pps_count; ++index)
    {
        uint16_t nal_length;
        playback_mp4_result_t result;

        if (box->data_length - cursor < 2U)
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
        nal_length = playback_mp4_read_be16(box->data + cursor);
        cursor += 2U;
        if ((nal_length == 0U) || (nal_length > box->data_length - cursor))
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
        result = playback_mp4_append_parameter_set(state, box->data + cursor, nal_length, MP4_NAL_TYPE_PPS);
        if (result != PLAYBACK_MP4_OK)
        {
            return result;
        }
        cursor += nal_length;
    }

    if (cursor != box->data_length)
    {
        return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
    }

    state->codec.parameter_sets_annex_b_length = state->parameter_sets_length;
    return PLAYBACK_MP4_OK;
}

static playback_mp4_result_t playback_mp4_parse_stsd(
    const playback_mp4_box_view_t *view,
    playback_mp4_state_t *state
)
{
    const uint8_t *entries;
    size_t entries_length;
    size_t cursor = 0U;
    playback_mp4_memory_box_t entry;
    playback_mp4_box_result_t box_result;
    uint32_t entry_count;
    size_t child_cursor = 0U;
    playback_mp4_memory_box_t child;
    bool avcc_found = false;

    if ((view == NULL) || (state == NULL) || !view->present || (view->length < 8U) || (view->data[0] != 0U))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    entry_count = playback_mp4_read_be32(view->data + 4U);
    if (entry_count != 1U)
    {
        return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
    }

    entries = view->data + 8U;
    entries_length = view->length - 8U;
    box_result = playback_mp4_next_memory_box(entries, entries_length, &cursor, &entry);
    if ((box_result != PLAYBACK_MP4_BOX_OK) || (entry.type != MP4_BOX_AVC1) || (entry.data_length < 78U) ||
        (cursor != entries_length))
    {
        return (box_result == PLAYBACK_MP4_BOX_INVALID) ? PLAYBACK_MP4_INVALID_CONTAINER
                                                       : PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
    }

    if ((playback_mp4_read_be16(entry.data + 6U) != 1U) ||
        (playback_mp4_read_be16(entry.data + 40U) != 1U))
    {
        return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
    }

    state->codec.width = playback_mp4_read_be16(entry.data + 24U);
    state->codec.height = playback_mp4_read_be16(entry.data + 26U);
    if ((state->codec.width == 0U) || (state->codec.height == 0U))
    {
        return PLAYBACK_MP4_CONTENT_INVALID;
    }

    while ((box_result = playback_mp4_next_memory_box(
                entry.data + 78U,
                entry.data_length - 78U,
                &child_cursor,
                &child
            )) == PLAYBACK_MP4_BOX_OK)
    {
        if (child.type == MP4_BOX_AVCC)
        {
            playback_mp4_result_t result;
            if (avcc_found)
            {
                return PLAYBACK_MP4_INVALID_CONTAINER;
            }
            result = playback_mp4_parse_avcc(&child, state);
            if (result != PLAYBACK_MP4_OK)
            {
                return result;
            }
            avcc_found = true;
        }
    }

    if ((box_result != PLAYBACK_MP4_BOX_END) || !avcc_found)
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    return PLAYBACK_MP4_OK;
}

static playback_mp4_result_t playback_mp4_parse_stsz(
    const playback_mp4_box_view_t *view,
    playback_mp4_state_t *state
)
{
    uint32_t uniform_size;
    uint32_t sample_count;
    size_t expected_length;
    uint32_t index;

    if ((view == NULL) || (state == NULL) || !view->present || (view->length < 12U) || (view->data[0] != 0U) ||
        (state->samples != NULL))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    uniform_size = playback_mp4_read_be32(view->data + 4U);
    sample_count = playback_mp4_read_be32(view->data + 8U);
    if ((sample_count == 0U) || (sample_count > PLAYBACK_MP4_MAX_SAMPLES))
    {
        return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
    }

    expected_length = 12U;
    if (uniform_size == 0U)
    {
        if (!playback_mp4_size_add(expected_length, (size_t)sample_count * 4U, &expected_length) ||
            (view->length != expected_length))
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
    }
    else if (view->length != expected_length)
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    state->samples = tal_calloc(sample_count, sizeof(*state->samples));
    if (state->samples == NULL)
    {
        return PLAYBACK_MP4_NO_MEMORY;
    }

    state->codec.sample_count = sample_count;
    for (index = 0U; index < sample_count; ++index)
    {
        uint32_t sample_size = uniform_size;
        if (uniform_size == 0U)
        {
            sample_size = playback_mp4_read_be32(view->data + 12U + ((size_t)index * 4U));
        }
        if ((sample_size == 0U) || (sample_size > PLAYBACK_HTTP_RANGE_MAX_LENGTH))
        {
            return PLAYBACK_MP4_CONTENT_INVALID;
        }

        state->samples[index].index = index;
        state->samples[index].byte_length = sample_size;
        if (sample_size > state->codec.max_sample_size)
        {
            state->codec.max_sample_size = sample_size;
        }
    }

    return PLAYBACK_MP4_OK;
}

static playback_mp4_result_t playback_mp4_parse_stts(
    const playback_mp4_box_view_t *view,
    playback_mp4_state_t *state
)
{
    uint32_t entry_count;
    size_t expected_length;
    uint32_t entry_index;
    uint32_t sample_index = 0U;
    uint64_t dts = 0U;

    if ((view == NULL) || (state == NULL) || !view->present || (view->length < 8U) || (view->data[0] != 0U) ||
        (state->samples == NULL) || (state->codec.sample_count == 0U))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    entry_count = playback_mp4_read_be32(view->data + 4U);
    expected_length = 8U + ((size_t)entry_count * 8U);
    if ((entry_count == 0U) || (entry_count > state->codec.sample_count) || (view->length != expected_length))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    for (entry_index = 0U; entry_index < entry_count; ++entry_index)
    {
        const uint8_t *entry = view->data + 8U + ((size_t)entry_index * 8U);
        uint32_t count = playback_mp4_read_be32(entry);
        uint32_t delta = playback_mp4_read_be32(entry + 4U);
        uint32_t count_index;

        if ((count == 0U) || (delta == 0U) || (count > state->codec.sample_count - sample_index))
        {
            return PLAYBACK_MP4_CONTENT_INVALID;
        }

        for (count_index = 0U; count_index < count; ++count_index)
        {
            uint64_t next_dts;
            state->samples[sample_index].dts = dts;
            state->samples[sample_index].pts = dts;
            state->samples[sample_index].duration = delta;
            if (!playback_mp4_u64_add(dts, delta, &next_dts))
            {
                return PLAYBACK_MP4_CONTENT_INVALID;
            }
            dts = next_dts;
            sample_index++;
        }
    }

    if (sample_index != state->codec.sample_count)
    {
        return PLAYBACK_MP4_CONTENT_INVALID;
    }

    state->codec.duration_ticks = dts;
    return PLAYBACK_MP4_OK;
}

static playback_mp4_result_t playback_mp4_parse_stss(
    const playback_mp4_box_view_t *view,
    playback_mp4_state_t *state
)
{
    uint32_t index;

    if ((view == NULL) || (state == NULL) || (state->samples == NULL))
    {
        return PLAYBACK_MP4_INVALID_ARGUMENT;
    }

    if (!view->present)
    {
        for (index = 0U; index < state->codec.sample_count; ++index)
        {
            state->samples[index].is_sync = true;
        }
        return PLAYBACK_MP4_OK;
    }
    else
    {
        uint32_t entry_count;
        uint32_t previous = 0U;
        size_t expected_length;

        if ((view->length < 8U) || (view->data[0] != 0U))
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
        entry_count = playback_mp4_read_be32(view->data + 4U);
        expected_length = 8U + ((size_t)entry_count * 4U);
        if ((entry_count == 0U) || (entry_count > state->codec.sample_count) || (view->length != expected_length))
        {
            return PLAYBACK_MP4_CONTENT_INVALID;
        }

        for (index = 0U; index < entry_count; ++index)
        {
            uint32_t sample_number = playback_mp4_read_be32(view->data + 8U + ((size_t)index * 4U));
            if ((sample_number == 0U) || (sample_number > state->codec.sample_count) ||
                (sample_number <= previous))
            {
                return PLAYBACK_MP4_CONTENT_INVALID;
            }
            state->samples[sample_number - 1U].is_sync = true;
            previous = sample_number;
        }
    }

    return state->samples[0].is_sync ? PLAYBACK_MP4_OK : PLAYBACK_MP4_CONTENT_INVALID;
}

static playback_mp4_result_t playback_mp4_parse_chunk_offsets(
    const playback_mp4_box_view_t *stsc_view,
    const playback_mp4_box_view_t *offset_view,
    bool offsets_are_64_bit,
    playback_mp4_state_t *state
)
{
    playback_mp4_stsc_entry_t *entries = NULL;
    uint32_t stsc_count;
    uint32_t chunk_count;
    size_t expected_length;
    uint32_t index;
    uint32_t sample_index = 0U;
    uint32_t stsc_index = 0U;
    uint64_t previous_sample_end = 0U;
    playback_mp4_result_t result = PLAYBACK_MP4_CONTENT_INVALID;

    if ((stsc_view == NULL) || (offset_view == NULL) || (state == NULL) || !stsc_view->present ||
        !offset_view->present || (stsc_view->length < 8U) || (offset_view->length < 8U) ||
        (stsc_view->data[0] != 0U) || (offset_view->data[0] != 0U) || (state->samples == NULL))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    stsc_count = playback_mp4_read_be32(stsc_view->data + 4U);
    expected_length = 8U + ((size_t)stsc_count * 12U);
    if ((stsc_count == 0U) || (stsc_count > PLAYBACK_MP4_MAX_STSC_ENTRIES) ||
        (stsc_view->length != expected_length))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    chunk_count = playback_mp4_read_be32(offset_view->data + 4U);
    expected_length = 8U + ((size_t)chunk_count * (offsets_are_64_bit ? 8U : 4U));
    if ((chunk_count == 0U) || (chunk_count > state->codec.sample_count) || (offset_view->length != expected_length))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    entries = tal_calloc(stsc_count, sizeof(*entries));
    if (entries == NULL)
    {
        return PLAYBACK_MP4_NO_MEMORY;
    }

    for (index = 0U; index < stsc_count; ++index)
    {
        const uint8_t *entry = stsc_view->data + 8U + ((size_t)index * 12U);
        uint32_t description_index = playback_mp4_read_be32(entry + 8U);

        entries[index].first_chunk = playback_mp4_read_be32(entry);
        entries[index].samples_per_chunk = playback_mp4_read_be32(entry + 4U);
        if ((entries[index].first_chunk == 0U) || (entries[index].first_chunk > chunk_count) ||
            (entries[index].samples_per_chunk == 0U) || (description_index != 1U) ||
            ((index == 0U) && (entries[index].first_chunk != 1U)) ||
            ((index > 0U) && (entries[index].first_chunk <= entries[index - 1U].first_chunk)))
        {
            goto cleanup;
        }
    }

    for (index = 0U; index < chunk_count; ++index)
    {
        uint64_t chunk_offset;
        uint64_t relative_offset = 0U;
        uint32_t in_chunk;

        while (((stsc_index + 1U) < stsc_count) && (entries[stsc_index + 1U].first_chunk <= (index + 1U)))
        {
            stsc_index++;
        }

        if (offsets_are_64_bit)
        {
            chunk_offset = playback_mp4_read_be64(offset_view->data + 8U + ((size_t)index * 8U));
        }
        else
        {
            chunk_offset = playback_mp4_read_be32(offset_view->data + 8U + ((size_t)index * 4U));
        }

        if ((chunk_offset > UINT32_MAX) || (chunk_offset < state->mdat_payload_start) ||
            (chunk_offset >= state->mdat_end))
        {
            goto cleanup;
        }

        for (in_chunk = 0U; in_chunk < entries[stsc_index].samples_per_chunk; ++in_chunk)
        {
            uint64_t sample_offset;
            uint64_t sample_end;

            if (sample_index >= state->codec.sample_count ||
                !playback_mp4_u64_add(chunk_offset, relative_offset, &sample_offset) ||
                !playback_mp4_u64_add(sample_offset, state->samples[sample_index].byte_length, &sample_end) ||
                (sample_offset > UINT32_MAX) || (sample_end > state->mdat_end) ||
                ((sample_index > 0U) && (sample_offset < previous_sample_end)))
            {
                goto cleanup;
            }

            state->samples[sample_index].byte_offset = (uint32_t)sample_offset;
            previous_sample_end = sample_end;
            relative_offset += state->samples[sample_index].byte_length;
            sample_index++;
        }
    }

    if (sample_index != state->codec.sample_count)
    {
        goto cleanup;
    }

    result = PLAYBACK_MP4_OK;

cleanup:
    tal_free(entries);
    return result;
}

static playback_mp4_result_t playback_mp4_parse_stbl(
    const playback_mp4_memory_box_t *box,
    playback_mp4_builder_t *builder
)
{
    size_t cursor = 0U;
    playback_mp4_memory_box_t child;
    playback_mp4_box_result_t box_result;

    if ((box == NULL) || (builder == NULL))
    {
        return PLAYBACK_MP4_INVALID_ARGUMENT;
    }

    while ((box_result = playback_mp4_next_memory_box(box->data, box->data_length, &cursor, &child)) ==
           PLAYBACK_MP4_BOX_OK)
    {
        bool stored = true;
        switch (child.type)
        {
            case MP4_BOX_STSD:
                stored = playback_mp4_box_view_set(&builder->stsd, &child);
                break;
            case MP4_BOX_STTS:
                stored = playback_mp4_box_view_set(&builder->stts, &child);
                break;
            case MP4_BOX_STSC:
                stored = playback_mp4_box_view_set(&builder->stsc, &child);
                break;
            case MP4_BOX_STSZ:
                stored = playback_mp4_box_view_set(&builder->stsz, &child);
                break;
            case MP4_BOX_STCO:
                if (builder->chunk_offsets.present)
                {
                    stored = false;
                }
                else
                {
                    stored = playback_mp4_box_view_set(&builder->chunk_offsets, &child);
                    builder->chunk_offsets_are_64_bit = false;
                }
                break;
            case MP4_BOX_CO64:
                if (builder->chunk_offsets.present)
                {
                    stored = false;
                }
                else
                {
                    stored = playback_mp4_box_view_set(&builder->chunk_offsets, &child);
                    builder->chunk_offsets_are_64_bit = true;
                }
                break;
            case MP4_BOX_STSS:
                stored = playback_mp4_box_view_set(&builder->stss, &child);
                break;
            case MP4_BOX_CTTS:
            case MP4_BOX_STZ2:
                return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
            default:
                break;
        }

        if (!stored)
        {
            return PLAYBACK_MP4_INVALID_CONTAINER;
        }
    }

    if (box_result != PLAYBACK_MP4_BOX_END)
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    return (builder->stsd.present && builder->stts.present && builder->stsc.present && builder->stsz.present &&
            builder->chunk_offsets.present)
               ? PLAYBACK_MP4_OK
               : PLAYBACK_MP4_INVALID_CONTAINER;
}

static playback_mp4_result_t playback_mp4_parse_minf(
    const playback_mp4_memory_box_t *box,
    playback_mp4_builder_t *builder
)
{
    size_t cursor = 0U;
    playback_mp4_memory_box_t child;
    playback_mp4_box_result_t box_result;
    bool stbl_found = false;

    if ((box == NULL) || (builder == NULL))
    {
        return PLAYBACK_MP4_INVALID_ARGUMENT;
    }

    while ((box_result = playback_mp4_next_memory_box(box->data, box->data_length, &cursor, &child)) ==
           PLAYBACK_MP4_BOX_OK)
    {
        if (child.type == MP4_BOX_STBL)
        {
            playback_mp4_result_t result;
            if (stbl_found)
            {
                return PLAYBACK_MP4_INVALID_CONTAINER;
            }
            result = playback_mp4_parse_stbl(&child, builder);
            if (result != PLAYBACK_MP4_OK)
            {
                return result;
            }
            stbl_found = true;
        }
    }

    return ((box_result == PLAYBACK_MP4_BOX_END) && stbl_found) ? PLAYBACK_MP4_OK : PLAYBACK_MP4_INVALID_CONTAINER;
}

static playback_mp4_result_t playback_mp4_parse_mdia(
    const playback_mp4_memory_box_t *box,
    playback_mp4_builder_t *builder
)
{
    size_t cursor = 0U;
    playback_mp4_memory_box_t child;
    playback_mp4_box_result_t box_result;
    bool minf_found = false;

    if ((box == NULL) || (builder == NULL))
    {
        return PLAYBACK_MP4_INVALID_ARGUMENT;
    }

    while ((box_result = playback_mp4_next_memory_box(box->data, box->data_length, &cursor, &child)) ==
           PLAYBACK_MP4_BOX_OK)
    {
        playback_mp4_result_t result = PLAYBACK_MP4_OK;
        if (child.type == MP4_BOX_MDHD)
        {
            result = playback_mp4_parse_mdhd(&child, builder);
        }
        else if (child.type == MP4_BOX_HDLR)
        {
            result = playback_mp4_parse_hdlr(&child, builder);
        }
        else if (child.type == MP4_BOX_MINF)
        {
            if (minf_found)
            {
                return PLAYBACK_MP4_INVALID_CONTAINER;
            }
            result = playback_mp4_parse_minf(&child, builder);
            minf_found = true;
        }

        if (result != PLAYBACK_MP4_OK)
        {
            return result;
        }
    }

    if ((box_result != PLAYBACK_MP4_BOX_END) || !builder->mdhd_found || !builder->hdlr_found || !minf_found)
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    return PLAYBACK_MP4_OK;
}

static playback_mp4_result_t playback_mp4_parse_trak(
    const playback_mp4_memory_box_t *box,
    playback_mp4_builder_t *builder
)
{
    size_t cursor = 0U;
    playback_mp4_memory_box_t child;
    playback_mp4_box_result_t box_result;
    bool mdia_found = false;

    if ((box == NULL) || (builder == NULL))
    {
        return PLAYBACK_MP4_INVALID_ARGUMENT;
    }

    while ((box_result = playback_mp4_next_memory_box(box->data, box->data_length, &cursor, &child)) ==
           PLAYBACK_MP4_BOX_OK)
    {
        playback_mp4_result_t result = PLAYBACK_MP4_OK;
        if (child.type == MP4_BOX_TKHD)
        {
            result = playback_mp4_parse_tkhd(&child, builder);
        }
        else if (child.type == MP4_BOX_EDTS)
        {
            result = playback_mp4_parse_edts(&child, builder);
        }
        else if (child.type == MP4_BOX_MDIA)
        {
            if (mdia_found)
            {
                return PLAYBACK_MP4_INVALID_CONTAINER;
            }
            result = playback_mp4_parse_mdia(&child, builder);
            mdia_found = true;
        }
        else if (child.type == MP4_BOX_TREF)
        {
            return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
        }

        if (result != PLAYBACK_MP4_OK)
        {
            return result;
        }
    }

    if ((box_result != PLAYBACK_MP4_BOX_END) || !builder->tkhd_found || !mdia_found)
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    return PLAYBACK_MP4_OK;
}

static playback_mp4_result_t playback_mp4_validate_timing(
    const playback_mp4_builder_t *builder,
    playback_mp4_state_t *state
)
{
    uint64_t media_limit;
    uint64_t movie_limit;
    uint64_t media_scaled;
    uint64_t movie_scaled;
    uint64_t duration_ms;

    if ((builder == NULL) || (state == NULL) || !builder->mvhd_found || !builder->tkhd_found ||
        !builder->mdhd_found || !builder->hdlr_found || (builder->handler_type != MP4_HANDLER_VIDEO) ||
        (state->codec.duration_ticks != builder->media_duration) ||
        (state->codec.width != builder->track_width) || (state->codec.height != builder->track_height))
    {
        return PLAYBACK_MP4_CONTENT_INVALID;
    }

    media_limit = (uint64_t)builder->media_timescale * PLAYBACK_MAX_DURATION_MS / 1000ULL;
    movie_limit = (uint64_t)builder->movie_timescale * PLAYBACK_MAX_DURATION_MS / 1000ULL;
    if ((builder->media_duration > media_limit) || (builder->movie_duration > movie_limit) ||
        (builder->track_duration != builder->movie_duration) ||
        (builder->edit_found && (builder->edit_segment_duration != builder->track_duration)))
    {
        return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
    }

    media_scaled = builder->media_duration * builder->movie_timescale;
    movie_scaled = builder->movie_duration * builder->media_timescale;
    if (media_scaled != movie_scaled)
    {
        return PLAYBACK_MP4_CONTENT_INVALID;
    }

    duration_ms = ((builder->media_duration * 1000ULL) + (builder->media_timescale / 2U)) /
                  builder->media_timescale;
    if ((duration_ms == 0U) || (duration_ms > PLAYBACK_MAX_DURATION_MS) || (duration_ms > UINT32_MAX))
    {
        return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
    }

    state->codec.timescale = builder->media_timescale;
    state->codec.duration_ms = (uint32_t)duration_ms;
    return PLAYBACK_MP4_OK;
}

static playback_mp4_result_t playback_mp4_parse_moov(
    const uint8_t *buffer,
    size_t buffer_size,
    size_t header_size,
    playback_mp4_state_t *state
)
{
    playback_mp4_builder_t builder;
    size_t cursor = 0U;
    playback_mp4_memory_box_t child;
    playback_mp4_box_result_t box_result;
    bool trak_found = false;
    playback_mp4_result_t result;

    if ((buffer == NULL) || (state == NULL) || (buffer_size < header_size) ||
        (playback_mp4_read_be32(buffer + 4U) != MP4_BOX_MOOV))
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    memset(&builder, 0, sizeof(builder));
    while ((box_result = playback_mp4_next_memory_box(
                buffer + header_size,
                buffer_size - header_size,
                &cursor,
                &child
            )) == PLAYBACK_MP4_BOX_OK)
    {
        if (child.type == MP4_BOX_MVHD)
        {
            result = playback_mp4_parse_mvhd(&child, &builder);
        }
        else if (child.type == MP4_BOX_TRAK)
        {
            if (trak_found)
            {
                return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
            }
            result = playback_mp4_parse_trak(&child, &builder);
            trak_found = true;
        }
        else if (child.type == MP4_BOX_MVEX)
        {
            return PLAYBACK_MP4_UNSUPPORTED_CONTAINER;
        }
        else
        {
            result = PLAYBACK_MP4_OK;
        }

        if (result != PLAYBACK_MP4_OK)
        {
            return result;
        }
    }

    if ((box_result != PLAYBACK_MP4_BOX_END) || !builder.mvhd_found || !trak_found)
    {
        return PLAYBACK_MP4_INVALID_CONTAINER;
    }

    result = playback_mp4_parse_stsd(&builder.stsd, state);
    if (result != PLAYBACK_MP4_OK)
    {
        return result;
    }
    result = playback_mp4_parse_stsz(&builder.stsz, state);
    if (result != PLAYBACK_MP4_OK)
    {
        return result;
    }
    result = playback_mp4_parse_stts(&builder.stts, state);
    if (result != PLAYBACK_MP4_OK)
    {
        return result;
    }
    result = playback_mp4_parse_stss(&builder.stss, state);
    if (result != PLAYBACK_MP4_OK)
    {
        return result;
    }
    result = playback_mp4_parse_chunk_offsets(
        &builder.stsc,
        &builder.chunk_offsets,
        builder.chunk_offsets_are_64_bit,
        state
    );
    if (result != PLAYBACK_MP4_OK)
    {
        return result;
    }

    return playback_mp4_validate_timing(&builder, state);
}

static void playback_mp4_state_release(playback_mp4_state_t *state)
{
    if (state == NULL)
    {
        return;
    }

    if (state->sample_scratch != NULL)
    {
        tal_free(state->sample_scratch);
    }
    if (state->samples != NULL)
    {
        tal_free(state->samples);
    }
    tal_free(state);
}

static playback_mp4_state_t *playback_mp4_get_state(const playback_mp4_demux_t *demux)
{
    return (demux == NULL) ? NULL : (playback_mp4_state_t *)demux->state;
}

playback_mp4_result_t playback_mp4_demux_open(
    playback_mp4_demux_t *demux,
    const playback_asset_t *asset,
    const playback_http_config_t *http_config
)
{
    playback_mp4_state_t *state;
    uint8_t *moov_buffer = NULL;
    size_t moov_size = 0U;
    size_t moov_header_size = 0U;
    playback_http_result_t http_result;
    playback_mp4_result_t result;

    if ((demux == NULL) || (asset == NULL) || (demux->state != NULL))
    {
        return PLAYBACK_MP4_INVALID_ARGUMENT;
    }

    state = tal_calloc(1U, sizeof(*state));
    if (state == NULL)
    {
        return PLAYBACK_MP4_NO_MEMORY;
    }

    http_result = playback_http_range_reader_init(&state->reader, asset, http_config);
    if (http_result != PLAYBACK_HTTP_OK)
    {
        playback_mp4_state_release(state);
        return playback_mp4_map_http_result(http_result);
    }
    http_result = playback_http_range_probe(&state->reader, &state->metadata);
    if (http_result != PLAYBACK_HTTP_OK)
    {
        playback_mp4_state_release(state);
        return playback_mp4_map_http_result(http_result);
    }

    result = playback_mp4_scan_file(state, &moov_buffer, &moov_size, &moov_header_size);
    if (result == PLAYBACK_MP4_OK)
    {
        result = playback_mp4_parse_moov(moov_buffer, moov_size, moov_header_size, state);
    }
    if (moov_buffer != NULL)
    {
        tal_free(moov_buffer);
    }
    if (result != PLAYBACK_MP4_OK)
    {
        playback_mp4_state_release(state);
        return result;
    }

    state->sample_scratch = tal_malloc(state->codec.max_sample_size);
    if (state->sample_scratch == NULL)
    {
        playback_mp4_state_release(state);
        return PLAYBACK_MP4_NO_MEMORY;
    }
    state->sample_scratch_capacity = state->codec.max_sample_size;
    demux->state = state;
    return PLAYBACK_MP4_OK;
}

playback_mp4_result_t playback_mp4_demux_open_memory(
    playback_mp4_demux_t *demux,
    const uint8_t *data,
    size_t length
)
{
    playback_mp4_state_t *state;
    uint8_t *moov_buffer = NULL;
    size_t moov_size = 0U;
    size_t moov_header_size = 0U;
    playback_mp4_result_t result;

    if ((demux == NULL) || (data == NULL) || (length == 0U) ||
        (length > UINT32_MAX) || (demux->state != NULL))
    {
        return PLAYBACK_MP4_INVALID_ARGUMENT;
    }

    state = tal_calloc(1U, sizeof(*state));
    if (state == NULL)
    {
        return PLAYBACK_MP4_NO_MEMORY;
    }

    state->memory_data = data;
    state->memory_length = length;
    state->memory_source = true;
    state->metadata.content_length = (uint32_t)length;
    state->metadata.accepts_byte_ranges = true;

    result = playback_mp4_scan_file(state, &moov_buffer, &moov_size, &moov_header_size);
    if (result == PLAYBACK_MP4_OK)
    {
        result = playback_mp4_parse_moov(moov_buffer, moov_size, moov_header_size, state);
    }
    if (moov_buffer != NULL)
    {
        tal_free(moov_buffer);
    }
    if (result != PLAYBACK_MP4_OK)
    {
        playback_mp4_state_release(state);
        return result;
    }

    state->sample_scratch = tal_malloc(state->codec.max_sample_size);
    if (state->sample_scratch == NULL)
    {
        playback_mp4_state_release(state);
        return PLAYBACK_MP4_NO_MEMORY;
    }
    state->sample_scratch_capacity = state->codec.max_sample_size;
    demux->state = state;
    return PLAYBACK_MP4_OK;
}

playback_mp4_result_t playback_mp4_demux_get_codec_config(
    const playback_mp4_demux_t *demux,
    playback_mp4_codec_config_t *config,
    uint8_t *parameter_sets_annex_b,
    size_t parameter_sets_capacity,
    size_t *parameter_sets_length
)
{
    const playback_mp4_state_t *state = playback_mp4_get_state(demux);

    if ((state == NULL) || (config == NULL) || (parameter_sets_length == NULL) ||
        ((parameter_sets_annex_b == NULL) && (parameter_sets_capacity != 0U)))
    {
        return PLAYBACK_MP4_INVALID_ARGUMENT;
    }

    *config = state->codec;
    *parameter_sets_length = state->parameter_sets_length;
    if (parameter_sets_capacity < state->parameter_sets_length)
    {
        return PLAYBACK_MP4_BUFFER_TOO_SMALL;
    }
    if (state->parameter_sets_length > 0U)
    {
        if (parameter_sets_annex_b == NULL)
        {
            return PLAYBACK_MP4_INVALID_ARGUMENT;
        }
        memcpy(parameter_sets_annex_b, state->parameter_sets, state->parameter_sets_length);
    }

    return PLAYBACK_MP4_OK;
}

playback_mp4_result_t playback_mp4_demux_get_sample(
    const playback_mp4_demux_t *demux,
    uint32_t sample_index,
    playback_mp4_sample_t *sample
)
{
    const playback_mp4_state_t *state = playback_mp4_get_state(demux);

    if ((state == NULL) || (sample == NULL))
    {
        return PLAYBACK_MP4_INVALID_ARGUMENT;
    }
    if (sample_index >= state->codec.sample_count)
    {
        return PLAYBACK_MP4_SAMPLE_OUT_OF_RANGE;
    }

    *sample = state->samples[sample_index];
    return PLAYBACK_MP4_OK;
}

playback_mp4_result_t playback_mp4_demux_find_sample_at_or_after(
    const playback_mp4_demux_t *demux,
    uint32_t position_ms,
    playback_mp4_sample_t *sample
)
{
    const playback_mp4_state_t *state = playback_mp4_get_state(demux);
    uint64_t target_ticks;
    uint32_t low;
    uint32_t high;

    if ((state == NULL) || (sample == NULL))
    {
        return PLAYBACK_MP4_INVALID_ARGUMENT;
    }
    if (position_ms >= state->codec.duration_ms)
    {
        return PLAYBACK_MP4_SAMPLE_OUT_OF_RANGE;
    }

    target_ticks = (((uint64_t)position_ms * state->codec.timescale) + 999ULL) / 1000ULL;
    low = 0U;
    high = state->codec.sample_count;
    while (low < high)
    {
        uint32_t middle = low + ((high - low) / 2U);
        if (state->samples[middle].pts < target_ticks)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }

    if (low >= state->codec.sample_count)
    {
        return PLAYBACK_MP4_SAMPLE_OUT_OF_RANGE;
    }
    *sample = state->samples[low];
    return PLAYBACK_MP4_OK;
}

playback_mp4_result_t playback_mp4_demux_find_sync_at_or_before(
    const playback_mp4_demux_t *demux,
    uint32_t position_ms,
    playback_mp4_sample_t *sample
)
{
    const playback_mp4_state_t *state = playback_mp4_get_state(demux);
    uint64_t target_ticks;
    uint32_t low;
    uint32_t high;

    if ((state == NULL) || (sample == NULL))
    {
        return PLAYBACK_MP4_INVALID_ARGUMENT;
    }
    if (position_ms > state->codec.duration_ms)
    {
        return PLAYBACK_MP4_SAMPLE_OUT_OF_RANGE;
    }

    target_ticks = ((uint64_t)position_ms * state->codec.timescale) / 1000ULL;
    low = 0U;
    high = state->codec.sample_count;
    while (low < high)
    {
        uint32_t middle = low + ((high - low) / 2U);
        if (state->samples[middle].pts <= target_ticks)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }

    while (low > 0U)
    {
        low--;
        if (state->samples[low].is_sync)
        {
            *sample = state->samples[low];
            return PLAYBACK_MP4_OK;
        }
    }

    return PLAYBACK_MP4_SAMPLE_OUT_OF_RANGE;
}

static uint32_t playback_mp4_read_nal_length(const uint8_t *data, uint8_t length_size)
{
    uint32_t value = 0U;
    uint8_t index;

    for (index = 0U; index < length_size; ++index)
    {
        value = (value << 8U) | data[index];
    }
    return value;
}

playback_mp4_result_t playback_mp4_demux_read_access_unit(
    playback_mp4_demux_t *demux,
    uint32_t sample_index,
    uint8_t *destination,
    size_t destination_capacity,
    size_t *access_unit_length
)
{
    playback_mp4_state_t *state = playback_mp4_get_state(demux);
    const playback_mp4_sample_t *sample;
    playback_mp4_result_t read_result;
    size_t cursor = 0U;
    size_t required = 0U;
    uint32_t nal_count = 0U;
    bool has_vcl = false;
    bool has_idr = false;

    if ((state == NULL) || (destination == NULL) || (destination_capacity == 0U) ||
        (access_unit_length == NULL))
    {
        return PLAYBACK_MP4_INVALID_ARGUMENT;
    }
    *access_unit_length = 0U;
    if (sample_index >= state->codec.sample_count)
    {
        return PLAYBACK_MP4_SAMPLE_OUT_OF_RANGE;
    }

    sample = &state->samples[sample_index];
    if (sample->byte_length > state->sample_scratch_capacity)
    {
        return PLAYBACK_MP4_CONTENT_INVALID;
    }

    read_result = playback_mp4_read_at(
        state,
        sample->byte_offset,
        sample->byte_length,
        state->sample_scratch,
        state->sample_scratch_capacity
    );
    if (read_result != PLAYBACK_MP4_OK)
    {
        return read_result;
    }

    while (cursor < sample->byte_length)
    {
        uint32_t nal_length;
        uint8_t nal_type;

        if ((sample->byte_length - cursor < state->codec.nal_length_size) ||
            (nal_count >= PLAYBACK_MP4_MAX_NALS_PER_ACCESS_UNIT))
        {
            return PLAYBACK_MP4_CONTENT_INVALID;
        }
        nal_length = playback_mp4_read_nal_length(state->sample_scratch + cursor, state->codec.nal_length_size);
        cursor += state->codec.nal_length_size;
        if ((nal_length == 0U) || (nal_length > sample->byte_length - cursor) ||
            !playback_mp4_size_add(required, 4U + nal_length, &required))
        {
            return PLAYBACK_MP4_CONTENT_INVALID;
        }

        nal_type = (uint8_t)(state->sample_scratch[cursor] & 0x1FU);
        if ((nal_type == 0U) || (nal_type >= 24U))
        {
            return PLAYBACK_MP4_CONTENT_INVALID;
        }
        has_vcl = has_vcl || (nal_type == MP4_NAL_TYPE_NON_IDR) || (nal_type == MP4_NAL_TYPE_IDR);
        has_idr = has_idr || (nal_type == MP4_NAL_TYPE_IDR);
        cursor += nal_length;
        nal_count++;
    }

    if (!has_vcl || (sample->is_sync != has_idr))
    {
        return PLAYBACK_MP4_CONTENT_INVALID;
    }

    *access_unit_length = required;
    if (destination_capacity < required)
    {
        return PLAYBACK_MP4_BUFFER_TOO_SMALL;
    }

    cursor = 0U;
    required = 0U;
    while (cursor < sample->byte_length)
    {
        uint32_t nal_length = playback_mp4_read_nal_length(
            state->sample_scratch + cursor,
            state->codec.nal_length_size
        );
        cursor += state->codec.nal_length_size;
        destination[required] = 0U;
        destination[required + 1U] = 0U;
        destination[required + 2U] = 0U;
        destination[required + 3U] = 1U;
        memcpy(destination + required + 4U, state->sample_scratch + cursor, nal_length);
        required += 4U + nal_length;
        cursor += nal_length;
    }

    return PLAYBACK_MP4_OK;
}

void playback_mp4_demux_cancel(playback_mp4_demux_t *demux)
{
    playback_mp4_state_t *state = playback_mp4_get_state(demux);
    if ((state != NULL) && !state->memory_source)
    {
        playback_http_range_cancel(&state->reader);
    }
}

void playback_mp4_demux_close(playback_mp4_demux_t *demux)
{
    playback_mp4_state_t *state;

    if (demux == NULL)
    {
        return;
    }

    state = playback_mp4_get_state(demux);
    demux->state = NULL;
    playback_mp4_state_release(state);
}

playback_error_t playback_mp4_result_to_error(playback_mp4_result_t result)
{
    switch (result)
    {
        case PLAYBACK_MP4_OK:
        case PLAYBACK_MP4_CANCELLED:
            return PLAYBACK_ERROR_NONE;
        case PLAYBACK_MP4_NETWORK_ERROR:
            return PLAYBACK_ERROR_NETWORK_TIMEOUT;
        case PLAYBACK_MP4_RANGE_INVALID:
            return PLAYBACK_ERROR_NETWORK_RANGE_INVALID;
        case PLAYBACK_MP4_RESOURCE_MISMATCH:
        case PLAYBACK_MP4_CONTENT_INVALID:
            return PLAYBACK_ERROR_CONTENT_INVALID;
        case PLAYBACK_MP4_UNSUPPORTED_CONTAINER:
        case PLAYBACK_MP4_INVALID_CONTAINER:
            return PLAYBACK_ERROR_MP4_DEMUX_FAILED;
        case PLAYBACK_MP4_INVALID_ARGUMENT:
        case PLAYBACK_MP4_NO_MEMORY:
        case PLAYBACK_MP4_SAMPLE_OUT_OF_RANGE:
        case PLAYBACK_MP4_BUFFER_TOO_SMALL:
        default:
            return PLAYBACK_ERROR_INTERNAL;
    }
}

const char *playback_mp4_result_name(playback_mp4_result_t result)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "CANCELLED",
        "NETWORK_ERROR",
        "RANGE_INVALID",
        "RESOURCE_MISMATCH",
        "UNSUPPORTED_CONTAINER",
        "INVALID_CONTAINER",
        "CONTENT_INVALID",
        "NO_MEMORY",
        "SAMPLE_OUT_OF_RANGE",
        "BUFFER_TOO_SMALL",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0]))) ? names[result] : "UNKNOWN_MP4_RESULT";
}
