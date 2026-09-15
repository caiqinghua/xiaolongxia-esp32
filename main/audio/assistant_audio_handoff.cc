#include "audio_service.h"
#include <esp_log.h>
#include <algorithm>
#include <limits>

namespace { constexpr auto TAG = "AudioService"; }

bool AudioService::PrepareWatchWakeWord(srmodel_list_t* models) {
    std::lock_guard<std::mutex> control_lock(audio_control_mutex_);
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (!models || !codec_ || !wake_word_guard_ || !assistant_suspended_ ||
        !session_coordinator_ || (!session_coordinator_->OwnsMaintenanceResources() &&
            !session_coordinator_->OwnsModelValidationPreparation(session_coordinator_->Generation())) ||
        audio_input_active_ || audio_output_active_ || audio_encode_active_ || audio_decode_active_ ||
        local_playback_generation_ || local_recording_encode_active_ ||
        xEventGroupGetBits(event_group_) != 0 || (models_list_ && models_list_ != models)) return false;
    if (!wake_word_ && !CreateWatchWakeWord(models)) return false;
    if (!wake_word_ || !wake_word_->SupportsBoundedHandoff()) return false;
    models_list_ = models;
    if (!wake_word_initialized_) wake_word_initialized_ = wake_word_->Initialize(codec_, models);
    return wake_word_initialized_;
}

bool AudioService::PrepareVoiceProcessing(srmodel_list_t* models, bool local_endpointing) {
    std::lock_guard<std::mutex> control_lock(audio_control_mutex_);
    if (!audio_processor_ || !codec_) return false;
    if (!AllowsAssistantAudio()) {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        // A suspended maintenance owner may initialize already pinned models.
        // This grants neither capture permission nor cloud authorization.
        if (!models || models != models_list_ || !wake_word_guard_ || !wake_word_initialized_ ||
            !assistant_capable_ || !assistant_suspended_ || !session_coordinator_ ||
            (!session_coordinator_->OwnsMaintenanceResources() &&
                !session_coordinator_->OwnsModelValidationPreparation(session_coordinator_->Generation())) || audio_input_active_ ||
            audio_output_active_ || audio_encode_active_ || audio_decode_active_ ||
            local_playback_generation_ || local_recording_encode_active_ ||
            xEventGroupGetBits(event_group_) != 0) return false;
    }
    if (models && models_list_ && models != models_list_) return false;
    if (!audio_processor_initialized_) {
        audio_processor_initialized_ = audio_processor_->Initialize(
            codec_, OPUS_FRAME_DURATION_MS, models ? models : models_list_);
    }
    return audio_processor_initialized_ && audio_processor_->SetLocalEndpointing(local_endpointing);
}

void AudioService::EnableWakeWordDetection(bool enable) {
    std::lock_guard<std::mutex> control_lock(audio_control_mutex_);
    if (enable && !AllowsWakeWordInput()) {
        return;
    }
    if (!wake_word_) {
        return;
    }

    if (enable && (session_coordinator_ || wake_word_guard_) && !wake_word_->SupportsBoundedHandoff()) {
        ESP_LOGE(TAG, "Wake-word backend does not support coordinated audio handoff");
        return;
    }

    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (wake_word_guard_ && IsWakeWordRunning()) return;
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_, models_list_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return;
            }
            wake_word_initialized_ = true;
            if (!wake_word_guard_) {
                wake_word_->OnWakeWordDetected([this](const std::string& word) {
                    if (AllowsWakeWordInput() && callbacks_.on_wake_word_detected)
                        callbacks_.on_wake_word_detected(word);
                });
            }
        }
        // Drain old callbacks before installing a new generation. Coordinated
        // callers cannot relabel a late result as belonging to the new owner.
        if (wake_word_guard_ && !wake_word_->StopAndDrain(250)) {
            ESP_LOGE(TAG, "Wake-word rearm drain failed");
            return;
        }
        if (wake_word_generation_ == std::numeric_limits<uint64_t>::max()) return;
        const auto generation = ++wake_word_generation_;
        if (wake_word_guard_) {
            wake_word_->OnWakeWordDetected([this, generation](const std::string& word) {
                if (!IsWakeWordGenerationCurrent(generation)) return;
                if (callbacks_.on_guarded_wake_word_detected)
                    callbacks_.on_guarded_wake_word_detected(word, generation);
            });
        }
        // Reset input resampler to clear cached data from previous mode (e.g. AudioProcessor)
        // This prevents buffer overflow when switching between different feed sizes
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        active_wake_word_generation_ = generation;
        if (AllowsWakeWordInput() && wake_word_->StartChecked()) {
            xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        } else {
            active_wake_word_generation_ = 0;
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            ESP_LOGE(TAG, "Wake-word start failed");
        }
    } else {
        active_wake_word_generation_ = 0;
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        audio_queue_cv_.notify_all();
        wake_word_->Stop();
    }
}

bool AudioService::SetWakeWordGuard(std::function<bool()> guard) {
    std::lock_guard<std::mutex> lock(audio_control_mutex_);
    if (!service_stopped_ || wake_word_guard_ || !guard || active_wake_word_generation_) return false;
    wake_word_guard_ = std::move(guard);
    return true;
}

bool AudioService::AllowsWakeWordInput() const {
    return AllowsAssistantAudio() && (!wake_word_guard_ || wake_word_guard_());
}

bool AudioService::IsWakeWordGenerationCurrent(uint64_t generation) const {
    return generation && generation == active_wake_word_generation_.load() && AllowsWakeWordInput();
}

bool AudioService::AllowsAssistantAudio() const {
    return assistant_capable_ && assistant_user_enabled_ && !assistant_suspended_.load() &&
        (session_coordinator_ == nullptr || session_coordinator_->AllowsAssistantResources());
}

bool AudioService::QuiesceAssistant(uint32_t timeout_ms) {
    std::lock_guard<std::mutex> control_lock(audio_control_mutex_);
    return QuiesceAssistantLocked(timeout_ms);
}

bool AudioService::QuiesceAssistantLocked(uint32_t timeout_ms, uint64_t playback_generation) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (local_playback_generation_ && local_playback_generation_ != playback_generation) return false;
    if ((xEventGroupGetBits(event_group_) & AS_EVENT_LOCAL_RECORDING_RUNNING) != 0 ||
        local_recording_encode_active_) {
        return false;
    }
    for (const auto& task : audio_encode_queue_) {
        if (task->type == kAudioTaskTypeEncodeToLocalRecording) {
            return false;
        }
    }
    assistant_suspended_ = true;
    model_validation_generation_ = 0;
    speech_monitor_generation_ = 0;
    active_wake_word_generation_ = 0;
    voice_detected_ = false;
    ++audio_generation_;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_MODEL_VALIDATION_RUNNING);
    audio_input_need_warmup_ = false;
    audio_encode_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_send_queue_.clear();
    timestamp_queue_.clear();
    audio_queue_cv_.notify_all();
    const bool drained = audio_queue_cv_.wait_until(lock, deadline, [this]() {
        return !audio_input_active_ && !audio_output_active_ &&
            !audio_decode_active_ && !audio_encode_active_;
    });
    if (!drained) {
        ESP_LOGE(TAG, "Audio resource drain timed out; assistant remains suspended");
        return false;
    }
    lock.unlock();
    // Stop processors only after Feed has returned. Late output callbacks are gated.
    if (wake_word_ && wake_word_initialized_) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (wake_word_->SupportsBoundedHandoff() || session_coordinator_) {
            if (!wake_word_->StopAndDrain(static_cast<uint32_t>(std::max<int64_t>(0, remaining)))) {
                ESP_LOGE(TAG, "Wake-word drain failed; assistant remains suspended");
                return false;
            }
        } else {
            wake_word_->Stop();
        }
    }
    if (audio_processor_ && audio_processor_initialized_) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (!audio_processor_->Stop(static_cast<uint32_t>(std::max<int64_t>(0, remaining)))) {
            ESP_LOGE(TAG, "Audio processor drain timed out; assistant remains suspended");
            return false;
        }
    }
    if (codec_) {
        codec_->EnableInput(false);
        codec_->EnableOutput(false);
    }
    return true;
}
