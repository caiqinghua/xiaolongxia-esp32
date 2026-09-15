#pragma once

#include "recording_packet_reader.h"

// Owns an independent native Opus decoder. Only the playback worker may call it.
// No assistant models, AFE, microphone or shared assistant decoder is involved.
class RecordingPlaybackDecoder {
public:
    ~RecordingPlaybackDecoder();
    RecordingPlaybackDecoder() = default;
    RecordingPlaybackDecoder(const RecordingPlaybackDecoder&) = delete;
    RecordingPlaybackDecoder& operator=(const RecordingPlaybackDecoder&) = delete;
    bool Open(const RecordingOpusInfo& info, uint32_t output_sample_rate);
    bool Decode(const RecordingOpusPacket& packet, std::vector<int16_t>& pcm);
    bool Reset(uint64_t raw_sample);
    bool Close();
    uint32_t SampleRate() const { return sample_rate_; }
    uint64_t DecodedSamples() const { return decoded_samples_; }

private:
    void* decoder_ = nullptr;
    RecordingOpusInfo info_;
    uint32_t sample_rate_ = 0;
    uint64_t decoded_samples_ = 0;
    bool failed_ = false;
    bool eos_ = false;
};
