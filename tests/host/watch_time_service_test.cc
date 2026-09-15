#include "watch_time_service.h"
#include "watch_calendar.h"
#include <cJSON.h>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <limits>

namespace {
void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
uint64_t Utc(int year, int month, int day, int hour = 0, int minute = 0) {
    std::tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    return static_cast<uint64_t>(timegm(&value)) * 1000;
}

struct Storage final : WatchTimeStorage {
    std::string json;
    bool fail_load = false;
    bool fail_save = false;
    int saves = 0;
    WatchStateLoad Load(std::string& value) override {
        if (fail_load) { return WatchStateLoad::kFailed; }
        value = json;
        return json.empty() ? WatchStateLoad::kAbsent : WatchStateLoad::kLoaded;
    }
    bool Save(const std::string& value) override {
        ++saves;
        if (fail_save) { return false; }
        json = value;
        return true;
    }
};

void TestStateRoundTrip() {
    WatchTimeState source;
    source.timezone = "Asia/Shanghai";
    source.hour24 = false;
    source.alarm_alerts = 7;
    source.checkpoint.rtc_epoch = std::numeric_limits<uint64_t>::max();
    source.checkpoint.stopwatch_elapsed_ms = std::numeric_limits<uint64_t>::max() - 4;
    source.checkpoint.laps = {1000, 2000, std::numeric_limits<uint64_t>::max() - 4};
    source.checkpoint.timer_remaining_ms = 90000;
    source.checkpoint.timer_running = source.checkpoint.timer_configured = true;
    source.checkpoint.alarms[2] = {23, 59, 0x7f, true, 741520};
    const auto json = EncodeWatchTimeState(source);
    Check(!json.empty(), "state encoded");
    WatchTimeState decoded;
    Check(DecodeWatchTimeState(json, decoded), "state decoded");
    Check(decoded.checkpoint.rtc_epoch == source.checkpoint.rtc_epoch &&
        decoded.checkpoint.stopwatch_elapsed_ms == source.checkpoint.stopwatch_elapsed_ms, "uint64 round trip is exact");
    Check(decoded.timezone == source.timezone && !decoded.hour24 && decoded.alarm_alerts == 7, "preferences and alerts restored");
    Check(decoded.checkpoint.alarms[2].last_fired_day == 741520, "alarm deduplication persisted");
    Check(decoded.checkpoint.laps == source.checkpoint.laps, "lap precision and ordering preserved");
    Check(!DecodeWatchTimeState(json + "{}", decoded), "trailing JSON rejected");
    Check(!DecodeWatchTimeState(std::string(4097, ' '), decoded), "oversized state rejected");
    Check(!DecodeWatchTimeState("{}", decoded), "missing fields rejected");
    auto* root = cJSON_Parse(json.c_str());
    cJSON_ReplaceItemInObject(root, "timer_ms", cJSON_CreateString("-1"));
    char* corrupted = cJSON_PrintUnformatted(root);
    Check(!DecodeWatchTimeState(corrupted, decoded), "negative uint64 rejected");
    cJSON_free(corrupted);
    cJSON_ReplaceItemInObject(root, "timer_ms", cJSON_CreateString("18446744073709551616"));
    corrupted = cJSON_PrintUnformatted(root);
    Check(!DecodeWatchTimeState(corrupted, decoded), "uint64 overflow rejected");
    cJSON_free(corrupted);
    cJSON_Delete(root);
    source.checkpoint.laps = {2000, 1000};
    Check(EncodeWatchTimeState(source).empty(), "decreasing laps rejected");
    source.checkpoint.laps = {source.checkpoint.stopwatch_elapsed_ms + 1};
    Check(EncodeWatchTimeState(source).empty(), "laps cannot exceed total stopwatch duration");
    source.checkpoint.laps = std::vector<uint64_t>(21, 0);
    Check(EncodeWatchTimeState(source).empty(), "persisted lap count bounded");
}

void TestPersistentCommands() {
    Storage storage;
    WatchTimeService service(storage);
    WatchClockReading clock{1000, 7, Utc(2026, 9, 13)};
    Check(service.Initialize(clock), "empty storage initializes");
    Check(service.Execute(WatchTimeCommand::kStartStopwatch, clock), "stopwatch persisted");
    Check(service.Execute(WatchTimeCommand::kStartTimer, clock, 30000), "timer persisted");
    const int saves = storage.saves;
    clock.rtc_ms += 10000;
    Check(service.Execute(WatchTimeCommand::kAddLap, clock), "lap persisted before sleep");
    const int lap_saves = storage.saves;
    service.Poll(clock);
    Check(storage.saves == saves + 1 && storage.saves == lap_saves, "ordinary ticks do not wear flash");
    uint64_t wake_delay = 0;
    Check(service.PrepareForSleep(clock, wake_delay) && wake_delay == 20000, "sleep checkpoint and timer wake delay");
    WatchTimeService restored(storage);
    clock.rtc_ms += 10000;
    Check(restored.Initialize(clock), "restore persisted state");
    const auto snapshot = restored.Snapshot(clock);
    Check(snapshot.state.checkpoint.stopwatch_elapsed_ms == 20000, "stopwatch includes deep sleep");
    Check(snapshot.state.checkpoint.timer_remaining_ms == 10000, "countdown includes deep sleep");
    Check(snapshot.laps == std::vector<uint64_t>{10000}, "laps survive deep sleep");
    clock.rtc_ms += 10000;
    Check(restored.Poll(clock).timer_expired, "timer expires after restore");
    Check(!restored.PrepareForSleep(clock, wake_delay), "undismissed alert blocks sleep");
    Check(restored.Execute(WatchTimeCommand::kDismissAlerts, clock), "alert dismissal persisted");
}

void TestStorageFailures() {
    Storage storage;
    WatchTimeService service(storage);
    const WatchClockReading clock{1000, 1, Utc(2026, 9, 13)};
    Check(service.Initialize(clock), "failure test initializes");
    storage.fail_save = true;
    Check(!service.Execute(WatchTimeCommand::kStartTimer, clock, 30000), "failed save is not success");
    Check(!service.Snapshot(clock).state.checkpoint.timer_running, "failed command rolled back");
    Check(service.Snapshot(clock).storage_error, "storage error visible");
    uint64_t delay;
    Check(!service.PrepareForSleep(clock, delay), "failed checkpoint blocks sleep");
    storage.fail_save = false;
    Check(service.Execute(WatchTimeCommand::kStartTimer, clock, 30000), "save retry succeeds");
    Check(!service.Snapshot(clock).storage_error, "successful retry clears storage error");
    Storage bad_storage;
    bad_storage.json = "{broken";
    WatchTimeService damaged(bad_storage);
    Check(!damaged.Initialize(clock), "corrupt storage reported");
    Check(!damaged.Execute(WatchTimeCommand::kResetStopwatch, clock), "corrupt state not silently overwritten");
    Check(bad_storage.saves == 0, "corrupt saved data retained");
}

void TestAlarmPersistence() {
    Storage storage;
    WatchTimeService service(storage);
    WatchClockReading clock{1000, 2, Utc(2026, 9, 13, 6, 59)};
    Check(service.Initialize(clock), "alarm test initializes");
    Check(service.SetAlarm(0, {7, 0, 127, true, -1}, clock), "daily alarm saved");
    uint64_t delay;
    Check(service.PrepareForSleep(clock, delay) && delay == 60000, "next alarm schedules wake");
    service.CancelSleep();
    clock.rtc_ms += 60000;
    clock.unix_ms += 60000;
    Check(service.Poll(clock).alarms.size() == 1, "alarm triggered");
    Check(service.Snapshot(clock).state.alarm_alerts == 1, "alarm alert visible");
    WatchTimeService restored(storage);
    Check(restored.Initialize(clock), "alarm state restored");
    Check(restored.Poll(clock).alarms.empty(), "reboot does not trigger same alarm again");
    Check(restored.Snapshot(clock).state.alarm_alerts == 1, "pending alarm survives reboot");
}

void TestTimeZonesAndDst() {
    std::tm local{};
    Check(WatchLocalTime(Utc(2026, 9, 13), "Asia/Kathmandu", local) &&
        local.tm_hour == 5 && local.tm_min == 45, "quarter-hour timezone supported");
    Check(WatchLocalTime(Utc(2026, 1, 1, 12), "America/New_York", local) && local.tm_hour == 7, "winter offset");
    Check(WatchLocalTime(Utc(2026, 7, 1, 12), "America/New_York", local) && local.tm_hour == 8, "summer offset");
    Check(!WatchLocalTime(0, "UTC", local), "invalid wall clock hidden");
    Check(!WatchLocalTime(Utc(2026, 9, 13), "Invalid/Zone", local), "unknown zone rejected");
    std::array<WatchAlarm, 5> alarms{};
    alarms[0] = {2, 30, 127, true, -1};
    const auto spring = Utc(2026, 3, 8, 6);
    Check(WatchNextAlarmDelayMs(spring, "America/New_York", alarms) ==
        Utc(2026, 3, 9, 6, 30) - spring, "nonexistent spring minute skipped");
    alarms[0] = {1, 30, 127, true, -1};
    const auto fall = Utc(2026, 11, 1, 4);
    Check(WatchNextAlarmDelayMs(fall, "America/New_York", alarms) == 90 * 60000, "first repeated minute selected");
    auto date = WatchCalendarAt(Utc(2026, 11, 1, 5, 30), "America/New_York");
    alarms[0].last_fired_day = date.day;
    Check(WatchNextAlarmDelayMs(Utc(2026, 11, 1, 6), "America/New_York", alarms) > 24 * 60 * 60000ULL,
        "second repeated minute does not ring again");
}

void TestSleepReservation() {
    Storage storage;
    WatchTimeService service(storage);
    WatchClockReading clock{1000, 2, Utc(2026, 9, 13)};
    Check(service.Initialize(clock), "sleep reservation initializes");
    Check(service.Execute(WatchTimeCommand::kStartTimer, clock, 10000), "reserved timer started");
    uint64_t delay = 0;
    Check(service.PrepareForSleep(clock, delay) && delay == 10000, "sleep state prepared");
    Check(service.Snapshot(clock).sleep_prepared, "reservation visible");
    Check(!service.Execute(WatchTimeCommand::kCancelTimer, clock), "commands rejected during sleep preparation");
    Check(!service.SetAlarm(0, {7, 0, 127, true, -1}, clock), "alarm edits cannot invalidate armed wake");
    Check(!service.SetPreferences("Asia/Shanghai", true, clock), "zone changes rejected while prepared");
    Check(!service.PrepareForSleep(clock, delay), "duplicate preparation rejected");
    clock.rtc_ms += 2500;
    clock.unix_ms += 2500;
    Check(service.SleepWakeDelay(clock, delay) && delay == 7500, "shutdown time subtracted from deadline");
    clock.unix_ms += 60000;
    Check(service.SleepWakeDelay(clock, delay) && delay == 1, "clock adjustment requires immediate reevaluation");
    clock.unix_ms -= 60000;
    clock.rtc_ms += 10000;
    clock.unix_ms += 10000;
    Check(service.SleepWakeDelay(clock, delay) && delay == 1, "expired timer never becomes disabled wake");
    Check(!service.Poll(clock).timer_expired, "poll cannot mutate persisted sleep checkpoint");
    service.CancelSleep();
    Check(service.Poll(clock).timer_expired, "aborted sleep delivers elapsed timer");
    Check(!service.SleepWakeDelay(clock, delay), "cancel invalidates wake reservation");
    Check(service.Execute(WatchTimeCommand::kDismissAlerts, clock), "commands resume after sleep abort");
    Check(service.PrepareForSleep(clock, delay) && delay == 0, "no timer needs no timed wake");
    Check(service.SleepWakeDelay(clock, delay) && delay == 0, "empty wake plan remains empty");
    clock.rtc_epoch++;
    Check(!service.SleepWakeDelay(clock, delay), "RTC discontinuity rejects wake plan");
}

void TestStoredCivilDateLabels() {
    Check(WatchCivilDateLabel(2024 * 366 + 59) == "2024-02-29", "leap date decoded from stored identity");
    Check(WatchCivilDateLabel(2025 * 366 + 59) == "2025-03-01", "non-leap month boundary decoded");
    Check(WatchCivilDateLabel(2024 * 366 + 365) == "2024-12-31", "leap year final day decoded");
    Check(WatchCivilDateLabel(2025 * 366 + 365).empty(), "unused non-leap identity rejected");
    Check(WatchCivilDateLabel(INT32_MIN).empty() && WatchCivilDateLabel(INT32_MAX).empty(), "invalid identities bounded");
    Check(WatchCivilDateLabel(2019 * 366).empty() && WatchCivilDateLabel(2100 * 366).empty(), "supported calendar range enforced");
    for (int year = 2020; year < 2100; ++year) {
        for (int month = 1; month <= 12; ++month) {
            const auto date = WatchCalendarAt(Utc(year, month, 1), "UTC");
            char expected[16];
            std::snprintf(expected, sizeof(expected), "%04d-%02d-01", year, month);
            Check(date.valid && WatchCivilDateLabel(date.day) == expected, "stored identity round-trips real calendar");
        }
    }
}

void TestUnsynchronizedAlarmSleep() {
    Storage storage;
    WatchTimeService service(storage);
    WatchClockReading clock{1000, 2, 0};
    Check(service.Initialize(clock), "unsynchronized clock initializes");
    Check(service.SetAlarm(0, {7, 0, 127, true, -1}, clock), "offline alarm setting persisted");
    const int saves = storage.saves;
    uint64_t delay;
    Check(!service.PrepareForSleep(clock, delay), "enabled alarm without valid calendar prevents indefinite sleep");
    Check(!service.Snapshot(clock).sleep_prepared, "failed preparation does not lock commands");
    Check(storage.saves == saves, "invalid calendar does not repeatedly write flash");
    Check(service.SetAlarm(0, {7, 0, 127, false, -1}, clock), "alarm disabled offline");
    Check(service.Execute(WatchTimeCommand::kStartTimer, clock, 10000), "monotonic timer works offline");
    Check(service.PrepareForSleep(clock, delay) && delay == 10000, "timer wake does not require wall clock");
}
}  // namespace

int main() {
    TestStateRoundTrip(); TestPersistentCommands(); TestStorageFailures();
    TestAlarmPersistence(); TestTimeZonesAndDst();
    TestSleepReservation(); TestUnsynchronizedAlarmSleep();
    TestStoredCivilDateLabels();
    std::cout << "8 watch persistence and calendar tests passed\n";
}
