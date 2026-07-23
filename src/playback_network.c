/**
 * @file playback_network.c
 * @brief Fixed demo Wi-Fi connection and streamed HTTP video fetch.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tuya_cloud_types.h"
#include "tal_api.h"

#include "http_download.h"
#include "netmgr.h"
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "netconn_wifi.h"
#endif

#include "demo_network_config.h"
#include "playback_network.h"

#define PLAYBACK_DOWNLOAD_RANGE_LENGTH (8U * 1024U)
#define PLAYBACK_DOWNLOAD_TIMEOUT_MS    (15U * 1000U)
#define PLAYBACK_FNV1A_OFFSET_BASIS     (2166136261UL)
#define PLAYBACK_FNV1A_PRIME            (16777619UL)

static THREAD_HANDLE s_download_thread_handle = NULL;
static volatile bool s_download_running = false;
static volatile bool s_download_complete = false;
static volatile bool s_download_succeeded = false;
static size_t s_download_bytes = 0U;
static size_t s_expected_bytes = 0U;
static uint32_t s_download_hash = PLAYBACK_FNV1A_OFFSET_BASIS;

static void playback_download_event(http_download_event_id_t id, http_download_event_t *event)
{
    size_t index;
    const uint8_t *data;

    if (event == NULL) {
        return;
    }

    switch (id) {
    case DL_EVENT_START:
        s_download_bytes = 0U;
        s_expected_bytes = 0U;
        s_download_hash = PLAYBACK_FNV1A_OFFSET_BASIS;
        s_download_succeeded = false;
        PR_NOTICE("Video fetch started: %s", DEMO_VIDEO_URL);
        break;

    case DL_EVENT_ON_FILESIZE:
        s_expected_bytes = event->file_size;
        PR_NOTICE("Video size: %u bytes", (unsigned int)s_expected_bytes);
        break;

    case DL_EVENT_ON_DATA:
        data = (const uint8_t *)event->data;
        for (index = 0U; index < event->data_len; ++index) {
            s_download_hash ^= data[index];
            s_download_hash *= PLAYBACK_FNV1A_PRIME;
        }
        s_download_bytes += event->data_len;
        event->remain_len = 0U;
        break;

    case DL_EVENT_FINISH:
        s_download_succeeded = (s_expected_bytes > 0U) && (s_download_bytes == s_expected_bytes);
        PR_NOTICE("Video fetch finished: bytes=%u expected=%u fnv1a=%08x valid=%s",
                  (unsigned int)s_download_bytes,
                  (unsigned int)s_expected_bytes,
                  (unsigned int)s_download_hash,
                  s_download_succeeded ? "yes" : "no");
        break;

    case DL_EVENT_FAULT:
        PR_ERR("Video fetch failed after %u of %u bytes",
               (unsigned int)s_download_bytes,
               (unsigned int)s_expected_bytes);
        break;

    case DL_EVENT_CONNECTED:
    default:
        break;
    }
}

static void playback_download_thread(void *arg)
{
    int result;
    http_download_config_t config;

    (void)arg;
    memset(&config, 0, sizeof(config));
    config.url = DEMO_VIDEO_URL;
    config.timeout_ms = PLAYBACK_DOWNLOAD_TIMEOUT_MS;
    config.range_length = PLAYBACK_DOWNLOAD_RANGE_LENGTH;
    config.event_handler = playback_download_event;

    result = http_file_download(&config);
    if ((result != OPRT_OK) || !s_download_succeeded) {
        PR_ERR("Video fetch task ended with result=%d", result);
    }

    s_download_complete = s_download_succeeded;
    s_download_running = false;

    tal_thread_delete(s_download_thread_handle);
    s_download_thread_handle = NULL;
}

static OPERATE_RET playback_link_status_callback(void *data)
{
    OPERATE_RET result;
    netmgr_status_e status;
    THREAD_CFG_T thread_config;

    if (data == NULL) {
        return OPRT_INVALID_PARM;
    }

    status = *((netmgr_status_e *)data);
    if ((status != NETMGR_LINK_UP) && (status != NETMGR_LINK_UP_SWITH)) {
        PR_NOTICE("Wi-Fi link status=%d", status);
        return OPRT_OK;
    }

    PR_NOTICE("Wi-Fi connected; public video endpoint is ready to fetch");
    if (s_download_running || s_download_complete) {
        return OPRT_OK;
    }

    memset(&thread_config, 0, sizeof(thread_config));
    thread_config.stackDepth = 1024U * 8U;
    thread_config.priority = THREAD_PRIO_2;
    thread_config.thrdname = "video_fetch";

    s_download_running = true;
    result = tal_thread_create_and_start(&s_download_thread_handle,
                                         NULL,
                                         NULL,
                                         playback_download_thread,
                                         NULL,
                                         &thread_config);
    if (result != OPRT_OK) {
        s_download_running = false;
        PR_ERR("Failed to start video fetch thread: %d", result);
    }

    return result;
}

OPERATE_RET playback_network_start(void)
{
#if !defined(ENABLE_WIFI) || (ENABLE_WIFI != 1)
    PR_ERR("Playback network requires ENABLE_WIFI=1");
    return OPRT_NOT_SUPPORTED;
#else
    OPERATE_RET result;
    netconn_wifi_info_t wifi_info;

    if ((DEMO_WIFI_SSID[0] == '\0') || (DEMO_WIFI_PASSWORD[0] == '\0') || (DEMO_VIDEO_URL[0] == '\0')) {
        PR_ERR("Demo network configuration is incomplete");
        return OPRT_INVALID_PARM;
    }

    result = tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key = "dflfuap134ddlduq",
    });
    if (result != OPRT_OK) {
        return result;
    }

    result = tal_sw_timer_init();
    if (result != OPRT_OK) {
        return result;
    }

    result = tal_workq_init();
    if (result != OPRT_OK) {
        return result;
    }

    result = tal_event_subscribe(EVENT_LINK_STATUS_CHG,
                                 "playback_network",
                                 playback_link_status_callback,
                                 SUBSCRIBE_TYPE_NORMAL);
    if (result != OPRT_OK) {
        return result;
    }

#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
    TUYA_LwIP_Init();
#endif

    result = netmgr_init(NETCONN_WIFI);
    if (result != OPRT_OK) {
        return result;
    }

    memset(&wifi_info, 0, sizeof(wifi_info));
    strncpy(wifi_info.ssid, DEMO_WIFI_SSID, sizeof(wifi_info.ssid) - 1U);
    strncpy(wifi_info.pswd, DEMO_WIFI_PASSWORD, sizeof(wifi_info.pswd) - 1U);

    PR_NOTICE("Connecting Playback Board to Wi-Fi SSID: %s", DEMO_WIFI_SSID);
    PR_NOTICE("Configured video URL: %s", DEMO_VIDEO_URL);
    return netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &wifi_info);
#endif
}
