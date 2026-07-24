#ifndef PLAYBACK_BLUETOOTH_BROWSER_H
#define PLAYBACK_BLUETOOTH_BROWSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAYBACK_BLUETOOTH_BROWSER_MAX_DEVICES (12U)
#define PLAYBACK_BLUETOOTH_BROWSER_NAME_MAX_LEN (63U)
#define PLAYBACK_BLUETOOTH_ADDRESS_BYTES (6U)

typedef enum
{
    PLAYBACK_BLUETOOTH_BROWSER_IDLE = 0,
    PLAYBACK_BLUETOOTH_BROWSER_SCANNING,
    PLAYBACK_BLUETOOTH_BROWSER_CONNECTING,
    PLAYBACK_BLUETOOTH_BROWSER_CONNECTED,
    PLAYBACK_BLUETOOTH_BROWSER_PAIRED,
    PLAYBACK_BLUETOOTH_BROWSER_FAILED,
} playback_bluetooth_browser_status_t;

typedef struct
{
    uint8_t address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES];
    char name[PLAYBACK_BLUETOOTH_BROWSER_NAME_MAX_LEN + 1U];
    int8_t rssi;
    uint32_t class_of_device;
    bool audio_device;
} playback_bluetooth_browser_device_t;

typedef void (*playback_bluetooth_browser_devices_cb)(
    void *context,
    const playback_bluetooth_browser_device_t *devices,
    size_t device_count,
    bool scanning
);

typedef void (*playback_bluetooth_browser_status_cb)(
    void *context,
    playback_bluetooth_browser_status_t status,
    const uint8_t address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES],
    const char *detail
);

typedef struct
{
    playback_bluetooth_browser_devices_cb on_devices;
    playback_bluetooth_browser_status_cb on_status;
    void *context;
} playback_bluetooth_browser_config_t;

typedef struct
{
    void *state;
} playback_bluetooth_browser_t;

bool playback_bluetooth_browser_init(
    playback_bluetooth_browser_t *browser,
    const playback_bluetooth_browser_config_t *config
);

bool playback_bluetooth_browser_start_scan(playback_bluetooth_browser_t *browser);

bool playback_bluetooth_browser_connect(
    playback_bluetooth_browser_t *browser,
    const uint8_t address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES]
);

void playback_bluetooth_browser_close(playback_bluetooth_browser_t *browser);

const char *playback_bluetooth_browser_status_name(
    playback_bluetooth_browser_status_t status
);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_BLUETOOTH_BROWSER_H */
