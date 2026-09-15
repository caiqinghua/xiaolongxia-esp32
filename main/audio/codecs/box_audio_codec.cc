#include "box_audio_codec.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <utility>

#define TAG "BoxAudioCodec"

BoxAudioCodec::BoxAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference, uint8_t input_mic_mask,
    bool enable_output_path, uint8_t input_channel_count) {
    duplex_ = enable_output_path;
    input_reference_ = input_reference;
    input_channels_ = input_channel_count == 0 ? (input_reference_ ? 2 : 1) : input_channel_count;
    assert(input_channels_ >= 1 && input_channels_ <= 4);
    assert(!input_reference_ || input_channels_ >= 2);
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = 37.5f;
    pa_pin_ = pa_pin;
    input_mic_mask_ = input_mic_mask == 0 ? ES7210_SEL_MIC1 | ES7210_SEL_MIC2 : input_mic_mask;
    output_path_enabled_ = enable_output_path;
    normalize_tdm_input_order_ = input_reference_ && input_channels_ == 3 &&
        input_mic_mask_ == (ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3);

    CreateDuplexChannels(mclk, bclk, ws, dout, din, output_path_enabled_);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = es7210_addr,
        .bus_handle = i2c_master_handle,
    };
    ForcePowerAmplifierOff();
    if (output_path_enabled_) {
        i2c_cfg.addr = es8311_addr;
        out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
        assert(out_ctrl_if_ != NULL);

        gpio_if_ = audio_codec_new_gpio();
        assert(gpio_if_ != NULL);

        es8311_codec_cfg_t es8311_cfg = {};
        es8311_cfg.ctrl_if = out_ctrl_if_;
        es8311_cfg.gpio_if = gpio_if_;
        es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
        es8311_cfg.pa_pin = pa_pin;
        es8311_cfg.use_mclk = true;
        es8311_cfg.hw_gain.pa_voltage = 5.0;
        es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
        out_codec_if_ = es8311_codec_new(&es8311_cfg);
        assert(out_codec_if_ != NULL);

        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_OUT,
            .codec_if = out_codec_if_,
            .data_if = data_if_,
        };
        output_dev_ = esp_codec_dev_new(&dev_cfg);
        assert(output_dev_ != NULL);
    } else {
        ESP_LOGI(TAG, "Output codec path disabled for recording-only mode");
    }

    // Input path
    i2c_cfg.addr = es7210_addr;
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(in_ctrl_if_ != NULL);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = input_mic_mask_;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    assert(in_codec_if_ != NULL);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = in_codec_if_,
        .data_if = data_if_,
    };
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(input_dev_ != NULL);

    if (normalize_tdm_input_order_) {
        ESP_LOGI(TAG, "ES7210 TDM slot order remapped: slots=MIC1,MIC3,MIC2 application=MIC1,MIC2,reference");
    }
    ESP_LOGI(TAG, "BoxAudioDevice initialized");
}

BoxAudioCodec::~BoxAudioCodec() {
    if (output_dev_ != nullptr) {
        ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
        esp_codec_dev_delete(output_dev_);
    }
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(input_dev_);

    audio_codec_delete_codec_if(in_codec_if_);
    audio_codec_delete_ctrl_if(in_ctrl_if_);
    if (out_codec_if_ != nullptr) {
        audio_codec_delete_codec_if(out_codec_if_);
    }
    if (out_ctrl_if_ != nullptr) {
        audio_codec_delete_ctrl_if(out_ctrl_if_);
    }
    if (gpio_if_ != nullptr) {
        audio_codec_delete_gpio_if(gpio_if_);
    }
    audio_codec_delete_data_if(data_if_);
}

void BoxAudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    bool enable_output_path) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    if (enable_output_path) {
        ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle_));
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    if (tx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    }
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    ESP_LOGI(TAG, "%s channels created", enable_output_path ? "Duplex" : "RX-only");
}

void BoxAudioCodec::EnsureChannelEnabled(i2s_chan_handle_t handle) {
    i2s_chan_info_t channel_info = {};
    esp_err_t result = i2s_channel_get_info(handle, &channel_info);
    if (result != ESP_OK || channel_info.is_enabled) {
        return;
    }
    ESP_ERROR_CHECK(i2s_channel_enable(handle));
}

void BoxAudioCodec::DisableChannelIfEnabled(i2s_chan_handle_t handle, const char* name) {
    if (handle == nullptr) {
        return;
    }
    i2s_chan_info_t channel_info = {};
    esp_err_t result = i2s_channel_get_info(handle, &channel_info);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to query %s channel before deep sleep: %s", name, esp_err_to_name(result));
        return;
    }
    if (!channel_info.is_enabled) {
        return;
    }
    result = i2s_channel_disable(handle);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to disable %s channel before deep sleep: %s", name, esp_err_to_name(result));
    }
}

void BoxAudioCodec::ForcePowerAmplifierOff() {
    if (pa_pin_ == GPIO_NUM_NC) {
        return;
    }
    gpio_config_t io_config = {};
    io_config.pin_bit_mask = 1ULL << pa_pin_;
    io_config.mode = GPIO_MODE_OUTPUT;
    io_config.pull_up_en = GPIO_PULLUP_DISABLE;
    io_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_config.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_config);
    gpio_set_level(pa_pin_, 0);
}

uint16_t BoxAudioCodec::InputChannelMask() const {
    uint16_t mask = 0;
    for (int channel = 0; channel < input_channels_; ++channel) {
        mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(channel);
    }
    return mask;
}

uint16_t BoxAudioCodec::MicrophoneChannelMask() const {
    const int microphone_channels = input_channels_ - (input_reference_ ? 1 : 0);
    uint16_t mask = 0;
    for (int channel = 0; channel < microphone_channels; ++channel) {
        mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(channel);
    }
    return mask;
}

void BoxAudioCodec::SetOutputVolume(int volume) {
    if (output_dev_ != nullptr) {
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    }
    AudioCodec::SetOutputVolume(volume);
}

void BoxAudioCodec::SetInputGain(float gain) {
    constexpr float kMaximumEs7210GainDb = 37.5f;
    constexpr float kMaximumProtocolGainDb = 37.0f;
    AudioCodec::SetInputGain(
        gain >= kMaximumProtocolGainDb ? kMaximumEs7210GainDb : gain
    );
}

bool BoxAudioCodec::InputData(std::vector<int16_t>& data) {
    if (!AudioCodec::InputData(data)) {
        return false;
    }
    if (!normalize_tdm_input_order_) {
        return true;
    }

    const size_t frame_count = data.size() / input_channels_;
    for (size_t frame = 0; frame < frame_count; ++frame) {
        const size_t base = frame * input_channels_;
        std::swap(data[base + 1], data[base + 2]);
    }
    return true;
}

void BoxAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = InputChannelMask(),
            .sample_rate = (uint32_t)input_sample_rate_,
            .mclk_multiple = 0,
        };
        EnsureChannelEnabled(rx_handle_);
        esp_err_t result = esp_codec_dev_open(input_dev_, &fs);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Input codec open failed without TX channel; retrying with TX enabled: %s", esp_err_to_name(result));
            esp_codec_dev_close(input_dev_);
            if (tx_handle_ != nullptr) {
                EnsureChannelEnabled(tx_handle_);
                result = esp_codec_dev_open(input_dev_, &fs);
            }
        }
        ESP_ERROR_CHECK(result);
        const uint16_t microphone_channel_mask = MicrophoneChannelMask();
        ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_, microphone_channel_mask, input_gain_));
        ESP_LOGI(
            TAG,
            "Applied input gain: gain=%.1f dB microphone_mask=0x%04x input_mask=0x%04x",
            input_gain_,
            static_cast<unsigned>(microphone_channel_mask),
            static_cast<unsigned>(fs.channel_mask)
        );
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    }
    AudioCodec::EnableInput(enable);
}

void BoxAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (output_dev_ == nullptr) {
        ForcePowerAmplifierOff();
        AudioCodec::EnableOutput(false);
        ESP_LOGW(TAG, "Output codec path is disabled");
        return;
    }
    if (enable) {
        // Local recording disables TX; restore its clock before opening the DAC.
        EnsureChannelEnabled(tx_handle_);
        // Play 16bit 1 channel
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(output_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, output_volume_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    }
    AudioCodec::EnableOutput(enable);
}

void BoxAudioCodec::PrepareForLocalRecording() {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (output_dev_ != nullptr) {
        esp_err_t result = esp_codec_dev_close(output_dev_);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Failed to close output codec before local recording: %s", esp_err_to_name(result));
        }
    }
    if (output_enabled_) {
        AudioCodec::EnableOutput(false);
    }
    ForcePowerAmplifierOff();
    DisableChannelIfEnabled(tx_handle_, "TX");
}

void BoxAudioCodec::PrepareForDeepSleep() {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    esp_err_t result = esp_codec_dev_close(input_dev_);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to close input codec before deep sleep: %s", esp_err_to_name(result));
    }
    if (input_enabled_) {
        AudioCodec::EnableInput(false);
    }
    if (output_dev_ != nullptr) {
        result = esp_codec_dev_close(output_dev_);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Failed to close output codec before deep sleep: %s", esp_err_to_name(result));
        }
    }
    if (output_enabled_) {
        AudioCodec::EnableOutput(false);
    }
    DisableChannelIfEnabled(rx_handle_, "RX");
    DisableChannelIfEnabled(tx_handle_, "TX");
    ESP_LOGI(TAG, "Audio codec prepared for deep sleep");
}

int BoxAudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(input_dev_, (void*)dest, samples * sizeof(int16_t)));
    }
    return samples;
}

int BoxAudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(output_dev_, (void*)data, samples * sizeof(int16_t)));
    }
    return samples;
}
