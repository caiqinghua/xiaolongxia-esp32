#include "audio_service.h"
#include <esp_log.h>

bool AudioService::IsAssistantPlaybackDrained() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return !local_playback_generation_ && !audio_output_active_ && !audio_decode_active_ &&
        audio_decode_queue_.empty() && audio_playback_queue_.empty() && audio_testing_queue_.empty();
}

bool AudioService::EnableVoiceProcessing(bool enable) {
    std::lock_guard<std::mutex> control_lock(audio_control_mutex_);
    return EnableVoiceProcessingLocked(enable, {});
}

bool AudioService::StartCaptureWithPreroll(uint64_t generation, std::vector<int16_t>&& pcm) {
    std::lock_guard<std::mutex> control_lock(audio_control_mutex_);
    if (pcm.empty() || pcm.size() > 16000 || service_stopped_ || !session_coordinator_ ||
        !session_coordinator_->OwnsAssistantListening(generation) || !AllowsAssistantCapture() ||
        (xEventGroupGetBits(event_group_) & (AS_EVENT_AUDIO_PROCESSOR_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_LOCAL_RECORDING_RUNNING))) return false;
    return EnableVoiceProcessingLocked(true, std::move(pcm));
}

bool AudioService::EnableVoiceProcessingLocked(bool enable, std::vector<int16_t>&& prefix) {
    if (!audio_processor_ || (enable && !AllowsAssistantAudio())) return false;
    if (speech_monitor_configured_) {
        speech_monitor_generation_ = 0;
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
        audio_queue_cv_.notify_all();
        if (!audio_processor_->Stop() || !audio_processor_->SetSpeechMonitor(false)) return false;
        speech_monitor_configured_ = false;
    }
    ESP_LOGD("AudioService", "%s voice processing", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!audio_processor_initialized_) {
            audio_processor_initialized_ = audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
            if (!audio_processor_initialized_) return false;
        }
        ResetDecoder();
        audio_input_need_warmup_ = true;
        // A mode switch must not retain resampler data from a previous feed size.
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_) esp_ae_rate_cvt_reset(input_resampler_);
        }
        if (!prefix.empty() && (!audio_processor_->Stop() ||
            !audio_processor_->SetCapturePreroll(std::move(prefix)))) return false;
        if (!audio_processor_->Start()) return false;
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
        audio_queue_cv_.notify_all();
        return audio_processor_->Stop();
    }
    return true;
}

bool AudioService::IsSpeechMonitorCurrent(uint64_t generation) const {
    return generation && generation == speech_monitor_generation_.load() && !service_stopped_ &&
        AllowsAssistantAudio() && session_coordinator_ &&
        session_coordinator_->OwnsAssistantSpeaking(generation);
}

bool AudioService::StartSpeechMonitor(uint64_t generation) {
    std::lock_guard<std::mutex> lock(audio_control_mutex_);
    if (!generation || service_stopped_ || !AllowsAssistantAudio() || !session_coordinator_ ||
        !session_coordinator_->OwnsAssistantSpeaking(generation) ||
        !codec_ || !codec_->input_reference() || !audio_processor_initialized_ ||
        !audio_processor_ || !callbacks_.on_speech_monitor ||
        (xEventGroupGetBits(event_group_) & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_LOCAL_RECORDING_RUNNING))) return false;
    speech_monitor_generation_ = 0;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    audio_queue_cv_.notify_all();
    if (!audio_processor_->Stop()) return false;
    audio_processor_->OnSpeechMonitor([this, generation](const int16_t* pcm, size_t count, bool speech) {
        if (IsSpeechMonitorCurrent(generation) && callbacks_.on_speech_monitor)
            callbacks_.on_speech_monitor(generation, pcm, count, speech);
    });
    speech_monitor_configured_ = true;
    if (!audio_processor_->SetSpeechMonitor(true)) return false;
    {
        std::lock_guard<std::mutex> resampler_lock(input_resampler_mutex_);
        if (input_resampler_ && esp_ae_rate_cvt_reset(input_resampler_) != ESP_AE_ERR_OK) return false;
    }
    speech_monitor_generation_ = generation;
    if (!IsSpeechMonitorCurrent(generation) || !audio_processor_->Start()) {
        speech_monitor_generation_ = 0;
        return false;
    }
    // Monitoring must not reset or drain the decoder that is playing the reply.
    audio_input_need_warmup_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    return true;
}
