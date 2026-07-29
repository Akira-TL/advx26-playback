/**
 * @file demo_ui.c
 * @brief Screen navigation and playback-report glue for the slave demo.
 */

#include <string.h>

#include "tal_api.h"
#include "lv_vendor.h"

#include "demo_cloud.h"
#include "demo_ui.h"

#define DEMO_UI_RETURN_DELAY_MS 4000U

static playback_engine_t *sg_engine;
static lv_obj_t *sg_wifi_scr;
static lv_obj_t *sg_pair_scr;
static lv_obj_t *sg_content_scr;
static lv_obj_t *sg_link_scr;

/* Written by the report sink thread, consumed by the LVGL timer. */
static volatile bool sg_return_pending;
static volatile SYS_TIME_T sg_return_at_ms;

playback_engine_t *demo_ui_engine(void)
{
    return sg_engine;
}

void demo_ui_show_wifi(void)
{
    if (sg_wifi_scr != NULL) {
        lv_disp_load_scr(sg_wifi_scr);
    }
}

void demo_ui_show_pair(void)
{
    if (sg_pair_scr != NULL) {
        lv_disp_load_scr(sg_pair_scr);
    }
}

void demo_ui_show_content(void)
{
    if (sg_content_scr != NULL) {
        lv_disp_load_scr(sg_content_scr);
    }
}

void demo_ui_show_link(void)
{
    if (sg_link_scr != NULL) {
        lv_disp_load_scr(sg_link_scr);
    }
}

void demo_ui_notify_report(const playback_report_t *report)
{
    if (report == NULL) {
        return;
    }
    switch (report->kind) {
        case PLAYBACK_REPORT_COMPLETED:
        case PLAYBACK_REPORT_ERROR:
            sg_return_at_ms = tal_system_get_millisecond() +
                              DEMO_UI_RETURN_DELAY_MS;
            sg_return_pending = true;
            break;
        case PLAYBACK_REPORT_STATE:
            if (report->state == PLAYBACK_STATE_ERROR) {
                sg_return_at_ms = tal_system_get_millisecond() +
                                  DEMO_UI_RETURN_DELAY_MS;
                sg_return_pending = true;
            } else if (report->state == PLAYBACK_STATE_LOADING ||
                       report->state == PLAYBACK_STATE_PLAYING) {
                sg_return_pending = false;
            }
            break;
        default:
            break;
    }
}

static void demo_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (sg_return_pending &&
        tal_system_get_millisecond() >= sg_return_at_ms) {
        sg_return_pending = false;
        demo_ui_show_link();
    }
}

void demo_ui_start(playback_engine_t *engine)
{
    sg_engine = engine;
    demo_cloud_refresh_auth();

    lv_vendor_disp_lock();
    sg_wifi_scr = demo_wifi_screen_create();
    sg_pair_scr = demo_pair_screen_create();
    sg_content_scr = demo_content_screen_create();
    sg_link_scr = demo_link_screen_create();
    lv_timer_create(demo_ui_timer_cb, 250, NULL);
    demo_ui_show_wifi();
    lv_vendor_disp_unlock();

    PR_NOTICE("demo ui: started (wifi -> pair -> content)");
}
