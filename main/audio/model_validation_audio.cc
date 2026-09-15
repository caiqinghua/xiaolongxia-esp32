#include "audio_service.h"
#include <chrono>

namespace {
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr uint32_t kMaximumValidationMs = 60000;
}

bool AudioService::IsModelValidationCurrent(uint64_t generation) const {
    const auto now = NowMs();
    return generation && generation == model_validation_generation_.load() && !service_stopped_ &&
        now >= model_validation_started_ms_.load() && now < model_validation_deadline_ms_.load() &&
        assistant_suspended_ && session_coordinator_ && session_coordinator_->OwnsModelValidationResources(generation);
}

bool AudioService::RevokeModelValidation(uint64_t generation) {
    // Permission is revoked immediately; event bits and backends remain owned
    // until the application completes checked cleanup. Never clear a newer lease.
    return generation && model_validation_generation_.compare_exchange_strong(generation, 0);
}

bool AudioService::StartModelValidation(uint64_t generation, uint32_t duration_ms) {
    std::lock_guard<std::mutex> control_lock(audio_control_mutex_);
    if (!duration_ms || duration_ms > kMaximumValidationMs) return false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (service_stopped_ || !assistant_capable_ || !assistant_suspended_ || !wake_word_guard_ ||
            !codec_ || !models_list_ || !wake_word_ || !wake_word_initialized_ ||
            !audio_processor_ || !audio_processor_initialized_ || !session_coordinator_ ||
            !session_coordinator_->OwnsModelValidationResources(generation) || model_validation_generation_ ||
            audio_input_active_ || audio_output_active_ || audio_encode_active_ || audio_decode_active_ ||
            local_playback_generation_ || local_recording_encode_active_ || xEventGroupGetBits(event_group_) != 0)
            return false;
    }
    if (!wake_word_->SupportsBoundedHandoff() || !wake_word_->StopAndDrain(250) ||
        !audio_processor_->Stop(250)) return false;
    // Validation consumes PCM locally. It cannot emit a wake event or a cloud capture.
    active_wake_word_generation_ = 0;
    wake_word_->OnWakeWordDetected([](const std::string&) {});
    if (!wake_word_->StartChecked() || !audio_processor_->Start()) return false;
    {
        std::lock_guard<std::mutex> lock(input_resampler_mutex_);
        if (input_resampler_ && esp_ae_rate_cvt_reset(input_resampler_) != ESP_AE_ERR_OK) return false;
    }
    if (!session_coordinator_->OwnsModelValidationResources(generation)) return false;
    const auto now = NowMs();
    model_validation_started_ms_ = now;
    model_validation_deadline_ms_ = now + duration_ms;
    model_validation_feeds_ = 0;
    model_validation_generation_ = generation;
    xEventGroupSetBits(event_group_, AS_EVENT_MODEL_VALIDATION_RUNNING);
    return true;
}

bool AudioService::ProcessModelValidationInput() {
    const auto generation = model_validation_generation_.load();
    if (!IsModelValidationCurrent(generation)) return false;
    std::vector<int16_t> pcm;
    constexpr int samples = 160;
    if (!ReadAudioDataInternal(pcm, 16000, samples, false) || !IsModelValidationCurrent(generation) ||
        codec_->input_channels() < 1 || pcm.size() != static_cast<size_t>(samples * codec_->input_channels())) return false;
    wake_word_->Feed(pcm);
    if (!IsModelValidationCurrent(generation)) return false;
    audio_processor_->Feed(std::move(pcm));
    if (!IsModelValidationCurrent(generation)) return false;
    model_validation_feeds_.fetch_add(1, std::memory_order_relaxed);
    return true;
}
