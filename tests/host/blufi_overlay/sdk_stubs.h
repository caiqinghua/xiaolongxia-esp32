#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define TRUE 1
#define BLUFI_INCLUDED TRUE
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_BLE_HOST_STATUS_ENABLED 1
#define ESP_BLE_HOST_STATUS_CHECK(value) ((void)(value))
#define BTC_TRACE_ERROR(...) ((void)0)
#define BTC_TRACE_WARNING(...) ((void)0)
#define BLUFI_TRACE_DEBUG(...) ((void)0)
#define BLUFI_TRACE_ERROR(...) ((void)0)
#define BLUFI_TRACE_WARNING(...) ((void)0)
#define BLUFI_TRACE_API(...) ((void)0)
#define BTC_SIG_API_CB 1
#define BTC_SIG_API_CALL 2
#define BTC_PID_BLUFI 1
#define BTC_PID_GATTS 2
#define BTC_GATTS_ACT_CLOSE 1
#define BT_STATUS_SUCCESS 0
#define BT_STATUS_FAIL 1
#define BTC_GATT_GET_CONN_ID(id) ((id) & 0xff)
#define BTC_GATT_CREATE_CONN_ID(interface, id) (((interface) << 8) | (id))
#define BLUFI_APP_UUID 0xffff
#define BLUFI_CHAR_P2E_UUID 0xff01
#define BLUFI_CHAR_E2P_UUID 0xff02
#define BLUFI_HDL_NUM 6
#define BLUFI_PREPARE_BUF_MAX_SIZE 1024
#define BLUFI_MAX_DATA_LEN 512
#define BLUFI_MTU_RESERVED_SIZE 4
#define BLUFI_FC_CHECK 2
#define ESP_BLUFI_BD_ADDR_LEN 6
#define ESP_BLUFI_DEINIT_OK 0
#define ESP_BLUFI_INIT_OK 0
#define ESP_BLUFI_INIT_FAILED 1
#define BTA_GATT_TRANSPORT_LE 1
#define BTA_GATT_OK 0
#define GATT_SUCCESS 0
#define GATT_READ_NOT_PERMIT 2
#define GATT_WRITE_NOT_PERMIT 3
#define GATT_INSUF_AUTHENTICATION 5
#define GATT_INSUF_RESOURCE 0x11
#define GATT_REQ_NOT_SUPPORTED 6
#define GATT_INVALID_OFFSET 7
#define GATT_INVALID_ATTR_LEN 13
#define GATT_NO_RESOURCES 17
#define ESP_GATT_INVALID_OFFSET GATT_INVALID_OFFSET
#define ESP_GATT_INVALID_ATTR_LEN GATT_INVALID_ATTR_LEN
#define GATT_PERM_READ 1
#define GATT_PERM_WRITE 16
#define GATT_PERM_READ_ENC_MITM 4
#define GATT_PERM_WRITE_ENC_MITM 64
#define GATT_CHAR_PROP_BIT_WRITE 8
#define GATT_CHAR_PROP_BIT_READ 2
#define GATT_CHAR_PROP_BIT_NOTIFY 16
#define GATT_PREP_WRITE_EXEC 1
#define ADV_TYPE_IND 0
#define BLE_ADDR_TYPE_PUBLIC 0
#define ADV_CHNL_ALL 7
#define ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY 0
#define ESP_BT_STATUS_SUCCESS 0
#define pdMS_TO_TICKS(value) (value)

typedef int esp_err_t;
typedef int bt_status_t;
typedef uint8_t esp_gatt_if_t;
typedef uint8_t esp_blufi_bd_addr_t[6];
typedef int esp_gap_ble_cb_event_t;
enum { ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT, ESP_GAP_BLE_ADV_START_COMPLETE_EVT };
typedef struct { struct { int status; } adv_start_cmpl; } esp_ble_gap_cb_param_t;
typedef struct {
    bool set_scan_rsp, include_name, include_txpower;
    int min_interval, max_interval, appearance, manufacturer_len;
    uint8_t *p_manufacturer_data;
    int service_data_len;
    uint8_t *p_service_data;
    int service_uuid_len;
    uint8_t *p_service_uuid;
    int flag;
} esp_ble_adv_data_t;
typedef struct {
    int adv_int_min, adv_int_max, adv_type, own_addr_type, channel_map, adv_filter_policy;
} esp_ble_adv_params_t;
typedef struct { struct { uint16_t uuid16; } uu; } tBT_UUID;
typedef uint16_t tBTA_GATT_STATUS;
typedef int tBTA_GATTS_EVT;
enum {
    BTA_GATTS_REG_EVT, BTA_GATTS_DEREG_EVT, BTA_GATTS_READ_EVT, BTA_GATTS_WRITE_EVT,
    BTA_GATTS_EXEC_WRITE_EVT, BTA_GATTS_MTU_EVT, BTA_GATTS_CONF_EVT, BTA_GATTS_CREATE_EVT,
    BTA_GATTS_ADD_CHAR_EVT, BTA_GATTS_ADD_CHAR_DESCR_EVT, BTA_GATTS_CONNECT_EVT,
    BTA_GATTS_DISCONNECT_EVT, BTA_GATTS_OPEN_EVT, BTA_GATTS_CLOSE_EVT, BTA_GATTS_CONGEST_EVT,
    BTA_GATTS_START_EVT
};
typedef struct { uint16_t handle, offset; bool is_long, need_rsp; } tGATT_READ_REQ;
typedef struct {
    uint16_t handle, offset, len;
    uint8_t value[600];
    bool need_rsp, is_prep;
} tGATT_WRITE_REQ;
typedef union { tGATT_READ_REQ read_req; tGATT_WRITE_REQ write_req; int exec_write; uint16_t mtu; } tGATT_REQ_DATA;
typedef struct {
    struct { int status; tBT_UUID uuid; uint8_t server_if; } reg_oper;
    struct { uint16_t conn_id; uint32_t trans_id; int status; tGATT_REQ_DATA* p_data; } req_data;
    struct { uint8_t server_if; uint16_t service_id; int status; } create;
    struct { tBT_UUID char_uuid; uint16_t attr_id, service_id; uint8_t server_if; int status; } add_result;
    struct { uint8_t server_if; uint16_t service_id; int status; } srvc_oper;
    struct { uint8_t remote_bda[6], server_if; int reason; uint16_t conn_id; } conn;
} tBTA_GATTS;
typedef struct { struct { uint16_t handle, len, offset; uint8_t value[600]; } attr_value; } tBTA_GATTS_RSP;
typedef tBTA_GATTS_RSP tBTA_GATTS_API_RSP;
typedef tBTA_GATTS_RSP tGATTS_RSP;
#pragma pack(push, 4)
typedef struct { uint8_t sig, aid, pid, act; uint8_t arg[0]; } btc_msg_t;
typedef void (*btc_arg_deep_copy_t)(btc_msg_t*, void*, void*);
typedef void (*btc_arg_deep_free_t)(btc_msg_t*);
typedef struct { struct { uint16_t conn_id; } close; } btc_ble_gatts_args_t;
typedef struct {
    struct { int state; } deinit_finish, init_finish;
    struct { uint8_t remote_bda[6]; uint16_t conn_id; } connect;
    struct { uint8_t remote_bda[6]; } disconnect;
    struct { uint8_t* data; uint32_t data_len; } custom_data;
} esp_blufi_cb_param_t;
enum { ESP_BLUFI_EVENT_DEINIT_FINISH, ESP_BLUFI_EVENT_INIT_FINISH, ESP_BLUFI_EVENT_BLE_CONNECT, ESP_BLUFI_EVENT_BLE_DISCONNECT, ESP_BLUFI_EVENT_RECV_CUSTOM_DATA };
enum { BTC_BLUFI_ACT_INIT, BTC_BLUFI_ACT_DEINIT, BTC_BLUFI_ACT_SEND_CFG_REPORT, BTC_BLUFI_ACT_SEND_WIFI_LIST, BTC_BLUFI_ACT_SEND_ERR_INFO, BTC_BLUFI_ACT_SEND_CUSTOM_DATA };
typedef int wifi_mode_t;
typedef int esp_blufi_sta_conn_state_t;
typedef int esp_blufi_error_state_t;
typedef struct { unsigned reserved; } esp_blufi_extra_info_t;
typedef struct { uint8_t ssid[32]; int8_t rssi; } esp_blufi_ap_record_t;
typedef union {
    struct { wifi_mode_t opmode; esp_blufi_sta_conn_state_t sta_conn_state; uint8_t softap_conn_num; esp_blufi_extra_info_t* extra_info; int extra_info_len; } wifi_conn_report;
    struct { uint16_t apCount; esp_blufi_ap_record_t* list; } wifi_list;
    struct { esp_blufi_error_state_t state; } blufi_err_infor;
    struct { uint8_t* data; uint32_t data_len; } custom_data;
} btc_blufi_args_t;
typedef void (*esp_blufi_event_cb_t)(int, esp_blufi_cb_param_t*);
typedef struct { esp_blufi_event_cb_t event_cb; } esp_blufi_callbacks_t;
#pragma pack(pop)
typedef struct {
    bool enabled, is_connected;
    uint8_t srvc_inst, gatt_if, remote_bda[6], recv_seq, send_seq, sec_mode;
    uint16_t handle_srvc, handle_char_p2e, handle_char_e2p, handle_descr_e2p, conn_id;
    uint8_t *prepare_buf, *aggr_buf;
    unsigned prepare_len, offset, frag_size;
} tBLUFI_ENV;
extern tBLUFI_ENV blufi_env;
static const tBT_UUID blufi_app_uuid = {{BLUFI_APP_UUID}}, blufi_srvc_uuid = {{BLUFI_APP_UUID}},
    blufi_char_uuid_p2e = {{BLUFI_CHAR_P2E_UUID}}, blufi_char_uuid_e2p = {{BLUFI_CHAR_E2P_UUID}},
    blufi_descr_uuid_e2p = {{0x2902}};
struct pkt_info { uint8_t* pkt; int pkt_len; };
struct blufi_hdr { uint8_t type, fc, seq, data_len; };

void* osi_malloc(size_t size);
void osi_free(void* ptr);
void vTaskDelay(unsigned ticks);
bt_status_t btc_transfer_context(btc_msg_t*, void*, int, btc_arg_deep_copy_t, btc_arg_deep_free_t);
void btc_blufi_set_callbacks(esp_blufi_callbacks_t*);
int btc_profile_cb_set(int, esp_blufi_event_cb_t);
uint16_t btc_blufi_get_version(void);
void btc_blufi_call_deep_copy(btc_msg_t*, void*, void*);
void btc_blufi_call_deep_free(btc_msg_t*);
void btc_blufi_recv_handler(uint8_t*, int);
void btc_blufi_send_notify(uint8_t*, int);
void BTA_GATTS_CreateService(uint8_t, const tBT_UUID*, uint8_t, int, bool);
void BTA_GATTS_AppRegister(const tBT_UUID*, void (*)(tBTA_GATTS_EVT, tBTA_GATTS*));
void BTA_GATTS_SendRsp(uint16_t, uint32_t, tBTA_GATT_STATUS, const tBTA_GATTS_RSP*);
void BTA_GATTS_AddCharacteristic(uint16_t, const tBT_UUID*, uint16_t, uint8_t, void*, void*);
void BTA_GATTS_AddCharDescriptor(uint16_t, uint16_t, const tBT_UUID*, void*, void*);
void BTA_GATTS_StartService(uint16_t, int);
void BTA_GATTS_StopService(uint16_t);
void BTA_GATTS_DeleteService(uint16_t);
void BTA_GATTS_AppDeregister(uint8_t);
void BTA_GATTS_HandleValueIndication(uint16_t, uint16_t, int, uint8_t*, bool);
int esp_ble_gap_start_advertising(const esp_ble_adv_params_t*);
int esp_ble_gap_stop_advertising(void);
int esp_ble_gap_config_adv_data(const esp_ble_adv_data_t*);
int esp_ble_gap_set_device_name(const char*);
unsigned esp_ble_get_cur_sendable_packets_num(uint16_t);
