/**
 * @file main.c
 * @brief Minimal TuyaOpen bootstrap for the Playback application.
 */

#include <string.h>

#include "playback_app.h"
#include "tal_api.h"

#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    (void)playback_app_start();
    while (1)
    {
        tal_system_sleep(500U);
    }
}
#else

static THREAD_HANDLE playback_app_thread_handle = NULL;

static void playback_app_thread(void *argument)
{
    (void)argument;
    (void)playback_app_start();

    tal_thread_delete(playback_app_thread_handle);
    playback_app_thread_handle = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thread_config;

    memset(&thread_config, 0, sizeof(thread_config));
    thread_config.stackDepth = 1024U * 8U;
    thread_config.priority = THREAD_PRIO_1;
    thread_config.thrdname = "playback_app";

    (void)tal_thread_create_and_start(
        &playback_app_thread_handle,
        NULL,
        NULL,
        playback_app_thread,
        NULL,
        &thread_config
    );
}
#endif
