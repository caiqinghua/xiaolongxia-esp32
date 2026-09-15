#pragma once

#include <cstdint>
#include <string>

enum class WatchRecordingCommand { kStart, kStop, kPause, kResume, kHighlight };
enum class WatchRecordingError { kNone, kStartFailed, kSaveFailed, kControlFailed };

struct WatchRecordingControlStatus {
    bool starting = false;
    WatchRecordingError error = WatchRecordingError::kNone;
};

struct WatchRecordingSnapshot {
    WatchRecordingControlStatus control;
    bool active = false;
    bool paused = false;
    bool stopping = false;
    bool transitioning = false;
    bool storage_mounted = false;
    bool resources_blocked = false;
    uint64_t duration_ms = 0;
    bool last_saved = false;
    std::string last_file;

    bool Can(WatchRecordingCommand command) const {
        switch (command) {
            case WatchRecordingCommand::kStart:
                return !active && !control.starting && !transitioning && storage_mounted && !resources_blocked;
            case WatchRecordingCommand::kStop:
                return active || control.starting;
            case WatchRecordingCommand::kPause:
                return active && !paused && !transitioning && !resources_blocked;
            case WatchRecordingCommand::kResume:
                return active && paused && !transitioning && !resources_blocked;
            case WatchRecordingCommand::kHighlight:
                return active && !paused && !transitioning && !resources_blocked;
        }
        return false;
    }
};
