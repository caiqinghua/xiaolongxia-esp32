#include "sdk_stubs.h"
#include <stdatomic.h>
#include AUDREAM_SDK_SOURCE
#undef btc_transfer_context

tBLUFI_ENV blufi_env;
static _Atomic uint64_t grant;
static unsigned lookup_transition;
static unsigned responses, received, indications, allocations, sleeps;
static unsigned sendable = 1;
static unsigned init_events, service_starts, service_creates, attribute_creates;
static int init_state;
static bool fail_init_queue;
static uint16_t status, permissions;
static tBTA_GATTS_RSP last_response;
static const uint8_t address[6] = {1, 2, 3, 4, 5, 6};

uint64_t audream_blufi_control_lease(uint16_t connection, const uint8_t* peer) {
    if (lookup_transition) {
        const unsigned transition = lookup_transition;
        lookup_transition = 0;
        if (transition == 1) audream_host_connection(false, 0x301, address, 0);
        else audream_host_attribute(102);
    }
    return connection == 1 && peer && memcmp(peer, address, 6) == 0 ? grant : 0;
}
void* osi_malloc(size_t size) { ++allocations; return malloc(size); }
void osi_free(void* ptr) { free(ptr); }
void vTaskDelay(unsigned ticks) { assert(ticks == 10); ++sleeps; grant = 0; }
bt_status_t btc_transfer_context(btc_msg_t* msg, void* arg, int len, btc_arg_deep_copy_t copy, btc_arg_deep_free_t cleanup) {
    if (msg->pid == BTC_PID_BLUFI && msg->sig == BTC_SIG_API_CB && msg->act == ESP_BLUFI_EVENT_INIT_FINISH) {
        assert(len == sizeof(audream_blufi_btc_envelope_t));
        const audream_blufi_btc_envelope_t* envelope = arg;
        assert(envelope->control_lease == 0);
        if (fail_init_queue) return BT_STATUS_FAIL;
        ++init_events; init_state = envelope->payload.callback.init_finish.state;
    }
    return BT_STATUS_SUCCESS;
}
void btc_blufi_recv_handler(uint8_t* data, int len) { ++received; }
void btc_blufi_send_notify(uint8_t* data, int len) {
    struct pkt_info packet = {data, len}; esp_blufi_send_notify(&packet);
}
void BTA_GATTS_CreateService(uint8_t a, const tBT_UUID* b, uint8_t c, int d, bool e) { ++service_creates; }
void BTA_GATTS_AppRegister(const tBT_UUID* a, void (*b)(tBTA_GATTS_EVT, tBTA_GATTS*)) {}
void BTA_GATTS_SendRsp(uint16_t connection, uint32_t transaction, tBTA_GATT_STATUS result, const tBTA_GATTS_RSP* response) {
    ++responses; status = result; last_response = response ? *response : (tBTA_GATTS_RSP){0};
}
void BTA_GATTS_AddCharacteristic(uint16_t a, const tBT_UUID* b, uint16_t flags, uint8_t d, void* e, void* f) { ++attribute_creates; permissions = flags; }
void BTA_GATTS_AddCharDescriptor(uint16_t a, uint16_t flags, const tBT_UUID* c, void* d, void* e) { ++attribute_creates; permissions = flags; }
void BTA_GATTS_StartService(uint16_t a, int b) { assert(a == 100); ++service_starts; }
void BTA_GATTS_StopService(uint16_t a) {}
void BTA_GATTS_DeleteService(uint16_t a) {}
void BTA_GATTS_AppDeregister(uint8_t a) {}
bool audream_blufi_queue_notification(uint64_t lease, uint16_t connection, uint16_t attribute, int len, const uint8_t* data) {
    assert(connection == 0x301 && attribute == blufi_env.handle_char_e2p);
    assert(audream_blufi_notify_current(connection, attribute, lease));
    ++indications;
    return true;
}
int esp_ble_gap_start_advertising(const esp_ble_adv_params_t* a) { return 0; }
int esp_ble_gap_stop_advertising(void) { return 0; }
int esp_ble_gap_config_adv_data(const esp_ble_adv_data_t* a) { return 0; }
int esp_ble_gap_set_device_name(const char* a) { return 0; }
unsigned esp_ble_get_cur_sendable_packets_num(uint16_t connection) { assert(connection == 1); return sendable; }

static void Connect(void) {
    tBTA_GATTS event = {0};
    event.conn.conn_id = 0x301; event.conn.server_if = 3;
    memcpy(event.conn.remote_bda, address, 6);
    blufi_profile_cb(BTA_GATTS_CONNECT_EVT, &event);
}
static void Write(uint16_t connection, uint16_t handle, unsigned len, unsigned value, bool prepared, unsigned offset, bool reply) {
    tGATT_REQ_DATA payload = {0};
    payload.write_req.handle = handle; payload.write_req.len = len;
    payload.write_req.value[0] = value; payload.write_req.need_rsp = reply;
    payload.write_req.is_prep = prepared; payload.write_req.offset = offset;
    tBTA_GATTS event = {0};
    event.req_data.conn_id = connection; event.req_data.p_data = &payload;
    blufi_profile_cb(BTA_GATTS_WRITE_EVT, &event);
}
static void Read(uint16_t connection, uint16_t handle, unsigned offset) {
    tGATT_REQ_DATA payload = {0};
    payload.read_req.handle = handle; payload.read_req.offset = offset;
    tBTA_GATTS event = {0};
    event.req_data.conn_id = connection; event.req_data.p_data = &payload;
    blufi_profile_cb(BTA_GATTS_READ_EVT, &event);
}

static void PublishPattern(unsigned pattern) {
    uint8_t peer[6];
    memset(peer, pattern, sizeof(peer));
    audream_host_connection(pattern == 1, (uint16_t)(0x300 + pattern), peer,
                            UINT64_C(0x1234567800000000) + pattern);
}

static void* SnapshotWriter(void* unused) {
    for (unsigned index = 0; index < 20000; ++index) PublishPattern(1 + index % 2);
    return NULL;
}

static void* SnapshotReader(void* unused) {
    uint64_t previous_epoch = 0;
    for (unsigned index = 0; index < 20000; ++index) {
        const audream_host_state_t state = audream_host_snapshot();
        const unsigned pattern = state.connection - 0x300;
        assert(pattern == 1 || pattern == 2);
        assert(state.connected == (pattern == 1));
        assert(state.previous_lease == UINT64_C(0x1234567800000000) + pattern);
        for (unsigned byte = 0; byte < sizeof(state.address); ++byte) assert(state.address[byte] == pattern);
        assert(state.epoch >= previous_epoch && state.notify_lease == 0);
        previous_epoch = state.epoch;
        assert(audream_blufi_transport_lease() == 0);
    }
    return NULL;
}

static void CheckSnapshotTransitions(void) {
    assert(audream_blufi_transport_lease() == grant);
    lookup_transition = 1;
    assert(audream_blufi_transport_lease() == 0);
    Connect(); ++grant;
    Write(0x301, 103, 2, 1, false, 0, true);
    assert(audream_blufi_notify_token(0x301, 102, grant));
    lookup_transition = 2;
    assert(audream_blufi_notify_token(0x301, 102, grant) == 0);
    lookup_transition = 1;
    assert(audream_host_subscribe(0x301, grant, true) == GATT_INSUF_AUTHENTICATION);
    PublishPattern(1);
    pthread_t writer, reader;
    assert(pthread_create(&writer, NULL, SnapshotWriter, NULL) == 0);
    assert(pthread_create(&reader, NULL, SnapshotReader, NULL) == 0);
    assert(pthread_join(writer, NULL) == 0);
    assert(pthread_join(reader, NULL) == 0);
    Connect(); ++grant;
    Write(0x301, 103, 2, 1, false, 0, true);
    assert(status == GATT_SUCCESS);
    puts("PASS: host snapshots remain coherent across concurrent publication and reject transitions during native lookup");
}
static tBTA_GATTS StartupData(unsigned step, bool success) {
    tBTA_GATTS event = {0};
    event.reg_oper.server_if = 3; event.reg_oper.uuid.uu.uuid16 = BLUFI_APP_UUID;
    event.reg_oper.status = success ? BTA_GATT_OK : 1;
    event.create.server_if = 3; event.create.service_id = 100;
    event.create.status = success ? BTA_GATT_OK : 1;
    event.add_result.server_if = 3; event.add_result.service_id = 100;
    event.add_result.attr_id = 99 + step;
    event.add_result.status = success ? BTA_GATT_OK : 1;
    event.add_result.char_uuid.uu.uuid16 = step == 2 ? BLUFI_CHAR_P2E_UUID : step == 3 ? BLUFI_CHAR_E2P_UUID : 0x2902;
    event.srvc_oper.server_if = 3; event.srvc_oper.service_id = 100;
    event.srvc_oper.status = success ? BTA_GATT_OK : 1;
    return event;
}
static void StartupStep(unsigned step, tBTA_GATTS* event) {
    const tBTA_GATTS_EVT types[] = {BTA_GATTS_REG_EVT, BTA_GATTS_CREATE_EVT, BTA_GATTS_ADD_CHAR_EVT,
        BTA_GATTS_ADD_CHAR_EVT, BTA_GATTS_ADD_CHAR_DESCR_EVT, BTA_GATTS_START_EVT};
    assert(step < sizeof(types) / sizeof(types[0]));
    blufi_profile_cb(types[step], event);
}
static void SucceedStep(unsigned step) {
    tBTA_GATTS event = StartupData(step, true); StartupStep(step, &event);
}
static void BeginStartup(void) {
    esp_blufi_deinit();
    memset(&blufi_env, 0, sizeof(blufi_env));
    init_events = service_starts = service_creates = attribute_creates = 0;
    init_state = -1; fail_init_queue = false;
    assert(esp_blufi_init() == GATT_SUCCESS);
}
static void TestStartup(void) {
    for (unsigned failed = 0; failed < 6; ++failed) {
        BeginStartup();
        for (unsigned step = 0; step < failed; ++step) { SucceedStep(step); assert(!init_events); }
        tBTA_GATTS event = StartupData(failed, false); StartupStep(failed, &event);
        assert(init_events == 1 && init_state == ESP_BLUFI_INIT_FAILED);
        const unsigned creates = service_creates + attribute_creates;
        for (unsigned step = 0; step < 6; ++step) SucceedStep(step);
        assert(init_events == 1 && service_creates + attribute_creates == creates);
        assert(service_starts == (failed == 5 ? 1U : 0U));
        assert(audream_host_snapshot().startup == AUDREAM_START_FAILED);
    }
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        BeginStartup();
        for (unsigned step = 0; step < invalid; ++step) SucceedStep(step);
        tBTA_GATTS event = StartupData(invalid, true);
        if (invalid == 0) event.reg_oper.server_if = 0;
        else if (invalid == 1) event.create.service_id = 0;
        else event.add_result.attr_id = 0;
        StartupStep(invalid, &event);
        assert(init_events == 1 && init_state == ESP_BLUFI_INIT_FAILED);
    }
    BeginStartup();
    for (unsigned step = 0; step < 6; ++step) {
        const unsigned creates = service_creates + attribute_creates + service_starts;
        tBTA_GATTS wrong = StartupData(step, true);
        if (step == 0) wrong.reg_oper.uuid.uu.uuid16 ^= 1;
        else if (step == 1) wrong.create.server_if = 4;
        else if (step < 5) wrong.add_result.service_id = 200;
        else wrong.srvc_oper.service_id = 200;
        StartupStep(step, &wrong);
        assert(!init_events && creates == service_creates + attribute_creates + service_starts);
        SucceedStep(step);
        if (step < 5) assert(!init_events);
        const unsigned after = service_creates + attribute_creates + service_starts;
        SucceedStep(step);
        assert(after == service_creates + attribute_creates + service_starts);
    }
    assert(init_events == 1 && init_state == ESP_BLUFI_INIT_OK && service_starts == 1);
    for (unsigned stopped = 0; stopped < 6; ++stopped) {
        BeginStartup();
        for (unsigned step = 0; step < stopped; ++step) SucceedStep(step);
        esp_blufi_deinit();
        for (unsigned step = 0; step < 6; ++step) SucceedStep(step);
        assert(!init_events && audream_host_snapshot().startup == AUDREAM_START_IDLE);
    }
    BeginStartup();
    for (unsigned step = 0; step < 5; ++step) SucceedStep(step);
    Connect(); grant = 100;
    assert(!audream_blufi_transport_lease());
    fail_init_queue = true; SucceedStep(5);
    assert(!init_events && audream_host_snapshot().startup == AUDREAM_START_FAILED);
    assert(!audream_blufi_transport_lease());
    grant = 0;
    puts("PASS: SDK startup waits for service completion, propagates stage failures and rejects duplicate/stopped results");
}
int main(void) {
    TestStartup();
    BeginStartup(); SucceedStep(0); SucceedStep(1);
    assert(permissions == GATT_PERM_WRITE_ENC_MITM);
    SucceedStep(2);
    assert(permissions == GATT_PERM_READ_ENC_MITM);
    SucceedStep(3);
    assert(permissions == (GATT_PERM_READ_ENC_MITM | GATT_PERM_WRITE_ENC_MITM));
    SucceedStep(4); assert(!init_events);
    SucceedStep(5); assert(init_events == 1 && init_state == ESP_BLUFI_INIT_OK);
    Connect();
    Read(0x301, 102, 0); assert(status == GATT_INSUF_AUTHENTICATION);
    Write(0x301, 101, 4, 0, false, 0, true); assert(status == GATT_INSUF_AUTHENTICATION && received == 0);
    Write(0x301, 103, 2, 1, false, 0, true); assert(status == GATT_INSUF_AUTHENTICATION);
    struct blufi_hdr frame = {0};
    frame.data_len = 0;
    esp_blufi_send_encap(&frame); assert(indications == 0);
    grant = 101;
    Read(0x301, 102, 0);
    const uint8_t secure_ready[] = {0x41, 0x44, 0x53, 0x43, 1, 7};
    assert(status == GATT_SUCCESS && last_response.attr_value.len == sizeof(secure_ready));
    assert(memcmp(last_response.attr_value.value, secure_ready, sizeof(secure_ready)) == 0);
    Read(0x301, 103, 0); assert(status == GATT_SUCCESS && last_response.attr_value.len == 2 && last_response.attr_value.value[0] == 0);
    esp_blufi_send_encap(&frame); assert(indications == 0);
    Write(0x301, 103, 1, 1, false, 0, true); assert(status == GATT_INVALID_ATTR_LEN);
    Write(0x301, 103, 2, 2, false, 0, true); assert(status == GATT_INVALID_ATTR_LEN);
    Write(0x302, 103, 2, 1, false, 0, true); assert(status == GATT_INSUF_AUTHENTICATION);
    Write(0x301, 103, 2, 1, false, 0, true); assert(status == GATT_SUCCESS);
    const uint64_t original_subscription = audream_blufi_notify_token(0x301, 102, grant);
    assert(original_subscription);
    Write(0x301, 103, 2, 0, false, 0, true);
    assert(audream_blufi_notify_token(0x301, 102, grant) == 0);
    Write(0x301, 103, 2, 1, false, 0, true);
    assert(audream_blufi_notify_token(0x301, 102, grant) != original_subscription);
    Read(0x301, 103, 0); assert(last_response.attr_value.value[0] == 1);
    esp_blufi_send_encap(&frame); assert(indications == 1);
    Write(0x301, 101, 4, 0, true, 0, true); assert(status == GATT_REQ_NOT_SUPPORTED && allocations == 0 && received == 0);
    Write(0x301, 101, 4, 0, false, 1, true); assert(status == GATT_INVALID_OFFSET);
    Write(0x301, 102, 4, 0, false, 0, true); assert(status == GATT_WRITE_NOT_PERMIT);
    Read(0x301, 101, 0); assert(status == GATT_READ_NOT_PERMIT);
    Read(0x301, 102, 1); assert(status == GATT_INVALID_OFFSET);
    Write(0x301, 101, 4, 0, false, 0, true); assert(status == GATT_SUCCESS && received == 1);
    grant = 102;
    esp_blufi_send_encap(&frame); assert(indications == 1);
    Write(0x301, 103, 2, 1, false, 0, true);
    sendable = 0;
    esp_blufi_send_encap(&frame); assert(indications == 1 && sleeps == 1 && grant == 0);
    const unsigned before = responses;
    Write(0x301, 101, 4, 0, false, 0, false); assert(responses == before && received == 1);
    grant = 103; sendable = 1;
    Write(0x301, 103, 2, 1, false, 0, true);
    Connect();
    esp_blufi_send_encap(&frame); assert(indications == 1);
    Write(0x301, 103, 2, 1, false, 0, true); assert(status == GATT_INSUF_AUTHENTICATION);
    grant = 104;
    const audream_blufi_scope_t previous = audream_blufi_scope_enter(103);
    Write(0x301, 103, 2, 1, false, 0, true); assert(status == GATT_INSUF_AUTHENTICATION);
    Read(0x301, 102, 0); assert(status == GATT_INSUF_AUTHENTICATION);
    Write(0x301, 101, 4, 0, false, 0, true); assert(status == GATT_INSUF_AUTHENTICATION && received == 1);
    esp_blufi_send_encap(&frame); assert(indications == 1);
    audream_blufi_scope_leave(previous);
    Write(0x301, 103, 2, 1, false, 0, true);
    Write(0x301, 103, 2, 0, false, 0, true);
    esp_blufi_send_encap(&frame); assert(indications == 1);
    tGATT_REQ_DATA execute = {0}; execute.exec_write = GATT_PREP_WRITE_EXEC;
    tBTA_GATTS event = {0};
    event.req_data.conn_id = 0x301; event.req_data.p_data = &execute;
    blufi_profile_cb(BTA_GATTS_EXEC_WRITE_EVT, &event);
    assert(status == GATT_REQ_NOT_SUPPORTED && received == 1);
    Write(0x301, 103, 2, 1, false, 0, true);
    CheckSnapshotTransitions();
    audream_host_state.epoch = UINT64_MAX - 1;
    Write(0x301, 103, 2, 1, false, 0, true);
    assert(status == GATT_INSUF_RESOURCE && !audream_blufi_notify_token(0x301, 102, grant));
    Write(0x301, 103, 2, 1, false, 0, true);
    assert(status == GATT_INSUF_AUTHENTICATION && audream_host_snapshot().epoch == UINT64_MAX);
    esp_blufi_deinit(); esp_blufi_send_encap(&frame); assert(indications == 1);
    puts("PASS: actual patched BLUFI host attributes, control admission, CCC, prepared-write rejection and send revocation");
}
