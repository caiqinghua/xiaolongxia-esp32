#pragma once

#include <atomic>
#include <cstdint>

enum class BlufiRole : uint32_t {
    kControlOnly = 1,
    kWifiProvisioning = 2,
    kProvisioningAndControl = 3,
};

// A ticket belongs to one connection attempt in one BLE lifecycle. Role changes
// and shutdown invalidate it so a delayed worker cannot close a newer session.
class BlufiSessionState {
public:
    using Ticket = uint32_t;

    bool IsActive() const { return (state_.load() & kRoleMask) != 0; }
    bool AllowsProvisioning() const { return (state_.load() & 2U) != 0; }
    bool KeepsControl() const { return (state_.load() & 1U) != 0; }
    bool IsProvisioned() const { return (state_.load() & kCompleted) != 0; }
    bool IsConnecting() const { return (state_.load() & kPending) != 0; }
    Ticket Snapshot() const { return state_.load(); }
    BlufiRole Role() const { return static_cast<BlufiRole>(state_.load() & kRoleMask); }

    bool Activate(BlufiRole role) {
        auto state = state_.load();
        for (;;) {
            if ((state & kRoleMask) == static_cast<uint32_t>(role)) {
                return false;
            }
            if (state_.compare_exchange_weak(state, Next(state, static_cast<uint32_t>(role)))) {
                return true;
            }
        }
    }

    void Deactivate() {
        auto state = state_.load();
        while (!state_.compare_exchange_weak(state, Next(state, 0))) {}
    }

    void InvalidateRequests() {
        auto state = state_.load();
        while (!state_.compare_exchange_weak(state, Next(state, state & (kRoleMask | kCompleted)))) {}
    }

    Ticket BeginConnection() {
        auto state = state_.load();
        for (;;) {
            if ((state & 2U) == 0 || (state & kPending) != 0) {
                return 0;
            }
            const auto next = Next(state, (state & kRoleMask) | kPending);
            if (state_.compare_exchange_weak(state, next)) {
                return next;
            }
        }
    }

    bool IsCurrent(Ticket ticket) const {
        return ticket != 0 && state_.load() == ticket;
    }

    bool CompleteConnection(Ticket ticket, bool success) {
        if (ticket == 0 || (ticket & kPending) == 0) {
            return false;
        }
        auto role = ticket & kRoleMask;
        if (success && role == static_cast<uint32_t>(BlufiRole::kProvisioningAndControl)) {
            role = static_cast<uint32_t>(BlufiRole::kControlOnly);
        }
        const auto next = Next(ticket, role | (success ? kCompleted : 0));
        return state_.compare_exchange_strong(ticket, next);
    }

private:
    static constexpr uint32_t kRoleMask = 3;
    static constexpr uint32_t kPending = 4;
    static constexpr uint32_t kCompleted = 8;
    static constexpr uint32_t kFlagsMask = 15;

    static uint32_t Next(uint32_t state, uint32_t flags) {
        return ((state & ~kFlagsMask) + 16U) | flags;
    }

    std::atomic<uint32_t> state_{0};
};
