#pragma once

#include <cstdint>

enum class PlaybackVolumeStatus {
    kNone, kPending, kApplied, kRejected, kFailed, kUnsafe, kCancelled,
};

struct PlaybackVolumeSnapshot {
    uint64_t request = 0;
    int value = -1;
    PlaybackVolumeStatus status = PlaybackVolumeStatus::kNone;
};
