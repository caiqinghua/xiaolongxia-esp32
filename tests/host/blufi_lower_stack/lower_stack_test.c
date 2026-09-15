// Compile actual SDK functions; only external I/O and reduced SDK data types
// are fixtures. This does not validate target ABI or controller queue behavior.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRUE 1
#define FALSE 0
#define BLE_INCLUDED TRUE
#define CLASSIC_BT_INCLUDED FALSE
#define L2CAP_NUM_FIXED_CHNLS 3
#define L2CAP_FIRST_FIXED_CHNL 4
#define L2CAP_BASE_APPL_CID 64
#define L2CA_BUFF_FREE 1
#define L2CAP_FCR_BASIC_MODE 0
#define L2CAP_TRACE_DEBUG(...) ((void)0)
#define L2CAP_TRACE_ERROR(...) ((void)0)
#define HCI_ERR_DIRECTED_ADVERTISING_TIMEOUT 0x3c
#define HCI_ERR_HOST_REJECT_SECURITY 0x0e
#define HCI_ERR_CONN_FAILED_ESTABLISHMENT 0x3e
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef bool BOOLEAN;
typedef uint8_t BD_ADDR[6];
typedef enum { BT_TRANSPORT_BR_EDR, BT_TRANSPORT_LE } tBT_TRANSPORT;
typedef enum { LST_CONNECTED, LST_DISCONNECTING, LST_CONNECTING,
               LST_DISCONNECTED, LST_CONNECT_HOLDING } tL2C_LINK_STATE;
typedef struct { size_t count; void* data[8]; } Queue;
typedef struct { uint8_t payload[16]; } BT_HDR;
typedef struct { int unused; } tL2C_RCB;
typedef struct tL2C_LCB tL2C_LCB;
typedef struct tL2C_CCB {
    tL2C_LCB* p_lcb;
    tL2C_RCB* p_rcb;
    bool in_use, should_free_rcb, cong_sent;
    UINT16 local_cid;
    int timer_entry;
    Queue* xmit_hold_q;
    struct { struct { int mode; } fcr; } peer_cfg;
} tL2C_CCB;
struct tL2C_LCB {
    UINT16 handle;
    bool in_use;
    BD_ADDR remote_bd_addr;
    tBT_TRANSPORT transport;
    tL2C_LINK_STATE link_state;
    UINT8 disc_reason, peer_chnl_mask[1];
    tL2C_CCB* p_fixed_ccbs[L2CAP_NUM_FIXED_CHNLS];
    tL2C_CCB* p_pending_ccb;
    struct { tL2C_CCB* p_first_ccb; } ccb_queue;
    Queue* link_xmit_data_q;
};
static struct {
    bool is_ble_connecting;
    struct {
        void (*pL2CA_FixedConn_Cb)(int, uint8_t*, bool, UINT8, tBT_TRANSPORT);
        void (*pL2CA_FixedTxComplete_Cb)(UINT16, int);
    }
        fixed_reg[L2CAP_NUM_FIXED_CHNLS];
} l2cb;
static struct { UINT8 acl_disc_reason; } btm_cb;
static tL2C_LCB link;
static tL2C_CCB channels[L2CAP_NUM_FIXED_CHNLS];
static int allocated, freed, callbacks, creates, releases, timer_frees;
static bool create_success;
static UINT16 header_handle;

static void osi_free(void* pointer) { if(pointer) { ++freed; free(pointer); } }
static void (*osi_free_func)(void*) = osi_free;
static Queue* make_queue(size_t count) {
    Queue* q = calloc(1, sizeof(*q)); assert(q); ++allocated;
    q->count = count;
    for (size_t i = 0; i < count; ++i) {
        q->data[i] = calloc(1, sizeof(BT_HDR)); assert(q->data[i]); ++allocated;
    }
    return q;
}
static void fixed_queue_free(Queue* q, void (*destroy)(void*)) {
    if (!q) return;
    if (destroy) for(size_t i = 0; i < q->count; ++i) destroy(q->data[i]);
    osi_free(q);
}
static bool list_is_empty(Queue* q) { return q->count == 0; }
static void* list_front(Queue* q) { assert(q->count); return q->data[0]; }
static void list_remove(Queue* q, void* p) {
    assert(q->count && q->data[0] == p);
    --q->count; memmove(q->data, q->data + 1, q->count * sizeof(void*));
}
static void* fixed_queue_dequeue(Queue* q, int timeout) {
    assert(timeout == 0);
    if (!q->count) return NULL;
    void* p = q->data[0]; list_remove(q, p); return p;
}
static bool fixed_queue_is_empty(Queue* q) { return q->count == 0; }
static void l2ble_update_att_acl_pkt_num(int kind, void* arg) { assert(kind == L2CA_BUFF_FREE && !arg); }
static void btm_sec_clr_temp_auth_service(uint8_t* address) { assert(address == link.remote_bd_addr); }
static void btu_free_timer(int* timer) { (void)timer; ++timer_frees; }
static void l2cu_dequeue_ccb(tL2C_CCB* ccb) { (void)ccb; assert(false); }
static void l2cu_no_dynamic_ccbs(tL2C_LCB* lcb) { assert(lcb == &link); }
static void l2c_link_adjust_chnl_allocation(void) {}
static void BTM_Recovery_Pre_State(void) {}
static void btm_acl_removed(uint8_t* address, tBT_TRANSPORT transport) {
    assert(address == link.remote_bd_addr && transport == link.transport);
}
static tL2C_LCB* l2cu_find_lcb_by_handle(UINT16 handle) { return handle == link.handle && link.in_use ? &link : NULL; }
static tL2C_LCB* l2cu_find_lcb_by_state(tL2C_LINK_STATE state) { assert(state == LST_CONNECT_HOLDING); return NULL; }
static bool l2cu_create_conn(tL2C_LCB* lcb, tBT_TRANSPORT transport) {
    assert(lcb == &link && transport == link.transport); ++creates;
#if AUDREAM_PATCHED
    if (transport == BT_TRANSPORT_LE) {
        assert(!lcb->link_xmit_data_q || lcb->link_xmit_data_q->count == 0);
        for (int i = 0; i < L2CAP_NUM_FIXED_CHNLS; ++i) {
            const tL2C_CCB* ccb = lcb->p_fixed_ccbs[i];
            assert(!ccb || !ccb->xmit_hold_q || ccb->xmit_hold_q->count == 0);
        }
    }
#endif
    if (create_success) lcb->link_state = LST_CONNECTING;
    return create_success;
}
#include "release_ccb.inc"
#include "fixed_disconnect.inc"
static void l2cu_check_channel_congestion(tL2C_CCB* ccb) { assert(ccb->p_lcb == &link); }
static void l2cu_set_acl_hci_header(BT_HDR* packet, tL2C_CCB* ccb) {
    assert(packet && ccb->p_lcb == &link); header_handle = ccb->p_lcb->handle;
}
#include "next_buffer.inc"
static void l2cu_release_lcb(tL2C_LCB* lcb) {
    // The full LCB function is outside this harness; only the called fixed
    // channel teardown is real. Do not infer HCI/controller cleanup from here.
    assert(lcb == &link); ++releases;
    lcb->in_use = false;
    l2cu_process_fixed_disc_cback(lcb);
    fixed_queue_free(lcb->link_xmit_data_q, osi_free_func);
    lcb->link_xmit_data_q = NULL;
}
#if AUDREAM_PATCHED
#include "disconnect_patched.inc"
#else
#include "disconnect.inc"
#endif

static void disconnected(int cid, uint8_t* address, bool connected, UINT8 reason, tBT_TRANSPORT transport) {
    assert(!connected && address == link.remote_bd_addr && reason == link.disc_reason && transport == link.transport);
    const int index = cid - L2CAP_FIRST_FIXED_CHNL;
    if (transport == BT_TRANSPORT_BR_EDR && link.in_use) {
        assert(link.p_fixed_ccbs[index] == &channels[index] && channels[index].in_use);
    } else {
        assert(!link.p_fixed_ccbs[index] && !channels[index].in_use && !channels[index].xmit_hold_q);
    }
    ++callbacks;
}
static void initialize(void) {
    assert(allocated == freed);
    memset(&link, 0, sizeof(link)); memset(channels, 0, sizeof(channels));
    memset(&l2cb, 0, sizeof(l2cb)); memset(&btm_cb, 0, sizeof(btm_cb));
    callbacks = creates = releases = timer_frees = 0; create_success = true;
    header_handle = 0;
    link.handle = 7; link.in_use = true; link.transport = BT_TRANSPORT_LE;
    link.link_state = LST_CONNECTED; link.peer_chnl_mask[0] = 0x70;
    link.link_xmit_data_q = make_queue(2);
    for (int i = 0; i < L2CAP_NUM_FIXED_CHNLS; ++i) {
        channels[i].in_use = true; channels[i].p_lcb = &link;
        channels[i].cong_sent = true;
        channels[i].local_cid = i + L2CAP_FIRST_FIXED_CHNL;
        channels[i].xmit_hold_q = make_queue(i + 1);
        link.p_fixed_ccbs[i] = &channels[i];
        l2cb.fixed_reg[i].pL2CA_FixedConn_Cb = disconnected;
    }
}
static void cleanup(void) {
    link.p_pending_ccb = NULL;
    l2cu_release_lcb(&link);
    assert(allocated == freed);
}
int main(void) {
    initialize();
    assert(l2c_link_hci_disc_comp(7, 0x13));
    assert(releases == 1 && callbacks == 3 && timer_frees == 3 && creates == 0);
    assert(allocated == freed);
    assert(!l2c_link_hci_disc_comp(7, 0x13));

    initialize();
    link.p_pending_ccb = &channels[0];
    l2cu_process_fixed_disc_cback(&link);
    assert(callbacks == 2 && channels[0].in_use && channels[0].xmit_hold_q->count == 1);
    cleanup();

    initialize();
    link.p_pending_ccb = &channels[0];
    assert(l2c_link_hci_disc_comp(7, 0x13));
    // Baseline reproducer: a successful pending LE reconnect retains CCB data.
    assert(creates == 1 && releases == 0 && callbacks == 0 && link.in_use);
    assert(link.link_state == LST_CONNECTING && !link.p_pending_ccb);
    assert(link.link_xmit_data_q->count == 0);
#if AUDREAM_PATCHED
    for (int i = 0; i < 3; ++i) {
        assert(channels[i].xmit_hold_q->count == 0 && !channels[i].cong_sent);
    }
    puts("PASS: patched pending LE reconnect discards fixed/link transmit data before reuse");
#else
    for (int i = 0; i < 3; ++i) assert(channels[i].xmit_hold_q->count == (size_t)i + 1);
    puts("REPRODUCED: pending LE reconnect retains fixed-channel transmit buffers in reviewed SDK");
#endif
    // The real scheduler picks retained data and asks for a header on the new
    // link. The header/HCI send are fixtures; this is not a radio transmission.
    link.handle = 42; link.link_state = LST_CONNECTED;
    BT_HDR* next = l2cu_get_next_buffer_to_send(&link);
#if AUDREAM_PATCHED
    assert(!next && header_handle == 0);
#else
    assert(next && header_handle == 42);
    osi_free(next);
#endif
    cleanup();

    initialize();
    link.p_pending_ccb = &channels[0]; create_success = false;
    assert(l2c_link_hci_disc_comp(7, 0x13));
    assert(creates == 1 && releases == 1 && callbacks == 3 && allocated == freed);

    initialize();
    link.transport = BT_TRANSPORT_BR_EDR; link.p_pending_ccb = &channels[0];
    assert(l2c_link_hci_disc_comp(7, 0x13));
    assert(creates == 1 && releases == 0 && callbacks == 2);
    assert(channels[0].xmit_hold_q->count == 1 && channels[0].cong_sent);
    assert(link.link_xmit_data_q->count == 2);
    link.in_use = false;
    cleanup();

    initialize();
    fixed_queue_free(channels[1].xmit_hold_q, osi_free_func);
    channels[1].xmit_hold_q = NULL;
    fixed_queue_free(link.link_xmit_data_q, osi_free_func);
    link.link_xmit_data_q = NULL;
    assert(l2c_link_hci_disc_comp(7, 0x13) && allocated == freed);
    puts("PASS: normal/failed-reconnect cleanup, classic isolation, absent queues and balanced frees");
}
