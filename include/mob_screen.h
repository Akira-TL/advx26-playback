#ifndef MOB_SCREEN_H
#define MOB_SCREEN_H

#include "playback_video_output.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build and load the initial MOB LVGL screen.
 *
 * The caller must hold the LVGL display lock while invoking this function.
 */
void mob_screen_create(void);

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
