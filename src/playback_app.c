/**
 * @file playback_app.c
 * @brief Concrete Playback application composition for T5AI.
 */

#include "playback_app.h"

#include <stdbool.h>
#include <string.h>

#include "board_com_api.h"
#include "demo_network_config.h"
#include "http_session.h"
#include "lv_vendor.h"
#include "lvgl.h"
#include "mob_screen.h"
#include "playback_bluetooth_browser.h"
#include "playback_board_link_gatt.h"
#include "playback_engine.h"
#include "playback_h264_decoder.h"
#include "playback_http_range.h"
#include "playback_mp4_demux.h"
#include "playback_network.h"
#include "playback_video_output.h"
#include "playback_video_test.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "tkl_speaker.h"
#include <modules/mp3dec.h>

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
    THREAD_HANDLE speaker_test_thread;
    THREAD_HANDLE video_test_thread;
    bool speaker_test_running;
    bool video_test_running;
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

static void playback_app_bluetooth_auth_failure_callback(
    void *context,
    const uint8_t address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES]
)
{
    playback_app_state_t *state = context;
    playback_speaker_link_status_t speaker_status;

    if ((state == NULL) || (address == NULL) ||
        (state->speaker_probe.state == NULL) ||
        (playback_speaker_link_get_status(
             &state->speaker_probe,
             &speaker_status
         ) != PLAYBACK_SPEAKER_LINK_OK) ||
        (memcmp(
             speaker_status.target_address,
             address,
             sizeof(speaker_status.target_address)
         ) != 0))
    {
        return;
    }

    PR_WARN("Bluetooth authentication failed; pausing A2DP retries until user taps again");
    (void)playback_speaker_link_disconnect(&state->speaker_probe);
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
    playback_speaker_link_status_t speaker_status;

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

    if ((state->speaker_probe.state != NULL) &&
        (playback_speaker_link_get_status(
             &state->speaker_probe,
             &speaker_status
         ) == PLAYBACK_SPEAKER_LINK_OK) &&
        (memcmp(
             speaker_status.target_address,
             address,
             sizeof(speaker_status.target_address)
         ) == 0))
    {
        PR_NOTICE("Reusing existing A2DP speaker link");
        if ((speaker_status.state == PLAYBACK_SPEAKER_DISCONNECTED) ||
            (speaker_status.state == PLAYBACK_SPEAKER_CONNECTING))
        {
            (void)playback_speaker_link_reconnect(&state->speaker_probe);
        }
        (void)playback_speaker_link_start(&state->speaker_probe);
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

#define PLAYBACK_MP3_CHUNK_BYTES     (8U * 1024U)
#define PLAYBACK_MP3_ACCUM_EXTRA     (16384U)

static void *playback_mp3_psram_alloc(size_t size)
{
    return tal_psram_malloc((uint32_t)size);
}

static void playback_mp3_psram_free(void *buff)
{
    if (buff != NULL) {
        tal_psram_free(buff);
    }
}

static void *playback_mp3_psram_memset(void *s, unsigned char c, size_t n)
{
    return memset(s, (int)c, n);
}

static void playback_app_speaker_test_run(void)
{
    static const char *const audio_url =
        "http://advx26.babelbeast.com/debug/audio.mp3";
    http_session_t session = NULL;
    http_req_t request;
    http_resp_t *response = NULL;
    http_custom_header_t request_headers[1U];
    OPERATE_RET operation_result;
    uint8_t *accum_buf = NULL;
    uint8_t *http_buf = NULL;
    short *pcm_buf = NULL;
    HMP3Decoder decoder = NULL;
    MP3FrameInfo frame_info;
    unsigned char *read_ptr;
    int accum_len;
    int read_size;
    uint32_t remaining;
    uint32_t frame_count = 0U;
    bool speaker_started = false;
    bool stream_complete = false;

    PR_NOTICE("MP3: URL = %s", audio_url);
    PR_NOTICE("MP3: opening persistent HTTP session...");
    operation_result = http_open_session(&session, audio_url, 30000U);
    if (operation_result != OPRT_OK)
    {
        PR_ERR("MP3: HTTP session open failed: %d", operation_result);
        return;
    }

    memset(&request, 0, sizeof(request));
    request_headers[0U].key = "Accept-Encoding";
    request_headers[0U].value = "identity";
    request.type = HTTP_GET;
    request.version = HTTP_VER_1_1;
    request.custom_headers = request_headers;
    request.custom_headers_count = 1;
    operation_result = http_send_request(
        session,
        &request,
        HTTP_REQUEST_KEEP_ALIVE_FLAG
    );
    if (operation_result != OPRT_OK)
    {
        PR_ERR("MP3: HTTP GET failed: %d", operation_result);
        goto cleanup;
    }

    operation_result = http_get_response_hdr(session, &response);
    if ((operation_result != OPRT_OK) ||
        (response == NULL) ||
        (response->status_code != 200) ||
        (response->content_length == 0U))
    {
        PR_ERR(
            "MP3: invalid HTTP response: result=%d status=%d length=%u",
            operation_result,
            (response != NULL) ? response->status_code : 0,
            (response != NULL) ? response->content_length : 0U
        );
        goto cleanup;
    }

    remaining = response->content_length;
    PR_NOTICE(
        "MP3: persistent stream length=%u read_chunk=%u",
        response->content_length,
        (unsigned int)PLAYBACK_MP3_CHUNK_BYTES
    );

    /* Allocate streaming buffers after the HTTP metadata probe. */
    accum_buf = tal_psram_malloc(PLAYBACK_MP3_CHUNK_BYTES + PLAYBACK_MP3_ACCUM_EXTRA);
    http_buf  = tal_psram_malloc(PLAYBACK_MP3_CHUNK_BYTES);
    pcm_buf   = tal_psram_malloc(
        (uint32_t)(MAX_NSAMP * MAX_NCHAN * MAX_NGRAN) * sizeof(short));
    if ((accum_buf == NULL) || (http_buf == NULL) || (pcm_buf == NULL)) {
        PR_ERR("MP3: buffer alloc failed");
        goto cleanup;
    }

    MP3SetBuffMethod(playback_mp3_psram_alloc,
                     playback_mp3_psram_free,
                     playback_mp3_psram_memset);
    decoder = MP3InitDecoder();
    if (decoder == NULL) {
        PR_ERR("MP3: decoder init failed");
        goto cleanup;
    }

    /* Stream: download, decode, downmix to mono, then play. */
    accum_len = 0;

    while (remaining > 0U) {
        const uint32_t requested =
            (remaining < PLAYBACK_MP3_CHUNK_BYTES)
                ? remaining
                : PLAYBACK_MP3_CHUNK_BYTES;
        uint32_t chunk;
        int decode_ret;

        read_size = http_read_content(session, http_buf, requested);
        if (read_size < 0)
        {
            PR_ERR(
                "MP3: persistent HTTP read failed after %u bytes",
                response->content_length - remaining
            );
            break;
        }
        if (read_size == 0)
        {
            PR_WARN(
                "MP3: premature HTTP EOF after %u/%u bytes",
                response->content_length - remaining,
                response->content_length
            );
            break;
        }
        chunk = (uint32_t)read_size;

        /* append to accumulator (only if there's room — shift if needed) */
        if ((uint32_t)accum_len + chunk >
            PLAYBACK_MP3_CHUNK_BYTES + PLAYBACK_MP3_ACCUM_EXTRA) {
            PR_ERR("MP3: accum overflow");
            break;
        }
        memcpy(accum_buf + accum_len, http_buf, chunk);
        accum_len += (int)chunk;
        remaining -= chunk;

        /* decode all complete frames in accumulator */
        read_ptr = accum_buf;
        while (accum_len > 0) {
            int sync_off = MP3FindSyncWord(read_ptr, accum_len);
            unsigned char *frame_start;
            int frame_length;

            if (sync_off < 0)
            {
                if (accum_len > 1)
                {
                    read_ptr += accum_len - 1;
                    accum_len = 1;
                }
                break;
            }
            read_ptr += sync_off;
            accum_len -= sync_off;
            frame_start = read_ptr;
            frame_length = accum_len;

            decode_ret = MP3Decode(decoder, &read_ptr, &accum_len, pcm_buf, 0);
            if (decode_ret == ERR_MP3_INDATA_UNDERFLOW)
            {
                read_ptr = frame_start;
                accum_len = frame_length;
                break;
            }
            if (decode_ret != ERR_MP3_NONE)
            {
                if ((read_ptr <= frame_start) || (accum_len >= frame_length))
                {
                    read_ptr = frame_start + 1;
                    accum_len = frame_length - 1;
                }
                continue;
            }

            MP3GetLastFrameInfo(decoder, &frame_info);
            if (frame_info.outputSamps <= 0) {
                continue;
            }

            frame_count++;
            {
                uint32_t sample_count = (uint32_t)frame_info.outputSamps;
                uint32_t pcm_bytes;
                short *pw = pcm_buf;

                if (frame_info.nChans == 2)
                {
                    uint32_t sample_index;
                    sample_count /= 2U;
                    for (sample_index = 0U; sample_index < sample_count; ++sample_index)
                    {
                        const int32_t mixed =
                            (int32_t)pcm_buf[sample_index * 2U] +
                            (int32_t)pcm_buf[(sample_index * 2U) + 1U];
                        pcm_buf[sample_index] = (short)(mixed / 2);
                    }
                }
                else if (frame_info.nChans != 1)
                {
                    PR_ERR("MP3: unsupported channel count %d", frame_info.nChans);
                    goto cleanup;
                }

                if (!speaker_started)
                {
                    TKL_SPK_CFG_T speaker_config = {0};
                    speaker_config.chl_num = 1;
                    speaker_config.sample_rate = (uint32_t)frame_info.samprate;
                    speaker_config.datebits = TKL_SPK_DATABITS_16;
                    speaker_config.volume = 60;
                    speaker_config.card = TKL_SPK_TYPE_BOARD;
                    speaker_config.codectype = TKL_CODEC_SPK_PCM;
                    speaker_config.spk_gpio = 28;
                    speaker_config.spk_gpio_polarity = 0;
                    if ((tkl_speaker_init(&speaker_config) != OPRT_OK) ||
                        (tkl_speaker_start() != OPRT_OK))
                    {
                        PR_ERR(
                            "MP3: speaker start failed at %d Hz",
                            frame_info.samprate
                        );
                        goto cleanup;
                    }
                    speaker_started = true;
                    PR_NOTICE(
                        "MP3: speaker started at %d Hz mono",
                        frame_info.samprate
                    );
                }

                pcm_bytes = sample_count * sizeof(short);
                while (pcm_bytes >= 640U)
                {
                    (void)tkl_speaker_write((uint8_t *)pw, 640U);
                    pw += 320;
                    pcm_bytes -= 640U;
                }
                if (pcm_bytes > 0U)
                {
                    (void)tkl_speaker_write((uint8_t *)pw, pcm_bytes);
                }
            }
        }

        /* shift leftover to front of accum_buf */
        if (accum_len > 0 && read_ptr != accum_buf) {
            memmove(accum_buf, read_ptr, (size_t)accum_len);
        }
    }

    stream_complete = (remaining == 0U);
    if (stream_complete)
    {
        PR_NOTICE("MP3: stream complete (%lu frames decoded)", frame_count);
    }
    else
    {
        PR_WARN(
            "MP3: stream stopped with %u bytes remaining (%lu frames decoded)",
            remaining,
            frame_count
        );
    }

cleanup:
    if (response != NULL)
    {
        (void)http_free_response_hdr(&response);
    }
    if (session != NULL)
    {
        (void)http_close_session(&session);
    }
    if (speaker_started)
    {
        (void)tkl_speaker_stop();
        (void)tkl_speaker_deinit();
    }
    if (pcm_buf   != NULL) { tal_psram_free(pcm_buf); }
    if (decoder   != NULL) { MP3FreeDecoder(decoder); }
    if (http_buf  != NULL) { tal_psram_free(http_buf); }
    if (accum_buf != NULL) { tal_psram_free(accum_buf); }
}

static void playback_app_speaker_test_worker(void *context)
{
    playback_app_state_t *state = context;

    playback_app_speaker_test_run();
    if (state != NULL)
    {
        state->speaker_test_running = false;
        state->speaker_test_thread = NULL;
        PR_NOTICE("MP3: test worker finished; TEST is available");
    }
}

static void playback_app_speaker_test(void *context)
{
    playback_app_state_t *state = context;
    THREAD_CFG_T thread_config;

    if ((state == NULL) || state->speaker_test_running || state->video_test_running)
    {
        PR_WARN("MP3: another media test is already running");
        return;
    }

    memset(&thread_config, 0, sizeof(thread_config));
    thread_config.stackDepth = 12U * 1024U;
    thread_config.priority = THREAD_PRIO_2;
    thread_config.thrdname = "speaker_test";
    thread_config.psram_mode = 1U;
    state->speaker_test_running = true;
    if (tal_thread_create_and_start(
            &state->speaker_test_thread,
            NULL,
            NULL,
            playback_app_speaker_test_worker,
            state,
            &thread_config
        ) != OPRT_OK)
    {
        state->speaker_test_running = false;
        state->speaker_test_thread = NULL;
        PR_ERR("MP3: unable to start test worker");
    }
}

static void playback_app_video_test_worker(void *context)
{
    playback_app_state_t *state = context;
    playback_video_test_result_t result = playback_video_test_run(
        "http://advx26.babelbeast.com/debug/video.mp4",
        mob_screen_present_rgb565,
        NULL
    );

    PR_NOTICE("VIDEO: test finished: %s", playback_video_test_result_name(result));
    if (state != NULL)
    {
        state->video_test_running = false;
        state->video_test_thread = NULL;
        PR_NOTICE("VIDEO: test worker finished; VIDEO is available");
    }
}

static void playback_app_video_test(void *context)
{
    playback_app_state_t *state = context;
    THREAD_CFG_T thread_config;

    if ((state == NULL) || state->video_test_running || state->speaker_test_running)
    {
        PR_WARN("VIDEO: another media test is already running");
        return;
    }

    memset(&thread_config, 0, sizeof(thread_config));
    thread_config.stackDepth = 24U * 1024U;
    thread_config.priority = THREAD_PRIO_2;
    thread_config.thrdname = "video_test";
    thread_config.psram_mode = 1U;
    state->video_test_running = true;
    if (tal_thread_create_and_start(
            &state->video_test_thread,
            NULL,
            NULL,
            playback_app_video_test_worker,
            state,
            &thread_config
        ) != OPRT_OK)
    {
        state->video_test_running = false;
        state->video_test_thread = NULL;
        PR_ERR("VIDEO: unable to start test worker");
    }
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
    mob_screen_set_speaker_test_callback(
        playback_app_speaker_test,
        &playback_app_state
    );
    mob_screen_set_video_test_callback(
        playback_app_video_test,
        &playback_app_state
    );
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
            "Beken dual-mode Bluetooth host preparation failed: %s",
            playback_speaker_link_result_name(speaker_result)
        );
        mob_screen_show_bluetooth_status(
            PLAYBACK_BLUETOOTH_BROWSER_FAILED,
            NULL,
            "A2DP initialization failed"
        );
        return OPRT_COM_ERROR;
    }
    PR_NOTICE("Beken dual-mode Bluetooth host prepared for A2DP and Board Link");

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
    bluetooth_config.on_auth_failure = playback_app_bluetooth_auth_failure_callback;
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
