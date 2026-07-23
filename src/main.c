/**
 * @file main.c
 * @brief TuyaOpen startup for the MOB custom display.
 */

#include <string.h>

#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "tkl_system.h"

#include "lvgl.h"
#include "lv_vendor.h"
#include "board_com_api.h"

#include "mob_screen.h"

static void mob_app_start(void)
{
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 4096, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("Application information:");
    PR_NOTICE("Project name:        %s", PROJECT_NAME);
    PR_NOTICE("App version:         %s", PROJECT_VERSION);
    PR_NOTICE("Compile time:        %s", __DATE__);
    PR_NOTICE("TuyaOpen version:    %s", OPEN_VERSION);
    PR_NOTICE("TuyaOpen commit-id:  %s", OPEN_COMMIT);
    PR_NOTICE("Platform chip:       %s", PLATFORM_CHIP);
    PR_NOTICE("Platform board:      %s", PLATFORM_BOARD);
    PR_NOTICE("Platform commit-id:  %s", PLATFORM_COMMIT);

    board_register_hardware();
    lv_vendor_init(DISPLAY_NAME);

    lv_vendor_disp_lock();
    mob_screen_create();
    lv_vendor_disp_unlock();

    lv_vendor_start(5, 1024 * 8);
}

#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    mob_app_start();

    while (1)
    {
        tal_system_sleep(500);
    }
}
#else

static THREAD_HANDLE mob_app_thread_handle = NULL;

static void mob_app_thread(void *arg)
{
    (void)arg;

    mob_app_start();

    tal_thread_delete(mob_app_thread_handle);
    mob_app_thread_handle = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thread_config;

    memset(&thread_config, 0, sizeof(thread_config));
    thread_config.stackDepth = 1024 * 4;
    thread_config.priority = THREAD_PRIO_1;
    thread_config.thrdname = "mob_app_main";

    tal_thread_create_and_start(&mob_app_thread_handle, NULL, NULL, mob_app_thread, NULL, &thread_config);
}
#endif
