#include "storage/temporary_ap_transfer_service.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "storage/sd_card_storage.h"
#include "system/audream_power_manager.h"

namespace {

constexpr char kTag[] = "TempAPTransfer";
constexpr uint32_t kSessionExpirySeconds = 300;
constexpr size_t kHttpBufferSize = 2048;
constexpr size_t kMinimumInternalHeapBeforeWifi = 48 * 1024;

std::string RandomHex(size_t bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string value;
    value.reserve(bytes * 2);
    for (size_t index = 0; index < bytes; ++index) {
        const uint8_t byte = static_cast<uint8_t>(esp_random() & 0xFF);
        value.push_back(kHex[byte >> 4]);
        value.push_back(kHex[byte & 0x0F]);
    }
    return value;
}

std::string HexBytes(const uint8_t* bytes, size_t length) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string value;
    value.reserve(length * 2);
    for (size_t index = 0; index < length; ++index) {
        const uint8_t byte = bytes[index];
        value.push_back(kHex[byte >> 4]);
        value.push_back(kHex[byte & 0x0F]);
    }
    return value;
}

bool ReadDefaultMac(uint8_t* mac) {
    return mac != nullptr && esp_efuse_mac_get_default(mac) == ESP_OK;
}

std::string BuildStableSSID() {
    uint8_t mac[6] = {};
    if (ReadDefaultMac(mac)) {
        return "Audream-" + HexBytes(mac + 3, 3);
    }
    return "Audream-" + RandomHex(3);
}

std::string BuildPassword() {
    uint8_t mac[6] = {};
    if (ReadDefaultMac(mac)) {
        return "Audream" + HexBytes(mac, 6);
    }
    return "Audream" + RandomHex(6);
}

bool QueryValue(httpd_req_t* request, const char* key, char* output, size_t output_size) {
    char query[192] = {};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, key, output, output_size) == ESP_OK;
}

bool EnsureWifiDriverInitialized() {
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "Failed to initialize network interface: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "Failed to create default event loop: %s", esp_err_to_name(ret));
        return false;
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    config.nvs_enable = false;
    ret = esp_wifi_init(&config);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "Failed to initialize WiFi driver: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

}  // namespace

TemporaryAPTransferService& TemporaryAPTransferService::GetInstance() {
    static TemporaryAPTransferService instance;
    return instance;
}

TemporaryAPTransferSession TemporaryAPTransferService::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    StopLocked();

    session_ = {};
    session_.ssid = BuildStableSSID();
    session_.password = BuildPassword();
    session_.token = RandomHex(16);
    session_.expires_in_seconds = kSessionExpirySeconds;
    session_.url = "http://192.168.4.1/download?token=" + session_.token;

    if (!StartAccessPointLocked() || !StartHttpServerLocked() || !StartExpiryTimerLocked()) {
        StopLocked();
        return {};
    }

    session_.valid = true;
    AudreamPowerManager::GetInstance().SetTemporaryAPTransferActive(true);
    ESP_LOGI(
        kTag,
        "Temporary AP transfer batch started as %s",
        session_.ssid.c_str()
    );
    return session_;
}

void TemporaryAPTransferService::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    StopLocked();
}

bool TemporaryAPTransferService::Complete(
    const std::string& token
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsTokenValidLocked(token)) {
        ESP_LOGW(kTag, "Ignored temporary AP completion with an invalid token");
        return false;
    }
    ESP_LOGI(kTag, "Temporary AP transfer batch completed");
    StopLocked();
    return true;
}

void TemporaryAPTransferService::StopLocked(bool stop_expiry_timer) {
    if (stop_expiry_timer && expiry_timer_ != nullptr && esp_timer_is_active(expiry_timer_)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_stop(expiry_timer_));
    }
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    esp_wifi_stop();
    if (ap_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }
    session_ = {};
    expires_at_us_ = 0;
    AudreamPowerManager::GetInstance().SetTemporaryAPTransferActive(false);
}

void TemporaryAPTransferService::ExpiryTimerCallback(void* arg) {
    auto* service = static_cast<TemporaryAPTransferService*>(arg);
    if (service == nullptr) {
        return;
    }
    const BaseType_t created = xTaskCreate(
        [](void* task_arg) {
            static_cast<TemporaryAPTransferService*>(task_arg)->StopFromExpiry();
            vTaskDelete(nullptr);
        },
        "ap_expiry",
        4096,
        service,
        3,
        nullptr
    );
    if (created != pdPASS) {
        ESP_LOGE(kTag, "Failed to create temporary AP expiry task");
    }
}

void TemporaryAPTransferService::StopFromExpiry() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session_.valid) {
        return;
    }
    ESP_LOGI(kTag, "Temporary AP transfer batch expired");
    StopLocked(false);
}

bool TemporaryAPTransferService::IsActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_.valid && !IsExpiredLocked();
}

bool TemporaryAPTransferService::StartAccessPointLocked() {
    const size_t internal_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (internal_heap < kMinimumInternalHeapBeforeWifi) {
        ESP_LOGE(
            kTag,
            "Insufficient internal heap for temporary AP transfer: free=%u required=%u",
            static_cast<unsigned>(internal_heap),
            static_cast<unsigned>(kMinimumInternalHeapBeforeWifi)
        );
        return false;
    }

    if (!EnsureWifiDriverInitialized()) {
        return false;
    }

    ap_netif_ = esp_netif_create_default_wifi_ap();
    if (ap_netif_ == nullptr) {
        ESP_LOGE(kTag, "Failed to create AP network interface");
        return false;
    }

    esp_netif_ip_info_t ip_info = {};
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(ap_netif_));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_ip_info(ap_netif_, &ip_info));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(ap_netif_));

    wifi_config_t wifi_config = {};
    std::strncpy(
        reinterpret_cast<char*>(wifi_config.ap.ssid),
        session_.ssid.c_str(),
        sizeof(wifi_config.ap.ssid) - 1
    );
    std::strncpy(
        reinterpret_cast<char*>(wifi_config.ap.password),
        session_.password.c_str(),
        sizeof(wifi_config.ap.password) - 1
    );
    wifi_config.ap.ssid_len = session_.ssid.size();
    wifi_config.ap.max_connection = 1;
    wifi_config.ap.channel = 6;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to set WiFi AP mode: %s", esp_err_to_name(ret));
        return false;
    }
    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to set AP config: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(kTag, "Failed to start WiFi AP: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool TemporaryAPTransferService::StartHttpServerLocked() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 30;

    esp_err_t ret = httpd_start(&server_, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return false;
    }

    httpd_uri_t download_uri = {};
    download_uri.uri = "/download*";
    download_uri.method = HTTP_GET;
    download_uri.handler = &TemporaryAPTransferService::DownloadHandler;
    download_uri.user_ctx = this;
    ret = httpd_register_uri_handler(server_, &download_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to register download handler: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool TemporaryAPTransferService::StartExpiryTimerLocked() {
    if (expiry_timer_ == nullptr) {
        const esp_timer_create_args_t timer_args = {
            .callback = &TemporaryAPTransferService::ExpiryTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ap_transfer_expiry",
            .skip_unhandled_events = true,
        };
        const esp_err_t create_result = esp_timer_create(&timer_args, &expiry_timer_);
        if (create_result != ESP_OK) {
            ESP_LOGE(
                kTag,
                "Failed to create temporary AP expiry timer: %s",
                esp_err_to_name(create_result)
            );
            return false;
        }
    } else if (esp_timer_is_active(expiry_timer_)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_stop(expiry_timer_));
    }

    expires_at_us_ = esp_timer_get_time() +
        static_cast<int64_t>(kSessionExpirySeconds) * 1000000LL;
    const esp_err_t start_result = esp_timer_start_once(
        expiry_timer_,
        static_cast<uint64_t>(kSessionExpirySeconds) * 1000000ULL
    );
    if (start_result != ESP_OK) {
        ESP_LOGE(
            kTag,
            "Failed to start temporary AP expiry timer: %s",
            esp_err_to_name(start_result)
        );
        return false;
    }
    return true;
}

bool TemporaryAPTransferService::IsExpiredLocked() const {
    return expires_at_us_ <= 0 || esp_timer_get_time() > expires_at_us_;
}

bool TemporaryAPTransferService::IsTokenValidLocked(const std::string& token) const {
    return session_.valid && !IsExpiredLocked() && token == session_.token;
}

esp_err_t TemporaryAPTransferService::DownloadHandler(httpd_req_t* request) {
    auto* service = static_cast<TemporaryAPTransferService*>(request->user_ctx);
    char token[80] = {};
    char recording_id[80] = {};
    if (service == nullptr ||
        !QueryValue(request, "token", token, sizeof(token)) ||
        !QueryValue(request, "recording_id", recording_id, sizeof(recording_id))) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Missing token or recording_id");
        return ESP_FAIL;
    }

    {
        std::lock_guard<std::mutex> lock(service->mutex_);
        if (!service->IsTokenValidLocked(token)) {
            httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "Invalid token");
            return ESP_FAIL;
        }
        if (!service->StartExpiryTimerLocked()) {
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to extend session");
            return ESP_FAIL;
        }
    }

    std::string file_path;
    uint32_t size_bytes = 0;
    if (!SdCardStorage::GetInstance().GetRecordingFilePath(
            recording_id,
            file_path,
            size_bytes
        )) {
        httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Recording not found");
        return ESP_FAIL;
    }

    FILE* file = fopen(file_path.c_str(), "rb");
    if (file == nullptr) {
        httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Recording not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(request, "audio/ogg");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    char content_length[16] = {};
    std::snprintf(content_length, sizeof(content_length), "%lu", static_cast<unsigned long>(size_bytes));
    httpd_resp_set_hdr(request, "Content-Length", content_length);

    std::vector<char> buffer(kHttpBufferSize);
    esp_err_t result = ESP_OK;
    while (true) {
        const size_t read = fread(buffer.data(), 1, buffer.size(), file);
        if (read > 0) {
            result = httpd_resp_send_chunk(request, buffer.data(), read);
            if (result != ESP_OK) {
                break;
            }
        }
        if (read < buffer.size()) {
            break;
        }
    }
    fclose(file);
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, nullptr, 0);
    }
    return result;
}
