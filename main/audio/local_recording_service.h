#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "audio/ogg_opus_writer.h"

class AudioService;

struct LocalRecordingMetadata {
    std::string recording_id;
    std::string file_name;
    std::string file_path;
    uint64_t rtc_epoch_seq = 0;
    uint32_t duration_ms = 0;
    uint32_t size_bytes = 0;
    uint32_t packet_count = 0;
    uint64_t started_at_unix_ms = 0;
    uint64_t started_at_uptime_ms = 0;
    uint64_t started_at_rtc_ms = 0;
    bool time_synchronized = false;
    bool valid = false;
};

class LocalRecordingService {
public:
    static LocalRecordingService& GetInstance();

    bool Start(AudioService& audio_service);
    LocalRecordingMetadata Stop(AudioService& audio_service);
    void HandleOpusPacket(const uint8_t* data, size_t size);
    bool IsRecording() const;
    LocalRecordingMetadata CurrentMetadata() const;
    LocalRecordingMetadata LastMetadata() const;

private:
    LocalRecordingService() = default;

    mutable std::mutex mutex_;
    OggOpusWriter writer_;
    LocalRecordingMetadata current_;
    LocalRecordingMetadata last_;
    int64_t started_at_us_ = 0;
    bool recording_ = false;

    bool WriteMetadataFile(const LocalRecordingMetadata& metadata) const;
};
