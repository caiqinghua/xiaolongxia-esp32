#include "sdk_stubs.h"
#include "watch_blufi_btc_envelope.h"
#include "watch_blufi_send.h"
#undef btc_transfer_context
#include <stdio.h>

esp_err_t esp_blufi_send_custom_data(uint8_t*, uint32_t);
esp_err_t esp_blufi_profile_init(void);
esp_err_t esp_blufi_profile_deinit(void);

static uint64_t grant;
static unsigned queued_count, copied, freed;
static int queued_size;
static btc_msg_t* queued;
static btc_arg_deep_free_t free_hook;
static bool fail_queue;
static bool fail_post;
static unsigned dispatched;

uint64_t audream_blufi_transport_lease(void) { return grant; }
void btc_blufi_set_callbacks(esp_blufi_callbacks_t* callbacks) {}
int btc_profile_cb_set(int profile, esp_blufi_event_cb_t callback) { return 0; }
uint16_t btc_blufi_get_version(void) { return 0x104; }

static void CopyData(uint8_t** destination, const uint8_t* source, size_t size) {
    *destination = malloc(size);
    assert(*destination);
    memcpy(*destination, source, size);
    ++copied;
}
void btc_blufi_call_deep_copy(btc_msg_t* msg, void* destination, void* source) {
    btc_blufi_args_t* dst = destination;
    const btc_blufi_args_t* src = source;
    assert(msg->sig == BTC_SIG_API_CALL);
    uint8_t* data = NULL;
    switch (msg->act) {
        case BTC_BLUFI_ACT_SEND_CUSTOM_DATA:
            CopyData(&data, src->custom_data.data, src->custom_data.data_len);
            dst->custom_data.data = data;
            break;
        case BTC_BLUFI_ACT_SEND_WIFI_LIST:
            CopyData(&data, (const uint8_t*)src->wifi_list.list,
                src->wifi_list.apCount * sizeof(esp_blufi_ap_record_t));
            dst->wifi_list.list = (esp_blufi_ap_record_t*)data;
            break;
        case BTC_BLUFI_ACT_SEND_CFG_REPORT:
            assert(!src->wifi_conn_report.extra_info);
            break;
        default: assert(false);
    }
}
void btc_blufi_call_deep_free(btc_msg_t* msg) {
    btc_blufi_args_t* args = (btc_blufi_args_t*)msg->arg;
    switch (msg->act) {
        case BTC_BLUFI_ACT_SEND_CUSTOM_DATA:
            free(args->custom_data.data); ++freed;
            break;
        case BTC_BLUFI_ACT_SEND_WIFI_LIST:
            free(args->wifi_list.list); ++freed;
            break;
        case BTC_BLUFI_ACT_SEND_CFG_REPORT:
            assert(!args->wifi_conn_report.extra_info);
            break;
        default: assert(false);
    }
}
static void CallbackCopy(btc_msg_t* msg, void* destination, void* source) {
    esp_blufi_cb_param_t* dst = destination;
    const esp_blufi_cb_param_t* src = source;
    uint8_t* data = NULL;
    CopyData(&data, src->custom_data.data, src->custom_data.data_len);
    dst->custom_data.data = data;
}
static void CallbackFree(btc_msg_t* msg) {
    esp_blufi_cb_param_t* args = (esp_blufi_cb_param_t*)msg->arg;
    free(args->custom_data.data); ++freed;
}
bt_status_t btc_transfer_context(btc_msg_t* msg, void* arg, int size,
    btc_arg_deep_copy_t copy, btc_arg_deep_free_t cleanup) {
    if (fail_queue) return BT_STATUS_FAIL;
    assert(!queued && size >= 0);
    queued = malloc(sizeof(*queued) + (size_t)size); assert(queued);
    memcpy(queued, msg, sizeof(*queued)); queued_size = size;
    if (size) {
        memcpy(queued->arg, arg, (size_t)size);
        if (copy) copy(queued, queued->arg, arg);
    }
    free_hook = cleanup;
    if (fail_post) {
        if (copy && cleanup) cleanup(queued);
        free(queued); queued = NULL; free_hook = NULL;
        return BT_STATUS_FAIL;
    }
    ++queued_count;
    return BT_STATUS_SUCCESS;
}
static void Release(void) {
    if (free_hook) free_hook(queued);
    free(queued); queued = NULL; free_hook = NULL;
}

static btc_msg_t* Take(void) {
    btc_msg_t* message = queued;
    queued = NULL; free_hook = NULL;
    return message;
}

static void RevokeDuringDispatch(btc_msg_t* message) {
    ++dispatched;
    const uint64_t original = audream_blufi_message_lease(message);
    assert(audream_blufi_operation_lease() == original);
    ++grant;
    uint8_t response[] = {4, 3, 2, 1};
    assert(esp_blufi_send_custom_data(response, sizeof(response)) == ESP_FAIL);
    assert(audream_blufi_send_custom_data_for_lease(grant, response, sizeof(response)) == ESP_FAIL);
    btc_msg_t callback = {.sig = BTC_SIG_API_CB, .pid = BTC_PID_BLUFI, .act = ESP_BLUFI_EVENT_RECV_CUSTOM_DATA};
    assert(audream_blufi_transfer_context(&callback, NULL, 0, NULL, NULL) == BT_STATUS_FAIL);
    assert(!queued && audream_blufi_operation_lease() == 0);
    btc_blufi_call_deep_free(message);
}

static void UnexpectedDispatch(btc_msg_t* message) {
    (void)message;
    assert(false);
}

int main(void) {
    uint8_t data[] = {1, 2, 3, 4};
    assert(esp_blufi_send_custom_data(data, sizeof(data)) == ESP_FAIL && queued_count == 0 && copied == 0);
    grant = 101;
    assert(esp_blufi_send_custom_data(data, sizeof(data)) == ESP_OK);
    assert(queued_size == sizeof(audream_blufi_btc_envelope_t) && copied == 1);
    assert(audream_blufi_message_current(queued));
    assert(sizeof(btc_msg_t) == 4 && ((uintptr_t)queued->arg % 8) == 4);
    const btc_blufi_args_t* args = (const btc_blufi_args_t*)queued->arg;
    data[0] = 9;
    assert(args->custom_data.data[0] == 1 && args->custom_data.data_len == 4);
    grant = 0; assert(!audream_blufi_message_current(queued));
    grant = 102; assert(!audream_blufi_message_current(queued));
    Release(); assert(freed == copied);

    // The caller's old grant must not be relabeled with the replacement peer's grant.
    const unsigned before_stale = queued_count;
    assert(audream_blufi_send_custom_data_for_lease(101, data, sizeof(data)) == ESP_FAIL);
    assert(audream_blufi_send_wifi_conn_report_for_lease(101, 0, 0, 0, NULL) == ESP_FAIL);
    assert(audream_blufi_send_wifi_list_for_lease(101, 0, NULL) == ESP_FAIL);
    assert(audream_blufi_send_custom_data_for_lease(0, data, sizeof(data)) == ESP_FAIL);
    assert(queued_count == before_stale && copied == freed);
    assert(audream_blufi_send_custom_data_for_lease(102, data, sizeof(data)) == ESP_OK);
    grant = 103; assert(!audream_blufi_message_current(queued));
    Release(); assert(copied == freed);

    esp_blufi_ap_record_t access_point = {.ssid = "Private WLAN", .rssi = -30};
    assert(audream_blufi_send_wifi_list_for_lease(grant, 1, &access_point) == ESP_OK);
    args = (const btc_blufi_args_t*)queued->arg;
    access_point.ssid[0] = 'X';
    assert(args->wifi_list.list != &access_point && args->wifi_list.list[0].ssid[0] == 'P');
    grant = 104; assert(!audream_blufi_message_current(queued)); Release();
    assert(audream_blufi_send_wifi_conn_report_for_lease(grant, 1, 0, 2, NULL) == ESP_OK);
    args = (const btc_blufi_args_t*)queued->arg;
    assert(args->wifi_conn_report.opmode == 1 && args->wifi_conn_report.softap_conn_num == 2);
    grant = 105; assert(!audream_blufi_message_current(queued)); Release();
    assert(copied == freed);

    assert(esp_blufi_send_custom_data(data, sizeof(data)) == ESP_OK);
    btc_msg_t* dispatch_message = Take();
    audream_blufi_dispatch_message(dispatch_message, RevokeDuringDispatch, btc_blufi_call_deep_free);
    free(dispatch_message);
    assert(dispatched == 1 && copied == freed && audream_blufi_operation_lease() == grant);
    assert(esp_blufi_send_custom_data(data, sizeof(data)) == ESP_OK);
    dispatch_message = Take(); ++grant;
    audream_blufi_dispatch_message(dispatch_message, UnexpectedDispatch, btc_blufi_call_deep_free);
    free(dispatch_message);
    assert(copied == freed && audream_blufi_operation_lease() == grant);

    // Empty callbacks must still carry a grant; they cannot bypass the envelope.
    btc_msg_t callback = {.sig = BTC_SIG_API_CB, .pid = BTC_PID_BLUFI, .act = ESP_BLUFI_EVENT_RECV_CUSTOM_DATA};
    esp_blufi_cb_param_t param = {0};
    param.custom_data.data = data; param.custom_data.data_len = sizeof(data);
    assert(audream_blufi_transfer_context(&callback, &param, sizeof(param), CallbackCopy, CallbackFree) == BT_STATUS_SUCCESS);
    assert(audream_blufi_message_current(queued));
    const esp_blufi_cb_param_t* result = (const esp_blufi_cb_param_t*)queued->arg;
    assert(result->custom_data.data != data && result->custom_data.data[0] == 9);
    ++grant; assert(!audream_blufi_message_current(queued));
    Release(); assert(freed == copied);
    assert(audream_blufi_transfer_context(&callback, NULL, 0, NULL, NULL) == BT_STATUS_SUCCESS);
    assert(audream_blufi_message_current(queued));
    grant = 0; assert(!audream_blufi_message_current(queued)); Release();

    assert(esp_blufi_profile_init() == ESP_OK && audream_blufi_message_current(queued)); Release();
    assert(esp_blufi_profile_deinit() == ESP_OK && audream_blufi_message_current(queued)); Release();
    callback.act = ESP_BLUFI_EVENT_BLE_DISCONNECT;
    assert(audream_blufi_transfer_context(&callback, &param, sizeof(param), NULL, NULL) == BT_STATUS_SUCCESS);
    assert(audream_blufi_message_current(queued)); Release();

    btc_msg_t other = {.sig = BTC_SIG_API_CALL, .pid = BTC_PID_GATTS, .act = BTC_GATTS_ACT_CLOSE};
    uint32_t other_data = 12;
    assert(audream_blufi_transfer_context(&other, &other_data, sizeof(other_data), NULL, NULL) == BT_STATUS_SUCCESS);
    assert(queued_size == sizeof(other_data) && *(uint32_t*)queued->arg == 12);
    assert(!audream_blufi_message_current(queued)); Release();
    const unsigned before = queued_count;
    callback.act = ESP_BLUFI_EVENT_RECV_CUSTOM_DATA;
    assert(audream_blufi_transfer_context(&callback, NULL, 0, NULL, NULL) == BT_STATUS_FAIL);
    grant = 104;
    assert(audream_blufi_transfer_context(&callback, NULL, sizeof(param), NULL, NULL) == BT_STATUS_FAIL);
    assert(audream_blufi_transfer_context(&callback, &param, -1, NULL, NULL) == BT_STATUS_FAIL);
    assert(audream_blufi_transfer_context(&callback, &param, sizeof(audream_blufi_btc_envelope_t) + 1, NULL, NULL) == BT_STATUS_FAIL);
    assert(queued_count == before);
    fail_queue = true;
    assert(esp_blufi_send_custom_data(data, sizeof(data)) == ESP_FAIL && queued_count == before && copied == freed);
    fail_queue = false;
    fail_post = true;
    const unsigned before_copy_failure = copied;
    assert(audream_blufi_send_custom_data_for_lease(grant, data, sizeof(data)) == ESP_FAIL);
    assert(!queued && queued_count == before && copied == before_copy_failure + 1 && copied == freed);
    assert(!audream_blufi_message_current(NULL));
    puts("PASS: actual SDK API queues preserve grants and deep-copy layout; stale callbacks/responses reject and release; other profiles unchanged");
}
