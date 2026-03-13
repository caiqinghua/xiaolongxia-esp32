#include "server_config.h"
#include "settings.h"
#include "sdkconfig.h"
#include <esp_log.h>

#define TAG "ServerConfig"

// NVS namespace for server configuration
#define SERVER_CONFIG_NVS_NAMESPACE "server"
#define OTA_URL_KEY "ota_url"
#define WEBSOCKET_URL_KEY "websocket_url"
#define VISION_URL_KEY "vision_url"

std::string ServerConfig::GetOtaUrl() {
    Settings settings(SERVER_CONFIG_NVS_NAMESPACE, false);
    std::string url = settings.GetString(OTA_URL_KEY);
    if (!url.empty()) {
        ESP_LOGI(TAG, "Using NVS OTA URL: %s", url.c_str());
        return url;
    }
    ESP_LOGI(TAG, "Using default OTA URL: %s", CONFIG_OTA_URL);
    return std::string(CONFIG_OTA_URL);
}

std::string ServerConfig::GetWebSocketUrl() {
    Settings settings(SERVER_CONFIG_NVS_NAMESPACE, false);
    std::string url = settings.GetString(WEBSOCKET_URL_KEY);
    if (!url.empty()) {
        ESP_LOGI(TAG, "Using NVS WebSocket URL: %s", url.c_str());
        return url;
    }
    // Return empty to use OTA-provided URL
    return "";
}

std::string ServerConfig::GetVisionUrl() {
    Settings settings(SERVER_CONFIG_NVS_NAMESPACE, false);
    std::string url = settings.GetString(VISION_URL_KEY);
    if (!url.empty()) {
        ESP_LOGI(TAG, "Using NVS Vision URL: %s", url.c_str());
        return url;
    }
    // Return empty to use OTA-provided URL
    return "";
}

void ServerConfig::SetOtaUrl(const std::string& url) {
    Settings settings(SERVER_CONFIG_NVS_NAMESPACE, true);
    settings.SetString(OTA_URL_KEY, url);
    ESP_LOGI(TAG, "OTA URL saved to NVS: %s", url.c_str());
}

void ServerConfig::SetWebSocketUrl(const std::string& url) {
    Settings settings(SERVER_CONFIG_NVS_NAMESPACE, true);
    settings.SetString(WEBSOCKET_URL_KEY, url);
    ESP_LOGI(TAG, "WebSocket URL saved to NVS: %s", url.c_str());
}

void ServerConfig::SetVisionUrl(const std::string& url) {
    Settings settings(SERVER_CONFIG_NVS_NAMESPACE, true);
    settings.SetString(VISION_URL_KEY, url);
    ESP_LOGI(TAG, "Vision URL saved to NVS: %s", url.c_str());
}

void ServerConfig::ClearAll() {
    Settings settings(SERVER_CONFIG_NVS_NAMESPACE, true);
    settings.EraseKey(OTA_URL_KEY);
    settings.EraseKey(WEBSOCKET_URL_KEY);
    settings.EraseKey(VISION_URL_KEY);
    ESP_LOGI(TAG, "All server URL overrides cleared");
}

std::string ServerConfig::GetConfigInfo() {
    Settings settings(SERVER_CONFIG_NVS_NAMESPACE, false);
    std::string info = "Server Configuration:\n";

    std::string ota_url = settings.GetString(OTA_URL_KEY);
    if (!ota_url.empty()) {
        info += "  OTA URL: " + ota_url + " (NVS)\n";
    } else {
        info += "  OTA URL: " + std::string(CONFIG_OTA_URL) + " (Default)\n";
    }

    std::string ws_url = settings.GetString(WEBSOCKET_URL_KEY);
    if (!ws_url.empty()) {
        info += "  WebSocket URL: " + ws_url + " (NVS)\n";
    } else {
        info += "  WebSocket URL: (from OTA server)\n";
    }

    std::string vision_url = settings.GetString(VISION_URL_KEY);
    if (!vision_url.empty()) {
        info += "  Vision URL: " + vision_url + " (NVS)\n";
    } else {
        info += "  Vision URL: (from OTA server)\n";
    }

    return info;
}
