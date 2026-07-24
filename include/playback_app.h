#ifndef PLAYBACK_APP_H
#define PLAYBACK_APP_H

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Initialize display, network, Board Link and the Playback engine. */
OPERATE_RET playback_app_start(void);

/** Release Playback engine and Board Link resources in reverse order. */
void playback_app_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_APP_H */
