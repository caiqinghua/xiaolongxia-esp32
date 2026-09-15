#include "audio/processors/afe_audio_processor.h"
#include "audio/processors/no_audio_processor.h"
#include "audio/assistant_model_bundle.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <thread>
#include <atomic>

using namespace std::chrono_literals;
namespace {
enum class Failure { None, Config, Interface, Data, Chunk, Task, Reset, Vad, Aec };
Failure failure = Failure::None;
int live_configs = 0, live_data = 0, input_channels = 1;
bool reference_enabled = false;
bool vad_enabled = false, aec_enabled = false;
std::atomic<bool> fixture_speech{false};
std::vector<std::thread> workers;
void JoinWorkers() {
    for (auto& worker : workers) worker.join();
    workers.clear();
}
}

struct esp_afe_sr_data_t {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::vector<int16_t>> queued;
    std::vector<int16_t> fetched;
    afe_fetch_result_t result;
};

AudioCodec::AudioCodec() = default;
AudioCodec::~AudioCodec() = default;
void AudioCodec::SetOutputVolume(int) {}
void AudioCodec::SetInputGain(float) {}
void AudioCodec::EnableInput(bool) {}
void AudioCodec::EnableOutput(bool) {}
void AudioCodec::PrepareForLocalRecording() {}
void AudioCodec::PrepareForDeepSleep() {}
void AudioCodec::OutputData(std::vector<int16_t>&) {}
bool AudioCodec::InputData(std::vector<int16_t>&) { return false; }
void AudioCodec::Start() {}
class Codec : public AudioCodec {
public:
    explicit Codec(int channels = 1, bool reference = false) {
        input_channels_ = channels;
        input_reference_ = reference;
    }
    int Read(int16_t*, int) override { return 0; }
    int Write(const int16_t*, int) override { return 0; }
};

srmodel_list_t* esp_srmodel_init(const char*) { return nullptr; }
char* esp_srmodel_filter(srmodel_list_t*, const char*, const char*) { return nullptr; }
afe_config_t* afe_config_init(const char* format, srmodel_list_t*, int, int) {
    input_channels = std::strlen(format);
    if (failure == Failure::Config) return nullptr;
    ++live_configs;
    return new afe_config_t;
}
void afe_config_free(afe_config_t* config) { --live_configs; delete config; }
const esp_afe_sr_iface_t* esp_afe_handle_from_config(afe_config_t*) {
    static const esp_afe_sr_iface_t iface = {
        [](afe_config_t* config) -> esp_afe_sr_data_t* {
            reference_enabled = config->aec_init;
            aec_enabled = config->aec_init;
            vad_enabled = config->vad_init;
            assert(vad_enabled);
            if (failure == Failure::Data) return nullptr;
            ++live_data;
            return new esp_afe_sr_data_t;
        },
        [](esp_afe_sr_data_t*) { return failure == Failure::Chunk ? 0 : 16; },
        [](esp_afe_sr_data_t*) { return 16; },
        [](esp_afe_sr_data_t* data) { --live_data; delete data; },
        [](esp_afe_sr_data_t* data, const int16_t* input) {
            std::lock_guard<std::mutex> lock(data->mutex);
            std::vector<int16_t> mono;
            for (int i = 0; i < 16; ++i) mono.push_back(input[i * input_channels]);
            data->queued.push_back(std::move(mono));
            data->cv.notify_all();
            return 0;
        },
        [](esp_afe_sr_data_t* data, TickType_t timeout) -> afe_fetch_result_t* {
            assert(timeout != portMAX_DELAY);
            std::unique_lock<std::mutex> lock(data->mutex);
            if (!data->cv.wait_for(lock, std::chrono::milliseconds(timeout), [&]() { return !data->queued.empty(); })) return nullptr;
            data->fetched = std::move(data->queued.front());
            data->queued.pop_front();
            data->result.data = data->fetched.data();
            data->result.data_size = static_cast<int>(data->fetched.size() * 2);
            data->result.vad_state = fixture_speech ? VAD_SPEECH : VAD_SILENCE;
            return &data->result;
        },
        [](esp_afe_sr_data_t* data) {
            std::lock_guard<std::mutex> lock(data->mutex);
            if (failure == Failure::Reset) return -1;
            data->queued.clear();
            return 1;
        },
        [](esp_afe_sr_data_t*) { if (failure == Failure::Vad) return -1; vad_enabled = false; return 0; },
        [](esp_afe_sr_data_t*) { if (failure == Failure::Vad) return -1; vad_enabled = true; return 0; },
        [](esp_afe_sr_data_t*) { if (failure == Failure::Aec) return -1; aec_enabled = true; return 0; },
        [](esp_afe_sr_data_t*) { if (failure == Failure::Aec) return -1; aec_enabled = false; return 0; }
    };
    return failure == Failure::Interface ? nullptr : &iface;
}
BaseType_t xTaskCreate(void (*fn)(void*), const char*, unsigned, void* arg, unsigned, TaskHandle_t*) {
    if (failure == Failure::Task) return 0;
    workers.emplace_back([=]() { fn(arg); });
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { assert(!task); }

void TestInitializationFailures() {
    Codec codec;
    for (auto stage : {Failure::Config, Failure::Interface, Failure::Data, Failure::Chunk, Failure::Task}) {
        {
            AfeAudioProcessor processor;
            assert(!processor.Start());
            assert(!processor.Initialize(nullptr, 60, nullptr));
            assert(!processor.Initialize(&codec, 0, nullptr));
            failure = stage;
            assert(!processor.Initialize(&codec, 60, nullptr));
            assert(!processor.Start());
            assert(live_configs == 0 && live_data == 0);
            processor.EnableDeviceAec(true);
            failure = Failure::None;
            assert(processor.Initialize(&codec, 60, nullptr));
            assert(!reference_enabled);
            assert(processor.Initialize(&codec, 60, nullptr));
            assert(!processor.Initialize(&codec, 20, nullptr));
            assert(processor.Start());
            assert(processor.Stop());
        }
        JoinWorkers();
        assert(live_configs == 0 && live_data == 0);
    }
    std::cout << "PASS checked AFE initialization failure and retry\n";
}

template<class Processor>
void TestDrain(const char* name) {
    Codec codec(3, true);
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    int outputs = 0;
    {
        Processor processor;
        assert(processor.Initialize(&codec, 1, nullptr));
        processor.OnOutput([&](std::vector<int16_t>&& frame) {
            assert(frame.size() == 16);
            for (auto sample : frame) assert(sample == 123);
            std::unique_lock<std::mutex> lock(mutex);
            ++outputs;
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&]() { return release; });
        });
        assert(processor.Start());
        std::thread feeder([&]() {
            std::vector<int16_t> data;
            for (int i = 0; i < 32; ++i) data.insert(data.end(), {123, -777, 30000});
            processor.Feed(std::move(data));
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            assert(cv.wait_for(lock, 2s, [&]() { return entered; }));
        }
        const auto before = std::chrono::steady_clock::now();
        assert(!processor.Stop(10));
        assert(std::chrono::steady_clock::now() - before < 500ms);
        assert(!processor.IsRunning());
        assert(!processor.Start());
        {
            std::lock_guard<std::mutex> lock(mutex);
            release = true;
            cv.notify_all();
        }
        feeder.join();
        assert(processor.Stop(1000));
        assert(outputs == 1);
        assert(processor.Start());
        std::this_thread::sleep_for(20ms);
        assert(processor.Stop(1000));
        assert(outputs == 1);
    }
    JoinWorkers();
    std::cout << "PASS " << name << " callback timeout, channel selection and clean restart\n";
}

void TestDestructorDrain() {
    Codec codec;
    auto processor = std::make_unique<AfeAudioProcessor>();
    assert(processor->Initialize(&codec, 1, nullptr));
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    processor->OnOutput([&](std::vector<int16_t>&&) { entered.set_value(); released.wait(); });
    assert(processor->Start());
    processor->Feed(std::vector<int16_t>(16, 10));
    assert(entered.get_future().wait_for(2s) == std::future_status::ready);
    auto destruction = std::async(std::launch::async, [&]() { processor.reset(); });
    assert(destruction.wait_for(20ms) == std::future_status::timeout);
    release.set_value();
    assert(destruction.wait_for(2s) == std::future_status::ready);
    destruction.get();
    JoinWorkers();
    assert(live_data == 0);
    std::cout << "PASS AFE destruction waits for output owner\n";
}

void TestBypassValidation() {
    NoAudioProcessor processor;
    Codec reference_only(1, true), invalid(0), codec;
    assert(!processor.Start());
    assert(!processor.Initialize(&reference_only, 60, nullptr));
    assert(!processor.Initialize(&invalid, 60, nullptr));
    assert(!processor.Initialize(&codec, -1, nullptr));
    assert(!processor.Initialize(&codec, 121, nullptr));
    assert(processor.Initialize(&codec, 60, nullptr));
    assert(processor.Start());
    assert(processor.Stop());
    std::cout << "PASS bypass input validation\n";
}

void TestResetFailure() {
    Codec codec;
    {
        AfeAudioProcessor processor;
        assert(processor.Initialize(&codec, 60, nullptr));
        failure = Failure::Reset;
        assert(!processor.Stop());
        assert(!processor.Start());
        failure = Failure::None;
        assert(processor.Stop());
        assert(processor.Start());
        assert(processor.Stop());
    }
    JoinWorkers();
    std::cout << "PASS AFE reset failure retains restart barrier\n";
}

void TestLocalEndpointingWithAec() {
    Codec codec(3, true);
    {
        AfeAudioProcessor processor;
        assert(!processor.SetLocalEndpointing(true));
        assert(processor.Initialize(&codec, 60, nullptr));
        assert(aec_enabled && !vad_enabled);
        assert(processor.SetLocalEndpointing(true));
        assert(aec_enabled && vad_enabled);
        assert(processor.Start());
        processor.EnableDeviceAec(true);
        assert(aec_enabled && vad_enabled && processor.IsRunning());
        assert(processor.SetLocalEndpointing(false));
        assert(aec_enabled && !vad_enabled && !processor.IsRunning());
        failure = Failure::Vad;
        assert(!processor.SetLocalEndpointing(true));
        assert(!processor.Start());
        failure = Failure::None;
        assert(processor.SetLocalEndpointing(true) && processor.Start());
        failure = Failure::Aec;
        processor.EnableDeviceAec(false);
        assert(!processor.Start());
        failure = Failure::None;
        assert(!processor.SetLocalEndpointing(true));
        processor.EnableDeviceAec(false);
        assert(!aec_enabled && vad_enabled && processor.Start());
        assert(processor.Stop());
    }
    JoinWorkers();
    NoAudioProcessor bypass;
    assert(!bypass.SetLocalEndpointing(true) && bypass.SetLocalEndpointing(false));
    std::cout << "PASS local endpointing retains AEC, restores manual VAD mode and rejects failed mode changes\n";
}

void PutU32(std::vector<uint8_t>& data, size_t at, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) data.at(at + i) = value >> (8 * i);
}
void TestSpeechMonitorIsolation() {
    Codec codec(3, true);
    std::atomic<unsigned> normal_pcm{0}, normal_vad{0}, monitored{0};
    std::promise<void> entered, release, normal_output;
    auto released = release.get_future().share();
    {
        AfeAudioProcessor processor;
        assert(!processor.SetSpeechMonitor(true));
        assert(processor.Initialize(&codec, 1, nullptr));
        assert(!processor.SetSpeechMonitor(true));
        processor.OnOutput([&](std::vector<int16_t>&& pcm) {
            assert(pcm.size() == 16 && pcm[0] == 123);
            ++normal_pcm; normal_output.set_value();
        });
        processor.OnVadStateChange([&](bool speech) { assert(speech); ++normal_vad; });
        processor.OnSpeechMonitor([&](const int16_t* pcm, size_t count, bool speech) {
            assert(count == 16 && speech);
            for (size_t i = 0; i < count; ++i) assert(pcm[i] == 123);
            ++monitored; entered.set_value(); released.wait();
        });
        assert(processor.SetSpeechMonitor(true) && aec_enabled && vad_enabled);
        assert(processor.SetLocalEndpointing(false) && vad_enabled);
        assert(processor.Start());
        fixture_speech = true;
        std::vector<int16_t> input;
        for (int i = 0; i < 16; ++i) input.insert(input.end(), {123, -777, 30000});
        processor.Feed(std::vector<int16_t>(input));
        assert(entered.get_future().wait_for(2s) == std::future_status::ready);
        assert(!processor.Stop(10) && !processor.Start());
        assert(normal_pcm == 0 && normal_vad == 0 && monitored == 1);
        release.set_value();
        assert(processor.Stop(1000));
        assert(processor.SetSpeechMonitor(false) && !vad_enabled);
        assert(processor.SetLocalEndpointing(true) && vad_enabled);
        assert(processor.Start());
        processor.Feed(std::move(input));
        assert(normal_output.get_future().wait_for(2s) == std::future_status::ready);
        assert(processor.Stop(1000));
        assert(normal_pcm == 1 && normal_vad == 1 && monitored == 1);
    }
    fixture_speech = false;
    JoinWorkers();
    std::cout << "PASS monitor callback isolation, borrowed mono audio, bounded drain and normal capture restoration\n";
}

void TestCapturePreroll() {
    Codec codec(3, true);
    std::promise<void> complete;
    std::vector<int16_t> received;
    std::atomic<unsigned> frames{0};
    {
        AfeAudioProcessor processor;
        assert(!processor.SetCapturePreroll({1, 2}));
        assert(processor.Initialize(&codec, 1, nullptr));
        assert(!processor.SetCapturePreroll({1, 2}));
        assert(processor.SetLocalEndpointing(true));
        assert(!processor.SetCapturePreroll(std::vector<int16_t>(16001)));
        std::vector<int16_t> prefix(21);
        for (int i = 0; i < 21; ++i) prefix[i] = 1000 + i;
        assert(processor.SetCapturePreroll(std::move(prefix)));
        processor.OnOutput([&](std::vector<int16_t>&& pcm) {
            received.insert(received.end(), pcm.begin(), pcm.end());
            if (++frames == 2) complete.set_value();
        });
        assert(processor.Start());
        assert(!processor.SetCapturePreroll({99}));
        std::vector<int16_t> input;
        for (int i = 0; i < 16; ++i) input.insert(input.end(), {123, 0, 0});
        processor.Feed(std::move(input));
        assert(complete.get_future().wait_for(2s) == std::future_status::ready);
        assert(processor.Stop(1000));
        assert(received.size() == 32);
        for (int i = 0; i < 21; ++i) assert(received[i] == 1000 + i);
        for (int i = 21; i < 32; ++i) assert(received[i] == 123);
        // Stop discards the five remaining samples; no prefix crosses turns.
        received.clear(); frames = 0;
        std::promise<void> next;
        processor.OnOutput([&](std::vector<int16_t>&& pcm) {
            assert(pcm.size() == 16);
            for (auto value : pcm) assert(value == 456);
            ++frames; next.set_value();
        });
        assert(processor.Start());
        input.clear();
        for (int i = 0; i < 16; ++i) input.insert(input.end(), {456, 0, 0});
        processor.Feed(std::move(input));
        assert(next.get_future().wait_for(2s) == std::future_status::ready);
        assert(processor.Stop(1000) && frames == 1);
    }
    JoinWorkers();
    NoAudioProcessor bypass;
    assert(!bypass.SetCapturePreroll({1}));
    std::cout << "PASS bounded capture preroll, frame assembly order and retired-tail isolation\n";
}

void TestSpeechMonitorAdmission() {
    NoAudioProcessor bypass;
    assert(!bypass.SetSpeechMonitor(true) && bypass.SetSpeechMonitor(false));
    for (bool reference : {false, true}) {
        Codec codec(reference ? 3 : 1, reference);
        {
            AfeAudioProcessor processor;
            processor.OnSpeechMonitor([](const int16_t*, size_t, bool) {});
            assert(processor.Initialize(&codec, 1, nullptr));
            if (!reference) assert(!processor.SetSpeechMonitor(true));
            else {
                failure = Failure::Vad;
                assert(!processor.SetSpeechMonitor(true) && !processor.Start());
                failure = Failure::None;
                assert(processor.SetSpeechMonitor(true) && processor.Start());
                processor.EnableDeviceAec(false);
                assert(!processor.IsRunning() && !processor.Start() && !processor.SetSpeechMonitor(true));
                processor.EnableDeviceAec(true);
                assert(processor.SetSpeechMonitor(true) && processor.Start());
                failure = Failure::Aec;
                processor.EnableDeviceAec(false);
                assert(!processor.Start() && !processor.SetSpeechMonitor(false));
                failure = Failure::None;
                processor.EnableDeviceAec(true);
                assert(processor.SetSpeechMonitor(false) && processor.Start());
                assert(processor.Stop());
            }
        }
        JoinWorkers();
    }
    assert(live_data == 0 && live_configs == 0);
    std::cout << "PASS monitor requires reference, AEC, VAD and callback; failed modes cannot restart\n";
}
void TestBundles() {
    std::vector<uint8_t> model(96, 0);
    PutU32(model, 0, 1);
    std::memcpy(model.data() + 4, "vadnet", 6);
    PutU32(model, 36, 1);
    std::memcpy(model.data() + 40, "weights", 7);
    PutU32(model, 72, 80);
    PutU32(model, 76, 16);
    assert(audream::ValidateAssistantModelBundle(model.data(), model.size()));
    for (size_t n = 0; n < model.size(); ++n) assert(!audream::ValidateAssistantModelBundle(model.data(), n));
    for (size_t at : {0, 36, 72, 76}) {
        auto corrupt = model;
        PutU32(corrupt, at, UINT32_MAX);
        assert(!audream::ValidateAssistantModelBundle(corrupt.data(), corrupt.size()));
    }
    auto corrupt = model;
    PutU32(corrupt, 72, 4);
    assert(!audream::ValidateAssistantModelBundle(corrupt.data(), corrupt.size()));
    std::fill(corrupt.begin() + 4, corrupt.begin() + 36, 'x');
    assert(!audream::ValidateAssistantModelBundle(corrupt.data(), corrupt.size()));

    std::vector<uint8_t> assets(58 + model.size(), 0);
    PutU32(assets, 0, 1);
    PutU32(assets, 8, assets.size() - 12);
    std::memcpy(assets.data() + 12, "srmodels.bin", 12);
    PutU32(assets, 44, model.size());
    assets[56] = assets[57] = 'Z';
    std::copy(model.begin(), model.end(), assets.begin() + 58);
    assert(audream::ValidateMappedAssetBundle(assets.data(), assets.size()));
    for (size_t n = 0; n < assets.size(); ++n) assert(!audream::ValidateMappedAssetBundle(assets.data(), n));
    for (size_t at : {0, 8, 44, 48}) {
        auto broken = assets;
        PutU32(broken, at, UINT32_MAX);
        assert(!audream::ValidateMappedAssetBundle(broken.data(), broken.size()));
    }
    assets[56] = 0;
    assert(!audream::ValidateMappedAssetBundle(assets.data(), assets.size()));
    std::cout << "PASS model and mapped asset bounds validation\n";
}

int main() {
    TestCapturePreroll();
#if CONFIG_USE_DEVICE_AEC
    TestSpeechMonitorIsolation();
    TestSpeechMonitorAdmission();
    TestLocalEndpointingWithAec();
#else
    {
        Codec codec(3, true);
        AfeAudioProcessor processor;
        processor.OnSpeechMonitor([](const int16_t*, size_t, bool) { assert(false); });
        assert(processor.Initialize(&codec, 1, nullptr));
        assert(!aec_enabled && !processor.SetSpeechMonitor(true));
        processor.EnableDeviceAec(true);
        assert(!processor.SetSpeechMonitor(true));
        assert(processor.Start() && processor.Stop());
    }
    JoinWorkers();
    std::cout << "PASS monitor rejected when device AEC is not compiled\n";
#endif
    TestInitializationFailures();
    TestBypassValidation();
    TestDrain<AfeAudioProcessor>("AFE");
    TestDrain<NoAudioProcessor>("bypass");
    TestDestructorDrain();
    TestResetFailure();
    TestBundles();
}
