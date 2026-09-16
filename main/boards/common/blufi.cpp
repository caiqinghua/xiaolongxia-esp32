#include "blufi.h"
#include "watch/watch_wifi_gate.h"
#include "audream_device_control.h"
#include "wifi_credential_fields.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "esp_bt.h"
#include "esp_event.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#if CONFIG_AUDREAM_WATCH_ANCS
#include "watch/watch_ancs_client.h"
#endif
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
#include "watch/watch_android_gatt_server.h"
#endif
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
#include "watch/watch_pairing_gatt_server.h"
#include "watch/watch_blufi_send.h"
#include "watch/watch_blufi_scope.h"
#endif

#define BLUFI_DEVICE_NAME "Audream Watch"

#ifdef CONFIG_BT_BLUEDROID_ENABLED
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "console/console.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
extern void esp_blufi_gatt_svr_register_cb(struct ble_gatt_register_ctxt* ctxt, void* arg);
extern int esp_blufi_gatt_svr_init(void);
extern void esp_blufi_gatt_svr_deinit(void);
extern void esp_blufi_btc_init(void);
extern void esp_blufi_btc_deinit(void);
#endif

extern "C" {
void esp_blufi_adv_start(void);

void esp_blufi_adv_start_with_name(const char* name);

void esp_blufi_adv_stop(void);

void esp_blufi_disconnect(void);

void btc_blufi_report_error(esp_blufi_error_state_t state);

#ifdef CONFIG_BT_BLUEDROID_ENABLED
void esp_blufi_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
void esp_blufi_gatt_svr_register_cb(struct ble_gatt_register_ctxt* ctxt, void* arg);
int esp_blufi_gatt_svr_init(void);
void esp_blufi_gatt_svr_deinit(void);
void esp_blufi_btc_init(void);
void esp_blufi_btc_deinit(void);
#endif
}

#include <wifi_station.h>
#include "esp_crc.h"
#include "esp_random.h"
#include "mbedtls/md5.h"
#include "ssid_manager.h"

static const char* BLUFI_TAG = "BLUFI_CLASS";

namespace {

void LogBluetoothHeap(const char* stage) {
    ESP_LOGI(
        BLUFI_TAG,
        "Bluetooth heap at %s: internal_free=%u internal_largest=%u spiram_free=%u",
        stage,
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM))
    );
}

constexpr uint16_t kAudreamCompanyIdentifier = 0xFFFF;
constexpr char kAudreamManufacturerMarker[] = "ADRM";
constexpr size_t kMaxManufacturerDataLength = 29;
constexpr size_t kManufacturerHeaderLength = 2 + sizeof(kAudreamManufacturerMarker) - 1;
constexpr uint32_t kAudreamControlWorkerStackSize = 6144;
constexpr UBaseType_t kAudreamControlWorkerPriority = 5;
constexpr UBaseType_t kAudreamControlQueueLength = 4;

#ifdef CONFIG_BT_BLUEDROID_ENABLED
uint8_t g_audream_manufacturer_data[kMaxManufacturerDataLength] = {};
esp_ble_adv_data_t g_audream_scan_response_data = {
    .set_scan_rsp = true,
    .include_name = false,
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = g_audream_manufacturer_data,
    .service_data_len = 0,
    .p_service_data = nullptr,
    .service_uuid_len = 0,
    .p_service_uuid = nullptr,
    .flag = 0x0,
};
#endif

std::string ReadDeviceSerialNumber() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    uint8_t serial_number[33] = {};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK && serial_number[0] != 0) {
        serial_number[32] = 0;
        return std::string(reinterpret_cast<char*>(serial_number));
    }
#endif

    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        char fallback[13] = {};
        snprintf(fallback, sizeof(fallback), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return std::string(fallback);
    }
    return "UNKNOWN";
}

#ifdef CONFIG_BT_BLUEDROID_ENABLED
void ConfigureAudreamScanResponse(const std::string& serial_number) {
    const size_t max_serial_length = kMaxManufacturerDataLength - kManufacturerHeaderLength;
    const size_t serial_length = std::min(serial_number.size(), max_serial_length);
    if (serial_number.size() > max_serial_length) {
        ESP_LOGW(BLUFI_TAG, "Serial number is too long for BLE scan response and will be truncated");
    }

    memset(g_audream_manufacturer_data, 0, sizeof(g_audream_manufacturer_data));
    g_audream_manufacturer_data[0] = static_cast<uint8_t>(kAudreamCompanyIdentifier & 0xFF);
    g_audream_manufacturer_data[1] = static_cast<uint8_t>((kAudreamCompanyIdentifier >> 8) & 0xFF);
    memcpy(&g_audream_manufacturer_data[2], kAudreamManufacturerMarker, sizeof(kAudreamManufacturerMarker) - 1);
    memcpy(&g_audream_manufacturer_data[kManufacturerHeaderLength], serial_number.data(), serial_length);

    g_audream_scan_response_data.manufacturer_len = kManufacturerHeaderLength + serial_length;
    esp_err_t ret = esp_ble_gap_config_adv_data(&g_audream_scan_response_data);
    if (ret != ESP_OK) {
        ESP_LOGW(BLUFI_TAG, "Failed to configure Audream scan response: %s", esp_err_to_name(ret));
    }
}
#endif

void StartAudreamBluFiAdvertising() {
    std::string serial_number = ReadDeviceSerialNumber();
    ESP_LOGI(BLUFI_TAG, "BLUFI device name: %s, serial number: %s", BLUFI_DEVICE_NAME, serial_number.c_str());

#ifdef CONFIG_BT_BLUEDROID_ENABLED
#if CONFIG_AUDREAM_WATCH_UI
    Blufi::GetInstance().BeginAdvertising();
#endif
    ConfigureAudreamScanResponse(serial_number);
#endif
    esp_blufi_adv_start_with_name(BLUFI_DEVICE_NAME);
}

struct AudreamCustomRequest {
    std::vector<uint8_t> data;
    Blufi::ControlTicket peer;
};

const char* FormatAudreamCustomOpcode(
    const uint8_t* data,
    size_t length,
    char* buffer,
    size_t buffer_size
) {
    if (data == nullptr || length < 2 || buffer == nullptr || buffer_size == 0) {
        return "unavailable";
    }
    std::snprintf(buffer, buffer_size, "0x%02x", data[1]);
    return buffer;
}

uint16_t ReadAudreamCustomRequestId(const uint8_t* data, size_t length) {
    if (data == nullptr || length < 4) {
        return 0;
    }
    return static_cast<uint16_t>(data[2]) |
        (static_cast<uint16_t>(data[3]) << 8);
}

void AppendUInt32LittleEndian(std::vector<uint8_t>& output, uint32_t value) {
    output.push_back(static_cast<uint8_t>(value & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void AppendUInt64LittleEndian(std::vector<uint8_t>& output, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

void AppendLengthPrefixedString(std::vector<uint8_t>& output, const std::string& value) {
    const size_t clamped_size = std::min<size_t>(value.size(), 255);
    output.push_back(static_cast<uint8_t>(clamped_size));
    output.insert(output.end(), value.begin(), value.begin() + clamped_size);
}

esp_err_t SendAudreamDeviceEvent(uint8_t event, const std::vector<uint8_t>& payload) {
    auto& blufi = Blufi::GetInstance();
    const auto peer = blufi.CaptureControlPeer();
    if (!peer) return ESP_ERR_INVALID_STATE;
    std::vector<uint8_t> event_payload;
    event_payload.reserve(3 + payload.size());
    event_payload.push_back(0x01);
    event_payload.push_back(0xF0);
    event_payload.push_back(event);
    event_payload.insert(event_payload.end(), payload.begin(), payload.end());
    return blufi.SendControlData(peer, event_payload.data(), event_payload.size());
}

QueueHandle_t g_audream_custom_queue = nullptr;
TaskHandle_t g_audream_custom_task = nullptr;
std::atomic<uint8_t> g_audream_active_opcode{0};
std::atomic<uint16_t> g_audream_active_request_id{0};
std::atomic<uint32_t> g_audream_active_started_at_ms{0};

void AudreamCustomDataTask(void* arg) {
    (void)arg;
    while (true) {
        AudreamCustomRequest* raw_request = nullptr;
        if (xQueueReceive(g_audream_custom_queue, &raw_request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        std::unique_ptr<AudreamCustomRequest> request(raw_request);
        if (!request) {
            ESP_LOGE(BLUFI_TAG, "Audream custom data worker received a null request");
            continue;
        }

        if (!Blufi::GetInstance().IsControlPeerCurrent(request->peer)) {
            ESP_LOGW(BLUFI_TAG, "Discarded custom request from an expired or unauthorized BLE connection");
            continue;
        }
#if CONFIG_AUDREAM_WATCH_UI && defined(CONFIG_BT_BLUEDROID_ENABLED)
        Blufi::GetInstance().NotifyLinkActivity();
#endif

        char opcode_buffer[8] = {};
        const uint8_t opcode = request->data.size() >= 2 ? request->data[1] : 0;
        const uint16_t request_id = ReadAudreamCustomRequestId(
            request->data.data(),
            request->data.size()
        );
        g_audream_active_opcode.store(opcode, std::memory_order_relaxed);
        g_audream_active_request_id.store(request_id, std::memory_order_relaxed);
        g_audream_active_started_at_ms.store(
            static_cast<uint32_t>(esp_timer_get_time() / 1000),
            std::memory_order_relaxed
        );
        ESP_LOGI(
            BLUFI_TAG,
            "Handling Audream custom data off the BLUFI task: %u bytes opcode=%s request_id=%u",
            static_cast<unsigned>(request->data.size()),
            FormatAudreamCustomOpcode(
                request->data.data(),
                request->data.size(),
                opcode_buffer,
                sizeof(opcode_buffer)
            ),
            static_cast<unsigned>(request_id)
        );
        std::vector<uint8_t> response =
            AudreamDeviceControl::GetInstance().HandleRequest(
                request->data.data(),
                request->data.size()
            );
        esp_err_t result = Blufi::GetInstance().SendControlData(
            request->peer,
            response.data(),
            response.size()
        );
        if (result != ESP_OK) {
            ESP_LOGE(
                BLUFI_TAG,
                "Failed to send Audream custom data response: %s response_size=%u free_heap=%u largest_block=%u",
                esp_err_to_name(result),
                static_cast<unsigned>(response.size()),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))
            );
        }
        g_audream_active_started_at_ms.store(0, std::memory_order_relaxed);
        g_audream_active_request_id.store(0, std::memory_order_relaxed);
        g_audream_active_opcode.store(0, std::memory_order_relaxed);
    }
}

bool EnsureAudreamCustomDataWorker() {
    if (g_audream_custom_queue == nullptr) {
        g_audream_custom_queue = xQueueCreate(
            kAudreamControlQueueLength,
            sizeof(AudreamCustomRequest*)
        );
        if (g_audream_custom_queue == nullptr) {
            ESP_LOGE(BLUFI_TAG, "Failed to create Audream custom data queue");
            return false;
        }
    }

    if (g_audream_custom_task == nullptr) {
        BaseType_t created = xTaskCreate(
            AudreamCustomDataTask,
            "audream_ctrl",
            kAudreamControlWorkerStackSize,
            nullptr,
            kAudreamControlWorkerPriority,
            &g_audream_custom_task
        );
        if (created != pdPASS) {
            ESP_LOGE(BLUFI_TAG, "Failed to create Audream custom data worker task");
            g_audream_custom_task = nullptr;
            return false;
        }
    }

    return true;
}

void QueueAudreamCustomData(const uint8_t* data, uint32_t length) {
    const auto peer = Blufi::GetInstance().CaptureControlPeer();
    if (!peer) {
        ESP_LOGW(BLUFI_TAG, "Rejected custom request without an admitted BLE connection");
        return;
    }
    if (data == nullptr || length == 0) {
        ESP_LOGW(BLUFI_TAG, "Ignored empty Audream custom data request");
        return;
    }
    if (!EnsureAudreamCustomDataWorker()) {
        return;
    }

    auto* request = new (std::nothrow) AudreamCustomRequest();
    if (request == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Failed to allocate Audream custom data request");
        return;
    }
    request->data.assign(data, data + length);
    request->peer = peer;

    if (xQueueSend(g_audream_custom_queue, &request, pdMS_TO_TICKS(100)) != pdTRUE) {
        const uint32_t active_started_at_ms = g_audream_active_started_at_ms.load(
            std::memory_order_relaxed
        );
        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        const uint32_t active_elapsed_ms = active_started_at_ms > 0
            ? now_ms - active_started_at_ms
            : 0;
        ESP_LOGE(
            BLUFI_TAG,
            "Audream custom data queue is full: queued=%u active_opcode=0x%02x active_request_id=%u active_elapsed_ms=%lu",
            static_cast<unsigned>(uxQueueMessagesWaiting(g_audream_custom_queue)),
            static_cast<unsigned>(g_audream_active_opcode.load(std::memory_order_relaxed)),
            static_cast<unsigned>(g_audream_active_request_id.load(std::memory_order_relaxed)),
            static_cast<unsigned long>(active_elapsed_ms)
        );
        delete request;
    }
}

} // namespace

static uint64_t ReadControlAuthorization(uint16_t connection, const uint8_t* address) {
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    const uint64_t lease = WatchAncsClient::Get().ControlPeerLease(connection, address);
    return lease && lease == audream_blufi_operation_lease() ? lease : 0;
#else
    (void)connection;
    (void)address;
    return 0;
#endif
}

Blufi::ControlTicket Blufi::CaptureControlPeer() const {
    return control_session_.Capture(ReadControlAuthorization);
}

bool Blufi::IsControlPeerCurrent(const ControlTicket& ticket) const {
    return control_session_.IsCurrent(ticket, ReadControlAuthorization);
}

esp_err_t Blufi::SendControlData(const ControlTicket& ticket, const uint8_t* data, size_t length) {
    if (!data || !length || length > UINT32_MAX) return ESP_ERR_INVALID_ARG;
    if (!IsControlPeerCurrent(ticket)) return ESP_ERR_INVALID_STATE;
#if CONFIG_AUDREAM_WATCH_UI && defined(CONFIG_BT_BLUEDROID_ENABLED)
    NotifyLinkActivity();
#endif
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    return audream_blufi_send_custom_data_for_lease(ticket.authorization,
        const_cast<uint8_t*>(data), static_cast<uint32_t>(length));
#else
    return esp_blufi_send_custom_data(const_cast<uint8_t*>(data), static_cast<uint32_t>(length));
#endif
}

esp_err_t Blufi::SendWifiReport(const ControlTicket& peer, wifi_mode_t mode,
    esp_blufi_sta_conn_state_t state, uint8_t softap_connections, esp_blufi_extra_info_t* info) {
    if (!IsControlPeerCurrent(peer)) return ESP_ERR_INVALID_STATE;
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    return audream_blufi_send_wifi_conn_report_for_lease(peer.authorization, mode, state, softap_connections, info);
#else
    return esp_blufi_send_wifi_conn_report(mode, state, softap_connections, info);
#endif
}

static wifi_mode_t GetWifiModeWithFallback(const WifiManager& wifi) {
    if (wifi.IsConfigMode()) {
        return WIFI_MODE_AP;
    }
    if (wifi.IsInitialized() && wifi.IsConnected()) {
        return WIFI_MODE_STA;
    }

    wifi_mode_t mode = WIFI_MODE_STA;
    esp_wifi_get_mode(&mode);
    return mode;
}

Blufi& Blufi::GetInstance() {
    static Blufi instance;
    return instance;
}

Blufi::Blufi()
    : m_sec(nullptr),
      m_ble_is_connected(false),
      m_sta_connected(false),
      m_sta_got_ip(false),
      m_sta_ssid_len(0) {
    memset(&m_sta_config, 0, sizeof(m_sta_config));
    memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
    memset(m_sta_ssid, 0, sizeof(m_sta_ssid));
    memset(&m_sta_conn_info, 0, sizeof(m_sta_conn_info));
}

Blufi::~Blufi() {
    if (m_sec) {
        _security_deinit();
    }
}

esp_err_t Blufi::SendDeviceWillSleepEvent() {
    const esp_err_t result = SendAudreamDeviceEvent(0x01, {});
    if (result != ESP_OK) {
        ESP_LOGW(BLUFI_TAG, "Failed to send device will sleep event: %s", esp_err_to_name(result));
    } else {
        ESP_LOGI(BLUFI_TAG, "Sent device will sleep event");
    }
    return result;
}

esp_err_t Blufi::SendRecordingStartedEvent(
    const std::string& recording_id,
    const std::string& file_name,
    uint64_t started_at_unix_ms
) {
    std::vector<uint8_t> payload;
    payload.reserve(2 + recording_id.size() + file_name.size() + 8);
    AppendLengthPrefixedString(payload, recording_id);
    AppendLengthPrefixedString(payload, file_name);
    AppendUInt64LittleEndian(payload, started_at_unix_ms);
    const esp_err_t result = SendAudreamDeviceEvent(0x02, payload);
    if (result != ESP_OK) {
        ESP_LOGW(BLUFI_TAG, "Failed to send recording started event: %s", esp_err_to_name(result));
    } else {
        ESP_LOGI(BLUFI_TAG, "Sent recording started event");
    }
    return result;
}

esp_err_t Blufi::SendRecordingStoppedEvent(
    const std::string& recording_id,
    const std::string& file_name,
    uint32_t duration_ms,
    uint32_t size_bytes
) {
    std::vector<uint8_t> payload;
    payload.reserve(2 + recording_id.size() + file_name.size() + 8);
    AppendLengthPrefixedString(payload, recording_id);
    AppendLengthPrefixedString(payload, file_name);
    AppendUInt32LittleEndian(payload, duration_ms);
    AppendUInt32LittleEndian(payload, size_bytes);
    const esp_err_t result = SendAudreamDeviceEvent(0x03, payload);
    if (result != ESP_OK) {
        ESP_LOGW(BLUFI_TAG, "Failed to send recording stopped event: %s", esp_err_to_name(result));
    } else {
        ESP_LOGI(BLUFI_TAG, "Sent recording stopped event");
    }
    return result;
}

esp_err_t Blufi::init(bool enable_wifi_provisioning) {
    return init(enable_wifi_provisioning ? BlufiRole::kWifiProvisioning : BlufiRole::kControlOnly);
}

esp_err_t Blufi::init(BlufiRole role) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (control_lease_ && role != BlufiRole::kControlOnly) return ESP_ERR_INVALID_STATE;
    if (role != BlufiRole::kControlOnly &&
        (!GetWatchWifiGate().Enabled() || GetWatchWifiGate().TransferActive())) return ESP_ERR_INVALID_STATE;
    if (shutdown_failed_) {
        ESP_LOGE(BLUFI_TAG, "BLUFI restart rejected after incomplete shutdown");
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    if (session_.IsActive() && profile_startup_.Failed(esp_timer_get_time() / 1000)) return ESP_FAIL;
#endif
    if (session_.IsActive() && session_.Role() == role) {
        return ESP_OK;
    }
    esp_err_t ret = ESP_FAIL;
    if (role != BlufiRole::kControlOnly) {
        auto& wifi_manager = WifiManager::GetInstance();
        if (wifi_manager.IsInitialized() && wifi_manager.IsConfigMode()) {
            ESP_LOGE(BLUFI_TAG,
                     "Blufi and WiFi hotspot network configuration cannot "
                     "be used simultaneously.");
            return ESP_ERR_INVALID_STATE;
        }
        // Provisioning owns scanning; WifiStation must not consume its results.
        if (wifi_manager.IsInitialized()) {
            if (!GetWatchWifiGate().Run([&]() { wifi_manager.StopStation(); return true; })) return ESP_ERR_INVALID_STATE;
        }
    }

    const bool already_active = session_.IsActive();
    session_.Activate(role);
    {
        std::lock_guard<std::mutex> credentials_lock(credentials_mutex_);
        m_sta_config = {};
        m_ssid_valid = false;
        m_password_valid = true;
    }
    ESP_LOGI(BLUFI_TAG, "BLUFI role changed: provisioning=%d control=%d reuse_stack=%d",
             session_.AllowsProvisioning(), session_.KeepsControl(), already_active);
    if (already_active) {
        return ESP_OK;
    }
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    profile_startup_.Begin(esp_timer_get_time() / 1000);
#endif

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ret = _controller_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "BLUFI controller init failed: %s", esp_err_to_name(ret));
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
        profile_startup_.Fail();
        if (_controller_deinit() != ESP_OK) shutdown_failed_ = true;
#else
        _controller_deinit();
#endif
        session_.Deactivate();
        return ret;
    }
#endif

    ret = _host_and_cb_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "BLUFI host and cb init failed: %s", esp_err_to_name(ret));
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
        profile_startup_.Fail();
        session_.Deactivate();
        control_session_.Reset();
        // Never stop the controller while a partially started host still owns it.
        if (AbortSecureHostStart() != ESP_OK) {
            shutdown_failed_ = true;
            ESP_LOGE(BLUFI_TAG, "Secure BLE startup cleanup failed; restart requires reboot");
            return ret;
        }
#endif
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
        if (_controller_deinit() != ESP_OK) shutdown_failed_ = true;
#else
        _controller_deinit();
#endif
#endif
        session_.Deactivate();
        return ret;
    }

    ESP_LOGI(BLUFI_TAG, "BLUFI VERSION %04x", esp_blufi_get_version());
    return ESP_OK;
}

esp_err_t Blufi::deinit() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return DeinitLocked();
}

void Blufi::DeinitCompletedProvisioning(BlufiSessionState::Ticket ticket) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (session_.IsCurrent(ticket) && session_.IsProvisioned() && !session_.KeepsControl()) {
        DeinitLocked();
    }
}

esp_err_t Blufi::DeinitLocked() {
    if (control_lease_) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_OK;
    if (shutdown_failed_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (session_.IsActive()) {
        session_.Deactivate();
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
        profile_startup_.Stop();
#endif
        control_session_.Reset();
        m_ble_is_connected = false;
#if CONFIG_AUDREAM_WATCH_UI && defined(CONFIG_BT_BLUEDROID_ENABLED)
        link_power_.Disconnected();
#endif
        ret = _host_deinit();
        if (ret) {
            ESP_LOGE(BLUFI_TAG, "Host deinit failed: %s", esp_err_to_name(ret));
            shutdown_failed_ = true;
            return ret;
        }
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
        ret = _controller_deinit();
        if (ret) {
            ESP_LOGE(BLUFI_TAG, "Controller deinit failed: %s", esp_err_to_name(ret));
            shutdown_failed_ = true;
        }
#endif
    }
    return ret;
}

bool Blufi::IsActive() const {
    return session_.IsActive();
}
bool Blufi::IsControlReady() const {
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    return profile_startup_.Ready(esp_timer_get_time() / 1000) && session_.IsActive() && !session_.AllowsProvisioning();
#else
    return session_.IsActive() && !session_.AllowsProvisioning();
#endif
}

void Blufi::HandleProfileInitialized(bool success) {
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    const auto now_ms = esp_timer_get_time() / 1000;
    if (!profile_startup_.Complete(success, now_ms)) {
        if (profile_startup_.Failed(now_ms)) ESP_LOGE(BLUFI_TAG, "BLUFI profile initialization failed or timed out");
        return;
    }
    if (!session_.IsActive()) return;
#else
    (void)success;
#endif
    ESP_LOGI(BLUFI_TAG, "BLUFI init finish");
    StartAudreamBluFiAdvertising();
}

esp_err_t Blufi::SendRecordingStatusEvent(
    const std::string& recording_id,
    const std::string& file_name,
    uint32_t duration_ms
) {
    std::vector<uint8_t> payload;
    payload.reserve(2 + recording_id.size() + file_name.size() + 4);
    AppendLengthPrefixedString(payload, recording_id);
    AppendLengthPrefixedString(payload, file_name);
    AppendUInt32LittleEndian(payload, duration_ms);
    const esp_err_t result = SendAudreamDeviceEvent(0x04, payload);
    if (result != ESP_OK) {
        ESP_LOGI(BLUFI_TAG, "Recording status event not sent: %s", esp_err_to_name(result));
    } else {
        ESP_LOGI(
            BLUFI_TAG,
            "Sent recording status event duration_ms=%lu",
            static_cast<unsigned long>(duration_ms)
        );
    }
    return result;
}

#ifdef CONFIG_BT_BLUEDROID_ENABLED
esp_err_t Blufi::_host_init() {
    LogBluetoothHeap("host init start");
    esp_err_t ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s init bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s enable bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
    LogBluetoothHeap("host init complete");
    ESP_LOGI(BLUFI_TAG, "BD ADDR: " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(esp_bt_dev_get_address()));
    return ESP_OK;
}

esp_err_t Blufi::_host_deinit() {
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    WatchPairingGattServer::Get().HostStopping();
#endif
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
    WatchAndroidGattServer::Get().HostStopping();
#endif
#if CONFIG_AUDREAM_WATCH_ANCS
    WatchAncsClient::Get().HostStopping();
#endif
    esp_err_t ret = esp_blufi_profile_deinit();
    if (ret != ESP_OK)
        return ret;

    ret = esp_bluedroid_disable();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s disable bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_INITIALIZED) return ESP_FAIL;
#endif
    ret = esp_bluedroid_deinit();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s deinit bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) return ESP_FAIL;
#endif
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
    WatchAndroidGattServer::Get().HostStopped();
#endif
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    WatchPairingGattServer::Get().HostStopped();
#endif
#if CONFIG_AUDREAM_WATCH_ANCS
    WatchAncsClient::Get().HostStopped();
#endif
    return ESP_OK;
}

esp_err_t Blufi::_gap_register_callback() {
#if CONFIG_AUDREAM_WATCH_ANCS || CONFIG_AUDREAM_WATCH_UI
    esp_err_t rc = esp_ble_gap_register_callback([](esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
#if CONFIG_AUDREAM_WATCH_UI
        if (event == ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT && param) {
            const auto& update = param->update_conn_params;
            Blufi::GetInstance().HandleLinkParameterUpdate(update.bda, update.min_int, update.max_int,
                update.conn_int, update.latency, update.timeout, update.status == ESP_BT_STATUS_SUCCESS);
        }
        if (Blufi::GetInstance().HandleAdvertisingGapEvent(event, param)) return;
#endif
#if CONFIG_AUDREAM_WATCH_ANCS
        if (WatchAncsClient::Get().GapEvent(event, param)) return;
#endif
        esp_blufi_gap_event_handler(event, param);
    });
#else
    esp_err_t rc = esp_ble_gap_register_callback(esp_blufi_gap_event_handler);
#endif
    if (rc) {
        return rc;
    }
    return esp_blufi_profile_init();
}

esp_err_t Blufi::_host_and_cb_init() {
    static esp_blufi_callbacks_t blufi_callbacks = {
        .event_cb = &_event_callback_trampoline,
        .negotiate_data_handler = &_negotiate_data_handler_trampoline,
        .encrypt_func = &_encrypt_func_trampoline,
        .decrypt_func = &_decrypt_func_trampoline,
        .checksum_func = &_checksum_func_trampoline,
    };

    esp_err_t ret = _host_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s initialise host failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
    ret = esp_blufi_register_callbacks(&blufi_callbacks);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s blufi register failed, error code = %x", __func__, ret);
        return ret;
    }
#if CONFIG_AUDREAM_WATCH_ANCS
    if (!WatchAncsClient::Get().HostStarted()) {
        ESP_LOGE(BLUFI_TAG, "Watch notification client initialization failed");
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
        return ESP_FAIL;
#endif
    }
#endif
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
    if (!WatchAndroidGattServer::Get().HostStarted()) {
        ESP_LOGE(BLUFI_TAG, "Watch Android notification service initialization failed");
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
        return ESP_FAIL;
#endif
    }
#endif
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    if (!WatchPairingGattServer::Get().HostStarted()) {
        ESP_LOGE(BLUFI_TAG, "Watch device pairing service initialization failed");
        return ESP_FAIL;
    }
#endif
    ret = _gap_register_callback();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s gap register failed, error code = %x", __func__, ret);
        return ret;
    }
    return ESP_OK;
}
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
esp_err_t Blufi::AbortSecureHostStart() {
    WatchPairingGattServer::Get().HostStopping();
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
    WatchAndroidGattServer::Get().HostStopping();
#endif
    WatchAncsClient::Get().HostStopping();
    // The profile init request has not been accepted on this failure path.
    // Do not enqueue profile deinit for an uninitialized profile.
    auto status = esp_bluedroid_get_status();
    if (status == ESP_BLUEDROID_STATUS_ENABLED) {
        if (esp_bluedroid_disable() != ESP_OK) return ESP_FAIL;
        status = esp_bluedroid_get_status();
    }
    if (status == ESP_BLUEDROID_STATUS_INITIALIZED) {
        if (esp_bluedroid_deinit() != ESP_OK) return ESP_FAIL;
        status = esp_bluedroid_get_status();
    }
    if (status != ESP_BLUEDROID_STATUS_UNINITIALIZED) return ESP_FAIL;
    WatchPairingGattServer::Get().HostStopped();
#if CONFIG_AUDREAM_WATCH_ANDROID_NOTIFICATIONS
    WatchAndroidGattServer::Get().HostStopped();
#endif
    WatchAncsClient::Get().HostStopped();
    return ESP_OK;
}
#endif
#endif /* CONFIG_BT_BLUEDROID_ENABLED */

#ifdef CONFIG_BT_NIMBLE_ENABLED
// Stubs for NimBLE specific store functionality
void ble_store_config_init();

void Blufi::_nimble_on_reset(int reason) {
    ESP_LOGE(BLUFI_TAG, "NimBLE Resetting state; reason=%d", reason);
}

void Blufi::_nimble_on_sync() { esp_blufi_profile_init(); }

void Blufi::_nimble_host_task(void* param) {
    ESP_LOGI(BLUFI_TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t Blufi::_host_init() {
    ble_hs_cfg.reset_cb = _nimble_on_reset;
    ble_hs_cfg.sync_cb = _nimble_on_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;

    ble_hs_cfg.sm_io_cap = 4;
#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_bonding = 1;
#endif

    int rc = esp_blufi_gatt_svr_init();
    assert(rc == 0);

    ble_store_config_init();
    esp_blufi_btc_init();

    esp_err_t err = esp_nimble_enable(_nimble_host_task);
    if (err) {
        ESP_LOGE(BLUFI_TAG, "%s failed: %s", __func__, esp_err_to_name(err));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t Blufi::_host_deinit(void) {
    esp_err_t ret = nimble_port_stop();
    if (ret == ESP_OK) {
        esp_nimble_deinit();
    }
    esp_blufi_gatt_svr_deinit();
    ret = esp_blufi_profile_deinit();
    esp_blufi_btc_deinit();
    return ret;
}

esp_err_t Blufi::_gap_register_callback(void) { return ESP_OK; }

esp_err_t Blufi::_host_and_cb_init() {
    static esp_blufi_callbacks_t blufi_callbacks = {
        .event_cb = &_event_callback_trampoline,
        .negotiate_data_handler = &_negotiate_data_handler_trampoline,
        .encrypt_func = &_encrypt_func_trampoline,
        .decrypt_func = &_decrypt_func_trampoline,
        .checksum_func = &_checksum_func_trampoline,
    };

    esp_err_t ret = esp_blufi_register_callbacks(&blufi_callbacks);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s blufi register failed, error code = %x", __func__, ret);
        return ret;
    }

    // Host init must be called after registering callbacks for NimBLE
    ret = _host_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s initialise host failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}
#endif /* CONFIG_BT_NIMBLE_ENABLED */

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
esp_err_t Blufi::_controller_init() {
    LogBluetoothHeap("controller init start");
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
    LogBluetoothHeap("controller init complete");

#ifdef CONFIG_BT_NIMBLE_ENABLED
    ret = esp_nimble_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "esp_nimble_init() failed: %s", esp_err_to_name(ret));
        return ret;
    }
#endif
    return ESP_OK;
}

esp_err_t Blufi::_controller_deinit() {
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    // Never tear down the radio beneath a host that still owns it.
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) return ESP_ERR_INVALID_STATE;
    auto status = esp_bt_controller_get_status();
    if (status == ESP_BT_CONTROLLER_STATUS_IDLE) return ESP_OK;
    if (status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        const auto ret = esp_bt_controller_disable();
        if (ret != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Controller disable failed: %s", esp_err_to_name(ret));
            return ret;
        }
        status = esp_bt_controller_get_status();
    }
    if (status != ESP_BT_CONTROLLER_STATUS_INITED) return ESP_ERR_INVALID_STATE;
    const auto ret = esp_bt_controller_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "Controller deinit failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE ? ESP_OK : ESP_FAIL;
#else
    esp_err_t ret = esp_bt_controller_disable();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s disable controller failed: %s", __func__, esp_err_to_name(ret));
    }
    ret = esp_bt_controller_deinit();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s deinit controller failed: %s", __func__, esp_err_to_name(ret));
    }
    return ret;
#endif
}
#endif

static int myrand(void* rng_state, unsigned char* output, size_t len) {
    esp_fill_random(output, len);
    return 0;
}

void Blufi::_security_init() {
    m_sec = new BlufiSecurity();
    if (m_sec == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Failed to allocate security context");
        return;
    }
    memset(m_sec, 0, sizeof(BlufiSecurity));
    m_sec->dhm = new mbedtls_dhm_context();
    m_sec->aes = new mbedtls_aes_context();

    mbedtls_dhm_init(m_sec->dhm);
    mbedtls_aes_init(m_sec->aes);

    memset(m_sec->iv, 0x0, sizeof(m_sec->iv));
}

void Blufi::_security_deinit() {
    if (m_sec == nullptr)
        return;

    if (m_sec->dh_param) {
        free(m_sec->dh_param);
    }
    mbedtls_dhm_free(m_sec->dhm);
    mbedtls_aes_free(m_sec->aes);
    delete m_sec->dhm;
    delete m_sec->aes;
    delete m_sec;
    m_sec = nullptr;
}

void Blufi::_dh_negotiate_data_handler(uint8_t* data, int len, uint8_t** output_data,
                                       int* output_len, bool* need_free) {
    if (m_sec == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Security not initialized in DH handler");
        btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }

    if (len < 1) {
        ESP_LOGE(BLUFI_TAG, "DH handler: data too short");
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    uint8_t type = data[0];
    switch (type) {
        case 0x00:
            if (len < 3) {
                ESP_LOGE(BLUFI_TAG, "DH_PARAM_LEN packet too short");
                btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
                return;
            }

            m_sec->dh_param_len = (data[1] << 8) | data[2];
            if (m_sec->dh_param) {
                free(m_sec->dh_param);
                m_sec->dh_param = nullptr;
            }
            m_sec->dh_param = (uint8_t*)malloc(m_sec->dh_param_len);
            if (m_sec->dh_param == nullptr) {
                ESP_LOGE(BLUFI_TAG, "DH malloc failed");
                btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
            }
            break;
        case 0x01: {
            if (m_sec->dh_param == nullptr) {
                ESP_LOGE(BLUFI_TAG, "DH param not allocated");
                btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
                return;
            }
            uint8_t* param = m_sec->dh_param;
            memcpy(m_sec->dh_param, &data[1], m_sec->dh_param_len);
            int ret = mbedtls_dhm_read_params(m_sec->dhm, &param, &param[m_sec->dh_param_len]);
            if (ret) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_dhm_read_params failed %d", ret);
                btc_blufi_report_error(ESP_BLUFI_READ_PARAM_ERROR);
                return;
            }

            const int dhm_len = mbedtls_dhm_get_len(m_sec->dhm);

            ret = mbedtls_dhm_make_public(m_sec->dhm, dhm_len, m_sec->self_public_key, dhm_len,
                                          myrand, NULL);
            if (ret != 0) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_dhm_make_public failed: %d", ret);
                btc_blufi_report_error(ESP_BLUFI_MAKE_PUBLIC_ERROR);
                return;
            }
            ret = mbedtls_dhm_calc_secret(m_sec->dhm, m_sec->share_key, SHARE_KEY_LEN,
                                          &m_sec->share_len, myrand, NULL);
            if (ret != 0) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_dhm_calc_secret failed: %d", ret);
                btc_blufi_report_error(ESP_BLUFI_ENCRYPT_ERROR);
                return;
            }

            ret = mbedtls_md5(m_sec->share_key, m_sec->share_len, m_sec->psk);
            if (ret != 0) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_md5 failed: %d", ret);
                btc_blufi_report_error(ESP_BLUFI_CALC_MD5_ERROR);
                return;
            }
            ret = mbedtls_aes_setkey_enc(m_sec->aes, m_sec->psk, PSK_LEN * 8);
            if (ret != 0) {
                ESP_LOGE(BLUFI_TAG, "mbedtls_aes_setkey_enc failed: -0x%04X", -ret);
                btc_blufi_report_error(ESP_BLUFI_ENCRYPT_ERROR);
                return;
            }
            *output_data = m_sec->self_public_key;
            *output_len = dhm_len;
            *need_free = false;
            ESP_LOGI(BLUFI_TAG, "DH negotiation completed successfully");

            free(m_sec->dh_param);
            m_sec->dh_param = nullptr;
            m_sec->dh_param_len = 0;
            break;
        }
        default:
            ESP_LOGE(BLUFI_TAG, "DH handler unknown type: %d", type);
    }
}

int Blufi::_aes_encrypt(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    if (!m_sec || !m_sec->aes || !crypt_data || crypt_len <= 0) {
        ESP_LOGE(BLUFI_TAG, "Invalid parameters for AES encryption");
        return -ESP_ERR_INVALID_ARG;
    }

    size_t iv_offset = 0;
    uint8_t iv0[16];
    memcpy(iv0, m_sec->iv, 16);
    iv0[0] = iv8;
    int ret = mbedtls_aes_crypt_cfb128(m_sec->aes, MBEDTLS_AES_ENCRYPT, crypt_len, &iv_offset, iv0,
                                       crypt_data, crypt_data);

    if (ret == 0) {
        return crypt_len;
    } else {
        ESP_LOGE(BLUFI_TAG, "AES encrypt failed: %d", ret);
        return ret;
    }
}

int Blufi::_aes_decrypt(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    if (!m_sec || !m_sec->aes || !crypt_data || crypt_len < 0) {
        ESP_LOGE(BLUFI_TAG, "Invalid parameters for AES decryption %p %p %d", m_sec->aes,
                 crypt_data, crypt_len);
        return -ESP_ERR_INVALID_ARG;
    }

    size_t iv_offset = 0;
    uint8_t iv0[16];
    memcpy(iv0, m_sec->iv, 16);
    iv0[0] = iv8;
    int ret = mbedtls_aes_crypt_cfb128(m_sec->aes, MBEDTLS_AES_DECRYPT, crypt_len, &iv_offset, iv0,
                                       crypt_data, crypt_data);
    if (ret != 0) {
        ESP_LOGE(BLUFI_TAG, "AES decrypt failed: %d", ret);
        return ret;
    } else {
        return crypt_len;
    }
}

uint16_t Blufi::_crc_checksum(uint8_t iv8, uint8_t* data, int len) {
    return esp_crc16_be(0, data, len);
}

int Blufi::_get_softap_conn_num() {
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized() || !wifi.IsConfigMode()) {
        return 0;
    }

    wifi_sta_list_t sta_list{};
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return sta_list.num;
    }
    return 0;
}

void Blufi::SuspendWifiProvisioning() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (session_.IsActive()) session_.Activate(BlufiRole::kControlOnly);
    std::lock_guard<std::mutex> scan_lock(scan_mutex_);
    scan_ticket_ = 0;
    m_scan_in_progress = false;
    m_ap_records.clear();
    // Saved SSIDs and the persistent BLE control connection remain intact.
}

bool Blufi::RunWithoutWifiProvisioning(const std::function<bool()>& operation) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return !session_.AllowsProvisioning() && operation && operation();
}

uint64_t Blufi::AcquireControlLease() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (control_lease_ || shutdown_failed_ || !IsControlReady() ||
        session_.Role() != BlufiRole::kControlOnly || next_control_lease_ == UINT64_MAX) return 0;
    control_lease_ = ++next_control_lease_;
    return control_lease_;
}

bool Blufi::ReleaseControlLease(uint64_t lease) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!lease || lease != control_lease_) return false;
    control_lease_ = 0;
    return true;
}

void Blufi::start_wifi_scan() {
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    // BTC must not wait for an owner that is awaiting host shutdown on BTC.
    std::unique_lock<std::mutex> lock(lifecycle_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        ESP_LOGW(BLUFI_TAG, "WiFi scan rejected while the BLE lifecycle is busy");
        return;
    }
#else
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
#endif
    GetWatchWifiGate().Run([this]() {
        StartWifiScanAllowed();
        return true;
    });
}

void Blufi::StartWifiScanAllowed() {
    const auto peer = CaptureControlPeer();
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    if (!peer) return;
#endif
    if (!session_.AllowsProvisioning() || session_.IsConnecting()) {
        return;
    }
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized()) {
        WifiManagerConfig config;
        config.ssid_prefix = session_.KeepsControl() ? "Audream" : "Xiaozhi";
        if (!wifi.Initialize(config)) {
            ESP_LOGE(BLUFI_TAG, "WiFi scan rejected: initialization failed");
            return;
        }
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t result = esp_wifi_get_mode(&mode);
    if (result != ESP_OK || mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        ESP_LOGW(BLUFI_TAG, "WiFi scan rejected: access point active or mode unavailable");
        return;
    }
    wifi.StopStation();
    if (mode == WIFI_MODE_NULL) {
        result = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (result == ESP_OK) {
        result = esp_wifi_start();
    }
    if (result != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "WiFi scan startup failed: %s", esp_err_to_name(result));
        return;
    }
    std::lock_guard<std::mutex> lock(scan_mutex_);
    if (m_scan_in_progress) {
        return;
    }
    // Register once for both STA and provisioning scans; never change AP mode.
    if (scan_event_instance_ == nullptr) {
        result = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
            &Blufi::_wifi_scan_event_handler, this, &scan_event_instance_);
        if (result != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "WiFi scan callback registration failed: %s", esp_err_to_name(result));
            return;
        }
    }
    scan_ticket_ = session_.Snapshot();
    scan_peer_ = peer;
    m_scan_in_progress = true;
    result = esp_wifi_scan_start(nullptr, false);
    if (result != ESP_OK) {
        m_scan_in_progress = false;
        ESP_LOGE(BLUFI_TAG, "WiFi scan failed: %s", esp_err_to_name(result));
    }
}

void Blufi::_send_wifi_list() {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    if (!session_.AllowsProvisioning() || !session_.IsCurrent(scan_ticket_) || !IsControlPeerCurrent(scan_peer_)) {
        m_ap_records.clear();
        return;
    }

    ESP_LOGI(BLUFI_TAG, "Sending WiFi list with %u APs", static_cast<unsigned>(m_ap_records.size()));

    std::vector<esp_blufi_ap_record_t> blufi_ap_list;
    for (const auto& ap : m_ap_records) {
        esp_blufi_ap_record_t blufi_ap;
        memset(&blufi_ap, 0, sizeof(blufi_ap));
        memcpy(blufi_ap.ssid, ap.ssid, std::min((size_t)32, sizeof(ap.ssid)));
        blufi_ap.rssi = ap.rssi;
        blufi_ap_list.push_back(blufi_ap);
    }

#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    audream_blufi_send_wifi_list_for_lease(scan_peer_.authorization, blufi_ap_list.size(), blufi_ap_list.data());
#else
    esp_blufi_send_wifi_list(blufi_ap_list.size(), blufi_ap_list.data());
#endif

    m_ap_records.clear();
}

void Blufi::_wifi_scan_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                     void* event_data) {
    Blufi* self = static_cast<Blufi*>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        {
            std::lock_guard<std::mutex> lock(self->scan_mutex_);
            if (!self->m_scan_in_progress) {
                return;
            }
            self->m_scan_in_progress = false;
            if (!self->session_.IsCurrent(self->scan_ticket_)) {
                return;
            }
            uint16_t ap_num = 0;
            esp_wifi_scan_get_ap_num(&ap_num);
            ap_num = std::min<uint16_t>(ap_num, 32);
            self->m_ap_records.resize(ap_num);
            if (ap_num != 0 && esp_wifi_scan_get_ap_records(&ap_num, self->m_ap_records.data()) != ESP_OK) {
                ap_num = 0;
            }
            self->m_ap_records.resize(ap_num);
        }
        self->_send_wifi_list();
    }
}

struct Blufi::ConnectionRequest {
    Blufi* owner;
    BlufiSessionState::Ticket ticket = 0;
    std::string ssid;
    std::string password;
    ControlTicket peer;
};

void Blufi::ConnectToAccessPoint(ConnectionRequest& request) {
    auto& wifi = WifiManager::GetInstance();
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!session_.IsCurrent(request.ticket) || !IsControlPeerCurrent(request.peer)) {
            session_.CompleteConnection(request.ticket, false);
            return;
        }
        if (!wifi.IsInitialized()) {
            WifiManagerConfig config;
            config.ssid_prefix = session_.KeepsControl() ? "Audream" : "Xiaozhi";
            if (!GetWatchWifiGate().Run([&]() { return wifi.Initialize(config); })) {
                session_.CompleteConnection(request.ticket, false);
                SendWifiReport(request.peer, WIFI_MODE_STA, ESP_BLUFI_STA_CONN_FAIL, 0, nullptr);
                ESP_LOGE(BLUFI_TAG, "Failed to initialize WiFi for provisioning");
                return;
            }
        }
        wifi_mode_t current_mode = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&current_mode) != ESP_OK ||
            current_mode == WIFI_MODE_AP || current_mode == WIFI_MODE_APSTA) {
            session_.CompleteConnection(request.ticket, false);
            SendWifiReport(request.peer, WIFI_MODE_STA, ESP_BLUFI_STA_CONN_FAIL, 0, nullptr);
            ESP_LOGW(BLUFI_TAG, "WiFi provisioning rejected: access point active or mode unavailable");
            return;
        }
        if (!GetWatchWifiGate().Run([&]() {
            if (!IsControlPeerCurrent(request.peer)) return false;
            SsidManager::GetInstance().AddSsid(request.ssid, request.password);
            {
                std::lock_guard<std::mutex> scan_lock(scan_mutex_);
                m_scan_in_progress = false;
                scan_ticket_ = 0;
            }
            esp_wifi_scan_stop();
            if (wifi.IsConfigMode()) wifi.StopConfigAp();
            wifi.StopStation();
            wifi.StartStation();
            return true;
        })) {
            session_.CompleteConnection(request.ticket, false);
            SendWifiReport(request.peer, WIFI_MODE_STA, ESP_BLUFI_STA_CONN_FAIL, 0, nullptr);
            return;
        }
    }

    constexpr int kConnectTimeoutMs = 10000;
    int waited_ms = 0;
    while (waited_ms < kConnectTimeoutMs && GetWatchWifiGate().Enabled() && session_.IsCurrent(request.ticket) &&
           IsControlPeerCurrent(request.peer) &&
           (!wifi.IsConnected() || wifi.GetSsid() != request.ssid)) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited_ms += 200;
    }

    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!session_.IsCurrent(request.ticket) || !IsControlPeerCurrent(request.peer)) {
        session_.CompleteConnection(request.ticket, false);
        ESP_LOGI(BLUFI_TAG, "Discarded stale WiFi provisioning result");
        return;
    }
    // Auto-reconnect to another saved SSID is not success for this request.
    const bool connected = GetWatchWifiGate().Enabled() && wifi.IsConnected() && wifi.GetSsid() == request.ssid;
    if (!connected) {
        GetWatchWifiGate().Run([&]() { wifi.StopStation(); return true; });
    }
    if (!session_.CompleteConnection(request.ticket, connected)) {
        return;
    }
    esp_blufi_extra_info_t info{};
    info.sta_ssid = reinterpret_cast<uint8_t*>(request.ssid.data());
    info.sta_ssid_len = request.ssid.size();
    wifi_ap_record_t ap_info{};
    if (connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        std::memcpy(info.sta_bssid, ap_info.bssid, sizeof(info.sta_bssid));
        info.sta_bssid_set = true;
    }
    SendWifiReport(request.peer, GetWifiModeWithFallback(wifi),
        connected ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_CONN_FAIL,
        _get_softap_conn_num(), &info);
    ESP_LOGI(BLUFI_TAG, "WiFi provisioning finished: connected=%d keep_control=%d",
             connected, session_.KeepsControl());
    if (connected && !session_.KeepsControl()) {
        if (m_ble_is_connected) {
            esp_blufi_disconnect();
        } else {
            DeinitLocked();
        }
    }
}

void Blufi::DisconnectStation(const ConnectionRequest& request) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!session_.IsCurrent(request.ticket) || !IsControlPeerCurrent(request.peer)) {
        return;
    }
    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsInitialized()) {
        GetWatchWifiGate().Run([&]() { wifi.StopStation(); return true; });
    }
}

void Blufi::_handle_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
#if CONFIG_AUDREAM_WATCH_UI && defined(CONFIG_BT_BLUEDROID_ENABLED)
    HandleLinkPowerEvent(event, param);
#endif
    const auto peer = CaptureControlPeer();
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    switch (event) {
        case ESP_BLUFI_EVENT_INIT_FINISH:
        case ESP_BLUFI_EVENT_DEINIT_FINISH:
        case ESP_BLUFI_EVENT_BLE_CONNECT:
        case ESP_BLUFI_EVENT_BLE_DISCONNECT:
            break;
        default:
            if (!peer) {
                ESP_LOGW(BLUFI_TAG, "Rejected BLUFI event without secure control authorization: event=%d", event);
                return;
            }
    }
#endif
    if (!session_.AllowsProvisioning() || !GetWatchWifiGate().Enabled() || GetWatchWifiGate().TransferActive()) {
        switch (event) {
            case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
            case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
            case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
            case ESP_BLUFI_EVENT_RECV_STA_BSSID:
            case ESP_BLUFI_EVENT_RECV_STA_SSID:
            case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
            case ESP_BLUFI_EVENT_GET_WIFI_LIST:
                ESP_LOGW(BLUFI_TAG, "Ignoring WiFi provisioning event in control-only mode");
                return;
            default:
                break;
        }
    }

    switch (event) {
        case ESP_BLUFI_EVENT_INIT_FINISH:
            HandleProfileInitialized(param && param->init_finish.state == ESP_BLUFI_INIT_OK);
            break;
        case ESP_BLUFI_EVENT_DEINIT_FINISH:
            control_session_.Reset();
            ESP_LOGI(BLUFI_TAG, "BLUFI deinit finish");
            break;
        case ESP_BLUFI_EVENT_BLE_CONNECT:
            control_session_.Connected(param->connect.conn_id, param->connect.remote_bda);
            ESP_LOGI(BLUFI_TAG, "BLUFI ble connect");
            m_ble_is_connected = true;
            session_.InvalidateRequests();
            {
                std::lock_guard<std::mutex> lock(credentials_mutex_);
                m_sta_config = {};
                m_ssid_valid = false;
                m_password_valid = true;
            }
            esp_blufi_adv_stop();
            _security_init();
            break;
        case ESP_BLUFI_EVENT_BLE_DISCONNECT:
            control_session_.Reset();
            ESP_LOGI(BLUFI_TAG, "BLUFI ble disconnect");
            m_ble_is_connected = false;
            session_.InvalidateRequests();
            _security_deinit();
            if (!session_.IsActive()) {
                break;
            }
            if (session_.KeepsControl() || !session_.IsProvisioned()) {
                StartAudreamBluFiAdvertising();
            } else {
                esp_blufi_adv_stop();
                struct ShutdownRequest {
                    Blufi* owner;
                    BlufiSessionState::Ticket ticket;
                };
                auto* request = new (std::nothrow) ShutdownRequest{this, session_.Snapshot()};
                if (request != nullptr) {
                    if (xTaskCreate(
                        [](void* ctx) {
                            std::unique_ptr<ShutdownRequest> request(static_cast<ShutdownRequest*>(ctx));
                            request->owner->DeinitCompletedProvisioning(request->ticket);
                            request.reset();
                            vTaskDelete(nullptr);
                        },
                        "blufi_deinit", 4096, request, 5, nullptr) != pdPASS) {
                        delete request;
                        ESP_LOGE(BLUFI_TAG, "Failed to schedule BLUFI shutdown");
                    }
                } else {
                    ESP_LOGE(BLUFI_TAG, "Failed to allocate BLUFI shutdown request");
                }
            }
            break;
        case ESP_BLUFI_EVENT_SET_WIFI_OPMODE: {
            ESP_LOGI(BLUFI_TAG, "BLUFI Set WIFI opmode %d", param->wifi_mode.op_mode);
            auto& wifi_manager = WifiManager::GetInstance();
            WifiManagerConfig config;
            config.ssid_prefix = session_.KeepsControl() ? "Audream" : "Xiaozhi";
            if (!wifi_manager.IsInitialized() &&
                !GetWatchWifiGate().Run([&]() { return wifi_manager.Initialize(config); })) {
                ESP_LOGE(BLUFI_TAG, "Failed to initialize WifiManager for opmode change");
                break;
            }
            wifi_mode_t current_mode = WIFI_MODE_NULL;
            if (esp_wifi_get_mode(&current_mode) != ESP_OK ||
                current_mode == WIFI_MODE_AP || current_mode == WIFI_MODE_APSTA) {
                ESP_LOGW(BLUFI_TAG, "WiFi mode change rejected: access point active or mode unavailable");
                break;
            }
            switch (param->wifi_mode.op_mode) {
                case WIFI_MODE_STA:
                    // Do not auto-connect before the phone supplies credentials.
                    // WifiStation would otherwise consume the BLUFI scan results.
                    if (session_.IsConnecting() || wifi_manager.IsConfigMode()) {
                        ESP_LOGW(BLUFI_TAG, "WiFi mode change rejected while network is busy");
                        break;
                    }
                    if (!GetWatchWifiGate().Run([&]() {
                        wifi_manager.StopStation();
                        return esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK && esp_wifi_start() == ESP_OK;
                    })) {
                        ESP_LOGE(BLUFI_TAG, "Failed to prepare WiFi station for provisioning");
                    }
                    break;
                case WIFI_MODE_AP:
                case WIFI_MODE_APSTA:
                    ESP_LOGW(BLUFI_TAG, "AP mode changes are not permitted during BLUFI provisioning");
                    break;
                default:
                    ESP_LOGW(BLUFI_TAG, "Unsupported WiFi provisioning mode");
                    break;
            }
            break;
        }
        case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP: {
            auto request = std::unique_ptr<ConnectionRequest>(new (std::nothrow) ConnectionRequest{this, 0, {}, {}, peer});
            if (!request) {
                ESP_LOGE(BLUFI_TAG, "Failed to allocate WiFi provisioning request");
                break;
            }
            {
                std::lock_guard<std::mutex> lock(credentials_mutex_);
                if (!m_ssid_valid || !m_password_valid) {
                    ESP_LOGW(BLUFI_TAG, "WiFi provisioning rejected: invalid credentials");
                    SendWifiReport(peer, WIFI_MODE_STA, ESP_BLUFI_STA_CONN_FAIL, 0, nullptr);
                    break;
                }
                request->ssid = ReadWifiCredentialField(m_sta_config.sta.ssid);
                request->password = ReadWifiCredentialField(m_sta_config.sta.password);
            }
            request->ticket = session_.BeginConnection();
            if (request->ticket == 0) {
                ESP_LOGW(BLUFI_TAG, "WiFi provisioning request ignored: inactive or already connecting");
                break;
            }
            m_sta_ssid_len = request->ssid.size();
            std::memcpy(m_sta_ssid, request->ssid.data(), m_sta_ssid_len);
            m_sta_conn_info = {};
            m_sta_conn_info.sta_ssid = m_sta_ssid;
            m_sta_conn_info.sta_ssid_len = m_sta_ssid_len;
            const auto ticket = request->ticket;
            auto* context = request.release();
            if (xTaskCreate([](void* ctx) {
                    std::unique_ptr<ConnectionRequest> request(static_cast<ConnectionRequest*>(ctx));
                    request->owner->ConnectToAccessPoint(*request);
                    request.reset();
                    vTaskDelete(nullptr);
                }, "blufi_wifi_conn", 4096, context, 5, nullptr) != pdPASS) {
                delete context;
                session_.CompleteConnection(ticket, false);
                SendWifiReport(peer, WIFI_MODE_STA, ESP_BLUFI_STA_CONN_FAIL, 0, nullptr);
                ESP_LOGE(BLUFI_TAG, "Failed to start WiFi provisioning task");
            }
            break;
        }
        case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP: {
            ESP_LOGI(BLUFI_TAG, "BLUFI request wifi disconnect from AP");
            session_.InvalidateRequests();
            m_sta_connected = false;
            m_sta_got_ip = false;
            auto* request = new (std::nothrow) ConnectionRequest{this, session_.Snapshot(), {}, {}, peer};
            if (request == nullptr) {
                ESP_LOGE(BLUFI_TAG, "Failed to allocate WiFi disconnect request");
                break;
            }
            if (xTaskCreate([](void* ctx) {
                    std::unique_ptr<ConnectionRequest> request(static_cast<ConnectionRequest*>(ctx));
                    request->owner->DisconnectStation(*request);
                    request.reset();
                    vTaskDelete(nullptr);
                }, "blufi_wifi_stop", 4096, request, 5, nullptr) != pdPASS) {
                delete request;
                ESP_LOGE(BLUFI_TAG, "Failed to schedule WiFi disconnect");
            }
            break;
        }
        case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA: {
            char opcode_buffer[8] = {};
            ESP_LOGI(
                BLUFI_TAG,
                "Received Audream custom data: %" PRIu32 " bytes opcode=%s request_id=%u",
                param->custom_data.data_len,
                FormatAudreamCustomOpcode(
                    param->custom_data.data,
                    param->custom_data.data_len,
                    opcode_buffer,
                    sizeof(opcode_buffer)
                ),
                static_cast<unsigned>(
                    ReadAudreamCustomRequestId(
                        param->custom_data.data,
                        param->custom_data.data_len
                    )
                )
            );
            QueueAudreamCustomData(param->custom_data.data, param->custom_data.data_len);
            break;
        }
        case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
            auto& wifi = WifiManager::GetInstance();
            wifi_mode_t mode = GetWifiModeWithFallback(wifi);
            const int softap_conn_num = _get_softap_conn_num();

            if (wifi.IsInitialized() && wifi.IsConnected()) {
                m_sta_connected = true;
                m_sta_got_ip = true;

                auto current_ssid = wifi.GetSsid();
                if (!current_ssid.empty()) {
                    m_sta_ssid_len =
                        static_cast<int>(std::min(current_ssid.size(), sizeof(m_sta_ssid)));
                    memcpy(m_sta_ssid, current_ssid.c_str(), m_sta_ssid_len);
                }

                esp_blufi_extra_info_t info;
                memset(&info, 0, sizeof(esp_blufi_extra_info_t));
                memcpy(info.sta_bssid, m_sta_bssid, 6);
                info.sta_ssid = m_sta_ssid;
                info.sta_ssid_len = m_sta_ssid_len;
                SendWifiReport(peer, mode, ESP_BLUFI_STA_CONN_SUCCESS, softap_conn_num,
                                                &info);
            } else if (session_.IsConnecting()) {
                SendWifiReport(peer, mode, ESP_BLUFI_STA_CONNECTING, softap_conn_num,
                                                &m_sta_conn_info);
            } else {
                SendWifiReport(peer, mode, ESP_BLUFI_STA_CONN_FAIL, softap_conn_num,
                                                &m_sta_conn_info);
            }
            ESP_LOGI(BLUFI_TAG, "BLUFI get wifi status");
            break;
        }
        case ESP_BLUFI_EVENT_RECV_STA_BSSID: {
            std::lock_guard<std::mutex> lock(credentials_mutex_);
            memcpy(m_sta_config.sta.bssid, param->sta_bssid.bssid, 6);
            m_sta_config.sta.bssid_set = true;
            ESP_LOGI(BLUFI_TAG, "Recv STA BSSID");
            break;
        }
        case ESP_BLUFI_EVENT_RECV_STA_SSID: {
            std::lock_guard<std::mutex> lock(credentials_mutex_);
            m_ssid_valid = CopyWifiCredentialField(m_sta_config.sta.ssid,
                param->sta_ssid.ssid, param->sta_ssid.ssid_len, false);
            ESP_LOGI(BLUFI_TAG, "Received WiFi SSID: bytes=%u valid=%d",
                     static_cast<unsigned>(param->sta_ssid.ssid_len), m_ssid_valid);
            break;
        }
        case ESP_BLUFI_EVENT_RECV_STA_PASSWD: {
            std::lock_guard<std::mutex> lock(credentials_mutex_);
            m_password_valid = CopyWifiCredentialField(m_sta_config.sta.password,
                param->sta_passwd.passwd, param->sta_passwd.passwd_len, true);
            ESP_LOGI(BLUFI_TAG, "Received WiFi password: valid=%d", m_password_valid);
            break;
        }
        case ESP_BLUFI_EVENT_GET_WIFI_LIST: {
            start_wifi_scan();
            break;
        }
        default:
            ESP_LOGW(BLUFI_TAG, "Unhandled event: %d", event);
            break;
    }
}

void Blufi::_event_callback_trampoline(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
    GetInstance()._handle_event(event, param);
}

void Blufi::_negotiate_data_handler_trampoline(uint8_t* data, int len, uint8_t** output_data,
                                               int* output_len, bool* need_free) {
    GetInstance()._dh_negotiate_data_handler(data, len, output_data, output_len, need_free);
}

int Blufi::_encrypt_func_trampoline(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    return GetInstance()._aes_encrypt(iv8, crypt_data, crypt_len);
}

int Blufi::_decrypt_func_trampoline(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    return GetInstance()._aes_decrypt(iv8, crypt_data, crypt_len);
}

uint16_t Blufi::_checksum_func_trampoline(uint8_t iv8, uint8_t* data, int len) {
    return _crc_checksum(iv8, data, len);
}
