#include "afe_audio_processor.h"
#include <esp_log.h>
#include <chrono>

#define TAG "AfeAudioProcessor"

AfeAudioProcessor::AfeAudioProcessor() = default;

bool AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    if (afe_data_) return codec == codec_ && frame_duration_ms * 16LL == frame_samples_;
    if (!codec || frame_duration_ms <= 0 || frame_duration_ms > 120 ||
        codec->input_channels() <= (codec->input_reference() ? 1 : 0) ||
        codec->input_channels() > 4) {
        ESP_LOGE(TAG, "Invalid audio processor input configuration");
        return false;
    }
    std::string format(codec->input_channels() - (codec->input_reference() ? 1 : 0), 'M');
    if (codec->input_reference()) format.push_back('R');

    // ESP-SR owns a process-wide model list, shared with wake-word detection.
    auto* models = models_list ? models_list : esp_srmodel_init("model");
    auto* config = afe_config_init(format.c_str(), nullptr, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    if (!config) {
        ESP_LOGE(TAG, "AFE configuration allocation failed");
        return false;
    }
    auto* ns = models ? esp_srmodel_filter(models, ESP_NSNET_PREFIX, nullptr) : nullptr;
    auto* vad = models ? esp_srmodel_filter(models, ESP_VADN_PREFIX, nullptr) : nullptr;
    config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    config->vad_mode = VAD_MODE_0;
    config->vad_min_noise_ms = 100;
    if (vad) config->vad_model_name = vad;
    config->ns_init = ns != nullptr;
    if (ns) {
        config->ns_model_name = ns;
        config->afe_ns_mode = AFE_NS_MODE_NET;
    }
    config->agc_init = false;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
#if CONFIG_USE_DEVICE_AEC
    config->aec_init = codec->input_reference();
#else
    config->aec_init = false;
#endif
    // Allocate VAD even with AEC so a later wake utterance can enable local
    // endpointing without rebuilding models while another task owns them.
    config->vad_init = true;
    device_aec_enabled_ = config->aec_init;
    config->afe_perferred_priority = 1;
    afe_iface_ = esp_afe_handle_from_config(config);
    if (afe_iface_) afe_data_ = afe_iface_->create_from_config(config);
    afe_config_free(config);
    if (!afe_data_ || afe_iface_->get_feed_chunksize(afe_data_) <= 0 ||
        afe_iface_->get_fetch_chunksize(afe_data_) <= 0) {
        if (afe_data_) afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
        ESP_LOGE(TAG, "AFE creation or chunk validation failed");
        return false;
    }
    codec_ = codec;
    if (device_aec_enabled_ && afe_iface_->disable_vad(afe_data_) < 0) {
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        codec_ = nullptr;
        ESP_LOGE(TAG, "AFE initial VAD mode could not be applied");
        return false;
    }
    frame_samples_ = frame_duration_ms * 16;
    output_buffer_.reserve(frame_samples_);
    worker_exited_ = false;
    if (xTaskCreate([](void* arg) {
            static_cast<AfeAudioProcessor*>(arg)->AudioProcessorTask();
            vTaskDelete(nullptr);
        }, "audio_communication", 4096, this, 5, nullptr) != pdPASS) {
        worker_exited_ = true;
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
        codec_ = nullptr;
        ESP_LOGE(TAG, "AFE worker creation failed");
        return false;
    }
    ESP_LOGI(TAG, "AFE initialized: channels=%d reference=%d neural_ns=%d",
        codec->input_channels(), codec->input_reference(), ns != nullptr);
    return true;
}

AfeAudioProcessor::~AfeAudioProcessor() {
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        running_ = false;
        shutdown_ = true;
        state_cv_.notify_all();
        // Destruction must not free memory still owned by a fetch/output callback.
        state_cv_.wait(lock, [this]() { return worker_exited_; });
    }
    std::lock_guard<std::timed_mutex> input_lock(input_buffer_mutex_);
    if (afe_data_) afe_iface_->destroy(afe_data_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    return afe_data_ ? afe_iface_->get_feed_chunksize(afe_data_) : 0;
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    std::lock_guard<std::timed_mutex> lock(input_buffer_mutex_);
    if (!IsRunning()) return;
    const size_t chunk_size = GetFeedSize() * codec_->input_channels();
    if (!chunk_size || data.size() % codec_->input_channels() != 0) return;
    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    while (input_buffer_.size() >= chunk_size && IsRunning()) {
        afe_iface_->feed(afe_data_, input_buffer_.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
    }
}

bool AfeAudioProcessor::Start() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!afe_data_ || worker_exited_ || stopping_ || shutdown_ || !mode_ready_ ||
        (speech_monitor_ && (!device_aec_enabled_ || aec_mode_uncertain_ || !speech_monitor_callback_))) return false;
    running_ = true;
    state_cv_.notify_all();
    return true;
}

bool AfeAudioProcessor::Stop(uint32_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        running_ = false;
        stopping_ = true;
        if (!state_cv_.wait_until(lock, deadline, [this]() { return !worker_active_; })) return false;
    }
    std::unique_lock<std::timed_mutex> input_lock(input_buffer_mutex_, std::defer_lock);
    if (!input_lock.try_lock_until(deadline)) return false;
    if (afe_data_ && afe_iface_->reset_buffer(afe_data_) < 0) {
        ESP_LOGE(TAG, "AFE buffer reset failed; restart remains inhibited");
        return false;
    }
    input_buffer_.clear();
    output_buffer_.clear();
    is_speaking_ = false;
    std::lock_guard<std::mutex> lock(state_mutex_);
    stopping_ = false;
    return true;
}

bool AfeAudioProcessor::IsRunning() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return running_;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&&)> callback) {
    output_callback_ = std::move(callback);
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool)> callback) {
    vad_state_change_callback_ = std::move(callback);
}
void AfeAudioProcessor::OnSpeechMonitor(SpeechMonitorCallback callback) {
    speech_monitor_callback_ = std::move(callback);
}

bool AfeAudioProcessor::SetCapturePreroll(std::vector<int16_t>&& pcm) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!afe_data_ || running_ || stopping_ || worker_active_ || shutdown_ ||
        speech_monitor_ || !mode_ready_ || !local_endpointing_ || pcm.size() > 16000) return false;
    output_buffer_ = std::move(pcm);
    return true;
}

bool AfeAudioProcessor::SetSpeechMonitor(bool enable) {
    if (!afe_data_ || aec_mode_uncertain_ ||
        (enable && (!codec_->input_reference() || !device_aec_enabled_ || !speech_monitor_callback_))) return false;
    if (!Stop()) return false;
    const bool applied = (enable || local_endpointing_ || !device_aec_enabled_
        ? afe_iface_->enable_vad(afe_data_) : afe_iface_->disable_vad(afe_data_)) >= 0;
    std::lock_guard<std::mutex> lock(state_mutex_);
    mode_ready_ = applied;
    if (applied) speech_monitor_ = enable;
    return applied;
}

void AfeAudioProcessor::AudioProcessorTask() {
    std::unique_lock<std::mutex> lock(state_mutex_);
    while (true) {
        state_cv_.wait(lock, [this]() { return running_ || shutdown_; });
        if (shutdown_) break;
        worker_active_ = true;
        lock.unlock();
        auto* res = afe_iface_->fetch_with_delay(afe_data_, pdMS_TO_TICKS(100));
        if (IsRunning() && res && res->ret_value != ESP_FAIL && res->data &&
            res->data_size > 0 && res->data_size % sizeof(int16_t) == 0) {
            const bool speaking = res->vad_state == VAD_SPEECH;
            if (speech_monitor_) {
                if (speech_monitor_callback_) speech_monitor_callback_(res->data,
                    res->data_size / sizeof(int16_t), speaking);
            } else if (vad_state_change_callback_ && speaking != is_speaking_) {
                is_speaking_ = speaking;
                vad_state_change_callback_(speaking);
            }
            if (!speech_monitor_ && output_callback_) {
                output_buffer_.insert(output_buffer_.end(), res->data,
                    res->data + res->data_size / sizeof(int16_t));
                while (output_buffer_.size() >= static_cast<size_t>(frame_samples_) && IsRunning()) {
                    std::vector<int16_t> frame(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                    output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                    output_callback_(std::move(frame));
                }
            }
        }
        lock.lock();
        worker_active_ = false;
        state_cv_.notify_all();
    }
    worker_exited_ = true;
    state_cv_.notify_all();
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    if (!afe_data_ || (enable && !codec_->input_reference())) return;
    const bool restart = IsRunning();
    if (!Stop()) {
        ESP_LOGE(TAG, "AFE mode change deferred until callbacks drain");
        return;
    }
    if (enable) {
#if CONFIG_USE_DEVICE_AEC
        aec_mode_uncertain_ = afe_iface_->enable_aec(afe_data_) < 0;
        if (!aec_mode_uncertain_) device_aec_enabled_ = true;
        const bool applied = !aec_mode_uncertain_ &&
            (local_endpointing_ || speech_monitor_ ? afe_iface_->enable_vad(afe_data_) : afe_iface_->disable_vad(afe_data_)) >= 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            mode_ready_ = applied;
        }
        if (!applied) return;
#else
        ESP_LOGE(TAG, "Device AEC is not supported");
#endif
    } else {
        aec_mode_uncertain_ = afe_iface_->disable_aec(afe_data_) < 0;
        if (!aec_mode_uncertain_) device_aec_enabled_ = false;
        const bool applied = !aec_mode_uncertain_ && afe_iface_->enable_vad(afe_data_) >= 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            mode_ready_ = applied;
        }
        if (!applied) return;
    }
    if (restart) Start();
}

bool AfeAudioProcessor::SetLocalEndpointing(bool enable) {
    if (!afe_data_ || aec_mode_uncertain_) return false;
    if (local_endpointing_ == enable && mode_ready_) return true;
    if (!Stop()) return false;
    const bool applied = (enable || speech_monitor_ || !device_aec_enabled_
        ? afe_iface_->enable_vad(afe_data_) : afe_iface_->disable_vad(afe_data_)) >= 0;
    std::lock_guard<std::mutex> lock(state_mutex_);
    mode_ready_ = applied;
    if (applied) local_endpointing_ = enable;
    return applied;
}
