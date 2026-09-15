#include "watch_opus_writer.h"

#include <new>
#include <utility>
#include <vector>
#include "ogg/ogg.h"
#include "esp_random.h"

// Public libopus packet APIs exported by the managed ESP codec archive.
extern "C" int opus_packet_parse(const unsigned char*, int32_t, unsigned char*, const unsigned char* [48], int16_t [48], int*);
extern "C" int opus_packet_get_nb_samples(const unsigned char*, int32_t, int32_t);

namespace {
WatchOpusFileIo default_io;
constexpr size_t kMaxPacketBytes = 8192;
void Append32(std::vector<uint8_t>& data, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) data.push_back(static_cast<uint8_t>(value >> shift));
}
}

struct WatchOpusWriter::State {
    FILE* file = nullptr;
    ogg_stream_state stream{};
    bool initialized = false;
    std::vector<uint8_t> pending;
    ogg_int64_t ordinal = 0;
    ~State() { if (initialized) ogg_stream_clear(&stream); }
};

WatchOpusWriter::WatchOpusWriter(WatchOpusFileIo* io) : io_(io ? io : &default_io) {}
WatchOpusWriter::~WatchOpusWriter() { Close(); }

bool WatchOpusWriter::Open(const char* path, uint32_t sample_rate, uint8_t channels, uint32_t frame_samples) {
    if (state_ || !path || !*path || channels != 1 || !frame_samples ||
        (sample_rate != 8000 && sample_rate != 12000 && sample_rate != 16000 &&
         sample_rate != 24000 && sample_rate != 48000)) return false;
    state_.reset(new (std::nothrow) State);
    if (!state_) return false;
    failed_ = false; granule_ = bytes_ = 0; packets_ = 0;
    if (ogg_stream_init(&state_->stream, static_cast<int>(esp_random())) != 0) {
        state_.reset(); failed_ = true; return false;
    }
    state_->initialized = true;
    state_->file = io_->Open(path);
    if (!state_->file) { state_.reset(); failed_ = true; return false; }
    // Preserve all encoded samples. Encoder delay/flush is a separate capture
    // contract; do not invent pre-skip and silently remove recorded speech.
    std::vector<uint8_t> head{'O','p','u','s','H','e','a','d',1,channels,0,0};
    Append32(head, sample_rate);
    head.insert(head.end(), {0,0,0});
    constexpr char vendor[] = "Audream watch Ogg Opus v2";
    std::vector<uint8_t> tags{'O','p','u','s','T','a','g','s'};
    Append32(tags, sizeof(vendor) - 1);
    tags.insert(tags.end(), vendor, vendor + sizeof(vendor) - 1);
    Append32(tags, 0);
    if (!Submit(head.data(), head.size(), 0, true, false) || !Submit(tags.data(), tags.size(), 0, false, false)) {
        Close(); return false;
    }
    return true;
}

bool WatchOpusWriter::Submit(const uint8_t* data, size_t size, uint64_t granule, bool first, bool last) {
    if (!state_ || !state_->file || failed_) return false;
    ogg_packet packet{};
    packet.packet = const_cast<unsigned char*>(data);
    packet.bytes = static_cast<long>(size);
    packet.b_o_s = first; packet.e_o_s = last;
    packet.granulepos = static_cast<ogg_int64_t>(granule);
    packet.packetno = state_->ordinal++;
    if (ogg_stream_packetin(&state_->stream, &packet) != 0) { failed_ = true; return false; }
    ogg_page page{};
    // Force bounded per-packet writes. Header packets must each end their page.
    while (ogg_stream_flush(&state_->stream, &page)) {
        for (const auto part : {std::make_pair(page.header, page.header_len), std::make_pair(page.body, page.body_len)}) {
            const auto count = io_->Write(state_->file, part.first, static_cast<size_t>(part.second));
            bytes_ += count;
            if (count != static_cast<size_t>(part.second)) { failed_ = true; return false; }
        }
    }
    return true;
}

bool WatchOpusWriter::WritePacket(const uint8_t* data, size_t size) {
    if (!state_ || failed_) return false;
    if (!data || !size || size > kMaxPacketBytes || packets_ == UINT32_MAX) { failed_ = true; return false; }
    unsigned char toc = 0;
    const unsigned char* frames[48]{};
    int16_t sizes[48]{};
    int offset = 0;
    const int count = opus_packet_get_nb_samples(data, static_cast<int32_t>(size), 48000);
    if (opus_packet_parse(data, static_cast<int32_t>(size), &toc, frames, sizes, &offset) <= 0 ||
        count <= 0 || count > 5760 || granule_ > static_cast<uint64_t>(INT64_MAX) - count) {
        failed_ = true; return false;
    }
    if (!state_->pending.empty() && !Submit(state_->pending.data(), state_->pending.size(), granule_, false, false)) return false;
    state_->pending.assign(data, data + size);
    granule_ += static_cast<uint64_t>(count);
    ++packets_;
    return true;
}

bool WatchOpusWriter::Close() {
    if (!state_) return !failed_;
    if (!failed_ && !state_->pending.empty())
        Submit(state_->pending.data(), state_->pending.size(), granule_, false, true);
    if (!packets_) failed_ = true;
    if (state_->file) {
        if (io_->Flush(state_->file) != 0) failed_ = true;
        if (io_->Close(state_->file) != 0) failed_ = true;
        state_->file = nullptr;
    }
    state_.reset();
    return !failed_;
}
