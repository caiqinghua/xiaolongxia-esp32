#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <string>
#include <vector>
#include <functional>

#include <model_path.h>
#include "audio_codec.h"

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;

    // Lifecycle/configuration calls are serialized by the owner. Register callbacks
    // before Start; Feed may run concurrently. Do not stop from an output callback.
    virtual bool Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) = 0;
    virtual void Feed(std::vector<int16_t>&& data) = 0;
    virtual bool Start() = 0;
    // A timeout leaves the processor stopped but retains resources until a retry drains it.
    virtual bool Stop(uint32_t timeout_ms = 3000) = 0;
    virtual bool IsRunning() = 0;
    virtual void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) = 0;
    virtual void OnVadStateChange(std::function<void(bool speaking)> callback) = 0;
    virtual size_t GetFeedSize() = 0;
    virtual void EnableDeviceAec(bool enable) = 0;
    virtual bool SetLocalEndpointing(bool enable) { return !enable; }
    // Owner-configured monitor mode never emits normal PCM or normal VAD events.
    // The mono 16 kHz frame is borrowed until the callback returns. Register before Start.
    using SpeechMonitorCallback = std::function<void(const int16_t*, size_t, bool)>;
    virtual void OnSpeechMonitor(SpeechMonitorCallback) {}
    virtual bool SetSpeechMonitor(bool enable) { return !enable; }
    virtual bool SetCapturePreroll(std::vector<int16_t>&& pcm) { return pcm.empty(); }
};

#endif
