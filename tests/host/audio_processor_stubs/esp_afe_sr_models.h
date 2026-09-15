#pragma once
#include "model_path.h"
#include <freertos/FreeRTOS.h>
#include <cstdint>

constexpr int ESP_FAIL = -1;
constexpr int AFE_TYPE_VC = 0;
constexpr int AFE_TYPE_SR = 1;
constexpr int AEC_MODE_SR_HIGH_PERF = 1;
constexpr int WAKENET_DETECTED = 1;
constexpr int AFE_MODE_HIGH_PERF = 0;
constexpr int AEC_MODE_VOIP_HIGH_PERF = 0;
constexpr int VAD_MODE_0 = 0;
constexpr int AFE_NS_MODE_NET = 1;
constexpr int AFE_MEMORY_ALLOC_MORE_PSRAM = 1;
constexpr int VAD_SPEECH = 1;
constexpr int VAD_SILENCE = 0;
constexpr auto ESP_NSNET_PREFIX = "ns";
constexpr auto ESP_VADN_PREFIX = "vad";

struct afe_config_t {
    int aec_mode = 0, vad_mode = 0, vad_min_noise_ms = 0;
    char* vad_model_name = nullptr;
    char* ns_model_name = nullptr;
    char* wakenet_model_name = nullptr;
    char* wakenet_model_name_2 = nullptr;
    int afe_perferred_core = 0;
    bool ns_init = false, agc_init = false, aec_init = false, vad_init = false;
    int afe_ns_mode = 0, memory_alloc_mode = 0, afe_perferred_priority = 0;
};
struct esp_afe_sr_data_t;
struct afe_fetch_result_t {
    int ret_value = 0;
    int16_t* data = nullptr;
    int data_size = 0;
    int vad_state = VAD_SILENCE;
    int wakeup_state = 0, wakenet_model_index = 0, wake_word_index = 0;
};
struct esp_afe_sr_iface_t {
    esp_afe_sr_data_t* (*create_from_config)(afe_config_t*);
    int (*get_feed_chunksize)(esp_afe_sr_data_t*);
    int (*get_fetch_chunksize)(esp_afe_sr_data_t*);
    void (*destroy)(esp_afe_sr_data_t*);
    int (*feed)(esp_afe_sr_data_t*, const int16_t*);
    afe_fetch_result_t* (*fetch_with_delay)(esp_afe_sr_data_t*, TickType_t);
    int (*reset_buffer)(esp_afe_sr_data_t*);
    int (*disable_vad)(esp_afe_sr_data_t*);
    int (*enable_vad)(esp_afe_sr_data_t*);
    int (*enable_aec)(esp_afe_sr_data_t*);
    int (*disable_aec)(esp_afe_sr_data_t*);
};
afe_config_t* afe_config_init(const char*, srmodel_list_t*, int, int);
void afe_config_free(afe_config_t*);
const esp_afe_sr_iface_t* esp_afe_handle_from_config(afe_config_t*);
