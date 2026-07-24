#ifndef PLAYBACK_H264BSD_PORT_H
#define PLAYBACK_H264BSD_PORT_H

#include "tal_api.h"

#define H264BSD_MALLOC(size) tal_psram_malloc(size)
#define H264BSD_FREE(pointer) tal_psram_free(pointer)

#endif /* PLAYBACK_H264BSD_PORT_H */
