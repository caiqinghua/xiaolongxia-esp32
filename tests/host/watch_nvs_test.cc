#include "watch/watch_time_runtime.h"
#include "nvs.h"
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main(int argc, char** argv) {
    Check(argc == 2, "test scenario provided");
    const std::string scenario = argv[1];
    auto& service = GetWatchTimeService();
    const auto clock = ReadWatchClock();
    Check(clock.rtc_ms == 1000 && clock.rtc_epoch == 7 && clock.unix_ms == 1789257601000ULL,
        "platform clock adapter reads milliseconds and epoch");
    if (scenario == "restore") {
        WatchTimeState state;
        state.timezone = "Asia/Shanghai";
        state.checkpoint.rtc_epoch = 7;
        state.checkpoint.saved_rtc_ms = 500;
        state.checkpoint.stopwatch_running = true;
        state.checkpoint.stopwatch_elapsed_ms = 9000;
        state.checkpoint.laps = {1000, 2000};
        fake_nvs::committed = EncodeWatchTimeState(state);
        fake_nvs::present = true;
        Check(service.Initialize(clock), "saved NVS state restored");
        const auto snapshot = service.Snapshot(clock);
        Check(snapshot.state.timezone == "Asia/Shanghai" && snapshot.state.checkpoint.stopwatch_elapsed_ms == 9500,
            "stored preferences and elapsed time restored through actual adapter");
        Check(snapshot.laps == state.checkpoint.laps && fake_nvs::commits == 0, "restoring does not write flash");
    } else if (scenario == "read_failures") {
        fake_nvs::fail_open = true;
        Check(!service.Initialize(clock), "NVS open error surfaced");
        fake_nvs::fail_open = false;
        fake_nvs::present = true;
        fake_nvs::fail_read = true;
        Check(!service.Initialize(clock), "NVS read error surfaced");
        fake_nvs::fail_read = false;
        fake_nvs::committed = std::string(4097, 'x');
        Check(!service.Initialize(clock), "oversized NVS state rejected before allocation");
        fake_nvs::committed = "{broken";
        Check(!service.Initialize(clock), "malformed NVS state preserved for recovery");
        Check(fake_nvs::commits == 0 && fake_nvs::handles == 0, "read failures do not overwrite or leak handles");
        fake_nvs::committed = EncodeWatchTimeState({});
        Check(service.Initialize(clock), "read retry can restore valid state");
    } else if (scenario == "save_failures") {
        Check(service.Initialize(clock), "missing namespace initialized without erasing flash");
        fake_nvs::fail_set = true;
        Check(!service.Execute(WatchTimeCommand::kStartStopwatch, clock), "set failure surfaced");
        Check(fake_nvs::commits == 0 && !service.Snapshot(clock).state.checkpoint.stopwatch_running,
            "set failure does not commit or change state");
        fake_nvs::fail_set = false;
        fake_nvs::fail_commit = true;
        Check(!service.Execute(WatchTimeCommand::kStartStopwatch, clock), "commit failure surfaced");
        Check(!service.Snapshot(clock).state.checkpoint.stopwatch_running && fake_nvs::committed.empty(),
            "failed commit does not acknowledge stopwatch start");
        fake_nvs::fail_commit = false;
        Check(service.Execute(WatchTimeCommand::kStartStopwatch, clock), "successful retry persists state");
        WatchTimeState decoded;
        Check(DecodeWatchTimeState(fake_nvs::committed, decoded) && decoded.checkpoint.stopwatch_running,
            "successful command reaches persistent NVS value");
    } else {
        Check(false, "unknown scenario");
    }
    Check(fake_nvs::handles == 0, "NVS handles closed");
    Check(fake_nvs::last_namespace == "audream_watch" && fake_nvs::last_key == "time_state",
        "watch state uses isolated namespace and key");
    std::cout << "PASS: watch NVS " << scenario << '\n';
}
