#pragma once

#include <cstdint>
#include <functional>

// Marshal closure to the protocol owner. Callbacks and their targets must
// outlive queued work; Application owns this object for the device lifetime.
class AssistantTransportHandoff {
public:
    using Dispatch = std::function<void(std::function<void()>)>;
    using IsOwner = std::function<bool()>;
    using IsCurrent = std::function<bool(uint64_t)>;
    using Close = std::function<bool(uint32_t)>;

    AssistantTransportHandoff(Dispatch dispatch, IsOwner is_owner,
                              IsCurrent is_current, Close close);
    bool CloseWithin(uint64_t generation, uint32_t timeout_ms) const;

private:
    const Dispatch dispatch_;
    const IsOwner is_owner_;
    const IsCurrent is_current_;
    const Close close_;
};
