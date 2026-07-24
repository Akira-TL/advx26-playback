/**
 * @file playback_bluetooth_browser.c
 * @brief Classic Bluetooth inquiry, pairing and ACL connection for the local UI.
 */

#include "playback_bluetooth_browser.h"

#include <stdio.h>
#include <string.h>

#include "components/bluetooth/bk_dm_bt_types.h"
#include "components/bluetooth/bk_dm_bluetooth_types.h"
#include "components/bluetooth/bk_dm_bt.h"
#include "components/bluetooth/bk_dm_gap_bt.h"
#include "tal_api.h"

#define PLAYBACK_BLUETOOTH_INQUIRY_LENGTH (0x0AU)
#define PLAYBACK_BLUETOOTH_PIN_LENGTH (4U)

typedef struct
{
    playback_bluetooth_browser_config_t config;
    playback_bluetooth_browser_device_t devices[PLAYBACK_BLUETOOTH_BROWSER_MAX_DEVICES];
    size_t device_count;
    uint8_t target_address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES];
    playback_bluetooth_browser_status_t status;
    MUTEX_HANDLE mutex;
    bk_bt_linkkey_storage_t link_key;
    bool has_link_key;
    bool scanning;
    bool remote_name_query_active;
    uint8_t remote_name_query_address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES];
    volatile bool closing;
} playback_bluetooth_browser_state_t;

static playback_bluetooth_browser_state_t *playback_bluetooth_active_state;

static bool playback_bluetooth_address_equal(const uint8_t *left, const uint8_t *right)
{
    return memcmp(left, right, PLAYBACK_BLUETOOTH_ADDRESS_BYTES) == 0;
}

static void playback_bluetooth_copy_name(char *destination, size_t capacity, const uint8_t *source, size_t length)
{
    size_t copy_length;

    if ((destination == NULL) || (capacity == 0U))
    {
        return;
    }
    destination[0] = '\0';
    if ((source == NULL) || (length == 0U))
    {
        return;
    }
    copy_length = (length < (capacity - 1U)) ? length : (capacity - 1U);
    memcpy(destination, source, copy_length);
    destination[copy_length] = '\0';
}

static void playback_bluetooth_name_from_eir(
    char *destination,
    size_t capacity,
    const uint8_t *eir,
    size_t eir_length
)
{
    size_t offset = 0U;

    while (offset < eir_length)
    {
        const uint8_t field_length = eir[offset];
        uint8_t field_type;

        if (field_length == 0U)
        {
            return;
        }
        if ((offset + 1U + field_length) > eir_length)
        {
            return;
        }
        field_type = eir[offset + 1U];
        if ((field_type == BK_BT_EIR_TYPE_CMPL_LOCAL_NAME) ||
            (field_type == BK_BT_EIR_TYPE_SHORT_LOCAL_NAME))
        {
            playback_bluetooth_copy_name(
                destination,
                capacity,
                &eir[offset + 2U],
                (size_t)field_length - 1U
            );
            return;
        }
        offset += (size_t)field_length + 1U;
    }
}

static bool playback_bluetooth_is_audio_device(uint32_t class_of_device)
{
    const uint32_t service =
        (class_of_device & BK_BT_COD_SRVC_BIT_MASK) >> BK_BT_COD_SRVC_BIT_OFFSET;
    const uint32_t major =
        (class_of_device & BK_BT_COD_MAJOR_DEV_BIT_MASK) >> BK_BT_COD_MAJOR_DEV_BIT_OFFSET;

    return (major == BK_BT_COD_MAJOR_DEV_AV) ||
           ((service & (BK_BT_COD_SRVC_AUDIO | BK_BT_COD_SRVC_RENDERING)) != 0U);
}

static int playback_bluetooth_device_priority(
    const playback_bluetooth_browser_device_t *device
)
{
    return (device->audio_device ? 256 : 0) + (int)device->rssi + 128;
}

static void playback_bluetooth_sort_devices(playback_bluetooth_browser_state_t *state)
{
    size_t index;

    for (index = 1U; index < state->device_count; ++index)
    {
        playback_bluetooth_browser_device_t candidate = state->devices[index];
        size_t position = index;

        while ((position > 0U) &&
               (playback_bluetooth_device_priority(&candidate) >
                playback_bluetooth_device_priority(&state->devices[position - 1U])))
        {
            state->devices[position] = state->devices[position - 1U];
            --position;
        }
        state->devices[position] = candidate;
    }
}

static void playback_bluetooth_publish_devices(playback_bluetooth_browser_state_t *state)
{
    playback_bluetooth_browser_devices_cb callback;
    void *context;
    size_t device_count;
    bool scanning;

    tal_mutex_lock(state->mutex);
    callback = state->config.on_devices;
    context = state->config.context;
    device_count = state->device_count;
    scanning = state->scanning;
    tal_mutex_unlock(state->mutex);

    if (callback != NULL)
    {
        callback(context, state->devices, device_count, scanning);
    }
}

static void playback_bluetooth_publish_status(
    playback_bluetooth_browser_state_t *state,
    playback_bluetooth_browser_status_t status,
    const uint8_t *address,
    const char *detail
)
{
    playback_bluetooth_browser_status_cb callback;
    void *context;

    tal_mutex_lock(state->mutex);
    state->status = status;
    callback = state->config.on_status;
    context = state->config.context;
    tal_mutex_unlock(state->mutex);

    if (callback != NULL)
    {
        callback(context, status, address, detail);
    }
}

static void playback_bluetooth_start_next_name_query(
    playback_bluetooth_browser_state_t *state
)
{
    uint8_t address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES];
    size_t index;

    for (;;)
    {
        bool found = false;

        tal_mutex_lock(state->mutex);
        if (state->closing || state->scanning || state->remote_name_query_active)
        {
            tal_mutex_unlock(state->mutex);
            return;
        }
        for (index = 0U; index < state->device_count; ++index)
        {
            if (!state->devices[index].name_resolved)
            {
                memcpy(address, state->devices[index].address, sizeof(address));
                memcpy(
                    state->remote_name_query_address,
                    address,
                    sizeof(state->remote_name_query_address)
                );
                state->remote_name_query_active = true;
                found = true;
                break;
            }
        }
        tal_mutex_unlock(state->mutex);

        if (!found)
        {
            return;
        }
        if (bk_bt_gap_read_remote_name(address) == BK_OK)
        {
            return;
        }

        tal_mutex_lock(state->mutex);
        state->remote_name_query_active = false;
        for (index = 0U; index < state->device_count; ++index)
        {
            if (playback_bluetooth_address_equal(state->devices[index].address, address))
            {
                state->devices[index].name_resolved = true;
                break;
            }
        }
        tal_mutex_unlock(state->mutex);
        playback_bluetooth_publish_devices(state);
    }
}

static void playback_bluetooth_apply_remote_name(
    playback_bluetooth_browser_state_t *state,
    const bk_bt_gap_cb_param_t *parameter
)
{
    size_t index;

    tal_mutex_lock(state->mutex);
    state->remote_name_query_active = false;
    memset(
        state->remote_name_query_address,
        0,
        sizeof(state->remote_name_query_address)
    );
    for (index = 0U; index < state->device_count; ++index)
    {
        playback_bluetooth_browser_device_t *device = &state->devices[index];

        if (!playback_bluetooth_address_equal(
                device->address,
                parameter->read_rmt_name.bda
            ))
        {
            continue;
        }
        if ((parameter->read_rmt_name.stat == BK_BT_STATUS_SUCCESS) &&
            (parameter->read_rmt_name.rmt_name[0] != '\0'))
        {
            playback_bluetooth_copy_name(
                device->name,
                sizeof(device->name),
                parameter->read_rmt_name.rmt_name,
                strnlen(
                    (const char *)parameter->read_rmt_name.rmt_name,
                    BK_BT_GAP_MAX_BDNAME_LEN
                )
            );
        }
        device->name_resolved = true;
        break;
    }
    tal_mutex_unlock(state->mutex);

    playback_bluetooth_publish_devices(state);
    playback_bluetooth_start_next_name_query(state);
}

static void playback_bluetooth_update_device(
    playback_bluetooth_browser_state_t *state,
    const bk_bt_gap_cb_param_t *parameter
)
{
    playback_bluetooth_browser_device_t discovered;
    playback_bluetooth_browser_device_t *destination = NULL;
    size_t index;

    memset(&discovered, 0, sizeof(discovered));
    memcpy(discovered.address, parameter->disc_res.bda, sizeof(discovered.address));
    discovered.rssi = -127;

    for (index = 0U; index < (size_t)parameter->disc_res.num_prop; ++index)
    {
        const bk_bt_gap_dev_prop_t *property = &parameter->disc_res.prop[index];

        if ((property->val == NULL) || (property->len <= 0))
        {
            continue;
        }
        switch (property->type)
        {
            case BK_BT_GAP_DEV_PROP_BDNAME:
                playback_bluetooth_copy_name(
                    discovered.name,
                    sizeof(discovered.name),
                    property->val,
                    (size_t)property->len
                );
                break;
            case BK_BT_GAP_DEV_PROP_COD:
                if ((size_t)property->len >= sizeof(discovered.class_of_device))
                {
                    memcpy(
                        &discovered.class_of_device,
                        property->val,
                        sizeof(discovered.class_of_device)
                    );
                }
                break;
            case BK_BT_GAP_DEV_PROP_RSSI:
                discovered.rssi = *(const int8_t *)property->val;
                break;
            case BK_BT_GAP_DEV_PROP_EIR:
                if (discovered.name[0] == '\0')
                {
                    playback_bluetooth_name_from_eir(
                        discovered.name,
                        sizeof(discovered.name),
                        property->val,
                        (size_t)property->len
                    );
                }
                break;
            default:
                break;
        }
    }

    discovered.name_resolved = discovered.name[0] != '\0';
    if (!discovered.name_resolved)
    {
        snprintf(
            discovered.name,
            sizeof(discovered.name),
            "Bluetooth %02X%02X",
            discovered.address[1],
            discovered.address[0]
        );
    }
    discovered.audio_device = playback_bluetooth_is_audio_device(discovered.class_of_device);

    tal_mutex_lock(state->mutex);
    for (index = 0U; index < state->device_count; ++index)
    {
        if (playback_bluetooth_address_equal(state->devices[index].address, discovered.address))
        {
            destination = &state->devices[index];
            break;
        }
    }
    if ((destination == NULL) &&
        (state->device_count < PLAYBACK_BLUETOOTH_BROWSER_MAX_DEVICES))
    {
        destination = &state->devices[state->device_count++];
    }
    if (destination == NULL)
    {
        destination = &state->devices[state->device_count - 1U];
        if (playback_bluetooth_device_priority(&discovered) <=
            playback_bluetooth_device_priority(destination))
        {
            tal_mutex_unlock(state->mutex);
            return;
        }
    }
    if (destination->name_resolved && !discovered.name_resolved)
    {
        memcpy(discovered.name, destination->name, sizeof(discovered.name));
        discovered.name_resolved = true;
    }
    *destination = discovered;
    playback_bluetooth_sort_devices(state);
    tal_mutex_unlock(state->mutex);
    playback_bluetooth_publish_devices(state);
}

static void playback_bluetooth_gap_callback(
    bk_gap_bt_cb_event_t event,
    bk_bt_gap_cb_param_t *parameter
)
{
    playback_bluetooth_browser_state_t *state = playback_bluetooth_active_state;
    bk_bt_pin_code_t pin_code = {'0', '0', '0', '0'};

    if ((state == NULL) || state->closing || (parameter == NULL))
    {
        return;
    }

    switch (event)
    {
        case BK_BT_GAP_DISC_RES_EVT:
            playback_bluetooth_update_device(state, parameter);
            break;

        case BK_BT_GAP_DISC_STATE_CHANGED_EVT:
            tal_mutex_lock(state->mutex);
            state->scanning =
                parameter->disc_st_chg.state == BK_BT_GAP_DISCOVERY_STARTED;
            tal_mutex_unlock(state->mutex);
            if (state->scanning)
            {
                playback_bluetooth_publish_status(
                    state,
                    PLAYBACK_BLUETOOTH_BROWSER_SCANNING,
                    NULL,
                    "Scanning nearby classic Bluetooth devices"
                );
            }
            else if (state->status == PLAYBACK_BLUETOOTH_BROWSER_SCANNING)
            {
                playback_bluetooth_publish_status(
                    state,
                    PLAYBACK_BLUETOOTH_BROWSER_IDLE,
                    NULL,
                    "Scan complete"
                );
            }
            playback_bluetooth_publish_devices(state);
            if (!state->scanning)
            {
                playback_bluetooth_start_next_name_query(state);
            }
            break;

        case BK_BT_GAP_READ_REMOTE_NAME_EVT:
            playback_bluetooth_apply_remote_name(state, parameter);
            break;

        case BK_BT_GAP_PIN_REQ_EVT:
            (void)bk_bt_gap_pin_reply(
                parameter->pin_req.bda,
                true,
                PLAYBACK_BLUETOOTH_PIN_LENGTH,
                pin_code
            );
            break;

        case BK_BT_GAP_CFM_REQ_EVT:
            (void)bk_bt_gap_ssp_confirm_reply(parameter->cfm_req.bda, true);
            break;

        case BK_BT_GAP_AUTH_CMPL_EVT:
            if (parameter->auth_cmpl.stat == BK_BT_STATUS_SUCCESS)
            {
                playback_bluetooth_publish_status(
                    state,
                    PLAYBACK_BLUETOOTH_BROWSER_PAIRED,
                    parameter->auth_cmpl.bda,
                    "Paired and ready for A2DP"
                );
            }
            else
            {
                playback_bluetooth_publish_status(
                    state,
                    PLAYBACK_BLUETOOTH_BROWSER_FAILED,
                    parameter->auth_cmpl.bda,
                    "Bluetooth authentication failed"
                );
            }
            break;

        case BK_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
            if (parameter->acl_conn_cmpl_stat.stat == BK_BT_STATUS_SUCCESS)
            {
                playback_bluetooth_publish_status(
                    state,
                    PLAYBACK_BLUETOOTH_BROWSER_CONNECTED,
                    parameter->acl_conn_cmpl_stat.bda,
                    "Classic Bluetooth link connected"
                );
            }
            else
            {
                playback_bluetooth_publish_status(
                    state,
                    PLAYBACK_BLUETOOTH_BROWSER_FAILED,
                    parameter->acl_conn_cmpl_stat.bda,
                    "Classic Bluetooth connection failed"
                );
            }
            break;

        case BK_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
            playback_bluetooth_publish_status(
                state,
                PLAYBACK_BLUETOOTH_BROWSER_IDLE,
                parameter->acl_disconn_cmpl_stat.bda,
                "Bluetooth link disconnected"
            );
            break;

        case BK_BT_GAP_LINK_KEY_NOTIF_EVT:
            tal_mutex_lock(state->mutex);
            memset(&state->link_key, 0, sizeof(state->link_key));
            state->link_key.size = sizeof(state->link_key);
            memcpy(state->link_key.addr, parameter->link_key_notif.bda, sizeof(state->link_key.addr));
            memcpy(
                state->link_key.link_key,
                parameter->link_key_notif.link_key,
                sizeof(state->link_key.link_key)
            );
            state->has_link_key = true;
            tal_mutex_unlock(state->mutex);
            break;

        case BK_BT_GAP_LINK_KEY_REQ_EVT:
        {
            bk_bt_linkkey_storage_t reply;
            bool found;

            tal_mutex_lock(state->mutex);
            found = state->has_link_key &&
                    playback_bluetooth_address_equal(
                        state->link_key.addr,
                        parameter->link_key_req.bda
                    );
            reply = state->link_key;
            if (!found)
            {
                memset(&reply, 0, sizeof(reply));
                reply.size = sizeof(reply);
                memcpy(reply.addr, parameter->link_key_req.bda, sizeof(reply.addr));
            }
            tal_mutex_unlock(state->mutex);
            (void)bk_bt_gap_linkkey_reply(found ? 1U : 0U, &reply);
            break;
        }

        default:
            break;
    }
}

bool playback_bluetooth_browser_init(
    playback_bluetooth_browser_t *browser,
    const playback_bluetooth_browser_config_t *config
)
{
    playback_bluetooth_browser_state_t *state;
    bk_bt_io_cap_t io_capability = BK_BT_IO_CAP_NONE;
    bk_bt_pin_code_t pin_code = {'0', '0', '0', '0'};

    if ((browser == NULL) || (config == NULL) || (browser->state != NULL) ||
        (playback_bluetooth_active_state != NULL))
    {
        return false;
    }

    state = tal_psram_calloc(1U, sizeof(*state));
    if (state == NULL)
    {
        return false;
    }
    if (tal_mutex_create_init(&state->mutex) != OPRT_OK)
    {
        tal_psram_free(state);
        return false;
    }
    state->config = *config;
    state->status = PLAYBACK_BLUETOOTH_BROWSER_IDLE;
    playback_bluetooth_active_state = state;
    browser->state = state;

    if ((bk_bt_gap_register_callback(playback_bluetooth_gap_callback) != BK_OK) ||
        (bk_bt_gap_set_security_param(
             BK_BT_SP_IOCAP_MODE,
             &io_capability,
             sizeof(io_capability)
         ) != BK_OK) ||
        (bk_bt_gap_set_pin(
             BK_BT_PIN_TYPE_FIXED,
             PLAYBACK_BLUETOOTH_PIN_LENGTH,
             pin_code
         ) != BK_OK))
    {
        playback_bluetooth_browser_close(browser);
        return false;
    }

    playback_bluetooth_publish_status(
        state,
        PLAYBACK_BLUETOOTH_BROWSER_IDLE,
        NULL,
        "Swipe left and scan for a speaker"
    );
    return true;
}

bool playback_bluetooth_browser_start_scan(playback_bluetooth_browser_t *browser)
{
    playback_bluetooth_browser_state_t *state;

    if ((browser == NULL) || (browser->state == NULL))
    {
        return false;
    }
    state = browser->state;

    (void)bk_bt_gap_cancel_discovery();
    tal_mutex_lock(state->mutex);
    memset(state->devices, 0, sizeof(state->devices));
    state->device_count = 0U;
    state->scanning = true;
    tal_mutex_unlock(state->mutex);
    if (bk_bt_gap_start_discovery(
            BK_BT_INQ_MODE_GENERAL_INQUIRY,
            PLAYBACK_BLUETOOTH_INQUIRY_LENGTH,
            0U
        ) != BK_OK)
    {
        tal_mutex_lock(state->mutex);
        state->scanning = false;
        tal_mutex_unlock(state->mutex);
        return false;
    }
    return true;
}

bool playback_bluetooth_browser_connect(
    playback_bluetooth_browser_t *browser,
    const uint8_t address[PLAYBACK_BLUETOOTH_ADDRESS_BYTES]
)
{
    playback_bluetooth_browser_state_t *state;

    if ((browser == NULL) || (browser->state == NULL) || (address == NULL))
    {
        return false;
    }
    state = browser->state;
    (void)bk_bt_gap_cancel_discovery();

    tal_mutex_lock(state->mutex);
    memcpy(state->target_address, address, sizeof(state->target_address));
    state->scanning = false;
    tal_mutex_unlock(state->mutex);
    return true;
}

void playback_bluetooth_browser_close(playback_bluetooth_browser_t *browser)
{
    playback_bluetooth_browser_state_t *state;

    if ((browser == NULL) || (browser->state == NULL))
    {
        return;
    }
    state = browser->state;
    state->closing = true;
    (void)bk_bt_gap_cancel_discovery();
    if (playback_bluetooth_active_state == state)
    {
        playback_bluetooth_active_state = NULL;
    }
    browser->state = NULL;
    if (state->mutex != NULL)
    {
        tal_mutex_release(state->mutex);
    }
    tal_psram_free(state);
}

const char *playback_bluetooth_browser_status_name(
    playback_bluetooth_browser_status_t status
)
{
    static const char *const names[] = {
        "IDLE",
        "SCANNING",
        "CONNECTING",
        "CONNECTED",
        "PAIRED",
        "FAILED",
    };

    return ((unsigned int)status < (sizeof(names) / sizeof(names[0])))
               ? names[status]
               : "UNKNOWN";
}
