// ==================== Camera Web Server Implementation ====================
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define TAG "CameraWebServer"

static httpd_handle_t camera_server = nullptr;
static bool server_started = false;

// HTTP GET handler for camera control
static esp_err_t cmd_handler(httpd_req_t *req) {
    // Get query string from URL
    char uri[256];
    int ret = httpd_req_get_url_query_str(req, uri, sizeof(uri));
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char value[32];
    char val_str[32];
    sensor_t *s = esp_camera_sensor_get();
    
    if (s != nullptr) {
        // Parse command parameters
        if (httpd_query_key_value(uri, "var", value, sizeof(value)) == ESP_OK) {
            if (httpd_query_key_value(uri, "val", val_str, sizeof(val_str)) == ESP_OK) {
                int val = atoi(val_str);
                int result = 0;
                
                if (strcmp(value, "framesize") == 0) {
                    result = s->set_framesize(s, (framesize_t)val);
                } else if (strcmp(value, "quality") == 0) {
                    result = s->set_quality(s, val);
                } else if (strcmp(value, "brightness") == 0) {
                    result = s->set_brightness(s, val);
                } else if (strcmp(value, "contrast") == 0) {
                    result = s->set_contrast(s, val);
                } else if (strcmp(value, "saturation") == 0) {
                    result = s->set_saturation(s, val);
                } else if (strcmp(value, "sharpness") == 0) {
                    result = s->set_sharpness(s, val);
                } else if (strcmp(value, "gainceiling") == 0) {
                    result = s->set_gainceiling(s, (gainceiling_t)val);
                } else if (strcmp(value, "colorbar") == 0) {
                    result = s->set_colorbar(s, val);
                } else if (strcmp(value, "whitebal") == 0) {
                    result = s->set_whitebal(s, val);
                } else if (strcmp(value, "awb_gain") == 0) {
                    result = s->set_awb_gain(s, val);
                } else if (strcmp(value, "wb_mode") == 0) {
                    result = s->set_wb_mode(s, val);
                } else if (strcmp(value, "exposure") == 0) {
                    result = s->set_exposure_ctrl(s, val);
                } else if (strcmp(value, "aec_value") == 0) {
                    result = s->set_aec_value(s, val);
                } else if (strcmp(value, "aec2") == 0) {
                    result = s->set_aec2(s, val);
                } else if (strcmp(value, "ae_level") == 0) {
                    result = s->set_ae_level(s, val);
                } else if (strcmp(value, "gain_ctrl") == 0) {
                    result = s->set_gain_ctrl(s, val);
                } else if (strcmp(value, "agc_gain") == 0) {
                    result = s->set_agc_gain(s, val);
                } else if (strcmp(value, "hmirror") == 0) {
                    result = s->set_hmirror(s, val);
                } else if (strcmp(value, "vflip") == 0) {
                    result = s->set_vflip(s, val);
                } else if (strcmp(value, "dcw") == 0) {
                    result = s->set_dcw(s, val);
                } else if (strcmp(value, "bpc") == 0) {
                    result = s->set_bpc(s, val);
                } else if (strcmp(value, "wpc") == 0) {
                    result = s->set_wpc(s, val);
                } else if (strcmp(value, "raw_gma") == 0) {
                    result = s->set_raw_gma(s, val);
                } else if (strcmp(value, "lenc") == 0) {
                    result = s->set_lenc(s, val);
                } else if (strcmp(value, "special_effect") == 0) {
                    result = s->set_special_effect(s, val);
                }
                
                ESP_LOGI(TAG, "Camera control: %s=%d, result=%d", value, val, result);
            }
        }
    }
    
    // Send success response
    const char* resp = "Success";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, resp, strlen(resp));
}

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
<title>ESP32 Camera Control</title>
<style>
body{font-family:Arial;margin:20px;background:#1a1a1a;color:#fff;text-align:center}
img{width:100%;max-width:320px;border-radius:8px}
.controls{max-width:320px;margin:20px auto;text-align:left}
.control-group{margin:15px 0}
.control-group label{display:block;margin-bottom:5px}
input[type=range]{width:100%}
select,button{padding:8px 15px;margin:5px;border-radius:4px;border:none}
button{background:#007bff;color:white;cursor:pointer}
button:hover{background:#0056b3}
</style>
</head><body>
<h1>ESP32 Camera Web Server</h1>
<img id="stream" src="/stream" alt="Camera Stream">
<div class="controls">
    <div class="control-group">
        <label>Brightness: <span id="brightness_val">0</span></label>
        <input type="range" id="brightness" min="-2" max="2" value="0" onchange="setControl('brightness',this.value)">
    </div>
    <div class="control-group">
        <label>Contrast: <span id="contrast_val">0</span></label>
        <input type="range" id="contrast" min="-2" max="2" value="0" onchange="setControl('contrast',this.value)">
    </div>
    <div class="control-group">
        <label>Saturation: <span id="saturation_val">0</span></label>
        <input type="range" id="saturation" min="-2" max="2" value="0" onchange="setControl('saturation',this.value)">
    </div>
    <div class="control-group">
        <label>Quality: <span id="quality_val">10</span></label>
        <input type="range" id="quality" min="10" max="63" value="10" onchange="setControl('quality',this.value)">
    </div>
    <div class="control-group">
        <button onclick="setControl('hmirror',1)">Flip H</button>
        <button onclick="setControl('vflip',1)">Flip V</button>
        <button onclick="refreshStream()">Refresh</button>
    </div>
</div>
<script>
function setControl(var_name,val){
    fetch('/control?var='+var_name+'&val='+val);
    document.getElementById(var_name+'_val').innerText=val;
}
function refreshStream(){document.getElementById('stream').src='/stream?'+new Date().getTime();}
setInterval(refreshStream,2000);
</script>
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

static httpd_uri_t uri_cmd = {
    .uri      = "/control",
    .method   = HTTP_GET,
    .handler  = cmd_handler,
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
                httpd_register_uri_handler(camera_server, &uri_cmd);
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
