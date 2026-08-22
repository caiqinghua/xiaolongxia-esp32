#pragma once

#include <cstdint>
#include <cstdio>

class OggOpusWriter {
public:
    OggOpusWriter() = default;
    ~OggOpusWriter();

    bool Open(const char* path, uint32_t sample_rate, uint8_t channels, uint32_t frame_samples);
    bool WritePacket(const uint8_t* data, size_t size);
    bool Close();
    uint64_t GranulePosition() const { return granule_position_; }
    uint32_t PacketCount() const { return packet_count_; }
    uint64_t BytesWritten() const { return bytes_written_; }

private:
    FILE* file_ = nullptr;
    uint32_t serial_ = 0;
    uint32_t sequence_ = 0;
    uint32_t sample_rate_ = 16000;
    uint32_t frame_samples_ = 960;
    uint8_t channels_ = 1;
    uint64_t granule_position_ = 0;
    uint32_t packet_count_ = 0;
    uint64_t bytes_written_ = 0;

    bool WritePage(const uint8_t* packet, size_t packet_size, uint8_t header_type, uint64_t granule_position);
};
