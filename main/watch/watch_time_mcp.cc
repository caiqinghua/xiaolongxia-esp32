#include "watch_time_mcp.h"
#include "watch_calendar.h"
#include <cJSON.h>
#include <memory>

std::string ExecuteWatchTimeTool(WatchTimeService& service, WatchTimeTool tool,
    const WatchClockReading& clock, bool allowed, int seconds) {
    if (!allowed) return R"({"ok":false,"error":"Watch resources are unavailable"})";
    const auto before = service.Snapshot(clock);
    if (!before.initialized || before.sleep_prepared)
        return R"({"ok":false,"error":"Watch time service is unavailable"})";
    bool succeeded = false;
    switch (tool) {
        case WatchTimeTool::kStatus: succeeded = true; break;
        case WatchTimeTool::kStartTimer:
            if (seconds < 1 || seconds > 604800)
                return R"({"ok":false,"error":"Timer duration is outside its allowed range"})";
            succeeded = service.Execute(WatchTimeCommand::kStartTimer, clock, uint64_t(seconds) * 1000);
            break;
        case WatchTimeTool::kCancelTimer:
            succeeded = service.Execute(WatchTimeCommand::kCancelTimer, clock);
            break;
        default: return R"({"ok":false,"error":"Unknown watch time tool"})";
    }
    if (!succeeded) return R"({"ok":false,"error":"Watch action was not saved; do not claim success"})";
    const auto state = service.Snapshot(clock);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> result(cJSON_CreateObject(), cJSON_Delete);
    if (!result) return R"({"ok":false,"error":"Watch result allocation failed"})";
    cJSON_AddBoolToObject(result.get(), "ok", true);
    cJSON_AddBoolToObject(result.get(), "storage_error", state.storage_error);
    cJSON_AddBoolToObject(result.get(), "clock_valid", WatchCalendarAt(clock.unix_ms, state.state.timezone).valid);
    cJSON_AddNumberToObject(result.get(), "unix_ms", clock.unix_ms);
    cJSON_AddStringToObject(result.get(), "timezone", state.state.timezone.c_str());
    cJSON_AddBoolToObject(result.get(), "timer_running", state.state.checkpoint.timer_running);
    cJSON_AddBoolToObject(result.get(), "timer_configured", state.state.checkpoint.timer_configured);
    cJSON_AddBoolToObject(result.get(), "timer_alert", state.state.checkpoint.timer_alert);
    cJSON_AddNumberToObject(result.get(), "timer_remaining_ms", state.state.checkpoint.timer_remaining_ms);
    auto* alarms = cJSON_AddArrayToObject(result.get(), "alarms");
    if (!alarms) return R"({"ok":false,"error":"Watch result allocation failed"})";
    for (size_t index = 0; index < state.state.checkpoint.alarms.size(); ++index) {
        const auto& alarm = state.state.checkpoint.alarms[index];
        auto* entry = cJSON_CreateObject();
        if (!entry) return R"({"ok":false,"error":"Watch result allocation failed"})";
        cJSON_AddItemToArray(alarms, entry);
        if (!cJSON_AddNumberToObject(entry, "index", index) ||
            !cJSON_AddNumberToObject(entry, "hour", alarm.hour) ||
            !cJSON_AddNumberToObject(entry, "minute", alarm.minute) ||
            !cJSON_AddNumberToObject(entry, "weekdays", alarm.weekdays) ||
            !cJSON_AddBoolToObject(entry, "enabled", alarm.enabled) ||
            !cJSON_AddStringToObject(entry, "state_token", WatchAlarmStateToken(state.state, index).c_str()))
            return R"({"ok":false,"error":"Watch result allocation failed"})";
    }
    std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(result.get()), cJSON_free);
    return text ? text.get() : R"({"ok":false,"error":"Watch result allocation failed"})";
}

std::string ExecuteWatchAlarmTool(WatchTimeService& service, const WatchAlarmToolRequest& request,
    const WatchClockReading& clock, bool allowed) {
    if (!allowed) return R"({"ok":false,"error":"Watch resources are unavailable"})";
    if (request.index < 0 || request.index >= 5 || request.hour < 0 || request.hour > 23 ||
        request.minute < 0 || request.minute > 59 || request.weekdays < 0 || request.weekdays > 127 ||
        request.expected_state.empty() || request.expected_state.size() > 256)
        return R"({"ok":false,"error":"Invalid alarm arguments"})";
    WatchAlarm alarm;
    alarm.hour = request.hour;
    alarm.minute = request.minute;
    alarm.weekdays = request.weekdays;
    alarm.enabled = request.enabled;
    if (!service.SetAlarmChecked(request.index, alarm, request.expected_state, clock))
        return R"({"ok":false,"error":"Alarm was not saved. Read watch status again; check clock, state and storage. Do not claim success."})";
    return ExecuteWatchTimeTool(service, WatchTimeTool::kStatus, clock, true);
}
