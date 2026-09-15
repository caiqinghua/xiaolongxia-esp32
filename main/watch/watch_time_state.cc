#include "watch_time_state.h"

#include <cJSON.h>
#include <charconv>
#include <cmath>
#include <cstring>
#include <memory>

namespace {
constexpr WatchTimeZone kTimeZones[] = {
    {"UTC", "UTC0"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Kathmandu", "NPT-5:45"},
    {"Asia/Dubai", "GST-4"},
    {"Asia/Singapore", "SGT-8"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"America/New_York", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Chicago", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"America/Denver", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"America/Phoenix", "MST7"},
    {"Pacific/Honolulu", "HST10"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
    {"Australia/Perth", "AWST-8"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0/2,M4.1.0/3"},
};
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

bool Number(const cJSON* parent, const char* key, int minimum, int maximum, int& value) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < minimum || item->valuedouble > maximum ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    value = item->valueint;
    return true;
}

bool Boolean(const cJSON* parent, const char* key, bool& value) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsBool(item)) { return false; }
    value = cJSON_IsTrue(item);
    return true;
}

bool UnsignedValue(const cJSON* item, uint64_t& value) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr) { return false; }
    const char* first = item->valuestring;
    const char* last = first + std::strlen(first);
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

bool Unsigned(const cJSON* parent, const char* key, uint64_t& value) {
    return UnsignedValue(cJSON_GetObjectItemCaseSensitive(parent, key), value);
}

bool AddUnsigned(cJSON* parent, const char* key, uint64_t value) {
    return cJSON_AddStringToObject(parent, key, std::to_string(value).c_str()) != nullptr;
}
}  // namespace

const WatchTimeZone* WatchTimeZones(size_t& count) {
    count = sizeof(kTimeZones) / sizeof(kTimeZones[0]);
    return kTimeZones;
}

const WatchTimeZone* FindWatchTimeZone(std::string_view id) {
    for (const auto& zone : kTimeZones) {
        if (zone.id == id) { return &zone; }
    }
    return nullptr;
}

std::string EncodeWatchTimeState(const WatchTimeState& state) {
    WatchTimeTools validator;
    const auto& value = state.checkpoint;
    if (!FindWatchTimeZone(state.timezone) || state.alarm_alerts > 31 ||
        !validator.Restore(value, value.saved_rtc_ms, value.rtc_epoch)) {
        return {};
    }
    Json root(cJSON_CreateObject(), &cJSON_Delete);
    if (!root || !cJSON_AddNumberToObject(root.get(), "version", 1) ||
        !cJSON_AddNumberToObject(root.get(), "alarm_alerts", state.alarm_alerts) ||
        !cJSON_AddStringToObject(root.get(), "timezone", state.timezone.c_str()) ||
        !cJSON_AddBoolToObject(root.get(), "hour24", state.hour24) ||
        !AddUnsigned(root.get(), "rtc_epoch", value.rtc_epoch) ||
        !AddUnsigned(root.get(), "saved_rtc_ms", value.saved_rtc_ms) ||
        !AddUnsigned(root.get(), "stopwatch_ms", value.stopwatch_elapsed_ms) ||
        !AddUnsigned(root.get(), "timer_ms", value.timer_remaining_ms) ||
        !cJSON_AddBoolToObject(root.get(), "stopwatch_running", value.stopwatch_running) ||
        !cJSON_AddBoolToObject(root.get(), "timer_running", value.timer_running) ||
        !cJSON_AddBoolToObject(root.get(), "timer_configured", value.timer_configured) ||
        !cJSON_AddBoolToObject(root.get(), "timer_alert", value.timer_alert)) {
        return {};
    }
    auto* laps = cJSON_AddArrayToObject(root.get(), "laps");
    if (!laps) { return {}; }
    for (const uint64_t lap : value.laps) {
        Json item(cJSON_CreateString(std::to_string(lap).c_str()), &cJSON_Delete);
        if (!item || !cJSON_AddItemToArray(laps, item.get())) { return {}; }
        item.release();
    }
    auto* alarms = cJSON_AddArrayToObject(root.get(), "alarms");
    if (!alarms) { return {}; }
    for (const auto& alarm : value.alarms) {
        Json item(cJSON_CreateObject(), &cJSON_Delete);
        if (!item || !cJSON_AddNumberToObject(item.get(), "hour", alarm.hour) ||
            !cJSON_AddNumberToObject(item.get(), "minute", alarm.minute) ||
            !cJSON_AddNumberToObject(item.get(), "weekdays", alarm.weekdays) ||
            !cJSON_AddBoolToObject(item.get(), "enabled", alarm.enabled) ||
            !cJSON_AddNumberToObject(item.get(), "last_day", alarm.last_fired_day) ||
            !cJSON_AddItemToArray(alarms, item.get())) {
            return {};
        }
        item.release();
    }
    char* text = cJSON_PrintUnformatted(root.get());
    if (!text) { return {}; }
    const std::string result(text);
    cJSON_free(text);
    return result;
}

bool DecodeWatchTimeState(std::string_view json, WatchTimeState& state) {
    if (json.empty() || json.size() > 4096 || json.find('\0') != std::string_view::npos) { return false; }
    const std::string terminated(json);
    Json root(cJSON_ParseWithLengthOpts(terminated.c_str(), terminated.size() + 1, nullptr, true), &cJSON_Delete);
    if (!cJSON_IsObject(root.get())) { return false; }
    WatchTimeState decoded;
    auto& value = decoded.checkpoint;
    const auto* zone = cJSON_GetObjectItemCaseSensitive(root.get(), "timezone");
    int version = 0;
    int alarm_alerts = 0;
    if (!Number(root.get(), "version", 1, 1, version) || !cJSON_IsString(zone) ||
        !Number(root.get(), "alarm_alerts", 0, 31, alarm_alerts) ||
        !zone->valuestring || !FindWatchTimeZone(zone->valuestring) ||
        !Boolean(root.get(), "hour24", decoded.hour24) ||
        !Unsigned(root.get(), "rtc_epoch", value.rtc_epoch) ||
        !Unsigned(root.get(), "saved_rtc_ms", value.saved_rtc_ms) ||
        !Unsigned(root.get(), "stopwatch_ms", value.stopwatch_elapsed_ms) ||
        !Unsigned(root.get(), "timer_ms", value.timer_remaining_ms) ||
        !Boolean(root.get(), "stopwatch_running", value.stopwatch_running) ||
        !Boolean(root.get(), "timer_running", value.timer_running) ||
        !Boolean(root.get(), "timer_configured", value.timer_configured) ||
        !Boolean(root.get(), "timer_alert", value.timer_alert)) {
        return false;
    }
    decoded.timezone = zone->valuestring;
    decoded.alarm_alerts = static_cast<uint8_t>(alarm_alerts);
    const auto* laps = cJSON_GetObjectItemCaseSensitive(root.get(), "laps");
    if (!cJSON_IsArray(laps) || cJSON_GetArraySize(laps) > static_cast<int>(WatchTimeTools::kMaximumLaps)) { return false; }
    const cJSON* lap = nullptr;
    cJSON_ArrayForEach(lap, laps) {
        uint64_t duration;
        if (!UnsignedValue(lap, duration)) { return false; }
        value.laps.push_back(duration);
    }
    const auto* alarms = cJSON_GetObjectItemCaseSensitive(root.get(), "alarms");
    if (!cJSON_IsArray(alarms) || cJSON_GetArraySize(alarms) != static_cast<int>(value.alarms.size())) { return false; }
    for (size_t index = 0; index < value.alarms.size(); ++index) {
        const auto* item = cJSON_GetArrayItem(alarms, static_cast<int>(index));
        auto& alarm = value.alarms[index];
        int hour, minute, weekdays, last_day;
        if (!cJSON_IsObject(item) || !Number(item, "hour", 0, 23, hour) ||
            !Number(item, "minute", 0, 59, minute) || !Number(item, "weekdays", 0, 127, weekdays) ||
            !Number(item, "last_day", -1, 999999, last_day) || !Boolean(item, "enabled", alarm.enabled)) {
            return false;
        }
        alarm.hour = static_cast<uint8_t>(hour);
        alarm.minute = static_cast<uint8_t>(minute);
        alarm.weekdays = static_cast<uint8_t>(weekdays);
        alarm.last_fired_day = last_day;
    }
    WatchTimeTools validator;
    if (!validator.Restore(value, value.saved_rtc_ms, value.rtc_epoch)) { return false; }
    state = std::move(decoded);
    return true;
}
