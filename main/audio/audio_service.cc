#include "audio_service.h"
#include <esp_log.h>
#include <algorithm>
#include <cstring>
#include <limits>

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)        \
    (esp_ae_rate_cvt_cfg_t)                                  \
    {                                                        \
        .src_rate        = (uint32_t)(_src_rate),            \
        .dest_rate       = (uint32_t)(_dest_rate),           \
        .channel         = (uint8_t)(_channel),              \
        .bits_per_sample = ESP_AUDIO_BIT16,                  \
        .complexity      = 2,                                \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,  \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                                    \
    (esp_opus_dec_cfg_t)                                                                                  \
    {                                                                                                     \
        .sample_rate    = (uint32_t)(_sample_rate),                                                       \
        .channel        = ESP_AUDIO_MONO,                                                                 \
        .frame_duration = (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms),  \
        .self_delimited = false,                                                                          \
    }

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#include "wake_words/afe_wake_word.h"
#include "wake_words/custom_wake_word.h"
#else
#include "wake_words/esp_wake_word.h"
#endif

#define TAG "AudioService"

namespace {

constexpr uint32_t kLocalRecordingLevelLogIntervalFrames = 5000 / OPUS_FRAME_DURATION_MS;

class InputActivity {
public:
    InputActivity(std::mutex& mutex, std::condition_variable& cv, bool& active)
        : mutex_(mutex), cv_(cv), active_(active) {
        active_ = true;
    }
    ~InputActivity() {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        cv_.notify_all();
    }
private:
    std::mutex& mutex_;
    std::condition_variable& cv_;
    bool& active_;
};

uint32_t SampleMagnitude(int16_t sample) {
    return sample == std::numeric_limits<int16_t>::min()
        ? 32768
        : static_cast<uint32_t>(sample < 0 ? -sample : sample);
}

uint32_t PeakMagnitude(const std::vector<int16_t>& data) {
    uint32_t peak = 0;
    for (const int16_t sample : data) {
        peak = std::max(peak, SampleMagnitude(sample));
    }
    return peak;
}

uint32_t InterleavedChannelPeak(
    const std::vector<int16_t>& data,
    int channel_count,
    int channel_index
) {
    if (channel_count <= 0 || channel_index < 0 || channel_index >= channel_count) {
        return 0;
    }

    uint32_t peak = 0;
    for (size_t index = channel_index; index < data.size(); index += channel_count) {
        peak = std::max(peak, SampleMagnitude(data[index]));
    }
    return peak;
}

}  // namespace

AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
    }
}

void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(codec->output_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
    } else {
        decoder_sample_rate_ = codec->output_sample_rate();
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create input resampler, error code: %d", resampler_ret);
        }
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        if (!AllowsAssistantAudio()) {
            return;
        }
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::InitializeForLocalRecording(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();
    const int reference_channels = codec_->input_reference() ? 1 : 0;
    ESP_LOGI(
        TAG,
        "Local recording input configured: channels=%d microphones=%d references=%d",
        codec_->input_channels(),
        codec_->input_channels() - reference_channels,
        reference_channels
    );

    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    const auto ret = esp_opus_enc_open(
        &opus_enc_cfg,
        sizeof(esp_opus_enc_config_t),
        &opus_encoder_
    );
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create local recording encoder, error code: %d", ret);
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(
            opus_encoder_,
            &encoder_frame_size_,
            &encoder_outbuf_size_
        );
        encoder_frame_size_ /= sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(),
            ESP_AUDIO_SAMPLE_RATE_16K,
            codec->input_channels()
        );
        const auto resampler_ret = esp_ae_rate_cvt_open(
            &input_resampler_cfg,
            &input_resampler_
        );
        if (input_resampler_ == nullptr) {
            ESP_LOGE(
                TAG,
                "Failed to create local recording input resampler, error code: %d",
                resampler_ret
            );
        }
    }

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            static_cast<AudioService*>(arg)->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

bool AudioService::Start() {
    return StartTasks(false);
}

bool AudioService::StartForLocalRecording() {
    return StartTasks(true);
}

bool AudioService::WaitForTasksStoppedLocked(std::unique_lock<std::mutex>& lock, uint32_t timeout_ms) {
    return audio_queue_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() {
        return audio_input_task_handle_ == nullptr && audio_output_task_handle_ == nullptr &&
            opus_codec_task_handle_ == nullptr;
    });
}

bool AudioService::StartTasks(bool recording_only) {
    std::lock_guard<std::mutex> control_lock(audio_control_mutex_);
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (!service_stopped_) {
        return assistant_capable_ == !recording_only;
    }
    if (!WaitForTasksStoppedLocked(lock, 3000) || codec_ == nullptr || opus_encoder_ == nullptr ||
        encoder_frame_size_ <= 0 || encoder_outbuf_size_ <= 0 ||
        event_group_ == nullptr || audio_power_timer_ == nullptr) {
        ESP_LOGE(TAG, "Audio tasks cannot start because resources are unavailable");
        return false;
    }
    if ((codec_->input_sample_rate() != 16000 && input_resampler_ == nullptr) ||
        (!recording_only && (opus_decoder_ == nullptr || audio_processor_ == nullptr))) {
        ESP_LOGE(TAG, "Audio processing resources are unavailable");
        return false;
    }
    assistant_capable_ = !recording_only;
    model_validation_generation_ = 0;
    assistant_suspended_ = false;
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_LOCAL_RECORDING_RUNNING | AS_EVENT_MODEL_VALIDATION_RUNNING);
    auto input_entry = [](void* arg) {
        auto* service = static_cast<AudioService*>(arg);
        service->AudioInputTask();
        {
            std::lock_guard<std::mutex> lock(service->audio_queue_mutex_);
            service->audio_input_task_handle_ = nullptr;
            service->audio_queue_cv_.notify_all();
        }
        vTaskDelete(nullptr);
    };
    auto output_entry = [](void* arg) {
        auto* service = static_cast<AudioService*>(arg);
        service->AudioOutputTask();
        {
            std::lock_guard<std::mutex> lock(service->audio_queue_mutex_);
            service->audio_output_task_handle_ = nullptr;
            service->audio_queue_cv_.notify_all();
        }
        vTaskDelete(nullptr);
    };
    auto opus_entry = [](void* arg) {
        auto* service = static_cast<AudioService*>(arg);
        service->OpusCodecTask();
        {
            std::lock_guard<std::mutex> lock(service->audio_queue_mutex_);
            service->opus_codec_task_handle_ = nullptr;
            service->audio_queue_cv_.notify_all();
        }
        vTaskDelete(nullptr);
    };
    bool created = xTaskCreate(opus_entry, "opus_codec", 2048 * 12, this, 2, &opus_codec_task_handle_) == pdPASS;
#if CONFIG_USE_AUDIO_PROCESSOR
    if (created && !recording_only) {
        created = xTaskCreatePinnedToCore(input_entry, "audio_input", 2048 * 3, this, 8,
            &audio_input_task_handle_, 0) == pdPASS;
    } else if (created) {
        created = xTaskCreate(input_entry, "record_input", 2048 * 2, this, 8, &audio_input_task_handle_) == pdPASS;
    }
#else
    if (created) {
        created = xTaskCreate(input_entry, "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_) == pdPASS;
    }
#endif
    if (created && !recording_only) {
        created = xTaskCreate(output_entry, "audio_output", 2048 * 2, this, 4, &audio_output_task_handle_) == pdPASS;
    }
    if (created) {
        created = esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000) == ESP_OK;
    }
    if (!created) {
        service_stopped_ = true;
        xEventGroupSetBits(event_group_, AS_EVENT_LOCAL_RECORDING_RUNNING);
        audio_queue_cv_.notify_all();
        WaitForTasksStoppedLocked(lock, 3000);
        ESP_LOGE(TAG, "Audio task creation failed; recording is unavailable");
        return false;
    }
    return true;
}

bool AudioService::Stop() {
    std::lock_guard<std::mutex> control_lock(audio_control_mutex_);
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (service_stopped_) {
        return WaitForTasksStoppedLocked(lock, 3000);
    }
    if (local_playback_generation_ || local_recording_packet_callback_ || local_recording_encode_active_ ||
        (xEventGroupGetBits(event_group_) & AS_EVENT_LOCAL_RECORDING_RUNNING) != 0) {
        ESP_LOGE(TAG, "Audio shutdown rejected while recording still owns resources");
        return false;
    }
    lock.unlock();
    if (!QuiesceAssistantLocked(3000)) {
        return false;
    }
    lock.lock();
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING |
        AS_EVENT_LOCAL_RECORDING_RUNNING);

    audio_encode_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
    const bool stopped = WaitForTasksStoppedLocked(lock, 3000);
    if (!stopped) {
        ESP_LOGE(TAG, "Audio tasks did not stop before the shutdown deadline");
    }
    return stopped;
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (service_stopped_ || !codec_ || audio_input_active_ || !AllowsAssistantAudio()) {
        return false;
    }
    // External readers such as acoustic provisioning share the same drain barrier.
    InputActivity activity(audio_queue_mutex_, audio_queue_cv_, audio_input_active_);
    lock.unlock();
    return ReadAudioDataInternal(data, sample_rate, samples);
}

bool AudioService::ReadAudioDataInternal(std::vector<int16_t>& data, int sample_rate, int samples, bool allow_debug) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (input_resampler_ != nullptr) {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            uint32_t output_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num, &output_samples);
            auto resampled = std::vector<int16_t>(output_samples * codec_->input_channels());
            uint32_t actual_output = output_samples;
            esp_ae_rate_cvt_process(input_resampler_, (esp_ae_sample_t)data.data(), in_sample_num,
                                   (esp_ae_sample_t)resampled.data(), &actual_output);
            resampled.resize(actual_output * codec_->input_channels());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        last_input_time_ = std::chrono::steady_clock::now();
    }
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // Feed raw audio samples to the audio debugger.
    if (allow_debug) {
        if (audio_debugger_ == nullptr) {
            audio_debugger_ = std::make_unique<AudioDebugger>();
        }
        audio_debugger_->Feed(data);
    }
#else
    (void)allow_debug;
#endif

    return true;
}

void AudioService::AudioInputTask() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_LOCAL_RECORDING_RUNNING |
            AS_EVENT_MODEL_VALIDATION_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_.exchange(false)) {
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        std::unique_lock<std::mutex> input_lock(audio_queue_mutex_);
        bits = xEventGroupGetBits(event_group_);
        if (service_stopped_) {
            break;
        }
        if (audio_input_active_ || ((bits & AS_EVENT_LOCAL_RECORDING_RUNNING) == 0 &&
            !((bits & AS_EVENT_MODEL_VALIDATION_RUNNING) && IsModelValidationCurrent(model_validation_generation_.load())) &&
            (!AllowsAssistantAudio() || (bits & (AS_EVENT_AUDIO_TESTING_RUNNING |
                AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) == 0))) {
            input_lock.unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        InputActivity input_activity(audio_queue_mutex_, audio_queue_cv_, audio_input_active_);
        input_lock.unlock();

        if (bits & AS_EVENT_LOCAL_RECORDING_RUNNING) {
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioDataInternal(data, 16000, samples)) {
                data = ProcessLocalRecordingAudio(std::move(data));
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToLocalRecording, std::move(data));
                continue;
            }
            ESP_LOGW(TAG, "Local recording input read failed; retrying");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (bits & AS_EVENT_MODEL_VALIDATION_RUNNING) {
            if (!ProcessModelValidationInput()) vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                    ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
                    if (AllowsAssistantAudio()) {
                        audio_decode_queue_ = std::move(audio_testing_queue_);
                        audio_queue_cv_.notify_all();
                    }
                    continue;
                }
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioDataInternal(data, 16000, samples) && AllowsAssistantAudio()) {
                data = DownmixInputToMono(std::move(data));
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the wake word and/or audio processor */
        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            const auto wake_generation = active_wake_word_generation_.load();
            const bool wake_allowed = (bits & AS_EVENT_WAKE_WORD_RUNNING) &&
                IsWakeWordGenerationCurrent(wake_generation);
            const bool capture_allowed = (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) &&
                (AllowsAssistantCapture() || IsSpeechMonitorCurrent(speech_monitor_generation_.load()));
            if (!wake_allowed && !capture_allowed) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            int samples = 160; // 10ms
            std::vector<int16_t> data;
            if (ReadAudioDataInternal(data, 16000, samples) && AllowsAssistantAudio()) {
                if (wake_allowed && IsWakeWordGenerationCurrent(wake_generation)) {
                    wake_word_->Feed(data);
                    wake_input_feeds_.fetch_add(1, std::memory_order_relaxed);
                }
                if ((bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) &&
                    (AllowsAssistantCapture() || IsSpeechMonitorCurrent(speech_monitor_generation_.load()))) {
                    audio_processor_->Feed(std::move(data));
                }
                continue;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

std::vector<int16_t> AudioService::DownmixInputToMono(std::vector<int16_t>&& data) const {
    const int channels = codec_ == nullptr ? 1 : codec_->input_channels();
    if (channels <= 1 || data.empty()) {
        return std::move(data);
    }

    const int microphone_channels = channels - (codec_->input_reference() ? 1 : 0);
    if (microphone_channels <= 0) {
        return {};
    }

    const size_t frame_count = data.size() / channels;
    std::vector<int16_t> mono_data(frame_count);
    for (size_t frame = 0; frame < frame_count; ++frame) {
        int32_t sum = 0;
        const size_t base = frame * channels;
        for (int channel = 0; channel < microphone_channels; ++channel) {
            sum += data[base + channel];
        }
        int32_t sample = sum / microphone_channels;
        sample = std::clamp(
            sample,
            static_cast<int32_t>(std::numeric_limits<int16_t>::min()),
            static_cast<int32_t>(std::numeric_limits<int16_t>::max())
        );
        mono_data[frame] = static_cast<int16_t>(sample);
    }
    return mono_data;
}

std::vector<int16_t> AudioService::ProcessLocalRecordingAudio(std::vector<int16_t>&& data) {
    const int channel_count = codec_ == nullptr ? 1 : codec_->input_channels();
    const int microphone_count = channel_count - (
        codec_ != nullptr && codec_->input_reference() ? 1 : 0
    );
    const bool should_log_levels = local_recording_level_frame_count_ == 0 ||
        local_recording_level_frame_count_ >= kLocalRecordingLevelLogIntervalFrames;
    const uint32_t microphone_1_peak = should_log_levels
        ? InterleavedChannelPeak(data, channel_count, 0)
        : 0;
    const uint32_t microphone_2_peak = should_log_levels && microphone_count > 1
        ? InterleavedChannelPeak(data, channel_count, 1)
        : 0;
    const uint32_t reference_peak = should_log_levels &&
        codec_ != nullptr && codec_->input_reference()
        ? InterleavedChannelPeak(data, channel_count, microphone_count)
        : 0;

    auto mono_data = DownmixInputToMono(std::move(data));
    const uint32_t mixed_peak = should_log_levels ? PeakMagnitude(mono_data) : 0;

    if (should_log_levels) {
        ESP_LOGI(
            TAG,
            "Local recording levels: mic1_peak=%lu mic2_peak=%lu reference_peak=%lu mixed_peak=%lu",
            static_cast<unsigned long>(microphone_1_peak),
            static_cast<unsigned long>(microphone_2_peak),
            static_cast<unsigned long>(reference_peak),
            static_cast<unsigned long>(mixed_peak)
        );
        local_recording_level_frame_count_ = 1;
    } else {
        ++local_recording_level_frame_count_;
    }

    return mono_data;
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_; });
        if (service_stopped_) {
            break;
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        audio_queue_cv_.notify_all();
        if (!AllowsAssistantAudio() || task->generation != audio_generation_) {
            continue;
        }
        audio_output_active_ = true;
        lock.unlock();

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }

        codec_->OutputData(task->pcm);

        /* Update the last output time */
        lock.lock();
        audio_output_active_ = false;
        last_output_time_ = std::chrono::steady_clock::now();
        audio_queue_cv_.notify_all();
        debug_statistics_.playback_count++;

#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp for server AEC */
        if (task->timestamp > 0) {
            timestamp_queue_.push_back(task->timestamp);
        }
#endif
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}


void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (decoder_sample_rate_ == sample_rate && decoder_duration_ms_ == frame_duration) {
        return;
    }
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    decoder_lock.unlock();
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
        return;
    }
    decoder_sample_rate_ = sample_rate;
    decoder_duration_ms_ = frame_duration;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (decoder_sample_rate_ != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", decoder_sample_rate_, codec->output_sample_rate());
        if (output_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(output_resampler_);
            output_resampler_ = nullptr;
        }
        esp_ae_rate_cvt_cfg_t output_resampler_cfg = RATE_CVT_CFG(
            decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
        }
    }
}


bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (service_stopped_ || !AllowsAssistantAudio()) {
        return false;
    }
    const uint64_t generation = audio_generation_;
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this, generation]() {
                return service_stopped_ || !AllowsAssistantAudio() || generation != audio_generation_ ||
                    audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE;
            });
        } else {
            return false;
        }
    }
    if (service_stopped_ || !AllowsAssistantAudio() || generation != audio_generation_) {
        return false;
    }
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    return wake_word_->GetLastDetectedWakeWord();
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}


void AudioService::EnableAudioTesting(bool enable) {
    std::lock_guard<std::mutex> control_lock(audio_control_mutex_);
    if (enable && !AllowsAssistantAudio()) {
        return;
    }
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (AllowsAssistantAudio()) {
            audio_decode_queue_ = std::move(audio_testing_queue_);
        } else {
            audio_testing_queue_.clear();
        }
        audio_queue_cv_.notify_all();
    }
}


bool AudioService::SetAssistantCaptureGuard(std::function<bool()> guard) {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (!service_stopped_ || assistant_capture_guard_ || !guard) return false;
    assistant_capture_guard_ = std::move(guard);
    return true;
}

void AudioService::SetSessionCoordinator(AudioSessionCoordinator* coordinator) {
    // The coordinator must outlive this service and be assigned before tasks start.
    if (!service_stopped_) {
        ESP_LOGE(TAG, "Cannot replace the audio coordinator while tasks are running");
        return;
    }
    session_coordinator_ = coordinator;
}


bool AudioService::ResumeAssistantAudio() {
    std::lock_guard<std::mutex> control_lock(audio_control_mutex_);
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (service_stopped_ || local_playback_generation_ || HasLocalRecordingEncodeWorkLocked() ||
        (xEventGroupGetBits(event_group_) & AS_EVENT_LOCAL_RECORDING_RUNNING) != 0 ||
        (session_coordinator_ && !session_coordinator_->AllowsAssistantResources())) {
        return false;
    }
    assistant_suspended_ = false;
    return true;
}

bool AudioService::EnableLocalRecording(bool enable) {
    std::lock_guard<std::mutex> control_lock(audio_control_mutex_);
    ESP_LOGI(TAG, "%s local recording", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (service_stopped_ || codec_ == nullptr || opus_encoder_ == nullptr ||
            !QuiesceAssistantLocked(3000)) {
            ESP_LOGE(TAG, "Local recording rejected because audio resources are not ready");
            return false;
        }
        local_recording_level_frame_count_ = 0;
        codec_->PrepareForLocalRecording();
        codec_->EnableInput(true);
        if (!codec_->input_enabled()) {
            ESP_LOGE(TAG, "Local recording input failed to open");
            return false;
        }
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        xEventGroupSetBits(event_group_, AS_EVENT_LOCAL_RECORDING_RUNNING);
    } else {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        xEventGroupClearBits(event_group_, AS_EVENT_LOCAL_RECORDING_RUNNING);
        audio_queue_cv_.notify_all();
    }
    return true;
}

bool AudioService::WaitForLocalRecordingDrain(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    const auto timeout = std::chrono::milliseconds(timeout_ms);
    const bool drained = audio_queue_cv_.wait_for(lock, timeout, [this]() {
        return !HasLocalRecordingEncodeWorkLocked();
    });
    if (drained && (xEventGroupGetBits(event_group_) & (AS_EVENT_LOCAL_RECORDING_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_MODEL_VALIDATION_RUNNING)) == 0) {
        codec_->EnableInput(false);
    }
    return drained;
}


void AudioService::EnableDeviceAec(bool enable) {
    std::lock_guard<std::mutex> control_lock(audio_control_mutex_);
    if (!AllowsAssistantAudio() || !audio_processor_) {
        return;
    }
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    if (!audio_processor_initialized_) {
        audio_processor_initialized_ = audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
        if (!audio_processor_initialized_) return;
    }

    const bool restart = (xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING) != 0;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    audio_queue_cv_.notify_all();
    if (!audio_processor_->Stop()) {
        ESP_LOGE(TAG, "Audio processor is still draining; AEC mode unchanged");
        return;
    }
    audio_processor_->EnableDeviceAec(enable);
    if (restart && audio_processor_->Start()) xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::PlaySound(const std::string_view& ogg) {
    if (!AllowsAssistantAudio() || codec_ == nullptr) {
        return;
    }

    const auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();

    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnDemuxerFinished([this](const uint8_t* data, int sample_rate, size_t size){
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = 60;
        packet->payload.resize(size);
        std::memcpy(packet->payload.data(), data, size);
        PushPacketToDecodeQueue(std::move(packet), true);
    });
    demuxer->Reset();
    demuxer->Process(buf, size);
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return !local_playback_generation_ && !audio_input_active_ && !audio_output_active_ && !audio_decode_active_ && !audio_encode_active_ &&
        (xEventGroupGetBits(event_group_) & AS_EVENT_MODEL_VALIDATION_RUNNING) == 0 &&
        audio_encode_queue_.empty() && audio_decode_queue_.empty() && audio_playback_queue_.empty() && audio_testing_queue_.empty();
}

bool AudioService::IsAssistantCaptureDrained() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return !audio_input_active_ && !audio_encode_active_ &&
        audio_encode_queue_.empty() && audio_send_queue_.empty() &&
        (xEventGroupGetBits(event_group_) & (AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_MODEL_VALIDATION_RUNNING)) == 0;
}

void AudioService::WaitForPlaybackQueueEmpty() {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait(lock, [this]() { 
        return service_stopped_ || !AllowsAssistantAudio() ||
            (!audio_decode_active_ && !audio_output_active_ && audio_decode_queue_.empty() && audio_playback_queue_.empty());
    });
}

void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    ++audio_generation_;
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_reset(opus_decoder_);
    }
    decoder_lock.unlock();
    timestamp_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

void AudioService::CheckAndUpdateAudioPowerState() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (local_playback_generation_) return;
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && !audio_input_active_ && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && !audio_output_active_ && codec_->output_enabled()) {
        codec_->EnableOutput(false);
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

bool AudioService::CreateWatchWakeWord(srmodel_list_t* models) {
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    if (!models || esp_srmodel_filter(models, ESP_WN_PREFIX, nullptr) == nullptr) return false;
    wake_word_ = std::make_unique<AfeWakeWord>();
    return true;
#else
    (void)models;
    return false;
#endif
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    models_list_ = models_list;

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    if (esp_srmodel_filter(models_list_, ESP_MN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<CustomWakeWord>();
    } else if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<AfeWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#else
    if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<EspWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#endif

    active_wake_word_generation_ = 0;
}

bool AudioService::IsAfeWakeWord() {
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    return wake_word_ != nullptr && dynamic_cast<AfeWakeWord*>(wake_word_.get()) != nullptr;
#else
    return false;
#endif
}
