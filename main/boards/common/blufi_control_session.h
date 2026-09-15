#pragma once

#include <array>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <mutex>

// Bind queued work to one physical connection and its native security grant.
// This guards application admission, not the SDK's asynchronous transmit queue.
class BlufiControlSession {
public:
    struct Ticket {
        uint64_t generation = 0;
        uint64_t authorization = 0;
        uint16_t connection = 0;
        std::array<uint8_t, 6> address{};
        explicit operator bool() const { return generation != 0; }
    };

    explicit BlufiControlSession(bool secure) : secure_(secure) {}

    void Connected(uint16_t connection, const uint8_t* address) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = {};
        if (!address || generation_ == std::numeric_limits<uint64_t>::max()) return;
        current_.generation = ++generation_;
        current_.connection = connection;
        std::copy_n(address, current_.address.size(), current_.address.begin());
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = {};
    }

    // The verifier must be a memory-only lookup in the native security owner.
    // It must not call back into this session or initiate BLE operations.
    template <typename Verifier>
    Ticket Capture(Verifier verify) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!current_) return {};
        auto ticket = current_;
        if (secure_) {
            ticket.authorization = verify(ticket.connection, ticket.address.data());
            if (!ticket.authorization) return {};
        }
        return ticket;
    }

    template <typename Verifier>
    bool IsCurrent(const Ticket& ticket, Verifier verify) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ticket || !current_ || ticket.generation != current_.generation ||
            ticket.connection != current_.connection || ticket.address != current_.address) return false;
        return secure_ ? ticket.authorization &&
            ticket.authorization == verify(ticket.connection, ticket.address.data()) : ticket.authorization == 0;
    }

private:
    const bool secure_;
    mutable std::mutex mutex_;
    uint64_t generation_ = 0;
    Ticket current_;
};
