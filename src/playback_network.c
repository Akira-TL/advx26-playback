/**
 * @file playback_network.c
 * @brief Fixed Wi-Fi STA bootstrap for Playback media Range requests.
 */

#include <string.h>

#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tal_wifi.h"

#include "demo_network_config.h"
#include "playback_network.h"

#define PLAYBACK_WIFI_RETRY_INTERVAL_MS (5000U)

static TIMER_ID playback_wifi_retry_timer;
static bool playback_wifi_connected;
static char playback_wifi_local_ip[16];
static char playback_wifi_gateway[16];
static playback_network_status_callback_t playback_wifi_status_callback;
static void *playback_wifi_status_context;

static void playback_wifi_publish_status(void)
{
    if (playback_wifi_status_callback != NULL)
    {
        playback_wifi_status_callback(
            playback_wifi_status_context,
            playback_wifi_connected,
            playback_wifi_local_ip
        );
    }
}

static void playback_wifi_retry_callback(TIMER_ID timer_id, void *argument)
{
    OPERATE_RET result;

    (void)timer_id;
    (void)argument;
    if (playback_wifi_connected)
    {
        return;
    }

    PR_NOTICE("Retrying Playback Wi-Fi connection");
    result = tal_wifi_station_connect(
        (int8_t *)DEMO_WIFI_SSID,
        (int8_t *)DEMO_WIFI_PASSWORD
    );
    if (result != OPRT_OK)
    {
        PR_WARN("Playback Wi-Fi retry request failed: %d", result);
    }
}

static void playback_wifi_event_callback(WF_EVENT_E event, void *argument)
{
    NW_IP_S station_ip;

    (void)argument;
    switch (event)
    {
        case WFE_CONNECTED:
            playback_wifi_connected = true;
            /* Wi-Fi power save caps throughput at ~25KB/s; disable for streaming. */
            (void)tal_wifi_lp_disable();
            if (playback_wifi_retry_timer != NULL)
            {
                (void)tal_sw_timer_stop(playback_wifi_retry_timer);
            }
            memset(&station_ip, 0, sizeof(station_ip));
            if (tal_wifi_get_ip(WF_STATION, &station_ip) == OPRT_OK)
            {
                PR_NOTICE(
                    "Playback Wi-Fi connected: ip=%s gateway=%s",
                    station_ip.ip,
                    station_ip.gw
                );
                strncpy(
                    playback_wifi_local_ip,
                    station_ip.ip,
                    sizeof(playback_wifi_local_ip) - 1U
                );
                strncpy(
                    playback_wifi_gateway,
                    station_ip.gw,
                    sizeof(playback_wifi_gateway) - 1U
                );
                playback_wifi_publish_status();
            }
            else
            {
                PR_NOTICE("Playback Wi-Fi connected; waiting for Board Link session");
            }
            break;

        case WFE_CONNECT_FAILED:
            playback_wifi_connected = false;
            playback_wifi_local_ip[0] = '\0';
            playback_wifi_gateway[0] = '\0';
            playback_wifi_publish_status();
            PR_WARN("Playback Wi-Fi connection failed");
            if (playback_wifi_retry_timer != NULL)
            {
                (void)tal_sw_timer_start(
                    playback_wifi_retry_timer,
                    PLAYBACK_WIFI_RETRY_INTERVAL_MS,
                    TAL_TIMER_CYCLE
                );
            }
            break;

        case WFE_DISCONNECTED:
            playback_wifi_connected = false;
            playback_wifi_local_ip[0] = '\0';
            playback_wifi_gateway[0] = '\0';
            playback_wifi_publish_status();
            PR_WARN("Playback Wi-Fi disconnected");
            if (playback_wifi_retry_timer != NULL)
            {
                (void)tal_sw_timer_start(
                    playback_wifi_retry_timer,
                    PLAYBACK_WIFI_RETRY_INTERVAL_MS,
                    TAL_TIMER_CYCLE
                );
            }
            break;

        default:
            break;
    }
}

void playback_network_set_status_callback(
    playback_network_status_callback_t callback,
    void *context
)
{
    playback_wifi_status_callback = callback;
    playback_wifi_status_context = context;
}

OPERATE_RET playback_network_start(void)
{
#if !defined(ENABLE_WIFI) || (ENABLE_WIFI != 1)
    PR_ERR("Playback network requires ENABLE_WIFI=1");
    return OPRT_NOT_SUPPORTED;
#else
    OPERATE_RET result;

    if ((DEMO_WIFI_SSID[0] == '\0') || (DEMO_WIFI_PASSWORD[0] == '\0'))
    {
        PR_ERR("Playback Wi-Fi configuration is incomplete");
        return OPRT_INVALID_PARM;
    }

    result = tal_sw_timer_init();
    if (result != OPRT_OK)
    {
        return result;
    }
    result = tal_workq_init();
    if (result != OPRT_OK)
    {
        return result;
    }
    result = tal_sw_timer_create(
        playback_wifi_retry_callback,
        NULL,
        &playback_wifi_retry_timer
    );
    if (result != OPRT_OK)
    {
        return result;
    }

#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
    TUYA_LwIP_Init();
#endif

    result = tal_wifi_init(playback_wifi_event_callback);
    if (result != OPRT_OK)
    {
        return result;
    }
    result = tal_wifi_set_work_mode(WWM_STATION);
    if (result != OPRT_OK)
    {
        return result;
    }

    playback_wifi_connected = false;
    playback_wifi_local_ip[0] = '\0';
    playback_wifi_gateway[0] = '\0';
    playback_wifi_publish_status();
    (void)tal_sw_timer_start(
        playback_wifi_retry_timer,
        PLAYBACK_WIFI_RETRY_INTERVAL_MS,
        TAL_TIMER_CYCLE
    );
    PR_NOTICE("Connecting Playback Board to Wi-Fi SSID: %s", DEMO_WIFI_SSID);
    return tal_wifi_station_connect(
        (int8_t *)DEMO_WIFI_SSID,
        (int8_t *)DEMO_WIFI_PASSWORD
    );
#endif
}

OPERATE_RET playback_network_start_manual(void)
{
#if !defined(ENABLE_WIFI) || (ENABLE_WIFI != 1)
    PR_ERR("Playback network requires ENABLE_WIFI=1");
    return OPRT_NOT_SUPPORTED;
#else
    OPERATE_RET result;

    result = tal_sw_timer_init();
    if (result != OPRT_OK)
    {
        return result;
    }
    result = tal_workq_init();
    if (result != OPRT_OK)
    {
        return result;
    }

#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
    TUYA_LwIP_Init();
#endif

    result = tal_wifi_init(playback_wifi_event_callback);
    if (result != OPRT_OK)
    {
        return result;
    }
    result = tal_wifi_set_work_mode(WWM_STATION);
    if (result != OPRT_OK)
    {
        return result;
    }

    playback_wifi_connected = false;
    playback_wifi_local_ip[0] = '\0';
    playback_wifi_gateway[0] = '\0';
    playback_wifi_publish_status();
    PR_NOTICE("Playback Wi-Fi ready for interactive provisioning");
    return OPRT_OK;
#endif
}

const char *playback_network_get_local_ip(void)
{
    return playback_wifi_local_ip;
}

const char *playback_network_get_gateway(void)
{
    return playback_wifi_gateway;
}
