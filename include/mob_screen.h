#ifndef MOB_SCREEN_H
#define MOB_SCREEN_H

#include "playback_bluetooth_browser.h"
#include "playback_video_output.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build and load the initial MOB LVGL screen.
 *
 * The caller must hold the LVGL display lock while invoking this function.
 */
typedef void (*mob_screen_bluetooth_scan_cb)(void *context);
typedef void (*mob_screen_bluetooth_connect_cb)(
    void *context,
    const uint8_t address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES]
);

typedef struct
{
    mob_screen_bluetooth_scan_cb on_scan;
    mob_screen_bluetooth_connect_cb on_connect;
    void *context;
} mob_screen_bluetooth_callbacks_t;

typedef void (*mob_screen_speaker_test_cb)(void *context);
typedef void (*mob_screen_video_test_cb)(void *context);

/** Configure optional speaker-test action before creating the screen. */
void mob_screen_set_speaker_test_callback(mob_screen_speaker_test_cb on_speaker_test, void *context);

/** Configure optional H.264 video-test action before creating the screen. */
void mob_screen_set_video_test_callback(mob_screen_video_test_cb on_video_test, void *context);

/** Configure Bluetooth-page actions before creating the screen. */
void mob_screen_set_bluetooth_callbacks(
    const mob_screen_bluetooth_callbacks_t *callbacks
);

void mob_screen_create(void);

/**
 * @brief Alternate full-screen white and black frames to reduce LCD image retention.
 *
 * The LVGL worker must already be running. This function acquires the display
 * lock internally and restores the idle screen before returning.
 */
void mob_screen_neutralize_panel(void);

/** Show a minimal output-only state screen without disturbing paused/completed video. */
void mob_screen_show_state(playback_state_t state, const char *diagnostic);

/** Replace the Bluetooth page device list. Thread-safe. */
void mob_screen_show_bluetooth_devices(
    const playback_bluetooth_browser_device_t *devices,
    size_t device_count,
    bool scanning
);

/** Update Bluetooth-page connection status. Thread-safe. */
void mob_screen_show_bluetooth_status(
    playback_bluetooth_browser_status_t status,
    const uint8_t address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES],
    const char *detail
);

/**
 * @brief Present a contiguous native-endian RGB565 surface through LVGL.
 *
 * This function is a thread-safe playback video sink. It acquires the LVGL
 * display lock internally, retains a copy of the last successful frame, and
 * performs a synchronous refresh before returning. The caller may reuse the
 * source surface immediately after a successful return. The context argument
 * is reserved for future display backends and may be NULL.
 */
playback_video_output_result_t mob_screen_present_rgb565(
    void *context,
    const playback_rgb565_surface_t *surface
);

#ifdef __cplusplus
}
#endif

#endif /* MOB_SCREEN_H */
