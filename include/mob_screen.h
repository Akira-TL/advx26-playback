#ifndef MOB_SCREEN_H
#define MOB_SCREEN_H

#include <stdbool.h>

#include "playback_video_output.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build and load the initial MOB LVGL screen.
 *
 * The caller must hold the LVGL display lock while invoking this function.
 */
typedef void (*mob_screen_speaker_test_cb)(void *context);
typedef void (*mob_screen_video_test_cb)(void *context);
typedef void (*mob_screen_av_test_cb)(void *context);
typedef bool (*mob_screen_peer_ip_submit_cb)(void *context, const char *peer_ip);

/** Configure optional speaker-test action before creating the screen. */
void mob_screen_set_speaker_test_callback(mob_screen_speaker_test_cb on_speaker_test, void *context);

/** Configure optional H.264 video-test action before creating the screen. */
void mob_screen_set_video_test_callback(mob_screen_video_test_cb on_video_test, void *context);

/** Configure optional synchronized audio/video-test action before creating the screen. */
void mob_screen_set_av_test_callback(mob_screen_av_test_cb on_av_test, void *context);

/** Configure the control-board IPv4 save action before creating the screen. */
void mob_screen_set_peer_ip_submit_callback(
    mob_screen_peer_ip_submit_cb on_submit,
    void *context
);

/** Update the network page with the current local and control-board IPv4 addresses. */
void mob_screen_update_network(const char *local_ip, const char *peer_ip);

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
