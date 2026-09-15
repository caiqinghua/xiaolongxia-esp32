#include "audio/recording_packet_reader.h"

#include <cstdio>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <sys/stat.h>

#include <ogg/ogg.h>

namespace {
std::atomic<uint64_t> next_reader_owner{1};
uint16_t Read16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
}

uint32_t Read32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

bool ValidTags(const uint8_t* data, size_t size) {
    if (size < 16 || std::memcmp(data, "OpusTags", 8) != 0) return false;
    const auto vendor_size = Read32(data + 8);
    if (vendor_size > size - 16) return false;
    size_t offset = 12 + vendor_size;
    const auto count = Read32(data + offset);
    offset += 4;
    if (count > (size - offset) / 4) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (size - offset < 4) return false;
        const auto length = Read32(data + offset);
        offset += 4;
        if (length > size - offset) return false;
        offset += length;
    }
    return true;
}
}  // namespace

struct RecordingPacketReader::State {
    FILE* file = nullptr;
    ogg_sync_state sync{};
    ogg_stream_state stream{};
    bool sync_ready = false;
    bool stream_ready = false;
    bool eos_page = false;
    bool cancelled = false;
    unsigned headers = 0;
    uint32_t next_page = 0;
    uint64_t file_bytes = 0;
    uint64_t read_bytes = 0;
    uint64_t packets = 0;
    uint64_t parsed_bytes = 0;
    uint64_t owner = 0;
    uint64_t anchor_offset = 0;
    uint64_t anchor_packets = 0;
    uint64_t restore_skip = 0;
    uint32_t anchor_page = 0;
    size_t continued_bytes = 0;
    RecordingOpusInfo info;
    RecordingReadError error = RecordingReadError::kNone;
    RecordingEndKind end = RecordingEndKind::kNone;

    ~State() { Release(); }

    bool Release() {
        const bool ok = !file || std::fclose(file) == 0;
        file = nullptr;
        if (stream_ready) ogg_stream_clear(&stream);
        if (sync_ready) ogg_sync_clear(&sync);
        stream_ready = sync_ready = false;
        return ok;
    }

    RecordingReadStatus Fail(RecordingReadError reason) {
        error = reason;
        return RecordingReadStatus::kError;
    }

    bool AcceptPage(ogg_page& page) {
        if (ogg_page_version(&page) != 0 || (page.header[5] & ~7U) != 0 || eos_page ||
            static_cast<uint32_t>(ogg_page_pageno(&page)) != next_page ||
            (ogg_page_continued(&page) != 0) != (continued_bytes != 0)) {
            Fail(RecordingReadError::kContainer);
            return false;
        }
        if (!stream_ready) {
            if (!ogg_page_bos(&page) || ogg_page_packets(&page) != 1 ||
                ogg_page_continued(&page) || ogg_page_eos(&page) ||
                page.header[26] != 1 || page.header[27] != 19 || page.body_len != 19) {
                Fail(RecordingReadError::kHeaders);
                return false;
            }
            if (ogg_stream_init(&stream, ogg_page_serialno(&page)) != 0) {
                Fail(RecordingReadError::kMemory);
                return false;
            }
            stream_ready = true;
        } else if (ogg_page_serialno(&page) != stream.serialno || ogg_page_bos(&page)) {
            Fail(RecordingReadError::kContainer);
            return false;
        }
        // libogg verifies framing/CRC. Bound unfinished packets before pagein,
        // which would otherwise grow its reassembly buffer across arbitrary pages.
        for (size_t i = 0; i < page.header[26]; ++i) {
            const auto bytes = page.header[27 + i];
            continued_bytes += bytes;
            if (continued_bytes > kMaxPacketBytes) {
                Fail(RecordingReadError::kLimit);
                return false;
            }
            if (bytes < 255) continued_bytes = 0;
        }
        if (ogg_page_eos(&page) && continued_bytes != 0) {
            Fail(RecordingReadError::kContainer);
            return false;
        }
        if (ogg_stream_pagein(&stream, &page) != 0) {
            Fail(RecordingReadError::kContainer);
            return false;
        }
        ++next_page;
        eos_page = ogg_page_eos(&page) != 0;
        return true;
    }

    bool AcceptHeader(const ogg_packet& packet) {
        const auto* data = packet.packet;
        const auto size = static_cast<size_t>(packet.bytes);
        if (headers == 0) {
            if (size != 19 || std::memcmp(data, "OpusHead", 8) != 0) {
                Fail(RecordingReadError::kHeaders);
                return false;
            }
            if (data[8] != 1 || data[9] != 1 || data[18] != 0) {
                Fail(RecordingReadError::kUnsupported);
                return false;
            }
            info.channels = data[9];
            info.pre_skip_48k = Read16(data + 10);
            info.input_sample_rate = Read32(data + 12);
            const auto gain = Read16(data + 16);
            info.output_gain_q8 = static_cast<int16_t>(gain < 32768 ? gain : int32_t(gain) - 65536);
        } else {
            if (!ValidTags(data, size)) {
                Fail(RecordingReadError::kHeaders);
                return false;
            }
            constexpr char legacy_vendor[] = "Audream firmware";
            info.legacy_audream_granules = Read32(data + 8) == sizeof(legacy_vendor) - 1 &&
                std::memcmp(data + 12, legacy_vendor, sizeof(legacy_vendor) - 1) == 0;
            constexpr char watch_vendor[] = "Audream watch Ogg Opus v2";
            info.requires_eos = Read32(data + 8) == sizeof(watch_vendor) - 1 &&
                std::memcmp(data + 12, watch_vendor, sizeof(watch_vendor) - 1) == 0;
        }
        ++headers;
        info.valid = headers == 2;
        return true;
    }
};

RecordingPacketReader::RecordingPacketReader() : state_(new (std::nothrow) State) {}
RecordingPacketReader::~RecordingPacketReader() = default;

bool RecordingPacketReader::Open(const char* path, uint64_t expected_bytes) {
    if (!Close()) return false;
    state_.reset(new (std::nothrow) State);
    if (!state_) return false;
    auto& s = *state_;
    if (!path || !(s.file = std::fopen(path, "rb"))) {
        s.Fail(RecordingReadError::kIo);
        return false;
    }
    struct stat value{};
    if (fstat(fileno(s.file), &value) != 0 || !S_ISREG(value.st_mode) || value.st_size <= 0) {
        s.Fail(RecordingReadError::kIo);
        s.Release();
        return false;
    }
    s.file_bytes = static_cast<uint64_t>(value.st_size);
    if (expected_bytes != 0 && expected_bytes != s.file_bytes) {
        s.Fail(RecordingReadError::kChanged);
        s.Release();
        return false;
    }
    if (ogg_sync_init(&s.sync) != 0) {
        s.Fail(RecordingReadError::kMemory);
        s.Release();
        return false;
    }
    s.sync_ready = true;
    s.owner = next_reader_owner.fetch_add(1);
    if (!s.owner) s.owner = next_reader_owner.fetch_add(1);
    return true;
}

bool RecordingPacketReader::Seek(const RecordingReaderBookmark& bookmark) {
    if (!state_) return false;
    auto& s = *state_;
    if (!s.file || !s.info.valid || s.error != RecordingReadError::kNone || s.cancelled ||
        !bookmark.owner_ || bookmark.owner_ != s.owner || bookmark.offset_ >= s.file_bytes ||
        bookmark.offset_ > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) return false;
    struct stat value{};
    if (fstat(fileno(s.file), &value) != 0) { s.Fail(RecordingReadError::kIo); return false; }
    if (value.st_size < 0 || static_cast<uint64_t>(value.st_size) != s.file_bytes) {
        s.Fail(RecordingReadError::kChanged); return false;
    }
    if (fseeko(s.file, static_cast<off_t>(bookmark.offset_), SEEK_SET) != 0) {
        s.Fail(RecordingReadError::kIo); return false;
    }
    const int serial = s.stream.serialno;
    if (s.stream_ready) ogg_stream_clear(&s.stream);
    if (s.sync_ready) ogg_sync_clear(&s.sync);
    s.stream_ready = s.sync_ready = false;
    if (ogg_sync_init(&s.sync) != 0) { s.Fail(RecordingReadError::kMemory); return false; }
    s.sync_ready = true;
    if (bookmark.offset_) {
        if (ogg_stream_init(&s.stream, serial) != 0) { s.Fail(RecordingReadError::kMemory); return false; }
        s.stream_ready = true;
        if (ogg_stream_reset(&s.stream) != 0) { s.Fail(RecordingReadError::kMemory); return false; }
    }
    s.headers = bookmark.offset_ ? 2 : 0;
    s.next_page = s.anchor_page = bookmark.page_;
    s.read_bytes = s.parsed_bytes = s.anchor_offset = bookmark.offset_;
    s.packets = s.anchor_packets = 0;
    s.restore_skip = bookmark.skip_;
    s.continued_bytes = 0;
    s.eos_page = false;
    s.end = RecordingEndKind::kNone;
    return true;
}

RecordingReadStatus RecordingPacketReader::Next(
    RecordingOpusPacket& packet, const std::function<bool()>& cancelled) {
    packet.bytes.clear();
    packet.granule_position = -1;
    packet.eos = false;
    packet.bookmark = {};
    if (!state_) return RecordingReadStatus::kError;
    auto& s = *state_;
    if (s.error != RecordingReadError::kNone) return RecordingReadStatus::kError;
    if (s.cancelled) return RecordingReadStatus::kCancelled;
    if (!s.file) return s.Fail(RecordingReadError::kNotOpen);
    if (s.end != RecordingEndKind::kNone) return RecordingReadStatus::kEnd;
    size_t reads = 0;
    size_t pages = 0;
    size_t skipped = 0;
    while (true) {
        if (cancelled && cancelled()) {
            s.cancelled = true;
            return RecordingReadStatus::kCancelled;
        }
        ogg_packet decoded{};
        const int available = s.stream_ready ? ogg_stream_packetout(&s.stream, &decoded) : 0;
        if (available < 0) return s.Fail(RecordingReadError::kContainer);
        if (available > 0) {
            if (decoded.bytes <= 0 || decoded.bytes > static_cast<long>(kMaxPacketBytes)) {
                return s.Fail(RecordingReadError::kLimit);
            }
            if (s.headers < 2) {
                if (!s.AcceptHeader(decoded)) return RecordingReadStatus::kError;
                continue;
            }
            const auto ordinal = s.anchor_packets++;
            ++s.packets;
            if (s.restore_skip) {
                --s.restore_skip;
                if (++skipped >= 64) return RecordingReadStatus::kPending;
                continue;
            }
            packet.bytes.assign(decoded.packet, decoded.packet + decoded.bytes);
            packet.granule_position = decoded.granulepos;
            packet.eos = decoded.e_o_s != 0;
            packet.bookmark.owner_ = s.owner;
            packet.bookmark.offset_ = s.anchor_offset;
            packet.bookmark.page_ = s.anchor_page;
            packet.bookmark.skip_ = ordinal;
            return RecordingReadStatus::kPacket;
        }
        if (pages >= kMaxPagesPerCall) return RecordingReadStatus::kPending;
        ogg_page page{};
        const long page_bytes = ogg_sync_pageseek(&s.sync, &page);
        if (page_bytes < 0) return s.Fail(RecordingReadError::kContainer);
        if (page_bytes > 0) {
            ++pages;
            if (s.headers == 2 && !ogg_page_continued(&page)) {
                s.anchor_offset = s.parsed_bytes;
                s.anchor_page = static_cast<uint32_t>(ogg_page_pageno(&page));
                s.anchor_packets = 0;
            }
            if (!s.AcceptPage(page)) return RecordingReadStatus::kError;
            s.parsed_bytes += static_cast<uint64_t>(page_bytes);
            continue;
        }
        if (s.read_bytes == s.file_bytes) {
            struct stat value{};
            if (fstat(fileno(s.file), &value) != 0) return s.Fail(RecordingReadError::kIo);
            if (value.st_size < 0 || static_cast<uint64_t>(value.st_size) != s.file_bytes) {
                return s.Fail(RecordingReadError::kChanged);
            }
            if (s.sync.fill != s.sync.returned || s.continued_bytes != 0 ||
                !s.info.valid || s.packets == 0 || s.restore_skip ||
                (s.info.requires_eos && !s.eos_page)) return s.Fail(RecordingReadError::kContainer);
            // Older Audream files omit EOS. Preserve that distinction for the UI;
            // page-boundary EOF cannot prove no complete pages were lost earlier.
            s.end = s.eos_page ? RecordingEndKind::kEos : RecordingEndKind::kLegacyPageBoundary;
            return RecordingReadStatus::kEnd;
        }
        if (reads >= kMaxReadsPerCall) return RecordingReadStatus::kPending;
        char* buffer = ogg_sync_buffer(&s.sync, kReadChunkBytes);
        if (!buffer) return s.Fail(RecordingReadError::kMemory);
        const auto remaining = s.file_bytes - s.read_bytes;
        const auto request = remaining < kReadChunkBytes ? static_cast<size_t>(remaining) : kReadChunkBytes;
        const auto count = std::fread(buffer, 1, request, s.file);
        ++reads;
        s.read_bytes += count;
        if (std::ferror(s.file)) return s.Fail(RecordingReadError::kIo);
        if (count != request) return s.Fail(RecordingReadError::kChanged);
        if (ogg_sync_wrote(&s.sync, static_cast<long>(count)) != 0) {
            return s.Fail(RecordingReadError::kContainer);
        }
    }
}

bool RecordingPacketReader::Close() {
    if (!state_) return true;
    const bool ok = state_->Release();
    if (!ok) state_->error = RecordingReadError::kIo;
    return ok;
}
RecordingOpusInfo RecordingPacketReader::Info() const { return state_ ? state_->info : RecordingOpusInfo{}; }
RecordingReadError RecordingPacketReader::Error() const {
    return state_ ? state_->error : RecordingReadError::kMemory;
}
RecordingEndKind RecordingPacketReader::EndKind() const { return state_ ? state_->end : RecordingEndKind::kNone; }
uint64_t RecordingPacketReader::BytesRead() const { return state_ ? state_->read_bytes : 0; }
size_t RecordingPacketReader::BufferCapacityBytes() const {
    if (!state_) return 0;
    const auto& s = *state_;
    return static_cast<size_t>(s.sync.storage) + static_cast<size_t>(s.stream.body_storage) +
        static_cast<size_t>(s.stream.lacing_storage) * (sizeof(int) + sizeof(ogg_int64_t));
}
