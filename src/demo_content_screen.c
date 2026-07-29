/**
 * @file demo_content_screen.c
 * @brief Cloud content list + tap-to-play screen for the slave demo (LVGL v8).
 */

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "lvgl.h"

#include "demo_cloud.h"
#include "demo_ui.h"

#define CONT_BG     0x071426
#define CONT_CARD   0x173852
#define CONT_WHITE  0xF7FBFF
#define CONT_MUTED  0xA9C1D7
#define CONT_CYAN   0x48D8FF
#define CONT_GREEN  0x48E0A4
#define CONT_YELLOW 0xFFD166
#define CONT_RED    0xFF6B7A

static lv_obj_t *sg_scr;
static lv_obj_t *sg_list;
static lv_obj_t *sg_status_label;
static lv_obj_t *sg_refresh_btn;

static THREAD_HANDLE sg_worker;

static demo_cloud_item_t sg_items[DEMO_CLOUD_MAX_ITEMS];
static int sg_item_count;
static demo_cloud_result_t sg_last_result;

static char sg_play_content_id[PLAYBACK_CONTENT_ID_MAX_LEN + 1U];
static uint32_t sg_sequence_id;

static volatile bool sg_list_req;
static volatile bool sg_list_done;
static volatile bool sg_play_req;
static volatile bool sg_play_started;
static volatile bool sg_play_failed;

static void cont_set_status(uint32_t color, const char *text)
{
    lv_obj_set_style_text_color(sg_status_label, lv_color_hex(color),
                                LV_PART_MAIN);
    lv_label_set_text(sg_status_label, text);
}

static const char *cont_result_text(demo_cloud_result_t result)
{
    switch (result) {
    case DEMO_CLOUD_NOT_PAIRED:
        return "Not paired - scan QR first";
    case DEMO_CLOUD_NET_ERROR:
        return "Network error";
    case DEMO_CLOUD_AUTH_ERROR:
        return "Auth rejected (401/403)";
    case DEMO_CLOUD_BAD_RESPONSE:
        return "Bad server response";
    default:
        return "OK";
    }
}

static void cont_item_click_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);

    if (LV_EVENT_CLICKED != lv_event_get_code(e)) {
        return;
    }
    if (sg_play_req || sg_list_req || idx < 0 || idx >= sg_item_count) {
        return;
    }
    if (!sg_items[idx].ready) {
        cont_set_status(CONT_YELLOW, "Content not ready yet");
        return;
    }
    snprintf(sg_play_content_id, sizeof(sg_play_content_id), "%s",
             sg_items[idx].content_id);
    cont_set_status(CONT_YELLOW, "Loading session...");
    sg_play_req = true;
}

static void cont_rebuild_list(void)
{
    int i;

    lv_obj_clean(sg_list);
    if (sg_item_count == 0) {
        lv_obj_t *label = lv_list_add_text(sg_list, "No content");

        lv_obj_set_style_text_color(label, lv_color_hex(CONT_MUTED),
                                    LV_PART_MAIN);
        return;
    }
    for (i = 0; i < sg_item_count; i++) {
        char line[80];
        lv_obj_t *btn;

        snprintf(line, sizeof(line), "%s  (%us)%s", sg_items[i].label,
                 (unsigned)(sg_items[i].duration_ms / 1000U),
                 sg_items[i].ready ? "" : "  [not ready]");
        btn = lv_list_add_btn(sg_list, LV_SYMBOL_PLAY, line);
        lv_obj_set_style_bg_color(btn, lv_color_hex(CONT_CARD), LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, lv_color_hex(
            sg_items[i].ready ? CONT_WHITE : CONT_MUTED), LV_PART_MAIN);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, cont_item_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
}

static void cont_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (sg_list_done) {
        sg_list_done = false;
        lv_obj_clear_state(sg_refresh_btn, LV_STATE_DISABLED);
        if (sg_last_result == DEMO_CLOUD_OK) {
            char msg[48];

            cont_rebuild_list();
            snprintf(msg, sizeof(msg), "%d item(s) - tap to play",
                     sg_item_count);
            cont_set_status(CONT_GREEN, msg);
        } else {
            cont_set_status(CONT_RED, cont_result_text(sg_last_result));
        }
    }

    if (sg_play_started) {
        sg_play_started = false;
        cont_set_status(CONT_GREEN, "Playing...");
    }

    if (sg_play_failed) {
        sg_play_failed = false;
        cont_set_status(CONT_RED, cont_result_text(sg_last_result));
    }
}

static void cont_start_playback(void)
{
    /* Static: this worker is single-threaded and these are too large for
     * the thread stack alongside the mbedTLS handshake frames. */
    static playback_session_t session;
    static playback_command_t command;
    demo_cloud_result_t result;
    playback_engine_result_t engine_result;

    memset(&session, 0, sizeof(session));
    result = demo_cloud_fetch_session(sg_play_content_id, &session);
    if (result != DEMO_CLOUD_OK) {
        sg_last_result = result;
        sg_play_failed = true;
        return;
    }

    memset(&command, 0, sizeof(command));
    command.schema_version = PLAYBACK_PROTOCOL_SCHEMA_VERSION;
    command.kind = PLAYBACK_COMMAND_LOAD_SESSION;
    snprintf(command.session_id, sizeof(command.session_id), "%s",
             session.session_id);
    command.sequence_id = ++sg_sequence_id;
    command.payload.session = session;

    engine_result = playback_engine_submit(demo_ui_engine(), &command);
    if (engine_result != PLAYBACK_ENGINE_OK) {
        PR_ERR("demo content: submit failed: %s",
               playback_engine_result_name(engine_result));
        sg_last_result = DEMO_CLOUD_BAD_RESPONSE;
        sg_play_failed = true;
        return;
    }
    PR_NOTICE("demo content: submitted LOAD_SESSION for %s",
              sg_play_content_id);
    sg_play_started = true;
}

static void cont_worker(void *arg)
{
    (void)arg;

    while (1) {
        if (sg_list_req) {
            sg_last_result = demo_cloud_fetch_list(sg_items,
                                                   DEMO_CLOUD_MAX_ITEMS,
                                                   &sg_item_count);
            sg_list_req = false;
            sg_list_done = true;
        } else if (sg_play_req) {
            cont_start_playback();
            sg_play_req = false;
        } else {
            tal_system_sleep(50);
        }
    }
}

static void cont_request_list(void)
{
    if (sg_list_req) {
        return;
    }
    cont_set_status(CONT_YELLOW, "Fetching content list...");
    lv_obj_add_state(sg_refresh_btn, LV_STATE_DISABLED);
    sg_list_req = true;
}

static void cont_refresh_cb(lv_event_t *e)
{
    if (LV_EVENT_CLICKED != lv_event_get_code(e)) {
        return;
    }
    cont_request_list();
}

static void cont_loaded_cb(lv_event_t *e)
{
    (void)e;
    cont_request_list();
}

lv_obj_t *demo_content_screen_create(void)
{
    lv_obj_t *label;

    sg_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(sg_scr, lv_color_hex(CONT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sg_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(sg_scr, LV_OBJ_FLAG_SCROLLABLE);

    label = lv_label_create(sg_scr);
    lv_label_set_text(label, "Cloud Content");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(CONT_WHITE),
                                LV_PART_MAIN);
    lv_obj_set_pos(label, 8, 10);

    sg_refresh_btn = lv_btn_create(sg_scr);
    lv_obj_set_pos(sg_refresh_btn, 372, 4);
    lv_obj_set_size(sg_refresh_btn, 100, 28);
    lv_obj_set_style_radius(sg_refresh_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sg_refresh_btn, lv_color_hex(CONT_CYAN),
                              LV_PART_MAIN);
    lv_obj_set_style_border_width(sg_refresh_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(sg_refresh_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(sg_refresh_btn, cont_refresh_cb, LV_EVENT_CLICKED,
                        NULL);
    label = lv_label_create(sg_refresh_btn);
    lv_label_set_text(label, "REFRESH");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(CONT_BG), LV_PART_MAIN);
    lv_obj_center(label);

    sg_list = lv_list_create(sg_scr);
    lv_obj_set_pos(sg_list, 8, 40);
    lv_obj_set_size(sg_list, 464, 236);
    lv_obj_set_style_bg_color(sg_list, lv_color_hex(CONT_CARD),
                              LV_PART_MAIN);
    lv_obj_set_style_border_width(sg_list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(sg_list, lv_color_hex(0x5D7891),
                                  LV_PART_MAIN);
    lv_obj_set_style_radius(sg_list, 8, LV_PART_MAIN);

    sg_status_label = lv_label_create(sg_scr);
    lv_label_set_text(sg_status_label, "Waiting...");
    lv_obj_set_style_text_font(sg_status_label, &lv_font_montserrat_14,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(sg_status_label, lv_color_hex(CONT_MUTED),
                                LV_PART_MAIN);
    lv_obj_set_pos(sg_status_label, 8, 288);
    lv_obj_set_width(sg_status_label, 464);
    lv_label_set_long_mode(sg_status_label, LV_LABEL_LONG_DOT);

    lv_obj_add_event_cb(sg_scr, cont_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

    lv_timer_create(cont_ui_timer_cb, 250, NULL);

    {
        THREAD_CFG_T cfg = {
            .stackDepth = 1024 * 20,
            .priority = THREAD_PRIO_2,
            .thrdname = "demo_cont",
        };
        OPERATE_RET rt = tal_thread_create_and_start(&sg_worker, NULL, NULL,
                                                     cont_worker, NULL, &cfg);
        if (OPRT_OK != rt) {
            PR_ERR("demo content worker create failed: %d", rt);
        }
    }

    return sg_scr;
}
