#pragma once

#include <cstdint>
#include <string>

enum class WatchPowerModeMessage { kNone, kSaving, kConflict, kFailed };

struct WatchPowerModeSnapshot {
    uint64_t revision = 0;
    uint64_t applied_revision = 0;
    uint64_t binding_generation = 0;
    std::string power_mode;
    std::string apply_error;
    WatchPowerModeMessage message = WatchPowerModeMessage::kNone;
    bool pending = false;
    bool can_change = false;
};

struct WatchPowerModeChange {
    uint64_t revision = 0;
    uint64_t binding_generation = 0;
    uint64_t audio_generation = 0;
    uint64_t voice_intent = 0;
    std::string power_mode;
    int64_t deadline_us = 0;
    bool sent = false;
};
