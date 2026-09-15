#include "audio/wake_words/afe_wake_word.h"
#include <esp_opus_enc.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <climits>
#include <cstring>
#include <future>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;
namespace {
enum class Failure { None, Models, Words, Config, Interface, Data, Chunk, Task, Reset,
    Encoder, FrameSize, Packet };
std::atomic<Failure> failure{Failure::None};
std::atomic<int> live_data{0}, live_configs{0}, live_encoders{0}, resets{0}, feeds{0};
std::vector<std::thread> workers;
srmodel_list_t models;
char model_one[] = "wn_first", model_two[] = "wn_second";
char words_one[] = "first;second", words_two[] = "third";
std::string configured_format;

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool blocked = false, entered = false;
    void Block() { std::lock_guard<std::mutex> lock(mutex); blocked = true; entered = false; }
    void Hit() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; cv.notify_all();
        cv.wait(lock, [&]() { return !blocked; });
    }
    void Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, 2s, [&]() { return entered; }));
    }
    void Release() { std::lock_guard<std::mutex> lock(mutex); blocked = false; cv.notify_all(); }
} fetch_gate, feed_gate, encode_gate;

void JoinWorkers() {
    for (auto& worker : workers) worker.join();
    workers.clear();
    assert(live_data == 0 && live_configs == 0 && live_encoders == 0);
}
struct Frame { std::vector<int16_t> pcm; int model = 0, word = 0; };
}

struct esp_afe_sr_data_t {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Frame> queue;
    Frame fetched;
    afe_fetch_result_t result;
};
esp_afe_sr_data_t* current_data = nullptr;

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
        input_channels_ = channels; input_reference_ = reference;
    }
    int Read(int16_t*, int) override { return 0; }
    int Write(const int16_t*, int) override { return 0; }
};

srmodel_list_t* esp_srmodel_init(const char*) { return failure == Failure::Models ? nullptr : &models; }
char* esp_srmodel_get_wake_words(srmodel_list_t* list, char* name) {
    assert(list == &models);
    if (failure == Failure::Words) return nullptr;
    return name == model_one ? words_one : words_two;
}
afe_config_t* afe_config_init(const char* format, srmodel_list_t* list, int type, int) {
    assert(list == &models && type == AFE_TYPE_SR);
    configured_format = format;
    if (failure == Failure::Config) return nullptr;
    ++live_configs;
    auto* config = new afe_config_t;
    config->wakenet_model_name = model_one;
    config->wakenet_model_name_2 = model_two;
    return config;
}
void afe_config_free(afe_config_t* config) { --live_configs; delete config; }
const esp_afe_sr_iface_t* esp_afe_handle_from_config(afe_config_t*) {
    static const esp_afe_sr_iface_t iface = {
        [](afe_config_t*) -> esp_afe_sr_data_t* {
            if (failure == Failure::Data) return nullptr;
            ++live_data;
            return current_data = new esp_afe_sr_data_t;
        },
        [](esp_afe_sr_data_t*) { return failure == Failure::Chunk ? 0 : 16; },
        [](esp_afe_sr_data_t*) { return 16; },
        [](esp_afe_sr_data_t* data) { --live_data; delete data; current_data = nullptr; },
        [](esp_afe_sr_data_t*, const int16_t*) { feed_gate.Hit(); ++feeds; return 0; },
        [](esp_afe_sr_data_t* data, TickType_t timeout) -> afe_fetch_result_t* {
            assert(timeout == pdMS_TO_TICKS(100));
            std::unique_lock<std::mutex> lock(data->mutex);
            if (!data->cv.wait_for(lock, std::chrono::milliseconds(timeout), [&]() { return !data->queue.empty(); })) return nullptr;
            data->fetched = std::move(data->queue.front());
            data->queue.pop_front();
            data->result.data = data->fetched.pcm.data();
            data->result.data_size = data->fetched.pcm.size() * sizeof(int16_t);
            data->result.wakeup_state = WAKENET_DETECTED;
            data->result.wakenet_model_index = data->fetched.model;
            data->result.wake_word_index = data->fetched.word;
            lock.unlock();
            fetch_gate.Hit();
            return &data->result;
        },
        [](esp_afe_sr_data_t* data) {
            if (failure == Failure::Reset) return -1;
            std::lock_guard<std::mutex> lock(data->mutex);
            ++resets; data->queue.clear(); return 0;
        },
        nullptr, nullptr, nullptr, nullptr
    };
    return failure == Failure::Interface ? nullptr : &iface;
}
BaseType_t xTaskCreateWithCaps(void (*fn)(void*), const char*, unsigned stack,
    void* arg, unsigned, TaskHandle_t*, unsigned caps) {
    assert(stack == 24576 && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (failure == Failure::Task) return 0;
    workers.emplace_back([=]() { fn(arg); });
    return pdPASS;
}
void vTaskDeleteWithCaps(TaskHandle_t task) { assert(!task); }

esp_audio_err_t esp_opus_enc_open(void* cfg, uint32_t size, void** handle) {
    const auto& config = *static_cast<esp_opus_enc_config_t*>(cfg);
    assert(size == sizeof(config) && config.sample_rate == 16000 && config.channel == 1);
    assert(config.frame_duration == ESP_OPUS_ENC_FRAME_DURATION_60_MS && config.complexity == 0);
    if (failure == Failure::Encoder) return ESP_AUDIO_ERR_FAIL;
    *handle = new int(1); ++live_encoders; return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_opus_enc_get_frame_size(void*, int* input, int* output) {
    *input = failure == Failure::FrameSize ? 0 : 1920;
    *output = 1275; return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_opus_enc_process(void*, esp_audio_enc_in_frame_t* in, esp_audio_enc_out_frame_t* out) {
    assert(in->len == 1920 && out->len == 1275);
    encode_gate.Hit();
    out->buffer[0] = 42;
    out->encoded_bytes = failure == Failure::Packet ? out->len + 1 : 1;
    return ESP_AUDIO_ERR_OK;
}
void esp_opus_enc_close(void* encoder) { --live_encoders; delete static_cast<int*>(encoder); }

void Inject(int model = 1, int word = 1, size_t samples = 1920) {
    auto* data = current_data;
    std::lock_guard<std::mutex> lock(data->mutex);
    data->queue.push_back({std::vector<int16_t>(samples, 123), model, word});
    data->cv.notify_all();
}
void Detect(AfeWakeWord& wake) {
    std::promise<void> detected;
    wake.OnWakeWordDetected([&](const std::string&) { detected.set_value(); });
    assert(wake.StartChecked());
    Inject();
    assert(detected.get_future().wait_for(2s) == std::future_status::ready);
    wake.Stop();
    wake.OnWakeWordDetected({});
}

void TestInitialization() {
    Codec codec(3, true), invalid(1, true);
    for (auto stage : {Failure::Models, Failure::Words, Failure::Config, Failure::Interface,
        Failure::Data, Failure::Chunk, Failure::Task}) {
        {
            AfeWakeWord wake;
            assert(!wake.StartChecked());
            assert(!wake.Initialize(nullptr, nullptr));
            assert(!wake.Initialize(&invalid, nullptr));
            failure = stage;
            assert(!wake.Initialize(&codec, nullptr));
            assert(live_data == 0 && live_configs == 0);
            failure = Failure::None;
            assert(wake.Initialize(&codec, &models));
            assert(configured_format == "MMR");
            assert(wake.StartChecked());
            assert(wake.StopAndDrain(1000));
        }
        JoinWorkers();
    }
    std::cout << "PASS initialization failures, retry, reference layout and shared model ownership\n";
}

void TestIndicesAndPreroll() {
    Codec codec;
    {
        AfeWakeWord wake;
        assert(wake.Initialize(&codec, &models));
        std::promise<std::string> detected;
        wake.OnWakeWordDetected([&](const std::string& word) { detected.set_value(word); });
        assert(wake.StartChecked());
        Inject(INT_MIN, INT_MIN);
        Inject(1, 99);
        for (int i = 0; i < 24; ++i) Inject(0, 0);
        Inject(1, 2);
        auto result = detected.get_future();
        assert(result.wait_for(2s) == std::future_status::ready);
        assert(result.get() == "second");
        wake.Stop(); // Legacy stop preserves the detected PCM snapshot.
        wake.EncodeWakeWordData();
        std::vector<uint8_t> opus;
        int packets = 0;
        while (wake.GetWakeWordOpus(opus)) { assert(opus == std::vector<uint8_t>{42}); ++packets; }
        assert(packets == 32000 / 960);
        wake.EncodeWakeWordData(); // Empty duplicate requests preserve completed EOF.
        assert(!wake.GetWakeWordOpus(opus));
        assert(wake.StopAndDrain(1000));
        wake.EncodeWakeWordData();
        assert(!wake.GetWakeWordOpus(opus));
        std::promise<std::string> second_model;
        wake.OnWakeWordDetected([&](const std::string& word) { second_model.set_value(word); });
        assert(wake.StartChecked());
        Inject(2, 1);
        auto second = second_model.get_future();
        assert(second.wait_for(2s) == std::future_status::ready && second.get() == "third");
        assert(wake.StopAndDrain(1000));
    }
    JoinWorkers();
    std::cout << "PASS model/word indices, bounded two-second preroll and explicit EOF\n";
}

void TestFetchAndFeedDrain() {
    Codec codec;
    {
        AfeWakeWord wake;
        assert(wake.Initialize(&codec, &models));
        std::atomic<int> callbacks{0};
        wake.OnWakeWordDetected([&](const std::string&) { ++callbacks; });
        fetch_gate.Block();
        assert(wake.StartChecked()); Inject(); fetch_gate.Wait();
        const auto before = resets.load();
        assert(!wake.StopAndDrain(10));
        assert(!wake.StartChecked() && resets == before);
        fetch_gate.Release();
        assert(wake.StopAndDrain(1000) && callbacks == 0);
        assert(wake.StartChecked());
        feed_gate.Block();
        std::thread feeder([&]() { wake.Feed(std::vector<int16_t>(32, 12)); });
        feed_gate.Wait();
        assert(!wake.StopAndDrain(150));
        assert(!wake.StartChecked());
        feed_gate.Release(); feeder.join();
        assert(wake.StopAndDrain(1000));
        failure = Failure::Reset;
        assert(!wake.StartChecked());
        failure = Failure::None;
        assert(!wake.StartChecked()); // A failed reset needs explicit drain recovery.
        assert(wake.StopAndDrain(1000) && wake.StartChecked());
        assert(wake.StopAndDrain(1000));
    }
    JoinWorkers();
    std::cout << "PASS stale fetch suppression, feed drain, reset failure and restart inhibition\n";
}

void TestCallbackDrain() {
    Codec codec;
    {
        AfeWakeWord wake;
        Gate callback;
        callback.Block();
        assert(wake.Initialize(&codec, &models));
        wake.OnWakeWordDetected([&](const std::string&) { callback.Hit(); });
        assert(wake.StartChecked()); Inject(); callback.Wait();
        assert(!wake.StopAndDrain(10));
        callback.Release();
        assert(wake.StopAndDrain(1000));
    }
    JoinWorkers();
    std::cout << "PASS handoff waits for in-flight wake callback\n";
}

void TestEncoderDrainAndFailure() {
    Codec codec;
    for (auto stage : {Failure::Encoder, Failure::FrameSize, Failure::Packet}) {
        {
            AfeWakeWord wake;
            assert(wake.Initialize(&codec, &models)); Detect(wake);
            failure = stage;
            wake.EncodeWakeWordData();
            std::vector<uint8_t> opus;
            assert(!wake.GetWakeWordOpus(opus));
            failure = Failure::None;
            assert(wake.StopAndDrain(1000));
        }
        JoinWorkers();
    }
    {
        AfeWakeWord wake;
        assert(wake.Initialize(&codec, &models)); Detect(wake);
        encode_gate.Block(); wake.EncodeWakeWordData(); encode_gate.Wait();
        wake.EncodeWakeWordData(); // A duplicate request must not spawn another worker.
        assert(live_encoders == 1 && workers.size() == 1);
        auto reader = std::async(std::launch::async, [&]() {
            std::vector<uint8_t> opus; return wake.GetWakeWordOpus(opus);
        });
        assert(!wake.StopAndDrain(10));
        assert(reader.wait_for(500ms) == std::future_status::ready && !reader.get());
        assert(!wake.StartChecked());
        encode_gate.Release();
        assert(wake.StopAndDrain(1000));
        std::vector<uint8_t> opus;
        assert(!wake.GetWakeWordOpus(opus));
    }
    JoinWorkers();
    std::cout << "PASS encoder failures, duplicate suppression, cancellation and reader release\n";
}

void TestDestructionAndReaderTimeout() {
    Codec codec;
    auto wake = std::make_unique<AfeWakeWord>();
    assert(wake->Initialize(&codec, &models)); Detect(*wake);
    encode_gate.Block(); wake->EncodeWakeWordData(); encode_gate.Wait();
    std::vector<uint8_t> opus;
    const auto start = std::chrono::steady_clock::now();
    assert(!wake->GetWakeWordOpus(opus));
    assert(std::chrono::steady_clock::now() - start < 4s);
    auto destructor = std::async(std::launch::async, [&]() { wake.reset(); });
    assert(destructor.wait_for(20ms) == std::future_status::timeout);
    assert(live_data == 1 && live_encoders == 1);
    encode_gate.Release();
    assert(destructor.wait_for(1s) == std::future_status::ready); destructor.get();
    JoinWorkers();
    std::cout << "PASS bounded reader timeout and destruction waits for encoder ownership\n";
}

int main() {
    TestInitialization(); TestIndicesAndPreroll(); TestFetchAndFeedDrain();
    TestCallbackDrain(); TestEncoderDrainAndFailure(); TestDestructionAndReaderTimeout();
}
