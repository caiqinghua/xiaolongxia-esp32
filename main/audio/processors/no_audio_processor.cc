#include "no_audio_processor.h"
#include <esp_log.h>
#include <chrono>

#define TAG "NoAudioProcessor"

NoAudioProcessor::~NoAudioProcessor() {
    is_running_ = false;
    std::lock_guard<std::timed_mutex> lock(feed_mutex_);
}

bool NoAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t*) {
    if (codec_) return codec == codec_ && frame_duration_ms * 16LL == frame_samples_;
    if (!codec || frame_duration_ms <= 0 || frame_duration_ms > 120 ||
        codec->input_channels() <= (codec->input_reference() ? 1 : 0) ||
        codec->input_channels() > 4) return false;
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
    output_buffer_.reserve(frame_samples_);
    return true;
}

void NoAudioProcessor::Feed(std::vector<int16_t>&& data) {
    std::lock_guard<std::timed_mutex> lock(feed_mutex_);
    if (!is_running_ || !output_callback_) {
        return;
    }

    // Assistant bypass selects the first microphone, never the reference channel.
    const size_t channels = codec_->input_channels();
    if (data.size() % channels != 0) return;
    for (size_t i = 0; i < data.size(); i += channels) {
        output_buffer_.push_back(data[i]);
    }

    // Output complete frames when buffer has enough data
    while (is_running_ && output_buffer_.size() >= (size_t)frame_samples_) {
        if (output_buffer_.size() == (size_t)frame_samples_) {
            output_callback_(std::move(output_buffer_));
            output_buffer_.clear();
            output_buffer_.reserve(frame_samples_);
        } else {
            output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
            output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
        }
    }
}

bool NoAudioProcessor::Start() {
    if (!codec_ || stopping_) return false;
    is_running_ = true;
    return true;
}

bool NoAudioProcessor::Stop(uint32_t timeout_ms) {
    is_running_ = false;
    stopping_ = true;
    std::unique_lock<std::timed_mutex> lock(feed_mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(timeout_ms))) return false;
    output_buffer_.clear();
    stopping_ = false;
    return true;
}

bool NoAudioProcessor::IsRunning() {
    return is_running_;
}

void NoAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void NoAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

size_t NoAudioProcessor::GetFeedSize() {
    if (!codec_) {
        return 0;
    }
    return frame_samples_;
}

void NoAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
        ESP_LOGE(TAG, "Device AEC is not supported");
    }
}
