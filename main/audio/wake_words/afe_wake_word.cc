#include "afe_wake_word.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_opus_enc.h>
#include <freertos/idf_additions.h>
#include <algorithm>
#include <chrono>
#include <sstream>

namespace {
constexpr auto TAG = "AfeWakeWord";
constexpr size_t kPrerollSamples = 32000;
constexpr int kFrameSamples = 960;
constexpr uint32_t kDrainMs = 3000;
}

bool AfeWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models) {
    std::lock_guard<std::timed_mutex> lifecycle(lifecycle_mutex_);
    if (afe_data_) return codec_ == codec;
    if (!codec || codec->input_channels() <= (codec->input_reference() ? 1 : 0) ||
        codec->input_channels() > 4) return false;
    // ESP-SR's model list is process-wide and shared with the speech processor.
    if (!models) models = esp_srmodel_init("model");
    if (!models) return false;
    std::string format(codec->input_channels() - (codec->input_reference() ? 1 : 0), 'M');
    if (codec->input_reference()) format.push_back('R');
    auto* config = afe_config_init(format.c_str(), models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!config) return false;
    wake_words_.clear();
    for (auto* name : {config->wakenet_model_name, config->wakenet_model_name_2}) {
        std::vector<std::string> words;
        const char* text = name ? esp_srmodel_get_wake_words(models, name) : nullptr;
        if (text) {
            std::stringstream stream(text);
            std::string word;
            while (std::getline(stream, word, ';')) words.push_back(word);
        }
        wake_words_.push_back(std::move(words));
    }
    if (wake_words_[0].empty()) {
        afe_config_free(config);
        ESP_LOGE(TAG, "No configured wake-word model is available");
        return false;
    }
    config->aec_init = codec->input_reference();
    config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    config->afe_perferred_core = 1;
    config->afe_perferred_priority = 1;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_iface_ = esp_afe_handle_from_config(config);
    if (afe_iface_) afe_data_ = afe_iface_->create_from_config(config);
    afe_config_free(config);
    if (afe_data_ && (afe_iface_->get_feed_chunksize(afe_data_) <= 0 ||
        afe_iface_->get_feed_chunksize(afe_data_) > static_cast<int>(kPrerollSamples) ||
        afe_iface_->get_fetch_chunksize(afe_data_) <= 0 ||
        afe_iface_->get_fetch_chunksize(afe_data_) > static_cast<int>(kPrerollSamples))) {
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
    }
    if (!afe_data_) return false;
    codec_ = codec;
    worker_exited_ = false;
    // One owned worker serializes fetch and preroll encoding. The SDK owns its
    // PSRAM stack; it is never freed while the task is still alive.
    if (xTaskCreateWithCaps([](void* arg) {
            static_cast<AfeWakeWord*>(arg)->Worker();
            vTaskDeleteWithCaps(nullptr);
        }, "wake_word", 24576, this, 5, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        worker_exited_ = true;
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        codec_ = nullptr;
        ESP_LOGE(TAG, "Wake-word worker creation failed");
        return false;
    }
    return true;
}

AfeWakeWord::~AfeWakeWord() {
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        shutdown_ = true;
        running_ = false;
        ++generation_;
        state_cv_.notify_all();
        state_cv_.wait(lock, [this]() { return worker_exited_; });
    }
    std::lock_guard<std::timed_mutex> input(input_mutex_);
    if (afe_data_) afe_iface_->destroy(afe_data_);
}

void AfeWakeWord::OnWakeWordDetected(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    callback_ = std::move(callback);
}

void AfeWakeWord::Start() { (void)StartChecked(); }

bool AfeWakeWord::StartChecked() {
    std::lock_guard<std::timed_mutex> lifecycle(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!afe_data_ || worker_exited_ || stopping_ || shutdown_) return false;
        if (running_) return true;
    }
    if (!Halt(kDrainMs, true)) return false;
    std::lock_guard<std::mutex> lock(state_mutex_);
    running_ = true;
    state_cv_.notify_all();
    return true;
}

void AfeWakeWord::Stop() {
    std::lock_guard<std::timed_mutex> lifecycle(lifecycle_mutex_);
    if (!Halt(kDrainMs, false)) ESP_LOGE(TAG, "Wake-word stop failed; restart remains inhibited");
}

bool AfeWakeWord::StopAndDrain(uint32_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::timed_mutex> lifecycle(lifecycle_mutex_, std::defer_lock);
    if (!lifecycle.try_lock_until(deadline)) return false;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    return Halt(static_cast<uint32_t>(std::max<int64_t>(0, remaining)), true);
}

bool AfeWakeWord::Halt(uint32_t timeout_ms, bool discard_pcm) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        running_ = false;
        stopping_ = true;
        ++generation_;
        encode_requested_ = false;
        encode_done_ = true;
        encode_pcm_.clear();
        opus_.clear();
        if (discard_pcm) { pcm_.clear(); pcm_samples_ = 0; }
        state_cv_.notify_all();
        if (!state_cv_.wait_until(lock, deadline, [this]() { return !worker_active_; })) return false;
    }
    std::unique_lock<std::timed_mutex> input(input_mutex_, std::defer_lock);
    if (!input.try_lock_until(deadline)) return false;
    if (afe_data_ && afe_iface_->reset_buffer(afe_data_) < 0) {
        ESP_LOGE(TAG, "Wake-word buffer reset failed; restart remains inhibited");
        return false;
    }
    input_buffer_.clear();
    std::lock_guard<std::mutex> lock(state_mutex_);
    stopping_ = false;
    return true;
}

void AfeWakeWord::Feed(const std::vector<int16_t>& data) {
    std::lock_guard<std::timed_mutex> input(input_mutex_);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!running_ || stopping_ || shutdown_) return;
    }
    const size_t channels = codec_->input_channels();
    if (data.size() % channels != 0 || data.size() > kPrerollSamples * channels) return;
    const size_t chunk = GetFeedSize() * channels;
    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    while (input_buffer_.size() >= chunk) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!running_ || stopping_ || shutdown_) break;
        }
        afe_iface_->feed(afe_data_, input_buffer_.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk);
    }
}

size_t AfeWakeWord::GetFeedSize() {
    return afe_data_ ? afe_iface_->get_feed_chunksize(afe_data_) : 0;
}

void AfeWakeWord::StorePcmLocked(const int16_t* data, size_t samples) {
    pcm_.emplace_back(data, data + samples);
    pcm_samples_ += samples;
    while (pcm_samples_ > kPrerollSamples) {
        const size_t drop = std::min(pcm_.front().size(), pcm_samples_ - kPrerollSamples);
        if (drop == pcm_.front().size()) pcm_.pop_front();
        else pcm_.front().erase(pcm_.front().begin(), pcm_.front().begin() + drop);
        pcm_samples_ -= drop;
    }
}

void AfeWakeWord::Worker() {
    std::unique_lock<std::mutex> lock(state_mutex_);
    while (true) {
        state_cv_.wait(lock, [this]() { return running_ || encode_requested_ || shutdown_; });
        if (shutdown_) break;
        worker_active_ = true;
        const auto generation = generation_;
        if (encode_requested_) {
            encode_requested_ = false;
            encoding_ = true;
            auto pcm = std::move(encode_pcm_);
            lock.unlock();
            Encode(std::move(pcm), generation);
            lock.lock();
            encoding_ = false;
            if (generation == generation_) encode_done_ = true;
        } else {
            lock.unlock();
            auto* result = afe_iface_->fetch_with_delay(afe_data_, pdMS_TO_TICKS(100));
            lock.lock();
            if (running_ && generation == generation_ && !shutdown_ && result &&
                result->ret_value != ESP_FAIL && result->data && result->data_size > 0 &&
                result->data_size % sizeof(int16_t) == 0 &&
                result->data_size / sizeof(int16_t) <= kPrerollSamples) {
                StorePcmLocked(result->data, result->data_size / sizeof(int16_t));
                const int64_t model = static_cast<int64_t>(result->wakenet_model_index) - 1;
                const int64_t word = static_cast<int64_t>(result->wake_word_index) - 1;
                if (result->wakeup_state == WAKENET_DETECTED && model >= 0 && word >= 0 &&
                    static_cast<size_t>(model) < wake_words_.size() &&
                    static_cast<size_t>(word) < wake_words_[model].size() &&
                    !wake_words_[model][word].empty()) {
                    running_ = false;
                    last_detected_wake_word_ = wake_words_[model][word];
                    const auto detected = last_detected_wake_word_;
                    const auto callback = callback_;
                    lock.unlock();
                    // StopAndDrain waits for this callback as well as fetch/encode.
                    if (callback) callback(detected);
                    lock.lock();
                }
            }
        }
        worker_active_ = false;
        state_cv_.notify_all();
    }
    worker_exited_ = true;
    state_cv_.notify_all();
}

void AfeWakeWord::EncodeWakeWordData() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!afe_data_ || worker_exited_ || shutdown_ || stopping_ || encoding_ || encode_requested_ ||
        pcm_samples_ == 0) return;
    running_ = false;
    ++generation_;
    opus_.clear();
    encode_pcm_.clear();
    encode_pcm_.reserve(pcm_samples_);
    for (const auto& pcm : pcm_) encode_pcm_.insert(encode_pcm_.end(), pcm.begin(), pcm.end());
    pcm_.clear();
    pcm_samples_ = 0;
    encode_done_ = encode_pcm_.empty();
    encode_requested_ = !encode_done_;
    state_cv_.notify_all();
}

bool AfeWakeWord::IsCurrent(uint64_t generation) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return generation == generation_ && !stopping_ && !shutdown_;
}

void AfeWakeWord::Encode(std::vector<int16_t> pcm, uint64_t generation) {
    if (!IsCurrent(generation)) return;
    esp_opus_enc_config_t config = {};
    config.sample_rate = 16000;
    config.channel = 1;
    config.bits_per_sample = 16;
    config.bitrate = ESP_OPUS_BITRATE_AUTO;
    config.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    config.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    config.enable_dtx = true;
    config.enable_vbr = true;
    void* encoder = nullptr;
    const auto opened = esp_opus_enc_open(&config, sizeof(config), &encoder);
    if (opened != ESP_AUDIO_ERR_OK || !encoder) {
        if (encoder) esp_opus_enc_close(encoder);
        ESP_LOGE(TAG, "Wake-word encoder creation failed");
        return;
    }
    int frame_bytes = 0, buffer_bytes = 0;
    const auto sized = esp_opus_enc_get_frame_size(encoder, &frame_bytes, &buffer_bytes);
    if (sized == ESP_AUDIO_ERR_OK && frame_bytes == kFrameSamples * sizeof(int16_t) &&
        buffer_bytes > 0 && buffer_bytes <= 4096) {
        for (size_t offset = 0; offset + kFrameSamples <= pcm.size() && IsCurrent(generation);
            offset += kFrameSamples) {
            std::vector<uint8_t> packet(buffer_bytes);
            esp_audio_enc_in_frame_t in = {};
            in.buffer = reinterpret_cast<uint8_t*>(pcm.data() + offset);
            in.len = frame_bytes;
            esp_audio_enc_out_frame_t out = {};
            out.buffer = packet.data();
            out.len = buffer_bytes;
            if (esp_opus_enc_process(encoder, &in, &out) != ESP_AUDIO_ERR_OK ||
                out.encoded_bytes <= 0 || out.encoded_bytes > packet.size()) {
                ESP_LOGE(TAG, "Wake-word encoding failed or returned an invalid packet");
                break;
            }
            packet.resize(out.encoded_bytes);
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (generation != generation_ || stopping_ || shutdown_) break;
            opus_.push_back(std::move(packet));
            state_cv_.notify_all();
        }
    } else {
        ESP_LOGE(TAG, "Wake-word encoder returned an invalid frame configuration");
    }
    esp_opus_enc_close(encoder);
}

bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    const auto generation = generation_;
    opus.clear();
    if (!state_cv_.wait_for(lock, std::chrono::milliseconds(kDrainMs), [this, generation]() {
            return !opus_.empty() || encode_done_ || generation != generation_ || shutdown_;
        })) {
        ++generation_;
        encode_requested_ = false;
        encode_done_ = true;
        encode_pcm_.clear();
        opus_.clear();
        state_cv_.notify_all();
        ESP_LOGE(TAG, "Wake-word preroll timed out and was discarded");
        return false;
    }
    if (generation != generation_ || shutdown_ || opus_.empty()) return false;
    opus = std::move(opus_.front());
    opus_.pop_front();
    return true;
}
