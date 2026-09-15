#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#include <memory>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <mutex>
#include <atomic>
#include "audio_session_coordinator.h"
#include "playback_volume.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <model_path.h>
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"

#include "audio_codec.h"
#include "audio_processor.h"
#include "processors/audio_debugger.h"
#include "wake_word.h"
#include "protocol.h"
#include "ogg_demuxer.h"

/*
 * There are two types of audio data flow:
 * 1. (MIC) -> [Processors] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)
 * 2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)
 *
 * We use one task for MIC / Speaker / Processors, and one task for Opus Encoder / Opus Decoder.
 * 
 * Decode Queue and Send Queue are the main queues, because Opus packets are quite smaller than PCM packets.
 * 
 */

#define OPUS_FRAME_DURATION_MS 60
#define MAX_ENCODE_TASKS_IN_QUEUE 2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000
#define MAX_TIMESTAMPS_IN_QUEUE 3

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000

#define AS_EVENT_AUDIO_TESTING_RUNNING      (1 << 0)
#define AS_EVENT_WAKE_WORD_RUNNING          (1 << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING    (1 << 2)
#define AS_EVENT_PLAYBACK_NOT_EMPTY         (1 << 3)
#define AS_EVENT_LOCAL_RECORDING_RUNNING    (1 << 4)
#define AS_EVENT_MODEL_VALIDATION_RUNNING   (1 << 5)

#define AS_OPUS_GET_FRAME_DRU_ENUM(duration_ms)                   \
    ((duration_ms) == 5 ? ESP_OPUS_ENC_FRAME_DURATION_5_MS :      \
     (duration_ms) == 10 ? ESP_OPUS_ENC_FRAME_DURATION_10_MS :    \
     (duration_ms) == 20 ? ESP_OPUS_ENC_FRAME_DURATION_20_MS :    \
     (duration_ms) == 40 ? ESP_OPUS_ENC_FRAME_DURATION_40_MS :    \
     (duration_ms) == 60 ? ESP_OPUS_ENC_FRAME_DURATION_60_MS :    \
     (duration_ms) == 80 ? ESP_OPUS_ENC_FRAME_DURATION_80_MS :    \
     (duration_ms) == 100 ? ESP_OPUS_ENC_FRAME_DURATION_100_MS :  \
     (duration_ms) == 120 ? ESP_OPUS_ENC_FRAME_DURATION_120_MS : -1)

#define AS_OPUS_ENC_CONFIG() {                                                                                    \
        .sample_rate        = ESP_AUDIO_SAMPLE_RATE_16K,                                                          \
        .channel            = ESP_AUDIO_MONO,                                                                     \
        .bits_per_sample    = ESP_AUDIO_BIT16,                                                                    \
        .bitrate            = ESP_OPUS_BITRATE_AUTO,                                                              \
        .frame_duration     = (esp_opus_enc_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(OPUS_FRAME_DURATION_MS),  \
        .application_mode   = ESP_OPUS_ENC_APPLICATION_AUDIO,                                                     \
        .complexity         = 0,                                                                                  \
        .enable_fec         = false,                                                                              \
        .enable_dtx         = true,                                                                               \
        .enable_vbr         = true,                                                                               \
    }

struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(const std::string&, uint64_t)> on_guarded_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void(uint64_t, const int16_t*, size_t, bool)> on_speech_monitor;
    std::function<void(void)> on_audio_testing_queue_full;
};


enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
    kAudioTaskTypeEncodeToLocalRecording,
};

struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    uint32_t timestamp = 0;
    uint64_t generation = 0;
    uint64_t recording_stream = 0;
};

struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t decode_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
};

class AudioService {
public:
    AudioService();
    ~AudioService();

    void Initialize(AudioCodec* codec);
    void InitializeForLocalRecording(AudioCodec* codec);
    bool Start();
    bool StartForLocalRecording();
    bool Stop();
    void EncodeWakeWord();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    const std::string& GetLastWakeWord() const;
    bool IsVoiceDetected() const { return voice_detected_; }
    bool IsIdle();
    bool IsAssistantCaptureDrained();
    void WaitForPlaybackQueueEmpty();
    bool IsWakeWordRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING; }
    uint32_t WakeInputFeedCount() const { return wake_input_feeds_.load(std::memory_order_relaxed); }
    bool IsAudioProcessorRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING; }
    bool IsAfeWakeWord();

    void EnableWakeWordDetection(bool enable);
    bool PrepareWatchWakeWord(srmodel_list_t* models);
    bool SetWakeWordGuard(std::function<bool()> guard);
    bool IsWakeWordGenerationCurrent(uint64_t generation) const;
    bool PrepareVoiceProcessing(srmodel_list_t* models = nullptr, bool local_endpointing = false);
    // The owner must quiesce even after a failed start or an expired/revoked lease.
    bool StartModelValidation(uint64_t generation, uint32_t duration_ms);
    bool RevokeModelValidation(uint64_t generation);
    bool IsModelValidationCurrent(uint64_t generation) const;
    uint32_t ModelValidationFeedCount() const { return model_validation_feeds_.load(std::memory_order_relaxed); }
    bool EnableVoiceProcessing(bool enable);
    bool StartSpeechMonitor(uint64_t assistant_generation);
    bool IsSpeechMonitorCurrent(uint64_t assistant_generation) const;
    bool StartCaptureWithPreroll(uint64_t assistant_generation, std::vector<int16_t>&& pcm);
    bool IsAssistantPlaybackDrained();
    void EnableAudioTesting(bool enable);
    void EnableDeviceAec(bool enable);
    bool EnableLocalRecording(bool enable);
    bool WaitForLocalRecordingDrain(uint32_t timeout_ms);
    bool QuiesceAssistant(uint32_t timeout_ms);
    bool ResumeAssistantAudio();
    void SetSessionCoordinator(AudioSessionCoordinator* coordinator);
    bool SetAssistantCaptureGuard(std::function<bool()> guard);
    void NotifyAssistantCaptureIntentChanged() { audio_queue_cv_.notify_all(); }
    void SetAssistantUserEnabled(bool enabled) {
        assistant_user_enabled_ = enabled;
        audio_queue_cv_.notify_all();
    }
    bool IsAssistantUserEnabled() const { return assistant_user_enabled_.load(); }
    AudioSessionCoordinator* GetSessionCoordinator() const { return session_coordinator_; }
    // Install only after draining the previous recording. Watch encoding errors
    // are reported as (nullptr, 0); consumers must retain that failure at close.
    void SetLocalRecordingPacketCallback(std::function<void(const uint8_t*, size_t)> callback);

    void SetCallbacks(AudioServiceCallbacks& callbacks);

    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    void PlaySound(const std::string_view& sound);
    bool PrepareLocalPlaybackOutput(uint64_t generation);
    bool WriteLocalPlaybackOutput(uint64_t generation, const std::vector<int16_t>& pcm);
    bool PauseLocalPlaybackOutput(uint64_t generation);
    PlaybackVolumeStatus SetLocalPlaybackVolume(uint64_t generation, int volume);
    bool CloseLocalPlaybackOutput(uint64_t generation);
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    void ResetDecoder();
    void SetModelsList(srmodel_list_t* models_list);

private:
    AudioCodec* codec_ = nullptr;
    AudioServiceCallbacks callbacks_;
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<WakeWord> wake_word_;
    std::atomic<uint32_t> wake_input_feeds_{0};
    std::atomic<uint64_t> model_validation_generation_{0};
    std::atomic<int64_t> model_validation_started_ms_{0};
    std::atomic<int64_t> model_validation_deadline_ms_{0};
    std::atomic<uint32_t> model_validation_feeds_{0};
    std::unique_ptr<AudioDebugger> audio_debugger_;
    void* opus_encoder_ = nullptr;
    void* opus_decoder_ = nullptr;
    std::mutex decoder_mutex_;
    std::mutex input_resampler_mutex_;
    esp_ae_rate_cvt_handle_t input_resampler_ = nullptr;
    esp_ae_rate_cvt_handle_t output_resampler_ = nullptr;
    
    // Encoder/Decoder state
    int encoder_sample_rate_ = 16000;
    int encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int encoder_frame_size_ = 0;
    int encoder_outbuf_size_ = 0;
    int decoder_sample_rate_ = 0;
    int decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int decoder_frame_size_ = 0;
    DebugStatistics debug_statistics_;
    srmodel_list_t* models_list_ = nullptr;

    EventGroupHandle_t event_group_;

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    std::mutex audio_queue_mutex_;
    std::mutex audio_control_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
    // For server AEC
    std::deque<uint32_t> timestamp_queue_;

    bool wake_word_initialized_ = false;
    uint64_t wake_word_generation_ = 0;
    std::atomic<uint64_t> active_wake_word_generation_{0};
    std::function<bool()> wake_word_guard_;
    bool audio_processor_initialized_ = false;
    std::atomic<bool> voice_detected_{false};
    std::atomic<bool> service_stopped_{true};
    std::atomic<bool> audio_input_need_warmup_{false};
    std::atomic<bool> assistant_suspended_{false};
    std::atomic<bool> assistant_user_enabled_{true};
    std::atomic<bool> assistant_capable_{false};
    AudioSessionCoordinator* session_coordinator_ = nullptr;
    uint64_t audio_generation_ = 1;
    bool audio_input_active_ = false;
    bool audio_output_active_ = false;
    uint64_t local_playback_generation_ = 0;
    bool local_playback_preparation_pending_ = false;
    bool audio_decode_active_ = false;
    bool audio_encode_active_ = false;
    bool local_recording_encode_active_ = false;
    uint64_t local_recording_stream_ = 0;
    uint32_t local_recording_level_frame_count_ = 0;
    std::function<void(const uint8_t*, size_t)> local_recording_packet_callback_;

    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    void AudioInputTask();
    void AudioOutputTask();
    void OpusCodecTask();
    std::vector<int16_t> DownmixInputToMono(std::vector<int16_t>&& data) const;
    std::vector<int16_t> ProcessLocalRecordingAudio(std::vector<int16_t>&& data);
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
    bool EnableVoiceProcessingLocked(bool enable, std::vector<int16_t>&& prefix);
    bool CanProcessEncodeTask() const;
    bool HasLocalRecordingEncodeWorkLocked() const;
    bool AllowsAssistantAudio() const;
    bool AllowsWakeWordInput() const;
    bool CreateWatchWakeWord(srmodel_list_t* models);
    bool AllowsAssistantCapture() const {
        return !speech_monitor_generation_.load() && (!assistant_capture_guard_ || assistant_capture_guard_());
    }
    std::atomic<uint64_t> speech_monitor_generation_{0};
    bool speech_monitor_configured_ = false;
    std::function<bool()> assistant_capture_guard_;
    bool ReadAudioDataInternal(std::vector<int16_t>& data, int sample_rate, int samples, bool allow_debug = true);
    bool ProcessModelValidationInput();
    bool QuiesceAssistantLocked(uint32_t timeout_ms, uint64_t playback_generation = 0);
    bool StartTasks(bool recording_only);
    bool WaitForTasksStoppedLocked(std::unique_lock<std::mutex>& lock, uint32_t timeout_ms);
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckAndUpdateAudioPowerState();
};

#endif
