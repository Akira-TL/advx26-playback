/**
 * @file mob_screen.c
 * @brief Output-only Playback status and RGB565 video screens.
 */

#include <stdbool.h>
#include <stddef.h>
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
static lv_img_dsc_t video_frame_descriptor;
static uint16_t *video_frame_pixels = NULL;
static size_t video_frame_bytes = 0U;

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
    lv_obj_t *footer = create_label(screen, "SOUNDPOLA MEDIA OUTPUT", MOB_COLOR_MUTED, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(footer, 2, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -18);
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

    idle_screen = screen;
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
