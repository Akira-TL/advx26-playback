#ifndef PLAYBACK_HTTP_DOWNLOAD_H
#define PLAYBACK_HTTP_DOWNLOAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
    PLAYBACK_HTTP_DOWNLOAD_OK = 0,
    PLAYBACK_HTTP_DOWNLOAD_INVALID_ARGUMENT,
    PLAYBACK_HTTP_DOWNLOAD_NO_MEMORY,
    PLAYBACK_HTTP_DOWNLOAD_NETWORK_ERROR,
    PLAYBACK_HTTP_DOWNLOAD_RESOURCE_MISMATCH,
} playback_http_download_result_t;

typedef struct
{
    uint8_t *data;
    uint32_t length;
} playback_http_download_file_t;

/**
 * Download a complete HTTP resource into PSRAM using resumable 128 KiB segments.
 * expected_length must match the server's complete Content-Length.
 * authorization: optional "Bearer <token>" header value (NULL/empty to skip).
 */
playback_http_download_result_t playback_http_download_file(
    const char *url,
    const char *authorization,
    uint32_t expected_length,
    const char *log_name,
    playback_http_download_file_t *file
);

void playback_http_download_release(playback_http_download_file_t *file);
const char *playback_http_download_result_name(
    playback_http_download_result_t result
);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_HTTP_DOWNLOAD_H */
