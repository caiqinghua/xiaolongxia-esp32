// ==================== Camera Web Server Implementation ====================
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>

#define TAG "CameraWebServer"

static httpd_handle_t camera_server = nullptr;
static bool server_started = false;

// HTTP GET handler for camera stream
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}

// Main page handler
static esp_err_t index_handler(httpd_req_t *req) {
    const char* html = R"rawliteral(
<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Camera</title>
<style>body{font-family:Arial;margin:20px;background:#1a1a1a;color:#fff;text-align:center}
img{width:100%;max-width:320px;border-radius:8px}button{margin:10px;padding:10px 20px;background:#007bff;color:white;border:none;border-radius:4px;cursor:pointer}</style>
</head><body>
<h1>ESP32 Camera Web Server</h1>
<img id="stream" src="/stream" alt="Camera Stream">
<div><button onclick="document.getElementById('stream').src='/stream?'+new Date().getTime()">Refresh</button></div>
<script>setInterval(function(){document.getElementById('stream').src='/stream?'+new Date().getTime()},2000);</script>
</body></html>
)rawliteral";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, html, strlen(html));
}

static httpd_uri_t uri_stream = {
    .uri      = "/stream",
    .method   = HTTP_GET,
    .handler  = stream_handler,
    .user_ctx = nullptr
};

static httpd_uri_t uri_index = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = index_handler,
    .user_ctx = nullptr
};

// Camera web server task
static void camera_web_server_task(void* pvParameters) {
    ESP_LOGI(TAG, "Waiting for WiFi and IP address...");
    
    // Wait for IP address (polling approach)
    int retry = 0;
    while (retry < 120) {  // Wait up to 60 seconds
        esp_netif_ip_info_t ip_info;
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char ip_str[16];
            sprintf(ip_str, IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI(TAG, "Got IP: %s", ip_str);
            
            // Start HTTP server
            httpd_config_t config = HTTPD_DEFAULT_CONFIG();
            config.server_port = 80;
            config.stack_size = 8 * 1024;
            config.max_uri_handlers = 2;

            ESP_LOGI(TAG, "Starting HTTP Server on port 80");
            if (httpd_start(&camera_server, &config) == ESP_OK) {
                httpd_register_uri_handler(camera_server, &uri_index);
                httpd_register_uri_handler(camera_server, &uri_stream);
                ESP_LOGI(TAG, "Camera web server started at http://%s", ip_str);
            } else {
                ESP_LOGE(TAG, "Failed to start HTTP server");
            }
            break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
        if (retry % 20 == 0) {
            ESP_LOGI(TAG, "Still waiting for IP... %d", retry);
        }
    }
    
    if (retry >= 120) {
        ESP_LOGE(TAG, "Timeout waiting for IP address");
    }
    
    vTaskDelete(NULL);
}

void start_camera_web_server() {
    // Create a task to wait for WiFi and start server
    xTaskCreate(camera_web_server_task, "camera_web_server", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Camera web server task created");
}
