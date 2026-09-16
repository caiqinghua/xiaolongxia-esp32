#include "watch_alert_sound.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits.h>
#include <utility>

#ifdef ESP_PLATFORM
#include "esp_pthread.h"
#endif

WatchAlertSound::WatchAlertSound(AudioSessionCoordinator& coordinator, Hooks hooks, uint32_t sample_rate)
    : coordinator_(coordinator), hooks_(std::move(hooks)), sample_rate_(sample_rate) {}

WatchAlertSound::~WatchAlertSound() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!joinable_) return;
        destroying_ = true;
        StopLocked();
    }
    // Do not release hooks while native output is still in flight.
    if (pthread_join(thread_, nullptr) != 0) std::abort();
}

void WatchAlertSound::StopLocked() {
    stop_ = retry_cleanup_ = true;
    changed_.notify_all();
}

void WatchAlertSound::Silence() {
    std::lock_guard<std::mutex> lock(mutex_);
    silenced_ = true;
    StopLocked();
}

bool WatchAlertSound::StopForRecording(uint64_t generation, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!coordinator_.HoldsExclusiveResources(generation) || generation == snapshot_.generation) return false;
    silenced_ = true;
    StopLocked();
    return changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !snapshot_.active; }) &&
        coordinator_.IsCurrent(generation);
}

WatchAlertSoundSnapshot WatchAlertSound::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void WatchAlertSound::Poll(uint8_t alerts, uint64_t now_ms, bool new_event) {
    std::unique_lock<std::mutex> operation(poll_mutex_, std::try_to_lock);
    if (!operation.owns_lock()) return;
    std::unique_lock<std::mutex> lock(mutex_);
    if (destroying_) return;
    if (alerts && (!alerts_ || new_event)) {
        began_ms_ = now_ms;
        attempted_ = false;
        silenced_ = false;
        if (!snapshot_.active) snapshot_.failed = false;
    }
    alerts_ = alerts;
    // At most one half-second chime per five seconds for one minute. A user
    // dismissal or recording preemption suppresses this event permanently.
    if (!alerts || silenced_ || now_ms < began_ms_ || now_ms - began_ms_ >= 60000) {
        if (snapshot_.active && !stop_) StopLocked();
        return;
    }
    if (snapshot_.active || snapshot_.failed ||
        (attempted_ && (now_ms < attempted_ms_ || now_ms - attempted_ms_ < 5000))) return;
    if (!hooks_.prepare || !hooks_.write || !hooks_.close || sample_rate_ < 8000 || sample_rate_ > 48000 ||
        sample_rate_ % 1000) { snapshot_.failed = true; return; }
    if (joinable_) {
        lock.unlock();
        const int result = pthread_join(thread_, nullptr);
        lock.lock();
        if (result != 0) { snapshot_.failed = true; return; }
        joinable_ = false;
        if (silenced_ || destroying_) return;
    }
    // Begin is atomic: no separate idle check can accidentally steal recording,
    // file playback, transfer, assistant, maintenance or sleep ownership.
    transition_ = coordinator_.Begin(AudioSessionMode::kLocalPlayback);
    if (!transition_) return;
    attempted_ = true; attempted_ms_ = now_ms;
    snapshot_ = {true, false, false, transition_.generation};
    stop_ = retry_cleanup_ = false;
    pthread_attr_t attributes;
    const bool initialized = pthread_attr_init(&attributes) == 0;
    const bool configured = initialized &&
        pthread_attr_setstacksize(&attributes, std::max<size_t>(16384, PTHREAD_STACK_MIN)) == 0;
    const int result = configured ? pthread_create(&thread_, &attributes, Entry, this) : -1;
    if (initialized) pthread_attr_destroy(&attributes);
    if (result != 0) {
        snapshot_.active = false; snapshot_.failed = true;
        coordinator_.ReleaseLocalPlayback(transition_.generation);
        return;
    }
    joinable_ = true;
}

bool WatchAlertSound::Cancelled() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_ || !coordinator_.IsCurrent(transition_.generation);
}

void* WatchAlertSound::Entry(void* self) { static_cast<WatchAlertSound*>(self)->Run(); return nullptr; }

void WatchAlertSound::Run() {
    const auto generation = transition_.generation;
    bool touched = false, failed = false;
    if (!Cancelled()) {
        touched = true;
        if (!hooks_.prepare(generation)) failed = !Cancelled();
        else if (coordinator_.Complete(transition_)) {
            const size_t chunk_samples = sample_rate_ / 50;
            std::vector<int16_t> pcm(chunk_samples);
            // Two 200 ms tones separated by 100 ms. Five-millisecond ramps
            // avoid a hard waveform edge; volume remains the hardware setting.
            for (size_t chunk = 0; chunk < 25 && !Cancelled(); ++chunk) {
                for (size_t i = 0; i < chunk_samples; ++i) {
                    const size_t sample = chunk * chunk_samples + i;
                    const size_t start = sample >= sample_rate_ * 3 / 10 ? sample_rate_ * 3 / 10 : 0;
                    const size_t position = sample - start;
                    const size_t tone_samples = sample_rate_ / 5;
                    if (position >= tone_samples) { pcm[i] = 0; continue; }
                    const double envelope = std::min(1.0, static_cast<double>(std::min(position, tone_samples - 1 - position)) /
                        (sample_rate_ / 200));
                    const double frequency = start ? 1040.0 : 880.0;
                    pcm[i] = static_cast<int16_t>(8192.0 * envelope *
                        std::sin(6.283185307179586 * frequency * position / sample_rate_));
                }
                if (!hooks_.write(generation, pcm)) { failed = !Cancelled(); break; }
            }
        }
    }
    while (touched && !hooks_.close(generation)) {
        coordinator_.MarkLocalPlaybackFault(generation);
        std::unique_lock<std::mutex> lock(mutex_);
        snapshot_.failed = snapshot_.cleanup_failed = true;
        changed_.notify_all();
        if (destroying_) std::abort();
        changed_.wait(lock, [this] { return retry_cleanup_ || destroying_; });
        retry_cleanup_ = false;
    }
    coordinator_.ReleaseLocalPlayback(generation);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.failed |= failed;
        snapshot_.active = snapshot_.cleanup_failed = false;
        changed_.notify_all();
    }
    if (hooks_.released) hooks_.released();
}
