#include "playback_board_link_gatt.h"

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tkl_bluetooth.h"
#include <components/bluetooth/bk_dm_bluetooth_types.h>
#include <components/bluetooth/bk_dm_gap_ble_types.h>
#include <components/bluetooth/bk_dm_gap_ble.h>

#include "playback_board_link_json.h"
#include "playback_board_link_wire.h"

#define PLAYBACK_BOARD_LINK_GATT_EVENT_QUEUE_DEPTH (16)
#define PLAYBACK_BOARD_LINK_GATT_WORKER_STACK_SIZE (12U * 1024U)
#define PLAYBACK_BOARD_LINK_GATT_WORKER_POLL_MS (500U)
#define PLAYBACK_BOARD_LINK_GATT_ADV_INTERVAL_MIN (160U)
#define PLAYBACK_BOARD_LINK_GATT_ADV_INTERVAL_MAX (320U)
#define PLAYBACK_BOARD_LINK_GATT_COMMAND_CHAR_INDEX (0U)
#define PLAYBACK_BOARD_LINK_GATT_REPORT_CHAR_INDEX (1U)
#define PLAYBACK_BOARD_LINK_GATT_CHARACTERISTIC_COUNT (2U)
#define PLAYBACK_BOARD_LINK_GATT_INVALID_CONNECTION (0xFFFFU)

static const uint8_t playback_board_link_service_uuid[16] = {
    0x51U, 0xC0U, 0x01U, 0x33U, 0xAFU, 0x80U, 0xA3U, 0x81U,
    0xF9U, 0x4AU, 0x75U, 0xDDU, 0xB5U, 0x87U, 0xD8U, 0x68U,
};

static const uint8_t playback_board_link_command_uuid[16] = {
    0x0EU, 0x78U, 0x1CU, 0xDEU, 0x37U, 0xD8U, 0x79U, 0xB6U,
    0xEBU, 0x46U, 0x6EU, 0xECU, 0xA5U, 0xF7U, 0x6FU, 0xC6U,
};

static const uint8_t playback_board_link_report_uuid[16] = {
    0xBEU, 0xEFU, 0xCDU, 0xAFU, 0x85U, 0xD6U, 0x43U, 0xA9U,
    0x7CU, 0x4AU, 0xC9U, 0x61U, 0x6AU, 0x80U, 0xB3U, 0xD1U,
};

static const uint8_t playback_board_link_adv_data[] = {
    0x02U, 0x01U, 0x06U,
    0x11U, 0x07U,
    0x51U, 0xC0U, 0x01U, 0x33U, 0xAFU, 0x80U, 0xA3U, 0x81U,
    0xF9U, 0x4AU, 0x75U, 0xDDU, 0xB5U, 0x87U, 0xD8U, 0x68U,
    0x09U, 0x08U, 'P', 'L', 'A', 'Y', 'B', 'A', 'C', 'K',
};

static const uint8_t playback_board_link_scan_response[] = {
    0x13U, 0x09U,
    'S', 'o', 'u', 'n', 'd', 'P', 'o', 'l', 'a', ' ',
    'P', 'l', 'a', 'y', 'b', 'a', 'c', 'k',
};

typedef enum
{
    PLAYBACK_BOARD_LINK_GATT_EVENT_STACK_READY = 0,
    PLAYBACK_BOARD_LINK_GATT_EVENT_CONNECTED,
    PLAYBACK_BOARD_LINK_GATT_EVENT_DISCONNECTED,
    PLAYBACK_BOARD_LINK_GATT_EVENT_SUBSCRIPTION,
    PLAYBACK_BOARD_LINK_GATT_EVENT_MTU_REQUEST,
    PLAYBACK_BOARD_LINK_GATT_EVENT_COMMAND_FRAGMENT,
    PLAYBACK_BOARD_LINK_GATT_EVENT_STOP,
} playback_board_link_gatt_event_kind_t;

typedef struct
{
    playback_board_link_gatt_event_kind_t kind;
    uint16_t connection_handle;
    uint16_t characteristic_handle;
    uint16_t value;
    uint16_t length;
    bool enabled;
    uint8_t data[PLAYBACK_BOARD_LINK_GATT_ATT_VALUE_MAX];
} playback_board_link_gatt_event_t;

typedef struct
{
    playback_board_link_gatt_config_t config;
    playback_board_link_gatt_status_t status;
    playback_hello_t local_hello;
    TKL_BLE_GATTS_PARAMS_T gatts;
    TKL_BLE_SERVICE_PARAMS_T service;
    TKL_BLE_CHAR_PARAMS_T characteristics[PLAYBACK_BOARD_LINK_GATT_CHARACTERISTIC_COUNT];
    QUEUE_HANDLE event_queue;
    THREAD_HANDLE worker_thread;
    SEM_HANDLE worker_stopped;
    MUTEX_HANDLE status_mutex;
    MUTEX_HANDLE send_mutex;
    playback_board_link_reassembler_t reassembler;
    uint8_t command_body[PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES + 1U];
    char report_body[PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES + 1U];
    uint8_t report_fragment[PLAYBACK_BOARD_LINK_GATT_ATT_VALUE_MAX];
    uint32_t next_report_message_id;
    bool starting;
    bool service_registered;
    bool stack_initialized;
    volatile bool closing;
} playback_board_link_gatt_state_t;

static playback_board_link_gatt_state_t *volatile playback_board_link_active_state = NULL;

static size_t playback_board_link_gatt_bounded_string_length(const char *value, size_t capacity)
{
    size_t length = 0U;

    if (value == NULL)
    {
        return capacity;
    }
    while ((length < capacity) && (value[length] != '\0'))
    {
        length++;
    }
    return length;
}

static void playback_board_link_gatt_copy_string(char *destination, size_t capacity, const char *source)
{
    size_t length;

    if ((destination == NULL) || (capacity == 0U))
    {
        return;
    }
    destination[0] = '\0';
    if (source == NULL)
    {
        return;
    }

    length = playback_board_link_gatt_bounded_string_length(source, capacity);
    if (length >= capacity)
    {
        length = capacity - 1U;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static uint16_t playback_board_link_gatt_att_value_capacity(uint16_t mtu)
{
    uint16_t capacity;

    if (mtu < PLAYBACK_BOARD_LINK_GATT_ATT_MTU_DEFAULT)
    {
        mtu = PLAYBACK_BOARD_LINK_GATT_ATT_MTU_DEFAULT;
    }
    if (mtu > PLAYBACK_BOARD_LINK_GATT_ATT_MTU_MAX)
    {
        mtu = PLAYBACK_BOARD_LINK_GATT_ATT_MTU_MAX;
    }

    capacity = (uint16_t)(mtu - 3U);
    return (capacity > PLAYBACK_BOARD_LINK_GATT_ATT_VALUE_MAX)
               ? PLAYBACK_BOARD_LINK_GATT_ATT_VALUE_MAX
               : capacity;
}

static void playback_board_link_gatt_status_snapshot(
    playback_board_link_gatt_state_t *state,
    playback_board_link_gatt_status_t *snapshot
)
{
    tal_mutex_lock(state->status_mutex);
    *snapshot = state->status;
    tal_mutex_unlock(state->status_mutex);
}

static void playback_board_link_gatt_publish_status(playback_board_link_gatt_state_t *state)
{
    playback_board_link_gatt_status_t snapshot;

    if (state->config.on_status == NULL)
    {
        return;
    }
    playback_board_link_gatt_status_snapshot(state, &snapshot);
    state->config.on_status(state->config.context, &snapshot);
}

static void playback_board_link_gatt_set_advertising(
    playback_board_link_gatt_state_t *state,
    bool advertising
)
{
    bool changed;

    tal_mutex_lock(state->status_mutex);
    changed = state->status.advertising != advertising;
    state->status.advertising = advertising;
    tal_mutex_unlock(state->status_mutex);

    if (changed)
    {
        playback_board_link_gatt_publish_status(state);
    }
}

static void playback_board_link_gatt_generate_boot_id(playback_board_link_gatt_state_t *state)
{
    const uint32_t random_a = (uint32_t)tal_system_get_random(UINT32_MAX);
    const uint32_t random_b = (uint32_t)tal_system_get_random(UINT32_MAX);
    const uint32_t uptime = (uint32_t)tal_system_get_millisecond();

    (void)snprintf(
        state->status.boot_id,
        sizeof(state->status.boot_id),
        "pb-%08lx-%08lx-%08lx",
        (unsigned long)random_a,
        (unsigned long)random_b,
        (unsigned long)uptime
    );
}

static void playback_board_link_gatt_configure_hello(playback_board_link_gatt_state_t *state)
{
    memset(&state->local_hello, 0, sizeof(state->local_hello));
    state->local_hello.protocol_major = PLAYBACK_BOARD_LINK_PROTOCOL_MAJOR;
    state->local_hello.protocol_minor = 0U;
    state->local_hello.max_message_bytes = PLAYBACK_BOARD_LINK_MAX_MESSAGE_BYTES;
    state->local_hello.capability_count = 2U;
    playback_board_link_gatt_copy_string(
        state->local_hello.boot_id,
        sizeof(state->local_hello.boot_id),
        state->status.boot_id
    );
    playback_board_link_gatt_copy_string(
        state->local_hello.capabilities[0],
        sizeof(state->local_hello.capabilities[0]),
        "BOARD_LINK_V1"
    );
    playback_board_link_gatt_copy_string(
        state->local_hello.capabilities[1],
        sizeof(state->local_hello.capabilities[1]),
        "T5AI_H264_MP3_V1"
    );
}

static void playback_board_link_gatt_configure_service(playback_board_link_gatt_state_t *state)
{
    memset(&state->gatts, 0, sizeof(state->gatts));
    memset(&state->service, 0, sizeof(state->service));
    memset(state->characteristics, 0, sizeof(state->characteristics));

    state->service.handle = TKL_BLE_GATT_INVALID_HANDLE;
    state->service.svc_uuid.uuid_type = TKL_BLE_UUID_TYPE_128;
    memcpy(
        state->service.svc_uuid.uuid.uuid128,
        playback_board_link_service_uuid,
        sizeof(playback_board_link_service_uuid)
    );
    state->service.type = TKL_BLE_UUID_SERVICE_PRIMARY;
    state->service.char_num = PLAYBACK_BOARD_LINK_GATT_CHARACTERISTIC_COUNT;
    state->service.p_char = state->characteristics;

    state->characteristics[PLAYBACK_BOARD_LINK_GATT_COMMAND_CHAR_INDEX].handle =
        TKL_BLE_GATT_INVALID_HANDLE;
    state->characteristics[PLAYBACK_BOARD_LINK_GATT_COMMAND_CHAR_INDEX].char_uuid.uuid_type =
        TKL_BLE_UUID_TYPE_128;
    memcpy(
        state->characteristics[PLAYBACK_BOARD_LINK_GATT_COMMAND_CHAR_INDEX].char_uuid.uuid.uuid128,
        playback_board_link_command_uuid,
        sizeof(playback_board_link_command_uuid)
    );
    state->characteristics[PLAYBACK_BOARD_LINK_GATT_COMMAND_CHAR_INDEX].property =
        TKL_BLE_GATT_CHAR_PROP_WRITE;
    state->characteristics[PLAYBACK_BOARD_LINK_GATT_COMMAND_CHAR_INDEX].permission =
        TKL_BLE_GATT_PERM_WRITE;
    state->characteristics[PLAYBACK_BOARD_LINK_GATT_COMMAND_CHAR_INDEX].value_len =
        PLAYBACK_BOARD_LINK_GATT_ATT_VALUE_MAX;

    state->characteristics[PLAYBACK_BOARD_LINK_GATT_REPORT_CHAR_INDEX].handle =
        TKL_BLE_GATT_INVALID_HANDLE;
    state->characteristics[PLAYBACK_BOARD_LINK_GATT_REPORT_CHAR_INDEX].char_uuid.uuid_type =
        TKL_BLE_UUID_TYPE_128;
    memcpy(
        state->characteristics[PLAYBACK_BOARD_LINK_GATT_REPORT_CHAR_INDEX].char_uuid.uuid.uuid128,
        playback_board_link_report_uuid,
        sizeof(playback_board_link_report_uuid)
    );
    state->characteristics[PLAYBACK_BOARD_LINK_GATT_REPORT_CHAR_INDEX].property =
        TKL_BLE_GATT_CHAR_PROP_NOTIFY;
    state->characteristics[PLAYBACK_BOARD_LINK_GATT_REPORT_CHAR_INDEX].permission =
        TKL_BLE_GATT_PERM_NONE;
    state->characteristics[PLAYBACK_BOARD_LINK_GATT_REPORT_CHAR_INDEX].value_len =
        PLAYBACK_BOARD_LINK_GATT_ATT_VALUE_MAX;

    state->gatts.svc_num = 1U;
    state->gatts.p_service = &state->service;
}

static OPERATE_RET playback_board_link_gatt_start_advertising(
    playback_board_link_gatt_state_t *state
)
{
    TKL_BLE_DATA_T adv_data;
    TKL_BLE_DATA_T scan_response;
    TKL_BLE_GAP_ADV_PARAMS_T adv_params;
    OPERATE_RET result;
    playback_board_link_gatt_status_t status;

    playback_board_link_gatt_status_snapshot(state, &status);
    if (!status.started || status.connected || status.advertising || state->closing)
    {
        return OPRT_OK;
    }

    memset(&adv_data, 0, sizeof(adv_data));
    memset(&scan_response, 0, sizeof(scan_response));
    memset(&adv_params, 0, sizeof(adv_params));

    adv_data.length = sizeof(playback_board_link_adv_data);
    adv_data.p_data = (uint8_t *)playback_board_link_adv_data;
    scan_response.length = sizeof(playback_board_link_scan_response);
    scan_response.p_data = (uint8_t *)playback_board_link_scan_response;

    if (bk_ble_gap_set_device_name(PLAYBACK_BOARD_LINK_GATT_DEVICE_NAME) != 0)
    {
        return OPRT_COM_ERROR;
    }
    result = tkl_ble_gap_adv_rsp_data_set(&adv_data, &scan_response);
    if (result != OPRT_OK)
    {
        return result;
    }

    adv_params.adv_type = TKL_BLE_GAP_ADV_TYPE_CONN_SCANNABLE_UNDIRECTED;
    adv_params.adv_interval_min = PLAYBACK_BOARD_LINK_GATT_ADV_INTERVAL_MIN;
    adv_params.adv_interval_max = PLAYBACK_BOARD_LINK_GATT_ADV_INTERVAL_MAX;
    adv_params.adv_channel_map = 0x07U;
    result = tkl_ble_gap_adv_start(&adv_params);
    if (result == OPRT_OK)
    {
        playback_board_link_gatt_set_advertising(state, true);
    }
    return result;
}

static bool playback_board_link_gatt_post_event(
    playback_board_link_gatt_state_t *state,
    const playback_board_link_gatt_event_t *event
)
{
    if ((state == NULL) || (state->event_queue == NULL) || state->closing)
    {
        return false;
    }
    if (tal_queue_post(state->event_queue, (void *)event, 0U) == OPRT_OK)
    {
        return true;
    }

    tal_mutex_lock(state->status_mutex);
    state->status.dropped_event_count++;
    tal_mutex_unlock(state->status_mutex);
    return false;
}

static void playback_board_link_gatt_gap_callback(TKL_BLE_GAP_PARAMS_EVT_T *event)
{
    playback_board_link_gatt_state_t *state = playback_board_link_active_state;
    playback_board_link_gatt_event_t queued;

    if ((state == NULL) || (event == NULL))
    {
        return;
    }
    memset(&queued, 0, sizeof(queued));
    queued.connection_handle = event->conn_handle;

    switch (event->type)
    {
        case TKL_BLE_EVT_STACK_INIT:
            if (event->result == OPRT_OK)
            {
                queued.kind = PLAYBACK_BOARD_LINK_GATT_EVENT_STACK_READY;
                (void)playback_board_link_gatt_post_event(state, &queued);
            }
            break;

        case TKL_BLE_GAP_EVT_CONNECT:
            if ((event->result == OPRT_OK) &&
                (event->gap_event.connect.role == TKL_BLE_ROLE_SERVER))
            {
                queued.kind = PLAYBACK_BOARD_LINK_GATT_EVENT_CONNECTED;
                (void)playback_board_link_gatt_post_event(state, &queued);
            }
            break;

        case TKL_BLE_GAP_EVT_DISCONNECT:
            if (event->gap_event.disconnect.role == TKL_BLE_ROLE_SERVER)
            {
                queued.kind = PLAYBACK_BOARD_LINK_GATT_EVENT_DISCONNECTED;
                (void)playback_board_link_gatt_post_event(state, &queued);
            }
            break;

        default:
            break;
    }
}

static void playback_board_link_gatt_gatt_callback(TKL_BLE_GATT_PARAMS_EVT_T *event)
{
    playback_board_link_gatt_state_t *state = playback_board_link_active_state;
    playback_board_link_gatt_event_t queued;
    const TKL_BLE_DATA_REPORT_T *write_report;

    if ((state == NULL) || (event == NULL))
    {
        return;
    }
    memset(&queued, 0, sizeof(queued));
    queued.connection_handle = event->conn_handle;

    switch (event->type)
    {
        case TKL_BLE_GATT_EVT_MTU_REQUEST:
            queued.kind = PLAYBACK_BOARD_LINK_GATT_EVENT_MTU_REQUEST;
            queued.value = event->gatt_event.exchange_mtu;
            (void)playback_board_link_gatt_post_event(state, &queued);
            break;

        case TKL_BLE_GATT_EVT_WRITE_REQ:
            write_report = &event->gatt_event.write_report;
            if ((write_report->char_handle !=
                 state->characteristics[PLAYBACK_BOARD_LINK_GATT_COMMAND_CHAR_INDEX].handle) ||
                (write_report->report.p_data == NULL) || (write_report->report.length == 0U) ||
                (write_report->report.length > PLAYBACK_BOARD_LINK_GATT_ATT_VALUE_MAX))
            {
                break;
            }

            queued.kind = PLAYBACK_BOARD_LINK_GATT_EVENT_COMMAND_FRAGMENT;
            queued.characteristic_handle = write_report->char_handle;
            queued.length = write_report->report.length;
            memcpy(queued.data, write_report->report.p_data, queued.length);
            (void)playback_board_link_gatt_post_event(state, &queued);
            break;

        case TKL_BLE_GATT_EVT_SUBSCRIBE:
            if (event->gatt_event.subscribe.char_handle ==
                state->characteristics[PLAYBACK_BOARD_LINK_GATT_REPORT_CHAR_INDEX].handle)
            {
                queued.kind = PLAYBACK_BOARD_LINK_GATT_EVENT_SUBSCRIPTION;
                queued.characteristic_handle = event->gatt_event.subscribe.char_handle;
                queued.enabled = event->gatt_event.subscribe.cur_notify != 0U;
                (void)playback_board_link_gatt_post_event(state, &queued);
            }
            break;

        default:
            break;
    }
}

static playback_board_link_gatt_result_t playback_board_link_gatt_send_report_state(
    playback_board_link_gatt_state_t *state,
    const playback_report_t *report
)
{
    playback_board_link_gatt_status_t status;
    playback_board_link_json_result_t json_result;
    playback_board_link_wire_result_t wire_result;
    size_t report_length;
    size_t fragment_payload_capacity;
    uint16_t fragment_count;
    uint16_t fragment_index;
    size_t fragment_length;
    uint32_t message_id;
    OPERATE_RET notify_result;

    if ((state == NULL) || (report == NULL))
    {
        return PLAYBACK_BOARD_LINK_GATT_INVALID_ARGUMENT;
    }

    tal_mutex_lock(state->send_mutex);
    playback_board_link_gatt_status_snapshot(state, &status);
    if (!status.connected)
    {
        tal_mutex_unlock(state->send_mutex);
        return PLAYBACK_BOARD_LINK_GATT_NOT_CONNECTED;
    }
    if (!status.subscribed)
    {
        tal_mutex_unlock(state->send_mutex);
        return PLAYBACK_BOARD_LINK_GATT_NOT_SUBSCRIBED;
    }

    json_result = playback_board_link_report_serialize(
        report,
        (report->kind == PLAYBACK_REPORT_HELLO_ACK) ? &state->local_hello : NULL,
        state->report_body,
        sizeof(state->report_body),
        &report_length
    );
    if (json_result != PLAYBACK_BOARD_LINK_JSON_OK)
    {
        tal_mutex_unlock(state->send_mutex);
        return PLAYBACK_BOARD_LINK_GATT_SERIALIZE_FAILED;
    }

    wire_result = playback_board_link_fragment_plan(
        report_length,
        status.att_value_capacity,
        &fragment_payload_capacity,
        &fragment_count
    );
    if (wire_result != PLAYBACK_BOARD_LINK_WIRE_OK)
    {
        tal_mutex_unlock(state->send_mutex);
        return PLAYBACK_BOARD_LINK_GATT_FRAGMENT_FAILED;
    }
    (void)fragment_payload_capacity;

    message_id = state->next_report_message_id++;
    if (message_id == 0U)
    {
        message_id = state->next_report_message_id++;
    }
    if (state->next_report_message_id == 0U)
    {
        state->next_report_message_id = 1U;
    }

    for (fragment_index = 0U; fragment_index < fragment_count; ++fragment_index)
    {
        wire_result = playback_board_link_fragment_encode(
            PLAYBACK_BOARD_LINK_MESSAGE_REPORT,
            message_id,
            (const uint8_t *)state->report_body,
            report_length,
            status.att_value_capacity,
            fragment_index,
            state->report_fragment,
            sizeof(state->report_fragment),
            &fragment_length
        );
        if (wire_result != PLAYBACK_BOARD_LINK_WIRE_OK)
        {
            tal_mutex_unlock(state->send_mutex);
            return PLAYBACK_BOARD_LINK_GATT_FRAGMENT_FAILED;
        }

        notify_result = tkl_ble_gatts_value_notify(
            status.connection_handle,
            state->characteristics[PLAYBACK_BOARD_LINK_GATT_REPORT_CHAR_INDEX].handle,
            state->report_fragment,
            (uint16_t)fragment_length
        );
        if (notify_result != OPRT_OK)
        {
            tal_mutex_unlock(state->send_mutex);
            return PLAYBACK_BOARD_LINK_GATT_NOTIFY_FAILED;
        }
    }

    tal_mutex_unlock(state->send_mutex);
    return PLAYBACK_BOARD_LINK_GATT_OK;
}

static void playback_board_link_gatt_send_nack(
    playback_board_link_gatt_state_t *state,
    const playback_command_t *command,
    playback_nack_t nack,
    const char *diagnostic
)
{
    playback_report_t report;

    memset(&report, 0, sizeof(report));
    report.kind = PLAYBACK_REPORT_NACK;
    report.sequence_id = (command != NULL) ? command->sequence_id : 0U;
    report.nack = nack;
    if (command != NULL)
    {
        playback_board_link_gatt_copy_string(
            report.session_id,
            sizeof(report.session_id),
            command->session_id
        );
    }
    playback_board_link_gatt_copy_string(
        report.diagnostic,
        sizeof(report.diagnostic),
        diagnostic
    );
    (void)playback_board_link_gatt_send_report_state(state, &report);
}

static void playback_board_link_gatt_handle_hello(
    playback_board_link_gatt_state_t *state,
    const playback_command_t *command
)
{
    playback_report_t report;
    playback_board_link_gatt_result_t result;

    memset(&report, 0, sizeof(report));
    report.kind = PLAYBACK_REPORT_HELLO_ACK;
    report.sequence_id = command->sequence_id;
    result = playback_board_link_gatt_send_report_state(state, &report);
    if (result != PLAYBACK_BOARD_LINK_GATT_OK)
    {
        return;
    }

    tal_mutex_lock(state->status_mutex);
    state->status.handshake_complete = true;
    tal_mutex_unlock(state->status_mutex);
    playback_board_link_gatt_publish_status(state);

    if (state->config.on_command != NULL)
    {
        state->config.on_command(state->config.context, command);
    }
}

static void playback_board_link_gatt_handle_complete_command(
    playback_board_link_gatt_state_t *state,
    size_t body_length
)
{
    playback_command_t command;
    playback_board_link_json_result_t json_result;
    playback_board_link_gatt_status_t status;

    memset(&command, 0, sizeof(command));
    json_result = playback_board_link_command_parse(
        state->command_body,
        body_length,
        &command
    );
    if (json_result != PLAYBACK_BOARD_LINK_JSON_OK)
    {
        playback_board_link_gatt_send_nack(
            state,
            &command,
            playback_board_link_json_result_to_nack(json_result),
            playback_board_link_json_result_name(json_result)
        );
        return;
    }

    if (command.kind == PLAYBACK_COMMAND_HELLO)
    {
        playback_board_link_gatt_handle_hello(state, &command);
        return;
    }

    playback_board_link_gatt_status_snapshot(state, &status);
    if (!status.handshake_complete)
    {
        playback_board_link_gatt_send_nack(
            state,
            &command,
            PLAYBACK_NACK_PROTOCOL_INCOMPATIBLE,
            "HELLO required"
        );
        return;
    }

    if (state->config.on_command != NULL)
    {
        state->config.on_command(state->config.context, &command);
    }
}

static void playback_board_link_gatt_process_fragment(
    playback_board_link_gatt_state_t *state,
    const playback_board_link_gatt_event_t *event
)
{
    playback_board_link_message_t message;
    playback_board_link_wire_result_t result;

    result = playback_board_link_reassembler_push(
        &state->reassembler,
        (uint32_t)tal_system_get_millisecond(),
        event->data,
        event->length,
        state->command_body,
        sizeof(state->command_body),
        &message
    );
    if (result == PLAYBACK_BOARD_LINK_WIRE_INCOMPLETE)
    {
        return;
    }
    if (result != PLAYBACK_BOARD_LINK_WIRE_COMPLETE)
    {
        playback_board_link_gatt_send_nack(
            state,
            NULL,
            PLAYBACK_NACK_MALFORMED_MESSAGE,
            playback_board_link_wire_result_name(result)
        );
        return;
    }
    if (message.message_kind != PLAYBACK_BOARD_LINK_MESSAGE_COMMAND)
    {
        playback_board_link_gatt_send_nack(
            state,
            NULL,
            PLAYBACK_NACK_MALFORMED_MESSAGE,
            "Command characteristic requires command message kind"
        );
        return;
    }

    playback_board_link_gatt_handle_complete_command(state, message.body_length);
}

static void playback_board_link_gatt_process_connected(
    playback_board_link_gatt_state_t *state,
    uint16_t connection_handle
)
{
    playback_board_link_reassembler_reset(&state->reassembler);

    tal_mutex_lock(state->status_mutex);
    state->status.advertising = false;
    state->status.connected = true;
    state->status.subscribed = false;
    state->status.handshake_complete = false;
    state->status.connection_handle = connection_handle;
    state->status.att_mtu = PLAYBACK_BOARD_LINK_GATT_ATT_MTU_DEFAULT;
    state->status.att_value_capacity = playback_board_link_gatt_att_value_capacity(
        PLAYBACK_BOARD_LINK_GATT_ATT_MTU_DEFAULT
    );
    tal_mutex_unlock(state->status_mutex);
    playback_board_link_gatt_publish_status(state);
}

static void playback_board_link_gatt_process_disconnected(
    playback_board_link_gatt_state_t *state
)
{
    playback_board_link_reassembler_reset(&state->reassembler);

    tal_mutex_lock(state->status_mutex);
    state->status.advertising = false;
    state->status.connected = false;
    state->status.subscribed = false;
    state->status.handshake_complete = false;
    state->status.connection_handle = PLAYBACK_BOARD_LINK_GATT_INVALID_CONNECTION;
    state->status.att_mtu = PLAYBACK_BOARD_LINK_GATT_ATT_MTU_DEFAULT;
    state->status.att_value_capacity = playback_board_link_gatt_att_value_capacity(
        PLAYBACK_BOARD_LINK_GATT_ATT_MTU_DEFAULT
    );
    tal_mutex_unlock(state->status_mutex);
    playback_board_link_gatt_publish_status(state);
    (void)playback_board_link_gatt_start_advertising(state);
}

static void playback_board_link_gatt_process_subscription(
    playback_board_link_gatt_state_t *state,
    const playback_board_link_gatt_event_t *event
)
{
    bool changed = false;

    tal_mutex_lock(state->status_mutex);
    if (state->status.connected &&
        (state->status.connection_handle == event->connection_handle) &&
        (state->status.subscribed != event->enabled))
    {
        state->status.subscribed = event->enabled;
        if (!event->enabled)
        {
            state->status.handshake_complete = false;
        }
        changed = true;
    }
    tal_mutex_unlock(state->status_mutex);

    if (changed)
    {
        playback_board_link_gatt_publish_status(state);
    }
}

static void playback_board_link_gatt_process_mtu(
    playback_board_link_gatt_state_t *state,
    const playback_board_link_gatt_event_t *event
)
{
    uint16_t mtu = event->value;

    if (mtu < PLAYBACK_BOARD_LINK_GATT_ATT_MTU_DEFAULT)
    {
        mtu = PLAYBACK_BOARD_LINK_GATT_ATT_MTU_DEFAULT;
    }
    if (mtu > PLAYBACK_BOARD_LINK_GATT_ATT_MTU_MAX)
    {
        mtu = PLAYBACK_BOARD_LINK_GATT_ATT_MTU_MAX;
    }
    if (tkl_ble_gatts_exchange_mtu_reply(event->connection_handle, mtu) != OPRT_OK)
    {
        return;
    }

    tal_mutex_lock(state->status_mutex);
    if (state->status.connected && (state->status.connection_handle == event->connection_handle))
    {
        state->status.att_mtu = mtu;
        state->status.att_value_capacity = playback_board_link_gatt_att_value_capacity(mtu);
    }
    tal_mutex_unlock(state->status_mutex);
    playback_board_link_gatt_publish_status(state);
}

static void playback_board_link_gatt_worker(void *argument)
{
    playback_board_link_gatt_state_t *state = argument;
    playback_board_link_gatt_event_t event;
    playback_board_link_gatt_status_t status;

    for (;;)
    {
        if (tal_queue_fetch(
                state->event_queue,
                &event,
                PLAYBACK_BOARD_LINK_GATT_WORKER_POLL_MS
            ) != OPRT_OK)
        {
            (void)playback_board_link_reassembler_expire(
                &state->reassembler,
                (uint32_t)tal_system_get_millisecond()
            );
            playback_board_link_gatt_status_snapshot(state, &status);
            if (status.started && !status.connected && !status.advertising)
            {
                (void)playback_board_link_gatt_start_advertising(state);
            }
            continue;
        }

        switch (event.kind)
        {
            case PLAYBACK_BOARD_LINK_GATT_EVENT_STACK_READY:
                (void)playback_board_link_gatt_start_advertising(state);
                break;

            case PLAYBACK_BOARD_LINK_GATT_EVENT_CONNECTED:
                playback_board_link_gatt_process_connected(state, event.connection_handle);
                break;

            case PLAYBACK_BOARD_LINK_GATT_EVENT_DISCONNECTED:
                playback_board_link_gatt_process_disconnected(state);
                break;

            case PLAYBACK_BOARD_LINK_GATT_EVENT_SUBSCRIPTION:
                playback_board_link_gatt_process_subscription(state, &event);
                break;

            case PLAYBACK_BOARD_LINK_GATT_EVENT_MTU_REQUEST:
                playback_board_link_gatt_process_mtu(state, &event);
                break;

            case PLAYBACK_BOARD_LINK_GATT_EVENT_COMMAND_FRAGMENT:
                playback_board_link_gatt_process_fragment(state, &event);
                break;

            case PLAYBACK_BOARD_LINK_GATT_EVENT_STOP:
                tal_semaphore_post(state->worker_stopped);
                return;

            default:
                break;
        }
    }
}

static void playback_board_link_gatt_release_state(playback_board_link_gatt_state_t *state)
{
    if (state == NULL)
    {
        return;
    }
    playback_board_link_reassembler_close(&state->reassembler);
    if (state->event_queue != NULL)
    {
        tal_queue_free(state->event_queue);
    }
    if (state->worker_stopped != NULL)
    {
        tal_semaphore_release(state->worker_stopped);
    }
    if (state->send_mutex != NULL)
    {
        tal_mutex_release(state->send_mutex);
    }
    if (state->status_mutex != NULL)
    {
        tal_mutex_release(state->status_mutex);
    }
    tal_psram_free(state);
}

playback_board_link_gatt_result_t playback_board_link_gatt_init(
    playback_board_link_gatt_t *gatt,
    const playback_board_link_gatt_config_t *config
)
{
    playback_board_link_gatt_state_t *state;
    THREAD_CFG_T worker_config;

    if ((gatt == NULL) || (config == NULL) || (config->on_command == NULL))
    {
        return PLAYBACK_BOARD_LINK_GATT_INVALID_ARGUMENT;
    }
    if ((gatt->state != NULL) || (playback_board_link_active_state != NULL))
    {
        return PLAYBACK_BOARD_LINK_GATT_ALREADY_INITIALIZED;
    }

    state = tal_psram_calloc(1U, sizeof(*state));
    if (state == NULL)
    {
        return PLAYBACK_BOARD_LINK_GATT_NO_MEMORY;
    }
    state->config = *config;
    state->next_report_message_id = 1U;
    state->status.initialized = true;
    state->status.connection_handle = PLAYBACK_BOARD_LINK_GATT_INVALID_CONNECTION;
    state->status.att_mtu = PLAYBACK_BOARD_LINK_GATT_ATT_MTU_DEFAULT;
    state->status.att_value_capacity = playback_board_link_gatt_att_value_capacity(
        PLAYBACK_BOARD_LINK_GATT_ATT_MTU_DEFAULT
    );
    playback_board_link_gatt_generate_boot_id(state);
    playback_board_link_gatt_configure_hello(state);
    playback_board_link_gatt_configure_service(state);

    if ((tal_mutex_create_init(&state->status_mutex) != OPRT_OK) ||
        (tal_mutex_create_init(&state->send_mutex) != OPRT_OK) ||
        (tal_semaphore_create_init(&state->worker_stopped, 0U, 1U) != OPRT_OK) ||
        (tal_queue_create_init(
             &state->event_queue,
             sizeof(playback_board_link_gatt_event_t),
             PLAYBACK_BOARD_LINK_GATT_EVENT_QUEUE_DEPTH
         ) != OPRT_OK) ||
        (playback_board_link_reassembler_init(&state->reassembler) != PLAYBACK_BOARD_LINK_WIRE_OK))
    {
        playback_board_link_gatt_release_state(state);
        return PLAYBACK_BOARD_LINK_GATT_NO_MEMORY;
    }

    memset(&worker_config, 0, sizeof(worker_config));
    worker_config.stackDepth = PLAYBACK_BOARD_LINK_GATT_WORKER_STACK_SIZE;
    worker_config.priority = THREAD_PRIO_2;
    worker_config.thrdname = "board_link";
    worker_config.psram_mode = 1U;
    if (tal_thread_create_and_start(
            &state->worker_thread,
            NULL,
            NULL,
            playback_board_link_gatt_worker,
            state,
            &worker_config
        ) != OPRT_OK)
    {
        playback_board_link_gatt_release_state(state);
        return PLAYBACK_BOARD_LINK_GATT_PLATFORM_ERROR;
    }

    playback_board_link_active_state = state;
    gatt->state = state;
    return PLAYBACK_BOARD_LINK_GATT_OK;
}

playback_board_link_gatt_result_t playback_board_link_gatt_start(
    playback_board_link_gatt_t *gatt
)
{
    playback_board_link_gatt_state_t *state;

    if ((gatt == NULL) || (gatt->state == NULL))
    {
        return PLAYBACK_BOARD_LINK_GATT_NOT_INITIALIZED;
    }
    state = gatt->state;

    tal_mutex_lock(state->status_mutex);
    if (state->status.started || state->starting)
    {
        tal_mutex_unlock(state->status_mutex);
        return PLAYBACK_BOARD_LINK_GATT_ALREADY_STARTED;
    }
    state->starting = true;
    tal_mutex_unlock(state->status_mutex);

    if ((tkl_ble_gap_callback_register(playback_board_link_gatt_gap_callback) != OPRT_OK) ||
        (tkl_ble_gatt_callback_register(playback_board_link_gatt_gatt_callback) != OPRT_OK) ||
        (tkl_ble_gatts_service_add(&state->gatts) != OPRT_OK))
    {
        tal_mutex_lock(state->status_mutex);
        state->starting = false;
        tal_mutex_unlock(state->status_mutex);
        return PLAYBACK_BOARD_LINK_GATT_PLATFORM_ERROR;
    }
    state->service_registered = true;

    if (tkl_ble_stack_init(TKL_BLE_ROLE_SERVER) != OPRT_OK)
    {
        (void)tkl_ble_stack_deinit(TKL_BLE_ROLE_SERVER);
        state->service_registered = false;
        tal_mutex_lock(state->status_mutex);
        state->starting = false;
        tal_mutex_unlock(state->status_mutex);
        return PLAYBACK_BOARD_LINK_GATT_PLATFORM_ERROR;
    }
    state->stack_initialized = true;
    tal_mutex_lock(state->status_mutex);
    state->starting = false;
    state->status.started = true;
    tal_mutex_unlock(state->status_mutex);
    return PLAYBACK_BOARD_LINK_GATT_OK;
}

playback_board_link_gatt_result_t playback_board_link_gatt_send_report(
    playback_board_link_gatt_t *gatt,
    const playback_report_t *report
)
{
    if ((gatt == NULL) || (gatt->state == NULL))
    {
        return PLAYBACK_BOARD_LINK_GATT_NOT_INITIALIZED;
    }
    return playback_board_link_gatt_send_report_state(gatt->state, report);
}

playback_board_link_gatt_result_t playback_board_link_gatt_get_status(
    playback_board_link_gatt_t *gatt,
    playback_board_link_gatt_status_t *status
)
{
    if (status == NULL)
    {
        return PLAYBACK_BOARD_LINK_GATT_INVALID_ARGUMENT;
    }
    if ((gatt == NULL) || (gatt->state == NULL))
    {
        return PLAYBACK_BOARD_LINK_GATT_NOT_INITIALIZED;
    }
    playback_board_link_gatt_status_snapshot(gatt->state, status);
    return PLAYBACK_BOARD_LINK_GATT_OK;
}

void playback_board_link_gatt_close(playback_board_link_gatt_t *gatt)
{
    playback_board_link_gatt_state_t *state;
    playback_board_link_gatt_event_t stop_event;
    playback_board_link_gatt_status_t status;

    if ((gatt == NULL) || (gatt->state == NULL))
    {
        return;
    }
    state = gatt->state;
    state->closing = true;

    playback_board_link_gatt_status_snapshot(state, &status);
    if (status.advertising)
    {
        (void)tkl_ble_gap_adv_stop();
    }
    if (state->stack_initialized || state->service_registered)
    {
        (void)tkl_ble_stack_deinit(TKL_BLE_ROLE_SERVER);
        state->stack_initialized = false;
        state->service_registered = false;
    }
    playback_board_link_active_state = NULL;

    memset(&stop_event, 0, sizeof(stop_event));
    stop_event.kind = PLAYBACK_BOARD_LINK_GATT_EVENT_STOP;
    (void)tal_queue_post(state->event_queue, &stop_event, QUEUE_WAIT_FOREVER);
    (void)tal_semaphore_wait(state->worker_stopped, 3000U);
    if (state->worker_thread != NULL)
    {
        (void)tal_thread_delete(state->worker_thread);
    }

    gatt->state = NULL;
    playback_board_link_gatt_release_state(state);
}

const char *playback_board_link_gatt_result_name(playback_board_link_gatt_result_t result)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARGUMENT",
        "ALREADY_INITIALIZED",
        "NOT_INITIALIZED",
        "ALREADY_STARTED",
        "NOT_CONNECTED",
        "NOT_SUBSCRIBED",
        "QUEUE_FULL",
        "SERIALIZE_FAILED",
        "FRAGMENT_FAILED",
        "NOTIFY_FAILED",
        "NO_MEMORY",
        "PLATFORM_ERROR",
    };

    return ((unsigned int)result < (sizeof(names) / sizeof(names[0])))
               ? names[result]
               : "UNKNOWN_BOARD_LINK_GATT_RESULT";
}
