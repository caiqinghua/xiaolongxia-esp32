#include "watch/watch_time_mcp.h"
#include "watch/watch_calendar.h"
#include <cJSON.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <memory>
#include <thread>

struct Storage : WatchTimeStorage {
    bool fail = false;
    int writes = 0;
    std::string saved;
    WatchStateLoad Load(std::string& value) override {
        value = saved;
        return saved.empty() ? WatchStateLoad::kAbsent : WatchStateLoad::kLoaded;
    }
    bool Save(const std::string& value) override {
        ++writes;
        if (fail) return false;
        saved = value; return true;
    }
};
bool Ok(const std::string& text) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> result(cJSON_Parse(text.c_str()), cJSON_Delete);
    assert(result);
    return cJSON_IsTrue(cJSON_GetObjectItem(result.get(), "ok"));
}

std::string Token(WatchTimeService& service, const WatchClockReading& clock, int index = 0) {
    const auto text = ExecuteWatchTimeTool(service, WatchTimeTool::kStatus, clock, true);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> result(cJSON_Parse(text.c_str()), cJSON_Delete);
    assert(result && cJSON_IsTrue(cJSON_GetObjectItem(result.get(), "ok")));
    const auto* alarms = cJSON_GetObjectItem(result.get(), "alarms");
    assert(cJSON_GetArraySize(alarms) == 5);
    const auto* entry = cJSON_GetArrayItem(alarms, index);
    assert(cJSON_GetObjectItem(entry, "index")->valueint == index);
    const auto* token = cJSON_GetObjectItem(entry, "state_token");
    assert(cJSON_IsString(token));
    return token->valuestring;
}

void TestAlarmPersistenceAndGuards() {
    Storage storage;
    WatchTimeService service(storage);
    WatchClockReading clock{1000, 1, 1789293600000ULL};
    WatchAlarmToolRequest request{0, 7, 30, 62, true, "uninitialized"};
    assert(!Ok(ExecuteWatchAlarmTool(service, request, clock, true)));
    assert(service.Initialize(clock));
    request.expected_state = Token(service, clock);
    assert(!Ok(ExecuteWatchAlarmTool(service, request, clock, false)));
    auto invalid_clock = clock;
    invalid_clock.unix_ms = 0;
    assert(!Ok(ExecuteWatchAlarmTool(service, request, invalid_clock, true)));
    for (int field = 0; field < 4; ++field) {
        for (int value : {-1, 256}) {
            auto invalid = request;
            if (field == 0) invalid.index = value;
            if (field == 1) invalid.hour = value;
            if (field == 2) invalid.minute = value;
            if (field == 3) invalid.weekdays = value;
            assert(!Ok(ExecuteWatchAlarmTool(service, invalid, clock, true)));
        }
    }
    assert(storage.writes == 0);
    assert(Ok(ExecuteWatchAlarmTool(service, request, clock, true)));
    const auto saved = storage.saved;
    assert(!Ok(ExecuteWatchAlarmTool(service, request, clock, true)));
    assert(storage.saved == saved);
    WatchTimeService reboot(storage);
    assert(reboot.Initialize(clock));
    const auto restored = reboot.Snapshot(clock).state.checkpoint.alarms[0];
    assert(restored.enabled && restored.hour == 7 && restored.minute == 30 && restored.weekdays == 62);

    request.expected_state = Token(service, clock);
    request.enabled = false;
    storage.fail = true;
    assert(!Ok(ExecuteWatchAlarmTool(service, request, clock, true)));
    assert(service.Snapshot(clock).state.checkpoint.alarms[0].enabled);
    assert(storage.saved == saved);
    storage.fail = false;
    assert(Ok(ExecuteWatchAlarmTool(service, request, invalid_clock, true)));
    assert(!service.Snapshot(clock).state.checkpoint.alarms[0].enabled);

    request.expected_state = Token(service, clock);
    auto local = service.Snapshot(clock).state.checkpoint.alarms[0];
    local.hour = 9;
    assert(service.SetAlarm(0, local, clock));
    assert(!Ok(ExecuteWatchAlarmTool(service, request, clock, true)));
    request.expected_state = Token(service, clock);
    assert(service.SetPreferences("Asia/Shanghai", true, clock));
    assert(!Ok(ExecuteWatchAlarmTool(service, request, clock, true)));
    request.expected_state = Token(service, clock);
    uint64_t delay = 0;
    assert(service.PrepareForSleep(clock, delay));
    assert(!Ok(ExecuteWatchAlarmTool(service, request, clock, true)));
    service.CancelSleep();
    request.index = 4;
    request.expected_state = Token(service, clock, 4);
    request.enabled = true;
    assert(Ok(ExecuteWatchAlarmTool(service, request, clock, true)));
    assert(service.Snapshot(clock).state.checkpoint.alarms[4].enabled);
}

void TestAlarmFiringAndConcurrentUpdates() {
    Storage storage;
    WatchTimeService service(storage);
    WatchClockReading clock{1000, 1, 1789293600000ULL};
    assert(service.Initialize(clock));
    const auto calendar = WatchCalendarAt(clock.unix_ms, "UTC");
    assert(calendar.valid);
    WatchAlarmToolRequest request{0, calendar.hour, calendar.minute, 127, true, Token(service, clock)};
    assert(Ok(ExecuteWatchAlarmTool(service, request, clock, true)));
    const auto before_fire = Token(service, clock);
    assert(service.Poll(clock).alarms.size() == 1);
    request.expected_state = before_fire;
    assert(!Ok(ExecuteWatchAlarmTool(service, request, clock, true)));
    request.expected_state = Token(service, clock);
    assert(Ok(ExecuteWatchAlarmTool(service, request, clock, true)));
    assert(service.Poll(clock).alarms.empty());
    WatchTimeService reboot(storage);
    assert(reboot.Initialize(clock));
    assert(reboot.Poll(clock).alarms.empty());
    clock.unix_ms += 86400000;
    clock.rtc_ms += 86400000;
    assert(service.Poll(clock).alarms.size() == 1);

    request.index = 1;
    request.weekdays = 0;
    request.expected_state = Token(service, clock, 1);
    assert(Ok(ExecuteWatchAlarmTool(service, request, clock, true)));
    assert(service.Poll(clock).alarms.size() == 1);
    assert(!service.Snapshot(clock).state.checkpoint.alarms[1].enabled);

    auto first = request;
    first.hour = (calendar.hour + 1) % 24;
    first.expected_state = Token(service, clock, 1);
    auto second = first;
    second.hour = (calendar.hour + 2) % 24;
    std::atomic<int> succeeded{0};
    std::thread a([&] { if (Ok(ExecuteWatchAlarmTool(service, first, clock, true))) ++succeeded; });
    std::thread b([&] { if (Ok(ExecuteWatchAlarmTool(service, second, clock, true))) ++succeeded; });
    a.join(); b.join();
    assert(succeeded == 1);
}
int main() {
    TestAlarmPersistenceAndGuards();
    TestAlarmFiringAndConcurrentUpdates();
    Storage storage;
    WatchTimeService service(storage);
    WatchClockReading clock{1000, 1, 1789293600000ULL};
    assert(!Ok(ExecuteWatchTimeTool(service, WatchTimeTool::kStatus, clock, true)));
    assert(service.Initialize(clock));
    assert(Ok(ExecuteWatchTimeTool(service, WatchTimeTool::kStatus, clock, true)));
    const auto writes = storage.writes;
    for (int duration : {-1, 0, 604801})
        assert(!Ok(ExecuteWatchTimeTool(service, WatchTimeTool::kStartTimer, clock, true, duration)));
    assert(!Ok(ExecuteWatchTimeTool(service, WatchTimeTool::kStartTimer, clock, false, 30)));
    assert(storage.writes == writes);
    assert(Ok(ExecuteWatchTimeTool(service, WatchTimeTool::kStartTimer, clock, true, 30)));
    assert(service.Snapshot(clock).state.checkpoint.timer_remaining_ms == 30000);
    WatchTimeService reboot(storage);
    clock.rtc_ms += 1000;
    assert(reboot.Initialize(clock));
    assert(reboot.Snapshot(clock).state.checkpoint.timer_remaining_ms == 29000);
    storage.fail = true;
    assert(!Ok(ExecuteWatchTimeTool(service, WatchTimeTool::kCancelTimer, clock, true)));
    assert(service.Snapshot(clock).state.checkpoint.timer_running);
    storage.fail = false;
    assert(Ok(ExecuteWatchTimeTool(service, WatchTimeTool::kCancelTimer, clock, true)));
    assert(!service.Snapshot(clock).state.checkpoint.timer_configured);
    uint64_t wake_delay = 0;
    assert(service.PrepareForSleep(clock, wake_delay));
    assert(!Ok(ExecuteWatchTimeTool(service, WatchTimeTool::kStartTimer, clock, true, 1)));
    std::puts("Watch MCP: timer, alarms, stale state, concurrent writes, firing, persistence and sleep checks passed");
}
