#pragma once

#include <cstdint>

enum class WatchStationAction { kNone, kStart, kStop };

// Application-task policy only. Driver success and IP association are separate.
class WatchStationPolicy {
public:
    void Request(uint64_t now_ms);
    void CancelDemand();
    void KeepForConversation(uint64_t now_ms);
    WatchStationAction Poll(uint64_t now_ms, bool enabled, bool continuous,
                            bool busy, bool connected, bool provisioning);
    void Complete(WatchStationAction action, bool succeeded, uint64_t now_ms);
    bool Suspended() const { return suspended_; }
    bool Failed() const { return failed_; }

private:
    bool initialized_ = false;
    bool suspended_ = false;
    bool waiting_ = false;
    bool cleanup_ = false;
    bool failed_ = false;
    unsigned attempts_ = 0;
    uint64_t hold_until_ms_ = 0;
    uint64_t association_deadline_ms_ = 0;
    uint64_t next_operation_ms_ = 0;
    uint64_t reconnect_after_ms_ = 0;
};
