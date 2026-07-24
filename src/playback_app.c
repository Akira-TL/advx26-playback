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
#include "playback_bluetooth_browser.h"
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

#ifndef DEMO_PLAYBACK_AUTHORIZATION
#define DEMO_PLAYBACK_AUTHORIZATION NULL
#endif

typedef struct
{
    playback_board_link_gatt_t gatt;
    playback_engine_t engine;
    playback_bluetooth_browser_t bluetooth_browser;
    playback_speaker_link_t speaker_probe;
    bool started;
} playback_app_state_t;

static playback_app_state_t playback_app_state;
static uint8_t playback_app_speaker_address[PLAYBACK_SPEAKER_LINK_ADDRESS_BYTES] =
    DEMO_SPEAKER_ADDRESS;
static const char *const playback_app_authorization = DEMO_PLAYBACK_AUTHORIZATION;

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

static void playback_app_bluetooth_devices_callback(
    void *context,
    const playback_bluetooth_browser_device_t *devices,
    size_t device_count,
    bool scanning
)
{
    (void)context;
    mob_screen_show_bluetooth_devices(devices, device_count, scanning);
}

static size_t playback_app_speaker_probe_read_pcm(
    void *context,
    int16_t *destination,
    size_t frame_capacity
)
{
    (void)context;

    if ((destination == NULL) || (frame_capacity == 0U))
    {
        return 0U;
    }
    memset(
        destination,
        0,
        frame_capacity * 2U * sizeof(*destination)
    );
    return frame_capacity;
}

static void playback_app_speaker_probe_status_callback(
    void *context,
    const playback_speaker_link_status_t *status
)
{
    (void)context;

    if (status == NULL)
    {
        return;
    }
    switch (status->state)
    {
        case PLAYBACK_SPEAKER_CONNECTING:
            mob_screen_show_bluetooth_status(
                PLAYBACK_BLUETOOTH_BROWSER_CONNECTING,
                status->target_address,
                "Opening A2DP audio profile"
            );
            break;
        case PLAYBACK_SPEAKER_CONNECTED:
            mob_screen_show_bluetooth_status(
                PLAYBACK_BLUETOOTH_BROWSER_CONNECTED,
                status->target_address,
                "A2DP connected; starting silent keepalive"
            );
            break;
        case PLAYBACK_SPEAKER_STREAMING:
            mob_screen_show_bluetooth_status(
                PLAYBACK_BLUETOOTH_BROWSER_PAIRED,
                status->target_address,
                "A2DP connected; silent keepalive active"
            );
            break;
        case PLAYBACK_SPEAKER_DISCONNECTED:
            if (status->desired_connected)
            {
                mob_screen_show_bluetooth_status(
                    PLAYBACK_BLUETOOTH_BROWSER_CONNECTING,
                    status->target_address,
                    "A2DP disconnected; reconnecting"
                );
            }
            break;
        default:
            break;
    }
}

static void playback_app_close_speaker_probe(playback_app_state_t *state)
{
    if ((state != NULL) && (state->speaker_probe.state != NULL))
    {
        playback_speaker_link_close(&state->speaker_probe);
    }
}

static void playback_app_bluetooth_status_callback(
    void *context,
    playback_bluetooth_browser_status_t status,
    const uint8_t address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES],
    const char *detail
)
{
    (void)context;
    mob_screen_show_bluetooth_status(status, address, detail);
    PR_NOTICE(
        "Bluetooth browser status=%s detail=%s",
        playback_bluetooth_browser_status_name(status),
        detail != NULL ? detail : ""
    );
}

static void playback_app_bluetooth_scan_callback(void *context)
{
    playback_app_state_t *state = context;

    if ((state == NULL) ||
        !playback_bluetooth_browser_start_scan(&state->bluetooth_browser))
    {
        PR_WARN("Bluetooth scanner is not ready");
    }
}

static void playback_app_bluetooth_connect_callback(
    void *context,
    const uint8_t address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES]
)
{
    playback_app_state_t *state = context;
    playback_engine_result_t engine_result;
    playback_speaker_link_config_t speaker_config;
    playback_speaker_link_result_t speaker_result;

    if ((state == NULL) || (address == NULL))
    {
        return;
    }
    memcpy(
        playback_app_speaker_address,
        address,
        sizeof(playback_app_speaker_address)
    );
    engine_result = playback_engine_set_speaker_address(&state->engine, address);
    if (engine_result != PLAYBACK_ENGINE_OK)
    {
        PR_WARN(
            "Unable to select Bluetooth speaker: %s",
            playback_engine_result_name(engine_result)
        );
        return;
    }
    if (!playback_bluetooth_browser_connect(&state->bluetooth_browser, address))
    {
        PR_WARN("Unable to select Bluetooth speaker");
        return;
    }

    playback_app_close_speaker_probe(state);
    memset(&speaker_config, 0, sizeof(speaker_config));
    memcpy(
        speaker_config.target_address,
        address,
        sizeof(speaker_config.target_address)
    );
    speaker_config.sample_rate = PLAYBACK_SPEAKER_LINK_SAMPLE_RATE;
    speaker_config.channels = 2U;
    speaker_config.read_pcm = playback_app_speaker_probe_read_pcm;
    speaker_config.on_status = playback_app_speaker_probe_status_callback;
    speaker_config.context = state;
    speaker_result = playback_speaker_link_init(
        &state->speaker_probe,
        &speaker_config
    );
    if (speaker_result == PLAYBACK_SPEAKER_LINK_OK)
    {
        speaker_result = playback_speaker_link_start(&state->speaker_probe);
    }
    if (speaker_result != PLAYBACK_SPEAKER_LINK_OK)
    {
        PR_WARN(
            "Bluetooth A2DP probe failed: %s",
            playback_speaker_link_result_name(speaker_result)
        );
        playback_app_close_speaker_probe(state);
    }
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

    if (command->kind == PLAYBACK_COMMAND_LOAD_SESSION)
    {
        playback_app_close_speaker_probe(state);
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
    playback_bluetooth_browser_config_t bluetooth_config;
    mob_screen_bluetooth_callbacks_t screen_bluetooth_callbacks;
    playback_board_link_gatt_result_t gatt_result;
    playback_engine_result_t engine_result;
    playback_speaker_link_result_t speaker_result;
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
    memset(&screen_bluetooth_callbacks, 0, sizeof(screen_bluetooth_callbacks));
    screen_bluetooth_callbacks.on_scan = playback_app_bluetooth_scan_callback;
    screen_bluetooth_callbacks.on_connect = playback_app_bluetooth_connect_callback;
    screen_bluetooth_callbacks.context = &playback_app_state;
    mob_screen_set_bluetooth_callbacks(&screen_bluetooth_callbacks);
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

    speaker_result = playback_speaker_link_prepare();
    if (speaker_result != PLAYBACK_SPEAKER_LINK_OK)
    {
        PR_ERR(
            "A2DP source preparation failed before BLE start: %s",
            playback_speaker_link_result_name(speaker_result)
        );
        mob_screen_show_bluetooth_status(
            PLAYBACK_BLUETOOTH_BROWSER_FAILED,
            NULL,
            "A2DP initialization failed"
        );
        return OPRT_COM_ERROR;
    }
    PR_NOTICE("A2DP source prepared before BLE Board Link startup");

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
    engine_config.scheduler.http.authorization = playback_app_authorization;
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

    memset(&bluetooth_config, 0, sizeof(bluetooth_config));
    bluetooth_config.on_devices = playback_app_bluetooth_devices_callback;
    bluetooth_config.on_status = playback_app_bluetooth_status_callback;
    bluetooth_config.context = &playback_app_state;
    if (!playback_bluetooth_browser_init(
            &playback_app_state.bluetooth_browser,
            &bluetooth_config
        ))
    {
        PR_WARN("Classic Bluetooth browser initialization failed");
        mob_screen_show_bluetooth_status(
            PLAYBACK_BLUETOOTH_BROWSER_FAILED,
            NULL,
            "Classic Bluetooth initialization failed"
        );
    }

    playback_app_state.started = true;
    mob_screen_show_state(PLAYBACK_STATE_IDLE, NULL);
    if (!playback_app_speaker_configured())
    {
        PR_WARN("Fixed speaker address is not configured; define DEMO_SPEAKER_ADDRESS in demo_network_config.h");
    }
    else
    {
        PR_NOTICE(
            "Playback speaker target: %02X:%02X:%02X:%02X:%02X:%02X",
            playback_app_speaker_address[5],
            playback_app_speaker_address[4],
            playback_app_speaker_address[3],
            playback_app_speaker_address[2],
            playback_app_speaker_address[1],
            playback_app_speaker_address[0]
        );
    }
    PR_NOTICE(
        "Playback media authorization: %s",
        ((playback_app_authorization != NULL) && (playback_app_authorization[0] != '\0'))
            ? "configured"
            : "not configured"
    );
    PR_NOTICE("Playback application ready; media starts only from Board Link LOAD_SESSION");
    return OPRT_OK;
}

void playback_app_stop(void)
{
    if (!playback_app_state.started)
    {
        return;
    }

    playback_app_close_speaker_probe(&playback_app_state);
    playback_bluetooth_browser_close(&playback_app_state.bluetooth_browser);
    playback_engine_close(&playback_app_state.engine);
    playback_board_link_gatt_close(&playback_app_state.gatt);
    playback_app_state.started = false;
    mob_screen_show_state(PLAYBACK_STATE_IDLE, NULL);
}
