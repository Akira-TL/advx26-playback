#ifndef PLAYBACK_NETWORK_H
#define PLAYBACK_NETWORK_H

#include "tuya_cloud_types.h"

/** Initialize the fixed runtime Wi-Fi connection used by media Range reads. */
OPERATE_RET playback_network_start(void);

/** Get the gateway IP address (e.g. phone hotspot). Returns empty string if not connected. */
const char *playback_network_get_gateway(void);

#endif /* PLAYBACK_NETWORK_H */
