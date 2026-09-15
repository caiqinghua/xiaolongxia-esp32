#pragma once

#include "watch_pairing_gatt_server.h"
#include "watch_voice_pairing.h"
#include "watch_voice_task.h"
#include <memory>

namespace watch_voice {

// The Application owner calls every method except Cancel. UI/GATT callbacks
// must enqueue owner commands, not run these operations under display/BLE locks.
class PairingController {
public:
    using Phase = PairingPhase;
    using Error = PairingError;
    using State = PairingState;
    struct Hooks {
        // Reserve storage, BLE and power for the whole physical pairing window.
        // Release is called on the owner, only after the actual worker is joined.
        std::function<bool()> acquire;
        std::function<void()> release;
        // Thread-safe, memory-only revocation check (recording, sleep, Wi-Fi off).
        // It must not call binding APIs or acquire display/Bluetooth locks.
        std::function<bool()> eligible;
        // Trusted clock only; return zero if unsynchronized. Worker-safe.
        std::function<uint64_t()> unix_seconds;
        std::function<bool()> prepare_tls_clock;
    };
    PairingController(Endpoints endpoints, Hooks hooks);
    ~PairingController();
    PairingController(const PairingController&) = delete;
    PairingController& operator=(const PairingController&) = delete;
    bool Open(bool control_only = false);
    bool OpenBindingRemoval();
    bool ConfirmBindingRemoval(uint64_t generation, bool accept);
    bool OpenControlRemoval(uint64_t peer_generation);
    bool OpenSavedControlPeers();
    bool SelectSavedControlPeer(uint64_t selection);
    bool ConfirmControlRemoval(uint64_t peer_generation, bool accept);
    bool ConfirmPeer(uint64_t confirmation, bool accept);
    bool ApproveAccount(uint64_t request, bool accept);
    void Cancel();
    void Poll();
    bool StopWithin(uint32_t timeout_ms);
    bool Active() const { return reserved_; }
    State Snapshot() const { return state_; }

private:
    struct Job;
    bool Begin(bool control_only, bool removing_binding, uint64_t control_peer = 0, bool saved_control = false);
    bool Launch(RequestTask::Work work, Phase phase);
    bool Current() const;
    void CloseTransport();
    void Finish(Phase phase, Error error = Error::kNone);
    void HandleJob(Result result);
    void Verify(WatchPairingGattServer::Request request);
    Endpoints endpoints_;
    Hooks hooks_;
    PairingSession signer_;
    State state_;
    uint64_t attempt_ = 0;
    bool reserved_ = false;
    uint64_t started_ms_ = 0, window_ = 0, bound_ms_ = 0, expected_generation_ = 0;
    std::atomic<bool> cancelled_{false};
    // Serializes the final write admission against memory-only cancellation.
    std::mutex admission_mutex_;
    WatchPairingGattServer::Request challenge_;
    std::shared_ptr<Job> job_;
    std::unique_ptr<RequestTask> task_;
};

}  // namespace watch_voice
