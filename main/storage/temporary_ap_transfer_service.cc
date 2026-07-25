#include "storage/temporary_ap_transfer_service.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <unistd.h>
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
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "storage/sd_card_storage.h"
#include "system/audream_power_manager.h"

namespace {

constexpr char kTag[] = "TempAPTransfer";
constexpr uint32_t kInactivityExpirySeconds = 45;
constexpr size_t kSdReadBufferSize = 4 * 1024;
constexpr size_t kMinimumSdReadBufferSize = 2 * 1024;
constexpr size_t kDownloadBufferSize = 16 * 1024;
constexpr size_t kDownloadBufferCount = 16;
constexpr size_t kMinimumDownloadBufferCount = 4;
constexpr size_t kMetadataHttpBufferSize = 4 * 1024;
constexpr uint32_t kDownloadReaderTaskStackSize = 3 * 1024;
constexpr UBaseType_t kDownloadReaderTaskPriority = 4;
constexpr TickType_t kDownloadPipelineWait = pdMS_TO_TICKS(100);
constexpr size_t kMinimumInternalHeapBeforeWifi = 48 * 1024;
constexpr int64_t kDownloadProgressIntervalUs = 5 * 1000 * 1000;

struct DownloadReadItem {
    uint16_t buffer_index = 0;
    size_t size = 0;
    bool end_of_file = false;
    int error = 0;
};

struct DownloadReadPipeline {
    int file = -1;
    char* buffer_block = nullptr;
    char* read_buffer = nullptr;
    char* send_buffer = nullptr;
    size_t read_buffer_size = 0;
    size_t buffer_size = kDownloadBufferSize;
    size_t buffer_count = 0;
    QueueHandle_t free_buffers = nullptr;
    QueueHandle_t ready_buffers = nullptr;
    SemaphoreHandle_t reader_done = nullptr;
    std::atomic<bool> cancelled{false};
    int64_t read_time_us = 0;
};

void DownloadReaderTask(void* arg) {
    auto* pipeline = static_cast<DownloadReadPipeline*>(arg);
    while (!pipeline->cancelled.load(std::memory_order_relaxed)) {
        uint16_t buffer_index = 0;
        if (xQueueReceive(
                pipeline->free_buffers,
                &buffer_index,
                kDownloadPipelineWait
            ) != pdTRUE) {
            continue;
        }

        char* buffer = pipeline->buffer_block +
            buffer_index * pipeline->buffer_size;
        size_t buffered_bytes = 0;
        bool end_of_file = false;
        int read_error = 0;
        while (buffered_bytes < pipeline->buffer_size) {
            const size_t requested_bytes = std::min(
                pipeline->read_buffer_size,
                pipeline->buffer_size - buffered_bytes
            );
            const int64_t read_started_at_us = esp_timer_get_time();
            errno = 0;
            const ssize_t bytes_read = read(
                pipeline->file,
                pipeline->read_buffer,
                requested_bytes
            );
            read_error = bytes_read < 0 ? errno : 0;
            pipeline->read_time_us += esp_timer_get_time() - read_started_at_us;
            if (bytes_read < 0) {
                break;
            }
            if (bytes_read > 0) {
                const size_t read_size = static_cast<size_t>(bytes_read);
                std::memcpy(
                    buffer + buffered_bytes,
                    pipeline->read_buffer,
                    read_size
                );
                buffered_bytes += read_size;
            }
            if (static_cast<size_t>(bytes_read) < requested_bytes) {
                end_of_file = true;
                break;
            }
        }

        DownloadReadItem item = {
            .buffer_index = buffer_index,
            .size = buffered_bytes,
            .end_of_file = end_of_file,
            .error = read_error,
        };
        bool queued = false;
        while (!pipeline->cancelled.load(std::memory_order_relaxed)) {
            if (xQueueSend(
                    pipeline->ready_buffers,
                    &item,
                    kDownloadPipelineWait
                ) == pdTRUE) {
                queued = true;
                break;
            }
        }
        if (!queued || item.error != 0 || item.end_of_file) {
            break;
        }
    }

    xSemaphoreGive(pipeline->reader_done);
    vTaskDelete(nullptr);
}

void StopDownloadReadPipeline(
    DownloadReadPipeline& pipeline,
    bool reader_started
) {
    pipeline.cancelled.store(true, std::memory_order_relaxed);
    if (reader_started && pipeline.reader_done != nullptr) {
        xSemaphoreTake(pipeline.reader_done, portMAX_DELAY);
    }
    if (pipeline.free_buffers != nullptr) {
        vQueueDelete(pipeline.free_buffers);
        pipeline.free_buffers = nullptr;
    }
    if (pipeline.ready_buffers != nullptr) {
        vQueueDelete(pipeline.ready_buffers);
        pipeline.ready_buffers = nullptr;
    }
    if (pipeline.reader_done != nullptr) {
        vSemaphoreDelete(pipeline.reader_done);
        pipeline.reader_done = nullptr;
    }
    if (pipeline.buffer_block != nullptr) {
        heap_caps_free(pipeline.buffer_block);
        pipeline.buffer_block = nullptr;
    }
    if (pipeline.read_buffer != nullptr) {
        heap_caps_free(pipeline.read_buffer);
        pipeline.read_buffer = nullptr;
    }
    if (pipeline.send_buffer != nullptr) {
        heap_caps_free(pipeline.send_buffer);
        pipeline.send_buffer = nullptr;
    }
}

int ConnectedStationRssi() {
    wifi_sta_list_t stations = {};
    if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK || stations.num == 0) {
        return 0;
    }
    return stations.sta[0].rssi;
}

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

bool SendAll(
    httpd_req_t* request,
    const char* data,
    size_t length,
    size_t& sent_bytes,
    int& send_result,
    int& send_errno
) {
    sent_bytes = 0;
    send_result = 0;
    send_errno = 0;
    while (sent_bytes < length) {
        send_result = httpd_send(
            request,
            data + sent_bytes,
            length - sent_bytes
        );
        if (send_result <= 0) {
            send_errno = errno;
            return false;
        }
        sent_bytes += static_cast<size_t>(send_result);
    }
    return true;
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

void StopWifiDriver() {
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK &&
        ret != ESP_ERR_WIFI_NOT_INIT &&
        ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(kTag, "Failed to stop WiFi driver: %s", esp_err_to_name(ret));
    }
}

void DeinitializeWifiDriver() {
    const esp_err_t ret = esp_wifi_deinit();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(kTag, "Failed to deinitialize WiFi driver: %s", esp_err_to_name(ret));
    }
}

}  // namespace

TemporaryAPTransferService& TemporaryAPTransferService::GetInstance() {
    static TemporaryAPTransferService instance;
    return instance;
}

TemporaryAPTransferSession TemporaryAPTransferService::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_pending_) {
        ESP_LOGW(kTag, "Temporary AP transfer start deferred while the previous batch is stopping");
        return {};
    }
    if (session_.valid && !IsExpiredLocked()) {
        if (!StartExpiryTimerLocked()) {
            StopLocked();
            return {};
        }
        ESP_LOGI(
            kTag,
            "Reusing active temporary AP transfer batch as %s",
            session_.ssid.c_str()
        );
        return session_;
    }

    StopLocked();

    session_ = {};
    session_.ssid = BuildStableSSID();
    session_.password = BuildPassword();
    session_.token = RandomHex(16);
    session_.expires_in_seconds = kInactivityExpirySeconds;
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
    struct StopContext {
        TemporaryAPTransferService* service = nullptr;
        httpd_handle_t server = nullptr;
    };

    auto* context = new (std::nothrow) StopContext();
    if (context == nullptr) {
        ESP_LOGE(kTag, "Failed to allocate temporary AP stop context");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsTokenValidLocked(token)) {
            ESP_LOGW(kTag, "Ignored temporary AP completion with an invalid token");
            delete context;
            return false;
        }
        if (stop_pending_) {
            delete context;
            return true;
        }
        stop_pending_ = true;
        if (expiry_timer_ != nullptr && esp_timer_is_active(expiry_timer_)) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_stop(expiry_timer_));
        }
        context->service = this;
        context->server = server_;
        server_ = nullptr;
    }

    const BaseType_t created = xTaskCreate(
        [](void* task_arg) {
            auto* stop_context = static_cast<StopContext*>(task_arg);
            if (stop_context->server != nullptr) {
                ESP_LOGI(kTag, "Stopping temporary AP HTTP server outside the service lock");
                const esp_err_t stop_result = httpd_stop(stop_context->server);
                ESP_LOGI(
                    kTag,
                    "Temporary AP HTTP server stop finished: %s",
                    esp_err_to_name(stop_result)
                );
            }
            {
                std::lock_guard<std::mutex> lock(stop_context->service->mutex_);
                stop_context->service->StopLocked(false);
            }
            delete stop_context;
            vTaskDelete(nullptr);
        },
        "ap_stop",
        4096,
        context,
        3,
        nullptr
    );
    if (created != pdPASS) {
        std::lock_guard<std::mutex> lock(mutex_);
        server_ = context->server;
        stop_pending_ = false;
        StartExpiryTimerLocked();
        delete context;
        ESP_LOGE(kTag, "Failed to create temporary AP stop task");
        return false;
    }
    ESP_LOGI(kTag, "Temporary AP transfer batch completion scheduled");
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
    StopWifiDriver();
    if (ap_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }
    DeinitializeWifiDriver();
    session_ = {};
    expires_at_us_ = 0;
    active_downloads_ = 0;
    stop_pending_ = false;
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
    if (active_downloads_ > 0) {
        ESP_LOGI(kTag, "Temporary AP transfer expiry delayed because a download is active");
        StartExpiryTimerLocked();
        return;
    }
    ESP_LOGI(kTag, "Temporary AP transfer batch expired after inactivity");
    StopLocked(false);
}

void TemporaryAPTransferService::FinishDownload() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_downloads_ > 0) {
        active_downloads_--;
    }
    if (session_.valid && !stop_pending_) {
        StartExpiryTimerLocked();
    }
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

    httpd_uri_t metadata_uri = {};
    metadata_uri.uri = "/metadata*";
    metadata_uri.method = HTTP_GET;
    metadata_uri.handler = &TemporaryAPTransferService::MetadataHandler;
    metadata_uri.user_ctx = this;
    ret = httpd_register_uri_handler(server_, &metadata_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to register metadata handler: %s", esp_err_to_name(ret));
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
        static_cast<int64_t>(kInactivityExpirySeconds) * 1000000LL;
    const esp_err_t start_result = esp_timer_start_once(
        expiry_timer_,
        static_cast<uint64_t>(kInactivityExpirySeconds) * 1000000ULL
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
        service->active_downloads_++;
        if (!service->StartExpiryTimerLocked()) {
            service->active_downloads_--;
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to extend session");
            return ESP_FAIL;
        }
    }

    struct DownloadGuard {
        TemporaryAPTransferService* service = nullptr;
        ~DownloadGuard() {
            if (service != nullptr) {
                service->FinishDownload();
            }
        }
    } download_guard{service};

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

    const int file = open(file_path.c_str(), O_RDONLY);
    if (file < 0) {
        httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Recording not found");
        return ESP_FAIL;
    }

    DownloadReadPipeline pipeline;
    pipeline.file = file;
    pipeline.read_buffer_size = kSdReadBufferSize;
    while ((pipeline.read_buffer == nullptr || pipeline.send_buffer == nullptr) &&
           pipeline.read_buffer_size >= kMinimumSdReadBufferSize) {
        pipeline.read_buffer = static_cast<char*>(heap_caps_malloc(
            pipeline.read_buffer_size,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT
        ));
        pipeline.send_buffer = static_cast<char*>(heap_caps_malloc(
            pipeline.read_buffer_size,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        ));
        if (pipeline.read_buffer == nullptr || pipeline.send_buffer == nullptr) {
            if (pipeline.read_buffer != nullptr) {
                heap_caps_free(pipeline.read_buffer);
                pipeline.read_buffer = nullptr;
            }
            if (pipeline.send_buffer != nullptr) {
                heap_caps_free(pipeline.send_buffer);
                pipeline.send_buffer = nullptr;
            }
            pipeline.read_buffer_size /= 2;
        }
    }
    pipeline.buffer_count = kDownloadBufferCount;
    while (pipeline.buffer_block == nullptr &&
           pipeline.buffer_count >= kMinimumDownloadBufferCount) {
        pipeline.buffer_block = static_cast<char*>(heap_caps_malloc(
            pipeline.buffer_size * pipeline.buffer_count,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        ));
        if (pipeline.buffer_block == nullptr) {
            pipeline.buffer_count /= 2;
        }
    }
    if (pipeline.read_buffer == nullptr ||
        pipeline.send_buffer == nullptr ||
        pipeline.buffer_block == nullptr) {
        StopDownloadReadPipeline(pipeline, false);
        close(file);
        httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Insufficient transfer buffer memory"
        );
        ESP_LOGE(
            kTag,
            "Failed to allocate recording read-ahead buffers: free_internal=%u free_spiram=%u",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM))
        );
        return ESP_ERR_NO_MEM;
    }

    pipeline.free_buffers = xQueueCreate(
        pipeline.buffer_count,
        sizeof(uint16_t)
    );
    pipeline.ready_buffers = xQueueCreate(
        pipeline.buffer_count,
        sizeof(DownloadReadItem)
    );
    pipeline.reader_done = xSemaphoreCreateBinary();
    if (pipeline.free_buffers == nullptr ||
        pipeline.ready_buffers == nullptr ||
        pipeline.reader_done == nullptr) {
        StopDownloadReadPipeline(pipeline, false);
        close(file);
        httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to create transfer pipeline"
        );
        ESP_LOGE(kTag, "Failed to create recording download pipeline resources");
        return ESP_ERR_NO_MEM;
    }
    for (uint16_t index = 0; index < pipeline.buffer_count; ++index) {
        xQueueSend(pipeline.free_buffers, &index, 0);
    }

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    const BaseType_t reader_created = xTaskCreatePinnedToCore(
        DownloadReaderTask,
        "ap_sd_reader",
        kDownloadReaderTaskStackSize,
        &pipeline,
        kDownloadReaderTaskPriority,
        nullptr,
        1
    );
#else
    const BaseType_t reader_created = xTaskCreate(
        DownloadReaderTask,
        "ap_sd_reader",
        kDownloadReaderTaskStackSize,
        &pipeline,
        kDownloadReaderTaskPriority,
        nullptr
    );
#endif
    const bool reader_started = reader_created == pdPASS;
    if (!reader_started) {
        StopDownloadReadPipeline(pipeline, false);
        close(file);
        httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to start transfer pipeline"
        );
        ESP_LOGE(kTag, "Failed to create recording download reader task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(
        kTag,
        "Recording download buffer ready: mode=read_ahead_internal_send buffers=%u bytes_each=%u io_bytes=%u free_internal=%u free_spiram=%u",
        static_cast<unsigned>(pipeline.buffer_count),
        static_cast<unsigned>(pipeline.buffer_size),
        static_cast<unsigned>(pipeline.read_buffer_size),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM))
    );

    char response_headers[256] = {};
    const int response_header_length = std::snprintf(
        response_headers,
        sizeof(response_headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: audio/ogg\r\n"
        "Content-Length: %lu\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n",
        static_cast<unsigned long>(size_bytes)
    );
    if (response_header_length <= 0 ||
        static_cast<size_t>(response_header_length) >= sizeof(response_headers)) {
        StopDownloadReadPipeline(pipeline, reader_started);
        close(file);
        ESP_LOGE(kTag, "Failed to build recording download response headers");
        return ESP_FAIL;
    }

    size_t sent_header_bytes = 0;
    int header_send_result = 0;
    int header_send_errno = 0;
    if (!SendAll(
            request,
            response_headers,
            static_cast<size_t>(response_header_length),
            sent_header_bytes,
            header_send_result,
            header_send_errno
        )) {
        StopDownloadReadPipeline(pipeline, reader_started);
        close(file);
        ESP_LOGW(
            kTag,
            "Failed to send recording download response headers: result=%d errno=%d error=%s sent=%u total=%d",
            header_send_result,
            header_send_errno,
            std::strerror(header_send_errno),
            static_cast<unsigned>(sent_header_bytes),
            response_header_length
        );
        return ESP_ERR_HTTPD_RESP_SEND;
    }

    esp_err_t result = ESP_OK;
    uint64_t sent_bytes = 0;
    int64_t send_time_us = 0;
    int64_t max_send_time_us = 0;
    const int64_t started_at_us = esp_timer_get_time();
    int64_t progress_started_at_us = started_at_us;
    uint64_t progress_started_at_bytes = 0;
    while (true) {
        DownloadReadItem item;
        if (xQueueReceive(
                pipeline.ready_buffers,
                &item,
                portMAX_DELAY
            ) != pdTRUE) {
            result = ESP_FAIL;
            ESP_LOGE(kTag, "Failed to receive data from recording reader task");
            break;
        }
        if (item.error != 0) {
            ESP_LOGE(
                kTag,
                "Failed to read recording during download: id=%s error=%s",
                recording_id,
                std::strerror(item.error)
            );
            result = ESP_FAIL;
            break;
        }
        bool item_send_failed = false;
        if (item.size > 0) {
            const char* read_ahead_buffer = pipeline.buffer_block +
                item.buffer_index * pipeline.buffer_size;
            size_t item_offset = 0;
            while (item_offset < item.size) {
                const size_t send_size = std::min(
                    pipeline.read_buffer_size,
                    item.size - item_offset
                );
                std::memcpy(
                    pipeline.send_buffer,
                    read_ahead_buffer + item_offset,
                    send_size
                );

                const int64_t send_started_at_us = esp_timer_get_time();
                size_t chunk_sent_bytes = 0;
                int raw_send_result = 0;
                int raw_send_errno = 0;
                const bool send_succeeded = SendAll(
                    request,
                    pipeline.send_buffer,
                    send_size,
                    chunk_sent_bytes,
                    raw_send_result,
                    raw_send_errno
                );
                const int64_t send_elapsed_us =
                    esp_timer_get_time() - send_started_at_us;
                send_time_us += send_elapsed_us;
                max_send_time_us = std::max(max_send_time_us, send_elapsed_us);
                sent_bytes += chunk_sent_bytes;
                if (!send_succeeded) {
                    result = ESP_ERR_HTTPD_RESP_SEND;
                    item_send_failed = true;
                    ESP_LOGW(
                        kTag,
                        "Recording download send failed: id=%s result=%d errno=%d error=%s chunk_sent=%u chunk_total=%u send_ms=%lld",
                        recording_id,
                        raw_send_result,
                        raw_send_errno,
                        std::strerror(raw_send_errno),
                        static_cast<unsigned>(chunk_sent_bytes),
                        static_cast<unsigned>(send_size),
                        static_cast<long long>(send_elapsed_us / 1000)
                    );
                    break;
                }
                item_offset += send_size;

                const int64_t now_us = esp_timer_get_time();
                const int64_t progress_elapsed_us = now_us - progress_started_at_us;
                if (progress_elapsed_us >= kDownloadProgressIntervalUs) {
                    const uint64_t progress_bytes = sent_bytes - progress_started_at_bytes;
                    const uint64_t interval_kib_s = progress_bytes * 1000000ULL /
                        static_cast<uint64_t>(progress_elapsed_us) / 1024ULL;
                    ESP_LOGI(
                        kTag,
                        "Recording download progress: id=%s sent=%llu total=%lu interval_kib_s=%llu last_send_ms=%lld max_send_ms=%lld ready_buffers=%u rssi=%d free_internal=%u largest_internal=%u minimum_internal=%u",
                        recording_id,
                        static_cast<unsigned long long>(sent_bytes),
                        static_cast<unsigned long>(size_bytes),
                        static_cast<unsigned long long>(interval_kib_s),
                        static_cast<long long>(send_elapsed_us / 1000),
                        static_cast<long long>(max_send_time_us / 1000),
                        static_cast<unsigned>(uxQueueMessagesWaiting(pipeline.ready_buffers)),
                        ConnectedStationRssi(),
                        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL))
                    );
                    progress_started_at_us = now_us;
                    progress_started_at_bytes = sent_bytes;
                }
            }
        }
        if (item_send_failed) {
            break;
        }
        if (item.end_of_file) {
            break;
        }
        xQueueSend(
            pipeline.free_buffers,
            &item.buffer_index,
            portMAX_DELAY
        );
    }
    StopDownloadReadPipeline(pipeline, reader_started);
    close(file);
    const uint64_t elapsed_ms = static_cast<uint64_t>(
        std::max<int64_t>(1, (esp_timer_get_time() - started_at_us) / 1000)
    );
    const uint64_t throughput_kib_s = sent_bytes * 1000ULL / elapsed_ms / 1024ULL;
    const char* outcome = result == ESP_OK ? "completed" : "interrupted";
    ESP_LOGI(
        kTag,
        "Recording download %s: id=%s sent=%llu total=%lu elapsed_ms=%llu read_ms=%lld send_ms=%lld max_send_ms=%lld throughput_kib_s=%llu",
        outcome,
        recording_id,
        static_cast<unsigned long long>(sent_bytes),
        static_cast<unsigned long>(size_bytes),
        static_cast<unsigned long long>(elapsed_ms),
        static_cast<long long>(pipeline.read_time_us / 1000),
        static_cast<long long>(send_time_us / 1000),
        static_cast<long long>(max_send_time_us / 1000),
        static_cast<unsigned long long>(throughput_kib_s)
    );
    return result;
}

esp_err_t TemporaryAPTransferService::MetadataHandler(httpd_req_t* request) {
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
        service->active_downloads_++;
        if (!service->StartExpiryTimerLocked()) {
            service->active_downloads_--;
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to extend session");
            return ESP_FAIL;
        }
    }

    struct DownloadGuard {
        TemporaryAPTransferService* service = nullptr;
        ~DownloadGuard() {
            if (service != nullptr) {
                service->FinishDownload();
            }
        }
    } download_guard{service};

    uint32_t total_size = 0;
    uint32_t offset = 0;
    std::vector<uint8_t> chunk;
    if (!SdCardStorage::GetInstance().ReadRecordingMetadata(
            recording_id,
            offset,
            kMetadataHttpBufferSize,
            chunk,
            total_size
        )) {
        httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Recording metadata not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    char content_length[16] = {};
    std::snprintf(content_length, sizeof(content_length), "%lu", static_cast<unsigned long>(total_size));
    httpd_resp_set_hdr(request, "Content-Length", content_length);

    esp_err_t result = ESP_OK;
    while (true) {
        if (!chunk.empty()) {
            result = httpd_resp_send_chunk(
                request,
                reinterpret_cast<const char*>(chunk.data()),
                chunk.size()
            );
            if (result != ESP_OK) {
                break;
            }
            offset += static_cast<uint32_t>(chunk.size());
        }
        if (offset >= total_size) {
            break;
        }
        if (!SdCardStorage::GetInstance().ReadRecordingMetadata(
                recording_id,
                offset,
                kMetadataHttpBufferSize,
                chunk,
                total_size
            )) {
            result = ESP_FAIL;
            break;
        }
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, nullptr, 0);
    }
    return result;
}
