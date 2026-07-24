/**
 * @file playback_video_test.c
 * @brief Blocking H.264 MP4 hardware-acceptance path.
 */

#include "playback_video_test.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "http_session.h"
#include "playback_h264_decoder.h"
#include "playback_media_package.h"
#include "playback_mp4_demux.h"
#include "tal_api.h"

#define PLAYBACK_VIDEO_TEST_READ_BYTES (8U * 1024U)
#define PLAYBACK_VIDEO_TEST_SEGMENT_BYTES (128U * 1024U)
#define PLAYBACK_VIDEO_TEST_MAX_BYTES (4U * 1024U * 1024U)
#define PLAYBACK_VIDEO_TEST_MAX_CONSECUTIVE_FAILURES (10U)
#define PLAYBACK_VIDEO_TEST_RETRY_DELAY_MS (2000U)
#define PLAYBACK_VIDEO_TEST_PROGRESS_BYTES (256U * 1024U)
#define PLAYBACK_VIDEO_TEST_ACCESS_UNIT_OVERHEAD \
    (PLAYBACK_MP4_MAX_NALS_PER_ACCESS_UNIT * 4U)

typedef struct
{
    uint8_t *data;
    uint32_t length;
} playback_video_test_file_t;

static uint32_t playback_video_test_gcd(uint32_t left, uint32_t right)
{
    while (right != 0U)
    {
        const uint32_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

static void playback_video_test_close_http(
    http_session_t *session,
    http_resp_t **response
)
{
    if ((response != NULL) && (*response != NULL))
    {
        (void)http_free_response_hdr(response);
    }
    if ((session != NULL) && (*session != NULL))
    {
        (void)http_close_session(session);
    }
}

static playback_video_test_result_t playback_video_test_download(
    const char *url,
    playback_video_test_file_t *file
)
{
    uint32_t downloaded = 0U;
    uint32_t next_progress = PLAYBACK_VIDEO_TEST_PROGRESS_BYTES;
    uint32_t consecutive_failures = 0U;

    if ((url == NULL) || (url[0] == '\0') || (file == NULL))
    {
        return PLAYBACK_VIDEO_TEST_INVALID_ARGUMENT;
    }
    memset(file, 0, sizeof(*file));

    while (((file->length == 0U) || (downloaded < file->length)) &&
           (consecutive_failures < PLAYBACK_VIDEO_TEST_MAX_CONSECUTIVE_FAILURES))
    {
        http_session_t session = NULL;
        http_resp_t *response = NULL;
        http_req_t request;
        http_custom_header_t request_headers[1U];
        OPERATE_RET operation_result;
        uint32_t segment_length = PLAYBACK_VIDEO_TEST_SEGMENT_BYTES;
        uint32_t segment_received = 0U;
        bool request_failed = false;

        if (file->length > 0U)
        {
            const uint32_t remaining = file->length - downloaded;
            if (segment_length > remaining)
            {
                segment_length = remaining;
            }
        }

        PR_NOTICE(
            "VIDEO: HTTP segment offset=%u length=%u failure=%u",
            (unsigned int)downloaded,
            (unsigned int)segment_length,
            (unsigned int)consecutive_failures
        );
        operation_result = http_open_session(&session, url, 30000U);
        if (operation_result != OPRT_OK)
        {
            PR_WARN("VIDEO: HTTP session open failed: %d", operation_result);
            request_failed = true;
            goto segment_done;
        }

        memset(&request, 0, sizeof(request));
        request_headers[0U].key = "Accept-Encoding";
        request_headers[0U].value = "identity";
        request.type = HTTP_GET;
        request.version = HTTP_VER_1_1;
        request.custom_headers = request_headers;
        request.custom_headers_count = 1;
        if (file->length > 0U)
        {
            request.download_offset = downloaded;
            request.download_size = segment_length;
        }

        operation_result = http_send_request(
            session,
            &request,
            HTTP_REQUEST_KEEP_ALIVE_FLAG
        );
        if (operation_result != OPRT_OK)
        {
            PR_WARN("VIDEO: HTTP GET failed: %d", operation_result);
            request_failed = true;
            goto segment_done;
        }

        operation_result = http_get_response_hdr(session, &response);
        if ((operation_result != OPRT_OK) || (response == NULL))
        {
            PR_WARN("VIDEO: response header failed: %d", operation_result);
            request_failed = true;
            goto segment_done;
        }

        if (file->length == 0U)
        {
            if ((response->status_code != 200) ||
                (response->content_length == 0U) ||
                (response->content_length > PLAYBACK_VIDEO_TEST_MAX_BYTES))
            {
                PR_ERR(
                    "VIDEO: invalid initial response status=%d length=%u",
                    response->status_code,
                    response->content_length
                );
                request_failed = true;
                goto segment_done;
            }
            file->length = response->content_length;
            file->data = tal_psram_malloc(file->length);
            if (file->data == NULL)
            {
                playback_video_test_close_http(&session, &response);
                return PLAYBACK_VIDEO_TEST_NO_MEMORY;
            }
            if (segment_length > file->length)
            {
                segment_length = file->length;
            }
            PR_NOTICE(
                "VIDEO: download size=%u segment=%u read=%u",
                (unsigned int)file->length,
                (unsigned int)PLAYBACK_VIDEO_TEST_SEGMENT_BYTES,
                (unsigned int)PLAYBACK_VIDEO_TEST_READ_BYTES
            );
        }
        else if ((response->status_code != 206) ||
                 (response->content_length != segment_length))
        {
            PR_WARN(
                "VIDEO: invalid segment response status=%d length=%u expected=%u",
                response->status_code,
                response->content_length,
                (unsigned int)segment_length
            );
            request_failed = true;
            goto segment_done;
        }

        while (segment_received < segment_length)
        {
            const uint32_t remaining = segment_length - segment_received;
            const uint32_t requested =
                (remaining > PLAYBACK_VIDEO_TEST_READ_BYTES)
                    ? PLAYBACK_VIDEO_TEST_READ_BYTES
                    : remaining;
            const int read_size = http_read_content(
                session,
                file->data + downloaded + segment_received,
                requested
            );

            if (read_size <= 0)
            {
                PR_WARN(
                    "VIDEO: segment interrupted at %u/%u: read=%d",
                    (unsigned int)(downloaded + segment_received),
                    (unsigned int)file->length,
                    read_size
                );
                request_failed = true;
                break;
            }
            segment_received += (uint32_t)read_size;
        }

segment_done:
        downloaded += segment_received;
        playback_video_test_close_http(&session, &response);

        while ((file->length > 0U) && (downloaded >= next_progress))
        {
            PR_NOTICE(
                "VIDEO: downloaded %u/%u",
                (unsigned int)downloaded,
                (unsigned int)file->length
            );
            next_progress += PLAYBACK_VIDEO_TEST_PROGRESS_BYTES;
        }

        if ((file->length > 0U) && (downloaded == file->length))
        {
            PR_NOTICE("VIDEO: download complete (%u bytes)", (unsigned int)downloaded);
            return PLAYBACK_VIDEO_TEST_OK;
        }

        if (request_failed)
        {
            if (segment_received > 0U)
            {
                consecutive_failures = 0U;
            }
            else
            {
                consecutive_failures++;
            }
            if (consecutive_failures < PLAYBACK_VIDEO_TEST_MAX_CONSECUTIVE_FAILURES)
            {
                tal_system_sleep(PLAYBACK_VIDEO_TEST_RETRY_DELAY_MS);
            }
        }
        else
        {
            consecutive_failures = 0U;
        }
    }

    if (file->data != NULL)
    {
        tal_psram_free(file->data);
        memset(file, 0, sizeof(*file));
    }
    return PLAYBACK_VIDEO_TEST_NETWORK_ERROR;
}

static bool playback_video_test_build_descriptor(
    playback_mp4_demux_t *demux,
    const playback_mp4_codec_config_t *codec,
    playback_video_descriptor_t *descriptor
)
{
    playback_mp4_sample_t sample;
    uint32_t divisor;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t index;
    uint64_t previous_sync_pts = 0U;
    uint32_t max_keyframe_interval_ms = 0U;
    bool has_previous_sync = false;

    if ((demux == NULL) || (codec == NULL) || (descriptor == NULL) ||
        (codec->sample_count == 0U) || (codec->timescale == 0U))
    {
        return false;
    }
    if (playback_mp4_demux_get_sample(demux, 0U, &sample) != PLAYBACK_MP4_OK)
    {
        return false;
    }

    divisor = playback_video_test_gcd(codec->timescale, sample.duration);
    fps_num = codec->timescale / divisor;
    fps_den = sample.duration / divisor;
    if ((fps_num == 0U) || (fps_num > UINT16_MAX) ||
        (fps_den == 0U) || (fps_den > UINT16_MAX))
    {
        return false;
    }

    for (index = 0U; index < codec->sample_count; ++index)
    {
        uint64_t interval_ms;
        if (playback_mp4_demux_get_sample(demux, index, &sample) != PLAYBACK_MP4_OK)
        {
            return false;
        }
        if (!sample.is_sync)
        {
            continue;
        }
        if (has_previous_sync)
        {
            interval_ms = ((sample.pts - previous_sync_pts) * 1000ULL) /
                          codec->timescale;
            if (interval_ms > UINT16_MAX)
            {
                return false;
            }
            if ((uint32_t)interval_ms > max_keyframe_interval_ms)
            {
                max_keyframe_interval_ms = (uint32_t)interval_ms;
            }
        }
        previous_sync_pts = sample.pts;
        has_previous_sync = true;
    }

    if (!has_previous_sync)
    {
        return false;
    }
    if (max_keyframe_interval_ms == 0U)
    {
        max_keyframe_interval_ms = PLAYBACK_H264_MAX_KEYFRAME_INTERVAL_MS;
    }

    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->width = codec->width;
    descriptor->height = codec->height;
    descriptor->fps_num = (uint16_t)fps_num;
    descriptor->fps_den = (uint16_t)fps_den;
    descriptor->max_keyframe_interval_ms =
        (uint16_t)max_keyframe_interval_ms;
    descriptor->h264_profile_idc = codec->profile_idc;
    descriptor->h264_level_idc = codec->level_idc;
    descriptor->yuv420p = true;
    descriptor->has_b_frames = false;
    return true;
}

playback_video_test_result_t playback_video_test_run(
    const char *url,
    playback_video_sink_present_fn present,
    void *present_context
)
{
    playback_video_test_file_t file;
    playback_mp4_demux_t demux = {0};
    playback_h264_decoder_t decoder = {0};
    playback_video_output_t output = {0};
    playback_mp4_codec_config_t codec;
    playback_video_descriptor_t descriptor;
    playback_video_output_config_t output_config;
    playback_mp4_sample_t sample;
    playback_h264_frame_t frame;
    playback_video_test_result_t test_result;
    playback_mp4_result_t mp4_result;
    playback_h264_result_t h264_result;
    playback_video_output_result_t output_result;
    uint8_t parameter_sets[PLAYBACK_MP4_MAX_CODEC_CONFIG_BYTES];
    size_t parameter_sets_length = 0U;
    uint8_t *access_unit = NULL;
    size_t access_unit_capacity;
    uint32_t sample_index;

    if ((url == NULL) || (url[0] == '\0') || (present == NULL))
    {
        return PLAYBACK_VIDEO_TEST_INVALID_ARGUMENT;
    }

    test_result = playback_video_test_download(url, &file);
    if (test_result != PLAYBACK_VIDEO_TEST_OK)
    {
        return test_result;
    }

    PR_NOTICE("VIDEO: opening in-memory MP4 demux");
    mp4_result = playback_mp4_demux_open_memory(&demux, file.data, file.length);
    if (mp4_result != PLAYBACK_MP4_OK)
    {
        PR_ERR("VIDEO: MP4 open failed: %s", playback_mp4_result_name(mp4_result));
        test_result = PLAYBACK_VIDEO_TEST_MP4_ERROR;
        goto cleanup;
    }

    memset(&codec, 0, sizeof(codec));
    mp4_result = playback_mp4_demux_get_codec_config(
        &demux,
        &codec,
        parameter_sets,
        sizeof(parameter_sets),
        &parameter_sets_length
    );
    if (mp4_result != PLAYBACK_MP4_OK)
    {
        PR_ERR("VIDEO: codec config failed: %s", playback_mp4_result_name(mp4_result));
        test_result = PLAYBACK_VIDEO_TEST_MP4_ERROR;
        goto cleanup;
    }
    if (!playback_video_test_build_descriptor(&demux, &codec, &descriptor))
    {
        PR_ERR("VIDEO: unable to build supported descriptor");
        test_result = PLAYBACK_VIDEO_TEST_UNSUPPORTED_VIDEO;
        goto cleanup;
    }

    PR_NOTICE(
        "VIDEO: codec %ux%u fps=%u/%u profile=%u level=%u frames=%u max_sample=%u",
        descriptor.width,
        descriptor.height,
        descriptor.fps_num,
        descriptor.fps_den,
        descriptor.h264_profile_idc,
        descriptor.h264_level_idc,
        (unsigned int)codec.sample_count,
        (unsigned int)codec.max_sample_size
    );

    h264_result = playback_h264_decoder_open(
        &decoder,
        &descriptor,
        &codec,
        parameter_sets,
        parameter_sets_length
    );
    if (h264_result != PLAYBACK_H264_OK)
    {
        PR_ERR("VIDEO: decoder open failed: %s", playback_h264_result_name(h264_result));
        test_result = PLAYBACK_VIDEO_TEST_UNSUPPORTED_VIDEO;
        goto cleanup;
    }

    memset(&output_config, 0, sizeof(output_config));
    output_config.source_width = descriptor.width;
    output_config.source_height = descriptor.height;
    output_config.rotation = PLAYBACK_VIDEO_ROTATION_0;
    output_config.swap_rgb565_bytes = false;
    output_result = playback_video_output_open(
        &output,
        &output_config,
        present,
        present_context
    );
    if (output_result != PLAYBACK_VIDEO_OUTPUT_OK)
    {
        PR_ERR("VIDEO: output open failed: %s", playback_video_output_result_name(output_result));
        test_result = PLAYBACK_VIDEO_TEST_OUTPUT_ERROR;
        goto cleanup;
    }

    if (codec.max_sample_size > SIZE_MAX - PLAYBACK_VIDEO_TEST_ACCESS_UNIT_OVERHEAD)
    {
        test_result = PLAYBACK_VIDEO_TEST_NO_MEMORY;
        goto cleanup;
    }
    access_unit_capacity =
        (size_t)codec.max_sample_size + PLAYBACK_VIDEO_TEST_ACCESS_UNIT_OVERHEAD;
    access_unit = tal_psram_malloc(access_unit_capacity);
    if (access_unit == NULL)
    {
        test_result = PLAYBACK_VIDEO_TEST_NO_MEMORY;
        goto cleanup;
    }

    PR_NOTICE("VIDEO: playback started");
    for (sample_index = 0U; sample_index < codec.sample_count; ++sample_index)
    {
        size_t access_unit_length = 0U;
        uint32_t frame_delay_ms;

        mp4_result = playback_mp4_demux_get_sample(&demux, sample_index, &sample);
        if (mp4_result != PLAYBACK_MP4_OK)
        {
            PR_ERR("VIDEO: sample %u metadata failed", (unsigned int)sample_index);
            test_result = PLAYBACK_VIDEO_TEST_MP4_ERROR;
            goto cleanup;
        }
        mp4_result = playback_mp4_demux_read_access_unit(
            &demux,
            sample_index,
            access_unit,
            access_unit_capacity,
            &access_unit_length
        );
        if (mp4_result != PLAYBACK_MP4_OK)
        {
            PR_ERR(
                "VIDEO: sample %u read failed: %s",
                (unsigned int)sample_index,
                playback_mp4_result_name(mp4_result)
            );
            test_result = PLAYBACK_VIDEO_TEST_MP4_ERROR;
            goto cleanup;
        }

        h264_result = playback_h264_decoder_submit_access_unit(
            &decoder,
            access_unit,
            access_unit_length,
            sample.pts,
            sample.duration,
            sample.is_sync
        );
        if (h264_result != PLAYBACK_H264_OK)
        {
            PR_ERR(
                "VIDEO: sample %u decode failed: %s",
                (unsigned int)sample_index,
                playback_h264_result_name(h264_result)
            );
            test_result = PLAYBACK_VIDEO_TEST_DECODE_ERROR;
            goto cleanup;
        }

        h264_result = playback_h264_decoder_poll_frame(&decoder, &frame);
        if (h264_result != PLAYBACK_H264_OK)
        {
            PR_ERR(
                "VIDEO: frame %u poll failed: %s",
                (unsigned int)sample_index,
                playback_h264_result_name(h264_result)
            );
            test_result = PLAYBACK_VIDEO_TEST_DECODE_ERROR;
            goto cleanup;
        }

        output_result = playback_video_output_present(&output, &frame);
        if (output_result != PLAYBACK_VIDEO_OUTPUT_OK)
        {
            PR_ERR(
                "VIDEO: frame %u output failed: %s",
                (unsigned int)sample_index,
                playback_video_output_result_name(output_result)
            );
            (void)playback_h264_decoder_release_frame(&decoder, &frame);
            test_result = PLAYBACK_VIDEO_TEST_OUTPUT_ERROR;
            goto cleanup;
        }
        h264_result = playback_h264_decoder_release_frame(&decoder, &frame);
        if (h264_result != PLAYBACK_H264_OK)
        {
            test_result = PLAYBACK_VIDEO_TEST_DECODE_ERROR;
            goto cleanup;
        }

        if (((sample_index + 1U) % 30U) == 0U)
        {
            PR_NOTICE(
                "VIDEO: presented %u/%u frames",
                (unsigned int)(sample_index + 1U),
                (unsigned int)codec.sample_count
            );
        }

        frame_delay_ms = (uint32_t)(
            (((uint64_t)sample.duration * 1000ULL) + (codec.timescale / 2U)) /
            codec.timescale
        );
        if (frame_delay_ms > 0U)
        {
            tal_system_sleep(frame_delay_ms);
        }
    }

    PR_NOTICE("VIDEO: playback complete (%u frames)", (unsigned int)codec.sample_count);
    test_result = PLAYBACK_VIDEO_TEST_OK;

cleanup:
    if (access_unit != NULL)
    {
        tal_psram_free(access_unit);
    }
    playback_video_output_close(&output);
    playback_h264_decoder_close(&decoder);
    playback_mp4_demux_close(&demux);
    if (file.data != NULL)
    {
        tal_psram_free(file.data);
    }
    return test_result;
}

const char *playback_video_test_result_name(playback_video_test_result_t result)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "NETWORK_ERROR",
        "NO_MEMORY",
        "MP4_ERROR",
        "UNSUPPORTED_VIDEO",
        "DECODE_ERROR",
        "OUTPUT_ERROR",
    };

    if ((unsigned int)result >= (sizeof(names) / sizeof(names[0])))
    {
        return "UNKNOWN";
    }
    return names[result];
}
