#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

enum class RecordingReadStatus { kPending, kPacket, kEnd, kError, kCancelled };
enum class RecordingReadError {
    kNone, kNotOpen, kIo, kChanged, kContainer, kHeaders, kUnsupported, kLimit, kMemory,
};
enum class RecordingEndKind { kNone, kEos, kLegacyPageBoundary };

struct RecordingOpusInfo {
    bool valid = false;
    uint32_t input_sample_rate = 0;
    uint16_t pre_skip_48k = 0;
    int16_t output_gain_q8 = 0;
    uint8_t channels = 0;
    bool legacy_audream_granules = false;
    bool requires_eos = false;
};

// Valid only for the open reader that issued it. Callers cannot construct an
// arbitrary file offset; reopening the reader invalidates old bookmarks.
class RecordingReaderBookmark {
public:
    bool Valid() const { return owner_ != 0; }
private:
    friend class RecordingPacketReader;
    uint64_t owner_ = 0;
    uint64_t offset_ = 0;
    uint64_t skip_ = 0;
    uint32_t page_ = 0;
};

struct RecordingOpusPacket {
    std::vector<uint8_t> bytes;
    // Legacy Audream writers used input-rate granules, not the Opus 48 kHz clock.
    // Derive their playback position from decoded samples, not this field.
    int64_t granule_position = -1;
    // Continue Next() through kEnd; EOS on a packet does not validate trailing bytes.
    bool eos = false;
    RecordingReaderBookmark bookmark;
};

// Single-owner, pull-based reader. The playback worker owns the storage lease
// and must stop reading before closing; cancellation does not release that lease.
class RecordingPacketReader {
public:
    static constexpr size_t kMaxPacketBytes = 8192;
    static constexpr size_t kReadChunkBytes = 2048;
    static constexpr size_t kMaxReadsPerCall = 8;
    static constexpr size_t kMaxPagesPerCall = 16;

    RecordingPacketReader();
    ~RecordingPacketReader();
    RecordingPacketReader(const RecordingPacketReader&) = delete;
    RecordingPacketReader& operator=(const RecordingPacketReader&) = delete;

    // expected_bytes is the reviewed catalog size; zero skips that comparison.
    bool Open(const char* path, uint64_t expected_bytes = 0);
    RecordingReadStatus Next(RecordingOpusPacket& packet,
                             const std::function<bool()>& cancelled = {});
    bool Seek(const RecordingReaderBookmark& bookmark);
    bool Close();
    RecordingOpusInfo Info() const;
    RecordingReadError Error() const;
    RecordingEndKind EndKind() const;
    uint64_t BytesRead() const;
    size_t BufferCapacityBytes() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};
