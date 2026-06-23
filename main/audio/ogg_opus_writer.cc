#include "audio/ogg_opus_writer.h"

#include <cstring>
#include <vector>

#include "esp_log.h"
#include "esp_random.h"

namespace {

constexpr char kTag[] = "OggOpusWriter";

uint32_t OggCrc(const uint8_t* data, size_t size) {
    uint32_t crc = 0;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x80000000) {
                crc = (crc << 1) ^ 0x04C11DB7;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

void AppendUInt16LE(std::vector<uint8_t>& data, uint16_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void AppendUInt32LE(std::vector<uint8_t>& data, uint32_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void AppendUInt64LE(std::vector<uint8_t>& data, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        data.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

}  // namespace

OggOpusWriter::~OggOpusWriter() {
    Close();
}

bool OggOpusWriter::Open(const char* path, uint32_t sample_rate, uint8_t channels, uint32_t frame_samples) {
    Close();
    file_ = fopen(path, "wb");
    if (file_ == nullptr) {
        ESP_LOGE(kTag, "Failed to open Ogg Opus file: %s", path);
        return false;
    }

    serial_ = esp_random();
    sequence_ = 0;
    sample_rate_ = sample_rate;
    channels_ = channels;
    frame_samples_ = frame_samples;
    granule_position_ = 0;
    packet_count_ = 0;

    std::vector<uint8_t> opus_head;
    opus_head.insert(opus_head.end(), {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'});
    opus_head.push_back(1);
    opus_head.push_back(channels_);
    AppendUInt16LE(opus_head, 0);
    AppendUInt32LE(opus_head, sample_rate_);
    AppendUInt16LE(opus_head, 0);
    opus_head.push_back(0);
    if (!WritePage(opus_head.data(), opus_head.size(), 0x02, 0)) {
        Close();
        return false;
    }

    const char vendor[] = "Audream firmware";
    std::vector<uint8_t> opus_tags;
    opus_tags.insert(opus_tags.end(), {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'});
    AppendUInt32LE(opus_tags, sizeof(vendor) - 1);
    opus_tags.insert(opus_tags.end(), vendor, vendor + sizeof(vendor) - 1);
    AppendUInt32LE(opus_tags, 0);
    if (!WritePage(opus_tags.data(), opus_tags.size(), 0x00, 0)) {
        Close();
        return false;
    }

    return true;
}

bool OggOpusWriter::WritePacket(const uint8_t* data, size_t size) {
    if (file_ == nullptr || data == nullptr || size == 0) {
        return false;
    }
    granule_position_ += frame_samples_;
    if (!WritePage(data, size, 0x00, granule_position_)) {
        return false;
    }
    packet_count_++;
    return true;
}

bool OggOpusWriter::Close() {
    if (file_ == nullptr) {
        return true;
    }
    const bool ok = fclose(file_) == 0;
    file_ = nullptr;
    if (!ok) {
        ESP_LOGE(kTag, "Failed to close Ogg Opus file");
    }
    return ok;
}

bool OggOpusWriter::WritePage(
    const uint8_t* packet,
    size_t packet_size,
    uint8_t header_type,
    uint64_t granule_position
) {
    if (file_ == nullptr || packet_size > 255 * 255) {
        return false;
    }

    std::vector<uint8_t> segment_table;
    size_t remaining = packet_size;
    while (remaining >= 255) {
        segment_table.push_back(255);
        remaining -= 255;
    }
    segment_table.push_back(static_cast<uint8_t>(remaining));

    std::vector<uint8_t> page;
    page.reserve(27 + segment_table.size() + packet_size);
    page.insert(page.end(), {'O', 'g', 'g', 'S'});
    page.push_back(0);
    page.push_back(header_type);
    AppendUInt64LE(page, granule_position);
    AppendUInt32LE(page, serial_);
    AppendUInt32LE(page, sequence_++);
    AppendUInt32LE(page, 0);
    page.push_back(static_cast<uint8_t>(segment_table.size()));
    page.insert(page.end(), segment_table.begin(), segment_table.end());
    page.insert(page.end(), packet, packet + packet_size);

    const uint32_t crc = OggCrc(page.data(), page.size());
    page[22] = static_cast<uint8_t>(crc & 0xFF);
    page[23] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    page[24] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    page[25] = static_cast<uint8_t>((crc >> 24) & 0xFF);

    return fwrite(page.data(), 1, page.size(), file_) == page.size();
}
