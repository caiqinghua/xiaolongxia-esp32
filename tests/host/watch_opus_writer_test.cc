#include "audio/watch_opus_writer.h"
#include "audio/recording_playback_source.h"
#include "audio/recording_playback_decoder.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>
#include <opus.h>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
std::vector<uint8_t> Encode(OpusEncoder* encoder, int samples) {
    std::vector<int16_t> pcm(samples);
    for (int i = 0; i < samples; ++i) pcm[i] = static_cast<int16_t>(5000 * std::sin(i * 0.13));
    std::vector<uint8_t> packet(8192);
    const int size = opus_encode(encoder, pcm.data(), samples, packet.data(), packet.size());
    Check(size > 0, "reference encoder creates valid packet");
    packet.resize(size); return packet;
}

void TestRoundTrip(const std::filesystem::path& root) {
    for (const auto rate : {8000, 12000, 16000, 24000, 48000}) {
        int error = 0;
        OpusEncoder* encoder = opus_encoder_create(rate, 1, OPUS_APPLICATION_AUDIO, &error);
        Check(encoder && error == OPUS_OK, "reference encoder opens");
        const auto path = (root / (std::to_string(rate) + ".opus")).string();
        WatchOpusWriter writer;
        Check(writer.Open(path.c_str(), rate, 1, rate * 60 / 1000), "standard writer opens");
        Check(!writer.Open(path.c_str(), rate, 1, 1), "opening cannot overwrite an active recording");
        std::vector<std::vector<uint8_t>> packets;
        uint64_t samples = 0;
        for (const int milliseconds : {20, 40, 60, 10, 20}) {
            packets.push_back(Encode(encoder, rate * milliseconds / 1000));
            Check(writer.WritePacket(packets.back().data(), packets.back().size()), "writer accepts actual variable duration packet");
            samples += rate * milliseconds / 1000;
        }
        opus_encoder_destroy(encoder);
        Check(writer.GranulePosition() == samples * (48000 / rate) && writer.PacketCount() == packets.size(),
            "granules use actual packet duration at 48 kHz, not nominal frame size");
        const auto before_close = writer.BytesWritten();
        Check(writer.Close() && writer.Close(), "successful finalization is idempotent");
        Check(writer.BytesWritten() == std::filesystem::file_size(path), "size includes headers and final EOS page");
        RecordingPlaybackSource source;
        Check(source.Open(path.c_str(), writer.BytesWritten(), rate), "production source accepts standard writer output");
        Check(!source.Info().legacy_audream_granules && source.TotalSamples() == samples, "new vendor uses standard timeline");
        RecordingPlaybackDecoder decoder;
        Check(decoder.Open(source.Info(), rate), "production decoder opens standard recording");
        size_t ordinal = 0, output = 0;
        RecordingOpusPacket packet;
        std::vector<int16_t> pcm;
        while (true) {
            const auto status = source.Next(packet);
            if (status == RecordingReadStatus::kPending) continue;
            if (status == RecordingReadStatus::kEnd) break;
            Check(status == RecordingReadStatus::kPacket && ordinal < packets.size(), "packet read succeeds");
            Check(packet.bytes == packets[ordinal] && packet.eos == (ordinal + 1 == packets.size()),
                "encoded packets preserved and only final audio packet has EOS");
            Check(decoder.Decode(packet, pcm), "standard packets decode");
            output += pcm.size(); ++ordinal;
        }
        Check(ordinal == packets.size() && output == samples && source.EndKind() == RecordingEndKind::kEos,
            "normal close yields verified EOS and complete encoded duration");
        Check(source.Close() && decoder.Close(), "round trip closes resources");
        const auto partial = path + ".partial";
        std::filesystem::copy_file(path, partial, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::resize_file(partial, before_close);
        Check(!source.Open(partial.c_str(), before_close, rate) && source.Error() == RecordingReadError::kContainer,
            "new-format file missing its final page is not mistaken for legacy complete-page EOF");
        Check(source.Close(), "truncated file closes");
    }
}

struct FaultIo : WatchOpusFileIo {
    bool fail_open = false, fail_flush = false, fail_close = false;
    int fail_write = -1, writes = 0, closes = 0;
    FILE* Open(const char* path) override { return fail_open ? nullptr : WatchOpusFileIo::Open(path); }
    size_t Write(FILE* file, const uint8_t* data, size_t bytes) override {
        if (writes++ == fail_write) return bytes ? WatchOpusFileIo::Write(file, data, bytes - 1) : 0;
        return WatchOpusFileIo::Write(file, data, bytes);
    }
    int Flush(FILE* file) override { const int result = WatchOpusFileIo::Flush(file); return fail_flush ? EOF : result; }
    int Close(FILE* file) override { ++closes; const int result = WatchOpusFileIo::Close(file); return fail_close ? EOF : result; }
};

void TestFailures(const std::filesystem::path& root) {
    const auto path = (root / "fault.opus").string();
    const uint8_t silence[] = {0xf8, 0xff, 0xfe};
    for (int failure = 0; failure < 6; ++failure) {
        FaultIo io;
        WatchOpusWriter writer(&io);
        if (failure == 0) io.fail_open = true;
        if (failure == 1) io.fail_write = 0;
        const bool opened = writer.Open(path.c_str(), 16000, 1, 960);
        if (failure < 2) {
            Check(!opened && !writer.Close(), "open/header failures remain failures on close");
        } else {
            Check(opened && writer.WritePacket(silence, sizeof(silence)), "fault fixture accepts pending audio");
            if (failure == 2) io.fail_write = io.writes;
            if (failure == 3) io.fail_flush = true;
            if (failure == 4) io.fail_close = true;
            if (failure == 5) {
                io.fail_write = io.writes;
                Check(!writer.WritePacket(silence, sizeof(silence)), "mid-recording short write fails");
                io.fail_write = -1;
                Check(!writer.WritePacket(silence, sizeof(silence)), "failed stream cannot append after a gap");
            }
            Check(!writer.Close() && !writer.Close(), "final packet, flush and close errors cannot become success on retry");
        }
        Check(io.closes == (failure == 0 ? 0 : 1), "descriptor closes exactly once even on failure");
        io.fail_open = io.fail_flush = io.fail_close = false; io.fail_write = -1;
        Check(writer.Open(path.c_str(), 16000, 1, 960) && writer.WritePacket(silence, sizeof(silence)) && writer.Close(),
            "a new explicit recording resets prior error and counters");
        Check(writer.PacketCount() == 1 && writer.GranulePosition() == 960, "single packet recording has correct granule");
        RecordingPlaybackSource source;
        Check(source.Open(path.c_str(), writer.BytesWritten(), 16000) && source.TotalSamples() == 320 && source.Close(),
            "single packet is retained and finalized as an EOS audio page");
    }
    WatchOpusWriter writer;
    Check(!writer.Open(path.c_str(), 44100, 1, 960) && !writer.Open(path.c_str(), 16000, 3, 960), "invalid format rejected before opening");
    Check(writer.Open(path.c_str(), 16000, 1, 960) && !writer.Close(), "header-only recording is not finalized audio");
    Check(writer.Open(path.c_str(), 16000, 1, 960), "malformed packet test opens");
    const uint8_t invalid[] = {0xff};
    Check(!writer.WritePacket(invalid, sizeof(invalid)) && !writer.Close(), "invalid packet never fabricates a duration or EOS");
}
}

int main(int argc, char** argv) {
    Check(argc == 2, "test directory required");
    std::filesystem::create_directories(argv[1]);
    TestRoundTrip(argv[1]); TestFailures(argv[1]);
    std::cout << "PASS: standard watch Opus writer round trip, timing, EOS and I/O failure tests\n";
}
