#include "system/audream_power_manager.h"
#include "audio/audio_session_coordinator.h"
#include "watch/watch_time_service.h"
#include "fake_esp_power.h"
#include <cstdlib>
#include <iostream>
#include <memory>

class AudreamPowerManagerTest {
public:
    static std::unique_ptr<AudreamPowerManager> Create() {
        return std::unique_ptr<AudreamPowerManager>(new AudreamPowerManager());
    }
    static void Attempt(AudreamPowerManager& power) {
        power.wake_gpio_ = 11;
        power.state_ = AudreamPowerState::kDeepSleepPreparing;
        try { power.EnterDeepSleep(); } catch (const fake_power::DeepSleepEntered&) {}
    }
    static void Tick(AudreamPowerManager& power) { power.Tick(); }
    static void SetIdle(AudreamPowerManager& power) { power.state_ = AudreamPowerState::kIdle; }
};

namespace {
void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct Storage final : WatchTimeStorage {
    std::string json;
    bool fail_save = false;
    WatchStateLoad Load(std::string& value) override {
        value = json;
        return json.empty() ? WatchStateLoad::kAbsent : WatchStateLoad::kLoaded;
    }
    bool Save(const std::string& value) override {
        if (fail_save) { return false; }
        json = value;
        return true;
    }
};
WatchClockReading Clock() {
    const auto milliseconds = static_cast<uint64_t>(fake_power::now_us / 1000);
    return {milliseconds, 7, 1789257600000ULL + milliseconds};
}
struct Fixture {
    Storage storage;
    WatchTimeService service{storage};
    AudioSessionCoordinator audio;
    std::unique_ptr<AudreamPowerManager> power = AudreamPowerManagerTest::Create();
    int shutdowns = 0;
    int cancellations = 0;
    int audio_resumes = 0;
    Fixture() {
        fake_power::Reset();
        Check(service.Initialize(Clock()), "sleep fixture initializes");
        power->SetAudioSessionCoordinator(&audio, []() { return true; }, [this]() { ++audio_resumes; });
        power->SetBeforeDeepSleepCallback([this]() { ++shutdowns; });
        power->SetSleepPreparationCallbacks(
            [this]() { uint64_t delay; return service.PrepareForSleep(Clock(), delay); },
            [this](uint64_t& delay) { return service.SleepWakeDelay(Clock(), delay); },
            [this]() { ++cancellations; service.CancelSleep(); });
    }
    void Attempt() { AudreamPowerManagerTest::Attempt(*power); }
};

void TestDeadlineAfterShutdown() {
    Fixture f;
    f.power->SetSleepPreparationCallbacks(
        [&]() {
            Check(f.service.Execute(WatchTimeCommand::kStartTimer, Clock(), 10000), "timer starts during setup");
            uint64_t delay;
            return f.service.PrepareForSleep(Clock(), delay);
        },
        [&](uint64_t& delay) { return f.service.SleepWakeDelay(Clock(), delay); },
        [&]() { ++f.cancellations; f.service.CancelSleep(); });
    f.power->SetAudioSessionCoordinator(&f.audio, []() { fake_power::now_us += 2000000; return true; });
    f.power->SetBeforeDeepSleepCallback([&]() { ++f.shutdowns; fake_power::now_us += 500000; });
    f.Attempt();
    Check(fake_power::slept && f.audio.Mode() == AudioSessionMode::kSleeping, "sleep entered only after audio release");
    Check(fake_power::wake_timers == std::vector<uint64_t>{8000000, 7000000}, "both audio and peripheral shutdown deducted");
    Check(f.shutdowns == 1 && f.cancellations == 0, "successful sleep retains reservation");
    Check(f.service.Snapshot(Clock()).sleep_prepared, "state frozen until reboot");
}

void TestCheckpointAndAudioFailures() {
    {
        Fixture f;
        f.storage.fail_save = true;
        f.Attempt();
        Check(!fake_power::slept && f.shutdowns == 0 && fake_power::wifi_stops == 0, "failed checkpoint does not shut down hardware");
        Check(f.cancellations == 1 && f.audio.Mode() == AudioSessionMode::kIdle, "failed preparation releases idle reservation");
    }
    {
        Fixture f;
        int attempts = 0;
        bool released = false;
        f.power->SetAudioSessionCoordinator(&f.audio, [&]() { ++attempts; return released; }, [&]() { ++f.audio_resumes; });
        f.Attempt();
        Check(!fake_power::slept && f.shutdowns == 0, "failed audio drain prevents shutdown");
        Check(f.audio.Mode() == AudioSessionMode::kTransitioning, "failed audio drain retains exact sleep lease");
        Check(f.audio_resumes == 0, "failed audio drain does not resume assistant audio");
        Check(!f.service.Snapshot(Clock()).sleep_prepared && f.cancellations == 1, "failed drain unlocks watch controls");
        const auto generation = f.audio.Generation();
        Check(!f.audio.Begin(AudioSessionMode::kLocalRecording), "failed sleep handoff blocks recording");
        AudreamPowerManagerTest::Tick(*f.power);
        Check(attempts == 1, "failed drain does not retry immediately");
        fake_power::now_us += 5000000;
        AudreamPowerManagerTest::Tick(*f.power);
        Check(attempts == 2 && f.audio.IsCurrent(generation) && f.audio_resumes == 0,
            "failed recovery retains original reservation without resuming audio");
        released = true;
        fake_power::now_us += 5000000;
        AudreamPowerManagerTest::Tick(*f.power);
        Check(attempts == 3 && f.audio.Mode() == AudioSessionMode::kIdle && f.audio_resumes == 1,
            "successful retry releases lease and resumes once");
        Check(!fake_power::slept && f.shutdowns == 0 && fake_power::wifi_stops == 0,
            "recovery never resumes the canceled sleep request");
    }
}

void TestWakeFailures() {
    {
        Fixture f;
        Check(f.service.Execute(WatchTimeCommand::kStartTimer, Clock(), 10000), "failure timer starts");
        fake_power::timer_failure_at = 1;
        f.Attempt();
        Check(!fake_power::slept && f.shutdowns == 0, "initial timer arm failure leaves peripherals alive");
        Check(f.audio.Mode() == AudioSessionMode::kIdle && f.cancellations == 1, "timer failure cancels reservations");
    }
    {
        Fixture f;
        Check(f.service.Execute(WatchTimeCommand::kStartTimer, Clock(), 10000), "fallback timer starts");
        fake_power::timer_failure_at = 2;
        f.Attempt();
        Check(fake_power::slept && fake_power::wake_timers.size() == 1, "final arm failure retains valid initial timer");
    }
    for (const bool clear_failure : {false, true}) {
        Fixture f;
        if (clear_failure) { fake_power::clear_error = ESP_ERR_INVALID_ARG; }
        else { fake_power::wake_error = ESP_ERR_INVALID_ARG; }
        f.Attempt();
        Check(!fake_power::slept && f.shutdowns == 0 && !f.service.Snapshot(Clock()).sleep_prepared,
            "wake GPIO or reset failure does not reserve or shut down resources");
    }
}

void TestActivityDuringPreparation() {
    Fixture f;
    f.power->SetAudioSessionCoordinator(&f.audio, [&]() {
        f.power->NotifyUserActivity("test_button");
        return true;
    });
    f.Attempt();
    Check(!fake_power::slept && f.shutdowns == 0, "user activity cancels pending shutdown");
    Check(f.cancellations == 1 && f.audio.Mode() == AudioSessionMode::kIdle, "activity rollback releases both reservations");
    Check(f.power->State() == AudreamPowerState::kActive, "canceled sleep remains active");
}

void TestAlertAndLegacyPaths() {
    {
        Fixture f;
        Check(f.service.Execute(WatchTimeCommand::kStartTimer, Clock(), 1), "short timer starts");
        f.Attempt();
        Check(!fake_power::slept && f.shutdowns == 0, "expired alert prevents sleep");
        Check(f.service.Snapshot(Clock()).state.checkpoint.timer_alert, "expiry remains visible");
    }
    {
        fake_power::Reset();
        auto power = AudreamPowerManagerTest::Create();
        int shutdowns = 0;
        power->SetBeforeDeepSleepCallback([&]() { ++shutdowns; });
        AudreamPowerManagerTest::Attempt(*power);
        Check(fake_power::slept && shutdowns == 1 && fake_power::wake_timers.empty(), "legacy recording-only sleep needs no watch hooks");
    }
}

void TestPeripheralFailureRollback() {
    Fixture f;
    int restores = 0;
    f.power->SetPeripheralSleepCallbacks([]() { return false; }, [&]() {
        Check(f.audio.Mode() == AudioSessionMode::kTransitioning, "restore runs while reservation is held");
        Check(f.audio_resumes == 0, "audio stays quiesced during peripheral restore");
        ++restores;
        return true;
    });
    f.Attempt();
    Check(!fake_power::slept && fake_power::wifi_stops == 0 && f.shutdowns == 0,
        "peripheral failure never reaches irreversible shutdown");
    Check(restores == 1 && f.audio_resumes == 1 && f.audio.Mode() == AudioSessionMode::kIdle,
        "successful rollback restores peripherals before releasing audio");
    Check(!f.service.Snapshot(Clock()).sleep_prepared, "watch controls unfreeze after rollback");
}

void TestPeripheralRecoveryRetry() {
    Fixture f;
    int restores = 0;
    bool ready = false;
    f.power->SetPeripheralSleepCallbacks([]() { return false; }, [&]() { ++restores; return ready; });
    f.Attempt();
    Check(!fake_power::slept && restores == 1 && f.audio_resumes == 0, "failed rollback retains quiesced resources");
    Check(f.audio.Mode() == AudioSessionMode::kTransitioning && f.audio.BlocksSleep(), "pending recovery keeps exact transition lease");
    Check(!f.audio.Begin(AudioSessionMode::kLocalRecording), "recording cannot use half-restored peripherals");
    Check(!f.service.Snapshot(Clock()).sleep_prepared, "time controls remain usable during hardware recovery");
    AudreamPowerManagerTest::Tick(*f.power);
    Check(restores == 1, "recovery retries are rate limited");
    fake_power::now_us += 5000000;
    AudreamPowerManagerTest::Tick(*f.power);
    Check(restores == 2 && f.audio_resumes == 0, "unsuccessful retry keeps resources reserved");
    ready = true;
    fake_power::now_us += 5000000;
    AudreamPowerManagerTest::Tick(*f.power);
    Check(restores == 3 && f.audio_resumes == 1 && f.audio.Mode() == AudioSessionMode::kIdle,
        "successful retry releases reservation and resumes audio once");
    AudreamPowerManagerTest::Tick(*f.power);
    Check(restores == 3, "completed recovery does not run again");
}

void TestPeripheralActivityRollback() {
    for (const bool gpio_only : {false, true}) {
        Fixture f;
        int restores = 0;
        f.power->SetPeripheralSleepCallbacks([&]() {
            if (gpio_only) { fake_power::wake_level = 0; }
            else { f.power->NotifyUserActivity("touch_during_shutdown"); }
            return true;
        }, [&]() { ++restores; return true; });
        f.Attempt();
        Check(restores == 1 && !fake_power::slept && f.shutdowns == 0,
            "late touch or GPIO activity restores peripherals without irreversible shutdown");
        Check(f.audio.Mode() == AudioSessionMode::kIdle && f.audio_resumes == 1, "late activity returns to usable idle resources");
    }
}

void TestStorageReadLeases() {
    Fixture f;
    Check(f.power->BeginStorageRead() && f.power->BeginStorageRead(), "independent storage readers acquire leases");
    f.power->SetStorageBusy(true);
    f.power->SetStorageBusy(false);
    f.Attempt();
    Check(!fake_power::slept && f.shutdowns == 0, "another storage operation cannot release catalog lease");
    f.power->EndStorageRead();
    f.Attempt();
    Check(!fake_power::slept, "one reader remains after a single release");
    f.power->EndStorageRead();
    bool rejected = false;
    f.power->SetBeforeDeepSleepCallback([&]() { rejected = !f.power->BeginStorageRead(); });
    f.Attempt();
    Check(fake_power::slept && rejected, "new directory reads are rejected during sleep preparation");
}

void TestPeripheralPreparationContract() {
    {
        Fixture f;
        int preparations = 0;
        f.power->SetPeripheralSleepCallbacks([&]() { ++preparations; return true; }, {});
        f.Attempt();
        Check(preparations == 0 && !fake_power::slept && f.shutdowns == 0,
            "missing restore callback prevents touching peripherals");
    }
    {
        Fixture f;
        bool prepared = false;
        int restores = 0;
        f.power->SetPeripheralSleepCallbacks([&]() { prepared = true; return true; }, [&]() { ++restores; return true; });
        f.power->SetBeforeDeepSleepCallback([&]() { Check(prepared, "checked preparation precedes final shutdown"); ++f.shutdowns; });
        f.Attempt();
        Check(fake_power::slept && f.shutdowns == 1 && restores == 0 && f.audio_resumes == 0,
            "successful sleep never restores peripherals or microphone");
    }
}
}

void TestVoiceNetworkLeases() {
    Fixture f;
    AudreamPowerManagerTest::SetIdle(*f.power);
    Check(!f.power->BeginVoiceNetworkRequest(), "background refresh cannot restart the idle sleep countdown");
    Check(!f.power->BeginVoiceNetworkRequest(true), "idle refresh cannot bypass enabled deep sleep");
    Check(f.power->State() == AudreamPowerState::kIdle, "declined refresh keeps the watch idle");
    f.power->NotifyUserActivity("test_voice_request");
    Check(f.power->BeginVoiceNetworkRequest(), "network request reserves power");
    Check(f.power->BeginVoiceNetworkRequest(), "multiple network requests are counted");
    f.Attempt();
    Check(!fake_power::slept, "active network request blocks sleep");
    f.power->EndVoiceNetworkRequest();
    f.Attempt();
    Check(!fake_power::slept, "remaining network request still blocks sleep");
    f.power->EndVoiceNetworkRequest();
    f.Attempt();
    Check(fake_power::slept, "joined network requests permit sleep");
    Check(!f.power->BeginVoiceNetworkRequest(), "sleep preparation refuses new network work");
}

void TestContinuousWatchStandby() {
    Fixture f;
    f.power->Start(11, false);
    Check(!f.power->IsDisplayIdle(), "watch screen starts awake");
    fake_power::now_us += 45000000;
    AudreamPowerManagerTest::Tick(*f.power);
    Check(f.power->State() == AudreamPowerState::kIdle, "continuous standby still permits screen idle");
    Check(f.power->IsDisplayIdle(), "watch screen follows idle state");
    fake_power::now_us += 12LL * 60 * 60 * 1000000;
    AudreamPowerManagerTest::Tick(*f.power);
    Check(f.power->State() == AudreamPowerState::kIdle && !fake_power::slept && f.shutdowns == 0,
        "overnight standby does not shut down continuous watch services");
    Check(!f.power->BeginVoiceNetworkRequest(), "continuous standby does not allow idle cloud refresh");
    Check(f.power->BeginVoiceNetworkRequest(true), "explicit always-available refresh may reserve idle power");
    Check(f.power->State() == AudreamPowerState::kIdle && f.power->IsDisplayIdle(),
        "always-available refresh does not wake the screen");
    f.power->EndVoiceNetworkRequest();
    Check(fake_power::wifi_stops == 0 && f.audio.Mode() == AudioSessionMode::kIdle &&
        !f.service.Snapshot(Clock()).sleep_prepared, "standby does not reserve audio or prepare peripherals for sleep");
    f.power->Start(12);
    AudreamPowerManagerTest::Tick(*f.power);
    Check(f.power->State() == AudreamPowerState::kIdle, "repeated startup cannot change the running standby policy");
    f.Attempt();
    Check(!fake_power::slept && f.shutdowns == 0 && f.power->State() == AudreamPowerState::kIdle,
        "sleep entry also checks the standby policy before touching hardware");
    f.power->NotifyUserActivity("watch_button");
    Check(f.power->State() == AudreamPowerState::kActive, "button reactivates continuous standby");
    Check(!f.power->IsDisplayIdle(), "button wakes the idle screen");
    f.power->SetRecordingActive(true);
    fake_power::now_us += 120000000;
    AudreamPowerManagerTest::Tick(*f.power);
    Check(f.power->State() == AudreamPowerState::kActive, "recording keeps its existing sleep blocker");
    Check(f.power->IsDisplayIdle(), "recording sleep blocker does not keep the display awake");
    f.power->NotifyUserActivity("recording_highlight");
    Check(!f.power->IsDisplayIdle(), "recording interaction restores the display timeout");
    fake_power::now_us += 45000000;
    f.power->SetRecordingActive(false);
    AudreamPowerManagerTest::Tick(*f.power);
    Check(f.power->State() == AudreamPowerState::kIdle && !fake_power::slept,
        "recording completion returns to continuous standby");
}

void TestLegacyIdleSleepCountdown() {
    Fixture f;
    f.power->Start(11);
    fake_power::now_us += 45000000;
    AudreamPowerManagerTest::Tick(*f.power);
    Check(f.power->State() == AudreamPowerState::kIdle, "legacy runtime enters idle after the active timeout");
    fake_power::now_us += 29000000;
    AudreamPowerManagerTest::Tick(*f.power);
    Check(!fake_power::slept, "legacy idle retains its full sleep grace period");
    fake_power::now_us += 1000000;
    try { AudreamPowerManagerTest::Tick(*f.power); } catch (const fake_power::DeepSleepEntered&) {}
    Check(fake_power::slept && f.shutdowns == 1, "legacy runtime still sleeps after thirty seconds idle");
}

void TestMotionWakeCannotOverrideRecording() {
    Fixture f;
    f.power->Start(11, false);
    Check(!f.power->TryWakeDisplayForMotion(), "motion cannot renew an already active screen");
    fake_power::now_us += 45000000;
    AudreamPowerManagerTest::Tick(*f.power);
    Check(f.power->TryWakeDisplayForMotion() && !f.power->IsDisplayIdle(), "motion wakes an idle watch screen");
    fake_power::now_us += 10000000;
    AudreamPowerManagerTest::Tick(*f.power);
    Check(f.power->IsDisplayIdle(), "motion wake uses a bounded ten-second screen timeout");
    Check(f.power->BeginStorageRead(), "test holds a storage reservation");
    Check(!f.power->TryWakeDisplayForMotion(), "motion does not override a pending resource owner");
    f.power->EndStorageRead();
    f.power->SetRecordingActive(true);
    fake_power::now_us += 45000000;
    Check(f.power->IsDisplayIdle() && !f.power->TryWakeDisplayForMotion(),
        "a delayed wrist event cannot relight the display during recording");
    f.power->SetRecordingActive(false);
    f.power->SetTemporaryAPTransferActive(true);
    Check(!f.power->TryWakeDisplayForMotion(), "transfer suppresses wrist wake");
    f.power->SetTemporaryAPTransferActive(false);
}

void TestMaintenanceDoesNotRelightDisplay() {
    Fixture f;
    f.power->Start(11, false);
    fake_power::now_us += 45000000;
    AudreamPowerManagerTest::Tick(*f.power);
    Check(f.power->IsDisplayIdle() && f.power->BeginStorageRead(), "storage starts while screen is idle");
    AudreamPowerManagerTest::Tick(*f.power);
    Check(f.power->State() == AudreamPowerState::kActive && f.power->IsDisplayIdle(),
        "a system sleep blocker does not become a display interaction");
    f.power->EndStorageRead();
    Check(f.power->TryWakeDisplayForMotion(), "motion can wake after maintenance without waiting for another system idle");
    auto listening = f.audio.Begin(AudioSessionMode::kAssistantConnecting);
    Check(bool(listening) && f.audio.Complete(listening) &&
        f.audio.UpdateAssistant(AudioSessionMode::kAssistantListening, listening.generation), "test acquires assistant input");
    fake_power::now_us += 60000000;
    Check(!f.power->IsDisplayIdle(), "active assistant input cannot be canceled by automatic screen hiding");
    Check(f.audio.EndAssistant(listening.generation), "assistant releases input");
    Check(f.power->IsDisplayIdle(), "screen timeout takes effect once assistant interaction ends");
}

void TestDynamicStandbyPolicy() {
    Fixture f;
    Check(!f.power->SetIdleDeepSleepEnabled(false), "policy cannot apply before the power task starts");
    f.power->Start(11, false);
    fake_power::now_us += 45000000;
    AudreamPowerManagerTest::Tick(*f.power);
    fake_power::now_us += 3600000000LL;
    Check(f.power->SetIdleDeepSleepEnabled(true) && f.power->IdleDeepSleepEnabled(), "battery saver is applied");
    Check(f.power->IsDisplayIdle(), "policy change does not light the screen");
    fake_power::now_us += 29000000;
    AudreamPowerManagerTest::Tick(*f.power);
    Check(!fake_power::slept, "enabling saver starts a new grace period even after long standby");
    Check(f.power->SetIdleDeepSleepEnabled(false), "standby can replace pending saver");
    fake_power::now_us += 3600000000LL;
    AudreamPowerManagerTest::Tick(*f.power);
    Check(!fake_power::slept && f.shutdowns == 0, "standby prevents shutdown");
    Check(f.power->SetIdleDeepSleepEnabled(true), "saver can be selected again");
    fake_power::now_us += 29000000;
    Check(f.power->SetIdleDeepSleepEnabled(true), "identical refresh succeeds without postponing sleep");
    fake_power::now_us += 1000000;
    try { AudreamPowerManagerTest::Tick(*f.power); } catch (const fake_power::DeepSleepEntered&) {}
    Check(fake_power::slept && f.shutdowns == 1, "saver executes the existing checkpoint and peripheral shutdown");
}

void TestPolicyChangePreservesRecordingAndRejectsShutdown() {
    Fixture f;
    f.power->Start(11, false);
    f.power->SetRecordingActive(true);
    Check(f.power->SetIdleDeepSleepEnabled(true), "saver preference may change without canceling recording");
    fake_power::now_us += 120000000;
    AudreamPowerManagerTest::Tick(*f.power);
    Check(!fake_power::slept && f.power->State() == AudreamPowerState::kActive,
        "recording remains a sleep blocker in saver mode");
    f.power->SetRecordingActive(false);
    f.power->SetAudioSessionCoordinator(&f.audio, [&]() {
        Check(!f.power->SetIdleDeepSleepEnabled(false), "in-flight shutdown rejects a policy change");
        return false;
    });
    f.Attempt();
    Check(!fake_power::slept && !f.power->SetIdleDeepSleepEnabled(false),
        "unconfirmed audio cleanup rejects policy changes until recovery");
}

int main() {
    TestDeadlineAfterShutdown(); TestCheckpointAndAudioFailures(); TestWakeFailures();
    TestActivityDuringPreparation(); TestAlertAndLegacyPaths();
    TestPeripheralFailureRollback(); TestPeripheralRecoveryRetry();
    TestPeripheralActivityRollback(); TestPeripheralPreparationContract();
    TestStorageReadLeases();
    TestVoiceNetworkLeases();
    TestContinuousWatchStandby(); TestLegacyIdleSleepCountdown();
    TestMotionWakeCannotOverrideRecording();
    TestMaintenanceDoesNotRelightDisplay();
    TestDynamicStandbyPolicy();
    TestPolicyChangePreservesRecordingAndRejectsShutdown();
    std::cout << "17 watch power integration tests passed\n";
}
