/**
 * @file playback_http_download.c
 * @brief Resumable complete-resource downloader for PSRAM media staging.
 */

#include "playback_http_download.h"

#include <stdbool.h>
#include <string.h>

#include "http_session.h"
#include "tal_api.h"

#define PLAYBACK_HTTP_DOWNLOAD_READ_BYTES (8U * 1024U)
#define PLAYBACK_HTTP_DOWNLOAD_SEGMENT_BYTES (128U * 1024U)
#define PLAYBACK_HTTP_DOWNLOAD_MAX_CONSECUTIVE_FAILURES (10U)
#define PLAYBACK_HTTP_DOWNLOAD_RETRY_DELAY_MS (2000U)
#define PLAYBACK_HTTP_DOWNLOAD_PROGRESS_BYTES (256U * 1024U)

static void playback_http_download_close_http(
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

playback_http_download_result_t playback_http_download_file(
    const char *url,
    const char *authorization,
    uint32_t expected_length,
    const char *log_name,
    playback_http_download_file_t *file
)
{
    uint32_t downloaded = 0U;
    uint32_t next_progress = PLAYBACK_HTTP_DOWNLOAD_PROGRESS_BYTES;
    uint32_t consecutive_failures = 0U;
    const char *name = ((log_name != NULL) && (log_name[0] != '\0'))
                           ? log_name
                           : "MEDIA";

    if ((url == NULL) || (url[0] == '\0') || (expected_length == 0U) ||
        (file == NULL))
    {
        return PLAYBACK_HTTP_DOWNLOAD_INVALID_ARGUMENT;
    }
    memset(file, 0, sizeof(*file));
    file->data = tal_psram_malloc(expected_length);
    if (file->data == NULL)
    {
        return PLAYBACK_HTTP_DOWNLOAD_NO_MEMORY;
    }
    file->length = expected_length;

    while ((downloaded < expected_length) &&
           (consecutive_failures < PLAYBACK_HTTP_DOWNLOAD_MAX_CONSECUTIVE_FAILURES))
    {
        http_session_t session = NULL;
        http_resp_t *response = NULL;
        http_req_t request;
        http_custom_header_t request_headers[2U];
        uint8_t header_count = 0U;
        OPERATE_RET operation_result;
        const uint32_t remaining = expected_length - downloaded;
        const uint32_t segment_length =
            (downloaded == 0U)
                ? remaining
                : ((remaining > PLAYBACK_HTTP_DOWNLOAD_SEGMENT_BYTES)
                       ? PLAYBACK_HTTP_DOWNLOAD_SEGMENT_BYTES
                       : remaining);
        uint32_t segment_received = 0U;
        bool request_failed = false;

        PR_NOTICE(
            "AV: %s segment offset=%u length=%u failure=%u",
            name,
            (unsigned int)downloaded,
            (unsigned int)segment_length,
            (unsigned int)consecutive_failures
        );
        operation_result = http_open_session(&session, url, 30000U);
        if (operation_result != OPRT_OK)
        {
            PR_WARN("AV: %s session open failed: %d", name, operation_result);
            request_failed = true;
            goto segment_done;
        }

        memset(&request, 0, sizeof(request));
        request_headers[header_count].key = "Accept-Encoding";
        request_headers[header_count].value = "identity";
        ++header_count;
        if ((authorization != NULL) && (authorization[0] != '\0'))
        {
            request_headers[header_count].key = "Authorization";
            request_headers[header_count].value = authorization;
            ++header_count;
        }
        request.type = HTTP_GET;
        request.version = HTTP_VER_1_1;
        request.custom_headers = request_headers;
        request.custom_headers_count = header_count;
        if (downloaded > 0U)
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
            PR_WARN("AV: %s GET failed: %d", name, operation_result);
            request_failed = true;
            goto segment_done;
        }

        operation_result = http_get_response_hdr(session, &response);
        if ((operation_result != OPRT_OK) || (response == NULL))
        {
            PR_WARN("AV: %s response header failed: %d", name, operation_result);
            request_failed = true;
            goto segment_done;
        }

        if (downloaded == 0U)
        {
            if ((response->status_code != 200) ||
                (response->content_length != expected_length))
            {
                PR_ERR(
                    "AV: %s initial response mismatch status=%d length=%u expected=%u",
                    name,
                    response->status_code,
                    response->content_length,
                    (unsigned int)expected_length
                );
                playback_http_download_close_http(&session, &response);
                playback_http_download_release(file);
                return PLAYBACK_HTTP_DOWNLOAD_RESOURCE_MISMATCH;
            }
        }
        else if ((response->status_code != 206) ||
                 (response->content_length != segment_length))
        {
            PR_WARN(
                "AV: %s segment response mismatch status=%d length=%u expected=%u",
                name,
                response->status_code,
                response->content_length,
                (unsigned int)segment_length
            );
            request_failed = true;
            goto segment_done;
        }

        while (segment_received < segment_length)
        {
            const uint32_t segment_remaining = segment_length - segment_received;
            const uint32_t requested =
                (segment_remaining > PLAYBACK_HTTP_DOWNLOAD_READ_BYTES)
                    ? PLAYBACK_HTTP_DOWNLOAD_READ_BYTES
                    : segment_remaining;
            const int read_size = http_read_content(
                session,
                file->data + downloaded + segment_received,
                requested
            );

            if (read_size <= 0)
            {
                PR_WARN(
                    "AV: %s interrupted at %u/%u: read=%d",
                    name,
                    (unsigned int)(downloaded + segment_received),
                    (unsigned int)expected_length,
                    read_size
                );
                request_failed = true;
                break;
            }
            segment_received += (uint32_t)read_size;
        }

segment_done:
        downloaded += segment_received;
        playback_http_download_close_http(&session, &response);

        if ((downloaded < expected_length) && (downloaded >= next_progress))
        {
            PR_NOTICE(
                "AV: %s downloaded %u/%u",
                name,
                (unsigned int)downloaded,
                (unsigned int)expected_length
            );
            next_progress = downloaded + PLAYBACK_HTTP_DOWNLOAD_PROGRESS_BYTES;
        }
        if (downloaded == expected_length)
        {
            PR_NOTICE(
                "AV: %s download complete (%u bytes)",
                name,
                (unsigned int)expected_length
            );
            return PLAYBACK_HTTP_DOWNLOAD_OK;
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
            if (consecutive_failures <
                PLAYBACK_HTTP_DOWNLOAD_MAX_CONSECUTIVE_FAILURES)
            {
                tal_system_sleep(PLAYBACK_HTTP_DOWNLOAD_RETRY_DELAY_MS);
            }
        }
        else
        {
            consecutive_failures = 0U;
        }
    }

    playback_http_download_release(file);
    return PLAYBACK_HTTP_DOWNLOAD_NETWORK_ERROR;
}

void playback_http_download_release(playback_http_download_file_t *file)
{
    if (file == NULL)
    {
        return;
    }
    if (file->data != NULL)
    {
        tal_psram_free(file->data);
    }
    memset(file, 0, sizeof(*file));
}

const char *playback_http_download_result_name(
    playback_http_download_result_t result
)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "NO_MEMORY",
        "NETWORK_ERROR",
        "RESOURCE_MISMATCH",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0])))
               ? names[result]
               : "UNKNOWN";
}
