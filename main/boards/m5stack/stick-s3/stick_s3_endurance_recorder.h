#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "audio/ogg_opus_writer.h"

class AudioService;

class StickS3EnduranceRecorder {
public:
    static StickS3EnduranceRecorder& GetInstance();

    bool Start(AudioService& audio_service);
    bool Stop(AudioService& audio_service, const std::string& reason);
    bool IsRecording() const;
    uint64_t DurationMs() const;
    void LogLastSessionDuration() const;

private:
    struct Segment {
        std::string recording_id;
        std::string file_name;
        std::string file_path;
        uint32_t index = 0;
        uint32_t duration_ms = 0;
        uint32_t size_bytes = 0;
        uint32_t packet_count = 0;
        uint64_t started_at_uptime_ms = 0;
        uint64_t started_at_rtc_ms = 0;
        uint64_t started_at_unix_ms = 0;
        bool time_synchronized = false;
    };

    StickS3EnduranceRecorder() = default;

    mutable std::mutex mutex_;
    OggOpusWriter writer_;
    Segment current_segment_;
    std::vector<Segment> segments_;
    std::string session_id_;
    uint32_t next_segment_index_ = 0;
    uint32_t overwritten_segment_count_ = 0;
    uint64_t total_encoded_duration_ms_ = 0;
    uint64_t started_at_uptime_ms_ = 0;
    uint64_t started_at_rtc_ms_ = 0;
    uint64_t started_at_unix_ms_ = 0;
    uint64_t rtc_epoch_seq_ = 0;
    int64_t started_at_us_ = 0;
    bool time_synchronized_ = false;
    bool recording_ = false;
    bool stopping_ = false;
    bool write_failed_ = false;

    void HandleOpusPacket(const uint8_t* data, size_t size);
    bool OpenNextSegmentLocked(uint64_t elapsed_ms);
    Segment FinalizeCurrentSegmentLocked();
    bool WriteSegmentMetadataLocked(const Segment& segment) const;
    bool WriteSessionMetadataLocked(uint64_t elapsed_ms, const std::string& reason) const;
    uint64_t DurationMsLocked() const;
};
