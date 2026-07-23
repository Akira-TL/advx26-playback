#ifndef PLAYBACK_HTTP_RANGE_H
#define PLAYBACK_HTTP_RANGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "playback_domain.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_HTTP_HOST_MAX_LEN (255U)
#define PLAYBACK_HTTP_PATH_MAX_LEN (1024U)
#define PLAYBACK_HTTP_RANGE_MAX_LENGTH (256U * 1024U)
#define PLAYBACK_HTTP_DEFAULT_TIMEOUT_MS (15000U)

typedef enum
{
    PLAYBACK_HTTP_OK = 0,
    PLAYBACK_HTTP_INVALID_ARGUMENT,
    PLAYBACK_HTTP_INVALID_URL,
    PLAYBACK_HTTP_CANCELLED,
    PLAYBACK_HTTP_NETWORK_ERROR,
    PLAYBACK_HTTP_STATUS_ERROR,
    PLAYBACK_HTTP_RANGE_INVALID,
    PLAYBACK_HTTP_RESOURCE_MISMATCH,
    PLAYBACK_HTTP_RESPONSE_TOO_LARGE,
} playback_http_result_t;

typedef struct
{
    const uint8_t *ca_cert;
    size_t ca_cert_length;
    bool tls_no_verify;
    uint32_t timeout_ms;
} playback_http_config_t;

typedef struct
{
    uint32_t content_length;
    char etag[PLAYBACK_ETAG_MAX_LEN + 1U];
    bool accepts_byte_ranges;
} playback_http_metadata_t;

typedef struct
{
    playback_asset_t asset;
    playback_http_config_t config;
    volatile bool cancelled;
} playback_http_range_reader_t;

playback_http_result_t playback_http_range_reader_init(
    playback_http_range_reader_t *reader,
    const playback_asset_t *asset,
    const playback_http_config_t *config
);

playback_http_result_t playback_http_range_probe(
    playback_http_range_reader_t *reader,
    playback_http_metadata_t *metadata
);

playback_http_result_t playback_http_range_read_at(
    playback_http_range_reader_t *reader,
    uint32_t offset,
    uint32_t length,
    uint8_t *destination,
    size_t destination_capacity
);

void playback_http_range_cancel(playback_http_range_reader_t *reader);
void playback_http_range_reset_cancel(playback_http_range_reader_t *reader);
const char *playback_http_result_name(playback_http_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_HTTP_RANGE_H */
