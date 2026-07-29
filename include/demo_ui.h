#ifndef DEMO_UI_H
#define DEMO_UI_H

#include "lvgl.h"
#include "playback_domain.h"
#include "playback_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the interactive demo UI (WiFi -> Pair -> Content -> Play).
 * Must be called after lv_vendor_start(); acquires the display lock itself. */
void demo_ui_start(playback_engine_t *engine);

/* Called from the engine report sink thread; schedules the return to the
 * content screen after COMPLETED/ERROR. */
void demo_ui_notify_report(const playback_report_t *report);

playback_engine_t *demo_ui_engine(void);

/* Screen switching (LVGL thread only). */
void demo_ui_show_wifi(void);
void demo_ui_show_pair(void);
void demo_ui_show_content(void);
void demo_ui_show_link(void);

/* Screen constructors (called by demo_ui_start under the display lock). */
lv_obj_t *demo_wifi_screen_create(void);
lv_obj_t *demo_pair_screen_create(void);
lv_obj_t *demo_content_screen_create(void);
lv_obj_t *demo_link_screen_create(void);

#ifdef __cplusplus
}
#endif

#endif /* DEMO_UI_H */
