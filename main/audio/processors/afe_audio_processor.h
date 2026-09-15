#ifndef AFE_AUDIO_PROCESSOR_H
#define AFE_AUDIO_PROCESSOR_H

#include <esp_afe_sr_models.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>

#include "audio_processor.h"
#include "audio_codec.h"

class AfeAudioProcessor : public AudioProcessor {
public:
    AfeAudioProcessor();
    ~AfeAudioProcessor();

    bool Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    void Feed(std::vector<int16_t>&& data) override;
    bool Start() override;
    bool Stop(uint32_t timeout_ms = 3000) override;
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void EnableDeviceAec(bool enable) override;
    bool SetLocalEndpointing(bool enable) override;
    void OnSpeechMonitor(SpeechMonitorCallback callback) override;
    bool SetSpeechMonitor(bool enable) override;
    bool SetCapturePreroll(std::vector<int16_t>&& pcm) override;

private:
    std::mutex state_mutex_;
    std::condition_variable state_cv_;
    bool running_ = false;
    bool stopping_ = false;
    bool shutdown_ = false;
    bool worker_active_ = false;
    bool worker_exited_ = true;
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    std::function<void(std::vector<int16_t>&& data)> output_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;
    AudioCodec* codec_ = nullptr;
    int frame_samples_ = 0;
    bool is_speaking_ = false;
    bool local_endpointing_ = false;
    bool speech_monitor_ = false;
    SpeechMonitorCallback speech_monitor_callback_;
    bool device_aec_enabled_ = false;
    bool mode_ready_ = true;
    bool aec_mode_uncertain_ = false;
    std::vector<int16_t> input_buffer_;
    std::timed_mutex input_buffer_mutex_;
    std::vector<int16_t> output_buffer_;

    void AudioProcessorTask();
};

#endif
