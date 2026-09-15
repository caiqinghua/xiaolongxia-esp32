#pragma once

enum class BoardRuntimeProfile {
    kAssistant,
    kRecordingOnly,
    kWatchAssistantRecorder,
};

constexpr BoardRuntimeProfile LegacyBoardRuntimeProfile(bool recording_only) {
    return recording_only ? BoardRuntimeProfile::kRecordingOnly : BoardRuntimeProfile::kAssistant;
}

constexpr bool KeepsAudreamBleControl(BoardRuntimeProfile profile) {
    return profile != BoardRuntimeProfile::kAssistant;
}

constexpr BoardRuntimeProfile WaveshareWatchRuntimeProfile(bool watch_enabled) {
    return watch_enabled ? BoardRuntimeProfile::kWatchAssistantRecorder : BoardRuntimeProfile::kRecordingOnly;
}

constexpr bool UsesOfflineWatchStartup(BoardRuntimeProfile profile) {
    return profile == BoardRuntimeProfile::kWatchAssistantRecorder;
}

constexpr bool UsesDefaultWakeWordDetection(BoardRuntimeProfile profile) {
    return profile == BoardRuntimeProfile::kAssistant;
}

constexpr bool UsesIdleDeepSleep(BoardRuntimeProfile profile) {
    return profile != BoardRuntimeProfile::kWatchAssistantRecorder;
}
