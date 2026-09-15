#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 2, ESP_ERR_NO_MEM = 3;
constexpr int ESP_ERR_WIFI_NOT_INIT = 4, ESP_ERR_WIFI_NOT_STARTED = 5, ESP_ERR_WIFI_CONN = 6;
constexpr int ESP_ERR_HTTPD_RESP_SEND = 7, HTTP_GET = 0, ESP_TIMER_TASK = 0;
constexpr int HTTPD_400_BAD_REQUEST = 400, HTTPD_403_FORBIDDEN = 403;
constexpr int HTTPD_404_NOT_FOUND = 404, HTTPD_500_INTERNAL_SERVER_ERROR = 500;
constexpr int MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_DMA = 2, MALLOC_CAP_8BIT = 4, MALLOC_CAP_SPIRAM = 8;
constexpr int WIFI_MODE_AP = 2, WIFI_IF_AP = 1, WIFI_AUTH_WPA2_PSK = 3, WIFI_PS_NONE = 0;
using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = uint32_t;
constexpr int pdTRUE = 1, pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(ms) (ms)
template<class... T> void TestLog(const char*, const char*, T...) {}
#define ESP_LOGE(...) TestLog(__VA_ARGS__)
#define ESP_LOGW(...) TestLog(__VA_ARGS__)
#define ESP_LOGI(...) TestLog(__VA_ARGS__)
#define ESP_LOGD(...) TestLog(__VA_ARGS__)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(expr) ((void)(expr))
inline const char* esp_err_to_name(int) { return "test-error"; }

struct TestTimer { bool active = false; void (*callback)(void*) = nullptr; void* arg = nullptr; };
using esp_timer_handle_t = TestTimer*;
struct esp_timer_create_args_t {
    void (*callback)(void*);
    void* arg;
    int dispatch_method;
    const char* name;
    bool skip_unhandled_events;
};
int64_t esp_timer_get_time();
int esp_timer_create(const esp_timer_create_args_t*, esp_timer_handle_t*);
int esp_timer_stop(esp_timer_handle_t);
bool esp_timer_is_active(esp_timer_handle_t);
int esp_timer_start_once(esp_timer_handle_t, uint64_t);
struct esp_netif_t {};
struct esp_netif_ip_info_t { int ip, gw, netmask; };
#define IP4_ADDR(ptr, a, b, c, d) (*(ptr) = (d))
int esp_netif_init();
esp_netif_t* esp_netif_create_default_wifi_ap();
void esp_netif_destroy_default_wifi(esp_netif_t*);
int esp_netif_dhcps_stop(esp_netif_t*);
int esp_netif_dhcps_start(esp_netif_t*);
int esp_netif_set_ip_info(esp_netif_t*, esp_netif_ip_info_t*);
int esp_event_loop_create_default();
struct wifi_init_config_t { bool nvs_enable = true; };
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
struct wifi_config_t {
    struct { uint8_t ssid[32], password[64]; size_t ssid_len; int max_connection, channel, authmode; } ap;
};
struct wifi_sta_list_t { int num; struct { int rssi; } sta[1]; };
int esp_wifi_init(wifi_init_config_t*);
int esp_wifi_stop();
int esp_wifi_deinit();
int esp_wifi_set_mode(int);
int esp_wifi_set_config(int, wifi_config_t*);
int esp_wifi_set_ps(int);
int esp_wifi_start();
int esp_wifi_ap_get_sta_list(wifi_sta_list_t*);
int esp_efuse_mac_get_default(uint8_t*);
uint32_t esp_random();
size_t heap_caps_get_free_size(int);
size_t heap_caps_get_largest_free_block(int);
size_t heap_caps_get_minimum_free_size(int);
void* heap_caps_malloc(size_t, int);
void heap_caps_free(void*);

using httpd_handle_t = void*;
struct httpd_req_t { void* user_ctx; std::string token; int error = 0; };
struct httpd_config_t { bool (*uri_match_fn)(const char*, const char*, size_t); int recv_wait_timeout, send_wait_timeout; };
#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}
struct httpd_uri_t { const char* uri; int method; int (*handler)(httpd_req_t*); void* user_ctx; };
bool httpd_uri_match_wildcard(const char*, const char*, size_t);
int httpd_start(httpd_handle_t*, httpd_config_t*);
int httpd_stop(httpd_handle_t);
int httpd_register_uri_handler(httpd_handle_t, httpd_uri_t*);
int httpd_req_get_url_query_str(httpd_req_t*, char*, size_t);
int httpd_query_key_value(const char*, const char*, char*, size_t);
int httpd_resp_send_err(httpd_req_t*, int, const char*);
int httpd_resp_set_type(httpd_req_t*, const char*);
int httpd_resp_set_hdr(httpd_req_t*, const char*, const char*);
int httpd_resp_send_chunk(httpd_req_t*, const char*, size_t);
int httpd_send(httpd_req_t*, const char*, size_t);
int httpd_req_to_sockfd(httpd_req_t*);

using QueueHandle_t = void*;
using SemaphoreHandle_t = void*;
QueueHandle_t xQueueCreate(size_t, size_t);
int xQueueReceive(QueueHandle_t, void*, TickType_t);
int xQueueSend(QueueHandle_t, const void*, TickType_t);
size_t uxQueueMessagesWaiting(QueueHandle_t);
void vQueueDelete(QueueHandle_t);
SemaphoreHandle_t xSemaphoreCreateBinary();
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
int xTaskCreate(void (*)(void*), const char*, size_t, void*, unsigned, void*);
int xTaskCreatePinnedToCore(void (*)(void*), const char*, size_t, void*, unsigned, void*, int);
void vTaskDelete(void*);

class WifiManager {
public:
    static WifiManager& GetInstance();
    bool IsInitialized() const;
    void StopStation();
};
class SdCardStorage {
public:
    static SdCardStorage& GetInstance();
    bool GetRecordingFilePath(const std::string&, std::string&, uint32_t&);
    bool ReadRecordingMetadata(const std::string&, uint32_t, size_t, std::vector<uint8_t>&, uint32_t&);
};
class AudreamPowerManager {
public:
    static AudreamPowerManager& GetInstance();
    void SetTemporaryAPTransferActive(bool);
};
namespace ap_test {
extern std::function<int()> http_stop;
extern std::function<void()> metadata_read;
extern std::function<void()> wifi_start, socket_shutdown;
extern std::atomic<int> stop_error, deinit_calls, stop_calls, tasks;
extern std::atomic<bool> task_failure, powered, manager_initialized;
extern httpd_uri_t metadata_handler;
extern TestTimer timer;
void JoinTasks();
}
