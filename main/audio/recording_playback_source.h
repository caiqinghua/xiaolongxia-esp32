#pragma once

#include "recording_packet_reader.h"
#include <array>

// No audio decoding during indexing or packet location. The caller owns the
// same storage lease throughout indexing, playback and seek.
class RecordingPlaybackSource {
public:
    static constexpr size_t kIndexPoints = 128;
    using Cancel = std::function<bool()>;
    using Yield = std::function<void()>;
    bool Open(const char* path, uint64_t bytes, uint32_t rate, const Cancel& cancel = {}, const Yield& yield = {});
    bool Seek(uint64_t output_sample, uint64_t& raw_start, const Cancel& cancel = {}, const Yield& yield = {});
    RecordingReadStatus Next(RecordingOpusPacket& packet, const Cancel& cancel = {}) { return reader_.Next(packet, cancel); }
    bool Close() { ready_ = false; return reader_.Close(); }
    RecordingOpusInfo Info() const { return reader_.Info(); }
    RecordingReadError Error() const { return error_ == RecordingReadError::kNone ? reader_.Error() : error_; }
    RecordingEndKind EndKind() const { return reader_.EndKind(); }
    uint64_t TotalSamples() const { return total_samples_; }
    uint64_t PreSkipSamples() const { return pre_skip_; }
    size_t IndexSize() const { return points_; }
    uint64_t InspectedPackets() const { return inspected_; }

private:
    struct Point { RecordingReaderBookmark bookmark; uint64_t raw_sample = 0; };
    RecordingPacketReader reader_;
    std::array<Point, kIndexPoints> index_{};
    size_t points_ = 0;
    uint64_t stride_ = 1;
    uint64_t inspected_ = 0;
    uint64_t total_samples_ = 0;
    uint64_t pre_skip_ = 0;
    uint32_t rate_ = 0;
    RecordingReadError error_ = RecordingReadError::kNone;
    bool ready_ = false;
    bool Fail(RecordingReadError error) { error_ = error; return false; }
    bool PacketSamples(const RecordingOpusPacket& packet, uint64_t& samples);
    void AddPoint(const RecordingOpusPacket& packet, uint64_t sample, uint64_t ordinal);
};
