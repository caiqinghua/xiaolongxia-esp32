#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "watch_blufi_scope.h"

#define TRUE 1
#define FALSE 0
#define BLUFI_INCLUDED TRUE
#define GATT_DYNAMIC_MEMORY FALSE
#define BTC_TRACE_ERROR(...) ((void)0)
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint8_t BD_ADDR[6];
typedef bool BOOLEAN;
typedef struct btc_msg btc_msg_t;
typedef struct {
    int (*decrypt_func)(uint8_t, uint8_t*, int);
    int (*encrypt_func)(uint8_t, uint8_t*, int);
    uint16_t (*checksum_func)(uint8_t, uint8_t*, int);
} esp_blufi_callbacks_t;
typedef enum {
    ESP_BLUFI_DATA_FORMAT_ERROR,
    ESP_BLUFI_SEQUENCE_ERROR,
    ESP_BLUFI_DECRYPT_ERROR,
    ESP_BLUFI_CHECKSUM_ERROR,
    ESP_BLUFI_MSG_STATE_ERROR,
    ESP_BLUFI_DH_MALLOC_ERROR,
    ESP_BLUFI_ENCRYPT_ERROR
} esp_blufi_error_state_t;
#include AUDREAM_BLUFI_INTERNAL_HEADER

tBLUFI_ENV blufi_env;
static uint64_t grant;
static unsigned allocations, releases, messages, errors, acknowledgements;
static bool allocation_failure, revoke_on_decrypt, revoke_on_ack;
static bool decryption_failure;
static bool revoke_on_send, revoke_on_encrypt;
static unsigned sent_frames;
static uint8_t received[128];
static int received_length;
static uint8_t received_type;

uint64_t audream_blufi_transport_lease(void) { return grant; }
static void* osi_malloc(size_t size) {
    if (allocation_failure) return NULL;
    void* result = malloc(size);
    assert(result); ++allocations;
    return result;
}
static void osi_free(void* pointer) {
    assert(pointer); ++releases; free(pointer);
}
static void btc_blufi_report_error(esp_blufi_error_state_t state) {
    (void)state; ++errors;
}
static void btc_blufi_send_ack(uint8_t sequence) {
    (void)sequence; ++acknowledgements;
    if (revoke_on_ack) ++grant;
}
void btc_blufi_protocol_handler(uint8_t type, uint8_t* data, int length) {
    assert(length >= 0 && (size_t)length <= sizeof(received));
    ++messages; received_type = type; received_length = length;
    memcpy(received, data, (size_t)length);
}
#include "blufi_receive.inc"

static void esp_blufi_send_encap(void* packet) {
    assert(packet);
    if (!audream_blufi_operation_lease()) return;
    ++sent_frames;
    if (revoke_on_send) ++grant;
}
#include "blufi_send.inc"

static void Start(void) {
    ++grant;
    blufi_env.cbs = NULL;
    allocation_failure = revoke_on_decrypt = revoke_on_ack = false;
    decryption_failure = false;
    messages = errors = acknowledgements = 0;
}
static void Packet(uint8_t type, uint8_t flags, uint8_t sequence, const uint8_t* data, uint8_t length) {
    uint8_t packet[4 + 255] = {type, flags, sequence, length};
    if (length) memcpy(packet + 4, data, length);
    btc_blufi_recv_handler(packet, 4 + length);
}
static void Begin(void) {
    const uint8_t data[] = {6, 0, 'a', 'b'};
    Packet(0x4d, BLUFI_FC_FRAG, 0, data, sizeof(data));
}
static int Decrypt(uint8_t sequence, uint8_t* data, int length) {
    (void)sequence; (void)data;
    if (revoke_on_decrypt) ++grant;
    return decryption_failure ? -1 : length;
}
static uint16_t Checksum(uint8_t sequence, uint8_t* data, int length) {
    (void)sequence; (void)data; (void)length;
    return 0x1234;
}
static int Encrypt(uint8_t sequence, uint8_t* data, int length) {
    (void)sequence; (void)data;
    if (revoke_on_encrypt) ++grant;
    return length;
}

int main(void) {
    const uint8_t middle[] = {4, 0, 'c', 'd'};
    const uint8_t tail[] = {'e', 'f'};
    Begin(); assert(messages == 0 && allocations == 0);
    Start(); blufi_env.send_seq = 7; Begin();
    assert(blufi_env.offset == 2 && errors == 0 && blufi_env.send_seq == 7);
    Packet(0x4d, BLUFI_FC_FRAG, 1, middle, sizeof(middle));
    Packet(0x4d, 0, 2, tail, sizeof(tail));
    assert(messages == 1 && received_type == 0x4d && received_length == 6);
    assert(memcmp(received, "abcdef", 6) == 0 && allocations == releases && !blufi_env.aggr_buf);

    Start(); Begin(); ++grant;
    Packet(0x4d, 0, 1, tail, sizeof(tail));
    assert(messages == 0 && errors == 1 && !blufi_env.aggr_buf && allocations == releases);
    Packet(0x4d, 0, 0, tail, sizeof(tail));
    assert(messages == 1 && received_length == 2);

    Start(); Begin(); grant = 0;
    Packet(0x4d, 0, 1, tail, sizeof(tail));
    assert(messages == 0 && !blufi_env.aggr_buf && allocations == releases);

    for (unsigned length = 0; length <= 2; ++length) {
        Start(); Begin();
        Packet(0x4d, BLUFI_FC_FRAG, 1, middle, (uint8_t)length);
        assert(errors == 1 && messages == 0 && !blufi_env.aggr_buf && allocations == releases);
    }
    Start(); Begin();
    Packet(0x49, BLUFI_FC_FRAG, 1, middle, sizeof(middle));
    assert(errors == 1 && !blufi_env.aggr_buf);
    Start(); Begin();
    Packet(0x49, 0, 1, middle, sizeof(middle));
    assert(errors == 1 && !blufi_env.aggr_buf);
    Start(); Begin();
    const uint8_t wrong_remaining[] = {5, 0, 'c', 'd'};
    Packet(0x4d, BLUFI_FC_FRAG, 1, wrong_remaining, sizeof(wrong_remaining));
    assert(errors == 1 && !blufi_env.aggr_buf);
    Start(); Begin();
    Packet(0x4d, 0, 1, tail, sizeof(tail));
    assert(errors == 1 && !blufi_env.aggr_buf);

    Start(); allocation_failure = true; Begin();
    assert(errors == 1 && !blufi_env.aggr_buf && allocations == releases);
    allocation_failure = false;
    Packet(0x4d, 0, 1, tail, sizeof(tail));
    assert(messages == 1);

    Start(); Packet(0x4d, BLUFI_FC_ENC, 0, tail, sizeof(tail));
    assert(errors == 1 && messages == 0);
    uint8_t checked[] = {0x4d, BLUFI_FC_CHECK, 0, 2, 'e', 'f', 0x34, 0x12};
    Start(); btc_blufi_recv_handler(checked, sizeof(checked));
    assert(errors == 1 && messages == 0);
    esp_blufi_callbacks_t callbacks = {.decrypt_func = Decrypt, .checksum_func = Checksum};
    Start(); blufi_env.cbs = &callbacks;
    btc_blufi_recv_handler(checked, sizeof(checked));
    assert(errors == 0 && messages == 1 && received_length == 2);
    Start(); Begin(); blufi_env.cbs = &callbacks;
    checked[2] = 1; checked[7] = 0;
    btc_blufi_recv_handler(checked, sizeof(checked));
    assert(errors == 1 && messages == 0 && !blufi_env.aggr_buf);
    Start(); Begin(); blufi_env.cbs = &callbacks; decryption_failure = true;
    Packet(0x4d, BLUFI_FC_FRAG | BLUFI_FC_ENC, 1, middle, sizeof(middle));
    assert(errors == 1 && messages == 0 && !blufi_env.aggr_buf);
    Start(); Begin();
    uint8_t truncated[] = {0x4d, BLUFI_FC_FRAG, 1, 4, 0};
    btc_blufi_recv_handler(truncated, sizeof(truncated));
    assert(errors == 1 && !blufi_env.aggr_buf);
    Start(); btc_blufi_recv_handler(NULL, 4);
    assert(errors == 1);
    Start(); btc_blufi_recv_handler(truncated, -1);
    assert(errors == 1);

    Start(); Begin();
    blufi_env.cbs = &callbacks;
    revoke_on_decrypt = true;
    Packet(0x4d, BLUFI_FC_FRAG | BLUFI_FC_ENC, 1, middle, sizeof(middle));
    assert(messages == 0 && !blufi_env.aggr_buf);
    Start(); Begin(); revoke_on_ack = true;
    Packet(0x4d, BLUFI_FC_REQ_ACK, 1, middle, sizeof(middle));
    assert(acknowledgements == 1 && messages == 0 && !blufi_env.aggr_buf);
    assert(allocations == releases);
    Start();
    blufi_env.is_connected = true;
    blufi_env.sec_mode = 0;
    blufi_env.frag_size = 2;
    uint8_t outgoing[] = {'a', 'b', 'c', 'd', 'e', 'f'};
    btc_blufi_send_encap(0x4d, outgoing, sizeof(outgoing));
    assert(sent_frames == 3 && allocations == releases);
    sent_frames = 0; revoke_on_send = true;
    btc_blufi_send_encap(0x4d, outgoing, sizeof(outgoing));
    assert(sent_frames == 1 && allocations == releases && audream_blufi_operation_lease() == grant);
    sent_frames = 0; revoke_on_send = false; revoke_on_encrypt = true;
    callbacks.encrypt_func = Encrypt; blufi_env.cbs = &callbacks;
    blufi_env.sec_mode = BLUFI_DATA_SEC_MODE_ENC_MASK;
    btc_blufi_send_encap(0x4d, outgoing, sizeof(outgoing));
    assert(sent_frames == 0 && allocations == releases && audream_blufi_operation_lease() == grant);
    revoke_on_encrypt = false;
    audream_blufi_scope_t previous = audream_blufi_scope_enter(grant - 1);
    btc_blufi_send_encap(0x4d, outgoing, sizeof(outgoing));
    assert(sent_frames == 0 && audream_blufi_operation_lease() == 0);
    audream_blufi_scope_leave(previous);
    blufi_env.frag_size = 0;
    btc_blufi_send_encap(0x4d, outgoing, sizeof(outgoing));
    assert(sent_frames == 0 && allocations == releases);
    puts("PASS: reviewed SDK receiver isolates fragment grants/types, rejects malformed lengths, releases failures and checks revocation");
    puts("PASS: reviewed SDK send loop retains its original grant and stops after send/crypto revocation");
}
