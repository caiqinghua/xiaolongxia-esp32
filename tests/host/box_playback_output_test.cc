// Actual BoxAudioCodec header and playback implementation; only hardware,
// construction and unrelated legacy methods are host adapters.
#include "audio/codecs/box_audio_codec.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <cmath>

namespace {
bool tx = false, opened = false;
int pa = 0, open_error = 0, volume_error = 0, close_error = 0;
int register_volume_error = 0, cached_volume = 70;
unsigned register_volume_calls = 0, cached_volume_calls = 0;
float register_db = 0;
int SetHardwareVolume(const audio_codec_if_t*, float db) {
    assert(opened);
    ++register_volume_calls;
    if (!register_volume_error) register_db = db;
    return register_volume_error;
}
audio_codec_if_t output_interface{};
int query_error = 0, enable_error = 0, disable_error = 0, gpio_error = 0, write_error = 0;
bool short_write = false, stuck_tx = false, stuck_pa = false;
uint32_t dma_bytes = 2880;
size_t data_bytes = 0, silence_bytes = 0, max_timeout = 0;
std::deque<uint8_t> dma;
std::vector<uint8_t> transmitted;
void Reset() {
    tx = opened = false; pa = 0;
    open_error = volume_error = close_error = query_error = enable_error = disable_error = gpio_error = write_error = 0;
    register_volume_error = 0; cached_volume = 70;
    register_volume_calls = cached_volume_calls = 0; register_db = 0;
    output_interface.set_vol = SetHardwareVolume;
    short_write = stuck_tx = stuck_pa = false;
    data_bytes = silence_bytes = max_timeout = 0;
    dma_bytes = 2880; dma.clear(); transmitted.clear();
    Board::profile = BoardRuntimeProfile::kWatchAssistantRecorder;
}
}

int i2s_channel_get_info(i2s_chan_handle_t, i2s_chan_info_t* info) {
    *info = {tx, dma_bytes}; return query_error;
}
int i2s_channel_enable(i2s_chan_handle_t) { if (!enable_error) tx = true; return enable_error; }
int i2s_channel_disable(i2s_chan_handle_t) { if (!disable_error && !stuck_tx) tx = false; return disable_error; }
int gpio_config(const gpio_config_t* config) {
    assert(config->mode == GPIO_MODE_INPUT_OUTPUT && config->pin_bit_mask == (1ULL << 9)); return gpio_error;
}
int gpio_set_level(gpio_num_t pin, uint32_t level) {
    assert(pin == 9 && level == 0); if (!gpio_error && !stuck_pa) pa = 0; return gpio_error;
}
int gpio_get_level(gpio_num_t) { return pa; }
int i2s_channel_write(i2s_chan_handle_t, const void* buffer, size_t size, size_t* written, uint32_t timeout) {
    assert(tx && timeout > 0 && timeout <= 500);
    max_timeout = std::max<size_t>(max_timeout, timeout);
    *written = write_error ? 0 : (short_write ? size / 2 : size);
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    const bool silent = std::all_of(bytes, bytes + size, [](uint8_t b) { return b == 0; });
    (silent ? silence_bytes : data_bytes) += *written;
    for (size_t i = 0; i < *written; ++i) {
        dma.push_back(bytes[i]);
        if (dma.size() > dma_bytes) { transmitted.push_back(dma.front()); dma.pop_front(); }
    }
    return write_error;
}
extern "C" int esp_codec_dev_open(esp_codec_dev_handle_t, esp_codec_dev_sample_info_t* info) {
    assert(info->bits_per_sample == 16 && info->channel == 1 && info->sample_rate == 16000);
    if (!open_error) { opened = true; pa = 1; } return open_error;
}
extern "C" int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t, int volume) {
    assert(volume >= 0 && volume <= 100);
    ++cached_volume_calls;
    if (!volume_error) cached_volume = volume;
    return volume_error;
}
extern "C" int esp_codec_dev_close(esp_codec_dev_handle_t) {
    if (!close_error) opened = false; return close_error;
}

AudioCodec::AudioCodec() = default;
AudioCodec::~AudioCodec() = default;
void AudioCodec::EnableOutput(bool enabled) { output_enabled_ = enabled; }
void AudioCodec::EnableInput(bool enabled) { input_enabled_ = enabled; }
void AudioCodec::SetOutputVolume(int volume) { output_volume_ = volume; }
void AudioCodec::SetInputGain(float gain) { input_gain_ = gain; }
void AudioCodec::PrepareForLocalRecording() {}
void AudioCodec::PrepareForDeepSleep() {}
void AudioCodec::OutputData(std::vector<int16_t>&) {}
bool AudioCodec::InputData(std::vector<int16_t>&) { return false; }
void AudioCodec::Start() {}
BoxAudioCodec::BoxAudioCodec(void*, int, int rate, gpio_num_t, gpio_num_t, gpio_num_t, gpio_num_t, gpio_num_t,
    gpio_num_t pin, uint8_t, uint8_t, bool, uint8_t, bool output_path, uint8_t) {
    output_sample_rate_ = rate; output_path_enabled_ = output_path; pa_pin_ = pin;
    output_dev_ = this; tx_handle_ = this;
    out_codec_if_ = &output_interface;
}
BoxAudioCodec::~BoxAudioCodec() = default;
void BoxAudioCodec::SetOutputVolume(int) {}
void BoxAudioCodec::SetInputGain(float) {}
bool BoxAudioCodec::InputData(std::vector<int16_t>&) { return false; }
void BoxAudioCodec::EnableInput(bool) {}
void BoxAudioCodec::EnableOutput(bool) {}
void BoxAudioCodec::PrepareForLocalRecording() {}
void BoxAudioCodec::PrepareForDeepSleep() {}
int BoxAudioCodec::Read(int16_t*, int) { return 0; }
int BoxAudioCodec::Write(const int16_t*, int) { return 0; }

int main() {
    const std::vector<int16_t> pcm(320, 1234);
    {
        Reset();
        BoxAudioCodec codec(nullptr, 16000, 16000, 0, 0, 0, 0, 0, 9, 0, 0, false);
        for (auto profile : {BoardRuntimeProfile::kRecordingOnly, BoardRuntimeProfile::kAssistant}) {
            Board::profile = profile;
            assert(!codec.SupportsWatchPlayback() && !codec.SetWatchPlaybackOutput(true));
            assert(!codec.WriteWatchPlayback(pcm) && !tx && !opened);
            assert(!codec.SetWatchOutputVolume(20));
        }
        Board::profile = BoardRuntimeProfile::kWatchAssistantRecorder;
        assert(codec.SupportsWatchPlayback() && !codec.WriteWatchPlayback(pcm));
        assert(codec.SetWatchPlaybackOutput(true));
        assert(!codec.WriteWatchPlayback({}) && !codec.WriteWatchPlayback(std::vector<int16_t>(1921)));
        assert(codec.WriteWatchPlayback(pcm));
        assert(data_bytes == 640 && transmitted.empty());
        assert(codec.SetWatchPlaybackOutput(false));
        assert(!tx && !opened && !pa && !codec.output_enabled());
        assert(silence_bytes == 2 * dma_bytes && transmitted.size() >= 640);
        assert(std::memcmp(transmitted.data(), pcm.data(), 640) == 0);
        assert(codec.SetWatchPlaybackOutput(true) && codec.WriteWatchPlayback(pcm));
        assert(codec.SetWatchPlaybackOutput(false));
    }
    std::puts("PASS: actual playback methods gate profiles, enforce PCM bounds and drain tail before shutdown");
    for (int* fault : {&query_error, &enable_error, &open_error, &volume_error, &register_volume_error}) {
        Reset();
        BoxAudioCodec codec(nullptr, 16000, 16000, 0, 0, 0, 0, 0, 9, 0, 0, false);
        *fault = 1;
        assert(!codec.SetWatchPlaybackOutput(true) && !codec.output_enabled());
        *fault = 0;
        assert(codec.SetWatchPlaybackOutput(false) && !tx && !opened && !pa);
    }
    std::puts("PASS: failed TX and DAC preparation require explicit cleanup");
    for (int mode = 0; mode < 8; ++mode) {
        Reset();
        BoxAudioCodec codec(nullptr, 16000, 16000, 0, 0, 0, 0, 0, 9, 0, 0, false);
        assert(codec.SetWatchPlaybackOutput(true) && codec.WriteWatchPlayback(pcm));
        switch (mode) {
            case 0: write_error = 1; break;
            case 1: short_write = true; break;
            case 2: close_error = 1; break;
            case 3: disable_error = 1; break;
            case 4: stuck_tx = true; break;
            case 5: gpio_error = 1; break;
            case 6: stuck_pa = true; break;
            case 7: dma_bytes = 0; break;
        }
        if (mode < 2) assert(!codec.WriteWatchPlayback(pcm));
        assert(!codec.SetWatchPlaybackOutput(false));
        assert(!codec.SetWatchPlaybackOutput(true) && !codec.WriteWatchPlayback(pcm));
        assert(!codec.SetWatchOutputVolume(20));
        if (mode != 3 && mode != 4) assert(!tx);
        if (mode != 5 && mode != 6) assert(!pa);
        write_error = close_error = disable_error = gpio_error = 0;
        short_write = stuck_tx = stuck_pa = false; dma_bytes = 2880;
        assert(codec.SetWatchPlaybackOutput(false) && !tx && !opened && !pa);
    }
    std::puts("PASS: short writes, drain and shutdown failures remain failures with best-effort power off and retry");
    {
        Reset();
        BoxAudioCodec codec(nullptr, 16000, 16000, 0, 0, 0, 0, 0, 9, 0, 0, false);
        assert(!codec.SetWatchOutputVolume(-1) && !codec.SetWatchOutputVolume(101));
        assert(codec.SetWatchOutputVolume(0) && !tx && !opened && !pa && !cached_volume_calls && !register_volume_calls);
        assert(codec.SetWatchPlaybackOutput(true));
        assert(register_db == -96.0f && cached_volume == 0);
        for (int volume = 1; volume <= 100; ++volume) {
            assert(codec.SetWatchOutputVolume(volume) && codec.output_volume() == volume && cached_volume == volume);
            assert(std::fabs(register_db - (-50.0f + 0.5f * volume)) < 0.0001f);
        }
        register_volume_error = 1;
        assert(!codec.SetWatchOutputVolume(25) && codec.output_volume() == 100 && cached_volume == 25);
        register_volume_error = 0;
        assert(codec.SetWatchOutputVolume(100) && cached_volume == 100 && register_db == 0.0f);
        assert(codec.SetWatchPlaybackOutput(false));
        const auto calls = register_volume_calls;
        assert(codec.SetWatchOutputVolume(25) && register_volume_calls == calls && !tx && !opened && !pa);
        assert(codec.SetWatchPlaybackOutput(true) && register_db == -37.5f && cached_volume == 25);
        assert(codec.SetWatchPlaybackOutput(false));
    }
    std::puts("PASS: actual Box volume preserves the default curve, applies paused preference on resume and detects errors hidden by the SDK wrapper");
}
