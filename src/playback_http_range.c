/**
 * @file playback_http_range.c
 * @brief Immutable HTTP HEAD and single-range reader for Playback media.
 */

#include "playback_http_range.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_http_client.h"
#include "http_client_interface.h"
#include "tal_api.h"

#define PLAYBACK_HTTP_RETRY_COUNT (3U)
#define PLAYBACK_HTTP_HEADER_COUNT_MAX (4U)

typedef struct
{
    char host[PLAYBACK_HTTP_HOST_MAX_LEN + 1U];
    char path[PLAYBACK_HTTP_PATH_MAX_LEN + 1U];
    uint16_t port;
    bool tls;
} playback_parsed_url_t;

typedef struct
{
    uint32_t start;
    uint32_t end;
    uint32_t total;
} playback_content_range_t;

static bool playback_ascii_equal_ignore_case(const char *left, size_t left_length, const char *right)
{
    size_t index;
    const size_t right_length = strlen(right);

    if (left_length != right_length)
    {
        return false;
    }

    for (index = 0U; index < left_length; ++index)
    {
        char left_value = left[index];
        char right_value = right[index];
        if ((left_value >= 'A') && (left_value <= 'Z'))
        {
            left_value = (char)(left_value - 'A' + 'a');
        }
        if ((right_value >= 'A') && (right_value <= 'Z'))
        {
            right_value = (char)(right_value - 'A' + 'a');
        }
        if (left_value != right_value)
        {
            return false;
        }
    }

    return true;
}

static playback_http_result_t playback_parse_url(const char *url, playback_parsed_url_t *parsed)
{
    const char *authority;
    const char *path;
    const char *port_separator;
    size_t authority_length;
    size_t host_length;
    size_t path_length;
    unsigned long parsed_port;
    char port_text[6U];
    size_t port_length;

    if ((url == NULL) || (parsed == NULL))
    {
        return PLAYBACK_HTTP_INVALID_ARGUMENT;
    }

    memset(parsed, 0, sizeof(*parsed));
    if (strncmp(url, "http://", 7U) == 0)
    {
        parsed->tls = false;
        parsed->port = 80U;
        authority = url + 7U;
    }
    else if (strncmp(url, "https://", 8U) == 0)
    {
        parsed->tls = true;
        parsed->port = 443U;
        authority = url + 8U;
    }
    else
    {
        return PLAYBACK_HTTP_INVALID_URL;
    }

    path = strchr(authority, '/');
    authority_length = (path == NULL) ? strlen(authority) : (size_t)(path - authority);
    if ((authority_length == 0U) || (authority_length > PLAYBACK_HTTP_HOST_MAX_LEN))
    {
        return PLAYBACK_HTTP_INVALID_URL;
    }

    port_separator = memchr(authority, ':', authority_length);
    host_length = (port_separator == NULL) ? authority_length : (size_t)(port_separator - authority);
    if ((host_length == 0U) || (host_length > PLAYBACK_HTTP_HOST_MAX_LEN))
    {
        return PLAYBACK_HTTP_INVALID_URL;
    }

    memcpy(parsed->host, authority, host_length);
    parsed->host[host_length] = '\0';

    if (port_separator != NULL)
    {
        port_length = authority_length - host_length - 1U;
        if ((port_length == 0U) || (port_length >= sizeof(port_text)))
        {
            return PLAYBACK_HTTP_INVALID_URL;
        }
        memcpy(port_text, port_separator + 1, port_length);
        port_text[port_length] = '\0';
        parsed_port = strtoul(port_text, NULL, 10);
        if ((parsed_port == 0UL) || (parsed_port > 65535UL))
        {
            return PLAYBACK_HTTP_INVALID_URL;
        }
        parsed->port = (uint16_t)parsed_port;
    }

    if (path == NULL)
    {
        strcpy(parsed->path, "/");
    }
    else
    {
        path_length = strlen(path);
        if ((path_length == 0U) || (path_length > PLAYBACK_HTTP_PATH_MAX_LEN))
        {
            return PLAYBACK_HTTP_INVALID_URL;
        }
        memcpy(parsed->path, path, path_length + 1U);
    }

    return PLAYBACK_HTTP_OK;
}

static bool playback_header_value(
    const uint8_t *headers,
    size_t headers_length,
    const char *name,
    char *value,
    size_t value_capacity
)
{
    size_t line_start = 0U;

    if ((headers == NULL) || (name == NULL) || (value == NULL) || (value_capacity == 0U))
    {
        return false;
    }

    while (line_start < headers_length)
    {
        size_t line_end = line_start;
        size_t colon;
        size_t value_start;
        size_t value_end;
        size_t copied_length;

        while ((line_end < headers_length) && (headers[line_end] != '\r') && (headers[line_end] != '\n'))
        {
            ++line_end;
        }
        if (line_end == line_start)
        {
            line_start++;
            continue;
        }

        colon = line_start;
        while ((colon < line_end) && (headers[colon] != ':'))
        {
            ++colon;
        }
        if ((colon < line_end) &&
            playback_ascii_equal_ignore_case((const char *)headers + line_start, colon - line_start, name))
        {
            value_start = colon + 1U;
            while ((value_start < line_end) && ((headers[value_start] == ' ') || (headers[value_start] == '\t')))
            {
                ++value_start;
            }
            value_end = line_end;
            while ((value_end > value_start) &&
                   ((headers[value_end - 1U] == ' ') || (headers[value_end - 1U] == '\t')))
            {
                --value_end;
            }
            copied_length = value_end - value_start;
            if (copied_length >= value_capacity)
            {
                return false;
            }
            memcpy(value, headers + value_start, copied_length);
            value[copied_length] = '\0';
            return true;
        }

        line_start = line_end;
        while ((line_start < headers_length) &&
               ((headers[line_start] == '\r') || (headers[line_start] == '\n')))
        {
            ++line_start;
        }
    }

    return false;
}

static bool playback_parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if ((text == NULL) || (value == NULL) || (text[0] == '\0'))
    {
        return false;
    }

    parsed = strtoul(text, &end, 10);
    if ((end == text) || (*end != '\0') || (parsed > 0xFFFFFFFFUL))
    {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static bool playback_parse_content_range(const char *text, playback_content_range_t *range)
{
    const char *cursor;
    char *end = NULL;
    unsigned long start;
    unsigned long finish;
    unsigned long total;

    if ((text == NULL) || (range == NULL) || (strncmp(text, "bytes ", 6U) != 0))
    {
        return false;
    }

    cursor = text + 6U;
    start = strtoul(cursor, &end, 10);
    if ((end == cursor) || (*end != '-') || (start > 0xFFFFFFFFUL))
    {
        return false;
    }

    cursor = end + 1;
    finish = strtoul(cursor, &end, 10);
    if ((end == cursor) || (*end != '/') || (finish > 0xFFFFFFFFUL))
    {
        return false;
    }

    cursor = end + 1;
    total = strtoul(cursor, &end, 10);
    if ((end == cursor) || (*end != '\0') || (total > 0xFFFFFFFFUL) || (finish < start))
    {
        return false;
    }

    range->start = (uint32_t)start;
    range->end = (uint32_t)finish;
    range->total = (uint32_t)total;
    return true;
}

static playback_http_result_t playback_http_request_once(
    playback_http_range_reader_t *reader,
    const char *method,
    http_client_header_t *headers,
    uint8_t header_count,
    http_client_response_t *response
)
{
    playback_parsed_url_t parsed;
    http_client_request_t request;
    http_client_status_t request_result;
    playback_http_result_t result;

    result = playback_parse_url(reader->asset.url, &parsed);
    if (result != PLAYBACK_HTTP_OK)
    {
        return result;
    }
    if (parsed.tls && (reader->config.ca_cert == NULL) && !reader->config.tls_no_verify)
    {
        return PLAYBACK_HTTP_INVALID_ARGUMENT;
    }

    memset(&request, 0, sizeof(request));
    memset(response, 0, sizeof(*response));
    request.host = parsed.host;
    request.port = parsed.port;
    request.path = parsed.path;
    request.cacert = reader->config.ca_cert;
    request.cacert_len = reader->config.ca_cert_length;
    request.tls_no_verify = parsed.tls && reader->config.tls_no_verify;
    request.method = method;
    request.headers = headers;
    request.headers_count = header_count;
    request.timeout_ms = reader->config.timeout_ms;

    request_result = http_client_request(&request, response);
    return (request_result == HTTP_CLIENT_SUCCESS) ? PLAYBACK_HTTP_OK : PLAYBACK_HTTP_NETWORK_ERROR;
}

static playback_http_result_t playback_http_request_with_retry(
    playback_http_range_reader_t *reader,
    const char *method,
    http_client_header_t *headers,
    uint8_t header_count,
    http_client_response_t *response
)
{
    static const uint32_t retry_delay_ms[PLAYBACK_HTTP_RETRY_COUNT] = {250U, 500U, 1000U};
    playback_http_result_t result = PLAYBACK_HTTP_NETWORK_ERROR;
    size_t attempt;

    for (attempt = 0U; attempt < PLAYBACK_HTTP_RETRY_COUNT; ++attempt)
    {
        if (reader->cancelled)
        {
            return PLAYBACK_HTTP_CANCELLED;
        }

        result = playback_http_request_once(reader, method, headers, header_count, response);
        if (result == PLAYBACK_HTTP_OK)
        {
            return result;
        }
        if (attempt + 1U < PLAYBACK_HTTP_RETRY_COUNT)
        {
            tal_system_sleep(retry_delay_ms[attempt]);
        }
    }

    return result;
}

playback_http_result_t playback_http_range_reader_init(
    playback_http_range_reader_t *reader,
    const playback_asset_t *asset,
    const playback_http_config_t *config
)
{
    playback_parsed_url_t parsed;

    if ((reader == NULL) || (asset == NULL) || (asset->url[0] == '\0') || (asset->byte_length == 0U) ||
        (asset->etag[0] == '\0'))
    {
        return PLAYBACK_HTTP_INVALID_ARGUMENT;
    }
    if (playback_parse_url(asset->url, &parsed) != PLAYBACK_HTTP_OK)
    {
        return PLAYBACK_HTTP_INVALID_URL;
    }

    memset(reader, 0, sizeof(*reader));
    reader->asset = *asset;
    if (config != NULL)
    {
        reader->config = *config;
    }
    if (reader->config.timeout_ms == 0U)
    {
        reader->config.timeout_ms = PLAYBACK_HTTP_DEFAULT_TIMEOUT_MS;
    }

    return PLAYBACK_HTTP_OK;
}

playback_http_result_t playback_http_range_probe(
    playback_http_range_reader_t *reader,
    playback_http_metadata_t *metadata
)
{
    http_client_response_t response;
    http_client_header_t headers[1U];
    playback_http_result_t result;
    uint8_t header_count = 0U;
    char content_length_text[24U];
    char accept_ranges[32U];
    char etag[PLAYBACK_ETAG_MAX_LEN + 1U];
    uint32_t content_length;

    if ((reader == NULL) || (metadata == NULL))
    {
        return PLAYBACK_HTTP_INVALID_ARGUMENT;
    }
    if (reader->cancelled)
    {
        return PLAYBACK_HTTP_CANCELLED;
    }

    if ((reader->config.authorization != NULL) && (reader->config.authorization[0] != '\0'))
    {
        headers[header_count].key = "Authorization";
        headers[header_count].value = reader->config.authorization;
        ++header_count;
    }

    result = playback_http_request_with_retry(
        reader,
        HTTP_METHOD_HEAD,
        (header_count > 0U) ? headers : NULL,
        header_count,
        &response
    );
    if (result != PLAYBACK_HTTP_OK)
    {
        return result;
    }

    if (response.status_code != 200U)
    {
        http_client_free(&response);
        return PLAYBACK_HTTP_STATUS_ERROR;
    }
    if (!playback_header_value(response.headers, response.headers_length, "Content-Length", content_length_text,
                               sizeof(content_length_text)) ||
        !playback_parse_u32(content_length_text, &content_length) ||
        !playback_header_value(response.headers, response.headers_length, "ETag", etag, sizeof(etag)) ||
        !playback_header_value(response.headers, response.headers_length, "Accept-Ranges", accept_ranges,
                               sizeof(accept_ranges)))
    {
        http_client_free(&response);
        return PLAYBACK_HTTP_RESOURCE_MISMATCH;
    }

    if ((content_length != reader->asset.byte_length) || (strcmp(etag, reader->asset.etag) != 0) ||
        !playback_ascii_equal_ignore_case(accept_ranges, strlen(accept_ranges), "bytes"))
    {
        http_client_free(&response);
        return PLAYBACK_HTTP_RESOURCE_MISMATCH;
    }

    memset(metadata, 0, sizeof(*metadata));
    metadata->content_length = content_length;
    strncpy(metadata->etag, etag, sizeof(metadata->etag) - 1U);
    metadata->etag[sizeof(metadata->etag) - 1U] = '\0';
    metadata->accepts_byte_ranges = true;
    http_client_free(&response);
    return PLAYBACK_HTTP_OK;
}

playback_http_result_t playback_http_range_read_at(
    playback_http_range_reader_t *reader,
    uint32_t offset,
    uint32_t length,
    uint8_t *destination,
    size_t destination_capacity
)
{
    http_client_response_t response;
    http_client_header_t headers[PLAYBACK_HTTP_HEADER_COUNT_MAX];
    playback_http_result_t result;
    playback_content_range_t parsed_range;
    char range_value[64U];
    char content_range_value[80U];
    char etag[PLAYBACK_ETAG_MAX_LEN + 1U];
    uint64_t requested_end;
    uint8_t header_count = 0U;

    if ((reader == NULL) || (destination == NULL) || (length == 0U) || (length > destination_capacity) ||
        (length > PLAYBACK_HTTP_RANGE_MAX_LENGTH))
    {
        return PLAYBACK_HTTP_INVALID_ARGUMENT;
    }

    requested_end = (uint64_t)offset + (uint64_t)length - 1ULL;
    if ((requested_end >= reader->asset.byte_length) || reader->cancelled)
    {
        return reader->cancelled ? PLAYBACK_HTTP_CANCELLED : PLAYBACK_HTTP_RANGE_INVALID;
    }

    snprintf(range_value, sizeof(range_value), "bytes=%lu-%lu", (unsigned long)offset, (unsigned long)requested_end);
    headers[header_count].key = "Range";
    headers[header_count].value = range_value;
    ++header_count;
    headers[header_count].key = "If-Range";
    headers[header_count].value = reader->asset.etag;
    ++header_count;
    headers[header_count].key = "Accept-Encoding";
    headers[header_count].value = "identity";
    ++header_count;
    if ((reader->config.authorization != NULL) && (reader->config.authorization[0] != '\0'))
    {
        headers[header_count].key = "Authorization";
        headers[header_count].value = reader->config.authorization;
        ++header_count;
    }

    result = playback_http_request_with_retry(
        reader,
        HTTP_METHOD_GET,
        headers,
        header_count,
        &response
    );
    if (result != PLAYBACK_HTTP_OK)
    {
        return result;
    }

    if (response.status_code == 200U)
    {
        http_client_free(&response);
        return PLAYBACK_HTTP_RESOURCE_MISMATCH;
    }
    if (response.status_code == 416U)
    {
        http_client_free(&response);
        return PLAYBACK_HTTP_RANGE_INVALID;
    }
    if (response.status_code != 206U)
    {
        http_client_free(&response);
        return PLAYBACK_HTTP_STATUS_ERROR;
    }

    if (!playback_header_value(response.headers, response.headers_length, "Content-Range", content_range_value,
                               sizeof(content_range_value)) ||
        !playback_parse_content_range(content_range_value, &parsed_range) ||
        !playback_header_value(response.headers, response.headers_length, "ETag", etag, sizeof(etag)))
    {
        http_client_free(&response);
        return PLAYBACK_HTTP_RESOURCE_MISMATCH;
    }

    if ((parsed_range.start != offset) || (parsed_range.end != (uint32_t)requested_end) ||
        (parsed_range.total != reader->asset.byte_length) || (strcmp(etag, reader->asset.etag) != 0) ||
        (response.body_length != length))
    {
        http_client_free(&response);
        return PLAYBACK_HTTP_RESOURCE_MISMATCH;
    }

    memcpy(destination, response.body, length);
    http_client_free(&response);
    return reader->cancelled ? PLAYBACK_HTTP_CANCELLED : PLAYBACK_HTTP_OK;
}

void playback_http_range_cancel(playback_http_range_reader_t *reader)
{
    if (reader != NULL)
    {
        reader->cancelled = true;
    }
}

void playback_http_range_reset_cancel(playback_http_range_reader_t *reader)
{
    if (reader != NULL)
    {
        reader->cancelled = false;
    }
}

const char *playback_http_result_name(playback_http_result_t result)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "INVALID_URL",
        "CANCELLED",
        "NETWORK_ERROR",
        "STATUS_ERROR",
        "RANGE_INVALID",
        "RESOURCE_MISMATCH",
        "RESPONSE_TOO_LARGE",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0]))) ? names[result] : "UNKNOWN_HTTP_RESULT";
}
