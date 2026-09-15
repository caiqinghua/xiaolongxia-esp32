#include "watch_motion_task.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits.h>

WatchMotionTask::~WatchMotionTask() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return;
        quit_ = true;
    }
    changed_.notify_all();
    // Never destroy captured board state while a driver operation is still active.
    if (pthread_join(thread_, nullptr) != 0) std::abort();
}

bool WatchMotionTask::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || !hooks_.initialize || !hooks_.start || !hooks_.read || !hooks_.stop ||
        !hooks_.load || !hooks_.save || !hooks_.clock || !hooks_.recording_or_transfer) return false;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    const bool configured = pthread_attr_setstacksize(&attributes, std::max<size_t>(8192, PTHREAD_STACK_MIN)) == 0;
    const int result = configured ? pthread_create(&thread_, &attributes, Entry, this) : -1;
    pthread_attr_destroy(&attributes);
    started_ = result == 0;
    return started_;
}

bool WatchMotionTask::Suspend(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!started_) return true;
    sleep_requested_ = true;
    changed_.notify_all();
    return changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
        [this]() { return sleep_ready_ || !sleep_requested_; }) && sleep_requested_ && sleep_ready_;
}

bool WatchMotionTask::Resume(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    sleep_requested_ = sleep_ready_ = false;
    changed_.notify_all();
    return changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
        [this]() { return !sleep_in_progress_; });
}

WatchMotionSnapshot WatchMotionTask::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

bool WatchMotionTask::RetryStorage(uint64_t request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || quit_ || sleep_requested_ || !request_id ||
        snapshot_.retry_pending || snapshot_.reset_pending ||
        request_id <= std::max(snapshot_.retry_request, snapshot_.reset_request)) return false;
    snapshot_.retry_request = request_id;
    snapshot_.retry_pending = true;
    snapshot_.retry_succeeded = false;
    changed_.notify_all();
    return true;
}

void WatchMotionTask::CompleteStorageRetry(uint64_t request_id, bool succeeded) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.retry_request != request_id || !snapshot_.retry_pending) return;
    snapshot_.retry_pending = false;
    snapshot_.retry_succeeded = succeeded;
}

bool WatchMotionTask::ResetCorruptStorage(uint64_t request_id, uint64_t repair_token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || quit_ || sleep_requested_ || !request_id || !repair_token ||
        repair_token != snapshot_.repair_token || snapshot_.initialized || !snapshot_.storage_error ||
        snapshot_.retry_pending || snapshot_.reset_pending ||
        request_id <= std::max(snapshot_.retry_request, snapshot_.reset_request)) return false;
    snapshot_.reset_request = request_id;
    snapshot_.reset_pending = true;
    snapshot_.reset_succeeded = false;
    reset_token_ = repair_token;
    changed_.notify_all();
    return true;
}

void WatchMotionTask::CompleteStorageReset(uint64_t request_id, bool succeeded) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.reset_request != request_id || !snapshot_.reset_pending) return;
    snapshot_.reset_pending = false;
    snapshot_.reset_succeeded = succeeded;
}

void WatchMotionTask::SetRepairable(bool corrupt) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.repair_token = 0;
    if (corrupt && repair_generation_ != UINT64_MAX) snapshot_.repair_token = ++repair_generation_;
}

void* WatchMotionTask::Entry(void* self) {
    static_cast<WatchMotionTask*>(self)->Run();
    return nullptr;
}

bool WatchMotionTask::Save(WatchActivity& activity, bool sleeping) {
    const auto record = EncodeWatchActivityCheckpoint(activity.Checkpoint());
    if (!record) return false;
    if (last_saved_ && *last_saved_ == *record) return true;
    if (!hooks_.save(*record, sleeping)) return false;
    last_saved_ = record;
    return true;
}

void WatchMotionTask::Publish(const WatchActivity& activity, const WatchMotionClock& clock,
    bool initialized, bool storage_error, bool sensor_error) {
    WatchMotionSnapshot next;
    next.activity = activity.Snapshot(clock.day);
    next.initialized = initialized;
    next.storage_error = storage_error;
    if (initialized) next.history = activity.Checkpoint();
    if (sensor_error) next.activity.status = WatchMotionStatus::kFault;
    std::lock_guard<std::mutex> lock(mutex_);
    next.retry_request = snapshot_.retry_request;
    next.retry_pending = snapshot_.retry_pending;
    next.retry_succeeded = snapshot_.retry_succeeded;
    next.repair_token = snapshot_.repair_token;
    next.reset_request = snapshot_.reset_request;
    next.reset_pending = snapshot_.reset_pending;
    next.reset_succeeded = snapshot_.reset_succeeded;
    snapshot_ = next;
}

void WatchMotionTask::Run() {
    WatchActivity activity;
    bool history_checked = false;
    bool history_ok = false;
    bool storage_error = false;
    bool initialized = false;
    bool running = false;
    bool stopped = false;
    bool sensor_error = false;
    uint64_t last_attempt = 0;
    bool attempted = false;
    uint64_t last_save = 0;
    while (true) {
        bool sleeping = false;
        uint64_t retry = 0;
        uint64_t reset = 0;
        bool reset_authorized = false;
        bool reset_succeeded = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (quit_) break;
            sleeping = sleep_requested_;
            retry = snapshot_.retry_pending ? snapshot_.retry_request : 0;
            reset = snapshot_.reset_pending ? snapshot_.reset_request : 0;
            reset_authorized = reset && reset_token_ && reset_token_ == snapshot_.repair_token;
            sleep_in_progress_ = sleeping;
            if (!sleeping) changed_.notify_all();
        }
        const auto clock = hooks_.clock();
        bool paused = sleeping || hooks_.recording_or_transfer();
        if (retry && sleeping) {
            CompleteStorageRetry(retry, false);
            retry = 0;
        }
        if (reset && sleeping) {
            CompleteStorageReset(reset, false);
            reset = 0;
        }
        if (retry && !history_ok) history_checked = false;
        if (!history_checked) {
            WatchActivityRecord stored{};
            const auto loaded = hooks_.load(stored, sleeping);
            if (loaded != WatchMotionLoad::kBusy) {
                WatchActivityCheckpoint checkpoint;
                history_checked = true;
                const bool decoded = loaded == WatchMotionLoad::kLoaded &&
                    DecodeWatchActivityCheckpoint(stored.data(), stored.size(), checkpoint);
                history_ok = loaded == WatchMotionLoad::kAbsent ||
                    (decoded && activity.Restore(checkpoint));
                SetRepairable(loaded == WatchMotionLoad::kCorrupt ||
                    (loaded == WatchMotionLoad::kLoaded && !decoded));
                storage_error = !history_ok;
                if (loaded == WatchMotionLoad::kLoaded && history_ok) last_saved_ = stored;
            }
        }
        if (!initialized && (!attempted || clock.monotonic_ms < last_attempt ||
            clock.monotonic_ms - last_attempt >= 30000)) {
            // Initializing also shuts down a sensor left running across an MCU reset.
            attempted = true;
            last_attempt = clock.monotonic_ms;
            initialized = hooks_.initialize();
            stopped = initialized;
            sensor_error = !initialized;
        }
        if (reset && reset_authorized && !paused && !history_ok && initialized && stopped) {
            WatchActivityRecord stored{};
            WatchActivityCheckpoint checkpoint;
            const auto loaded = hooks_.load(stored, false);
            const bool decoded = loaded == WatchMotionLoad::kLoaded &&
                DecodeWatchActivityCheckpoint(stored.data(), stored.size(), checkpoint);
            const bool corrupt = loaded == WatchMotionLoad::kCorrupt ||
                (loaded == WatchMotionLoad::kLoaded && !decoded);
            SetRepairable(corrupt);
            if (decoded || loaded == WatchMotionLoad::kAbsent) {
                // A record recovered since confirmation must not be erased.
                activity = WatchActivity{};
                history_ok = activity.Restore(checkpoint);
                last_saved_ = decoded ? std::optional<WatchActivityRecord>(stored) : std::nullopt;
                storage_error = !history_ok;
                reset_succeeded = history_ok;
            } else if (corrupt) {
                bool cancelled;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    cancelled = sleep_requested_ || quit_;
                }
                if (!cancelled && !hooks_.recording_or_transfer()) {
                    WatchActivityCheckpoint empty;
                    empty.incomplete = true;
                    const auto replacement = *EncodeWatchActivityCheckpoint(empty);
                    // Commit may fail after flash changed. Require a fresh read
                    // before another reset, and never write old history back.
                    SetRepairable(false);
                    last_saved_.reset();
                    if (hooks_.save(replacement, false)) {
                        activity = WatchActivity{};
                        history_ok = activity.Restore(empty);
                        last_saved_ = replacement;
                        storage_error = !history_ok;
                        reset_succeeded = history_ok;
                    } else storage_error = true;
                }
            }
        }
        if (reset) {
            paused = paused || hooks_.recording_or_transfer();
            std::lock_guard<std::mutex> lock(mutex_);
            paused = paused || sleep_requested_;
        }
        if (paused || !history_ok) {
            ObserveMotion(nullptr, clock.monotonic_ms, false);
            if (running) {
                WatchImuReading reading;
                if (hooks_.read(reading)) {
                    const auto captured = hooks_.clock();
                    activity.Observe(reading.generation, reading.steps, captured.monotonic_ms, captured.day);
                }
                else activity.ReadFailed();
                running = false;
            }
            activity.Pause();
            if (initialized && !stopped) stopped = hooks_.stop();
            sensor_error = !initialized || !stopped;
            if (history_ok && stopped && (!storage_error || sleeping || clock.monotonic_ms < last_save ||
                clock.monotonic_ms - last_save >= 60000)) {
                storage_error = !Save(activity, sleeping);
                last_save = clock.monotonic_ms;
            }
            Publish(activity, clock, history_ok, storage_error, sensor_error);
            if (sleeping && stopped && history_checked && (!history_ok || !storage_error)) {
                std::unique_lock<std::mutex> lock(mutex_);
                if (sleep_requested_) {
                    sleep_ready_ = true;
                    changed_.notify_all();
                    changed_.wait(lock, [this]() { return quit_ || !sleep_requested_; });
                }
                continue;
            }
        } else if (initialized) {
            bool just_started = false;
            if (!running && (!attempted || stopped || clock.monotonic_ms < last_attempt ||
                clock.monotonic_ms - last_attempt >= 30000)) {
                last_attempt = clock.monotonic_ms;
                const uint64_t generation = hooks_.start();
                stopped = false;
                const auto started = hooks_.clock();
                running = generation && activity.BeginCounter(generation, started.monotonic_ms, started.day);
                just_started = running;
                sensor_error = !running;
            }
            if (running && !just_started) {
                WatchImuReading reading;
                if (hooks_.read(reading)) {
                    const auto captured = hooks_.clock();
                    sensor_error = !activity.Observe(reading.generation, reading.steps, captured.monotonic_ms, captured.day);
                    ObserveMotion(sensor_error ? nullptr : &reading, captured.monotonic_ms, true);
                } else {
                    activity.ReadFailed();
                    sensor_error = true;
                    ObserveMotion(nullptr, hooks_.clock().monotonic_ms, true);
                }
            } else {
                ObserveMotion(nullptr, clock.monotonic_ms, running);
            }
            if (clock.monotonic_ms < last_save || clock.monotonic_ms - last_save >= 60000) {
                storage_error = !Save(activity, false);
                last_save = clock.monotonic_ms;
            }
            Publish(activity, hooks_.clock(), history_ok, storage_error, sensor_error);
        } else {
            ObserveMotion(nullptr, clock.monotonic_ms, false);
            Publish(activity, clock, history_ok, storage_error, true);
        }
        if (retry) {
            // Never reload older storage over valid, not-yet-committed RAM totals.
            const bool succeeded = history_ok && Save(activity, false);
            if (history_ok) storage_error = !succeeded;
            Publish(activity, hooks_.clock(), history_ok, storage_error, sensor_error);
            CompleteStorageRetry(retry, succeeded);
        }
        if (reset) CompleteStorageReset(reset, reset_succeeded);
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, std::chrono::milliseconds(paused ? 1000 : 250),
            [this, sleeping]() { return quit_ || sleep_requested_ != sleeping ||
                snapshot_.retry_pending || snapshot_.reset_pending; });
    }
    ObserveMotion(nullptr, hooks_.clock().monotonic_ms, false);
    if (initialized) hooks_.stop();
}

void WatchMotionTask::ObserveMotion(const WatchImuReading* reading, uint64_t now_ms, bool allowed) {
    if (!hooks_.motion_observation) return;
    // A recording or sleep request can arrive during the sensor read.
    bool canceled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        canceled = quit_ || sleep_requested_;
    }
    allowed = allowed && !canceled && !hooks_.recording_or_transfer();
    hooks_.motion_observation(allowed ? reading : nullptr, now_ms, allowed);
}
