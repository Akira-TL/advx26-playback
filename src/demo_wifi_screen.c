/**
 * @file demo_wifi_screen.c
 * @brief Interactive Wi-Fi provisioning screen for the slave demo (LVGL v8).
 */

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tal_wifi.h"
#include "lvgl.h"

#include "demo_cloud.h"
#include "demo_ui.h"

#define WIFI_BG       0x071426
#define WIFI_CARD     0x173852
#define WIFI_WHITE    0xF7FBFF
#define WIFI_MUTED    0xA9C1D7
#define WIFI_CYAN     0x48D8FF
#define WIFI_GREEN    0x48E0A4
#define WIFI_YELLOW   0xFFD166
#define WIFI_RED      0xFF6B7A

#define WIFI_SSID_MAX 33
#define WIFI_PASS_MAX 65
#define WIFI_AP_MAX   12
#define WIFI_POLL_TIMEOUT 200 /* 200 * 100ms = 20s */

#define WIFI_KV_SSID  "demo_wifi_ssid"
#define WIFI_KV_PSWD  "demo_wifi_pswd"

static lv_obj_t *sg_scr;
static lv_obj_t *sg_roller;
static lv_obj_t *sg_textarea;
static lv_obj_t *sg_keyboard;
static lv_obj_t *sg_status_label;
static lv_obj_t *sg_connect_btn;
static lv_obj_t *sg_rescan_btn;
static lv_obj_t *sg_kb_toggle_label;
static bool sg_keyboard_visible;

static THREAD_HANDLE sg_worker;

static AP_IF_S sg_aps[WIFI_AP_MAX];
static uint32_t sg_ap_count;
static char sg_roller_opts[512];

static char sg_conn_ssid[WIFI_SSID_MAX];
static char sg_conn_pass[WIFI_PASS_MAX];

static volatile bool sg_scan_req;
static volatile bool sg_scan_done;
static volatile bool sg_connect_req;
static volatile bool sg_connect_started;
static volatile uint8_t sg_poll_count;
static volatile bool sg_polling;
static bool sg_advance_pending;
static SYS_TIME_T sg_advance_at_ms;

static void wifi_set_status(uint32_t color, const char *text)
{
    lv_obj_set_style_text_color(sg_status_label, lv_color_hex(color),
                                LV_PART_MAIN);
    lv_label_set_text(sg_status_label, text);
}

static lv_obj_t *wifi_create_label(lv_obj_t *parent, const char *text,
                                   const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    return label;
}

static void wifi_save_credentials(void)
{
    tal_kv_set(WIFI_KV_SSID, (const uint8_t *)sg_conn_ssid,
               strlen(sg_conn_ssid));
    tal_kv_set(WIFI_KV_PSWD, (const uint8_t *)sg_conn_pass,
               strlen(sg_conn_pass));
}

static bool wifi_load_credentials(void)
{
    uint8_t *val = NULL;
    size_t len = 0;

    if (tal_kv_get(WIFI_KV_SSID, &val, &len) != OPRT_OK || val == NULL ||
        len == 0U) {
        if (val != NULL) {
            tal_kv_free(val);
        }
        return false;
    }
    if (len >= sizeof(sg_conn_ssid)) {
        len = sizeof(sg_conn_ssid) - 1U;
    }
    memcpy(sg_conn_ssid, val, len);
    sg_conn_ssid[len] = '\0';
    tal_kv_free(val);
    val = NULL;

    sg_conn_pass[0] = '\0';
    if (tal_kv_get(WIFI_KV_PSWD, &val, &len) == OPRT_OK && val != NULL) {
        if (len >= sizeof(sg_conn_pass)) {
            len = sizeof(sg_conn_pass) - 1U;
        }
        memcpy(sg_conn_pass, val, len);
        sg_conn_pass[len] = '\0';
        tal_kv_free(val);
    }
    return sg_conn_ssid[0] != '\0';
}

static void wifi_build_roller_options(void)
{
    size_t off = 0;
    uint32_t i;

    if (sg_ap_count == 0U) {
        lv_roller_set_options(sg_roller, "No networks found",
                              LV_ROLLER_MODE_NORMAL);
        return;
    }
    for (i = 0U; i < sg_ap_count; i++) {
        char ssid[34];
        size_t slen = (sg_aps[i].s_len < 33U) ? sg_aps[i].s_len : 32U;

        memcpy(ssid, sg_aps[i].ssid, slen);
        ssid[slen] = '\0';
        off += (size_t)snprintf(sg_roller_opts + off,
                                sizeof(sg_roller_opts) - off,
                                "%s%s", (i == 0U) ? "" : "\n", ssid);
        if (off + 34U >= sizeof(sg_roller_opts)) {
            break;
        }
    }
    lv_roller_set_options(sg_roller, sg_roller_opts, LV_ROLLER_MODE_NORMAL);
}

static void wifi_on_connected(void)
{
    wifi_save_credentials();
    demo_cloud_refresh_auth();
    sg_advance_pending = true;
    sg_advance_at_ms = tal_system_get_millisecond() + 1200U;
}

static void wifi_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (sg_scan_done) {
        sg_scan_done = false;
        wifi_build_roller_options();
        if (sg_ap_count > 0U) {
            wifi_set_status(WIFI_MUTED, "Select network, enter password");
        } else {
            wifi_set_status(WIFI_RED, "No networks found");
        }
        lv_obj_clear_state(sg_rescan_btn, LV_STATE_DISABLED);
        lv_obj_clear_state(sg_connect_btn, LV_STATE_DISABLED);
    }

    if (sg_connect_started) {
        sg_connect_started = false;
        sg_polling = true;
        sg_poll_count = 0U;
        wifi_set_status(WIFI_YELLOW, "Connecting...");
    }

    if (sg_polling) {
        WF_STATION_STAT_E stat = WSS_IDLE;

        sg_poll_count++;
        if (tal_wifi_station_get_status(&stat) == OPRT_OK) {
            if (stat == WSS_GOT_IP) {
                NW_IP_S ip;
                char msg[48];

                memset(&ip, 0, sizeof(ip));
                tal_wifi_get_ip(WF_STATION, &ip);
                snprintf(msg, sizeof(msg), "Connected  %s", ip.ip);
                wifi_set_status(WIFI_GREEN, msg);
                sg_polling = false;
                lv_obj_clear_state(sg_connect_btn, LV_STATE_DISABLED);
                wifi_on_connected();
            } else if (stat == WSS_PASSWD_WRONG) {
                wifi_set_status(WIFI_RED, "Wrong password");
                sg_polling = false;
                lv_obj_clear_state(sg_connect_btn, LV_STATE_DISABLED);
            } else if (stat == WSS_NO_AP_FOUND) {
                wifi_set_status(WIFI_RED, "AP not found");
                sg_polling = false;
                lv_obj_clear_state(sg_connect_btn, LV_STATE_DISABLED);
            } else if (stat == WSS_CONN_FAIL || stat == WSS_DHCP_FAIL) {
                wifi_set_status(WIFI_RED, "Connect failed");
                sg_polling = false;
                lv_obj_clear_state(sg_connect_btn, LV_STATE_DISABLED);
            } else if (sg_poll_count > (uint8_t)WIFI_POLL_TIMEOUT) {
                wifi_set_status(WIFI_RED, "Timeout");
                sg_polling = false;
                lv_obj_clear_state(sg_connect_btn, LV_STATE_DISABLED);
            }
        }
    }

    if (sg_advance_pending &&
        tal_system_get_millisecond() >= sg_advance_at_ms) {
        sg_advance_pending = false;
        if (demo_cloud_is_paired()) {
            demo_ui_show_link();
        } else {
            demo_ui_show_pair();
        }
    }
}

static void wifi_worker(void *arg)
{
    (void)arg;

    while (1) {
        if (sg_scan_req) {
            AP_IF_S *aps = NULL;
            uint32_t num = 0U;

            sg_ap_count = 0U;
            if (tal_wifi_all_ap_scan(&aps, &num) == OPRT_OK && aps != NULL) {
                uint32_t i;
                uint32_t cnt = (num > (uint32_t)WIFI_AP_MAX) ?
                               (uint32_t)WIFI_AP_MAX : num;

                for (i = 0U; i < cnt; i++) {
                    memcpy(&sg_aps[i], &aps[i], sizeof(AP_IF_S));
                }
                sg_ap_count = cnt;
                tal_wifi_release_ap(aps);
            }
            PR_NOTICE("demo wifi: scan done, found %u",
                      (unsigned)sg_ap_count);
            sg_scan_req = false;
            sg_scan_done = true;
        } else if (sg_connect_req) {
            PR_NOTICE("demo wifi: connect ssid='%s'", sg_conn_ssid);
            tal_wifi_station_connect((int8_t *)sg_conn_ssid,
                                     (int8_t *)sg_conn_pass);
            sg_connect_req = false;
            sg_connect_started = true;
        } else {
            tal_system_sleep(30);
        }
    }
}

static void wifi_rescan_cb(lv_event_t *e)
{
    if (LV_EVENT_CLICKED != lv_event_get_code(e) || sg_scan_req) {
        return;
    }
    wifi_set_status(WIFI_YELLOW, "Scanning...");
    lv_obj_add_state(sg_rescan_btn, LV_STATE_DISABLED);
    lv_obj_add_state(sg_connect_btn, LV_STATE_DISABLED);
    sg_scan_req = true;
}

static void wifi_connect_cb(lv_event_t *e)
{
    char ssid[34];
    const char *pass;
    size_t plen;

    if (LV_EVENT_CLICKED != lv_event_get_code(e) || sg_connect_req ||
        sg_polling || sg_ap_count == 0U) {
        return;
    }
    lv_roller_get_selected_str(sg_roller, ssid, sizeof(ssid));
    if (ssid[0] == '\0') {
        wifi_set_status(WIFI_RED, "Select a network first");
        return;
    }
    pass = lv_textarea_get_text(sg_textarea);
    plen = strlen(pass);
    if (plen >= WIFI_PASS_MAX) {
        plen = WIFI_PASS_MAX - 1U;
    }
    snprintf(sg_conn_ssid, sizeof(sg_conn_ssid), "%s", ssid);
    memcpy(sg_conn_pass, pass, plen);
    sg_conn_pass[plen] = '\0';

    wifi_set_status(WIFI_YELLOW, "Connecting...");
    lv_obj_add_state(sg_connect_btn, LV_STATE_DISABLED);
    sg_connect_req = true;
}

static void wifi_apply_kb_visibility(void)
{
    if (sg_keyboard_visible) {
        lv_obj_clear_flag(sg_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(sg_roller, 210, 96);
        lv_roller_set_visible_row_count(sg_roller, 4);
        lv_obj_set_pos(sg_rescan_btn, 8, 142);
        lv_label_set_text(sg_kb_toggle_label, LV_SYMBOL_DOWN);
    } else {
        lv_obj_add_flag(sg_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(sg_roller, 210, 244);
        lv_roller_set_visible_row_count(sg_roller, 10);
        lv_obj_set_pos(sg_rescan_btn, 8, 290);
        lv_label_set_text(sg_kb_toggle_label, LV_SYMBOL_KEYBOARD);
    }
}

static void wifi_kb_toggle_cb(lv_event_t *e)
{
    if (LV_EVENT_CLICKED != lv_event_get_code(e)) {
        return;
    }
    sg_keyboard_visible = !sg_keyboard_visible;
    wifi_apply_kb_visibility();
}

static void wifi_textarea_focus_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (LV_EVENT_CLICKED != code && LV_EVENT_FOCUSED != code) {
        return;
    }
    if (!sg_keyboard_visible) {
        sg_keyboard_visible = true;
        wifi_apply_kb_visibility();
    }
}

lv_obj_t *demo_wifi_screen_create(void)
{
    lv_obj_t *label;
    lv_obj_t *btn;

    sg_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(sg_scr, lv_color_hex(WIFI_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sg_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(sg_scr, LV_OBJ_FLAG_SCROLLABLE);

    label = wifi_create_label(sg_scr, "WiFi Setup", &lv_font_montserrat_16,
                              WIFI_WHITE);
    lv_obj_set_pos(label, 8, 10);

    btn = lv_btn_create(sg_scr);
    lv_obj_set_pos(btn, 430, 4);
    lv_obj_set_size(btn, 44, 28);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(WIFI_CARD), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, wifi_kb_toggle_cb, LV_EVENT_CLICKED, NULL);
    sg_kb_toggle_label = wifi_create_label(btn, LV_SYMBOL_DOWN,
                                           &lv_font_montserrat_14, WIFI_WHITE);
    lv_obj_center(sg_kb_toggle_label);

    sg_roller = lv_roller_create(sg_scr);
    lv_obj_set_pos(sg_roller, 8, 40);
    lv_obj_set_width(sg_roller, 210);
    lv_roller_set_visible_row_count(sg_roller, 4);
    lv_roller_set_options(sg_roller, "Tap RESCAN", LV_ROLLER_MODE_NORMAL);
    lv_obj_set_style_text_font(sg_roller, &lv_font_montserrat_14,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(sg_roller, lv_color_hex(WIFI_WHITE),
                                LV_PART_MAIN);
    lv_obj_set_style_bg_color(sg_roller, lv_color_hex(WIFI_CARD),
                              LV_PART_MAIN);
    lv_obj_set_style_border_width(sg_roller, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(sg_roller, lv_color_hex(0x5D7891),
                                  LV_PART_MAIN);

    sg_rescan_btn = lv_btn_create(sg_scr);
    lv_obj_set_pos(sg_rescan_btn, 8, 142);
    lv_obj_set_size(sg_rescan_btn, 100, 28);
    lv_obj_set_style_radius(sg_rescan_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sg_rescan_btn, lv_color_hex(WIFI_CYAN),
                              LV_PART_MAIN);
    lv_obj_set_style_border_width(sg_rescan_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(sg_rescan_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(sg_rescan_btn, wifi_rescan_cb, LV_EVENT_CLICKED,
                        NULL);
    label = wifi_create_label(sg_rescan_btn, "RESCAN",
                              &lv_font_montserrat_14, WIFI_BG);
    lv_obj_center(label);

    sg_textarea = lv_textarea_create(sg_scr);
    lv_obj_set_pos(sg_textarea, 226, 40);
    lv_obj_set_size(sg_textarea, 246, 36);
    lv_textarea_set_one_line(sg_textarea, true);
    lv_textarea_set_password_mode(sg_textarea, true);
    lv_textarea_set_placeholder_text(sg_textarea, "Enter password...");
    lv_textarea_set_max_length(sg_textarea, WIFI_PASS_MAX - 1);
    lv_obj_set_style_text_font(sg_textarea, &lv_font_montserrat_14,
                               LV_PART_MAIN);
    lv_obj_set_style_radius(sg_textarea, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sg_textarea, lv_color_hex(WIFI_CARD),
                              LV_PART_MAIN);
    lv_obj_set_style_text_color(sg_textarea, lv_color_hex(WIFI_WHITE),
                                LV_PART_MAIN);
    lv_obj_set_style_border_width(sg_textarea, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(sg_textarea, lv_color_hex(0x5D7891),
                                  LV_PART_MAIN);
    lv_obj_add_event_cb(sg_textarea, wifi_textarea_focus_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(sg_textarea, wifi_textarea_focus_cb,
                        LV_EVENT_FOCUSED, NULL);

    sg_connect_btn = lv_btn_create(sg_scr);
    lv_obj_set_pos(sg_connect_btn, 226, 82);
    lv_obj_set_size(sg_connect_btn, 120, 32);
    lv_obj_set_style_radius(sg_connect_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sg_connect_btn, lv_color_hex(WIFI_GREEN),
                              LV_PART_MAIN);
    lv_obj_set_style_border_width(sg_connect_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(sg_connect_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(sg_connect_btn, wifi_connect_cb, LV_EVENT_CLICKED,
                        NULL);
    label = wifi_create_label(sg_connect_btn, "CONNECT",
                              &lv_font_montserrat_14, WIFI_BG);
    lv_obj_center(label);

    sg_status_label = wifi_create_label(sg_scr, "Tap RESCAN to start",
                                        &lv_font_montserrat_14, WIFI_MUTED);
    lv_obj_set_pos(sg_status_label, 226, 122);
    lv_obj_set_width(sg_status_label, 246);
    lv_label_set_long_mode(sg_status_label, LV_LABEL_LONG_WRAP);

    sg_keyboard = lv_keyboard_create(sg_scr);
    lv_obj_set_size(sg_keyboard, 464, 160);
    lv_obj_set_align(sg_keyboard, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(sg_keyboard, 8, 152);
    lv_keyboard_set_textarea(sg_keyboard, sg_textarea);
    lv_obj_set_style_bg_color(sg_keyboard, lv_color_hex(WIFI_CARD),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(sg_keyboard, lv_color_hex(0x1E2D3F),
                              LV_PART_ITEMS);
    lv_obj_set_style_text_color(sg_keyboard, lv_color_hex(WIFI_WHITE),
                                LV_PART_ITEMS);
    lv_obj_set_style_text_font(sg_keyboard, &lv_font_montserrat_14,
                               LV_PART_ITEMS);

    sg_scan_req = false;
    sg_scan_done = false;
    sg_connect_req = false;
    sg_connect_started = false;
    sg_polling = false;
    sg_ap_count = 0U;
    sg_keyboard_visible = true;
    wifi_apply_kb_visibility();

    lv_timer_create(wifi_ui_timer_cb, 100, NULL);

    {
        THREAD_CFG_T cfg = {
            .stackDepth = 1024 * 4,
            .priority = THREAD_PRIO_2,
            .thrdname = "demo_wifi",
        };
        OPERATE_RET rt = tal_thread_create_and_start(&sg_worker, NULL, NULL,
                                                     wifi_worker, NULL, &cfg);
        if (OPRT_OK != rt) {
            PR_ERR("demo wifi worker create failed: %d", rt);
        }
    }

    if (wifi_load_credentials()) {
        wifi_set_status(WIFI_YELLOW, "Auto-connecting...");
        lv_obj_add_state(sg_connect_btn, LV_STATE_DISABLED);
        sg_connect_req = true;
    }

    return sg_scr;
}
