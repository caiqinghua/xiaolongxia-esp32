#include "watch_pairing_controller.h"
#include "watch_ancs_client.h"
#include "watch_voice_binding.h"
#include "esp_timer.h"
#include <chrono>
#include <thread>
#include <algorithm>

namespace watch_voice {
namespace {
uint64_t Now() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }
void Wipe(std::string& value) {
    volatile char* bytes = value.data();
    for (size_t i = 0; i < value.size(); ++i) bytes[i] = 0;
    value.clear();
}
}

struct PairingController::Job {
    WatchPairingGattServer::Request request;
    PairingOffer offer;
    DeviceIdentity identity;
    uint64_t generation = 0;
    std::array<uint8_t, 64> proof{};
    Error error = Error::kNone;
    bool committed = false;
    bool storage_failed = false;
    bool control_removal_started = false;
    bool control_removed = false;
    std::vector<SavedControlPeer> saved_peers;
    ~Job() { Wipe(request.payload); }
};

PairingController::PairingController(Endpoints endpoints, Hooks hooks)
    : endpoints_(std::move(endpoints)), hooks_(std::move(hooks)), task_(std::make_unique<RequestTask>()) {}
PairingController::~PairingController() {
    Cancel();
    task_.reset();  // RequestTask joins before any captured owner state is released.
    if (job_ && job_->control_removal_started) {
        Finish(job_->control_removed ? Phase::kControlRemoved : Phase::kFailed, job_->error);
        return;
    }
    if (state_.control_only && reserved_ && window_) WatchAncsClient::Get().CancelControlPairing(window_);
    Finish(job_ && job_->committed ? (state_.removing_binding ? Phase::kBindingRemoved : Phase::kBound) :
        state_.phase == Phase::kBound || state_.phase == Phase::kControlReady || state_.phase == Phase::kBindingRemoved ||
        state_.phase == Phase::kControlRemoved
            ? state_.phase : Phase::kCancelled);
}
bool PairingController::Current() const {
    const auto now = Now();
    return !cancelled_.load() && now >= started_ms_ && now - started_ms_ < 120000 && hooks_.eligible();
}
bool PairingController::Launch(RequestTask::Work work, Phase phase) {
    if (!task_->Start(std::move(work), [] { return true; }, [] {})) {
        Finish(Phase::kFailed, Error::kUnavailable);
        return false;
    }
    state_.phase = phase;
    return true;
}
bool PairingController::Open(bool control_only) {
    return Begin(control_only, false);
}
bool PairingController::OpenBindingRemoval() { return Begin(false, true); }
bool PairingController::OpenControlRemoval(uint64_t peer_generation) {
    return peer_generation && Begin(false, false, peer_generation);
}
bool PairingController::OpenSavedControlPeers() { return Begin(false, false, 0, true); }
bool PairingController::SelectSavedControlPeer(uint64_t selection) {
    if (!reserved_ || !state_.removing_saved_control || state_.phase != Phase::kAwaitSavedPeer ||
        !Current() || !selection || !WatchAncsClient::Get().CanForgetSavedControlPeer(selection) ||
        std::none_of(state_.saved_peers.begin(), state_.saved_peers.end(), [&](const auto& peer) {
            return peer.selection == selection;
        })) return false;
    state_.control_peer_generation = selection;
    state_.phase = Phase::kAwaitControlRemoval;
    return true;
}
bool PairingController::Begin(bool control_only, bool removing_binding, uint64_t control_peer, bool saved_control) {
    if (reserved_ || task_->Active()) return false;
    state_ = {};
    state_.control_only = control_only;
    state_.removing_binding = removing_binding;
    state_.removing_control = control_peer != 0 || saved_control;
    state_.removing_saved_control = saved_control;
    state_.control_peer_generation = control_peer;
    if (attempt_ == UINT64_MAX) {
        state_.phase = Phase::kFailed;
        state_.error = Error::kUnavailable;
        return false;
    }
    state_.attempt = ++attempt_;
    if (!hooks_.acquire || !hooks_.release || !hooks_.eligible ||
        (!control_only && !removing_binding && !control_peer && !saved_control &&
         (!endpoints_.Valid() || !hooks_.unix_seconds || !hooks_.prepare_tls_clock))) {
        state_.phase = Phase::kFailed;
        state_.error = Error::kUnavailable;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        cancelled_.store(false);
    }
    if (!hooks_.eligible() || !hooks_.acquire()) {
        state_.phase = Phase::kFailed;
        state_.error = Error::kUnavailable;
        return false;
    }
    reserved_ = true;
    started_ms_ = Now();
    expected_generation_ = 0;
    if (saved_control) {
        if (!Current() || !WatchAncsClient::Get().CanInspectSavedControlPeers()) {
            Finish(Phase::kFailed, Error::kUnavailable);
            return false;
        }
        job_ = std::make_shared<Job>();
        const auto job = job_;
        return Launch([this, job](const Client::Cancelled& cancelled, Response&) {
            if (cancelled() || !Current()) return Result::kCancelled;
            std::vector<WatchSavedControlPeer> peers;
            if (!WatchAncsClient::Get().InspectSavedControlPeers(peers)) {
                job->error = Error::kControlRemoval;
                return Result::kInvalidRequest;
            }
            for (const auto& peer : peers) job->saved_peers.push_back({peer.selection, peer.stored_address});
            return Result::kOk;
        }, Phase::kInspectingSavedPeers);
    }
    if (control_peer) {
        if (!Current() || !WatchAncsClient::Get().CanForgetControlPeer(control_peer)) {
            Finish(Phase::kFailed, Error::kUnavailable); return false;
        }
        state_.phase = Phase::kAwaitControlRemoval;
        return true;
    }
    if (control_only) {
        if (!Current()) { Finish(Phase::kCancelled); return false; }
        window_ = WatchAncsClient::Get().OpenControlPairingWindow();
        if (!window_) { Finish(Phase::kFailed, Error::kUnavailable); return false; }
        state_.phase = Phase::kAwaitPeer;
        return true;
    }
    job_ = std::make_shared<Job>();
    const auto job = job_;
    return Launch([this, job, removing_binding](const Client::Cancelled& cancelled, Response&) {
        if (cancelled() || !Current()) return Result::kCancelled;
        Binding existing;
        const auto binding = LoadBinding(existing);
        job->generation = existing.generation;
        Wipe(existing.credential);
        if (removing_binding) {
            if (binding != BindingStatus::kBound) {
                job->error = binding == BindingStatus::kAbsent || binding == BindingStatus::kRemoved
                    ? Error::kNoBinding : Error::kStorage;
                return Result::kInvalidRequest;
            }
            job->offer.device_id = existing.device_id;
            return Result::kOk;
        }
        if (binding == BindingStatus::kBound) { job->error = Error::kAlreadyBound; return Result::kConflict; }
        if (binding != BindingStatus::kAbsent && binding != BindingStatus::kRemoved) {
            job->error = Error::kStorage; return Result::kInvalidRequest;
        }
        if (LoadDeviceIdentity(job->identity) != IdentityStatus::kOk || !signer_.OpenLocalWindow(Now())) {
            job->error = Error::kIdentity; return Result::kInvalidRequest;
        }
        const auto unix_time = hooks_.unix_seconds();
        if (unix_time < 1600000000 || unix_time > 4102444800) {
            job->error = Error::kClock; return Result::kClockRequired;
        }
        return Result::kOk;
    }, removing_binding ? Phase::kInspectingBinding : Phase::kPreparing);
}
bool PairingController::ConfirmBindingRemoval(uint64_t generation, bool accept) {
    if (!reserved_ || !state_.removing_binding || state_.phase != Phase::kAwaitRemoval ||
        !generation || generation != expected_generation_ || !Current()) return false;
    if (!accept) { Cancel(); Finish(Phase::kCancelled); return true; }
    job_ = std::make_shared<Job>();
    const auto job = job_;
    return Launch([this, job, generation](const Client::Cancelled& cancelled, Response&) {
        if (cancelled() || !Current()) return Result::kCancelled;
        const auto removed = RemoveBindingAuthorized(generation, [&] {
            std::lock_guard<std::mutex> lock(admission_mutex_);
            return !cancelled() && Current();
        });
        job->committed = removed == BindingStatus::kRemoved;
        job->storage_failed = removed == BindingStatus::kStorageError;
        if (job->committed) return Result::kOk;
        job->error = removed == BindingStatus::kConflict ? Error::kConflict : Error::kStorage;
        return removed == BindingStatus::kCancelled ? Result::kCancelled : Result::kConflict;
    }, Phase::kRemovingBinding);
}
bool PairingController::ConfirmControlRemoval(uint64_t generation, bool accept) {
    if (!reserved_ || !state_.removing_control || state_.phase != Phase::kAwaitControlRemoval ||
        !generation || generation != state_.control_peer_generation || !Current()) return false;
    if (!accept) { Cancel(); Finish(Phase::kCancelled); return true; }
    job_ = std::make_shared<Job>();
    const auto job = job_;
    const bool saved = state_.removing_saved_control;
    return Launch([this, job, generation, saved](const Client::Cancelled& cancelled, Response&) {
        auto& native = WatchAncsClient::Get();
        {
            std::lock_guard<std::mutex> lock(admission_mutex_);
            if (cancelled() || !Current()) return Result::kCancelled;
            job->control_removal_started = saved ? native.ForgetSavedControlPeer(generation) : native.ForgetControlPeer(generation);
        }
        if (!job->control_removal_started) {
            job->error = Error::kControlRemoval;
            return Result::kConflict;
        }
        // Accepted revocation cannot be undone by cancellation. Complete local
        // I/O and observe the native result before releasing the worker's owner.
        while (true) {
            native.Poll();
            const auto result = native.ControlRemovalSnapshot();
            if (result.peer_generation != generation) {
                job->error = Error::kControlRemoval;
                return Result::kConflict;
            }
            if (result.phase == WatchControlRemovalPhase::kComplete && !result.io_pending) {
                job->control_removed = true;
                return Result::kOk;
            }
            if (result.phase == WatchControlRemovalPhase::kFailed && !result.io_pending) {
                job->error = result.error == WatchControlRemovalError::kStorage ? Error::kStorage : Error::kControlRemoval;
                return Result::kConflict;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }, Phase::kRemovingControl);
}
void PairingController::Cancel() {
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        cancelled_.store(true);
    }
    if (task_) task_->Cancel();
}
void PairingController::CloseTransport() {
    if (!window_) return;
    if (!state_.control_only) WatchPairingGattServer::Get().CloseLocalWindow(window_);
    WatchAncsClient::Get().CloseDevicePairingWindow(window_);
    window_ = 0;
}
void PairingController::Finish(Phase phase, Error error) {
    CloseTransport();
    signer_.Cancel();
    Wipe(challenge_.payload);
    challenge_ = {};
    job_.reset();
    state_.phase = phase;
    state_.error = error;
    state_.offer = {};
    state_.saved_peers.clear();
    if (reserved_) { reserved_ = false; hooks_.release(); }
}
bool PairingController::ConfirmPeer(uint64_t confirmation, bool accept) {
    if (!reserved_ || state_.phase != Phase::kAwaitPeer || !Current()) return false;
    if (state_.control_only) {
        const bool handled = WatchAncsClient::Get().ConfirmControlPairing(window_, confirmation, accept);
        if (handled && !accept) { Cancel(); Finish(Phase::kCancelled); }
        return handled;
    }
    if (!WatchPairingGattServer::Get().WindowCurrent(window_)) return false;
    const bool handled = WatchAncsClient::Get().ConfirmDevicePairing(confirmation, accept);
    if (handled && !accept) { Cancel(); Finish(Phase::kCancelled); }
    return handled;
}
bool PairingController::ApproveAccount(uint64_t request, bool accept) {
    if (!reserved_ || state_.phase != Phase::kAwaitApproval || !request || request != state_.offer.request ||
        !Current() || !WatchPairingGattServer::Get().RequestCurrent(challenge_)) return false;
    if (!accept) { Cancel(); Finish(Phase::kCancelled); return true; }
    job_ = std::make_shared<Job>();
    const auto job = job_;
    job->request = challenge_;
    return Launch([this, job, request](const Client::Cancelled& cancelled, Response&) {
        if (cancelled() || !Current() || !WatchPairingGattServer::Get().RequestCurrent(job->request))
            return Result::kCancelled;
        if (!signer_.ApproveLocally(request, Now()) ||
            !signer_.TakeProof(request, job->request.peer_lease, Now(), job->proof)) {
            job->error = Error::kRejected; return Result::kInvalidRequest;
        }
        return Result::kOk;
    }, Phase::kSigning);
}
void PairingController::Verify(WatchPairingGattServer::Request request) {
    job_ = std::make_shared<Job>();
    const auto job = job_;
    // Copy then wipe even short-string storage; a move may leave secret bytes.
    job->request = request;
    Wipe(request.payload);
    const auto offer = state_.offer;
    Launch([this, job, offer](const Client::Cancelled& cancelled, Response&) {
        const auto revoked = [this, job, cancelled] {
            return cancelled() || !Current() || !WatchPairingGattServer::Get().RequestCurrent(job->request);
        };
        if (revoked()) { Wipe(job->request.payload); return Result::kCancelled; }
        if (!hooks_.prepare_tls_clock()) {
            Wipe(job->request.payload); job->error = Error::kClock; return Result::kClockRequired;
        }
        PairingReceipt receipt;
        auto result = Client(endpoints_).VerifyPairing(offer.device_id, job->request.payload,
            offer.user_id, offer.challenge_id, true, revoked, receipt);
        if (result == Result::kOk) {
            const auto saved = SaveBindingAuthorized(expected_generation_, offer.device_id, job->request.payload, [&] {
                std::lock_guard<std::mutex> lock(admission_mutex_);
                return !cancelled() && Current() && WatchPairingGattServer::Get().AdmitCredentialCommit(job->request);
            });
            job->committed = saved == BindingStatus::kBound;
            job->storage_failed = saved == BindingStatus::kStorageError;
            if (!job->committed) {
                job->error = saved == BindingStatus::kConflict ? Error::kConflict : Error::kStorage;
                result = saved == BindingStatus::kCancelled ? Result::kCancelled : Result::kConflict;
            }
        } else job->error = Error::kVerification;
        Wipe(job->request.payload);
        return result;
    }, Phase::kVerifying);
}
void PairingController::HandleJob(Result result) {
    if (job_->control_removal_started) {
        Finish(job_->control_removed ? Phase::kControlRemoved : Phase::kFailed, job_->error);
        return;
    }
    // Durable truth wins over cancellation and over a lost final BLE response.
    if (job_->committed) {
        state_.binding_generation = expected_generation_ + 1;
        if (state_.removing_binding) { Finish(Phase::kBindingRemoved); return; }
        state_.phase = Phase::kBound;
        state_.offer = {};
        bound_ms_ = Now();
        const bool notified = WatchPairingGattServer::Get().BindingCommitted(job_->request);
        job_.reset();
        if (!notified || cancelled_.load() || !hooks_.eligible()) Finish(Phase::kBound);
        return;
    }
    // A failed NVS operation can have changed durable bytes. Cancellation must
    // not hide this condition or invite a blind retry with unknown credentials.
    if (job_->storage_failed) { Finish(Phase::kFailed, Error::kStorage); return; }
    if (!Current() || result == Result::kCancelled) {
        const bool expired = state_.error == Error::kTimeout;
        Finish(expired ? Phase::kFailed : Phase::kCancelled, expired ? Error::kTimeout : Error::kNone);
        return;
    }
    if (result != Result::kOk) { Finish(Phase::kFailed, job_->error); return; }
    if (state_.phase == Phase::kInspectingSavedPeers) {
        state_.saved_peers = std::move(job_->saved_peers);
        state_.phase = Phase::kAwaitSavedPeer;
    } else if (state_.phase == Phase::kInspectingBinding) {
        expected_generation_ = state_.binding_generation = job_->generation;
        state_.device_id = job_->offer.device_id;
        state_.phase = Phase::kAwaitRemoval;
    } else if (state_.phase == Phase::kPreparing) {
        expected_generation_ = job_->generation;
        state_.device_id = job_->identity.device_id;
        window_ = WatchAncsClient::Get().OpenDevicePairingWindow();
        if (!window_ || !WatchPairingGattServer::Get().OpenLocalWindow(window_, state_.device_id)) {
            Finish(Phase::kFailed, Error::kUnavailable); return;
        }
        state_.phase = Phase::kAwaitPeer;
    } else if (state_.phase == Phase::kReadingChallenge) {
        if (!WatchPairingGattServer::Get().RequestCurrent(job_->request)) { Finish(Phase::kCancelled); return; }
        state_.offer = job_->offer;
        challenge_ = job_->request;
        Wipe(challenge_.payload);
        state_.phase = Phase::kAwaitApproval;
    } else if (state_.phase == Phase::kSigning) {
        if (!WatchPairingGattServer::Get().PublishProof(job_->request, job_->proof)) { Finish(Phase::kCancelled); return; }
        state_.phase = Phase::kAwaitCredential;
    }
    job_.reset();
}
void PairingController::Poll() {
    if (!reserved_) return;
    if (state_.control_only) {
        auto& native = WatchAncsClient::Get();
        if (!Current()) {
            const auto now = Now();
            const bool expired = !cancelled_.load() && (now < started_ms_ || now - started_ms_ >= 120000);
            native.CancelControlPairing(window_);
            Finish(expired ? Phase::kFailed : Phase::kCancelled, expired ? Error::kTimeout : Error::kNone);
        } else if (const auto lease = native.ControlPairingLease(window_)) {
            state_.control_peer_generation = lease;
            Finish(Phase::kControlReady);
        } else if (!native.ControlPairingWindowCurrent(window_)) {
            native.CancelControlPairing(window_);
            Finish(Phase::kFailed, Error::kRejected);
        }
        return;
    }
    if (state_.phase == Phase::kBound) {
        const auto now = Now();
        if (cancelled_.load() || !hooks_.eligible() || now < bound_ms_ || now - bound_ms_ >= 5000 ||
            !WatchPairingGattServer::Get().WindowCurrent(window_)) Finish(Phase::kBound);
        return;
    }
    if (!Current() || (window_ && !WatchPairingGattServer::Get().WindowCurrent(window_))) {
        const auto now = Now();
        if (!cancelled_.load() && (now < started_ms_ || now - started_ms_ >= 120000)) state_.error = Error::kTimeout;
        Cancel(); CloseTransport();
    }
    Result result;
    Response response;
    if (task_->Poll(result, response)) HandleJob(result);
    if (!reserved_ || state_.phase == Phase::kBound || task_->Active()) return;
    if (state_.removing_saved_control && !cancelled_.load() &&
        (state_.phase == Phase::kAwaitSavedPeer || state_.phase == Phase::kAwaitControlRemoval)) {
        auto& native = WatchAncsClient::Get();
        if (!native.CanInspectSavedControlPeers() || std::any_of(state_.saved_peers.begin(), state_.saved_peers.end(),
            [&](const auto& peer) { return !native.CanForgetSavedControlPeer(peer.selection); })) {
            Finish(Phase::kFailed, Error::kControlRemoval);
            return;
        }
    }
    if (cancelled_.load()) {
        const bool expired = state_.error == Error::kTimeout;
        Finish(expired ? Phase::kFailed : Phase::kCancelled, expired ? Error::kTimeout : Error::kNone);
        return;
    }
    if (state_.phase != Phase::kAwaitPeer && state_.phase != Phase::kAwaitCredential) return;
    auto request = WatchPairingGattServer::Get().TakePending();
    if (!request) return;
    if (state_.phase == Phase::kAwaitCredential && request->kind == WatchPairingGattServer::Kind::kCredential &&
        request->window == challenge_.window && request->peer_lease == challenge_.peer_lease &&
        request->wire_request == challenge_.wire_request) {
        Verify(*request); Wipe(request->payload); return;
    }
    if (state_.phase != Phase::kAwaitPeer || request->kind != WatchPairingGattServer::Kind::kChallenge) {
        Wipe(request->payload); Finish(Phase::kFailed, Error::kRejected); return;
    }
    job_ = std::make_shared<Job>();
    const auto job = job_;
    job->request = *request;
    Wipe(request->payload);
    Launch([this, job](const Client::Cancelled& cancelled, Response&) {
        if (cancelled() || !Current()) return Result::kCancelled;
        job->offer = signer_.Offer(job->request.peer_lease, job->request.payload, hooks_.unix_seconds(), Now());
        Wipe(job->request.payload);
        if (!job->offer.request || job->offer.device_id != state_.device_id) {
            job->error = Error::kInvalidChallenge; return Result::kInvalidRequest;
        }
        return Result::kOk;
    }, Phase::kReadingChallenge);
}
bool PairingController::StopWithin(uint32_t timeout_ms) {
    if (!reserved_) return true;
    Cancel();
    if (state_.control_only) WatchAncsClient::Get().CancelControlPairing(window_);
    CloseTransport();
    if (!task_->StopWithin(timeout_ms)) return false;
    if (job_ && job_->control_removal_started) {
        Finish(job_->control_removed ? Phase::kControlRemoved : Phase::kFailed, job_->error);
        return true;
    }
    if (job_ && job_->committed) state_.binding_generation = expected_generation_ + 1;
    if (job_ && job_->storage_failed) { Finish(Phase::kFailed, Error::kStorage); return true; }
    const bool committed = (job_ && job_->committed) || state_.phase == Phase::kBound || state_.phase == Phase::kBindingRemoved;
    Finish(committed ? (state_.removing_binding ? Phase::kBindingRemoved : Phase::kBound) : Phase::kCancelled);
    return true;
}

}  // namespace watch_voice
