#ifndef PLAYBACK_NETWORK_H
#define PLAYBACK_NETWORK_H

#include <stdbool.h>

#include "tuya_cloud_types.h"

typedef void (*playback_network_status_callback_t)(
    void *context,
    bool connected,
    const char *local_ip
);

/** Configure an optional Wi-Fi status callback before starting the network. */
void playback_network_set_status_callback(
    playback_network_status_callback_t callback,
    void *context
);

/** Initialize the fixed runtime Wi-Fi connection used by media and Board Link HTTP. */
OPERATE_RET playback_network_start(void);

/** Get the station IPv4 address. Returns empty string while disconnected. */
const char *playback_network_get_local_ip(void);

/** Get the gateway IP address. Returns empty string while disconnected. */
const char *playback_network_get_gateway(void);

#endif /* PLAYBACK_NETWORK_H */
