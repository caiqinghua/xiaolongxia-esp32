#include "box_audio_codec.h"

#include "esp_log.h"
#include <algorithm>
#include <array>
#include <chrono>

bool BoxAudioCodec::ApplyWatchOutputVolumeLocked(int volume) {
    if (!out_codec_if_ || !out_codec_if_->set_vol ||
        esp_codec_dev_set_out_vol(output_dev_, volume) != ESP_CODEC_DEV_OK) return false;
    // Keep the SDK's default curve and cached preference. Its public setter
    // discards codec->set_vol failures, so explicitly apply the same dB value
    // through the ES8311 interface and require its actual I2C result.
    const esp_codec_dev_vol_range_t range{{0, -50.0f}, {100, 0.0f}};
    const float db = volume == 0 ? -96.0f : esp_codec_dev_vol_calc_db(&range, volume);
    return out_codec_if_->set_vol(out_codec_if_, db) == ESP_CODEC_DEV_OK;
}

bool BoxAudioCodec::SetWatchOutputVolume(int volume) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (!output_dev_ || volume < 0 || volume > 100 || watch_playback_shutdown_started_ ||
        Board::GetInstance().GetRuntimeProfile() != BoardRuntimeProfile::kWatchAssistantRecorder) return false;
    // Changing a paused or idle preference must not power the DAC or amplifier.
    if (output_enabled_ && !ApplyWatchOutputVolumeLocked(volume)) return false;
    output_volume_ = volume;
    return true;
}

bool BoxAudioCodec::DrainWatchPlaybackLocked() {
    if (!output_enabled_) return true;
    i2s_chan_info_t channel{};
    if (i2s_channel_get_info(tx_handle_, &channel) != ESP_OK || !channel.is_enabled ||
        !channel.total_dma_buf_size ||
        channel.total_dma_buf_size > AUDIO_CODEC_DMA_DESC_NUM * AUDIO_CODEC_DMA_FRAME_NUM * 4) return false;
    // Blocking writes replace the DMA ring in FIFO order. Two complete rings
    // cover a partially filled current buffer as well as queued audio. Silence
    // is not counted as recording position. Do not use a guessed sleep delay.
    size_t remaining = static_cast<size_t>(channel.total_dma_buf_size) * 2;
    std::array<uint8_t, 512> silence{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (remaining) {
        const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (timeout <= 0) return false;
        const size_t count = std::min(remaining, silence.size());
        size_t written = 0;
        if (i2s_channel_write(tx_handle_, silence.data(), count, &written, static_cast<uint32_t>(timeout)) != ESP_OK ||
            written != count) return false;
        remaining -= written;
    }
    return true;
}

bool BoxAudioCodec::SupportsWatchPlayback() const {
    return output_path_enabled_ && output_dev_ && tx_handle_ &&
        Board::GetInstance().GetRuntimeProfile() == BoardRuntimeProfile::kWatchAssistantRecorder;
}

bool BoxAudioCodec::SetWatchPlaybackOutput(bool enable) {
    if (!SupportsWatchPlayback()) return false;
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable) {
        if (watch_playback_shutdown_started_) return false;
        if (output_enabled_) return true;
        i2s_chan_info_t channel{};
        if (i2s_channel_get_info(tx_handle_, &channel) != ESP_OK ||
            (!channel.is_enabled && i2s_channel_enable(tx_handle_) != ESP_OK)) return false;
        esp_codec_dev_sample_info_t format{};
        format.bits_per_sample = 16;
        format.channel = 1;
        format.sample_rate = output_sample_rate_;
        if (esp_codec_dev_open(output_dev_, &format) != ESP_CODEC_DEV_OK ||
            !ApplyWatchOutputVolumeLocked(output_volume_)) {
            ESP_LOGE("WatchPlayback", "Playback DAC initialization failed; cleanup is required");
            return false;
        }
        AudioCodec::EnableOutput(true);
        return true;
    }
    // esp_codec_dev_close does not forward all peripheral errors. Also disable
    // and verify TX and force the amplifier off before acknowledging release.
    const bool retry = watch_playback_shutdown_started_;
    watch_playback_shutdown_started_ = true;
    bool closed = retry || DrainWatchPlaybackLocked();
    const bool codec_closed = esp_codec_dev_close(output_dev_) == ESP_CODEC_DEV_OK;
    if (codec_closed) AudioCodec::EnableOutput(false);
    closed = closed && codec_closed;
    i2s_chan_info_t channel{};
    const bool queried = i2s_channel_get_info(tx_handle_, &channel) == ESP_OK;
    if (!queried || channel.is_enabled) {
        const auto result = i2s_channel_disable(tx_handle_);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) closed = false;
    }
    if (!queried || i2s_channel_get_info(tx_handle_, &channel) != ESP_OK || channel.is_enabled) closed = false;
    if (pa_pin_ != GPIO_NUM_NC) {
        gpio_config_t config{};
        config.pin_bit_mask = 1ULL << pa_pin_;
        config.mode = GPIO_MODE_INPUT_OUTPUT;
        config.pull_up_en = GPIO_PULLUP_DISABLE;
        config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        config.intr_type = GPIO_INTR_DISABLE;
        const bool configured = gpio_config(&config) == ESP_OK;
        const bool disabled = gpio_set_level(pa_pin_, 0) == ESP_OK;
        if (!configured || !disabled || gpio_get_level(pa_pin_) != 0) closed = false;
    }
    if (closed) watch_playback_shutdown_started_ = false;
    return closed;
}

bool BoxAudioCodec::WriteWatchPlayback(const std::vector<int16_t>& data) {
    if (!SupportsWatchPlayback() || data.empty() || data.size() > static_cast<size_t>(output_sample_rate_) * 120 / 1000)
        return false;
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (!output_enabled_ || watch_playback_shutdown_started_) return false;
    // ES8311 uses hardware volume. Bypass the SDK I2S wrapper here because it
    // can report success during reconfiguration without writing any bytes.
    size_t written = 0;
    const size_t bytes = data.size() * sizeof(int16_t);
    return i2s_channel_write(tx_handle_, data.data(), bytes, &written, 500) == ESP_OK && written == bytes;
}
