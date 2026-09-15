#pragma once

#include <esp_afe_sr_models.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include "wake_word.h"

class AfeWakeWord : public WakeWord {
public:
    // The owner serializes initialization/destruction with public calls. Wake
    // callbacks only enqueue events; they must not synchronously stop this worker.
    AfeWakeWord() = default;
    ~AfeWakeWord() override;
    bool Initialize(AudioCodec* codec, srmodel_list_t* models) override;
    void Feed(const std::vector<int16_t>& data) override;
    void OnWakeWordDetected(std::function<void(const std::string&)> callback) override;
    void Start() override;
    void Stop() override;
    bool SupportsBoundedHandoff() const override { return true; }
    bool StartChecked() override;
    bool StopAndDrain(uint32_t timeout_ms) override;
    size_t GetFeedSize() override;
    void EncodeWakeWordData() override;
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override;
    // Read after the detection callback and before rearming the detector.
    const std::string& GetLastDetectedWakeWord() const override { return last_detected_wake_word_; }

private:
    bool Halt(uint32_t timeout_ms, bool discard_pcm);
    bool IsCurrent(uint64_t generation);
    void Worker();
    void Encode(std::vector<int16_t> pcm, uint64_t generation);
    void StorePcmLocked(const int16_t* data, size_t samples);

    AudioCodec* codec_ = nullptr;
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    std::vector<std::vector<std::string>> wake_words_;
    std::string last_detected_wake_word_;
    std::function<void(const std::string&)> callback_;
    std::timed_mutex lifecycle_mutex_;
    std::timed_mutex input_mutex_;
    std::vector<int16_t> input_buffer_;
    std::mutex state_mutex_;
    std::condition_variable state_cv_;
    bool running_ = false, stopping_ = false, shutdown_ = false;
    bool worker_active_ = false, worker_exited_ = true;
    bool encode_requested_ = false, encoding_ = false, encode_done_ = true;
    uint64_t generation_ = 0;
    size_t pcm_samples_ = 0;
    std::deque<std::vector<int16_t>> pcm_;
    std::vector<int16_t> encode_pcm_;
    std::deque<std::vector<uint8_t>> opus_;
};
