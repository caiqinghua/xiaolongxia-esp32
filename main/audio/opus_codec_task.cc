#include "audio_service.h"
#include "esp_log.h"
#if CONFIG_AUDREAM_WATCH_UI
#include "watch_stream_encoder.h"
#endif

#define TAG "AudioService"

void AudioService::OpusCodecTask() {
#if CONFIG_AUDREAM_WATCH_UI
    WatchStreamEncoder stream_encoder;
#endif
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ ||
                CanProcessEncodeTask() ||
                (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            const uint64_t generation = audio_generation_;
            audio_decode_active_ = true;
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;
            task->generation = generation;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            if (opus_decoder_ != nullptr) {
                task->pcm.resize(decoder_frame_size_);
                esp_audio_dec_in_raw_t raw = {
                    .buffer = (uint8_t *)(packet->payload.data()),
                    .len = (uint32_t)(packet->payload.size()),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out_frame = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(task->pcm.size() * sizeof(int16_t)),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                decoder_lock.unlock();
                if (ret == ESP_AUDIO_ERR_OK) {
                    task->pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                    if (decoder_sample_rate_ != codec_->output_sample_rate() && output_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task->pcm.size(), &target_size);
                        std::vector<int16_t> resampled(target_size);
                        uint32_t actual_output = target_size;
                        esp_ae_rate_cvt_process(output_resampler_, (esp_ae_sample_t)task->pcm.data(), task->pcm.size(),
                                                (esp_ae_sample_t)resampled.data(), &actual_output);
                        resampled.resize(actual_output);
                        task->pcm = std::move(resampled);
                    }
                    lock.lock();
                    if (AllowsAssistantAudio() && generation == audio_generation_) {
                        audio_playback_queue_.push_back(std::move(task));
                    }
                    audio_queue_cv_.notify_all();
                    debug_statistics_.decode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to decode audio after resize, error code: %d", ret);
                    lock.lock();
                }
            } else {
                ESP_LOGE(TAG, "Audio decoder is not configured");
                lock.lock();
            }
            debug_statistics_.decode_count++;
            audio_decode_active_ = false;
            audio_queue_cv_.notify_all();
        }
        /* Encode the audio to send queue */
        if (CanProcessEncodeTask()) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            local_recording_encode_active_ =
                task->type == kAudioTaskTypeEncodeToLocalRecording;
            audio_encode_active_ = true;
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;

            bool encoded = false;
#if CONFIG_AUDREAM_WATCH_UI
            const auto role = task->type == kAudioTaskTypeEncodeToLocalRecording ? WatchEncodeRole::kRecording :
                task->type == kAudioTaskTypeEncodeToTestingQueue ? WatchEncodeRole::kTesting : WatchEncodeRole::kAssistant;
            const auto stream = role == WatchEncodeRole::kRecording ? task->recording_stream : task->generation;
            encoded = stream_encoder.Encode(opus_encoder_, role, stream, task->pcm,
                encoder_frame_size_, encoder_outbuf_size_, packet->payload);
            if (!encoded) ESP_LOGE(TAG, "Watch stream encoding failed: role=%d stream=%llu",
                static_cast<int>(role), static_cast<unsigned long long>(stream));
#else
            if (opus_encoder_ != nullptr && task->pcm.size() == static_cast<size_t>(encoder_frame_size_)) {
                std::vector<uint8_t> buf(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = buf.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet->payload.assign(buf.data(), buf.data() + out.encoded_bytes);
                    encoded = true;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG, "Failed to encode audio: encoder not configured or invalid frame size (got %u, expected %u)",
                         task->pcm.size(), encoder_frame_size_);
            }
#endif
            if (encoded) {
                if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                    {
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        if (AllowsAssistantAudio() && task->generation == audio_generation_) {
                            audio_send_queue_.push_back(std::move(packet));
                        }
                    }
                    if (callbacks_.on_send_queue_available) {
                        callbacks_.on_send_queue_available();
                    }
                } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                    std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                    if (AllowsAssistantAudio() && task->generation == audio_generation_) {
                        audio_testing_queue_.push_back(std::move(packet));
                    }
                } else if (task->type == kAudioTaskTypeEncodeToLocalRecording) {
                    std::function<void(const uint8_t*, size_t)> callback;
                    {
                        std::lock_guard<std::mutex> callback_lock(audio_queue_mutex_);
#if CONFIG_AUDREAM_WATCH_UI
                        if (task->recording_stream == local_recording_stream_) {
                            callback = local_recording_packet_callback_;
                        }
#else
                        callback = local_recording_packet_callback_;
#endif
                    }
                    if (callback) {
                        callback(packet->payload.data(), packet->payload.size());
                    }
                }
                debug_statistics_.encode_count++;
            }
#if CONFIG_AUDREAM_WATCH_UI
            else if (task->type == kAudioTaskTypeEncodeToLocalRecording) {
                std::function<void(const uint8_t*, size_t)> callback;
                {
                    std::lock_guard<std::mutex> callback_lock(audio_queue_mutex_);
                    if (task->recording_stream == local_recording_stream_) callback = local_recording_packet_callback_;
                }
                // The recording service latches this failure across metadata
                // retries. Never represent missing encoded speech as success.
                if (callback) callback(nullptr, 0);
            }
#endif
            lock.lock();
            audio_encode_active_ = false;
            audio_queue_cv_.notify_all();
            if (local_recording_encode_active_) {
                local_recording_encode_active_ = false;
                audio_queue_cv_.notify_all();
            }
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

bool AudioService::CanProcessEncodeTask() const {
    if (audio_encode_queue_.empty()) {
        return false;
    }
    const auto& task = audio_encode_queue_.front();
    if (task->type == kAudioTaskTypeEncodeToLocalRecording) {
        return true;
    }
    return audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE;
}

bool AudioService::HasLocalRecordingEncodeWorkLocked() const {
    if (audio_input_active_) {
        return true;
    }
    if (local_recording_encode_active_) {
        return true;
    }
    for (const auto& task : audio_encode_queue_) {
        if (task && task->type == kAudioTaskTypeEncodeToLocalRecording) {
            return true;
        }
    }
    return false;
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);
    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    const auto capture_stopped = [this, type]() {
        return type == kAudioTaskTypeEncodeToSendQueue &&
            ((xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING) == 0 || !AllowsAssistantCapture());
    };
    if (service_stopped_ || capture_stopped() ||
        (type != kAudioTaskTypeEncodeToLocalRecording && !AllowsAssistantAudio())) {
        return;
    }
    const uint64_t generation = audio_generation_;
    task->generation = generation;
    if (type == kAudioTaskTypeEncodeToLocalRecording) task->recording_stream = local_recording_stream_;

    /* If the task is to send queue, we need to set the timestamp */
    if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
        if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
            task->timestamp = timestamp_queue_.front();
        } else {
            ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
        }
        timestamp_queue_.pop_front();
    }

    audio_queue_cv_.wait(lock, [this, type, generation, &capture_stopped]() {
        return service_stopped_ || capture_stopped() || audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE ||
            (type != kAudioTaskTypeEncodeToLocalRecording &&
                (!AllowsAssistantAudio() || generation != audio_generation_));
    });
    if (service_stopped_ || capture_stopped() || (type != kAudioTaskTypeEncodeToLocalRecording &&
        (!AllowsAssistantAudio() || generation != audio_generation_))) {
        return;
    }
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

void AudioService::SetLocalRecordingPacketCallback(std::function<void(const uint8_t*, size_t)> callback) {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    // Installing a callback starts a new file identity. Pause/resume keeps it;
    // queued work from an older file cannot publish to its replacement.
    if (++local_recording_stream_ == 0) ++local_recording_stream_;
    local_recording_packet_callback_ = std::move(callback);
}
