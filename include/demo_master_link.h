#ifndef DEMO_MASTER_LINK_H
#define DEMO_MASTER_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "playback_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NDJSON control server for the master board (tuyatest COMM screen).
 * Listens on TCP :8788; 8787 is the legacy binary board link and 8789 the
 * pairing HTTP server. */
#define DEMO_MASTER_LINK_PORT 8788U

typedef struct
{
    bool client_connected;
    char peer_ip[16];
    char phase[32]; /* human-readable: idle / loading / ready / playing ... */
} demo_master_link_status_t;

OPERATE_RET demo_master_link_start(playback_engine_t *engine);

/* Thread-safe snapshot for the UI poll timer. */
void demo_master_link_get_status(demo_master_link_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* DEMO_MASTER_LINK_H */
