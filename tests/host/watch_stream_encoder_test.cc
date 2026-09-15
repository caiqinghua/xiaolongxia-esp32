// Exercise the actual codec worker, enqueue and callback methods. Only capture,
// service lifecycle and unused decode hardware are controlled host boundaries.
#include "audio/audio_service.h"
#include "audio/watch_stream_encoder.h"
#include <opus.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {
std::thread worker;
AudioTaskType input_role = kAudioTaskTypeEncodeToLocalRecording;
std::vector<int16_t> input_pcm;
unsigned resets = 0, processes = 0;
enum class Fault { kNone, kReset, kProcess, kEmpty, kOversized };
Fault fault = Fault::kNone;
std::mutex native_mutex;
std::condition_variable native_changed;
bool blocked = false, entered = false;

OpusEncoder* CreateEncoder() {
    int error = 0;
    auto* encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_AUDIO, &error);
    assert(encoder && error == OPUS_OK);
    assert(opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0)) == OPUS_OK);
    assert(opus_encoder_ctl(encoder, OPUS_SET_DTX(1)) == OPUS_OK);
    return encoder;
}
std::vector<int16_t> Speech(int phase) {
    std::vector<int16_t> pcm(960);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<int16_t>(12000 * std::sin((i + phase) * 0.081));
    return pcm;
}
std::vector<uint8_t> Reference(OpusEncoder* encoder, const std::vector<int16_t>& pcm) {
    std::vector<uint8_t> packet(8192);
    int size = opus_encode(encoder, pcm.data(), static_cast<int>(pcm.size()), packet.data(), packet.size());
    assert(size > 0);
    packet.resize(size);
    return packet;
}
void Feed(AudioService& audio, AudioTaskType role, std::vector<int16_t> pcm) {
    input_role = role;
    input_pcm = std::move(pcm);
    audio.EncodeWakeWord();
}
void Drain(AudioService& audio) { assert(audio.WaitForLocalRecordingDrain(2000)); }
}

esp_audio_err_t esp_opus_enc_reset(void* handle) {
    ++resets;
    if (fault == Fault::kReset) return ESP_AUDIO_ERR_FAIL;
    return opus_encoder_ctl(static_cast<OpusEncoder*>(handle), OPUS_RESET_STATE) == OPUS_OK ?
        ESP_AUDIO_ERR_OK : ESP_AUDIO_ERR_FAIL;
}
esp_audio_err_t esp_opus_enc_process(void* handle, esp_audio_enc_in_frame_t* input,
                                   esp_audio_enc_out_frame_t* output) {
    {
        std::unique_lock<std::mutex> lock(native_mutex);
        entered = true;
        native_changed.notify_all();
        native_changed.wait(lock, [] { return !blocked; });
    }
    ++processes;
    if (fault == Fault::kProcess) return ESP_AUDIO_ERR_FAIL;
    if (fault == Fault::kEmpty || fault == Fault::kOversized) {
        output->encoded_bytes = fault == Fault::kEmpty ? 0 : output->len + 1;
        return ESP_AUDIO_ERR_OK;
    }
    const int size = opus_encode(static_cast<OpusEncoder*>(handle),
        reinterpret_cast<const int16_t*>(input->buffer), input->len / sizeof(int16_t),
        output->buffer, output->len);
    if (size <= 0) return ESP_AUDIO_ERR_FAIL;
    output->encoded_bytes = size;
    return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_opus_dec_decode(void*, esp_audio_dec_in_raw_t*, esp_audio_dec_out_frame_t*,
                                  esp_audio_dec_info_t*) { assert(false); return ESP_AUDIO_ERR_FAIL; }
esp_ae_err_t esp_ae_rate_cvt_get_max_out_sample_num(esp_ae_rate_cvt_handle_t, uint32_t, uint32_t*) {
    assert(false); return ESP_AE_ERR_OK;
}
esp_ae_err_t esp_ae_rate_cvt_process(esp_ae_rate_cvt_handle_t, esp_ae_sample_t, uint32_t,
                                  esp_ae_sample_t, uint32_t*) { assert(false); return ESP_AE_ERR_OK; }
EventBits_t xEventGroupGetBits(EventGroupHandle_t) { return AS_EVENT_AUDIO_PROCESSOR_RUNNING; }
AudioDebugger::~AudioDebugger() = default;
AudioService::AudioService() = default;
AudioService::~AudioService() { assert(!worker.joinable()); opus_encoder_destroy(static_cast<OpusEncoder*>(opus_encoder_)); }
void AudioService::Initialize(AudioCodec*) {
    opus_encoder_ = CreateEncoder(); encoder_frame_size_ = 960; encoder_outbuf_size_ = 8192;
    event_group_ = nullptr;
}
bool AudioService::Start() {
    service_stopped_ = false;
    worker = std::thread([this] { OpusCodecTask(); });
    return true;
}
bool AudioService::Stop() {
    if (!WaitForLocalRecordingDrain(2000)) return false;
    { std::lock_guard<std::mutex> lock(audio_queue_mutex_); service_stopped_ = true; }
    audio_queue_cv_.notify_all(); worker.join(); return true;
}
bool AudioService::AllowsAssistantAudio() const { return !service_stopped_ && !assistant_suspended_; }
void AudioService::SetDecodeSampleRate(int, int) { assert(false); }
void AudioService::EncodeWakeWord() { PushTaskToEncodeQueue(input_role, std::move(input_pcm)); }
bool AudioService::EnableLocalRecording(bool) {
    // Model the generation change during production quiescence, not its hardware.
    std::lock_guard<std::mutex> lock(audio_queue_mutex_); ++audio_generation_; return true;
}
bool AudioService::WaitForLocalRecordingDrain(uint32_t timeout) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    return audio_queue_cv_.wait_for(lock, std::chrono::milliseconds(timeout), [this] {
        return !HasLocalRecordingEncodeWorkLocked() && !audio_encode_active_ && audio_encode_queue_.empty();
    });
}
std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) return nullptr;
    auto packet = std::move(audio_send_queue_.front()); audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all(); return packet;
}

int main() {
    AudioService audio;
    audio.Initialize(nullptr);
    std::vector<std::vector<uint8_t>> packets;
    unsigned errors = 0;
    const auto callback = [&](const uint8_t* data, size_t bytes) {
        if (!data || !bytes) { ++errors; return; }
        packets.emplace_back(data, data + bytes);
    };
    audio.SetLocalRecordingPacketCallback(callback);
    assert(audio.Start());
    auto* reference = CreateEncoder();
    Feed(audio, kAudioTaskTypeEncodeToLocalRecording, Speech(0)); Drain(audio);
    assert(packets.back() == Reference(reference, Speech(0)));
    assert(audio.EnableLocalRecording(false) && audio.EnableLocalRecording(true));
    Feed(audio, kAudioTaskTypeEncodeToLocalRecording, Speech(1)); Drain(audio);
    assert(packets.back() == Reference(reference, Speech(1)));
    assert(resets == (CONFIG_AUDREAM_WATCH_UI ? 1U : 0U));
    std::puts("PASS: production encode worker preserves packet continuity across a capture generation change");

#if CONFIG_AUDREAM_WATCH_UI
    audio.SetLocalRecordingPacketCallback(callback);
    Feed(audio, kAudioTaskTypeEncodeToLocalRecording, Speech(2)); Drain(audio);
    assert(opus_encoder_ctl(reference, OPUS_RESET_STATE) == OPUS_OK);
    assert(packets.back() == Reference(reference, Speech(2)) && resets == 2);
    Feed(audio, kAudioTaskTypeEncodeToSendQueue, Speech(3)); Drain(audio);
    auto sent = audio.PopPacketFromSendQueue();
    assert(opus_encoder_ctl(reference, OPUS_RESET_STATE) == OPUS_OK);
    assert(sent && sent->payload == Reference(reference, Speech(3)) && resets == 3);
    Feed(audio, kAudioTaskTypeEncodeToTestingQueue, Speech(4)); Drain(audio);
    assert(resets == 4);
    Feed(audio, kAudioTaskTypeEncodeToLocalRecording, Speech(5)); Drain(audio);
    assert(resets == 5);
    std::puts("PASS: new files and assistant/testing/recording role changes reset the shared encoder");

    {
        std::lock_guard<std::mutex> lock(native_mutex); blocked = true; entered = false;
    }
    const auto before = packets.size();
    Feed(audio, kAudioTaskTypeEncodeToLocalRecording, Speech(6));
    {
        std::unique_lock<std::mutex> lock(native_mutex);
        assert(native_changed.wait_for(lock, std::chrono::seconds(2), [] { return entered; }));
    }
    assert(!audio.WaitForLocalRecordingDrain(10));
    audio.SetLocalRecordingPacketCallback(callback);
    {
        std::lock_guard<std::mutex> lock(native_mutex); blocked = false; native_changed.notify_all();
    }
    Drain(audio);
    assert(packets.size() == before);
    Feed(audio, kAudioTaskTypeEncodeToLocalRecording, Speech(7)); Drain(audio);
    assert(packets.size() == before + 1 && resets == 6);
    std::puts("PASS: blocked encoding remains active and an old file cannot publish into its replacement");

    for (auto injected : {Fault::kReset, Fault::kProcess, Fault::kEmpty, Fault::kOversized}) {
        audio.SetLocalRecordingPacketCallback(callback);
        fault = injected;
        const auto prior_errors = errors;
        const auto prior_packets = packets.size();
        Feed(audio, kAudioTaskTypeEncodeToLocalRecording, Speech(8)); Drain(audio);
        fault = Fault::kNone;
        const auto prior_processes = processes;
        Feed(audio, kAudioTaskTypeEncodeToLocalRecording, Speech(9)); Drain(audio);
        assert(errors == prior_errors + 2 && packets.size() == prior_packets && processes == prior_processes);
        audio.SetLocalRecordingPacketCallback(callback);
        Feed(audio, kAudioTaskTypeEncodeToLocalRecording, Speech(10)); Drain(audio);
        assert(packets.size() == prior_packets + 1);
    }
    audio.SetLocalRecordingPacketCallback(callback);
    auto prior_errors = errors;
    Feed(audio, kAudioTaskTypeEncodeToLocalRecording, std::vector<int16_t>(959)); Drain(audio);
    assert(errors == prior_errors + 1);
    std::puts("PASS: reset/process/empty/oversized/invalid-frame failures reach the recording callback and remain sticky per file");
    Feed(audio, kAudioTaskTypeEncodeToSendQueue, Speech(11)); Drain(audio);
    assert(audio.PopPacketFromSendQueue());
    const auto before_generation_change = resets;
    assert(audio.EnableLocalRecording(true));
    Feed(audio, kAudioTaskTypeEncodeToSendQueue, Speech(12)); Drain(audio);
    assert(resets == before_generation_change + 1);
    auto new_session = audio.PopPacketFromSendQueue();
    assert(opus_encoder_ctl(reference, OPUS_RESET_STATE) == OPUS_OK);
    assert(new_session && new_session->payload == Reference(reference, Speech(12)));
    std::puts("PASS: a new assistant generation resets even when the encoding role is unchanged");
#else
    Feed(audio, kAudioTaskTypeEncodeToSendQueue, Speech(3)); Drain(audio);
    auto sent = audio.PopPacketFromSendQueue();
    assert(sent && sent->payload == Reference(reference, Speech(3)));
    assert(resets == 0 && errors == 0);
    std::puts("PASS: non-watch profile retains its original encoder and packet dispatch behavior");
#endif
    assert(audio.Stop());
#if CONFIG_AUDREAM_WATCH_UI
    WatchStreamEncoder bounded;
    std::vector<uint8_t> output{1, 2, 3};
    assert(!bounded.Encode(nullptr, WatchEncodeRole::kRecording, 1, Speech(0), 960, 8192, output));
    assert(output.empty());
    assert(!bounded.Encode(reference, WatchEncodeRole::kRecording, 0, Speech(0), 960, 8192, output));
    uint64_t stream = 1;
    for (size_t capacity : {size_t(0), size_t(8193)}) {
        const auto before_processes = processes;
        assert(!bounded.Encode(reference, WatchEncodeRole::kRecording, stream++, Speech(0), 960, capacity, output));
        assert(output.empty() && processes == before_processes);
    }
    for (size_t samples : {size_t(0), size_t(1921), SIZE_MAX}) {
        const auto before_processes = processes;
        assert(!bounded.Encode(reference, WatchEncodeRole::kRecording, stream++, Speech(0), samples, 8192, output));
        assert(output.empty() && processes == before_processes);
    }
    std::puts("PASS: missing identities and invalid frame/output bounds never reach the native encoder");
#endif
    opus_encoder_destroy(reference);
}
