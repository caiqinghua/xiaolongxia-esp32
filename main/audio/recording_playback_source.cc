#include "recording_playback_source.h"
#include <limits>

// Stable public libopus ABI, also exported by the managed ESP audio codec
// archive. No second decoder/library or hand-written TOC parser is linked.
// https://opus-codec.org/docs/opus_api-1.3.1/group__opus__decoder.html
extern "C" int opus_packet_parse(const unsigned char*, int32_t, unsigned char*, const unsigned char* [48], int16_t [48], int*);
extern "C" int opus_packet_get_nb_samples(const unsigned char*, int32_t, int32_t);

bool RecordingPlaybackSource::PacketSamples(const RecordingOpusPacket& packet, uint64_t& samples) {
    if (packet.bytes.empty() || packet.bytes.size() > RecordingPacketReader::kMaxPacketBytes)
        return Fail(RecordingReadError::kLimit);
    unsigned char toc = 0;
    const unsigned char* frames[48]{};
    int16_t sizes[48]{};
    int offset = 0;
    const auto length = static_cast<int32_t>(packet.bytes.size());
    if (opus_packet_parse(packet.bytes.data(), length, &toc, frames, sizes, &offset) <= 0)
        return Fail(RecordingReadError::kContainer);
    const int count = opus_packet_get_nb_samples(packet.bytes.data(), length, static_cast<int32_t>(rate_));
    if (count <= 0 || count > static_cast<int>(rate_ * 120 / 1000)) return Fail(RecordingReadError::kContainer);
    samples = static_cast<uint64_t>(count);
    ++inspected_;
    return true;
}

void RecordingPlaybackSource::AddPoint(const RecordingOpusPacket& packet, uint64_t sample, uint64_t ordinal) {
    if (ordinal % stride_) return;
    if (points_ == index_.size()) {
        for (size_t i = 0; i < points_ / 2; ++i) index_[i] = index_[i * 2];
        points_ /= 2;
        stride_ *= 2;
        if (ordinal % stride_) return;
    }
    index_[points_++] = {packet.bookmark, sample};
}

bool RecordingPlaybackSource::Open(const char* path, uint64_t bytes, uint32_t rate, const Cancel& cancel, const Yield& yield) {
    ready_ = false; points_ = 0; stride_ = 1; inspected_ = total_samples_ = pre_skip_ = 0;
    error_ = RecordingReadError::kNone; rate_ = rate;
    if (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000)
        return Fail(RecordingReadError::kUnsupported);
    if (!reader_.Open(path, bytes)) return false;
    uint64_t raw = 0, end = 0, ordinal = 0;
    RecordingOpusPacket packet;
    while (!cancel || !cancel()) {
        const auto status = reader_.Next(packet, cancel);
        if (status == RecordingReadStatus::kPending) { if (yield) yield(); continue; }
        if (status == RecordingReadStatus::kError || status == RecordingReadStatus::kCancelled) return false;
        if (status == RecordingReadStatus::kEnd) {
            if (!points_) return Fail(RecordingReadError::kContainer);
            const auto ratio = 48000 / rate_;
            pre_skip_ = (reader_.Info().pre_skip_48k + ratio - 1) / ratio;
            total_samples_ = end > pre_skip_ ? end - pre_skip_ : 0;
            ready_ = reader_.Seek(index_[0].bookmark);
            return ready_;
        }
        uint64_t samples = 0;
        if (!PacketSamples(packet, samples) || samples > std::numeric_limits<uint64_t>::max() - raw)
            return Fail(RecordingReadError::kContainer);
        AddPoint(packet, raw, ordinal++);
        const auto before = raw;
        raw += samples;
        end = raw;
        if (!reader_.Info().legacy_audream_granules && packet.granule_position >= 0) {
            const auto granule = static_cast<uint64_t>(packet.granule_position);
            const auto ratio = 48000 / rate_;
            if (packet.eos) {
                end = granule / ratio;
                if (end < before || end > raw) return Fail(RecordingReadError::kContainer);
            } else if (raw > std::numeric_limits<uint64_t>::max() / ratio || granule != raw * ratio) {
                // Cropped/live streams with a nonzero initial granule origin
                // need a different timeline; never silently mislabel them.
                return Fail(RecordingReadError::kUnsupported);
            }
        } else if (packet.eos && !reader_.Info().legacy_audream_granules) {
            return Fail(RecordingReadError::kContainer);
        }
        if (ordinal % 64 == 0 && yield) yield();
    }
    return false;
}

bool RecordingPlaybackSource::Seek(uint64_t output_sample, uint64_t& raw_start, const Cancel& cancel, const Yield& yield) {
    if (!ready_ || output_sample > total_samples_ || (cancel && cancel())) return false;
    // RFC 7845 4.6: seek near the start uses the entire original pre-skip;
    // otherwise locate a packet at least 80 ms before the audible target.
    const uint64_t preroll = rate_ * 80 / 1000;
    const uint64_t warm = output_sample < preroll ? 0 : output_sample + pre_skip_ - preroll;
    size_t chosen = 0;
    while (chosen + 1 < points_ && index_[chosen + 1].raw_sample <= warm) ++chosen;
    if (!reader_.Seek(index_[chosen].bookmark)) return false;
    uint64_t raw = index_[chosen].raw_sample;
    unsigned count = 0;
    RecordingOpusPacket packet;
    while (!cancel || !cancel()) {
        const auto status = reader_.Next(packet, cancel);
        if (status == RecordingReadStatus::kPending) { if (yield) yield(); continue; }
        if (status != RecordingReadStatus::kPacket) return false;
        uint64_t samples = 0;
        if (!PacketSamples(packet, samples)) return false;
        if (samples > std::numeric_limits<uint64_t>::max() - raw) return Fail(RecordingReadError::kContainer);
        if (raw + samples > warm || packet.eos) {
            raw_start = raw;
            return reader_.Seek(packet.bookmark);
        }
        raw += samples;
        if (++count % 64 == 0 && yield) yield();
    }
    return false;
}
