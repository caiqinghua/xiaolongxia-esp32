#include "audio/recording_packet_reader.h"
#include "audio/ogg_opus_writer.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <ogg/ogg.h>

namespace {
using Bytes = std::vector<uint8_t>;
std::filesystem::path root;

void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

void Put32(Bytes& bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

Bytes Page(uint32_t sequence, uint8_t flags, Bytes lacing, Bytes body, uint32_t serial = 73) {
    Bytes bytes(27 + lacing.size());
    std::copy_n("OggS", 4, bytes.begin());
    bytes[5] = flags;
    Put32(bytes, 14, serial);
    Put32(bytes, 18, sequence);
    bytes[26] = static_cast<uint8_t>(lacing.size());
    std::copy(lacing.begin(), lacing.end(), bytes.begin() + 27);
    bytes.insert(bytes.end(), body.begin(), body.end());
    ogg_page page{bytes.data(), static_cast<long>(27 + lacing.size()),
                  bytes.data() + 27 + lacing.size(), static_cast<long>(body.size())};
    ogg_page_checksum_set(&page);
    return bytes;
}

Bytes PacketPage(uint32_t sequence, uint8_t flags, const Bytes& packet, uint32_t serial = 73) {
    Bytes lacing(packet.size() / 255, 255);
    lacing.push_back(static_cast<uint8_t>(packet.size() % 255));
    return Page(sequence, flags, lacing, packet, serial);
}

Bytes Head() {
    Bytes bytes{'O','p','u','s','H','e','a','d',1,1,0,0,0,0,0,0,0,0,0};
    Put32(bytes, 12, 16000);
    return bytes;
}
Bytes Tags() { return {'O','p','u','s','T','a','g','s',0,0,0,0,0,0,0,0}; }
void Append(Bytes& target, const Bytes& bytes) { target.insert(target.end(), bytes.begin(), bytes.end()); }
Bytes Headers() {
    auto bytes = PacketPage(0, 2, Head());
    Append(bytes, PacketPage(1, 0, Tags()));
    return bytes;
}
std::string Save(const Bytes& bytes) {
    const auto path = (root / "input.opus").string();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    Check(!out.fail(), "fixture write");
    return path;
}

struct Collected {
    RecordingReadStatus status;
    std::vector<Bytes> packets;
};
Collected Drain(RecordingPacketReader& reader) {
    Collected result{RecordingReadStatus::kPending, {}};
    RecordingOpusPacket packet;
    for (size_t i = 0; i < 100000; ++i) {
        result.status = reader.Next(packet);
        Check(reader.BufferCapacityBytes() < 200 * 1024, "bounded libogg buffers");
        if (result.status == RecordingReadStatus::kPacket) result.packets.push_back(packet.bytes);
        else if (result.status != RecordingReadStatus::kPending) return result;
    }
    Check(false, "reader terminates");
    return result;
}

void TestProductionWriterRoundTrip() {
    const auto path = (root / "production.opus").string();
    OggOpusWriter writer;
    const Bytes payload{0xf8, 0xff, 0xfe};
    Check(writer.Open(path.c_str(), 16000, 1, 320), "real writer open");
    for (unsigned i = 0; i < 5000; ++i) Check(writer.WritePacket(payload.data(), payload.size()), "real writer packet");
    Check(writer.Close(), "real writer close");
    RecordingPacketReader reader;
    Check(reader.Open(path.c_str(), std::filesystem::file_size(path)), "read real recording");
    const auto result = Drain(reader);
    Check(result.status == RecordingReadStatus::kEnd && result.packets.size() == 5000, "all real packets retained");
    Check(std::all_of(result.packets.begin(), result.packets.end(), [&](const Bytes& b) { return b == payload; }),
          "packet contents unchanged");
    Check(reader.Info().valid && reader.Info().input_sample_rate == 16000 && reader.Info().channels == 1,
          "recording header info");
    Check(reader.EndKind() == RecordingEndKind::kLegacyPageBoundary, "legacy EOF is explicit");
    Check(reader.Close(), "close recording");
}

void TestMultipleAndContinuedPackets() {
    auto bytes = Headers();
    Append(bytes, Page(2, 0, {3, 2}, {1,2,3,4,5}));
    Bytes payload(8192);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i);
    Append(bytes, Page(3, 0, Bytes(16, 255), Bytes(payload.begin(), payload.begin() + 4080)));
    Bytes lacing(16, 255);
    lacing.push_back(32);
    Append(bytes, Page(4, 5, lacing, Bytes(payload.begin() + 4080, payload.end())));
    RecordingPacketReader reader;
    Check(reader.Open(Save(bytes).c_str()), "continued open");
    const auto result = Drain(reader);
    Check(result.status == RecordingReadStatus::kEnd && result.packets == std::vector<Bytes>{{1,2,3},{4,5},payload},
          "cross-page packet and multiple packets preserved");
    Check(reader.EndKind() == RecordingEndKind::kEos, "EOS retained");
}

void TestBookmarks() {
    auto bytes = Headers();
    Bytes lacing(200, 1), body(200);
    for (size_t i = 0; i < body.size(); ++i) body[i] = static_cast<uint8_t>(i + 1);
    Append(bytes, Page(2, 0, lacing, body));
    Append(bytes, Page(3, 0, {255}, Bytes(255, 3)));
    Append(bytes, Page(4, 5, {5, 1}, Bytes(6, 4)));
    const auto path = Save(bytes);
    RecordingPacketReader reader;
    Check(reader.Open(path.c_str()), "bookmark reader opens");
    std::vector<RecordingOpusPacket> expected;
    RecordingOpusPacket packet;
    while (true) {
        const auto status = reader.Next(packet);
        if (status == RecordingReadStatus::kPending) continue;
        if (status == RecordingReadStatus::kEnd) break;
        Check(status == RecordingReadStatus::kPacket && packet.bookmark.Valid(), "each audio packet has an issued bookmark");
        expected.push_back(packet);
    }
    Check(expected.size() == 202, "multi-packet and continued bookmark fixture parsed");
    for (const size_t index : {201U, 170U, 0U, 200U, 100U}) {
        Check(reader.Seek(expected[index].bookmark), "reader restores issued bookmark after EOF");
        size_t pending = 0;
        auto status = reader.Next(packet);
        while (status == RecordingReadStatus::kPending) { ++pending; status = reader.Next(packet); }
        Check(status == RecordingReadStatus::kPacket && packet.bytes == expected[index].bytes &&
            packet.eos == expected[index].eos && packet.granule_position == expected[index].granule_position,
            "seek retains exact packet, continued assembly and end flags");
        if (index == 170) Check(pending >= 2, "discarding prior page packets yields in bounded groups");
        Check(Drain(reader).status == RecordingReadStatus::kEnd, "remainder validates after bookmark seek");
    }
    RecordingPacketReader other;
    Check(other.Open(path.c_str()) && !other.Seek(expected[0].bookmark), "other reader cannot reuse bookmarks");
    Check(reader.Open(path.c_str()) && Drain(reader).status == RecordingReadStatus::kEnd &&
        !reader.Seek(expected[0].bookmark), "reopen invalidates old bookmarks even at the same path");
    Check(!reader.Seek({}), "unissued bookmark rejected");
    Check(other.Close(), "other reader closes");
}

void TestCorruptionIsNotResynchronized() {
    const auto headers = Headers();
    const auto audio = PacketPage(2, 4, {0xf8,0xff,0xfe});
    for (size_t offset = 0; offset < audio.size(); ++offset) {
        auto damaged = audio;
        damaged[offset] ^= 1;
        auto bytes = headers;
        Append(bytes, damaged);
        RecordingPacketReader reader;
        Check(reader.Open(Save(bytes).c_str()), "corrupt open");
        const auto result = Drain(reader);
        Check(result.status == RecordingReadStatus::kError && result.packets.empty(), "corrupt page never emitted");
    }
    for (size_t length = 1; length < audio.size(); ++length) {
        auto bytes = headers;
        bytes.insert(bytes.end(), audio.begin(), audio.begin() + length);
        RecordingPacketReader reader;
        Check(reader.Open(Save(bytes).c_str()), "truncated open");
        Check(Drain(reader).status == RecordingReadStatus::kError, "partial page not clean EOF");
    }
}

void TestSequenceAndStreamIdentity() {
    const std::vector<Bytes> invalid{
        PacketPage(3, 4, {1}), PacketPage(2, 4, {1}, 74), PacketPage(2, 6, {1}),
        PacketPage(2, 5, {1}), PacketPage(2, 8, {1}),
    };
    for (const auto& page : invalid) {
        auto bytes = Headers(); Append(bytes, page);
        RecordingPacketReader reader;
        Check(reader.Open(Save(bytes).c_str()), "invalid stream open");
        Check(Drain(reader).status == RecordingReadStatus::kError, "invalid stream rejected");
    }
    auto bytes = Headers(); Append(bytes, PacketPage(2, 4, {1})); Append(bytes, Headers());
    RecordingPacketReader reader;
    Check(reader.Open(Save(bytes).c_str()), "chained open");
    Check(Drain(reader).status == RecordingReadStatus::kError, "chaining not silently accepted");
}

void TestPacketBoundsBeforeReassembly() {
    auto bytes = Headers();
    Append(bytes, Page(2, 0, Bytes(32, 255), Bytes(8160, 7)));
    Append(bytes, Page(3, 1, {255}, Bytes(255, 8)));
    RecordingPacketReader reader;
    Check(reader.Open(Save(bytes).c_str()), "large packet open");
    const auto result = Drain(reader);
    Check(result.status == RecordingReadStatus::kError && result.packets.empty() &&
          reader.Error() == RecordingReadError::kLimit, "continued oversized packet rejected");
    bytes = Headers();
    Append(bytes, Page(2, 4, Bytes(255, 255), Bytes(65025, 9)));
    Check(reader.Open(Save(bytes).c_str()), "maximum page open");
    Check(Drain(reader).status == RecordingReadStatus::kError && reader.Error() == RecordingReadError::kLimit,
          "maximum Ogg page cannot create unbounded packet");
}

void TestHeadersAndInfo() {
    for (unsigned variant = 0; variant < 7; ++variant) {
        auto head = Head(); auto tags = Tags();
        switch (variant) {
            case 0: head[9] = 2; break;
            case 1: head[18] = 1; break;
            case 2: head[8] = 16; break;
            case 3: head.resize(18); break;
            case 4: tags.resize(8); break;
            case 5: Put32(tags, 8, 0xffffffff); break;
            case 6: Put32(tags, 12, 0xffffffff); break;
        }
        auto bytes = PacketPage(0, 2, head); Append(bytes, PacketPage(1, 0, tags));
        Append(bytes, PacketPage(2, 4, {1}));
        RecordingPacketReader reader;
        Check(reader.Open(Save(bytes).c_str()), "bad header open");
        Check(Drain(reader).status == RecordingReadStatus::kError && !reader.Info().valid, "bad header fails");
    }
    auto head = Head(); head[10] = 0x38; head[11] = 1; head[16] = 0; head[17] = 0xff;
    Put32(head, 12, 0);
    auto bytes = PacketPage(0, 2, head); Append(bytes, PacketPage(1, 0, Tags())); Append(bytes, PacketPage(2, 4, {1}));
    RecordingPacketReader reader;
    Check(reader.Open(Save(bytes).c_str()), "info open");
    Check(Drain(reader).status == RecordingReadStatus::kEnd, "unknown input rate is valid Opus metadata");
    const auto info = reader.Info();
    Check(info.pre_skip_48k == 312 && info.output_gain_q8 == -256 && info.input_sample_rate == 0,
          "pre-skip and signed gain preserved without applying");
}

void TestCancellationAndWorkBudget() {
    auto bytes = Headers();
    for (uint32_t i = 2; i < 80; ++i) Append(bytes, Page(i, 0, {}, {}));
    Append(bytes, PacketPage(80, 4, {1}));
    const auto path = Save(bytes);
    RecordingPacketReader reader;
    RecordingOpusPacket packet;
    Check(reader.Open(path.c_str()), "cancel open");
    Check(reader.Next(packet, [] { return true; }) == RecordingReadStatus::kCancelled && reader.BytesRead() == 0,
          "cancel before IO");
    Check(reader.Next(packet) == RecordingReadStatus::kCancelled, "cancel is sticky");
    Check(reader.Open(path.c_str()), "reopen after cancellation");
    Check(reader.Next(packet) == RecordingReadStatus::kPending, "empty pages yield to worker");
    Check(reader.BytesRead() <= RecordingPacketReader::kMaxReadsPerCall * RecordingPacketReader::kReadChunkBytes,
          "per-call reads bounded");
    Check(reader.Next(packet, [] { return true; }) == RecordingReadStatus::kCancelled && packet.bytes.empty(),
          "cancel while parsing has no late packet");
    Check(reader.Open(path.c_str()), "open reset");
    Check(Drain(reader).status == RecordingReadStatus::kEnd, "pending can resume");
}

void TestSizeAndFileErrors() {
    auto bytes = Headers();
    for (uint32_t i = 2; i < 100; ++i) Append(bytes, PacketPage(i, 0, Bytes(1024, 1)));
    auto path = Save(bytes);
    RecordingPacketReader reader;
    Check(!reader.Open(path.c_str(), bytes.size() + 1) && reader.Error() == RecordingReadError::kChanged,
          "catalog size mismatch");
    Check(!reader.Open((root / "missing").c_str()), "missing file fails");
    Check(!reader.Open(root.c_str()), "directory fails");
    Check(reader.Open(path.c_str()), "mutation open");
    RecordingOpusPacket packet;
    Check(reader.Next(packet) == RecordingReadStatus::kPacket, "first packet before mutation");
    std::filesystem::resize_file(path, 100);
    Check(Drain(reader).status == RecordingReadStatus::kError && reader.Error() == RecordingReadError::kChanged,
          "mid-read truncation fails");
    path = Save(bytes);
    Check(reader.Open(path.c_str()), "append open");
    { std::ofstream out(path, std::ios::binary | std::ios::app); out.put('x'); }
    Check(Drain(reader).status == RecordingReadStatus::kError && reader.Error() == RecordingReadError::kChanged,
          "append cannot masquerade as fixed file");
    Check(reader.Close(), "explicit close");
}

void TestMaximumValidPageAndLargeFile() {
    auto bytes = Headers();
    const auto page = Page(2, 4, Bytes(255, 254), Bytes(255 * 254, 0xf8));
    Append(bytes, page);
    RecordingPacketReader reader;
    Check(reader.Open(Save(bytes).c_str()), "large valid page open");
    RecordingOpusPacket packet;
    Check(reader.Next(packet) == RecordingReadStatus::kPending, "large page yields before enough bytes read");
    Check(reader.BytesRead() == RecordingPacketReader::kMaxReadsPerCall * RecordingPacketReader::kReadChunkBytes,
          "large page obeys read budget");
    const auto result = Drain(reader);
    Check(result.status == RecordingReadStatus::kEnd && result.packets.size() == 255, "all packets in large page");
    Check(std::all_of(result.packets.begin(), result.packets.end(), [](const Bytes& b) { return b == Bytes(254, 0xf8); }),
          "large page data retained");
    const auto path = (root / "large.opus").string();
    OggOpusWriter writer;
    const Bytes payload(300, 0xf8);
    Check(writer.Open(path.c_str(), 16000, 1, 960), "large recording writer");
    for (unsigned i = 0; i < 10000; ++i) Check(writer.WritePacket(payload.data(), payload.size()), "large recording write");
    Check(writer.Close() && reader.Open(path.c_str()), "large recording read");
    size_t packets = 0;
    size_t maximum = 0;
    while (true) {
        const auto status = reader.Next(packet);
        maximum = std::max(maximum, reader.BufferCapacityBytes());
        if (status == RecordingReadStatus::kPending) continue;
        if (status == RecordingReadStatus::kEnd) break;
        Check(status == RecordingReadStatus::kPacket && packet.bytes == payload, "long stream exact packet");
        ++packets;
    }
    Check(packets == 10000 && reader.BytesRead() > 3 * 1024 * 1024 && maximum < 64 * 1024,
          "normal recording memory stays bounded independent of file length");
}

void TestStandardFixture(const char* path) {
    RecordingPacketReader reader;
    Check(reader.Open(path), "standard encoder fixture opens");
    const auto result = Drain(reader);
    Check(result.status == RecordingReadStatus::kEnd && result.packets.size() == 34 &&
          reader.EndKind() == RecordingEndKind::kEos && reader.Info().pre_skip_48k == 312,
          "two-second 60 ms mono standard Opus fixture parsed");
    std::cout << "Standard encoder fixture: packets=" << result.packets.size()
              << " pre_skip_48k=" << reader.Info().pre_skip_48k << " EOS=1\n";
}
}  // namespace

int main(int argc, char** argv) {
    Check(argc == 2, "test directory supplied"); root = argv[1];
    TestProductionWriterRoundTrip();
    TestMultipleAndContinuedPackets();
    TestBookmarks();
    TestCorruptionIsNotResynchronized();
    TestSequenceAndStreamIdentity();
    TestPacketBoundsBeforeReassembly();
    TestHeadersAndInfo();
    TestCancellationAndWorkBudget();
    TestSizeAndFileErrors();
    TestMaximumValidPageAndLargeFile();
    if (const auto* fixture = std::getenv("AUDREAM_OPUS_FIXTURE")) TestStandardFixture(fixture);
    std::cout << "10 recording packet reader test groups passed\n";
}
