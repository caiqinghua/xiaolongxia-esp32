#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "esp_http_server.h"
#include "esp_netif.h"

struct TemporaryAPTransferSession {
    bool valid = false;
    std::string recording_id;
    std::string ssid;
    std::string password;
    std::string url;
    std::string token;
    uint32_t size_bytes = 0;
    uint32_t expires_in_seconds = 0;
};

class TemporaryAPTransferService {
public:
    static TemporaryAPTransferService& GetInstance();

    TemporaryAPTransferSession Start(const std::string& recording_id);
    void Stop();
    bool IsActive() const;

private:
    TemporaryAPTransferService() = default;

    static esp_err_t DownloadHandler(httpd_req_t* request);

    mutable std::mutex mutex_;
    httpd_handle_t server_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    TemporaryAPTransferSession session_;
    std::string file_path_;
    int64_t expires_at_us_ = 0;

    void StopLocked();
    bool StartAccessPointLocked();
    bool StartHttpServerLocked();
    bool IsExpiredLocked() const;
    bool IsTokenValidLocked(const std::string& token) const;
};
