#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

class WatchOpusFileIo {
public:
    virtual ~WatchOpusFileIo() = default;
    virtual FILE* Open(const char* path) { return std::fopen(path, "wb"); }
    virtual size_t Write(FILE* file, const uint8_t* data, size_t bytes) { return std::fwrite(data, 1, bytes, file); }
    virtual int Flush(FILE* file) { return std::fflush(file); }
    virtual int Close(FILE* file) { return std::fclose(file); }
};

// The caller serializes access and retains storage ownership through Close.
// One encoded packet is retained so normal close can mark the last packet EOS.
class WatchOpusWriter {
public:
    explicit WatchOpusWriter(WatchOpusFileIo* io = nullptr);
    ~WatchOpusWriter();
    WatchOpusWriter(const WatchOpusWriter&) = delete;
    WatchOpusWriter& operator=(const WatchOpusWriter&) = delete;
    bool Open(const char* path, uint32_t sample_rate, uint8_t channels, uint32_t frame_samples);
    bool WritePacket(const uint8_t* data, size_t size);
    bool Close();
    uint64_t GranulePosition() const { return granule_; }
    uint32_t PacketCount() const { return packets_; }
    uint64_t BytesWritten() const { return bytes_; }

private:
    struct State;
    std::unique_ptr<State> state_;
    WatchOpusFileIo* io_;
    uint64_t granule_ = 0;
    uint64_t bytes_ = 0;
    uint32_t packets_ = 0;
    bool failed_ = false;
    bool Submit(const uint8_t* data, size_t size, uint64_t granule, bool first, bool last);
};
