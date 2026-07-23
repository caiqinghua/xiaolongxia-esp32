#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "audio/ogg_opus_writer.h"

class AudioService;

struct LocalRecordingHighlight {
    std::string id;
    std::string kind;
    std::string source;
    uint32_t timestamp_ms = 0;
};

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
    bool Pause(AudioService& audio_service);
    bool Resume(AudioService& audio_service);
    LocalRecordingMetadata Stop(AudioService& audio_service);
    bool AddMarkerHighlight(const std::string& source);
    void HandleOpusPacket(const uint8_t* data, size_t size);
    bool IsRecording() const;
    bool IsPaused() const;
    uint32_t CurrentDurationMs() const;
    LocalRecordingMetadata CurrentMetadata() const;
    LocalRecordingMetadata LastMetadata() const;

private:
    enum class State {
        kClosed,
        kOpening,
        kRecording,
        kPausing,
        kPaused,
        kResuming,
        kStopping,
    };

    LocalRecordingService() = default;

    mutable std::mutex mutex_;
    OggOpusWriter writer_;
    LocalRecordingMetadata current_;
    LocalRecordingMetadata last_;
    std::vector<LocalRecordingHighlight> current_highlights_;
    std::vector<LocalRecordingHighlight> last_highlights_;
    int64_t started_at_us_ = 0;
    int64_t paused_at_us_ = 0;
    int64_t accumulated_paused_us_ = 0;
    State state_ = State::kClosed;

    uint32_t CurrentDurationMsLocked(int64_t now_us) const;
    bool WriteMetadataFile(const LocalRecordingMetadata& metadata) const;
};
