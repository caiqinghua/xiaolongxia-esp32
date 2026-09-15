#include "watch_blufi_scope.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRUE 1
#define CLASSIC_BT_INCLUDED 0
#define BTA_GATT_MAX_ATTR_LEN 517
#define APPL_TRACE_ERROR(...) ((void)0)
#define BTA_GATTS_API_INDICATION_EVT 13
#define BTA_GATTS_CONF_EVT 14
#define BTA_GATT_ILLEGAL_PARAMETER 1
#define GATT_SUCCESS 0
#define SIG_BTU_BTA_MSG 1
#define OSI_THREAD_MAX_TIMEOUT 0xffffffff
enum { L2CA_ADD_BTU_NUM, L2CA_DECREASE_BTU_NUM, L2CA_DECREASE_BTC_NUM };
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef bool BOOLEAN;
typedef uint8_t tGATT_IF;
typedef uint8_t BD_ADDR[6];
typedef int tBTA_TRANSPORT;
typedef int tBTA_GATT_STATUS;
#include "bta_types.inc"
#include "watch_blufi_bta.h"

typedef union { tBTA_GATTS_API_INDICATION api_indicate; } tBTA_GATTS_DATA;
typedef struct {
    struct { int status; uint16_t conn_id, handle; uint8_t* value; unsigned data_len; } req_data;
} tBTA_GATTS;
typedef struct { void (*p_cback)(int, tBTA_GATTS*); } tBTA_GATTS_RCB;
typedef struct { tBTA_GATTS_RCB rcb[1]; } tBTA_GATTS_CB;
typedef struct { unsigned rcb_idx; } tBTA_GATTS_SRVC_CB;

static uint64_t grant;
static uint64_t subscription = 1;
static uint16_t connection = 0x301, attribute = 102;
static bool subscribed, fail_allocation, fail_post, missing_connection, missing_service, revoke_lookup;
static unsigned allocations, releases, sent, reports;
static int btu_credit, btc_credit = 9;
static void* queued;
static uint8_t sent_value[BTA_GATT_MAX_ATTR_LEN];
static unsigned sent_length;
static tBTA_GATTS_CB control;
static tBTA_GATTS_SRVC_CB service;

uint64_t audream_blufi_transport_lease(void) { return grant; }
bool audream_blufi_is_notify_handle(uint16_t handle) { return handle == attribute; }
bool audream_blufi_notify_current(uint16_t conn, uint16_t handle, uint64_t lease) {
    return lease && lease == audream_blufi_operation_lease() && conn == connection && handle == attribute && subscribed;
}
uint64_t audream_blufi_notify_token(uint16_t conn, uint16_t handle, uint64_t lease) {
    return audream_blufi_notify_current(conn, handle, lease) ? subscription : 0;
}
static void* osi_malloc(size_t size) {
    if (fail_allocation) return NULL;
    void* result = malloc(size); assert(result); ++allocations;
    return result;
}
static void osi_free(void* pointer) { assert(pointer); ++releases; free(pointer); }
static void l2ble_update_att_acl_pkt_num(unsigned kind, void* parameter) {
    assert(!parameter);
    switch (kind) {
        case L2CA_ADD_BTU_NUM: ++btu_credit; break;
        case L2CA_DECREASE_BTU_NUM: --btu_credit; break;
        case L2CA_DECREASE_BTC_NUM: --btc_credit; break;
        default: assert(false);
    }
    assert(btu_credit >= 0 && btc_credit >= 0);
}
static bool btu_task_post(unsigned signal, void* message, unsigned timeout) {
    assert(signal == SIG_BTU_BTA_MSG && !queued);
    assert(timeout == (audream_blufi_bta_tagged(message) ? 0 : OSI_THREAD_MAX_TIMEOUT));
    if (fail_post) return false;
    queued = message;
    return true;
}
static void bta_sys_sendmsg(void* message) {
    assert(btu_task_post(SIG_BTU_BTA_MSG, message, OSI_THREAD_MAX_TIMEOUT));
}
static tBTA_GATTS_SRVC_CB* bta_gatts_find_srvc_cb_by_attr_id(tBTA_GATTS_CB* cb, unsigned handle) {
    (void)handle; assert(cb == &control);
    return missing_service ? NULL : &service;
}
static bool GATT_GetConnectionInfor(uint16_t conn, tGATT_IF* interface, BD_ADDR peer, tBTA_TRANSPORT* transport) {
    assert(conn == connection);
    *interface = 3; *transport = 0; memset(peer, 1, 6);
    if (revoke_lookup) ++grant;
    return !missing_connection;
}
static tBTA_GATTS_RCB* bta_gatts_find_app_rcb_by_app_if(tGATT_IF interface) {
    assert(interface == 3); return &control.rcb[0];
}
static int GATTS_HandleValueNotification(uint16_t conn, uint16_t handle, unsigned length, uint8_t* value) {
    (void)handle;
    assert(conn == connection && length <= sizeof(sent_value));
    memcpy(sent_value, value, length); sent_length = length; ++sent;
    return GATT_SUCCESS;
}
static int GATTS_HandleValueIndication(uint16_t conn, uint16_t handle, unsigned length, uint8_t* value) {
    return GATTS_HandleValueNotification(conn, handle, length, value);
}
static void Report(int event, tBTA_GATTS* data) {
    assert(event == BTA_GATTS_CONF_EVT && data->req_data.status == GATT_SUCCESS);
    assert(data->req_data.data_len <= sizeof(sent_value)); ++reports;
}
#include "bta_api.inc"
#include "bta_action.inc"

static void Drain(void) {
    assert(queued);
    void* message = queued; queued = NULL;
    bta_gatts_indicate_handle(&control, message);
    osi_free(message);
    assert(btu_credit == 0 && allocations == releases);
}
static void Queue(const uint8_t* data, int length) {
    assert(audream_blufi_queue_notification(grant, connection, attribute, length, data));
    assert(queued && btu_credit == 1);
}

int main(void) {
    control.rcb[0].p_cback = Report;
    uint8_t data[] = {1, 2, 3};
    assert(!audream_blufi_queue_notification(0, connection, attribute, sizeof(data), data));
    grant = 11; subscribed = true;
    Queue(data, sizeof(data)); data[0] = 9;
    assert(btc_credit == 9);
    Drain(); assert(sent == 1 && reports == 1 && sent_length == 3 && sent_value[0] == 1);

    Queue(data, sizeof(data)); ++grant; Drain();
    Queue(data, sizeof(data)); subscribed = false; Drain(); subscribed = true;
    Queue(data, sizeof(data)); ++subscription; Drain();
    Queue(data, sizeof(data)); ++connection; Drain(); --connection;
    Queue(data, sizeof(data)); ++attribute; Drain(); --attribute;
    assert(sent == 1 && reports == 1 && btc_credit == 9);

    Queue(data, sizeof(data)); missing_connection = true; Drain(); missing_connection = false;
    Queue(data, sizeof(data)); missing_service = true; Drain(); missing_service = false;
    Queue(data, sizeof(data)); revoke_lookup = true; Drain(); revoke_lookup = false;
    assert(sent == 1 && reports == 1);

    Queue(data, sizeof(data)); ((tBTA_GATTS_API_INDICATION*)queued)->hdr.len = sizeof(tBTA_GATTS_API_INDICATION); Drain();
    Queue(data, sizeof(data)); ((tBTA_GATTS_API_INDICATION*)queued)->need_confirm = true; Drain();
    Queue(data, sizeof(data)); ((tBTA_GATTS_API_INDICATION*)queued)->len = BTA_GATT_MAX_ATTR_LEN + 1; Drain();
    assert(sent == 1 && reports == 1);

    // An ordinary SDK producer cannot bypass BLUFI provenance. Foreign profiles are unchanged.
    BTA_GATTS_HandleValueIndication(connection, attribute, sizeof(data), data, false);
    Drain(); assert(sent == 1 && reports == 1 && btc_credit == 8);
    BTA_GATTS_HandleValueIndication(connection, 202, sizeof(data), data, false);
    Drain(); assert(sent == 2 && reports == 2 && btc_credit == 7);
    BTA_GATTS_HandleValueIndication(connection, 202, sizeof(data), data, true);
    Drain(); assert(sent == 3 && reports == 2 && btc_credit == 7);

    fail_allocation = true;
    assert(!audream_blufi_queue_notification(grant, connection, attribute, sizeof(data), data));
    fail_allocation = false; fail_post = true;
    assert(!audream_blufi_queue_notification(grant, connection, attribute, sizeof(data), data));
    fail_post = false;
    assert(!queued && btu_credit == 0 && allocations == releases && btc_credit == 7);
    assert(!audream_blufi_queue_notification(grant - 1, connection, attribute, sizeof(data), data));
    assert(!audream_blufi_queue_notification(grant, connection, attribute, 0, data));
    assert(!audream_blufi_queue_notification(grant, connection, attribute, -1, data));
    assert(!audream_blufi_queue_notification(grant, connection, attribute, BTA_GATT_MAX_ATTR_LEN + 1, data));
    assert(!audream_blufi_queue_notification(grant, connection, attribute, sizeof(data), NULL));
    audream_blufi_scope_t previous = audream_blufi_scope_enter(grant - 1);
    assert(!audream_blufi_queue_notification(grant, connection, attribute, sizeof(data), data));
    audream_blufi_scope_leave(previous);
    uint8_t maximum[BTA_GATT_MAX_ATTR_LEN]; memset(maximum, 0x5a, sizeof(maximum));
    Queue(maximum, sizeof(maximum)); Drain();
    assert(sent_length == sizeof(maximum) && memcmp(sent_value, maximum, sizeof(maximum)) == 0);
    assert(allocations == releases && btu_credit == 0 && btc_credit == 7);
    puts("PASS: reviewed BTA producer/consumer retain authorization, reject stale/unmarked BLUFI, preserve foreign profiles and balance queue credits");
}
