#pragma once

#include "sdkconfig.h"
#include <aes/esp_aes.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include "blufi_session_state.h"
#include "blufi_control_session.h"
#include "blufi_profile_startup.h"
#if CONFIG_AUDREAM_WATCH_UI && defined(CONFIG_BT_BLUEDROID_ENABLED)
#include "watch/watch_ble_advertising_policy.h"
#include "watch/watch_ble_link_policy.h"
#include "esp_gap_ble_api.h"
#endif
#include "esp_blufi_api.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/aes.h"
#include "mbedtls/dhm.h"
#include "wifi_manager.h"

class Blufi {
public:
    /**
     * @brief Get the singleton instance of the Blufi class.
     */
    static Blufi &GetInstance();

    // Request one asynchronous station scan while provisioning owns WiFi.
    // Active access points are never switched or stopped by this operation.
    void start_wifi_scan();

    /**
     * @brief Initializes the Bluetooth controller, host, and Blufi profile.
     * This is the main entry point to start the Blufi process.
     * @return ESP_OK on success, otherwise an error code.
     */
    esp_err_t init(bool enable_wifi_provisioning = true);
    esp_err_t init(BlufiRole role);
    void SuspendWifiProvisioning();
    bool RunWithoutWifiProvisioning(const std::function<bool()>& operation);
    // Keep an existing control-only host alive until the pairing worker joins.
    uint64_t AcquireControlLease();
    bool ReleaseControlLease(uint64_t lease);

    using ControlTicket = BlufiControlSession::Ticket;
    ControlTicket CaptureControlPeer() const;
    bool IsControlPeerCurrent(const ControlTicket& ticket) const;
    esp_err_t SendControlData(const ControlTicket& ticket, const uint8_t* data, size_t length);

    /**
     * @brief Deinitializes Blufi and the Bluetooth stack.
     * @return ESP_OK on success, otherwise an error code.
     */
    esp_err_t deinit();
    bool IsActive() const;
    bool IsControlReady() const;
    bool IsConnected() const { return m_ble_is_connected.load(); }
    bool IsWifiProvisioning() const { return session_.AllowsProvisioning(); }
#if CONFIG_AUDREAM_WATCH_UI && defined(CONFIG_BT_BLUEDROID_ENABLED)
    void BeginAdvertising();
    bool HandleAdvertisingGapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
    void PollLinkPower();
    void NotifyLinkActivity();
    void HandleLinkPowerEvent(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param);
    void HandleLinkParameterUpdate(const uint8_t* address, uint16_t minimum, uint16_t maximum,
                                  uint16_t interval, uint16_t latency, uint16_t timeout, bool success);
#endif

    esp_err_t SendDeviceWillSleepEvent();
    esp_err_t SendRecordingStartedEvent(
        const std::string& recording_id,
        const std::string& file_name,
        uint64_t started_at_unix_ms
    );
    esp_err_t SendRecordingStoppedEvent(
        const std::string& recording_id,
        const std::string& file_name,
        uint32_t duration_ms,
        uint32_t size_bytes
    );
    esp_err_t SendRecordingStatusEvent(
        const std::string& recording_id,
        const std::string& file_name,
        uint32_t duration_ms
    );

    // Delete copy constructor and assignment operator for singleton
    Blufi(const Blufi &) = delete;

    Blufi &operator=(const Blufi &) = delete;

private:
    std::mutex lifecycle_mutex_;
    std::mutex credentials_mutex_;
    BlufiSessionState session_;
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    BlufiControlSession control_session_{true};
    BlufiProfileStartup profile_startup_;
#else
    BlufiControlSession control_session_{false};
#endif
    bool shutdown_failed_ = false;
    uint64_t control_lease_ = 0;
    uint64_t next_control_lease_ = 0;
#if CONFIG_AUDREAM_WATCH_UI && defined(CONFIG_BT_BLUEDROID_ENABLED)
    WatchBleAdvertisingPolicy advertising_power_;
    WatchBleLinkPolicy link_power_;
#endif

    Blufi();

    ~Blufi();

    // Initialization logic
    static esp_err_t _controller_init();

    static esp_err_t _controller_deinit();

    static esp_err_t _host_init();

    static esp_err_t _host_deinit();

    static esp_err_t _gap_register_callback();

    static esp_err_t _host_and_cb_init();
    void HandleProfileInitialized(bool success);
#if CONFIG_AUDREAM_WATCH_UI && defined(CONFIG_BT_BLUEDROID_ENABLED)
    void ApplyAdvertisingAction(WatchBleAdvertisingAction action, int64_t now_ms);
#endif

#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
    static esp_err_t AbortSecureHostStart();
#endif

    void _security_init();

    void _security_deinit();

    void _dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len,
                                    bool *need_free);

    int _aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    int _aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    static uint16_t _crc_checksum(uint8_t iv8, uint8_t *data, int len);

    void _handle_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);

    static int _get_softap_conn_num();

    // WiFi scan methods
    void _send_wifi_list();
    void StartWifiScanAllowed();
    static void _wifi_scan_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                         void *event_data);

    // These C-style functions are registered with ESP-IDF and call the corresponding instance
    // methods.

    static void _event_callback_trampoline(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);

    static void _negotiate_data_handler_trampoline(uint8_t *data, int len, uint8_t **output_data,
                                                   int *output_len, bool *need_free);

    static int _encrypt_func_trampoline(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    static int _decrypt_func_trampoline(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    static uint16_t _checksum_func_trampoline(uint8_t iv8, uint8_t *data, int len);

#ifdef CONFIG_BT_NIMBLE_ENABLED
    static void _nimble_on_reset(int reason);
    static void _nimble_on_sync();
    static void _nimble_host_task(void *param);
#endif

    // Security context, formerly blufi_sec struct
    struct BlufiSecurity {
#define DH_SELF_PUB_KEY_LEN 128
        uint8_t self_public_key[DH_SELF_PUB_KEY_LEN];
#define SHARE_KEY_LEN 128
        uint8_t share_key[SHARE_KEY_LEN];
        size_t share_len;
#define PSK_LEN 16
        uint8_t psk[PSK_LEN];
        uint8_t *dh_param;
        int dh_param_len;
        uint8_t iv[16];
        mbedtls_dhm_context *dhm;
        esp_aes_context *aes;
    };

    BlufiSecurity *m_sec;

    // State variables
    wifi_config_t m_sta_config{};
    std::atomic<bool> m_ble_is_connected{false};
    bool m_sta_connected;
    bool m_sta_got_ip;
    bool m_ssid_valid = false;
    bool m_password_valid = true;
    uint8_t m_sta_bssid[6]{};
    uint8_t m_sta_ssid[32]{};
    int m_sta_ssid_len;
    esp_blufi_extra_info_t m_sta_conn_info{};

    // WiFi scan related
    std::vector<wifi_ap_record_t> m_ap_records;
    std::mutex scan_mutex_;
    bool m_scan_in_progress = false;
    BlufiSessionState::Ticket scan_ticket_ = 0;
    ControlTicket scan_peer_;
    esp_event_handler_instance_t scan_event_instance_ = nullptr;
    struct ConnectionRequest;
    void ConnectToAccessPoint(ConnectionRequest& request);
    void DisconnectStation(const ConnectionRequest& request);
    esp_err_t SendWifiReport(const ControlTicket& peer, wifi_mode_t mode,
        esp_blufi_sta_conn_state_t state, uint8_t softap_connections, esp_blufi_extra_info_t* info);
    esp_err_t DeinitLocked();
    void DeinitCompletedProvisioning(BlufiSessionState::Ticket ticket);
};
