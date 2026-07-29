#ifndef DEMO_CLOUD_H
#define DEMO_CLOUD_H

#include <stdbool.h>
#include <stdint.h>

#include "playback_domain.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEMO_CLOUD_MAX_ITEMS 8
#define DEMO_CLOUD_LABEL_MAX 48

typedef struct
{
    char content_id[PLAYBACK_CONTENT_ID_MAX_LEN + 1U];
    char label[DEMO_CLOUD_LABEL_MAX];
    uint32_t duration_ms;
    bool ready;
} demo_cloud_item_t;

typedef enum
{
    DEMO_CLOUD_OK = 0,
    DEMO_CLOUD_NOT_PAIRED,
    DEMO_CLOUD_NET_ERROR,
    DEMO_CLOUD_AUTH_ERROR,
    DEMO_CLOUD_BAD_RESPONSE,
} demo_cloud_result_t;

bool demo_cloud_is_paired(void);

/* Blocking; call from a worker thread. Returns item count in *count_out. */
demo_cloud_result_t demo_cloud_fetch_list(
    demo_cloud_item_t *items,
    int max_items,
    int *count_out
);

/* Blocking; resolves /c/{content_id} into a ready-to-submit session. */
demo_cloud_result_t demo_cloud_fetch_session(
    const char *content_id,
    playback_session_t *session
);

/* Stable pointer for the playback engine HTTP authorization; contents are
 * refreshed from KV/compile-time config by demo_cloud_refresh_auth(). */
const char *demo_cloud_playback_authorization(void);
void demo_cloud_refresh_auth(void);

#ifdef __cplusplus
}
#endif

#endif /* DEMO_CLOUD_H */
