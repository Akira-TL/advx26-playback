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
#include "playback_av_test.h"
#include "playback_board_link_gatt.h"
#include "playback_board_link_tcp.h"
#include "playback_board_link_uart.h"
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

#ifndef DEMO_HTTP_TLS_NO_VERIFY
#define DEMO_HTTP_TLS_NO_VERIFY (0)
#endif

#ifndef DEMO_PLAYBACK_AUTHORIZATION
#define DEMO_PLAYBACK_AUTHORIZATION NULL
#endif

typedef struct
{
    playback_board_link_gatt_t gatt;
    playback_board_link_tcp_t http;
    playback_board_link_uart_t uart;
    playback_engine_t engine;
    THREAD_HANDLE speaker_test_thread;
    THREAD_HANDLE video_test_thread;
    THREAD_HANDLE av_test_thread;
    bool speaker_test_running;
    bool video_test_running;
    bool av_test_running;
    bool started;
} playback_app_state_t;

static playback_app_state_t playback_app_state;
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
    playback_board_link_gatt_result_t gatt_result;
    playback_board_link_tcp_result_t http_result;
    playback_board_link_uart_result_t uart_result;

    if ((state == NULL) || (report == NULL))
    {
        return;
    }

    playback_app_show_report(report);
    gatt_result = playback_board_link_gatt_send_report(&state->gatt, report);
    if ((gatt_result != PLAYBACK_BOARD_LINK_GATT_OK) &&
        (gatt_result != PLAYBACK_BOARD_LINK_GATT_NOT_CONNECTED) &&
        (gatt_result != PLAYBACK_BOARD_LINK_GATT_NOT_SUBSCRIBED))
    {
        PR_WARN(
            "Board Link GATT report failed: %s",
            playback_board_link_gatt_result_name(gatt_result)
        );
    }

    uart_result = playback_board_link_uart_send_report(&state->uart, report);
    if ((uart_result != PLAYBACK_BOARD_LINK_UART_OK) &&
        (uart_result != PLAYBACK_BOARD_LINK_UART_NOT_STARTED) &&
        (uart_result != PLAYBACK_BOARD_LINK_UART_NOT_HANDSHAKEN))
    {
        PR_WARN(
            "Board Link UART report failed: %s",
            playback_board_link_uart_result_name(uart_result)
        );
    }

    if (report->kind != PLAYBACK_REPORT_PROGRESS)
    {
        http_result = playback_board_link_tcp_send_report(&state->http, report);
        if ((http_result != PLAYBACK_BOARD_LINK_TCP_OK) &&
            (http_result != PLAYBACK_BOARD_LINK_TCP_NOT_STARTED) &&
            (http_result != PLAYBACK_BOARD_LINK_TCP_PEER_NOT_CONFIGURED))
        {
            PR_WARN(
                "Board Link TCP report failed: %s",
                playback_board_link_tcp_result_name(http_result)
            );
        }
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
    (void)playback_board_link_uart_send_report(&state->uart, &report);
    (void)playback_board_link_tcp_send_report(&state->http, &report);
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

static void playback_app_uart_status_callback(
    void *context,
    const playback_board_link_uart_status_t *status
)
{
    (void)context;

    if (status != NULL)
    {
        PR_DEBUG(
            "Board Link UART started=%u handshake=%u received=%u rejected=%u",
            status->started ? 1U : 0U,
            status->handshake_complete ? 1U : 0U,
            (unsigned int)status->received_message_count,
            (unsigned int)status->rejected_message_count
        );
    }
}

static bool playback_app_http_snapshot_callback(
    void *context,
    playback_report_t *report
)
{
    playback_app_state_t *state = context;
    playback_snapshot_t snapshot;

    if ((state == NULL) || (report == NULL) ||
        (playback_engine_get_snapshot(&state->engine, &snapshot) != PLAYBACK_ENGINE_OK))
    {
        return false;
    }

    memset(report, 0, sizeof(*report));
    report->kind = PLAYBACK_REPORT_STATE;
    report->state = snapshot.state;
    report->intent = snapshot.intent;
    report->position_ms = snapshot.position_ms;
    report->duration_ms = snapshot.has_session ? snapshot.session.duration_ms : 0U;
    if (snapshot.has_session)
    {
        strncpy(
            report->session_id,
            snapshot.session.session_id,
            sizeof(report->session_id) - 1U
        );
    }
    return true;
}

static void playback_app_http_status_callback(
    void *context,
    const playback_board_link_tcp_status_t *status
)
{
    (void)context;

    if (status != NULL)
    {
        PR_DEBUG(
            "Board Link TCP started=%u peer=%s received=%u rejected=%u sent=%u failed=%u",
            status->started ? 1U : 0U,
            status->peer_configured ? status->peer_ip : "not-configured",
            (unsigned int)status->received_command_count,
            (unsigned int)status->rejected_request_count,
            (unsigned int)status->sent_report_count,
            (unsigned int)status->failed_report_count
        );
    }
}

static bool playback_app_peer_ip_submit(void *context, const char *peer_ip)
{
    playback_app_state_t *state = context;

    return (state != NULL) &&
           (playback_board_link_tcp_set_peer_ip(&state->http, peer_ip) ==
            PLAYBACK_BOARD_LINK_TCP_OK);
}

static void playback_app_network_status_callback(
    void *context,
    bool connected,
    const char *local_ip
)
{
    playback_app_state_t *state = context;
    playback_board_link_tcp_status_t http_status;
    const char *peer_ip = "";

    if ((state != NULL) &&
        (playback_board_link_tcp_get_status(&state->http, &http_status) ==
         PLAYBACK_BOARD_LINK_TCP_OK) &&
        http_status.peer_configured)
    {
        peer_ip = http_status.peer_ip;
    }
    mob_screen_update_network(connected ? local_ip : "", peer_ip);
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

    if ((state == NULL) || state->speaker_test_running || state->video_test_running ||
        state->av_test_running)
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

    if ((state == NULL) || state->video_test_running || state->speaker_test_running ||
        state->av_test_running)
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

static void playback_app_set_debug_asset(
    playback_asset_t *asset,
    const char *url,
    uint32_t byte_length,
    const char *sha256,
    const char *etag
)
{
    if (asset == NULL)
    {
        return;
    }
    memset(asset, 0, sizeof(*asset));
    strncpy(asset->url, url, sizeof(asset->url) - 1U);
    asset->byte_length = byte_length;
    strncpy(asset->sha256, sha256, sizeof(asset->sha256) - 1U);
    strncpy(asset->etag, etag, sizeof(asset->etag) - 1U);
}

static void playback_app_build_debug_av_session(playback_session_t *session)
{
    if (session == NULL)
    {
        return;
    }
    memset(session, 0, sizeof(*session));
    strncpy(session->session_id, "debug-av-30s", sizeof(session->session_id) - 1U);
    strncpy(session->content_id, "debug-av", sizeof(session->content_id) - 1U);
    session->revision = 1U;
    session->duration_ms = 30000U;
    strncpy(session->profile, PLAYBACK_PROFILE_H264_MP3, sizeof(session->profile) - 1U);

    playback_app_set_debug_asset(
        &session->video.asset,
        "http://advx26.babelbeast.com/debug/video.mp4",
        1730964U,
        "a2c111e2225cfc6d3154a9d4378498fbfd95cc0c53643f4e99e6f318afbafff4",
        "\"a2c111e2225cfc6d3154a9d4378498fbfd95cc0c53643f4e99e6f318afbafff4\""
    );
    session->video.width = PLAYBACK_VIDEO_WIDTH;
    session->video.height = PLAYBACK_VIDEO_HEIGHT;
    session->video.fps_num = 10U;
    session->video.fps_den = 1U;
    session->video.max_keyframe_interval_ms = 1000U;
    session->video.h264_profile_idc = PLAYBACK_H264_BASELINE_PROFILE_IDC;
    session->video.h264_level_idc = 30U;
    session->video.yuv420p = true;
    session->video.has_b_frames = false;

    playback_app_set_debug_asset(
        &session->audio.asset,
        "http://advx26.babelbeast.com/debug/audio.mp3",
        480698U,
        "704d64dd8815724f72f56d13a6caed9c258e33cb3a0fe2a44746768116007beb",
        "\"704d64dd8815724f72f56d13a6caed9c258e33cb3a0fe2a44746768116007beb\""
    );
    playback_app_set_debug_asset(
        &session->audio.index_asset,
        "http://advx26.babelbeast.com/debug/audio.idx",
        18416U,
        "1e4f612fe32ee795dbac6874f87261c61aa4e25046e87022f16966a6c945aa18",
        "\"1e4f612fe32ee795dbac6874f87261c61aa4e25046e87022f16966a6c945aa18\""
    );
    session->audio.sample_rate = PLAYBACK_MP3_SAMPLE_RATE;
    session->audio.bitrate_kbps = PLAYBACK_MP3_BITRATE_KBPS;
    session->audio.channels = 2U;
    session->audio.index_version = PLAYBACK_AUDIO_INDEX_VERSION;
    session->autoplay = true;
    session->end_behavior = PLAYBACK_END_HOLD_LAST_FRAME;
}

static void playback_app_av_test_worker(void *context)
{
    playback_app_state_t *state = context;
    playback_session_t session;
    playback_media_scheduler_config_t config;
    playback_av_test_result_t result;

    memset(&config, 0, sizeof(config));
    playback_app_build_debug_av_session(&session);
    config.http.timeout_ms = PLAYBACK_HTTP_DEFAULT_TIMEOUT_MS;
    config.http.authorization = playback_app_authorization;
    config.http.tls_no_verify = DEMO_HTTP_TLS_NO_VERIFY != 0;
    config.wired_speaker.volume = PLAYBACK_WIRED_SPEAKER_DEFAULT_VOLUME;
    config.wired_speaker.amplifier_gpio = PLAYBACK_WIRED_SPEAKER_DEFAULT_GPIO;
    config.wired_speaker.amplifier_gpio_polarity =
        PLAYBACK_WIRED_SPEAKER_DEFAULT_GPIO_POLARITY;
    config.wired_speaker.output_latency_ms =
        PLAYBACK_WIRED_SPEAKER_DEFAULT_LATENCY_MS;
    config.video_output.rotation = PLAYBACK_VIDEO_ROTATION_0;
    config.video_output.swap_rgb565_bytes = false;
    config.present_video = mob_screen_present_rgb565;
    config.video_context = NULL;

    result = playback_av_test_run(&session, &config);
    PR_NOTICE("AV: test finished: %s", playback_av_test_result_name(result));
    if (state != NULL)
    {
        state->av_test_running = false;
        state->av_test_thread = NULL;
        PR_NOTICE("AV: test worker finished; AV is available");
    }
}

static void playback_app_av_test(void *context)
{
    playback_app_state_t *state = context;
    playback_snapshot_t snapshot;
    THREAD_CFG_T thread_config;

    if ((state == NULL) || state->av_test_running || state->speaker_test_running ||
        state->video_test_running)
    {
        PR_WARN("AV: another media test is already running");
        return;
    }
    if ((playback_engine_get_snapshot(&state->engine, &snapshot) != PLAYBACK_ENGINE_OK) ||
        snapshot.has_session)
    {
        PR_WARN("AV: stop the active Board Link session first");
        return;
    }

    memset(&thread_config, 0, sizeof(thread_config));
    thread_config.stackDepth = 24U * 1024U;
    thread_config.priority = THREAD_PRIO_2;
    thread_config.thrdname = "av_test";
    thread_config.psram_mode = 1U;
    state->av_test_running = true;
    if (tal_thread_create_and_start(
            &state->av_test_thread,
            NULL,
            NULL,
            playback_app_av_test_worker,
            state,
            &thread_config
        ) != OPRT_OK)
    {
        state->av_test_running = false;
        state->av_test_thread = NULL;
        PR_ERR("AV: unable to start test worker");
    }
}

OPERATE_RET playback_app_start(void)
{
    playback_board_link_gatt_config_t gatt_config;
    playback_board_link_gatt_status_t gatt_status;
    playback_board_link_tcp_config_t http_config;
    playback_board_link_uart_config_t uart_config;
    playback_engine_config_t engine_config;
    playback_board_link_gatt_result_t gatt_result;
    playback_board_link_tcp_result_t http_result;
    playback_board_link_uart_result_t uart_result;
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
    mob_screen_set_speaker_test_callback(
        playback_app_speaker_test,
        &playback_app_state
    );
    mob_screen_set_video_test_callback(
        playback_app_video_test,
        &playback_app_state
    );
    mob_screen_set_av_test_callback(
        playback_app_av_test,
        &playback_app_state
    );
    mob_screen_set_peer_ip_submit_callback(
        playback_app_peer_ip_submit,
        &playback_app_state
    );
    playback_network_set_status_callback(
        playback_app_network_status_callback,
        &playback_app_state
    );

    lv_vendor_init(DISPLAY_NAME);
    lv_vendor_disp_lock();
    mob_screen_create();
    lv_vendor_disp_unlock();
    lv_vendor_start(5U, 1024U * 8U);
    mob_screen_neutralize_panel();
    mob_screen_update_network("", "");

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

    memset(&uart_config, 0, sizeof(uart_config));
    uart_config.boot_id = gatt_status.boot_id;
    uart_config.on_command = playback_app_command_callback;
    uart_config.on_status = playback_app_uart_status_callback;
    uart_config.context = &playback_app_state;
    uart_result = playback_board_link_uart_init(&playback_app_state.uart, &uart_config);
    if (uart_result != PLAYBACK_BOARD_LINK_UART_OK)
    {
        PR_ERR(
            "Board Link UART initialization failed: %s",
            playback_board_link_uart_result_name(uart_result)
        );
        playback_board_link_gatt_close(&playback_app_state.gatt);
        mob_screen_show_state(PLAYBACK_STATE_ERROR, "Board Link UART initialization failed");
        return OPRT_COM_ERROR;
    }

    memset(&engine_config, 0, sizeof(engine_config));
    strncpy(engine_config.boot_id, gatt_status.boot_id, sizeof(engine_config.boot_id) - 1U);
    engine_config.scheduler.http.timeout_ms = PLAYBACK_HTTP_DEFAULT_TIMEOUT_MS;
    engine_config.scheduler.http.authorization = playback_app_authorization;
    engine_config.scheduler.http.tls_no_verify = DEMO_HTTP_TLS_NO_VERIFY != 0;
    engine_config.scheduler.wired_speaker.volume =
        PLAYBACK_WIRED_SPEAKER_DEFAULT_VOLUME;
    engine_config.scheduler.wired_speaker.amplifier_gpio =
        PLAYBACK_WIRED_SPEAKER_DEFAULT_GPIO;
    engine_config.scheduler.wired_speaker.amplifier_gpio_polarity =
        PLAYBACK_WIRED_SPEAKER_DEFAULT_GPIO_POLARITY;
    engine_config.scheduler.wired_speaker.output_latency_ms =
        PLAYBACK_WIRED_SPEAKER_DEFAULT_LATENCY_MS;
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
        playback_board_link_uart_close(&playback_app_state.uart);
        playback_board_link_gatt_close(&playback_app_state.gatt);
        mob_screen_show_state(PLAYBACK_STATE_ERROR, "Playback engine initialization failed");
        return OPRT_COM_ERROR;
    }

    memset(&http_config, 0, sizeof(http_config));
    http_config.on_command = playback_app_command_callback;
    http_config.on_status = playback_app_http_status_callback;
    http_config.get_snapshot = playback_app_http_snapshot_callback;
    http_config.context = &playback_app_state;
    http_result = playback_board_link_tcp_init(&playback_app_state.http, &http_config);
    if (http_result != PLAYBACK_BOARD_LINK_TCP_OK)
    {
        PR_ERR(
            "Board Link TCP initialization failed: %s",
            playback_board_link_tcp_result_name(http_result)
        );
        playback_engine_close(&playback_app_state.engine);
        playback_board_link_uart_close(&playback_app_state.uart);
        playback_board_link_gatt_close(&playback_app_state.gatt);
        mob_screen_show_state(PLAYBACK_STATE_ERROR, "Board Link TCP initialization failed");
        return OPRT_COM_ERROR;
    }

    gatt_result = playback_board_link_gatt_start(&playback_app_state.gatt);
    if (gatt_result != PLAYBACK_BOARD_LINK_GATT_OK)
    {
        PR_ERR("Board Link GATT start failed: %s", playback_board_link_gatt_result_name(gatt_result));
        playback_board_link_tcp_close(&playback_app_state.http);
        playback_engine_close(&playback_app_state.engine);
        playback_board_link_uart_close(&playback_app_state.uart);
        playback_board_link_gatt_close(&playback_app_state.gatt);
        mob_screen_show_state(PLAYBACK_STATE_ERROR, "Board Link start failed");
        return OPRT_COM_ERROR;
    }

    uart_result = playback_board_link_uart_start(&playback_app_state.uart);
    if (uart_result != PLAYBACK_BOARD_LINK_UART_OK)
    {
        PR_ERR(
            "Board Link UART start failed: %s",
            playback_board_link_uart_result_name(uart_result)
        );
        playback_board_link_tcp_close(&playback_app_state.http);
        playback_board_link_gatt_close(&playback_app_state.gatt);
        playback_engine_close(&playback_app_state.engine);
        playback_board_link_uart_close(&playback_app_state.uart);
        mob_screen_show_state(PLAYBACK_STATE_ERROR, "Board Link UART start failed");
        return OPRT_COM_ERROR;
    }

    http_result = playback_board_link_tcp_start(&playback_app_state.http);
    if (http_result != PLAYBACK_BOARD_LINK_TCP_OK)
    {
        PR_ERR(
            "Board Link TCP start failed: %s",
            playback_board_link_tcp_result_name(http_result)
        );
        playback_board_link_tcp_close(&playback_app_state.http);
        playback_board_link_uart_close(&playback_app_state.uart);
        playback_board_link_gatt_close(&playback_app_state.gatt);
        playback_engine_close(&playback_app_state.engine);
        mob_screen_show_state(PLAYBACK_STATE_ERROR, "Board Link TCP start failed");
        return OPRT_COM_ERROR;
    }

    playback_app_state.started = true;
    mob_screen_update_network(playback_network_get_local_ip(), "");
    mob_screen_show_state(PLAYBACK_STATE_IDLE, NULL);
    PR_NOTICE(
        "Playback audio output: onboard wired speaker, mono, %u ms latency",
        PLAYBACK_WIRED_SPEAKER_DEFAULT_LATENCY_MS
    );
    PR_NOTICE(
        "Playback network control: listen=%s:%u status_interval=%u ms",
        playback_network_get_local_ip()[0] != '\0'
            ? playback_network_get_local_ip()
            : "0.0.0.0",
        (unsigned int)PLAYBACK_BOARD_LINK_TCP_PORT,
        500U
    );
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

    playback_board_link_tcp_close(&playback_app_state.http);
    playback_board_link_uart_close(&playback_app_state.uart);
    playback_board_link_gatt_close(&playback_app_state.gatt);
    playback_engine_close(&playback_app_state.engine);
    playback_app_state.started = false;
    mob_screen_show_state(PLAYBACK_STATE_IDLE, NULL);
}
