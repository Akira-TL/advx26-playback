/**
 * @file playback_network.c
 * @brief Fixed runtime Wi-Fi bootstrap for Playback media Range requests.
 */

#include <string.h>

#include "tuya_cloud_types.h"
#include "tal_api.h"

#include "netmgr.h"
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "netconn_wifi.h"
#endif

#include "demo_network_config.h"
#include "playback_network.h"

static OPERATE_RET playback_link_status_callback(void *data)
{
    netmgr_status_e status;

    if (data == NULL)
    {
        return OPRT_INVALID_PARM;
    }

    status = *((netmgr_status_e *)data);
    if ((status == NETMGR_LINK_UP) || (status == NETMGR_LINK_UP_SWITH))
    {
        PR_NOTICE("Playback Wi-Fi connected; waiting for Board Link session");
    }
    else
    {
        PR_NOTICE("Playback Wi-Fi link status=%d", status);
    }
    return OPRT_OK;
}

OPERATE_RET playback_network_start(void)
{
#if !defined(ENABLE_WIFI) || (ENABLE_WIFI != 1)
    PR_ERR("Playback network requires ENABLE_WIFI=1");
    return OPRT_NOT_SUPPORTED;
#else
    OPERATE_RET result;
    netconn_wifi_info_t wifi_info;

    if ((DEMO_WIFI_SSID[0] == '\0') || (DEMO_WIFI_PASSWORD[0] == '\0'))
    {
        PR_ERR("Playback Wi-Fi configuration is incomplete");
        return OPRT_INVALID_PARM;
    }

    result = tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key = "dflfuap134ddlduq",
    });
    if (result != OPRT_OK)
    {
        return result;
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
    result = tal_event_subscribe(
        EVENT_LINK_STATUS_CHG,
        "playback_network",
        playback_link_status_callback,
        SUBSCRIBE_TYPE_NORMAL
    );
    if (result != OPRT_OK)
    {
        return result;
    }

#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
    TUYA_LwIP_Init();
#endif

    result = netmgr_init(NETCONN_WIFI);
    if (result != OPRT_OK)
    {
        return result;
    }

    memset(&wifi_info, 0, sizeof(wifi_info));
    strncpy(wifi_info.ssid, DEMO_WIFI_SSID, sizeof(wifi_info.ssid) - 1U);
    strncpy(wifi_info.pswd, DEMO_WIFI_PASSWORD, sizeof(wifi_info.pswd) - 1U);

    PR_NOTICE("Connecting Playback Board to Wi-Fi SSID: %s", DEMO_WIFI_SSID);
    return netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &wifi_info);
#endif
}
