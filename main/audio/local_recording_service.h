#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#include "audio/ogg_opus_writer.h"
#if CONFIG_AUDREAM_WATCH_UI
#include "audio/watch_opus_writer.h"
#endif
#include "audio/audio_session_coordinator.h"

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
    bool SetAssistantHandoff(std::function<bool(uint64_t, uint32_t)> callback);
    bool SetPlaybackHandoff(std::function<bool(uint64_t, uint32_t)> callback);
    bool RecoverFailedStart(AudioService& audio_service, uint32_t timeout_ms = 0);
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
    void ForgetDeletedRecording(const std::string& recording_id);

    enum class State {
        kClosed,
        kOpening,
        kRecording,
        kPausing,
        kPaused,
        kResuming,
        kStopping,
    };

    struct Snapshot {
        State state = State::kClosed;
        uint32_t duration_ms = 0;
        LocalRecordingMetadata last;
    };
    Snapshot GetSnapshot() const;

private:

    LocalRecordingService() = default;

    mutable std::mutex mutex_;
    std::mutex operation_mutex_;
#if CONFIG_AUDREAM_WATCH_UI
    WatchOpusWriter writer_;
#else
    OggOpusWriter writer_;
#endif
    LocalRecordingMetadata current_;
    LocalRecordingMetadata last_;
    std::vector<LocalRecordingHighlight> current_highlights_;
    std::vector<LocalRecordingHighlight> last_highlights_;
    int64_t started_at_us_ = 0;
    int64_t paused_at_us_ = 0;
    int64_t accumulated_paused_us_ = 0;
    State state_ = State::kClosed;
    AudioSessionTransition pending_transition_;
    std::function<bool(uint64_t, uint32_t)> prepare_assistant_;
    std::function<bool(uint64_t, uint32_t)> prepare_playback_;
    bool playback_cleanup_pending_ = false;
    bool writer_failed_ = false;
    uint64_t failed_start_generation_ = 0;

    bool RecoverFailedStartLocked(AudioService& audio_service, uint32_t timeout_ms);

    uint32_t CurrentDurationMsLocked(int64_t now_us) const;
    bool WriteMetadataFile(const LocalRecordingMetadata& metadata) const;
};
