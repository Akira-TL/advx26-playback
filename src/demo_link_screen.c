/**
 * @file demo_link_screen.c
 * @brief "Waiting for master board" screen (LVGL v8).
 *
 * Shown after WiFi + pairing. Displays the local IP:8788 the master must
 * connect to, the link/playback phase, and a button to the local content
 * screen for standalone debugging.
 */

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tal_wifi.h"
#include "lvgl.h"

#include "demo_master_link.h"
#include "demo_ui.h"

#define LINK_BG     0x071426
#define LINK_CARD   0x173852
#define LINK_WHITE  0xF7FBFF
#define LINK_MUTED  0xA9C1D7
#define LINK_CYAN   0x48D8FF
#define LINK_GREEN  0x48E0A4
#define LINK_YELLOW 0xFFD166

static lv_obj_t *sg_scr;
static lv_obj_t *sg_ip_label;
static lv_obj_t *sg_conn_label;
static lv_obj_t *sg_phase_label;

static void link_ui_timer_cb(lv_timer_t *timer)
{
    static char last_ip[20];
    static demo_master_link_status_t last_status;
    demo_master_link_status_t status;
    WF_STATION_STAT_E stat = WSS_IDLE;
    const char *ip_str = "--";
    NW_IP_S ip;

    (void)timer;
    if (lv_scr_act() != sg_scr) {
        return;
    }

    if (tal_wifi_station_get_status(&stat) == OPRT_OK && stat == WSS_GOT_IP) {
        memset(&ip, 0, sizeof(ip));
        if (tal_wifi_get_ip(WF_STATION, &ip) == OPRT_OK && ip.ip[0] != '\0') {
            ip_str = ip.ip;
        }
    }
    if (strcmp(ip_str, last_ip) != 0) {
        char buf[48];

        snprintf(last_ip, sizeof(last_ip), "%s", ip_str);
        snprintf(buf, sizeof(buf), "%s : %u", ip_str,
                 (unsigned)DEMO_MASTER_LINK_PORT);
        lv_label_set_text(sg_ip_label, buf);
    }

    demo_master_link_get_status(&status);
    if (status.client_connected != last_status.client_connected ||
        strcmp(status.peer_ip, last_status.peer_ip) != 0) {
        if (status.client_connected) {
            char buf[48];

            snprintf(buf, sizeof(buf), "Master connected: %s", status.peer_ip);
            lv_obj_set_style_text_color(sg_conn_label,
                                        lv_color_hex(LINK_GREEN),
                                        LV_PART_MAIN);
            lv_label_set_text(sg_conn_label, buf);
        } else {
            lv_obj_set_style_text_color(sg_conn_label,
                                        lv_color_hex(LINK_YELLOW),
                                        LV_PART_MAIN);
            lv_label_set_text(sg_conn_label, "Waiting for master...");
        }
    }
    if (strcmp(status.phase, last_status.phase) != 0) {
        lv_label_set_text(sg_phase_label, status.phase);
    }
    last_status = status;
}

static void link_content_btn_cb(lv_event_t *e)
{
    if (LV_EVENT_CLICKED != lv_event_get_code(e)) {
        return;
    }
    demo_ui_show_content();
}

lv_obj_t *demo_link_screen_create(void)
{
    lv_obj_t *label;
    lv_obj_t *btn;

    sg_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(sg_scr, lv_color_hex(LINK_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sg_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(sg_scr, LV_OBJ_FLAG_SCROLLABLE);

    label = lv_label_create(sg_scr);
    lv_label_set_text(label, "Board Link");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(LINK_WHITE),
                                LV_PART_MAIN);
    lv_obj_set_pos(label, 8, 10);

    label = lv_label_create(sg_scr);
    lv_label_set_text(label, "Connect the master COMM screen to:");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(LINK_MUTED),
                                LV_PART_MAIN);
    lv_obj_set_pos(label, 8, 64);

    sg_ip_label = lv_label_create(sg_scr);
    lv_label_set_text(sg_ip_label, "-- : 8788");
    lv_obj_set_style_text_font(sg_ip_label, &lv_font_montserrat_24,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(sg_ip_label, lv_color_hex(LINK_CYAN),
                                LV_PART_MAIN);
    lv_obj_set_pos(sg_ip_label, 8, 92);

    sg_conn_label = lv_label_create(sg_scr);
    lv_label_set_text(sg_conn_label, "Waiting for master...");
    lv_obj_set_style_text_font(sg_conn_label, &lv_font_montserrat_16,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(sg_conn_label, lv_color_hex(LINK_YELLOW),
                                LV_PART_MAIN);
    lv_obj_set_pos(sg_conn_label, 8, 150);

    sg_phase_label = lv_label_create(sg_scr);
    lv_label_set_text(sg_phase_label, "starting");
    lv_obj_set_style_text_font(sg_phase_label, &lv_font_montserrat_14,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(sg_phase_label, lv_color_hex(LINK_MUTED),
                                LV_PART_MAIN);
    lv_obj_set_pos(sg_phase_label, 8, 180);

    btn = lv_btn_create(sg_scr);
    lv_obj_set_pos(btn, 8, 276);
    lv_obj_set_size(btn, 180, 32);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(LINK_CARD), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x5D7891), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, link_content_btn_cb, LV_EVENT_CLICKED, NULL);
    label = lv_label_create(btn);
    lv_label_set_text(label, "Local content list");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(LINK_WHITE),
                                LV_PART_MAIN);
    lv_obj_center(label);

    lv_timer_create(link_ui_timer_cb, 250, NULL);

    return sg_scr;
}
