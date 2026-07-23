/**
 * @file mob_screen.c
 * @brief Initial MOB screen composition using LVGL 8 primitives only.
 */

#include <stdbool.h>
#include <stddef.h>

#include "tal_api.h"
#include "lvgl.h"

#include "mob_screen.h"

#define MOB_COLOR_BACKGROUND lv_color_hex(0x090B0F)
#define MOB_COLOR_SURFACE lv_color_hex(0x151922)
#define MOB_COLOR_SURFACE_ALT lv_color_hex(0x202735)
#define MOB_COLOR_PRIMARY lv_color_hex(0xF5F7FA)
#define MOB_COLOR_MUTED lv_color_hex(0x8D96A8)
#define MOB_COLOR_ACCENT lv_color_hex(0x74F0C5)
#define MOB_COLOR_ACCENT_DARK lv_color_hex(0x183C34)

static lv_obj_t *touch_status_label = NULL;
static lv_obj_t *touch_button_label = NULL;
static bool touch_confirmed = false;

static void configure_transparent_container(lv_obj_t *object)
{
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, lv_color_t color, lv_text_align_t alignment)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, alignment, 0);
    return label;
}

static void handle_touch_test(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_target(event);

    touch_confirmed = !touch_confirmed;

    lv_label_set_text(touch_status_label, touch_confirmed ? "TOUCH OK" : "READY");
    lv_label_set_text(touch_button_label, touch_confirmed ? "TOUCH INPUT OK" : "TAP TO TEST TOUCH");
    lv_obj_set_style_bg_color(button, touch_confirmed ? MOB_COLOR_ACCENT_DARK : MOB_COLOR_SURFACE_ALT, 0);

    PR_NOTICE("MOB touch %s", touch_confirmed ? "confirmed" : "reset");
}

static lv_obj_t *create_header(lv_obj_t *screen)
{
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, lv_pct(88), 48);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 20);
    configure_transparent_container(header);

    lv_obj_t *brand = create_label(header, "MOB", MOB_COLOR_PRIMARY, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(brand, 4, 0);
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *status = lv_obj_create(header);
    lv_obj_set_size(status, 82, 32);
    lv_obj_align(status, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(status, MOB_COLOR_ACCENT_DARK, 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_radius(status, 16, 0);
    lv_obj_set_style_pad_all(status, 0, 0);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *status_label = create_label(status, "READY", MOB_COLOR_ACCENT, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(status_label, 1, 0);
    lv_obj_center(status_label);

    return status_label;
}

static void create_hero_card(lv_obj_t *screen, lv_obj_t *status_label)
{
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_size(card, lv_pct(88), lv_pct(62));
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 12);
    lv_obj_set_style_bg_color(card, MOB_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, MOB_COLOR_SURFACE_ALT, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 26, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *signal = lv_obj_create(card);
    lv_obj_set_size(signal, 54, 6);
    lv_obj_align(signal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(signal, MOB_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(signal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(signal, 0, 0);
    lv_obj_set_style_radius(signal, 3, 0);
    lv_obj_set_style_pad_all(signal, 0, 0);
    lv_obj_clear_flag(signal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *eyebrow = create_label(card, "TUYA T5AI", MOB_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(eyebrow, 2, 0);
    lv_obj_align(eyebrow, LV_ALIGN_TOP_LEFT, 0, 30);

    lv_obj_t *title = create_label(card, "DISPLAY\nREADY", MOB_COLOR_PRIMARY, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, -8);

    lv_obj_t *description =
        create_label(card, "Custom LVGL interface\n320 x 480 / GT1151 touch", MOB_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_line_space(description, 6, 0);
    lv_obj_align(description, LV_ALIGN_BOTTOM_LEFT, 0, -66);

    lv_obj_t *touch_button = lv_btn_create(card);
    lv_obj_set_size(touch_button, lv_pct(100), 48);
    lv_obj_align(touch_button, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(touch_button, MOB_COLOR_SURFACE_ALT, 0);
    lv_obj_set_style_bg_opa(touch_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(touch_button, 0, 0);
    lv_obj_set_style_radius(touch_button, 16, 0);
    lv_obj_set_style_pad_all(touch_button, 0, 0);
    lv_obj_add_event_cb(touch_button, handle_touch_test, LV_EVENT_CLICKED, NULL);

    touch_status_label = status_label;
    touch_button_label = create_label(touch_button, "TAP TO TEST TOUCH", MOB_COLOR_ACCENT, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(touch_button_label, 1, 0);
    lv_obj_center(touch_button_label);
}

static void create_footer(lv_obj_t *screen)
{
    lv_obj_t *footer = create_label(screen, "CUSTOM DISPLAY PROTOTYPE", MOB_COLOR_MUTED, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(footer, 2, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -18);
}

void mob_screen_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, MOB_COLOR_BACKGROUND, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    touch_confirmed = false;
    touch_status_label = create_header(screen);
    create_hero_card(screen, touch_status_label);
    create_footer(screen);

    lv_disp_load_scr(screen);
}
