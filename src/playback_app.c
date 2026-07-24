/**
 * @file playback_app.c
 * @brief Concrete Playback application composition for T5AI.
 */

#include "playback_app.h"

#include <stdbool.h>
#include <string.h>

#include "board_com_api.h"
#include "demo_network_config.h"
#include "lv_vendor.h"
#include "lvgl.h"
#include "mob_screen.h"
#include "playback_board_link_gatt.h"
#include "playback_engine.h"
#include "playback_network.h"
#include "tal_api.h"
#include "tkl_output.h"

#if defined(TUYA_T5AI_BOARD_LCD_35565) && (TUYA_T5AI_BOARD_LCD_35565 == 1)
#include "tdd_disp_ili9488.h"
#endif

#ifndef DEMO_SPEAKER_ADDRESS
#define DEMO_SPEAKER_ADDRESS {0U, 0U, 0U, 0U, 0U, 0U}
#endif

#ifndef DEMO_HTTP_TLS_NO_VERIFY
#define DEMO_HTTP_TLS_NO_VERIFY (0)
#endif

typedef struct
{
    playback_board_link_gatt_t gatt;
    playback_engine_t engine;
    bool started;
} playback_app_state_t;

static playback_app_state_t playback_app_state;
static const uint8_t playback_app_speaker_address[PLAYBACK_SPEAKER_LINK_ADDRESS_BYTES] =
    DEMO_SPEAKER_ADDRESS;

#if defined(TUYA_T5AI_BOARD_LCD_35565) && (TUYA_T5AI_BOARD_LCD_35565 == 1)
static const uint8_t playback_ili9488_init_sequence[] = {
    1, 120, ILI9488_SWRESET,
    3, 0, ILI9488_PWCTR1, 0x0E, 0x0E,
    2, 0, ILI9488_PWCTR2, 0x46,
    4, 0, ILI9488_VMCTR1, 0x00, 0x2D, 0x80,
    2, 0, ILI9488_IFMODE, 0x00,
    2, 0, ILI9488_FRMCTR1, 0xA0,
    2, 0, ILI9488_INVCTR, 0x02,
    5, 0, ILI9488_PRCTR, 0x08, 0x0C, 0x50, 0x64,
    3, 0, ILI9488_DFUNCTR, 0x32, 0x02,
    2, 0, ILI9488_MADCTL, 0x48,
    2, 0, ILI9488_PIXFMT, 0x70,
    1, 0, ILI9488_INVON,
    2, 0, ILI9488_SETIMAGE, 0x01,
    5, 0, ILI9488_ACTRL3, 0xA9, 0x51, 0x2C, 0x82,
    3, 0, ILI9488_ACTRL4, 0x21, 0x05,
    16, 0, ILI9488_GMCTRP1, 0x00, 0x0C, 0x10, 0x03, 0x0F, 0x05, 0x37, 0x66,
        0x4D, 0x03, 0x0C, 0x0A, 0x2F, 0x35, 0x0F,
    16, 0, ILI9488_GMCTRN1, 0x00, 0x0F, 0x16, 0x06, 0x13, 0x07, 0x3B, 0x35,
        0x51, 0x07, 0x10, 0x0D, 0x36, 0x3B, 0x0F,
    1, 120, ILI9488_SLPOUT,
    1, 20, ILI9488_DISPON,
    0,
};

static OPERATE_RET playback_app_prepare_display_panel(void)
{
    return tdd_disp_rgb_ili9488_set_init_seq(playback_ili9488_init_sequence);
}
#else
static OPERATE_RET playback_app_prepare_display_panel(void)
{
    return OPRT_OK;
}
#endif

static bool playback_app_speaker_configured(void)
{
    uint8_t index;
    uint8_t value = 0U;

    for (index = 0U; index < PLAYBACK_SPEAKER_LINK_ADDRESS_BYTES; ++index)
    {
        value |= playback_app_speaker_address[index];
    }
    return value != 0U;
}

static void playback_app_show_report(const playback_report_t *report)
{
    if (report == NULL)
    {
        return;
    }

    switch (report->kind)
    {
        case PLAYBACK_REPORT_STATE:
        case PLAYBACK_REPORT_ERROR:
        case PLAYBACK_REPORT_COMPLETED:
            mob_screen_show_state(report->state, report->diagnostic);
            break;
        default:
            break;
    }
}

static void playback_app_report_sink(void *context, const playback_report_t *report)
{
    playback_app_state_t *state = context;
    playback_board_link_gatt_result_t result;

    if ((state == NULL) || (report == NULL))
    {
        return;
    }

    playback_app_show_report(report);
    result = playback_board_link_gatt_send_report(&state->gatt, report);
    if ((result != PLAYBACK_BOARD_LINK_GATT_OK) &&
        (result != PLAYBACK_BOARD_LINK_GATT_NOT_CONNECTED) &&
        (result != PLAYBACK_BOARD_LINK_GATT_NOT_SUBSCRIBED))
    {
        PR_WARN("Board Link report failed: %s", playback_board_link_gatt_result_name(result));
    }
}

static void playback_app_send_submit_nack(
    playback_app_state_t *state,
    const playback_command_t *command,
    playback_engine_result_t result
)
{
    playback_report_t report;

    memset(&report, 0, sizeof(report));
    report.kind = PLAYBACK_REPORT_NACK;
    report.sequence_id = command->sequence_id;
    report.acknowledged_command = command->kind;
    report.nack = PLAYBACK_NACK_INVALID_STATE;
    strncpy(report.session_id, command->session_id, sizeof(report.session_id) - 1U);
    strncpy(
        report.diagnostic,
        playback_engine_result_name(result),
        sizeof(report.diagnostic) - 1U
    );
    (void)playback_board_link_gatt_send_report(&state->gatt, &report);
}

static void playback_app_command_callback(
    void *context,
    const playback_command_t *command
)
{
    playback_app_state_t *state = context;
    playback_engine_result_t result;

    if ((state == NULL) || (command == NULL))
    {
        return;
    }

    result = playback_engine_submit(&state->engine, command);
    if (result != PLAYBACK_ENGINE_OK)
    {
        PR_ERR("Playback command enqueue failed: %s", playback_engine_result_name(result));
        playback_app_send_submit_nack(state, command, result);
    }
}

static void playback_app_link_status_callback(
    void *context,
    const playback_board_link_gatt_status_t *status
)
{
    (void)context;

    if (status != NULL)
    {
        PR_DEBUG(
            "Board Link connected=%u subscribed=%u handshake=%u mtu=%u",
            status->connected ? 1U : 0U,
            status->subscribed ? 1U : 0U,
            status->handshake_complete ? 1U : 0U,
            status->att_mtu
        );
    }
}

static void playback_app_log_information(void)
{
    PR_NOTICE("Application information:");
    PR_NOTICE("Project name:        %s", PROJECT_NAME);
    PR_NOTICE("App version:         %s", PROJECT_VERSION);
    PR_NOTICE("Compile time:        %s", __DATE__);
    PR_NOTICE("TuyaOpen version:    %s", OPEN_VERSION);
    PR_NOTICE("TuyaOpen commit-id:  %s", OPEN_COMMIT);
    PR_NOTICE("Platform chip:       %s", PLATFORM_CHIP);
    PR_NOTICE("Platform board:      %s", PLATFORM_BOARD);
    PR_NOTICE("Platform commit-id:  %s", PLATFORM_COMMIT);
}

OPERATE_RET playback_app_start(void)
{
    playback_board_link_gatt_config_t gatt_config;
    playback_board_link_gatt_status_t gatt_status;
    playback_engine_config_t engine_config;
    playback_board_link_gatt_result_t gatt_result;
    playback_engine_result_t engine_result;
    OPERATE_RET result;

    if (playback_app_state.started)
    {
        return OPRT_OK;
    }

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 4096U, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    playback_app_log_information();

    result = playback_app_prepare_display_panel();
    if (result != OPRT_OK)
    {
        PR_ERR("Playback display panel preparation failed: %d", result);
        return result;
    }

    board_register_hardware();
    lv_vendor_init(DISPLAY_NAME);
    lv_vendor_disp_lock();
    mob_screen_create();
    lv_vendor_disp_unlock();
    lv_vendor_start(5U, 1024U * 8U);
    mob_screen_neutralize_panel();

    result = playback_network_start();
    if (result != OPRT_OK)
    {
        PR_ERR("Playback network initialization failed: %d", result);
        mob_screen_show_state(PLAYBACK_STATE_ERROR, "Wi-Fi initialization failed");
        return result;
    }

    memset(&gatt_config, 0, sizeof(gatt_config));
    gatt_config.on_command = playback_app_command_callback;
    gatt_config.on_status = playback_app_link_status_callback;
    gatt_config.context = &playback_app_state;
    gatt_result = playback_board_link_gatt_init(&playback_app_state.gatt, &gatt_config);
    if (gatt_result != PLAYBACK_BOARD_LINK_GATT_OK)
    {
        PR_ERR("Board Link initialization failed: %s", playback_board_link_gatt_result_name(gatt_result));
        mob_screen_show_state(PLAYBACK_STATE_ERROR, "Board Link initialization failed");
        return OPRT_COM_ERROR;
    }

    gatt_result = playback_board_link_gatt_get_status(
        &playback_app_state.gatt,
        &gatt_status
    );
    if (gatt_result != PLAYBACK_BOARD_LINK_GATT_OK)
    {
        playback_board_link_gatt_close(&playback_app_state.gatt);
        return OPRT_COM_ERROR;
    }

    memset(&engine_config, 0, sizeof(engine_config));
    strncpy(engine_config.boot_id, gatt_status.boot_id, sizeof(engine_config.boot_id) - 1U);
    engine_config.scheduler.http.timeout_ms = PLAYBACK_HTTP_DEFAULT_TIMEOUT_MS;
    engine_config.scheduler.http.tls_no_verify = DEMO_HTTP_TLS_NO_VERIFY != 0;
    memcpy(
        engine_config.scheduler.speaker_address,
        playback_app_speaker_address,
        sizeof(engine_config.scheduler.speaker_address)
    );
    engine_config.scheduler.speaker_latency_ms =
        PLAYBACK_SCHEDULER_DEFAULT_SPEAKER_LATENCY_MS;
    engine_config.scheduler.video_output.rotation = PLAYBACK_VIDEO_ROTATION_0;
    engine_config.scheduler.video_output.swap_rgb565_bytes = false;
    engine_config.scheduler.present_video = mob_screen_present_rgb565;
    engine_config.scheduler.video_context = NULL;
    engine_config.report_sink = playback_app_report_sink;
    engine_config.report_context = &playback_app_state;

    engine_result = playback_engine_init(&playback_app_state.engine, &engine_config);
    if (engine_result != PLAYBACK_ENGINE_OK)
    {
        PR_ERR("Playback engine initialization failed: %s", playback_engine_result_name(engine_result));
        playback_board_link_gatt_close(&playback_app_state.gatt);
        mob_screen_show_state(PLAYBACK_STATE_ERROR, "Playback engine initialization failed");
        return OPRT_COM_ERROR;
    }

    gatt_result = playback_board_link_gatt_start(&playback_app_state.gatt);
    if (gatt_result != PLAYBACK_BOARD_LINK_GATT_OK)
    {
        PR_ERR("Board Link start failed: %s", playback_board_link_gatt_result_name(gatt_result));
        playback_engine_close(&playback_app_state.engine);
        playback_board_link_gatt_close(&playback_app_state.gatt);
        mob_screen_show_state(PLAYBACK_STATE_ERROR, "Board Link start failed");
        return OPRT_COM_ERROR;
    }

    playback_app_state.started = true;
    mob_screen_show_state(PLAYBACK_STATE_IDLE, NULL);
    if (!playback_app_speaker_configured())
    {
        PR_WARN("Fixed speaker address is not configured; define DEMO_SPEAKER_ADDRESS in demo_network_config.h");
    }
    PR_NOTICE("Playback application ready; media starts only from Board Link LOAD_SESSION");
    return OPRT_OK;
}

void playback_app_stop(void)
{
    if (!playback_app_state.started)
    {
        return;
    }

    playback_engine_close(&playback_app_state.engine);
    playback_board_link_gatt_close(&playback_app_state.gatt);
    playback_app_state.started = false;
    mob_screen_show_state(PLAYBACK_STATE_IDLE, NULL);
}
