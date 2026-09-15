#pragma once

enum class WatchLevel { kBrightness, kVolume };

bool IsValidWatchLevel(WatchLevel kind, int value);
// Returns -1 on a storage error. Missing or invalid legacy values use defaults.
int ReadWatchLevel(WatchLevel kind);
bool SaveWatchLevel(WatchLevel kind, int value);

struct WatchControlSnapshot {
    int brightness = -1;
    int volume = -1;
    bool can_adjust = false;
    bool voice_enabled = false;
    bool voice_save_failed = false;
    bool wifi_enabled = true;
    bool wifi_change_failed = false;
};
