#include "audio/audio_service.h"
#include <cassert>
#include <iostream>

namespace {
EventGroupHandle_t events = nullptr;
std::vector<std::string> calls;
std::function<void()> input_hook, wake_feed_hook, processor_feed_hook;
bool input_ok = true;
int input_size_adjustment = 0;
unsigned input_reads = 0, wake_feeds = 0, processor_feeds = 0;
class Wake : public WakeWord {
public:
    bool supported = true, start_ok = true, drain_ok = true, init_ok = true;
    uint32_t budget = 0;
    std::string word;
    std::function<void(const std::string&)> detected;
    bool Initialize(AudioCodec*, srmodel_list_t*) override { calls.push_back("init"); return init_ok; }
    void Feed(const std::vector<int16_t>&) override { ++wake_feeds; if (wake_feed_hook) wake_feed_hook(); }
    void OnWakeWordDetected(std::function<void(const std::string&)> callback) override { detected = std::move(callback); }
    void Start() override { assert(false); }
    void Stop() override { calls.push_back("legacy_stop"); }
    bool SupportsBoundedHandoff() const override { return supported; }
    bool StartChecked() override { calls.push_back("start"); return start_ok; }
    bool StopAndDrain(uint32_t timeout) override {
        calls.push_back("drain"); budget = timeout; return supported && drain_ok;
    }
    size_t GetFeedSize() override { return 16; }
    void EncodeWakeWordData() override {}
    bool GetWakeWordOpus(std::vector<uint8_t>&) override { return false; }
    const std::string& GetLastDetectedWakeWord() const override { return word; }
};
Wake* wake = nullptr;
bool monitor_fixture = false;
bool preparation_fixture = false;
unsigned decoder_resets = 0;
class Processor : public AudioProcessor {
public:
    bool stop_ok = true, start_ok = true, monitor_ok = true, prefix_ok = true;
    bool init_ok = true, endpoint_ok = true;
    unsigned initializations = 0, endpoint_settings = 0, starts = 0;
    srmodel_list_t* prepared_models = nullptr;
    std::vector<int16_t> prefix;
    SpeechMonitorCallback callback;
    bool Initialize(AudioCodec*, int duration, srmodel_list_t* models) override {
        assert(duration == OPUS_FRAME_DURATION_MS);
        ++initializations; prepared_models = models; return init_ok;
    }
    void Feed(std::vector<int16_t>&&) override { ++processor_feeds; if (processor_feed_hook) processor_feed_hook(); }
    bool Start() override { ++starts; return start_ok; }
    bool Stop(uint32_t) override { return stop_ok; }
    bool IsRunning() override { return false; }
    void OnOutput(std::function<void(std::vector<int16_t>&&)>) override {}
    void OnVadStateChange(std::function<void(bool)>) override {}
    size_t GetFeedSize() override { return 16; }
    void EnableDeviceAec(bool) override {}
    bool SetLocalEndpointing(bool) override { ++endpoint_settings; return endpoint_ok; }
    bool SetSpeechMonitor(bool) override { return monitor_ok; }
    bool SetCapturePreroll(std::vector<int16_t>&& value) override {
        if (!prefix_ok) return false;
        prefix = std::move(value);
        return true;
    }
    void OnSpeechMonitor(SpeechMonitorCallback value) override { callback = std::move(value); }
};
Processor* processor = nullptr;
class Codec : public AudioCodec {
public:
    explicit Codec(bool reference = false) { input_enabled_ = true; output_enabled_ = true; input_reference_ = reference; }
    int Read(int16_t*, int) override { return 0; }
    int Write(const int16_t*, int) override { return 0; }
};
}

AudioCodec::AudioCodec() = default;
AudioCodec::~AudioCodec() = default;
void AudioCodec::SetOutputVolume(int) {}
void AudioCodec::SetInputGain(float) {}
void AudioCodec::EnableInput(bool enabled) { input_enabled_ = enabled; calls.push_back("input_off"); }
void AudioCodec::EnableOutput(bool enabled) { output_enabled_ = enabled; calls.push_back("output_off"); }
void AudioCodec::PrepareForLocalRecording() {}
void AudioCodec::PrepareForDeepSleep() {}
void AudioCodec::OutputData(std::vector<int16_t>&) {}
bool AudioCodec::InputData(std::vector<int16_t>&) { return false; }
void AudioCodec::Start() {}
AudioDebugger::~AudioDebugger() = default;
AudioService::AudioService() {
    event_group_ = events = new HostTransportEvents;
    wake_word_ = std::make_unique<Wake>();
    wake = static_cast<Wake*>(wake_word_.get());
    assistant_capable_ = true;
}
AudioService::~AudioService() { delete event_group_; }
void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    if (monitor_fixture || preparation_fixture) {
        audio_processor_ = std::make_unique<Processor>();
        processor = static_cast<Processor*>(audio_processor_.get());
        audio_processor_initialized_ = monitor_fixture;
        service_stopped_ = false;
    }
}
void AudioService::SetSessionCoordinator(AudioSessionCoordinator* coordinator) { session_coordinator_ = coordinator; }
void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) { callbacks_ = callbacks; }
bool AudioService::CreateWatchWakeWord(srmodel_list_t*) { return false; }
void AudioService::ResetDecoder() { ++decoder_resets; }
bool AudioService::ReadAudioDataInternal(std::vector<int16_t>& pcm, int rate, int samples, bool allow_debug) {
    assert(rate == 16000 && samples == 160 && !allow_debug);
    ++input_reads;
    pcm.assign(samples * codec_->input_channels() + input_size_adjustment, 123);
    if (input_hook) input_hook();
    return input_ok;
}
// Invoke the production per-read step without starting FreeRTOS in this test.
bool AudioService::ReadAudioData(std::vector<int16_t>&, int, int) { return ProcessModelValidationInput(); }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) { return group->bits |= bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) {
    auto previous = group->bits; group->bits &= ~bits; return previous;
}
EventBits_t xEventGroupGetBits(EventGroupHandle_t group) { return group->bits; }
esp_ae_err_t esp_ae_rate_cvt_reset(esp_ae_rate_cvt_handle_t) { return ESP_AE_ERR_OK; }

int main() {
    for (int mode = 0; mode < 10; ++mode) {
        preparation_fixture = true;
        AudioSessionCoordinator owner;
        AudioService audio; Codec codec; srmodel_list_t models;
        assert(audio.SetWakeWordGuard([] { return false; }));
        audio.Initialize(&codec); audio.SetSessionCoordinator(&owner); audio.SetAssistantUserEnabled(false);
        const auto validation = owner.Begin(AudioSessionMode::kModelValidation);
        assert(validation && owner.OwnsModelValidationPreparation(validation.generation));
        assert(!audio.StartModelValidation(validation.generation));
        assert(audio.QuiesceAssistant(25));
        assert(audio.PrepareWatchWakeWord(&models) && audio.PrepareVoiceProcessing(&models, true));
        assert(owner.Complete(validation));
        if (mode == 6) wake->start_ok = false;
        if (mode == 7) processor->start_ok = false;
        if (mode >= 6 && mode <= 7) {
            assert(!audio.StartModelValidation(validation.generation) && !events->bits);
            assert(audio.QuiesceAssistant(25));
            assert(owner.ReleaseModelValidation(validation.generation, true));
            preparation_fixture = false;
            continue;
        }
        assert(audio.StartModelValidation(validation.generation));
        assert(!audio.StartModelValidation(validation.generation));
        assert(audio.IsModelValidationCurrent(validation.generation) && !audio.IsAssistantUserEnabled());
        assert(events->bits == AS_EVENT_MODEL_VALIDATION_RUNNING && !audio.IsWakeWordGenerationCurrent(1));
        if (wake->detected) wake->detected("unexpected");
        input_reads = wake_feeds = processor_feeds = 0;
        input_ok = mode != 1; input_size_adjustment = mode == 2 ? -1 : 0;
        AudioSessionTransition recording;
        auto preempt = [&] { recording = owner.Begin(AudioSessionMode::kLocalRecording); assert(recording); };
        if (mode == 3) input_hook = preempt;
        if (mode == 4) wake_feed_hook = preempt;
        if (mode == 5) processor_feed_hook = preempt;
        std::vector<int16_t> unused;
        const bool consumed = audio.ReadAudioData(unused, 0, 0);
        assert(consumed == (mode == 0 || mode >= 8));
        assert(audio.ModelValidationFeedCount() == static_cast<unsigned>(consumed));
        assert(input_reads == 1 && wake_feeds == static_cast<unsigned>(mode != 1 && mode != 2 && mode != 3));
        assert(processor_feeds == static_cast<unsigned>(mode == 0 || mode == 5 || mode >= 8));
        input_hook = wake_feed_hook = processor_feed_hook = {};
        if (recording) {
            assert(!audio.IsModelValidationCurrent(validation.generation));
            assert(!audio.ReadAudioData(unused, 0, 0) && input_reads == 1);
        }
        if (mode == 8) wake->drain_ok = false;
        if (mode == 9) processor->stop_ok = false;
        if (mode >= 8) {
            assert(!audio.QuiesceAssistant(25));
            assert(!audio.IsModelValidationCurrent(validation.generation) && !events->bits);
            assert(!owner.ReleaseModelValidation(validation.generation, false) && owner.BlocksSleep());
            wake->drain_ok = processor->stop_ok = true;
        }
        assert(audio.QuiesceAssistant(25));
        assert(!events->bits && !codec.input_enabled() && !codec.output_enabled());
        assert(owner.ReleaseModelValidation(validation.generation, true) == !static_cast<bool>(recording));
        if (recording) assert(owner.Complete(recording));
        assert(!audio.ReadAudioData(unused, 0, 0) && input_reads == 1);
        input_ok = true; input_size_adjustment = 0;
        preparation_fixture = false;
    }
    calls.clear();
    std::cout << "PASS: local model validation input, no debug output, explicit owner, preemption and failed drain\n";
    AudioSessionCoordinator coordinator;
    {
        AudioService audio;
        Codec codec;
        audio.Initialize(&codec);
        audio.SetSessionCoordinator(&coordinator);
        wake->supported = false;
        audio.EnableWakeWordDetection(true);
        assert(calls.empty() && !events->bits);
        wake->supported = true;
        wake->init_ok = false;
        audio.EnableWakeWordDetection(true);
        assert(calls == std::vector<std::string>{"init"} && !events->bits);
        calls.clear(); wake->init_ok = true; wake->start_ok = false;
        audio.EnableWakeWordDetection(true);
        assert((calls == std::vector<std::string>{"init", "start"}) && !events->bits);
        calls.clear(); wake->start_ok = true;
        audio.EnableWakeWordDetection(true);
        assert(calls == std::vector<std::string>{"start"} && events->bits == AS_EVENT_WAKE_WORD_RUNNING);
        calls.clear(); wake->drain_ok = false;
        assert(!audio.QuiesceAssistant(25));
        assert(calls == std::vector<std::string>{"drain"} && wake->budget <= 25);
        assert(codec.input_enabled() && codec.output_enabled() && !events->bits);
        calls.clear(); audio.EnableWakeWordDetection(true);
        assert(calls.empty()); // Failed handoff keeps the assistant suspended.
        wake->drain_ok = true;
        assert(audio.QuiesceAssistant(25));
        assert((calls == std::vector<std::string>{"drain", "input_off", "output_off"}));
        assert(!codec.input_enabled() && !codec.output_enabled());
    }
    calls.clear();
    {
        preparation_fixture = true;
        AudioSessionCoordinator owner;
        AudioService audio;
        Codec codec;
        srmodel_list_t models, replacement;
        assert(audio.SetWakeWordGuard([] { return false; }));
        audio.Initialize(&codec);
        audio.SetSessionCoordinator(&owner);
        audio.SetAssistantUserEnabled(false);
        assert(!audio.PrepareVoiceProcessing(&models, true));
        auto reservation = owner.Begin(AudioSessionMode::kMaintenance);
        assert(reservation && audio.QuiesceAssistant(25));
        assert(!audio.PrepareVoiceProcessing(&models, true) && !processor->initializations);
        assert(audio.PrepareWatchWakeWord(&models));
        assert(!audio.PrepareVoiceProcessing(nullptr, true));
        assert(!audio.PrepareVoiceProcessing(&replacement, true));
        processor->init_ok = false;
        assert(!audio.PrepareVoiceProcessing(&models, true) && processor->initializations == 1 &&
            !processor->endpoint_settings);
        processor->init_ok = true; processor->endpoint_ok = false;
        assert(!audio.PrepareVoiceProcessing(&models, true) && processor->initializations == 2);
        processor->endpoint_ok = true;
        assert(audio.PrepareVoiceProcessing(&models, true) && processor->prepared_models == &models &&
            processor->initializations == 2);
        assert(!audio.IsAssistantUserEnabled() && !audio.IsWakeWordRunning() && !events->bits && !processor->starts);
        assert(!codec.input_enabled() && !codec.output_enabled());
        events->bits = AS_EVENT_LOCAL_RECORDING_RUNNING;
        const auto configured = processor->endpoint_settings;
        assert(!audio.PrepareVoiceProcessing(&models, true) && processor->endpoint_settings == configured);
        events->bits = 0;
        assert(owner.Fail(reservation, true));
        assert(!audio.PrepareVoiceProcessing(&models, true));
        const auto recording = owner.Begin(AudioSessionMode::kLocalRecording);
        assert(recording && !audio.PrepareVoiceProcessing(&models, true));
        assert(owner.Fail(recording, true));
        audio.EnableWakeWordDetection(true);
        assert(!events->bits && !processor->starts);
        preparation_fixture = false;
    }
    std::cout << "PASS: real suspended model preparation, pinned identity, initialization failures and capture isolation\n";
    calls.clear();
    {
        AudioService audio;
        Codec codec;
        srmodel_list_t models, replacement;
        audio.Initialize(&codec);
        audio.SetSessionCoordinator(&coordinator);
        assert(audio.SetWakeWordGuard([] { return false; }));
        assert(!audio.PrepareWatchWakeWord(&models));
        const auto reservation = coordinator.Begin(AudioSessionMode::kMaintenance);
        assert(reservation && coordinator.OwnsMaintenanceResources());
        assert(audio.QuiesceAssistant(25));
        assert(!audio.PrepareWatchWakeWord(nullptr));
        wake->init_ok = false;
        assert(!audio.PrepareWatchWakeWord(&models));
        wake->init_ok = true;
        assert(audio.PrepareWatchWakeWord(&models));
        assert(audio.PrepareWatchWakeWord(&models));
        assert(!audio.PrepareWatchWakeWord(&replacement));
        assert(!audio.IsWakeWordRunning());
        assert(coordinator.Fail(reservation, true));
        assert(!audio.PrepareWatchWakeWord(&models));
    }
    calls.clear();
    {
        AudioService audio;
        Codec codec;
        audio.Initialize(&codec);
        audio.SetSessionCoordinator(&coordinator);
        events->bits = AS_EVENT_LOCAL_RECORDING_RUNNING;
        assert(!audio.QuiesceAssistant(25));
        assert(calls.empty() && codec.input_enabled());
    }
    {
        AudioService audio;
        Codec codec;
        audio.Initialize(&codec);
        wake->supported = false;
        audio.EnableWakeWordDetection(true); // Legacy boards retain their original path.
        calls.clear();
        assert(audio.QuiesceAssistant(25));
        assert((calls == std::vector<std::string>{"legacy_stop", "input_off", "output_off"}));
    }
    {
        calls.clear();
        AudioService audio;
        Codec codec;
        audio.Initialize(&codec);
        audio.SetSessionCoordinator(&coordinator);
        bool allowed = false;
        assert(!audio.SetWakeWordGuard({}));
        assert(audio.SetWakeWordGuard([&] { return allowed; }));
        assert(!audio.SetWakeWordGuard([] { return true; }));
        uint64_t detected = 0;
        int count = 0;
        AudioServiceCallbacks callbacks;
        callbacks.on_wake_word_detected = [](const auto&) { assert(false); };
        callbacks.on_guarded_wake_word_detected = [&](const auto&, uint64_t generation) {
            detected = generation;
            ++count;
        };
        audio.SetCallbacks(callbacks);
        audio.EnableWakeWordDetection(true);
        assert(calls.empty());
        allowed = true;
        audio.EnableWakeWordDetection(true);
        auto old_callback = wake->detected;
        old_callback("wake");
        assert(count == 1 && detected && audio.IsWakeWordGenerationCurrent(detected));
        const auto previous = detected;
        allowed = false;
        old_callback("expired");
        assert(count == 1 && !audio.IsWakeWordGenerationCurrent(previous));
        audio.EnableWakeWordDetection(false);
        allowed = true;
        wake->drain_ok = false;
        audio.EnableWakeWordDetection(true);
        assert(!audio.IsWakeWordRunning());
        old_callback("late");
        assert(count == 1);
        wake->drain_ok = true;
        audio.EnableWakeWordDetection(true);
        old_callback("old owner");
        assert(count == 1);
        wake->detected("new");
        assert(count == 2 && detected != previous && !audio.IsWakeWordGenerationCurrent(previous));
        const auto transition = coordinator.Begin(AudioSessionMode::kLocalRecording);
        assert(transition);
        wake->detected("preempted");
        assert(count == 2 && !audio.IsWakeWordGenerationCurrent(detected));
        assert(coordinator.Fail(transition, true));
        assert(audio.QuiesceAssistant(25));
        wake->detected("stopped");
        assert(count == 2);
    }
    monitor_fixture = true;
    {
        AudioSessionCoordinator monitor_owner;
        AudioService audio;
        Codec codec(true);
        audio.Initialize(&codec);
        unsigned callbacks = 0;
        AudioServiceCallbacks listener;
        listener.on_speech_monitor = [&](uint64_t generation, const int16_t* pcm, size_t count, bool speech) {
            assert(generation == monitor_owner.Generation() && count == 2 && pcm[0] == 123 && speech);
            ++callbacks;
        };
        const auto connection = monitor_owner.Begin(AudioSessionMode::kAssistantConnecting);
        assert(connection && monitor_owner.Complete(connection));
        const auto first = connection.generation;
        assert(!audio.StartSpeechMonitor(first));
        audio.SetSessionCoordinator(&monitor_owner);
        audio.SetCallbacks(listener);
        assert(!audio.StartSpeechMonitor(first));
        assert(monitor_owner.UpdateAssistant(AudioSessionMode::kAssistantSpeaking, first));
        assert(!audio.StartSpeechMonitor(0) && !audio.StartSpeechMonitor(first + 1));
        events->bits = AS_EVENT_WAKE_WORD_RUNNING;
        assert(!audio.StartSpeechMonitor(first));
        events->bits = 0;
        processor->stop_ok = false; assert(!audio.StartSpeechMonitor(first)); processor->stop_ok = true;
        processor->monitor_ok = false; assert(!audio.StartSpeechMonitor(first)); processor->monitor_ok = true;
        processor->start_ok = false; assert(!audio.StartSpeechMonitor(first)); processor->start_ok = true;
        assert(!audio.IsSpeechMonitorCurrent(first));
        assert(audio.StartSpeechMonitor(first) && audio.IsSpeechMonitorCurrent(first));
        assert(decoder_resets == 0);
        assert(events->bits == AS_EVENT_AUDIO_PROCESSOR_RUNNING && codec.output_enabled());
        int16_t pcm[2]{123, 456};
        const auto old_callback = processor->callback;
        old_callback(pcm, 2, true); assert(callbacks == 1);
        assert(monitor_owner.UpdateAssistant(AudioSessionMode::kAssistantThinking, first));
        old_callback(pcm, 2, true); assert(callbacks == 1 && !audio.IsSpeechMonitorCurrent(first));
        const auto second = monitor_owner.RotateAssistant(first);
        assert(second && monitor_owner.UpdateAssistant(AudioSessionMode::kAssistantSpeaking, second));
        assert(audio.StartSpeechMonitor(second));
        old_callback(pcm, 2, true); assert(callbacks == 1);
        processor->callback(pcm, 2, true); assert(callbacks == 2);
        auto second_callback = processor->callback;
        processor->stop_ok = false;
        assert(!audio.EnableVoiceProcessing(true) && !audio.IsSpeechMonitorCurrent(second) && !events->bits);
        second_callback(pcm, 2, true); assert(callbacks == 2 && decoder_resets == 0);
        processor->stop_ok = true;
        processor->monitor_ok = false;
        assert(!audio.EnableVoiceProcessing(true) && !events->bits && decoder_resets == 0);
        processor->monitor_ok = true;
        assert(audio.EnableVoiceProcessing(true) && decoder_resets == 1);
        second_callback(pcm, 2, true); assert(callbacks == 2);
        assert(audio.EnableVoiceProcessing(false));
        assert(audio.StartSpeechMonitor(second) && decoder_resets == 1);
        const auto recording = monitor_owner.Begin(AudioSessionMode::kLocalRecording);
        assert(recording);
        processor->callback(pcm, 2, true); assert(callbacks == 2 && !audio.IsSpeechMonitorCurrent(second));
        assert(!audio.StartSpeechMonitor(second));
        assert(audio.QuiesceAssistant(25));
        assert(!events->bits);
    }
    {
        AudioSessionCoordinator monitor_owner;
        AudioService audio;
        Codec codec;
        audio.Initialize(&codec); audio.SetSessionCoordinator(&monitor_owner);
        AudioServiceCallbacks listener;
        listener.on_speech_monitor = [](uint64_t, const int16_t*, size_t, bool) { assert(false); };
        audio.SetCallbacks(listener);
        const auto connection = monitor_owner.Begin(AudioSessionMode::kAssistantConnecting);
        assert(connection && monitor_owner.Complete(connection));
        assert(monitor_owner.UpdateAssistant(AudioSessionMode::kAssistantSpeaking, connection.generation));
        assert(!audio.StartSpeechMonitor(connection.generation));
    }
    {
        AudioSessionCoordinator owner;
        AudioService audio;
        Codec codec(true);
        audio.Initialize(&codec); audio.SetSessionCoordinator(&owner);
        const auto connection = owner.Begin(AudioSessionMode::kAssistantConnecting);
        assert(connection && owner.Complete(connection));
        const auto token = connection.generation;
        assert(!audio.StartCaptureWithPreroll(token, {1, 2}));
        assert(owner.UpdateAssistant(AudioSessionMode::kAssistantListening, token));
        assert(!audio.StartCaptureWithPreroll(token + 1, {1, 2}));
        assert(!audio.StartCaptureWithPreroll(0, {1, 2}));
        assert(!audio.StartCaptureWithPreroll(token, {}));
        assert(!audio.StartCaptureWithPreroll(token, std::vector<int16_t>(16001)));
        processor->stop_ok = false;
        assert(!audio.StartCaptureWithPreroll(token, {1, 2}));
        processor->stop_ok = true; processor->prefix_ok = false;
        assert(!audio.StartCaptureWithPreroll(token, {1, 2}));
        processor->prefix_ok = true;
        assert(audio.StartCaptureWithPreroll(token, {1, 2, 3}));
        assert((processor->prefix == std::vector<int16_t>{1, 2, 3}));
        assert(events->bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING);
        assert(!audio.StartCaptureWithPreroll(token, {4, 5}));
        assert((processor->prefix == std::vector<int16_t>{1, 2, 3}));
        assert(audio.IsAssistantPlaybackDrained());
        assert(owner.Begin(AudioSessionMode::kLocalRecording));
        assert(!audio.StartCaptureWithPreroll(token, {4, 5}));
        assert((processor->prefix == std::vector<int16_t>{1, 2, 3}));
    }
    std::cout << "PASS real AudioService monitor admission, callback epochs, playback preservation and recording preemption\n";
    std::cout << "PASS real AudioService handoff: checked start, unsupported backend, failed drain, recording priority and legacy path\n";
}
