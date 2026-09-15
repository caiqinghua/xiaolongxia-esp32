#pragma once

#include "watch_firmware_update.h"

enum class WatchUpdateBlock { kNone, kWifiDisabled, kNoCredentials, kBusy, kBattery, kNotConfigured };

struct WatchFirmwareSnapshot {
    bool available = false;
    bool can_check = false;
    bool can_install = false;
    bool can_reboot = false;
    bool network_preparing = false;
    uint64_t request = 0;
    std::string current_version;
    bool current_builtin = false;
    WatchUpdateBlock blocked = WatchUpdateBlock::kNotConfigured;
    watch_update::Snapshot update;
};
