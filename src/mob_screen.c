/**
 * @file mob_screen.c
 * @brief Output-only Playback status and RGB565 video screens.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "lvgl.h"
#include "lv_vendor.h"

#include "mob_screen.h"

#define MOB_COLOR_BACKGROUND lv_color_hex(0x090B0F)
#define MOB_COLOR_SURFACE lv_color_hex(0x151922)
#define MOB_COLOR_SURFACE_ALT lv_color_hex(0x202735)
#define MOB_COLOR_PRIMARY lv_color_hex(0xF5F7FA)
#define MOB_COLOR_MUTED lv_color_hex(0x8D96A8)
#define MOB_COLOR_ACCENT lv_color_hex(0x74F0C5)
#define MOB_COLOR_ACCENT_DARK lv_color_hex(0x183C34)
#define MOB_LVGL_IMAGE_DIMENSION_MAX (2047U)
#define MOB_PANEL_NEUTRALIZE_STEPS (4U)
#define MOB_PANEL_NEUTRALIZE_HOLD_MS (250U)

static lv_obj_t *idle_screen = NULL;
static lv_obj_t *status_screen = NULL;
static lv_obj_t *status_title = NULL;
static lv_obj_t *status_detail = NULL;
static lv_obj_t *video_screen = NULL;
static lv_obj_t *video_image = NULL;
static lv_obj_t *bluetooth_screen = NULL;
static lv_obj_t *bluetooth_list = NULL;
static lv_obj_t *bluetooth_status = NULL;
static lv_obj_t *bluetooth_scan_button = NULL;
static lv_img_dsc_t video_frame_descriptor;
static uint16_t *video_frame_pixels = NULL;
static size_t video_frame_bytes = 0U;
static mob_screen_bluetooth_callbacks_t bluetooth_callbacks;

typedef struct
{
    uint8_t address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES];
} mob_bluetooth_row_t;

static mob_bluetooth_row_t bluetooth_rows[PLAYBACK_BLUETOOTH_BROWSER_MAX_DEVICES];

static bool video_surface_is_valid(const playback_rgb565_surface_t *surface)
{
    size_t expected_pixel_count;

    if ((surface == NULL) || (surface->pixels == NULL) || (surface->width == 0U) || (surface->height == 0U) ||
        (surface->width > MOB_LVGL_IMAGE_DIMENSION_MAX) || (surface->height > MOB_LVGL_IMAGE_DIMENSION_MAX))
    {
        return false;
    }

    expected_pixel_count = (size_t)surface->width * (size_t)surface->height;
    return (surface->pixel_count == expected_pixel_count) &&
           (surface->stride_bytes == ((uint32_t)surface->width * (uint32_t)sizeof(uint16_t)));
}

static bool video_display_matches(const playback_rgb565_surface_t *surface, lv_disp_t **display)
{
    lv_disp_t *default_display = lv_disp_get_default();

    if ((default_display == NULL) ||
        (lv_disp_get_hor_res(default_display) != (lv_coord_t)surface->width) ||
        (lv_disp_get_ver_res(default_display) != (lv_coord_t)surface->height))
    {
        return false;
    }

    *display = default_display;
    return true;
}

static bool ensure_video_frame_buffer(size_t required_bytes)
{
    uint16_t *new_pixels;

    if ((video_frame_pixels != NULL) && (video_frame_bytes == required_bytes))
    {
        return true;
    }

    new_pixels = tal_psram_malloc(required_bytes);
    if (new_pixels == NULL)
    {
        return false;
    }

    if (video_frame_pixels != NULL)
    {
        tal_psram_free(video_frame_pixels);
    }
    video_frame_pixels = new_pixels;
    video_frame_bytes = required_bytes;
    return true;
}

static bool configure_video_screen(const playback_rgb565_surface_t *surface)
{
    video_screen = lv_obj_create(NULL);
    if (video_screen == NULL)
    {
        return false;
    }

    lv_obj_set_style_bg_color(video_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(video_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(video_screen, 0, 0);
    lv_obj_set_style_pad_all(video_screen, 0, 0);
    lv_obj_clear_flag(video_screen, LV_OBJ_FLAG_SCROLLABLE);

    video_image = lv_img_create(video_screen);
    if (video_image == NULL)
    {
        lv_obj_del(video_screen);
        video_screen = NULL;
        return false;
    }

    lv_obj_set_size(video_image, (lv_coord_t)surface->width, (lv_coord_t)surface->height);
    lv_obj_center(video_image);
    return true;
}

static void update_video_descriptor(const playback_rgb565_surface_t *surface)
{
    video_frame_descriptor.header.always_zero = 0U;
    video_frame_descriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
    video_frame_descriptor.header.reserved = 0U;
    video_frame_descriptor.header.w = (uint32_t)surface->width & MOB_LVGL_IMAGE_DIMENSION_MAX;
    video_frame_descriptor.header.h = (uint32_t)surface->height & MOB_LVGL_IMAGE_DIMENSION_MAX;
    video_frame_descriptor.data_size = (uint32_t)video_frame_bytes;
    video_frame_descriptor.data = (const uint8_t *)video_frame_pixels;
}

static bool configure_status_screen(void)
{
    status_screen = lv_obj_create(NULL);
    if (status_screen == NULL)
    {
        return false;
    }
    lv_obj_set_style_bg_color(status_screen, MOB_COLOR_BACKGROUND, 0);
    lv_obj_set_style_bg_opa(status_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_screen, 0, 0);
    lv_obj_set_style_pad_all(status_screen, 28, 0);
    lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_SCROLLABLE);

    status_title = lv_label_create(status_screen);
    status_detail = lv_label_create(status_screen);
    if ((status_title == NULL) || (status_detail == NULL))
    {
        lv_obj_del(status_screen);
        status_screen = NULL;
        status_title = NULL;
        status_detail = NULL;
        return false;
    }

    lv_obj_set_width(status_title, lv_pct(100));
    lv_obj_set_style_text_color(status_title, MOB_COLOR_PRIMARY, 0);
    lv_obj_set_style_text_align(status_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(status_title, 3, 0);
    lv_obj_align(status_title, LV_ALIGN_CENTER, 0, -24);

    lv_obj_set_width(status_detail, lv_pct(100));
    lv_obj_set_style_text_color(status_detail, MOB_COLOR_MUTED, 0);
    lv_obj_set_style_text_align(status_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_detail, LV_ALIGN_CENTER, 0, 20);
    return true;
}

static const char *mob_state_title(playback_state_t state)
{
    switch (state)
    {
        case PLAYBACK_STATE_LOADING:
            return "LOADING";
        case PLAYBACK_STATE_WAITING_SPEAKER:
            return "WAITING SPEAKER";
        case PLAYBACK_STATE_BUFFERING:
            return "BUFFERING";
        case PLAYBACK_STATE_ERROR:
            return "PLAYBACK ERROR";
        default:
            return "READY";
    }
}

static const char *mob_state_detail(playback_state_t state)
{
    switch (state)
    {
        case PLAYBACK_STATE_LOADING:
            return "Preparing media package";
        case PLAYBACK_STATE_WAITING_SPEAKER:
            return "Connecting fixed speaker";
        case PLAYBACK_STATE_BUFFERING:
            return "Buffering audio and video";
        case PLAYBACK_STATE_ERROR:
            return "Check Trigger for details";
        default:
            return "Waiting for Board Link session";
    }
}

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

static void create_header(lv_obj_t *screen)
{
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, lv_pct(88), 48);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 20);
    configure_transparent_container(header);

    lv_obj_t *brand = create_label(header, "PLAYBACK", MOB_COLOR_PRIMARY, LV_TEXT_ALIGN_LEFT);
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
}

static void create_hero_card(lv_obj_t *screen)
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

    lv_obj_t *title = create_label(card, "PLAYBACK\nREADY", MOB_COLOR_PRIMARY, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, -8);

    lv_obj_t *description =
        create_label(card, "Board Link media output\nH.264 video / A2DP audio", MOB_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_line_space(description, 6, 0);
    lv_obj_align(description, LV_ALIGN_BOTTOM_LEFT, 0, -66);

    lv_obj_t *waiting = lv_obj_create(card);
    lv_obj_set_size(waiting, lv_pct(100), 48);
    lv_obj_align(waiting, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(waiting, MOB_COLOR_SURFACE_ALT, 0);
    lv_obj_set_style_bg_opa(waiting, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(waiting, 0, 0);
    lv_obj_set_style_radius(waiting, 16, 0);
    lv_obj_set_style_pad_all(waiting, 0, 0);
    lv_obj_clear_flag(waiting, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *waiting_label =
        create_label(waiting, "WAITING FOR BOARD LINK", MOB_COLOR_ACCENT, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(waiting_label, 1, 0);
    lv_obj_center(waiting_label);
}

static void create_footer(lv_obj_t *screen)
{
    lv_obj_t *footer = create_label(screen, "SWIPE LEFT  ·  BLUETOOTH", MOB_COLOR_MUTED, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(footer, 2, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -18);
}

static void mob_bluetooth_format_address(
    char *destination,
    size_t capacity,
    const uint8_t address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES]
)
{
    if ((destination == NULL) || (capacity == 0U))
    {
        return;
    }
    if (address == NULL)
    {
        destination[0] = '\0';
        return;
    }
    (void)snprintf(
        destination,
        capacity,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        address[5],
        address[4],
        address[3],
        address[2],
        address[1],
        address[0]
    );
}

static void mob_bluetooth_start_scan(void)
{
    if (bluetooth_status != NULL)
    {
        lv_label_set_text(bluetooth_status, "SCANNING  ·  Keep the speaker in pairing mode");
        lv_obj_set_style_text_color(bluetooth_status, MOB_COLOR_ACCENT, 0);
    }
    if (bluetooth_list != NULL)
    {
        lv_obj_clean(bluetooth_list);
        lv_obj_t *label = create_label(
            bluetooth_list,
            "Scanning nearby classic Bluetooth devices...",
            MOB_COLOR_MUTED,
            LV_TEXT_ALIGN_CENTER
        );
        lv_obj_set_width(label, lv_pct(100));
        lv_obj_set_style_pad_top(label, 62, 0);
    }
    if (bluetooth_callbacks.on_scan != NULL)
    {
        bluetooth_callbacks.on_scan(bluetooth_callbacks.context);
    }
}

static void mob_bluetooth_scan_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    {
        mob_bluetooth_start_scan();
    }
}

static void mob_bluetooth_device_event(lv_event_t *event)
{
    mob_bluetooth_row_t *row;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }
    row = lv_event_get_user_data(event);
    if ((row != NULL) && (bluetooth_callbacks.on_connect != NULL))
    {
        char address[24U];
        char status_text[64U];

        mob_bluetooth_format_address(address, sizeof(address), row->address);
        (void)snprintf(
            status_text,
            sizeof(status_text),
            "CONNECTING  ·  %s",
            address
        );
        if (bluetooth_status != NULL)
        {
            lv_label_set_text(bluetooth_status, status_text);
            lv_obj_set_style_text_color(bluetooth_status, MOB_COLOR_ACCENT, 0);
        }
        bluetooth_callbacks.on_connect(
            bluetooth_callbacks.context,
            row->address
        );
    }
}

static void mob_bluetooth_load_page(void)
{
    lv_disp_t *display = lv_disp_get_default();

    if ((display == NULL) || (bluetooth_screen == NULL))
    {
        return;
    }
    lv_disp_load_scr(bluetooth_screen);
    lv_refr_now(display);
    mob_bluetooth_start_scan();
}

static void mob_idle_gesture_event(lv_event_t *event)
{
    lv_indev_t *input;

    if (lv_event_get_code(event) != LV_EVENT_GESTURE)
    {
        return;
    }
    input = lv_indev_get_act();
    if ((input != NULL) && (lv_indev_get_gesture_dir(input) == LV_DIR_LEFT))
    {
        mob_bluetooth_load_page();
    }
}

static void mob_bluetooth_gesture_event(lv_event_t *event)
{
    lv_indev_t *input;
    lv_disp_t *display;

    if (lv_event_get_code(event) != LV_EVENT_GESTURE)
    {
        return;
    }
    input = lv_indev_get_act();
    if ((input == NULL) || (lv_indev_get_gesture_dir(input) != LV_DIR_RIGHT))
    {
        return;
    }
    display = lv_disp_get_default();
    if ((display != NULL) && (idle_screen != NULL))
    {
        lv_disp_load_scr(idle_screen);
        lv_refr_now(display);
    }
}

static bool configure_bluetooth_screen(void)
{
    lv_obj_t *title;
    lv_obj_t *hint;
    lv_obj_t *scan_label;

    bluetooth_screen = lv_obj_create(NULL);
    if (bluetooth_screen == NULL)
    {
        return false;
    }
    lv_obj_set_style_bg_color(bluetooth_screen, MOB_COLOR_BACKGROUND, 0);
    lv_obj_set_style_bg_opa(bluetooth_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bluetooth_screen, 0, 0);
    lv_obj_set_style_pad_all(bluetooth_screen, 0, 0);
    lv_obj_clear_flag(bluetooth_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(
        bluetooth_screen,
        mob_bluetooth_gesture_event,
        LV_EVENT_GESTURE,
        NULL
    );

    title = create_label(
        bluetooth_screen,
        "BLUETOOTH SPEAKERS",
        MOB_COLOR_PRIMARY,
        LV_TEXT_ALIGN_LEFT
    );
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 18);

    hint = create_label(
        bluetooth_screen,
        "Tap a device to pair  ·  Swipe right to return",
        MOB_COLOR_MUTED,
        LV_TEXT_ALIGN_LEFT
    );
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 24, 46);

    bluetooth_scan_button = lv_btn_create(bluetooth_screen);
    lv_obj_set_size(bluetooth_scan_button, 84, 34);
    lv_obj_align(bluetooth_scan_button, LV_ALIGN_TOP_RIGHT, -22, 16);
    lv_obj_set_style_bg_color(bluetooth_scan_button, MOB_COLOR_ACCENT_DARK, 0);
    lv_obj_set_style_radius(bluetooth_scan_button, 17, 0);
    lv_obj_add_event_cb(
        bluetooth_scan_button,
        mob_bluetooth_scan_event,
        LV_EVENT_CLICKED,
        NULL
    );
    scan_label = create_label(
        bluetooth_scan_button,
        "SCAN",
        MOB_COLOR_ACCENT,
        LV_TEXT_ALIGN_CENTER
    );
    lv_obj_center(scan_label);

    bluetooth_status = create_label(
        bluetooth_screen,
        "Ready to scan",
        MOB_COLOR_ACCENT,
        LV_TEXT_ALIGN_LEFT
    );
    lv_obj_set_width(bluetooth_status, lv_pct(90));
    lv_obj_align(bluetooth_status, LV_ALIGN_TOP_LEFT, 24, 72);

    bluetooth_list = lv_obj_create(bluetooth_screen);
    lv_obj_set_size(bluetooth_list, lv_pct(90), 212);
    lv_obj_align(bluetooth_list, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_flex_flow(bluetooth_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(bluetooth_list, MOB_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(bluetooth_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bluetooth_list, MOB_COLOR_SURFACE_ALT, 0);
    lv_obj_set_style_border_width(bluetooth_list, 1, 0);
    lv_obj_set_style_radius(bluetooth_list, 18, 0);
    lv_obj_set_style_pad_all(bluetooth_list, 8, 0);
    lv_obj_set_style_pad_row(bluetooth_list, 6, 0);
    return true;
}

void mob_screen_set_bluetooth_callbacks(
    const mob_screen_bluetooth_callbacks_t *callbacks
)
{
    if (callbacks == NULL)
    {
        memset(&bluetooth_callbacks, 0, sizeof(bluetooth_callbacks));
        return;
    }
    bluetooth_callbacks = *callbacks;
}

void mob_screen_create(void)
{
    lv_obj_t *screen;

    if (idle_screen != NULL)
    {
        lv_disp_load_scr(idle_screen);
        return;
    }

    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, MOB_COLOR_BACKGROUND, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    create_header(screen);
    create_hero_card(screen);
    create_footer(screen);
    lv_obj_add_event_cb(screen, mob_idle_gesture_event, LV_EVENT_GESTURE, NULL);

    idle_screen = screen;
    (void)configure_bluetooth_screen();
    lv_disp_load_scr(screen);
}

void mob_screen_neutralize_panel(void)
{
    lv_disp_t *display;
    lv_obj_t *neutral_screen;
    uint8_t step;

    lv_vendor_disp_lock();
    display = lv_disp_get_default();
    if (display == NULL)
    {
        lv_vendor_disp_unlock();
        return;
    }

    neutral_screen = lv_obj_create(NULL);
    if (neutral_screen == NULL)
    {
        lv_vendor_disp_unlock();
        return;
    }

    lv_obj_set_style_border_width(neutral_screen, 0, 0);
    lv_obj_set_style_pad_all(neutral_screen, 0, 0);
    lv_obj_clear_flag(neutral_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_disp_load_scr(neutral_screen);
    lv_vendor_disp_unlock();

    for (step = 0U; step < MOB_PANEL_NEUTRALIZE_STEPS; ++step)
    {
        lv_vendor_disp_lock();
        lv_obj_set_style_bg_color(
            neutral_screen,
            ((step & 1U) == 0U) ? lv_color_white() : lv_color_black(),
            0
        );
        lv_obj_set_style_bg_opa(neutral_screen, LV_OPA_COVER, 0);
        lv_obj_invalidate(neutral_screen);
        lv_refr_now(display);
        lv_vendor_disp_unlock();
        tal_system_sleep(MOB_PANEL_NEUTRALIZE_HOLD_MS);
    }

    lv_vendor_disp_lock();
    if (idle_screen != NULL)
    {
        lv_disp_load_scr(idle_screen);
        lv_refr_now(display);
    }
    lv_obj_del(neutral_screen);
    lv_vendor_disp_unlock();
}

void mob_screen_show_bluetooth_devices(
    const playback_bluetooth_browser_device_t *devices,
    size_t device_count,
    bool scanning
)
{
    lv_disp_t *display;
    size_t index;

    lv_vendor_disp_lock();
    display = lv_disp_get_default();
    if ((display == NULL) || (bluetooth_list == NULL))
    {
        lv_vendor_disp_unlock();
        return;
    }

    lv_obj_clean(bluetooth_list);
    memset(bluetooth_rows, 0, sizeof(bluetooth_rows));
    if ((devices == NULL) || (device_count == 0U))
    {
        lv_obj_t *empty = create_label(
            bluetooth_list,
            scanning ? "Scanning... keep the speaker in pairing mode" : "No classic Bluetooth devices found",
            MOB_COLOR_MUTED,
            LV_TEXT_ALIGN_CENTER
        );
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_pad_top(empty, 62, 0);
    }
    else
    {
        if (device_count > PLAYBACK_BLUETOOTH_BROWSER_MAX_DEVICES)
        {
            device_count = PLAYBACK_BLUETOOTH_BROWSER_MAX_DEVICES;
        }
        for (index = 0U; index < device_count; ++index)
        {
            lv_obj_t *button;
            lv_obj_t *name;
            lv_obj_t *detail;
            char address[24U];
            char detail_text[64U];

            memcpy(
                bluetooth_rows[index].address,
                devices[index].address,
                sizeof(bluetooth_rows[index].address)
            );
            mob_bluetooth_format_address(
                address,
                sizeof(address),
                devices[index].address
            );
            (void)snprintf(
                detail_text,
                sizeof(detail_text),
                "%s  ·  RSSI %d%s",
                address,
                (int)devices[index].rssi,
                devices[index].audio_device ? "  ·  AUDIO" : ""
            );

            button = lv_btn_create(bluetooth_list);
            lv_obj_set_size(button, lv_pct(100), 58);
            lv_obj_set_style_bg_color(button, MOB_COLOR_SURFACE_ALT, 0);
            lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(button, 14, 0);
            lv_obj_set_style_shadow_width(button, 0, 0);
            lv_obj_add_event_cb(
                button,
                mob_bluetooth_device_event,
                LV_EVENT_CLICKED,
                &bluetooth_rows[index]
            );

            name = create_label(
                button,
                devices[index].name,
                MOB_COLOR_PRIMARY,
                LV_TEXT_ALIGN_LEFT
            );
            lv_obj_set_width(name, lv_pct(96));
            lv_obj_align(name, LV_ALIGN_TOP_LEFT, 2, 4);

            detail = create_label(
                button,
                detail_text,
                MOB_COLOR_MUTED,
                LV_TEXT_ALIGN_LEFT
            );
            lv_obj_set_width(detail, lv_pct(96));
            lv_obj_align(detail, LV_ALIGN_BOTTOM_LEFT, 2, -4);
        }
    }
    if (lv_disp_get_scr_act(display) == bluetooth_screen)
    {
        lv_refr_now(display);
    }
    lv_vendor_disp_unlock();
}

void mob_screen_show_bluetooth_status(
    playback_bluetooth_browser_status_t status,
    const uint8_t address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES],
    const char *detail
)
{
    lv_disp_t *display;
    char address_text[24U];
    char status_text[112U];
    const char *status_name = playback_bluetooth_browser_status_name(status);

    mob_bluetooth_format_address(address_text, sizeof(address_text), address);
    if ((detail != NULL) && (detail[0] != '\0'))
    {
        (void)snprintf(
            status_text,
            sizeof(status_text),
            "%s%s%s  ·  %s",
            status_name,
            address_text[0] != '\0' ? "  ·  " : "",
            address_text,
            detail
        );
    }
    else
    {
        (void)snprintf(
            status_text,
            sizeof(status_text),
            "%s%s%s",
            status_name,
            address_text[0] != '\0' ? "  ·  " : "",
            address_text
        );
    }

    lv_vendor_disp_lock();
    display = lv_disp_get_default();
    if ((display != NULL) && (bluetooth_status != NULL))
    {
        lv_label_set_text(bluetooth_status, status_text);
        lv_obj_set_style_text_color(
            bluetooth_status,
            (status == PLAYBACK_BLUETOOTH_BROWSER_FAILED)
                ? lv_color_hex(0xFF7A7A)
                : MOB_COLOR_ACCENT,
            0
        );
        if (lv_disp_get_scr_act(display) == bluetooth_screen)
        {
            lv_refr_now(display);
        }
    }
    lv_vendor_disp_unlock();
}

void mob_screen_show_state(playback_state_t state, const char *diagnostic)
{
    lv_disp_t *display;
    const char *detail;

    if ((state == PLAYBACK_STATE_PLAYING) ||
        (state == PLAYBACK_STATE_PAUSED) ||
        (state == PLAYBACK_STATE_COMPLETED) ||
        (state == PLAYBACK_STATE_SEEKING))
    {
        return;
    }

    lv_vendor_disp_lock();
    display = lv_disp_get_default();
    if (display == NULL)
    {
        lv_vendor_disp_unlock();
        return;
    }

    if (state == PLAYBACK_STATE_IDLE)
    {
        if (idle_screen != NULL)
        {
            lv_disp_load_scr(idle_screen);
            lv_refr_now(display);
        }
        lv_vendor_disp_unlock();
        return;
    }

    if ((status_screen == NULL) && !configure_status_screen())
    {
        lv_vendor_disp_unlock();
        return;
    }

    detail = ((diagnostic != NULL) && (diagnostic[0] != '\0'))
                 ? diagnostic
                 : mob_state_detail(state);
    lv_label_set_text(status_title, mob_state_title(state));
    lv_label_set_text(status_detail, detail);
    lv_disp_load_scr(status_screen);
    lv_refr_now(display);
    lv_vendor_disp_unlock();
}

playback_video_output_result_t mob_screen_present_rgb565(
    void *context,
    const playback_rgb565_surface_t *surface
)
{
#if (LV_COLOR_DEPTH != 16) || (LV_COLOR_16_SWAP != 0)
    (void)context;
    (void)surface;
    return PLAYBACK_VIDEO_OUTPUT_UNSUPPORTED_CONFIG;
#else
    lv_disp_t *display = NULL;
    size_t required_bytes;

    (void)context;

    if (surface == NULL)
    {
        return PLAYBACK_VIDEO_OUTPUT_INVALID_ARGUMENT;
    }
    if (!video_surface_is_valid(surface))
    {
        return PLAYBACK_VIDEO_OUTPUT_INVALID_FRAME;
    }

    lv_vendor_disp_lock();

    if (!video_display_matches(surface, &display))
    {
        lv_vendor_disp_unlock();
        return PLAYBACK_VIDEO_OUTPUT_UNSUPPORTED_CONFIG;
    }

    required_bytes = surface->pixel_count * sizeof(uint16_t);
    if (!ensure_video_frame_buffer(required_bytes))
    {
        lv_vendor_disp_unlock();
        return PLAYBACK_VIDEO_OUTPUT_NO_MEMORY;
    }

    memcpy(video_frame_pixels, surface->pixels, required_bytes);

    if ((video_screen == NULL) && !configure_video_screen(surface))
    {
        lv_vendor_disp_unlock();
        return PLAYBACK_VIDEO_OUTPUT_NO_MEMORY;
    }

    update_video_descriptor(surface);
    lv_img_set_src(video_image, &video_frame_descriptor);

    if (lv_disp_get_scr_act(display) != video_screen)
    {
        lv_disp_load_scr(video_screen);
    }
    else
    {
        lv_obj_invalidate(video_image);
    }

    lv_refr_now(display);
    lv_vendor_disp_unlock();
    return PLAYBACK_VIDEO_OUTPUT_OK;
#endif
}
