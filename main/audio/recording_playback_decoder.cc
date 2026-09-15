#include "recording_playback_decoder.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "esp_opus_dec.h"

RecordingPlaybackDecoder::~RecordingPlaybackDecoder() { Close(); }

bool RecordingPlaybackDecoder::Reset(uint64_t raw_sample) {
    if (!decoder_ || failed_ || esp_opus_dec_reset(decoder_) != ESP_AUDIO_ERR_OK) {
        failed_ = true; return false;
    }
    decoded_samples_ = raw_sample;
    eos_ = false;
    return true;
}

bool RecordingPlaybackDecoder::Close() {
    if (decoder_ && esp_opus_dec_close(decoder_) != ESP_AUDIO_ERR_OK) return false;
    decoder_ = nullptr;
    return true;
}

bool RecordingPlaybackDecoder::Open(const RecordingOpusInfo& info, uint32_t output_sample_rate) {
    if (!Close()) return false;
    if (!info.valid || info.channels != 1 ||
        (output_sample_rate != 8000 && output_sample_rate != 12000 && output_sample_rate != 16000 &&
         output_sample_rate != 24000 && output_sample_rate != 48000)) return false;
    info_ = info;
    sample_rate_ = output_sample_rate;
    decoded_samples_ = 0;
    failed_ = eos_ = false;
    esp_opus_dec_cfg_t config{};
    config.sample_rate = sample_rate_;
    config.channel = 1;
    config.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_120_MS;
    config.self_delimited = false;
    failed_ = esp_opus_dec_open(&config, sizeof(config), &decoder_) != ESP_AUDIO_ERR_OK || !decoder_;
    return !failed_;
}

bool RecordingPlaybackDecoder::Decode(const RecordingOpusPacket& packet, std::vector<int16_t>& pcm) {
    pcm.clear();
    if (!decoder_ || failed_ || eos_ || packet.bytes.empty() ||
        packet.bytes.size() > RecordingPacketReader::kMaxPacketBytes) return false;
    pcm.resize(sample_rate_ * 120 / 1000);
    esp_audio_dec_in_raw_t input{};
    input.buffer = const_cast<uint8_t*>(packet.bytes.data());
    input.len = static_cast<uint32_t>(packet.bytes.size());
    input.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;
    esp_audio_dec_out_frame_t output{};
    output.buffer = reinterpret_cast<uint8_t*>(pcm.data());
    output.len = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    esp_audio_dec_info_t format{};
    const auto status = esp_opus_dec_decode(decoder_, &input, &output, &format);
    if (status != ESP_AUDIO_ERR_OK || input.consumed != input.len || output.decoded_size == 0 ||
        output.decoded_size > output.len || output.decoded_size % sizeof(int16_t) != 0 ||
        format.sample_rate != sample_rate_ || format.channel != 1 || format.bits_per_sample != 16) {
        failed_ = true;
        pcm.clear();
        return false;
    }
    pcm.resize(output.decoded_size / sizeof(int16_t));
    const auto before = decoded_samples_;
    if (pcm.size() > std::numeric_limits<uint64_t>::max() - before) {
        failed_ = true; pcm.clear(); return false;
    }
    decoded_samples_ += pcm.size();
    const uint32_t ratio = 48000 / sample_rate_;
    const uint64_t skip = (info_.pre_skip_48k + ratio - 1) / ratio;
    const size_t first = before >= skip ? 0 : static_cast<size_t>(std::min<uint64_t>(pcm.size(), skip - before));
    size_t last = pcm.size();
    if (packet.eos && !info_.legacy_audream_granules) {
        if (packet.granule_position < 0) {
            failed_ = true; pcm.clear(); return false;
        }
        const auto end = static_cast<uint64_t>(packet.granule_position) / ratio;
        // A final granule may trim the last packet, not audio already published.
        if (end < before || end > decoded_samples_) {
            failed_ = true; pcm.clear(); return false;
        }
        last = static_cast<size_t>(end - before);
    }
    if (last <= first) pcm.clear();
    else {
        std::move(pcm.begin() + first, pcm.begin() + last, pcm.begin());
        pcm.resize(last - first);
    }
    // Apply only the file's mandatory Opus output gain; no extra recording boost.
    if (info_.output_gain_q8 != 0) {
        const double gain = std::pow(10.0, info_.output_gain_q8 / 5120.0);
        for (auto& sample : pcm) {
            sample = static_cast<int16_t>(std::lround(std::clamp(sample * gain, -32768.0, 32767.0)));
        }
    }
    eos_ = packet.eos;
    return true;
}
