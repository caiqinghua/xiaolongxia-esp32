#pragma once

#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <functional>
#include "audio/audio_session_coordinator.h"

#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"

struct TemporaryAPTransferSession {
    bool valid = false;
    std::string ssid;
    std::string password;
    std::string url;
    std::string token;
    uint32_t expires_in_seconds = 0;
};

class TemporaryAPTransferService {
public:
    static TemporaryAPTransferService& GetInstance();

    TemporaryAPTransferSession Start();
    bool Complete(const std::string& token);
    bool StopWithin(uint32_t timeout_ms);
    bool Stop() { return StopWithin(3000); }
    bool IsActive() const;
    // Install before network workers start. Legacy recording profiles omit it.
    bool SetAudioOwnership(AudioSessionCoordinator& coordinator,
        std::function<bool(uint64_t, uint32_t)> prepare, std::function<void()> released,
        std::function<void()> finished = {});

private:
    TemporaryAPTransferService() = default;

    static esp_err_t DownloadHandler(httpd_req_t* request);
    static esp_err_t MetadataHandler(httpd_req_t* request);

    mutable std::timed_mutex mutex_;
    httpd_handle_t server_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    esp_timer_handle_t expiry_timer_ = nullptr;
    TemporaryAPTransferSession session_;
    int64_t expires_at_us_ = 0;
    size_t active_downloads_ = 0;
    int active_socket_ = -1;
    bool stop_pending_ = false;
    bool wifi_owned_ = false;
    bool cleanup_running_ = false;
    std::atomic<bool> cancel_downloads_{false};
    std::atomic<uint64_t> stop_generation_{0};
    std::condition_variable_any cleanup_done_;
    std::atomic<bool> ownership_required_{false};
    AudioSessionCoordinator* audio_coordinator_ = nullptr;
    std::function<bool(uint64_t, uint32_t)> prepare_audio_;
    std::function<void()> audio_released_;
    std::function<void()> cleanup_finished_;
    uint64_t audio_generation_ = 0;
    bool audio_ready_ = false;

    static void ExpiryTimerCallback(void* arg);

    bool ScheduleStopLocked();
    void RunStop();
    bool HasResourcesLocked() const;
    void StopFromExpiry();
    void FinishDownload(int socket);
    bool StartAccessPointLocked();
    bool StartAccessPointAllowed();
    bool StartHttpServerLocked();
    bool StartExpiryTimerLocked();
    bool IsExpiredLocked() const;
    bool IsTokenValidLocked(const std::string& token) const;
};
