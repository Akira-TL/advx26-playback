#ifndef MOB_SCREEN_H
#define MOB_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build and load the initial MOB LVGL screen.
 *
 * The caller must hold the LVGL display lock while invoking this function.
 */
void mob_screen_create(void);

#ifdef __cplusplus
}
#endif

#endif /* MOB_SCREEN_H */
