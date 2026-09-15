#include "watch_view.h"
#include "watch_calendar.h"
#include "assets/lang_config.h"
#include <font_awesome.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {
constexpr uint32_t kBackground = 0x090B0C;
constexpr uint32_t kSurface = 0x202427;
constexpr uint32_t kText = 0xF4F7F6;
constexpr uint32_t kMuted = 0xA5AFB4;
constexpr uint32_t kAccent = 0x70DDC0;
constexpr uint32_t kWarning = 0xF6B35C;
bool IsFirmwareAction(WatchViewAction action) {
    return action == WatchViewAction::kCheckFirmware || action == WatchViewAction::kCancelFirmware ||
        action == WatchViewAction::kInstallFirmware || action == WatchViewAction::kRebootFirmware ||
        action == WatchViewAction::kSelectUpdateTarget;
}
bool IsFirmwarePage(WatchPage page) {
    return page == WatchPage::kFirmwareUpdate || page == WatchPage::kFirmwareConfirm;
}
bool IsPairingAction(WatchViewAction action) {
    return action == WatchViewAction::kOpenDevicePairing || action == WatchViewAction::kCancelDevicePairing ||
        action == WatchViewAction::kConfirmDevicePairingPeer || action == WatchViewAction::kApproveDevicePairingAccount ||
        action == WatchViewAction::kOpenBindingRemoval || action == WatchViewAction::kConfirmBindingRemoval ||
        action == WatchViewAction::kOpenControlRemoval || action == WatchViewAction::kConfirmControlRemoval ||
        action == WatchViewAction::kOpenSavedControlPeers || action == WatchViewAction::kSelectSavedControlPeer ||
        action == WatchViewAction::kSelectAllSavedControlPeers || action == WatchViewAction::kConfirmAllControlRemoval;
}
bool SamePairing(const WatchPairingSnapshot& a, const WatchPairingSnapshot& b) {
    return a.available == b.available && a.active == b.active && a.can_open == b.can_open &&
        a.network_required == b.network_required && a.can_remove_binding == b.can_remove_binding &&
        a.can_authorize_cloud == b.can_authorize_cloud && a.forget_peer_generation == b.forget_peer_generation &&
        a.can_inspect_saved_peers == b.can_inspect_saved_peers &&
        a.state.removing_saved_control == b.state.removing_saved_control && a.state.saved_peers == b.state.saved_peers &&
        a.confirmation == b.confirmation && a.passkey == b.passkey && a.compare_number == b.compare_number &&
        a.state.phase == b.state.phase && a.state.error == b.state.error && a.state.attempt == b.state.attempt &&
        a.state.control_only == b.state.control_only && a.state.control_peer_generation == b.state.control_peer_generation &&
        a.state.removing_binding == b.state.removing_binding && a.state.removing_control == b.state.removing_control &&
        a.state.removing_all_control == b.state.removing_all_control &&
        a.state.binding_generation == b.state.binding_generation && a.state.device_id == b.state.device_id &&
        a.state.offer.request == b.state.offer.request && a.state.offer.user_id == b.state.offer.user_id &&
        a.state.offer.device_id == b.state.offer.device_id && a.state.offer.challenge_id == b.state.offer.challenge_id;
}
bool IsNotificationAction(WatchViewAction action) {
    return action == WatchViewAction::kSetNotificationsEnabled || action == WatchViewAction::kConfirmNotificationPeer ||
        action == WatchViewAction::kOpenNotification || action == WatchViewAction::kClearNotifications;
}
bool SameNotifications(const WatchNotificationSnapshot& a, const WatchNotificationSnapshot& b) {
    if (a.available != b.available || a.save_failed != b.save_failed || a.client.enabled != b.client.enabled ||
        a.client.state != b.client.state || a.client.confirmation != b.client.confirmation || a.client.passkey != b.client.passkey ||
        a.client.android_peer != b.client.android_peer || a.client.link_generation != b.client.link_generation ||
        a.client.device_pairing_peer != b.client.device_pairing_peer ||
        a.client.control_pairing_peer != b.client.control_pairing_peer ||
        a.content.source != b.content.source || a.content.session != b.content.session ||
        a.content.ready != b.content.ready) return false;
    const auto& x = a.content.items;
    const auto& y = b.content.items;
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i) {
        if (x[i].uid != y[i].uid || x[i].revision != y[i].revision || x[i].details_ready != y[i].details_ready ||
            x[i].read != y[i].read || x[i].app_name != y[i].app_name || x[i].app_identifier != y[i].app_identifier ||
            x[i].title != y[i].title || x[i].message != y[i].message || x[i].date != y[i].date ||
            x[i].posted_at_ms != y[i].posted_at_ms) return false;
    }
    return true;
}
bool IsNotificationPage(WatchPage page) {
    return page == WatchPage::kNotifications || page == WatchPage::kNotificationDetail;
}
std::string NotificationDate(const std::string& value) {
    if (value.size() != 15 || value[8] != 'T') return value;
    for (size_t i = 0; i < value.size(); ++i)
        if (i != 8 && (value[i] < '0' || value[i] > '9')) return value;
    // ANCS supplies phone-local wall time without a UTC offset.
    return value.substr(0, 4) + "-" + value.substr(4, 2) + "-" + value.substr(6, 2) + " " +
        value.substr(9, 2) + ":" + value.substr(11, 2) + ":" + value.substr(13, 2);
}
const char* NotificationStatus(WatchAncsLinkState state) {
    using namespace Lang::Strings;
    switch (state) {
        case WatchAncsLinkState::kOffline: return WATCH_NOTIFY_OFFLINE;
        case WatchAncsLinkState::kDisabled: return WATCH_NOTIFY_DISABLED;
        case WatchAncsLinkState::kPeerConsent: return WATCH_NOTIFY_PEER_CONSENT;
        case WatchAncsLinkState::kConfirmNumber: return WATCH_NOTIFY_COMPARE;
        case WatchAncsLinkState::kReady: return WATCH_NOTIFY_READY;
        case WatchAncsLinkState::kUnavailable: return WATCH_NOTIFY_UNAVAILABLE;
        case WatchAncsLinkState::kDevicePairingReady:
        case WatchAncsLinkState::kControlReady: return WATCH_NOTIFY_UNAVAILABLE;
        case WatchAncsLinkState::kFailed: return WATCH_NOTIFY_FAILED;
        default: return WATCH_NOTIFY_CONNECTING;
    }
}
bool IsLibraryLoad(WatchViewAction action) {
    return action == WatchViewAction::kLoadRecordings || action == WatchViewAction::kLoadOlderRecordings ||
        action == WatchViewAction::kLoadNewerRecordings;
}
bool IsAssistantAction(WatchViewAction action) {
    return action == WatchViewAction::kAssistantTalk || action == WatchViewAction::kAssistantSend ||
        action == WatchViewAction::kAssistantEnd || action == WatchViewAction::kAssistantClear ||
        action == WatchViewAction::kAssistantPress || action == WatchViewAction::kAssistantRelease ||
        action == WatchViewAction::kAssistantCancelPress || action == WatchViewAction::kPrepareAssistantNetwork;
}
const char* TimeZoneLabel(const std::string& id) {
    using namespace Lang::Strings;
    static const struct { const char* id; const char* label; } labels[] = {
        {"UTC", "UTC"}, {"Asia/Shanghai", WATCH_ZONE_SHANGHAI},
        {"Asia/Tokyo", WATCH_ZONE_TOKYO}, {"Asia/Kolkata", WATCH_ZONE_KOLKATA},
        {"Asia/Kathmandu", WATCH_ZONE_KATHMANDU}, {"Asia/Dubai", WATCH_ZONE_DUBAI},
        {"Asia/Singapore", WATCH_ZONE_SINGAPORE}, {"Europe/London", WATCH_ZONE_LONDON},
        {"Europe/Berlin", WATCH_ZONE_BERLIN}, {"America/New_York", WATCH_ZONE_NEW_YORK},
        {"America/Chicago", WATCH_ZONE_CHICAGO}, {"America/Denver", WATCH_ZONE_DENVER},
        {"America/Los_Angeles", WATCH_ZONE_LOS_ANGELES}, {"America/Phoenix", WATCH_ZONE_PHOENIX},
        {"Pacific/Honolulu", WATCH_ZONE_HONOLULU}, {"Australia/Sydney", WATCH_ZONE_SYDNEY},
        {"Australia/Perth", WATCH_ZONE_PERTH}, {"Pacific/Auckland", WATCH_ZONE_AUCKLAND},
    };
    for (const auto& entry : labels) { if (id == entry.id) { return entry.label; } }
    return nullptr;
}
std::string Duration(uint64_t milliseconds, bool fractions = false) {
    const uint64_t seconds = milliseconds / 1000;
    char text[48];
    std::snprintf(text, sizeof(text), "%02llu:%02u:%02u",
        static_cast<unsigned long long>(seconds / 3600),
        static_cast<unsigned>((seconds / 60) % 60), static_cast<unsigned>(seconds % 60));
    std::string result(text);
    if (fractions) {
        std::snprintf(text, sizeof(text), ".%02u", static_cast<unsigned>((milliseconds % 1000) / 10));
        result += text;
    }
    return result;
}
void Plain(lv_obj_t* object) {
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
}
const char* PageTitle(WatchPage page) {
    using namespace Lang::Strings;
    switch (page) {
        case WatchPage::kFace: return WATCH_TITLE;
        case WatchPage::kApps: return WATCH_APPS;
        case WatchPage::kTimer: return WATCH_TIMER;
        case WatchPage::kStopwatch: return WATCH_STOPWATCH;
        case WatchPage::kAlarms: return WATCH_ALARMS;
        case WatchPage::kAlarmEditor: return WATCH_EDIT_ALARM;
        case WatchPage::kSettings: return WATCH_TIME_SETTINGS;
        case WatchPage::kControls: return WATCH_CONTROLS;
        case WatchPage::kPowerSettings: return WATCH_POWER_MODE;
        case WatchPage::kWristSettings: return WATCH_WRIST_WAKE;
        case WatchPage::kActivity: return WATCH_ACTIVITY;
        case WatchPage::kActivityHistory: return WATCH_ACTIVITY_HISTORY;
        case WatchPage::kActivityResetConfirm: return WATCH_ACTIVITY_RESET;
        case WatchPage::kAssistant: return WATCH_ASSISTANT;
        case WatchPage::kRecorder: return WATCH_RECORDER;
        case WatchPage::kRecordings: return WATCH_RECENT_RECORDINGS;
        case WatchPage::kRecordingDetail: return WATCH_RECORDING_DETAILS;
        case WatchPage::kPlayback: return WATCH_PLAYBACK;
        case WatchPage::kRecordingDeleteConfirm: return WATCH_RECORDING_DELETE;
        case WatchPage::kNotifications: return WATCH_NOTIFICATIONS;
        case WatchPage::kDevicePairing: return WATCH_PAIR_DEVICE;
        case WatchPage::kFirmwareUpdate:
        case WatchPage::kFirmwareConfirm: return WATCH_FIRMWARE;
        case WatchPage::kNotificationDetail: return WATCH_NOTIFY_DETAIL;
    }
    return WATCH_TITLE;
}
}

WatchView::WatchView(lv_obj_t* parent, const lv_font_t* text_font, const lv_font_t* digits_font,
    const lv_font_t* icon_font, RequestHandler handler, std::function<void()> activity)
    : text_font_(text_font), digits_font_(digits_font), icon_font_(icon_font), handler_(std::move(handler)),
      activity_(std::move(activity)) {
    icons_with_text_ = *icon_font;
    icons_with_text_.fallback = text_font;
    icon_font_ = &icons_with_text_;
    root_ = lv_obj_create(parent);
    Plain(root_);
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(root_, lv_color_hex(kText), 0);
    lv_obj_set_style_text_font(root_, text_font_, 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    render_timer_ = lv_timer_create([](lv_timer_t* timer) {
        auto* self = static_cast<WatchView*>(lv_timer_get_user_data(timer));
        if (self->visible_ && self->dirty_) { self->Render(); }
        if (self->visible_ && self->touch_locked_) self->RefreshTouchLock();
        if (self->visible_) self->RefreshPixelShift();
    }, 40, this);
    BuildTouchLock(parent);
    Render();
}

WatchView::~WatchView() {
    AssistantRelease(true);
    if (render_timer_) { lv_timer_delete(render_timer_); }
    if (lock_root_) { lv_obj_delete(lock_root_); }
    if (root_) { lv_obj_delete(root_); }
}

void WatchView::Bind(lv_obj_t* object, lv_event_code_t event, std::function<void(lv_event_t*)> callback) {
    bindings_.push_back({this, std::move(callback)});
    lv_obj_add_event_cb(object, [](lv_event_t* value) {
        auto* binding = static_cast<Binding*>(lv_event_get_user_data(value));
        if (!binding->owner->visible_ || binding->owner->touch_locked_) return;
        if (binding->owner->activity_) { binding->owner->activity_(); }
        binding->callback(value);
    }, event, &bindings_.back());
}

void WatchView::BuildTouchLock(lv_obj_t* parent) {
    lock_root_ = lv_obj_create(parent);
    Plain(lock_root_);
    lv_obj_set_size(lock_root_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(lock_root_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(lock_root_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(lock_root_, text_font_, 0);
    lv_obj_set_style_pad_all(lock_root_, 24, 0);
    lv_obj_set_style_pad_row(lock_root_, 20, 0);
    lv_obj_set_flex_flow(lock_root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lock_root_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(lock_root_, LV_OBJ_FLAG_SCROLLABLE);
    for (auto* label : {Label(lock_root_, FONT_AWESOME_LOCK, icon_font_),
                       Label(lock_root_, Lang::Strings::WATCH_TOUCH_LOCKED)}) {
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    lock_status_ = Label(lock_root_, "");
    lv_obj_set_height(lock_status_, 48);
    lv_obj_set_style_text_align(lock_status_, LV_TEXT_ALIGN_CENTER, 0);
    auto* unlock = lv_button_create(lock_root_);
    lv_obj_remove_flag(unlock, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_size(unlock, LV_PCT(100), 96);
    lv_obj_set_style_radius(unlock, 8, 0);
    lv_obj_set_style_bg_color(unlock, lv_color_hex(kSurface), 0);
    lv_obj_set_style_shadow_width(unlock, 0, 0);
    auto* label = Label(unlock, Lang::Strings::WATCH_TOUCH_UNLOCK);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    unlock_progress_ = lv_bar_create(lock_root_);
    lv_obj_set_size(unlock_progress_, LV_PCT(100), 8);
    lv_bar_set_range(unlock_progress_, 0, 2000);
    lv_obj_set_style_bg_color(unlock_progress_, lv_color_hex(kAccent), LV_PART_INDICATOR);
    // Unlock only on release to avoid passing a held touch through to the page.
    lv_obj_add_event_cb(unlock, [](lv_event_t* event) {
        auto* self = static_cast<WatchView*>(lv_event_get_user_data(event));
        const auto code = lv_event_get_code(event);
        if (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
        if (!self->touch_locked_ || !self->visible_ || self->state_.time.sleep_prepared) {
            self->unlock_pressed_ = false;
            return;
        }
        if (code == LV_EVENT_PRESSED && !self->unlock_pressed_) {
            self->unlock_pressed_ = true;
            self->unlock_started_ = lv_tick_get();
            if (self->activity_) self->activity_();
        } else if (code == LV_EVENT_RELEASED) {
            const bool complete = self->unlock_pressed_ && lv_tick_elaps(self->unlock_started_) >= 2000;
            self->unlock_pressed_ = false;
            if (complete) {
                self->SetTouchLocked(false);
                if (self->activity_) self->activity_();
            }
        } else if (code == LV_EVENT_PRESS_LOST) {
            self->unlock_pressed_ = false;
        }
        self->RefreshTouchLock();
    }, LV_EVENT_ALL, this);
    lv_obj_add_flag(lock_root_, LV_OBJ_FLAG_HIDDEN);
}

void WatchView::SetTouchLocked(bool locked) {
    if (locked == touch_locked_) return;
    if (locked) {
        AssistantRelease(true);
        if (page_ == WatchPage::kDevicePairing) CancelPairing();
    }
    touch_locked_ = locked;
    unlock_pressed_ = false;
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lock_root_, LV_OBJ_FLAG_HIDDEN);
    if (visible_) lv_obj_remove_flag(Root(), LV_OBJ_FLAG_HIDDEN);
    RefreshTouchLock();
    dirty_ = true;
}

void WatchView::RefreshTouchLock() {
    if (state_.time.sleep_prepared) unlock_pressed_ = false;
    const auto elapsed = unlock_pressed_ ? std::min<uint32_t>(lv_tick_elaps(unlock_started_), 2000) : 0;
    if (lv_bar_get_value(unlock_progress_) != static_cast<int32_t>(elapsed))
        lv_bar_set_value(unlock_progress_, static_cast<int32_t>(elapsed), LV_ANIM_OFF);
    const char* status = state_.time.state.alarm_alerts ? Lang::Strings::WATCH_ALARM :
        state_.time.state.checkpoint.timer_alert ? Lang::Strings::WATCH_TIMER_FINISHED :
        state_.recording ? Lang::Strings::WATCH_RECORDING : "";
    if (std::strcmp(lv_label_get_text(lock_status_), status) != 0) lv_label_set_text(lock_status_, status);
}

void WatchView::RefreshPixelShift() {
    const auto minute = lv_tick_get() / 60000U;
    if (minute == pixel_shift_minute_) return;
    pixel_shift_minute_ = minute;
    // Move passive text within existing padding; never move controls or change
    // hit targets. Automatic screen-off remains the primary OLED protection.
    constexpr int8_t positions[][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}, {-2, 2},
                                      {-2, 0}, {-2, -2}, {0, -2}, {2, -2}};
    const auto& offset = positions[minute % (sizeof(positions) / sizeof(positions[0]))];
    const auto shift = [&](lv_obj_t* object) {
        if (!object) return;
        lv_obj_set_style_translate_x(object, offset[0], 0);
        lv_obj_set_style_translate_y(object, offset[1], 0);
    };
    if (rendered_page_ == WatchPage::kFace)
        for (auto* label : {digits_, detail_, connections_, battery_, record_}) shift(label);
    for (uint32_t index = 0; index < lv_obj_get_child_count(lock_root_); ++index) {
        auto* child = lv_obj_get_child(lock_root_, index);
        if (lv_obj_check_type(child, &lv_label_class)) shift(child);
    }
}

lv_obj_t* WatchView::Label(lv_obj_t* parent, const char* text, const lv_font_t* font) {
    auto* label = lv_label_create(parent);
    lv_label_set_recolor(label, false);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(kText), 0);
    if (font) { lv_obj_set_style_text_font(label, font, 0); }
    return label;
}

lv_obj_t* WatchView::Row(lv_obj_t* parent, int height) {
    auto* row = lv_obj_create(parent);
    Plain(row);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t* WatchView::Button(lv_obj_t* parent, const char* text, std::function<void()> callback, bool command, bool icon) {
    auto* button = lv_button_create(parent);
    lv_obj_set_size(button, icon ? 48 : LV_PCT(100), 48);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(kSurface), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_hor(button, 8, 0);
    lv_obj_set_style_pad_ver(button, 4, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x35453F), LV_STATE_PRESSED);
    auto* label = Label(button, text, icon ? icon_font_ : text_font_);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    Bind(button, LV_EVENT_CLICKED, [callback = std::move(callback)](lv_event_t*) { callback(); });
    if (command) { commands_.push_back(button); }
    return button;
}

uint32_t WatchView::NumberValue(lv_obj_t* picker) {
    return *static_cast<uint32_t*>(lv_obj_get_user_data(picker));
}

lv_obj_t* WatchView::NumberPicker(lv_obj_t* parent, unsigned maximum, unsigned selected) {
    auto* picker = lv_obj_create(parent);
    Plain(picker);
    lv_obj_set_size(picker, 64, 116);
    lv_obj_set_flex_grow(picker, 1);
    lv_obj_set_flex_flow(picker, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(picker, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(picker, LV_OBJ_FLAG_SCROLLABLE);
    numbers_.push_back(std::min(maximum, selected));
    auto* number = &numbers_.back();
    lv_obj_set_user_data(picker, number);
    auto* up = Button(picker, FONT_AWESOME_ANGLE_UP, []() {}, true, true);
    auto* label = Label(picker, "");
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    auto* down = Button(picker, FONT_AWESOME_ANGLE_DOWN, []() {}, true, true);
    const auto refresh = [number, label]() {
        char value[12]; std::snprintf(value, sizeof(value), "%02u", *number);
        lv_label_set_text(label, value);
    };
    for (auto* button : {up, down}) {
        lv_obj_set_size(button, LV_PCT(100), 40);
        lv_obj_set_style_pad_ver(button, 2, 0);
        const bool increase = button == up;
        const auto change = [number, maximum, increase, refresh](lv_event_t*) {
            if (increase && *number < maximum) { ++*number; }
            if (!increase && *number > 0) { --*number; }
            refresh();
        };
        Bind(button, LV_EVENT_SHORT_CLICKED, change);
        Bind(button, LV_EVENT_LONG_PRESSED_REPEAT, change);
    }
    refresh();
    return picker;
}

void WatchView::Navigate(WatchPage page) {
    const auto previous = page_;
    if (IsFirmwarePage(page_) && !IsFirmwarePage(page)) CancelFirmware();
    if (page == WatchPage::kFirmwareConfirm && page_ != page) firmware_confirmation_ = state_.firmware.update;
    if (page_ == WatchPage::kDevicePairing && page != page_) CancelPairing();
    if (page_ == WatchPage::kWristSettings && page != page_ &&
        (state_.wrist.pending || (pending_request_ && pending_action_ == WatchViewAction::kWristCommand))) {
        WristCommand(WatchWristAction::kCancel);
    }
    if (page_ == WatchPage::kRecordings && body_) recordings_scroll_y_ = lv_obj_get_scroll_y(body_);
    if ((page_ == WatchPage::kRecordings || page_ == WatchPage::kRecordingDetail) &&
        page != WatchPage::kRecordings && page != WatchPage::kRecordingDetail &&
        (state_.library.status == WatchLibraryStatus::kLoading ||
            (pending_request_ && IsLibraryLoad(pending_action_)))) {
        LibraryCommand(WatchViewAction::kCancelRecordings);
    }
    if (page_ == WatchPage::kAssistant && page != page_) {
        if (assistant_press_id_) AssistantRelease(true);
        else if (state_.assistant.can_end || pending_request_) AssistantCommand(WatchViewAction::kAssistantEnd);
    }
    if (page == WatchPage::kSettings && page_ != page) {
        timezone_draft_ = state_.time.state.timezone;
        hour24_draft_ = state_.time.state.hour24;
    }
    if (page == WatchPage::kPowerSettings && page_ != page) {
        power_draft_revision_ = state_.power_mode.revision;
        power_draft_binding_ = state_.power_mode.binding_generation;
        power_draft_ = state_.power_mode.power_mode;
    }
    if (page == WatchPage::kActivityResetConfirm && page_ != page) activity_reset_token_ = state_.motion.repair_token;
    if (page == WatchPage::kRecordingDeleteConfirm && page_ != page) {
        const auto entry = std::find_if(state_.library.entries.begin(), state_.library.entries.end(),
            [this](const auto& item) { return item.id == selected_recording_id_; });
        deletion_entry_ = entry == state_.library.entries.end() ? WatchRecordingEntry{} : *entry;
        deletion_revision_ = state_.library.revision;
        deletion_request_ = 0;
        if (entry == state_.library.entries.end() && state_.deletion.cleanup_available &&
            state_.deletion.recording_id == selected_recording_id_) {
            deletion_entry_.id = selected_recording_id_;
            deletion_entry_.file_name = selected_recording_id_ + ".opus.json";
            deletion_entry_.audio_available = false;
            deletion_request_ = state_.deletion.request;
        }
    }
    page_ = page;
    failed_ = false;
    dirty_ = true;
    if (page == WatchPage::kRecordings && previous != WatchPage::kRecordingDetail && previous != page && !pending_request_) {
        LibraryCommand(WatchViewAction::kLoadRecordings);
    }
}

void WatchView::Update(const WatchViewState& state) {
    if (state_.firmware.available != state.firmware.available && page_ == WatchPage::kSettings) dirty_ = true;
    if (state.time.sleep_prepared) unlock_pressed_ = false;
    const bool pairing_changed = !SamePairing(state_.pairing, state.pairing);
    const bool pairing_page = page_ == WatchPage::kDevicePairing || rendered_page_ == WatchPage::kDevicePairing;
    if (pairing_changed && pairing_page) dirty_ = true;
    if (state_.pairing.available != state.pairing.available && page_ == WatchPage::kControls) dirty_ = true;
    const bool notifications_changed = !SameNotifications(state_.notifications, state.notifications);
    if (page_ == WatchPage::kNotificationDetail && state_.time.state.timezone != state.time.state.timezone)
        dirty_ = true;
    if (notifications_changed && (IsNotificationPage(page_) || IsNotificationPage(rendered_page_) ||
        state_.notifications.client.confirmation != state.notifications.client.confirmation)) dirty_ = true;
    const bool sound_stopping = !state.time.state.alarm_alerts && !state.time.state.checkpoint.timer_alert && state.alert_sound.active;
    const bool was_sound_stopping = !state_.time.state.alarm_alerts && !state_.time.state.checkpoint.timer_alert && state_.alert_sound.active;
    if (sound_stopping != was_sound_stopping || state.alert_sound.cleanup_failed != state_.alert_sound.cleanup_failed) dirty_ = true;
    if (page_ == WatchPage::kRecordings && state.deletion.request != state_.deletion.request) dirty_ = true;
    if (state.library.revision != state_.library.revision) {
        recordings_scroll_y_ = 0;
        if (page_ == WatchPage::kRecordings || page_ == WatchPage::kRecordingDetail) dirty_ = true;
    }
    const auto& old = state_.time.state.checkpoint;
    const auto& next = state.time.state.checkpoint;
    if (old.timer_running != next.timer_running || old.timer_configured != next.timer_configured ||
        old.timer_alert != next.timer_alert || old.stopwatch_running != next.stopwatch_running ||
        old.laps != next.laps || state_.time.state.alarm_alerts != state.time.state.alarm_alerts) {
        dirty_ = true;
    }
    if (page_ == WatchPage::kAlarms) {
        for (size_t index = 0; index < old.alarms.size(); ++index) {
            const auto& a = old.alarms[index]; const auto& b = next.alarms[index];
            if (a.hour != b.hour || a.minute != b.minute || a.enabled != b.enabled || a.weekdays != b.weekdays) { dirty_ = true; }
        }
    }
    state_ = state;
    if (pending_request_ && pending_action_ == WatchViewAction::kSeekPlayback) {
        const auto& playback = state_.playback;
        if (playback.generation != pending_playback_generation_) Complete(pending_request_, false);
        else if (playback.seek.request == pending_request_ && playback.seek.status != PlaybackSeekStatus::kPending)
            Complete(pending_request_, playback.seek.status == PlaybackSeekStatus::kApplied);
    }
    if (pending_request_ && pending_action_ == WatchViewAction::kSetPlaybackVolume) {
        const auto& playback = state_.playback;
        if (playback.generation != pending_playback_generation_) {
            Complete(pending_request_, false);
        } else if (playback.volume.request == pending_request_ && playback.volume.status != PlaybackVolumeStatus::kPending) {
            Complete(pending_request_, playback.volume.status == PlaybackVolumeStatus::kApplied);
        }
    }
    if (pending_request_ && pending_action_ == WatchViewAction::kResetCorruptActivityStorage &&
        state_.motion.reset_request == pending_request_ && !state_.motion.reset_pending) {
        Complete(pending_request_, state_.motion.reset_succeeded);
    }
    if (pending_request_ && pending_action_ == WatchViewAction::kRetryActivityStorage &&
        state_.motion.retry_request == pending_request_ && !state_.motion.retry_pending) {
        Complete(pending_request_, state_.motion.retry_succeeded);
    }
    if (pending_request_ && pending_action_ == WatchViewAction::kWristCommand &&
        state_.wrist.request == pending_request_ && !state_.wrist.pending) Complete(pending_request_, state_.wrist.succeeded);
    // Purge retained LVGL text even while the display is hidden. A disconnected,
    // removed or revised notification must never reappear for one render tick.
    if ((notifications_changed && (IsNotificationPage(page_) || IsNotificationPage(rendered_page_))) ||
        (pairing_changed && pairing_page)) {
        const int scroll = !pairing_changed && rendered_page_ == page_ && body_ ? lv_obj_get_scroll_y(body_) : 0;
        Render();
        lv_obj_update_layout(body_);
        lv_obj_scroll_to_y(body_, scroll, LV_ANIM_OFF);
    } else if (visible_ && !dirty_) { RefreshLabels(); RefreshCommandState(); }
}

void WatchView::Complete(uint64_t request_id, bool succeeded) {
    if (request_id != pending_request_ || request_id == 0) { return; }
    pending_request_ = 0;
    failed_ = !succeeded;
    if (succeeded && pending_action_ == WatchViewAction::kSetAlarm && page_ == WatchPage::kAlarmEditor) {
        Navigate(WatchPage::kAlarms);
    } else if (succeeded && pending_action_ == WatchViewAction::kResetCorruptActivityStorage && page_ == WatchPage::kActivityResetConfirm) {
        Navigate(WatchPage::kActivity);
    } else if (succeeded && pending_action_ == WatchViewAction::kDeleteRecording && page_ == WatchPage::kRecordingDeleteConfirm) {
        Navigate(WatchPage::kRecordings);
    } else if (succeeded && pending_action_ == WatchViewAction::kPlayRecording && page_ == WatchPage::kRecordingDetail) {
        Navigate(WatchPage::kPlayback);
    } else if (visible_) {
        RefreshCommandState();
    }
}

void WatchView::SetVisible(bool visible) {
    visible_ = visible;
    unlock_pressed_ = false;
    if (visible) {
        lv_obj_remove_flag(Root(), LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(render_timer_);
        dirty_ = true;
    } else {
        AssistantRelease(true);
        if (page_ == WatchPage::kDevicePairing) CancelPairing();
        if (IsFirmwarePage(page_)) CancelFirmware();
        if (page_ == WatchPage::kWristSettings && (state_.wrist.pending ||
            (pending_request_ && pending_action_ == WatchViewAction::kWristCommand))) WristCommand(WatchWristAction::kCancel);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lock_root_, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(render_timer_);
    }
}

void WatchView::Submit(WatchViewRequest request) {
    const bool assistant = IsAssistantAction(request.action);
    const bool recording = request.action == WatchViewAction::kRecordingCommand;
    const bool library = IsLibraryLoad(request.action) || request.action == WatchViewAction::kCancelRecordings ||
        request.action == WatchViewAction::kDeleteRecording;
    const bool playback = request.action == WatchViewAction::kPlayRecording || request.action == WatchViewAction::kPausePlayback ||
        request.action == WatchViewAction::kResumePlayback || request.action == WatchViewAction::kStopPlayback ||
        request.action == WatchViewAction::kSeekPlayback;
    const bool level = request.action == WatchViewAction::kSetLevel || request.action == WatchViewAction::kSetPlaybackVolume;
    const bool voice = request.action == WatchViewAction::kSetVoiceEnabled;
    const bool wifi = request.action == WatchViewAction::kSetWifiEnabled;
    const bool power = request.action == WatchViewAction::kSetPowerMode;
    const bool notifications = IsNotificationAction(request.action);
    const bool pairing = IsPairingAction(request.action);
    const bool firmware = IsFirmwareAction(request.action);
    const bool activity = request.action == WatchViewAction::kRetryActivityStorage ||
        request.action == WatchViewAction::kResetCorruptActivityStorage || request.action == WatchViewAction::kWristCommand;
    const bool dismiss_alert = request.action == WatchViewAction::kTimeCommand && request.command == WatchTimeCommand::kDismissAlerts;
    const bool cancel = ((voice || wifi || request.action == WatchViewAction::kSetNotificationsEnabled ||
        request.action == WatchViewAction::kConfirmNotificationPeer) && !request.enabled) ||
        request.action == WatchViewAction::kClearNotifications || request.action == WatchViewAction::kAssistantEnd ||
        request.action == WatchViewAction::kCancelDevicePairing ||
        request.action == WatchViewAction::kCancelFirmware ||
        ((request.action == WatchViewAction::kConfirmDevicePairingPeer ||
          request.action == WatchViewAction::kApproveDevicePairingAccount ||
          request.action == WatchViewAction::kConfirmBindingRemoval || request.action == WatchViewAction::kConfirmControlRemoval) && !request.enabled) ||
        request.action == WatchViewAction::kAssistantRelease || request.action == WatchViewAction::kAssistantCancelPress ||
        request.action == WatchViewAction::kCancelRecordings ||
        request.action == WatchViewAction::kStopPlayback || dismiss_alert ||
        (recording && request.recording_command == WatchRecordingCommand::kStop) ||
        (request.action == WatchViewAction::kWristCommand && (request.wrist_action == WatchWristAction::kCancel ||
            (request.wrist_action == WatchWristAction::kSetEnabled && !request.enabled)));
    if ((pending_request_ && !cancel) || !handler_ || (!firmware && !pairing && !notifications && !dismiss_alert && !assistant && !recording && !library && !playback && !level && !voice && !wifi && !power && !activity && !state_.time.initialized) ||
        (state_.time.sleep_prepared && !cancel)) { return; }
    request.id = ++next_request_;
    if (!request.id) { request.id = ++next_request_; }
    pending_request_ = request.id;
    pending_action_ = request.action;
    if (request.action == WatchViewAction::kCheckFirmware) firmware_check_request_ = request.id;
    pending_playback_generation_ = request.playback_generation;
    if (request.action == WatchViewAction::kDeleteRecording) deletion_request_ = request.id;
    pending_recording_command_ = request.recording_command;
    pending_enabled_ = request.enabled;
    failed_ = false;
    RefreshCommandState();
    handler_(request);
}

void WatchView::Command(WatchTimeCommand command, uint64_t duration_ms) {
    WatchViewRequest request;
    request.command = command;
    request.duration_ms = duration_ms;
    Submit(request);
}

void WatchView::AssistantCommand(WatchViewAction action) {
    if (action == WatchViewAction::kAssistantEnd) {
        assistant_press_id_ = 0;
        assistant_click_handled_ = true;
    }
    WatchViewRequest request;
    request.action = action;
    Submit(request);
}

void WatchView::AssistantPress() {
    assistant_click_handled_ = false;
    if (assistant_press_id_ || pending_request_ || !handler_ || state_.time.sleep_prepared ||
        !(state_.assistant.can_talk || state_.assistant.can_interrupt)) return;
    assistant_press_id_ = WatchPushToTalk::NextToken();
    WatchViewRequest request;
    request.action = WatchViewAction::kAssistantPress;
    request.press_token = assistant_press_id_;
    Submit(request);
}

void WatchView::AssistantRelease(bool cancel) {
    if (!assistant_press_id_) return;
    WatchViewRequest request;
    request.action = cancel ? WatchViewAction::kAssistantCancelPress : WatchViewAction::kAssistantRelease;
    request.press_token = assistant_press_id_;
    assistant_press_id_ = 0;
    assistant_click_handled_ = true;
    Submit(request);
}

void WatchView::RecordingCommand(WatchRecordingCommand command) {
    const bool pending_start = pending_request_ && pending_action_ == WatchViewAction::kRecordingCommand &&
        pending_recording_command_ == WatchRecordingCommand::kStart;
    if (!state_.recorder.Can(command) && !(command == WatchRecordingCommand::kStop && pending_start)) return;
    if (pending_request_ && pending_action_ == WatchViewAction::kRecordingCommand &&
        pending_recording_command_ == WatchRecordingCommand::kStop) return;
    WatchViewRequest request;
    request.action = WatchViewAction::kRecordingCommand;
    request.recording_command = command;
    Submit(request);
}

void WatchView::LibraryCommand(WatchViewAction action) {
    WatchViewRequest request;
    request.action = action;
    request.library_revision = state_.library.revision;
    Submit(request);
}

void WatchView::CaptureDraft() {
    if (rendered_page_ != page_) { return; }
    if (page_ == WatchPage::kAlarmEditor && hour_ && minute_) {
        alarm_draft_.hour = static_cast<uint8_t>(NumberValue(hour_));
        alarm_draft_.minute = static_cast<uint8_t>(NumberValue(minute_));
        alarm_draft_.enabled = lv_obj_has_state(enabled_, LV_STATE_CHECKED);
        alarm_draft_.weekdays = 0;
        for (size_t day = 0; day < weekdays_.size(); ++day) {
            if (lv_obj_has_state(weekdays_[day], LV_STATE_CHECKED)) { alarm_draft_.weekdays |= 1U << day; }
        }
    } else if (page_ == WatchPage::kTimer && hour_ && minute_ && second_) {
        duration_draft_ = {NumberValue(hour_), NumberValue(minute_), NumberValue(second_)};
    } else if (page_ == WatchPage::kSettings && zone_ && format_) {
        size_t count = 0; const auto* zones = WatchTimeZones(count);
        const auto selected = lv_dropdown_get_selected(zone_);
        if (selected < count) { timezone_draft_ = zones[selected].id; }
        hour24_draft_ = lv_obj_has_state(format_, LV_STATE_CHECKED);
    }
}

void WatchView::Render() {
    // Replacing a pressed widget may suppress the input device's release event.
    AssistantRelease(true);
    CaptureDraft();
    rendered_page_ = page_;
    pixel_shift_minute_ = UINT32_MAX;
    dirty_ = false;
    lv_obj_clean(root_);
    bindings_.clear(); commands_.clear(); numbers_.clear();
    digits_ = detail_ = connections_ = battery_ = record_ = nullptr;
    hour_ = minute_ = second_ = enabled_ = zone_ = format_ = nullptr;
    assistant_status_ = assistant_history_ = assistant_primary_ = assistant_end_ = assistant_clear_ = nullptr;
    assistant_rendered_revision_ = UINT64_MAX;
    recorder_status_ = recorder_duration_ = recorder_detail_ = nullptr;
    recorder_primary_ = recorder_stop_ = recorder_highlight_ = nullptr;
    library_refresh_ = library_older_ = library_newer_ = nullptr;
    recording_delete_ = recording_delete_cancel_ = recording_delete_status_ = nullptr;
    recording_play_ = playback_status_ = playback_position_ = playback_name_ = playback_pause_ = playback_stop_ = nullptr;
    playback_volume_ = playback_volume_value_ = nullptr;
    playback_total_ = playback_seek_ = nullptr;
    activity_retry_ = activity_history_status_ = nullptr;
    wrist_switch_ = wrist_status_ = wrist_calibrate_ = wrist_cancel_ = nullptr;
    activity_reset_ = activity_reset_cancel_ = nullptr;
    activity_history_rows_.fill(nullptr);
    activity_history_dates_.fill(nullptr);
    activity_history_steps_.fill(nullptr);
    level_sliders_.fill(nullptr);
    level_values_.fill(nullptr);
    controls_status_ = nullptr;
    power_options_ = power_status_ = power_saved_ = power_apply_ = power_reload_ = nullptr;
    voice_switch_ = nullptr;
    wifi_switch_ = wifi_status_ = wifi_retry_ = nullptr;
    notification_switch_ = notification_accept_ = notification_reject_ = notification_clear_ = nullptr;
    pairing_open_ = pairing_remove_ = pairing_cloud_ = pairing_forget_ = pairing_accept_ = pairing_reject_ = pairing_cancel_ = nullptr;
    pairing_saved_ = pairing_saved_all_ = nullptr;
    firmware_status_ = firmware_version_ = firmware_progress_ = firmware_progress_text_ = nullptr;
    firmware_target_ = firmware_installed_ = nullptr;
    firmware_check_ = firmware_install_ = firmware_cancel_ = firmware_reboot_ = nullptr;
    pairing_saved_choices_.clear();
    notification_items_.clear();
    weekdays_.fill(nullptr);
    auto* header = Row(root_, 64);
    lv_obj_set_style_pad_hor(header, 16, 0);
    Button(header, page_ == WatchPage::kFace ? FONT_AWESOME_WATCH : FONT_AWESOME_ANGLE_LEFT, [this]() {
        if (page_ == WatchPage::kRecordingDeleteConfirm) {
            if (!pending_request_) Navigate(WatchPage::kRecordings);
            return;
        }
        Navigate(page_ == WatchPage::kFace ? WatchPage::kApps :
            page_ == WatchPage::kRecordingDetail ? WatchPage::kRecordings :
            page_ == WatchPage::kNotificationDetail ? WatchPage::kNotifications :
            page_ == WatchPage::kPowerSettings ? WatchPage::kControls :
            page_ == WatchPage::kDevicePairing ? WatchPage::kControls :
            page_ == WatchPage::kFirmwareUpdate ? WatchPage::kSettings :
            page_ == WatchPage::kFirmwareConfirm ? WatchPage::kFirmwareUpdate :
            (page_ == WatchPage::kActivityHistory || page_ == WatchPage::kActivityResetConfirm) ? WatchPage::kActivity :
            page_ == WatchPage::kAlarmEditor ? WatchPage::kAlarms : WatchPage::kFace);
    }, false, true);
    auto* title = Label(header, page_ == WatchPage::kRecordingDeleteConfirm && !deletion_entry_.audio_available
        ? Lang::Strings::WATCH_RECORDING_METADATA_CLEANUP : PageTitle(page_));
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, 1);
    auto* spacer = lv_obj_create(header);
    Plain(spacer); lv_obj_set_size(spacer, 48, 1);
    if (state_.notifications.available && !state_.notifications.client.device_pairing_peer &&
        !state_.notifications.client.control_pairing_peer &&
        state_.notifications.client.confirmation && !IsNotificationPage(page_)) {
        Button(root_, Lang::Strings::WATCH_NOTIFY_PAIR_REQUEST, [this]() { Navigate(WatchPage::kNotifications); });
    }
    if (state_.time.state.alarm_alerts || state_.time.state.checkpoint.timer_alert || state_.alert_sound.active) {
        auto* alert = Row(root_, 52);
        lv_obj_set_style_bg_color(alert, lv_color_hex(0x473319), 0);
        lv_obj_set_style_bg_opa(alert, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(alert, 16, 0);
        auto* label = Label(alert, state_.alert_sound.cleanup_failed ? Lang::Strings::WATCH_ALERT_CLEANUP_FAILED :
            !state_.time.state.alarm_alerts && !state_.time.state.checkpoint.timer_alert ? Lang::Strings::WATCH_ALERT_STOPPING :
            state_.time.state.checkpoint.timer_alert ? Lang::Strings::WATCH_TIMER_FINISHED : Lang::Strings::WATCH_ALARM);
        lv_obj_set_flex_grow(label, 1); lv_obj_set_width(label, 1);
        auto* dismiss = Button(alert, FONT_AWESOME_CHECK, [this]() { Command(WatchTimeCommand::kDismissAlerts); }, false, true);
        if (!handler_) lv_obj_add_state(dismiss, LV_STATE_DISABLED);
    }
    body_ = lv_obj_create(root_);
    Plain(body_);
    lv_obj_set_width(body_, LV_PCT(100));
    lv_obj_set_flex_grow(body_, 1);
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body_, 18, 0);
    lv_obj_set_style_pad_row(body_, 12, 0);
    lv_obj_set_scroll_dir(body_, LV_DIR_VER);
    Bind(body_, LV_EVENT_SCROLL_BEGIN, [](lv_event_t*) {});
    switch (page_) {
        case WatchPage::kFace: BuildFace(); break;
        case WatchPage::kApps: BuildApps(); break;
        case WatchPage::kTimer: BuildTimer(); break;
        case WatchPage::kStopwatch: BuildStopwatch(); break;
        case WatchPage::kAlarms: BuildAlarms(); break;
        case WatchPage::kAlarmEditor: BuildAlarmEditor(); break;
        case WatchPage::kSettings: BuildSettings(); break;
        case WatchPage::kControls: BuildControls(); break;
        case WatchPage::kPowerSettings: BuildPowerSettings(); break;
        case WatchPage::kWristSettings: BuildWristSettings(); break;
        case WatchPage::kActivity: BuildActivity(); break;
        case WatchPage::kActivityHistory: BuildActivityHistory(); break;
        case WatchPage::kActivityResetConfirm: BuildActivityResetConfirm(); break;
        case WatchPage::kAssistant: BuildAssistant(); break;
        case WatchPage::kRecorder: BuildRecorder(); break;
        case WatchPage::kRecordings: BuildRecordings(); break;
        case WatchPage::kRecordingDetail: BuildRecordingDetail(); break;
        case WatchPage::kPlayback: BuildPlayback(); break;
        case WatchPage::kRecordingDeleteConfirm: BuildRecordingDeleteConfirm(); break;
        case WatchPage::kNotifications: BuildNotifications(); break;
        case WatchPage::kDevicePairing: BuildDevicePairing(); break;
        case WatchPage::kFirmwareUpdate:
        case WatchPage::kFirmwareConfirm: BuildFirmwareUpdate(); break;
        case WatchPage::kNotificationDetail: BuildNotificationDetail(); break;
    }
    error_ = Label(root_, "");
    lv_obj_set_style_pad_hor(error_, 18, 0);
    lv_obj_set_style_text_color(error_, lv_color_hex(kWarning), 0);
    RefreshLabels(); RefreshCommandState();
    RefreshPixelShift();
    if (page_ == WatchPage::kRecordings) {
        lv_obj_update_layout(body_);
        lv_obj_scroll_to_y(body_, recordings_scroll_y_, LV_ANIM_OFF);
    }
}

void WatchView::RefreshCommandState() {
    const bool busy = pending_request_ || state_.time.sleep_prepared || !state_.time.initialized || !handler_;
    for (auto* object : commands_) {
        if (busy) { lv_obj_add_state(object, LV_STATE_DISABLED); }
        else { lv_obj_remove_state(object, LV_STATE_DISABLED); }
    }
    const bool runtime_request = IsFirmwareAction(pending_action_) || IsPairingAction(pending_action_) || IsAssistantAction(pending_action_) || pending_action_ == WatchViewAction::kRecordingCommand ||
        (IsNotificationAction(pending_action_) && pending_action_ != WatchViewAction::kSetNotificationsEnabled) ||
        IsLibraryLoad(pending_action_) || pending_action_ == WatchViewAction::kCancelRecordings ||
        pending_action_ == WatchViewAction::kDeleteRecording ||
        pending_action_ == WatchViewAction::kPlayRecording || pending_action_ == WatchViewAction::kPausePlayback ||
        pending_action_ == WatchViewAction::kResumePlayback || pending_action_ == WatchViewAction::kStopPlayback ||
        pending_action_ == WatchViewAction::kSeekPlayback ||
        pending_action_ == WatchViewAction::kSetWifiEnabled || pending_action_ == WatchViewAction::kRetryActivityStorage ||
        pending_action_ == WatchViewAction::kResetCorruptActivityStorage || pending_action_ == WatchViewAction::kWristCommand;
    const char* message = pending_request_ ? (runtime_request ? Lang::Strings::WATCH_WORKING : Lang::Strings::WATCH_SAVING) :
        failed_ && runtime_request ? Lang::Strings::WATCH_ACTION_FAILED :
        failed_ || state_.time.storage_error ? Lang::Strings::WATCH_SAVE_FAILED :
        state_.alert_sound.failed && (state_.time.state.alarm_alerts || state_.time.state.checkpoint.timer_alert || state_.alert_sound.active)
            ? Lang::Strings::WATCH_ALERT_SOUND_FAILED :
        state_.time.interrupted ? Lang::Strings::WATCH_TIMER_INTERRUPTED : "";
    lv_label_set_text(error_, message);
    if (*message) { lv_obj_remove_flag(error_, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(error_, LV_OBJ_FLAG_HIDDEN); }
    if (assistant_primary_) RefreshAssistant();
    if (recorder_primary_) RefreshRecorder();
    if (playback_status_) RefreshPlayback();
    if (recording_play_) {
        const bool current = state_.playback.resources_held && state_.playback.recording_id == selected_recording_id_;
        if (!handler_ || pending_request_ || state_.time.sleep_prepared || (!current && !state_.can_play_recording))
            lv_obj_add_state(recording_play_, LV_STATE_DISABLED);
        else lv_obj_remove_state(recording_play_, LV_STATE_DISABLED);
    }
    if (controls_status_) RefreshControls();
    if (power_status_) RefreshPowerSettings();
    if (wrist_status_) RefreshWristSettings();
    RefreshNotificationControls();
    RefreshPairingControls();
    if (firmware_status_) RefreshFirmwareUpdate();
    if (library_refresh_) {
        if (!handler_ || pending_request_ || state_.time.sleep_prepared || state_.library.status == WatchLibraryStatus::kLoading)
            lv_obj_add_state(library_refresh_, LV_STATE_DISABLED);
        else lv_obj_remove_state(library_refresh_, LV_STATE_DISABLED);
    }
    if (library_older_ && library_newer_) {
        const bool ready = handler_ && !pending_request_ && !state_.time.sleep_prepared &&
            state_.library.status == WatchLibraryStatus::kReady;
        for (auto* button : {library_newer_, library_older_}) {
            const bool available = button == library_newer_ ? state_.library.has_newer : state_.library.has_older;
            if (ready && available) lv_obj_remove_state(button, LV_STATE_DISABLED);
            else lv_obj_add_state(button, LV_STATE_DISABLED);
        }
    }
    if (recording_delete_) RefreshRecordingDelete();
    if (activity_retry_) {
        if (!handler_ || pending_request_ || state_.motion.retry_pending || state_.motion.reset_pending || state_.time.sleep_prepared)
            lv_obj_add_state(activity_retry_, LV_STATE_DISABLED);
        else lv_obj_remove_state(activity_retry_, LV_STATE_DISABLED);
    }
    if (activity_reset_) {
        const bool current = state_.motion.repair_token && !state_.motion.initialized && state_.motion.storage_error &&
            (page_ != WatchPage::kActivityResetConfirm || activity_reset_token_ == state_.motion.repair_token);
        if (!current || !handler_ || pending_request_ || state_.motion.retry_pending || state_.motion.reset_pending ||
            state_.time.sleep_prepared || state_.recording)
            lv_obj_add_state(activity_reset_, LV_STATE_DISABLED);
        else lv_obj_remove_state(activity_reset_, LV_STATE_DISABLED);
    }
    if (activity_reset_cancel_) {
        if (state_.motion.reset_pending || (pending_request_ && pending_action_ == WatchViewAction::kResetCorruptActivityStorage))
            lv_obj_add_state(activity_reset_cancel_, LV_STATE_DISABLED);
        else lv_obj_remove_state(activity_reset_cancel_, LV_STATE_DISABLED);
    }
}

void WatchView::BuildFace() {
    digits_ = Label(body_, "--:--", digits_font_);
    lv_obj_set_style_text_align(digits_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(digits_, 20, 0);
    lv_obj_set_style_text_color(digits_, lv_color_hex(kAccent), 0);
    detail_ = Label(body_, "");
    lv_obj_set_style_text_color(detail_, lv_color_hex(kMuted), 0);
    lv_obj_set_style_text_align(detail_, LV_TEXT_ALIGN_CENTER, 0);
    battery_ = Label(body_, "");
    connections_ = Label(body_, "", icon_font_);
    record_ = Label(body_, "");
    lv_obj_set_style_text_color(record_, lv_color_hex(kWarning), 0);
    auto* assistant = Button(body_, FONT_AWESOME_MICROPHONE, [this]() { Navigate(WatchPage::kAssistant); }, false, true);
    lv_obj_set_size(assistant, LV_PCT(100), 48);
    Button(body_, Lang::Strings::WATCH_RECORDER, [this]() { Navigate(WatchPage::kRecorder); });
    Button(body_, Lang::Strings::WATCH_TIMER, [this]() { Navigate(WatchPage::kTimer); });
    Button(body_, Lang::Strings::WATCH_ALARMS, [this]() { Navigate(WatchPage::kAlarms); });
}

void WatchView::BuildApps() {
    for (const auto page : {WatchPage::kRecorder, WatchPage::kRecordings, WatchPage::kPlayback, WatchPage::kAssistant, WatchPage::kNotifications, WatchPage::kActivity, WatchPage::kControls, WatchPage::kTimer, WatchPage::kStopwatch, WatchPage::kAlarms, WatchPage::kSettings}) {
        Button(body_, PageTitle(page), [this, page]() { Navigate(page); });
    }
}

void WatchView::BuildActivity() {
    using namespace Lang::Strings;
    Label(body_, WATCH_STEPS_TODAY);
    digits_ = Label(body_, "--", digits_font_);
    detail_ = Label(body_, "");
    connections_ = Label(body_, "");
    record_ = Label(body_, "");
    Button(body_, WATCH_ACTIVITY_HISTORY, [this]() { Navigate(WatchPage::kActivityHistory); });
    activity_retry_ = Button(body_, WATCH_ACTIVITY_RETRY, [this]() {
        WatchViewRequest request;
        request.action = WatchViewAction::kRetryActivityStorage;
        Submit(request);
    });
    activity_reset_ = Button(body_, WATCH_ACTIVITY_RESET, [this]() {
        if (!pending_request_ && state_.motion.repair_token && !state_.motion.retry_pending && !state_.motion.reset_pending)
            Navigate(WatchPage::kActivityResetConfirm);
    });
}

void WatchView::BuildActivityResetConfirm() {
    Label(body_, Lang::Strings::WATCH_ACTIVITY_RESET_WARNING);
    activity_reset_ = Button(body_, Lang::Strings::WATCH_ACTIVITY_RESET_CONFIRM, [this]() {
        if (!activity_reset_token_ || activity_reset_token_ != state_.motion.repair_token || state_.motion.initialized) return;
        WatchViewRequest request;
        request.action = WatchViewAction::kResetCorruptActivityStorage;
        request.repair_token = activity_reset_token_;
        Submit(request);
    });
    lv_obj_set_style_bg_color(activity_reset_, lv_color_hex(0x822C36), 0);
    activity_reset_cancel_ = Button(body_, Lang::Strings::WATCH_ACTIVITY_RESET_CANCEL, [this]() {
        if (!state_.motion.reset_pending && !(pending_request_ && pending_action_ == WatchViewAction::kResetCorruptActivityStorage))
            Navigate(WatchPage::kActivity);
    });
}

void WatchView::RefreshActivity() {
    using namespace Lang::Strings;
    const auto& motion = state_.motion;
    const auto& activity = motion.activity;
    lv_label_set_text(digits_, motion.initialized && activity.day ? std::to_string(activity.steps).c_str() : "--");
    const char* status = WATCH_MOTION_UNAVAILABLE;
    if (motion.initialized) {
        switch (activity.status) {
            case WatchMotionStatus::kRunning: status = WATCH_MOTION_RUNNING; break;
            case WatchMotionStatus::kPaused: status = WATCH_MOTION_PAUSED; break;
            case WatchMotionStatus::kFault: status = WATCH_MOTION_ERROR; break;
            case WatchMotionStatus::kUnavailable: break;
        }
    }
    lv_label_set_text(detail_, motion.storage_error ? WATCH_SAVE_FAILED : status);
    std::string undated;
    if (!activity.day) undated = WATCH_TIME_UNSYNCED;
    if (activity.undated_steps) {
        if (!undated.empty()) undated += "\n";
        undated += std::string(WATCH_STEPS_UNDATED) + ": " + std::to_string(activity.undated_steps);
    }
    lv_label_set_text(connections_, undated.c_str());
    lv_label_set_text(record_, activity.incomplete ? WATCH_MOTION_INTERRUPTED : "");
    if (motion.storage_error || motion.retry_pending) lv_obj_remove_flag(activity_retry_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(activity_retry_, LV_OBJ_FLAG_HIDDEN);
    if (motion.repair_token && !motion.initialized) lv_obj_remove_flag(activity_reset_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(activity_reset_, LV_OBJ_FLAG_HIDDEN);
}

void WatchView::BuildActivityHistory() {
    activity_history_status_ = Label(body_, "");
    for (size_t i = 0; i < activity_history_rows_.size(); ++i) {
        auto* row = Row(body_, 86);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(row, 4, 0);
        activity_history_rows_[i] = row;
        activity_history_dates_[i] = Label(row, "");
        lv_obj_set_style_text_color(activity_history_dates_[i], lv_color_hex(kMuted), 0);
        activity_history_steps_[i] = Label(row, "");
    }
}

void WatchView::RefreshActivityHistory() {
    using namespace Lang::Strings;
    const auto& motion = state_.motion;
    const auto count = motion.initialized ? std::min(motion.history.count, motion.history.days.size()) : 0;
    const char* status = !motion.initialized ? WATCH_MOTION_UNAVAILABLE :
        count == 0 ? WATCH_ACTIVITY_HISTORY_EMPTY : motion.history.incomplete ? WATCH_MOTION_INTERRUPTED : "";
    if (motion.storage_error) status = WATCH_SAVE_FAILED;
    lv_label_set_text(activity_history_status_, status);
    if (*status) lv_obj_remove_flag(activity_history_status_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(activity_history_status_, LV_OBJ_FLAG_HIDDEN);
    auto days = motion.history.days;
    std::sort(days.begin(), days.begin() + count, [](const auto& a, const auto& b) { return a.day > b.day; });
    for (size_t i = 0; i < activity_history_rows_.size(); ++i) {
        if (i >= count) {
            lv_obj_add_flag(activity_history_rows_[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(activity_history_rows_[i], LV_OBJ_FLAG_HIDDEN);
        const auto date = WatchCivilDateLabel(days[i].day);
        lv_label_set_text(activity_history_dates_[i], date.empty() ? WATCH_ACTIVITY_DATE_UNKNOWN : date.c_str());
        const auto steps = std::string(WATCH_ACTIVITY_STEPS) + ": " + std::to_string(days[i].steps);
        lv_label_set_text(activity_history_steps_[i], steps.c_str());
    }
}

void WatchView::BuildControls() {
    using namespace Lang::Strings;
    auto* lock_row = Row(body_, 60);
    auto* lock_label = Label(lock_row, WATCH_TOUCH_LOCK);
    lv_obj_set_width(lock_label, 1);
    lv_obj_set_flex_grow(lock_label, 1);
    Button(lock_row, FONT_AWESOME_LOCK, [this]() {
        if (!state_.time.sleep_prepared) SetTouchLocked(true);
    }, false, true);
    for (size_t index = 0; index < level_sliders_.size(); ++index) {
        auto* row = Row(body_, 46);
        const std::string title = std::string(index == 0 ? FONT_AWESOME_SUN : FONT_AWESOME_VOLUME_HIGH) + "  " +
            (index == 0 ? WATCH_BRIGHTNESS : WATCH_VOLUME);
        auto* label = Label(row, title.c_str(), icon_font_);
        lv_obj_set_width(label, 1);
        lv_obj_set_flex_grow(label, 1);
        level_values_[index] = Label(row, "");
        lv_obj_set_width(level_values_[index], 76);
        lv_obj_set_style_text_align(level_values_[index], LV_TEXT_ALIGN_RIGHT, 0);
        auto* track = Row(body_, 54);
        lv_obj_set_style_pad_hor(track, 18, 0);
        auto* slider = lv_slider_create(track);
        level_sliders_[index] = slider;
        lv_obj_set_width(slider, LV_PCT(100));
        lv_obj_set_height(slider, 12);
        lv_slider_set_range(slider, index == 0 ? 10 : 0, 100);
        lv_obj_set_style_bg_color(slider, lv_color_hex(kAccent), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, lv_color_hex(kAccent), LV_PART_KNOB);
        Bind(slider, LV_EVENT_VALUE_CHANGED, [this, index](lv_event_t*) {
            char value[12];
            std::snprintf(value, sizeof(value), "%d%%", static_cast<int>(lv_slider_get_value(level_sliders_[index])));
            lv_label_set_text(level_values_[index], value);
        });
        Bind(slider, LV_EVENT_RELEASED, [this, index](lv_event_t*) {
            const auto& control = state_.controls;
            const int saved = index == 0 ? control.brightness : control.volume;
            const int value = lv_slider_get_value(level_sliders_[index]);
            if (!control.can_adjust || saved < 0 || value == saved || pending_request_) return;
            WatchViewRequest request;
            request.action = WatchViewAction::kSetLevel;
            request.level = index == 0 ? WatchLevel::kBrightness : WatchLevel::kVolume;
            request.level_value = value;
            Submit(request);
        });
    }
    auto* wifi_row = Row(body_, 60);
    auto* wifi_label = Label(wifi_row, WATCH_WIFI);
    lv_obj_set_width(wifi_label, 1);
    lv_obj_set_flex_grow(wifi_label, 1);
    wifi_retry_ = Button(wifi_row, FONT_AWESOME_ARROWS_ROTATE, [this]() {
        if (!state_.controls.can_adjust || pending_request_) return;
        WatchViewRequest request;
        request.action = WatchViewAction::kSetWifiEnabled;
        request.enabled = state_.controls.wifi_enabled;
        Submit(request);
    }, false, true);
    lv_obj_set_width(wifi_retry_, 52);
    lv_obj_set_flex_grow(wifi_retry_, 0);
    wifi_switch_ = lv_switch_create(wifi_row);
    lv_obj_set_size(wifi_switch_, 60, 32);
    lv_obj_set_style_bg_color(wifi_switch_, lv_color_hex(kAccent), LV_PART_INDICATOR | LV_STATE_CHECKED);
    Bind(wifi_switch_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t*) {
        const bool enabled = lv_obj_has_state(wifi_switch_, LV_STATE_CHECKED);
        const bool cancel_enable = !enabled && pending_request_ && pending_action_ == WatchViewAction::kSetWifiEnabled && pending_enabled_;
        if (!state_.controls.can_adjust || state_.time.sleep_prepared || (pending_request_ && !cancel_enable)) {
            RefreshControls();
            return;
        }
        WatchViewRequest request;
        request.action = WatchViewAction::kSetWifiEnabled;
        request.enabled = enabled;
        Submit(request);
    });
    wifi_status_ = Label(body_, "");
    auto* voice_row = Row(body_, 60);
    auto* voice_label = Label(voice_row, WATCH_ASSISTANT);
    lv_obj_set_width(voice_label, 1);
    lv_obj_set_flex_grow(voice_label, 1);
    voice_switch_ = lv_switch_create(voice_row);
    lv_obj_set_size(voice_switch_, 60, 32);
    lv_obj_set_style_bg_color(voice_switch_, lv_color_hex(kAccent), LV_PART_INDICATOR | LV_STATE_CHECKED);
    Bind(voice_switch_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t*) {
        const bool enabled = lv_obj_has_state(voice_switch_, LV_STATE_CHECKED);
        if (enabled && (!state_.controls.can_adjust || pending_request_)) {
            RefreshControls();
            return;
        }
        WatchViewRequest request;
        request.action = WatchViewAction::kSetVoiceEnabled;
        request.enabled = enabled;
        Submit(request);
    });
    controls_status_ = Label(body_, "");
    lv_obj_set_style_text_color(controls_status_, lv_color_hex(kWarning), 0);
    Button(body_, WATCH_TIME_SETTINGS, [this]() { Navigate(WatchPage::kSettings); });
    Button(body_, WATCH_POWER_MODE, [this]() { Navigate(WatchPage::kPowerSettings); });
    Button(body_, WATCH_WRIST_WAKE, [this]() { Navigate(WatchPage::kWristSettings); });
    Button(body_, WATCH_NOTIFICATIONS, [this]() { Navigate(WatchPage::kNotifications); });
    if (state_.pairing.available) Button(body_, WATCH_PAIR_DEVICE, [this]() { Navigate(WatchPage::kDevicePairing); });
}

void WatchView::ReloadPowerDraft() {
    power_draft_revision_ = state_.power_mode.revision;
    power_draft_binding_ = state_.power_mode.binding_generation;
    power_draft_ = state_.power_mode.power_mode;
    if (power_options_) lv_dropdown_set_selected(power_options_, power_draft_ == "always_available" ? 2 :
        power_draft_ == "battery_saver" ? 1 : 0);
}

void WatchView::BuildPowerSettings() {
    using namespace Lang::Strings;
    power_status_ = Label(body_, "");
    power_saved_ = Label(body_, "");
    power_options_ = lv_dropdown_create(body_);
    lv_obj_set_width(power_options_, LV_PCT(100));
    lv_dropdown_set_symbol(power_options_, FONT_AWESOME_ANGLE_DOWN);
    lv_obj_set_style_text_font(power_options_, icon_font_, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(power_options_, lv_color_hex(kSurface), 0);
    lv_obj_set_style_text_color(power_options_, lv_color_hex(kText), 0);
    auto* list = lv_dropdown_get_list(power_options_);
    lv_obj_set_style_text_font(list, text_font_, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(kSurface), 0);
    lv_obj_set_style_text_color(list, lv_color_hex(kText), 0);
    lv_obj_set_style_max_height(list, 240, 0);
    const std::string options = std::string(WATCH_POWER_STANDBY) + "\n" + WATCH_POWER_SAVER + "\n" + WATCH_POWER_ALWAYS;
    lv_dropdown_set_options(power_options_, options.c_str());
    lv_dropdown_set_selected(power_options_, power_draft_ == "always_available" ? 2 : power_draft_ == "battery_saver" ? 1 : 0);
    Bind(power_options_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t*) {
        const auto selected = lv_dropdown_get_selected(power_options_);
        power_draft_ = selected == 2 ? "always_available" : selected == 1 ? "battery_saver" : "watch_standby";
        RefreshPowerSettings();
    });
    auto* row = Row(body_, 60);
    power_reload_ = Button(row, FONT_AWESOME_ARROWS_ROTATE, [this]() {
        ReloadPowerDraft();
        RefreshPowerSettings();
    }, false, true);
    lv_obj_set_width(power_reload_, 52);
    lv_obj_set_flex_grow(power_reload_, 0);
    power_apply_ = Button(row, WATCH_SAVE, [this]() {
        if (!state_.power_mode.can_change || power_draft_revision_ != state_.power_mode.revision ||
            power_draft_binding_ != state_.power_mode.binding_generation) return;
        WatchViewRequest request;
        request.action = WatchViewAction::kSetPowerMode;
        request.settings_revision = power_draft_revision_;
        request.binding_generation = power_draft_binding_;
        request.power_mode = power_draft_;
        Submit(request);
    });
    lv_obj_set_width(power_apply_, 1);
    lv_obj_set_flex_grow(power_apply_, 1);
}

void WatchView::RefreshPowerSettings() {
    using namespace Lang::Strings;
    const auto& state = state_.power_mode;
    const char* saved_mode = state.power_mode == "always_available" ? WATCH_POWER_ALWAYS :
        state.power_mode == "battery_saver" ? WATCH_POWER_SAVER : WATCH_POWER_STANDBY;
    char saved[160];
    std::snprintf(saved, sizeof(saved), WATCH_POWER_SAVED_FORMAT, saved_mode);
    lv_label_set_text(power_saved_, state.revision ? saved : "");
    if (!power_draft_revision_ && state.revision) ReloadPowerDraft();
    const bool stale = power_draft_revision_ != state.revision || power_draft_binding_ != state.binding_generation;
    const bool ready = handler_ && !pending_request_ && !state_.time.sleep_prepared && state.can_change;
    for (auto* object : {power_options_, power_reload_}) {
        if (ready) lv_obj_remove_state(object, LV_STATE_DISABLED);
        else lv_obj_add_state(object, LV_STATE_DISABLED);
    }
    if (ready && !stale && (power_draft_ != state.power_mode || !state.apply_error.empty()))
        lv_obj_remove_state(power_apply_, LV_STATE_DISABLED);
    else lv_obj_add_state(power_apply_, LV_STATE_DISABLED);
    const char* status = state.message == WatchPowerModeMessage::kSaving ? WATCH_SAVING :
        state.message == WatchPowerModeMessage::kConflict && stale ? WATCH_POWER_CONFLICT :
        state.message == WatchPowerModeMessage::kFailed ? WATCH_POWER_UNCONFIRMED :
        !state_.wifi_connected ? WATCH_POWER_OFFLINE :
        !state.revision ? WATCH_POWER_UNAVAILABLE :
        !state.apply_error.empty() ? WATCH_POWER_FAILED :
        stale && power_draft_ != state.power_mode ? WATCH_POWER_CONFLICT :
        state.pending || state.applied_revision != state.revision ? WATCH_POWER_PENDING : WATCH_POWER_APPLIED;
    lv_label_set_text(power_status_, status);
}

void WatchView::WristCommand(WatchWristAction action, bool enabled) {
    WatchViewRequest request;
    request.action = WatchViewAction::kWristCommand;
    request.wrist_action = action;
    request.enabled = enabled;
    Submit(request);
}

void WatchView::BuildWristSettings() {
    using namespace Lang::Strings;
    auto* row = Row(body_, 60);
    auto* label = Label(row, WATCH_WRIST_WAKE);
    lv_obj_set_width(label, 1);
    lv_obj_set_flex_grow(label, 1);
    wrist_switch_ = lv_switch_create(row);
    lv_obj_set_size(wrist_switch_, 60, 32);
    lv_obj_set_style_bg_color(wrist_switch_, lv_color_hex(kAccent), LV_PART_INDICATOR | LV_STATE_CHECKED);
    Bind(wrist_switch_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t*) {
        const bool enabled = lv_obj_has_state(wrist_switch_, LV_STATE_CHECKED);
        const bool ready = state_.wrist.calibrated && !state_.wrist.pending && !pending_request_ &&
            state_.motion.activity.status == WatchMotionStatus::kRunning;
        if (!enabled || ready) WristCommand(WatchWristAction::kSetEnabled, enabled);
        RefreshWristSettings();
    });
    wrist_status_ = Label(body_, "");
    Label(body_, WATCH_WRIST_CALIBRATION_POSE);
    wrist_calibrate_ = Button(body_, WATCH_WRIST_CALIBRATE, [this]() { WristCommand(WatchWristAction::kCalibrate); });
    wrist_cancel_ = Button(body_, WATCH_WRIST_CANCEL, [this]() { WristCommand(WatchWristAction::kCancel); });
}

void WatchView::RefreshWristSettings() {
    using namespace Lang::Strings;
    const auto& wrist = state_.wrist;
    const bool pending = wrist.pending || (pending_request_ && pending_action_ == WatchViewAction::kWristCommand);
    const bool ready = wrist.available && (wrist.loaded || wrist.storage_error) && !pending &&
        !pending_request_ && !state_.time.sleep_prepared && state_.motion.activity.status == WatchMotionStatus::kRunning;
    if (wrist.enabled) lv_obj_add_state(wrist_switch_, LV_STATE_CHECKED);
    else lv_obj_remove_state(wrist_switch_, LV_STATE_CHECKED);
    const auto enable = [](lv_obj_t* object, bool allowed) {
        if (allowed) lv_obj_remove_state(object, LV_STATE_DISABLED);
        else lv_obj_add_state(object, LV_STATE_DISABLED);
    };
    enable(wrist_switch_, handler_ && (wrist.enabled || (ready && wrist.calibrated)));
    enable(wrist_calibrate_, handler_ && ready);
    enable(wrist_cancel_, handler_ && (pending || wrist.storage_error));
    lv_label_set_text(lv_obj_get_child(wrist_cancel_, 0),
        !pending && wrist.storage_error ? WATCH_WRIST_RETRY_DISABLE : WATCH_WRIST_CANCEL);
    const char* status = !wrist.available ? WATCH_MOTION_UNAVAILABLE :
        pending ? (wrist.calibration == WatchCalibrationStatus::kCollecting ? WATCH_WRIST_KEEP_STILL : WATCH_SAVING) :
        wrist.storage_error ? WATCH_SAVE_FAILED :
        wrist.calibration == WatchCalibrationStatus::kFailed ? WATCH_WRIST_CALIBRATION_FAILED :
        !wrist.loaded ? WATCH_WORKING :
        state_.motion.activity.status != WatchMotionStatus::kRunning ? WATCH_WRIST_PAUSED :
        wrist.calibrated ? WATCH_WRIST_CALIBRATED : WATCH_WRIST_CALIBRATION_REQUIRED;
    lv_label_set_text(wrist_status_, status);
}

void WatchView::CancelPairing() {
    const bool opening = pending_request_ && (pending_action_ == WatchViewAction::kOpenDevicePairing ||
        pending_action_ == WatchViewAction::kOpenBindingRemoval || pending_action_ == WatchViewAction::kOpenControlRemoval ||
        pending_action_ == WatchViewAction::kOpenSavedControlPeers);
    if (!state_.pairing.active && !opening) return;
    if (pending_request_ && pending_action_ == WatchViewAction::kCancelDevicePairing) return;
    WatchViewRequest request;
    request.action = WatchViewAction::kCancelDevicePairing;
    request.pairing_attempt = state_.pairing.state.attempt;
    if (opening)
        request.pairing_open_request = pending_request_;
    Submit(request);
}

void WatchView::BuildDevicePairing() {
    using namespace Lang::Strings;
    using Phase = watch_voice::PairingPhase;
    using Error = watch_voice::PairingError;
    const auto snapshot = state_.pairing;
    if (!snapshot.available) { Label(body_, WATCH_PAIR_UNAVAILABLE); return; }
    const auto& state = snapshot.state;
    const char* status = WATCH_PAIR_IDLE;
    switch (state.phase) {
        case Phase::kClosed: break;
        case Phase::kPreparing: status = WATCH_PAIR_PREPARING; break;
        case Phase::kAwaitPeer: status = WATCH_PAIR_WAIT_PHONE; break;
        case Phase::kReadingChallenge: status = WATCH_PAIR_CHECK_REQUEST; break;
        case Phase::kAwaitApproval: status = WATCH_PAIR_APPROVAL; break;
        case Phase::kSigning: status = WATCH_PAIR_SIGNING; break;
        case Phase::kAwaitCredential: status = WATCH_PAIR_WAIT_CREDENTIAL; break;
        case Phase::kVerifying: status = WATCH_PAIR_VERIFYING; break;
        case Phase::kBound: status = state.binding_generation ? WATCH_PAIR_BOUND : WATCH_PAIR_FAILED; break;
        case Phase::kControlReady: status = state.control_only && state.control_peer_generation
            ? WATCH_PAIR_CONTROL_READY : WATCH_PAIR_FAILED; break;
        case Phase::kFailed: status = WATCH_PAIR_FAILED; break;
        case Phase::kCancelled: status = WATCH_PAIR_CANCELLED; break;
        case Phase::kInspectingBinding: status = WATCH_BINDING_READING; break;
        case Phase::kAwaitRemoval: status = WATCH_BINDING_CONFIRM; break;
        case Phase::kRemovingBinding: status = WATCH_BINDING_REMOVING; break;
        case Phase::kBindingRemoved: status = state.removing_binding && state.binding_generation
            ? WATCH_BINDING_REMOVED : WATCH_PAIR_FAILED; break;
        case Phase::kInspectingSavedPeers: status = WATCH_SAVED_PHONES_READING; break;
        case Phase::kAwaitSavedPeer: status = state.saved_peers.empty() ? WATCH_SAVED_PHONES_EMPTY : WATCH_SAVED_PHONES; break;
        case Phase::kAwaitAllControlRemoval: status = WATCH_SAVED_PHONES_REMOVE_ALL_CONFIRM; break;
        case Phase::kAwaitControlRemoval: status = state.removing_saved_control ? WATCH_SAVED_PHONE_CONFIRM : WATCH_CONTROL_REMOVE_CONFIRM; break;
        case Phase::kRemovingControl: status = WATCH_CONTROL_REMOVING; break;
        case Phase::kControlRemoved: status = state.removing_control && state.control_peer_generation
            ? WATCH_CONTROL_REMOVED : WATCH_PAIR_FAILED; break;
        case Phase::kRemovingAllControl: status = WATCH_SAVED_PHONES_REMOVING_ALL; break;
        case Phase::kAllControlRemoved: status = state.removing_saved_control && state.removing_all_control
            ? WATCH_SAVED_PHONES_REMOVED_ALL : WATCH_PAIR_FAILED; break;
    }
    if (state.phase == Phase::kFailed) {
        switch (state.error) {
            case Error::kNone: break;
            case Error::kUnavailable: status = WATCH_PAIR_UNAVAILABLE; break;
            case Error::kAlreadyBound: status = WATCH_PAIR_ALREADY_BOUND; break;
            case Error::kIdentity: status = WATCH_PAIR_IDENTITY_ERROR; break;
            case Error::kStorage: status = state.removing_control ? WATCH_CONTROL_REMOVE_STORAGE : WATCH_PAIR_STORAGE_ERROR; break;
            case Error::kClock: status = WATCH_PAIR_CLOCK_ERROR; break;
            case Error::kInvalidChallenge: status = WATCH_PAIR_INVALID_REQUEST; break;
            case Error::kRejected: status = WATCH_PAIR_CANCELLED; break;
            case Error::kVerification: status = WATCH_PAIR_VERIFY_ERROR; break;
            case Error::kConflict: status = WATCH_PAIR_CONFLICT; break;
            case Error::kTimeout: status = WATCH_PAIR_TIMEOUT; break;
            case Error::kNoBinding: status = WATCH_BINDING_ABSENT; break;
            case Error::kControlRemoval: status = WATCH_CONTROL_REMOVE_FAILED; break;
        }
    }
    if (snapshot.active && state.phase == Phase::kAwaitPeer && snapshot.confirmation)
        status = snapshot.compare_number ? WATCH_PAIR_COMPARE : WATCH_PAIR_PEER_CONSENT;
    if (!snapshot.active && state.phase == Phase::kClosed && !snapshot.can_open)
        status = snapshot.network_required ? WATCH_PAIR_NETWORK_REQUIRED : WATCH_PAIR_UNAVAILABLE;
    Label(body_, status);
    if (snapshot.active && state.removing_saved_control && state.phase == Phase::kAwaitSavedPeer) {
        for (const auto& peer : state.saved_peers) {
            char address[24];
            std::snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X",
                peer.address[0], peer.address[1], peer.address[2], peer.address[3], peer.address[4], peer.address[5]);
            auto* button = Button(body_, address, [this, snapshot, selection = peer.selection]() {
                if (!SamePairing(snapshot, state_.pairing) || !visible_ || page_ != WatchPage::kDevicePairing) return;
                WatchViewRequest request;
                request.action = WatchViewAction::kSelectSavedControlPeer;
                request.pairing_attempt = snapshot.state.attempt;
                request.pairing_peer_generation = selection;
                Submit(request);
            });
            pairing_saved_choices_.push_back(button);
        }
        if (!state.saved_peers.empty()) {
            pairing_saved_all_ = Button(body_, WATCH_SAVED_PHONES_REMOVE_ALL, [this, snapshot]() {
                if (!SamePairing(snapshot, state_.pairing) || !visible_ || page_ != WatchPage::kDevicePairing) return;
                WatchViewRequest request;
                request.action = WatchViewAction::kSelectAllSavedControlPeers;
                request.pairing_attempt = snapshot.state.attempt;
                Submit(request);
            });
        }
    }
    if (snapshot.active && state.removing_all_control && state.phase == Phase::kAwaitAllControlRemoval) {
        Label(body_, WATCH_SAVED_PHONES_REMOVE_ALL_NOTICE);
        const auto confirm = [this, snapshot](bool accept) {
            if (!SamePairing(snapshot, state_.pairing) || !visible_ || page_ != WatchPage::kDevicePairing) return;
            WatchViewRequest request;
            request.action = WatchViewAction::kConfirmAllControlRemoval;
            request.pairing_attempt = snapshot.state.attempt;
            request.enabled = accept;
            Submit(request);
        };
        pairing_accept_ = Button(body_, WATCH_SAVED_PHONES_REMOVE_ALL, [confirm]() { confirm(true); });
        pairing_reject_ = Button(body_, WATCH_NOTIFY_REJECT, [confirm]() { confirm(false); });
    }
    if (snapshot.active && state.removing_control && state.phase == Phase::kAwaitControlRemoval && state.control_peer_generation) {
        if (state.removing_saved_control) {
            for (const auto& peer : state.saved_peers) if (peer.selection == state.control_peer_generation) {
                char address[24];
                std::snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X",
                    peer.address[0], peer.address[1], peer.address[2], peer.address[3], peer.address[4], peer.address[5]);
                Label(body_, address);
            }
        }
        Label(body_, WATCH_CONTROL_REMOVE_NOTICE);
        const auto confirm = [this, snapshot](bool accept) {
            if (!SamePairing(snapshot, state_.pairing) || !visible_ || page_ != WatchPage::kDevicePairing) return;
            WatchViewRequest request;
            request.action = WatchViewAction::kConfirmControlRemoval;
            request.pairing_attempt = snapshot.state.attempt;
            request.pairing_peer_generation = snapshot.state.control_peer_generation;
            request.enabled = accept;
            Submit(request);
        };
        pairing_accept_ = Button(body_, WATCH_CONTROL_REMOVE, [confirm]() { confirm(true); });
        pairing_reject_ = Button(body_, WATCH_NOTIFY_REJECT, [confirm]() { confirm(false); });
    }
    if (snapshot.active && state.removing_binding && state.phase == Phase::kAwaitRemoval && state.binding_generation) {
        Label(body_, WATCH_BINDING_REMOVE_NOTICE);
        const auto confirm = [this, snapshot](bool accept) {
            if (!SamePairing(snapshot, state_.pairing) || !visible_ || page_ != WatchPage::kDevicePairing) return;
            WatchViewRequest request;
            request.action = WatchViewAction::kConfirmBindingRemoval;
            request.pairing_attempt = snapshot.state.attempt;
            request.binding_generation = snapshot.state.binding_generation;
            request.enabled = accept;
            Submit(request);
        };
        pairing_accept_ = Button(body_, WATCH_BINDING_REMOVE, [confirm]() { confirm(true); });
        lv_obj_set_height(pairing_accept_, 96);
        pairing_reject_ = Button(body_, WATCH_NOTIFY_REJECT, [confirm]() { confirm(false); });
    }
    if (snapshot.active && state.phase == Phase::kAwaitPeer && snapshot.confirmation) {
        if (snapshot.compare_number) {
            char code[16];
            std::snprintf(code, sizeof(code), "%06u", static_cast<unsigned>(snapshot.passkey));
            Label(body_, code, digits_font_);
        }
        const auto confirm = [this, snapshot](bool accept) {
            if (!SamePairing(snapshot, state_.pairing) || !visible_ || page_ != WatchPage::kDevicePairing) return;
            WatchViewRequest request;
            request.action = WatchViewAction::kConfirmDevicePairingPeer;
            request.pairing_attempt = snapshot.state.attempt;
            request.pairing_confirmation = snapshot.confirmation;
            request.enabled = accept;
            Submit(request);
        };
        pairing_accept_ = Button(body_, snapshot.compare_number ? WATCH_NOTIFY_MATCH : WATCH_NOTIFY_ALLOW,
            [confirm]() { confirm(true); });
        pairing_reject_ = Button(body_, WATCH_NOTIFY_REJECT, [confirm]() { confirm(false); });
    }
    if (snapshot.active && state.phase == Phase::kAwaitApproval && state.offer.request &&
        !state.offer.user_id.empty() && !state.offer.challenge_id.empty() && state.offer.device_id == state.device_id) {
        Label(body_, WATCH_PAIR_ACCOUNT_ID);
        Label(body_, state.offer.user_id.c_str());
        const auto approve = [this, snapshot](bool accept) {
            if (!SamePairing(snapshot, state_.pairing) || !visible_ || page_ != WatchPage::kDevicePairing) return;
            WatchViewRequest request;
            request.action = WatchViewAction::kApproveDevicePairingAccount;
            request.pairing_attempt = snapshot.state.attempt;
            request.pairing_request = snapshot.state.offer.request;
            request.enabled = accept;
            Submit(request);
        };
        pairing_accept_ = Button(body_, WATCH_PAIR_BIND_ACCOUNT, [approve]() { approve(true); });
        pairing_reject_ = Button(body_, WATCH_NOTIFY_REJECT, [approve]() { approve(false); });
    }
    if (snapshot.active && state.phase != Phase::kBound && state.phase != Phase::kRemovingControl &&
        state.phase != Phase::kRemovingAllControl) {
        pairing_cancel_ = Button(body_, WATCH_PAIR_CANCEL, [this, snapshot]() {
            if (snapshot.state.attempt == state_.pairing.state.attempt) CancelPairing();
        });
    } else if (!snapshot.active && state.phase != Phase::kControlReady && (snapshot.can_open ||
               (state.phase != Phase::kBound && state.error != Error::kAlreadyBound && state.error != Error::kStorage))) {
        pairing_open_ = Button(body_, WATCH_PAIR_START, [this, snapshot]() {
            if (!SamePairing(snapshot, state_.pairing) || !state_.pairing.can_open || !visible_ ||
                page_ != WatchPage::kDevicePairing) return;
            WatchViewRequest request;
            request.action = WatchViewAction::kOpenDevicePairing;
            request.pairing_control_only = true;
            request.pairing_attempt = snapshot.state.attempt;
            Submit(request);
        });
    }
    if (!state.device_id.empty()) {
        Label(body_, WATCH_PAIR_DEVICE_ID);
        Label(body_, state.device_id.c_str());
    }
    if (!snapshot.active) {
        pairing_cloud_ = Button(body_, WATCH_PAIR_CLOUD_START, [this, snapshot]() {
            if (!SamePairing(snapshot, state_.pairing) || !snapshot.can_authorize_cloud || !visible_ ||
                page_ != WatchPage::kDevicePairing) return;
            WatchViewRequest request;
            request.action = WatchViewAction::kOpenDevicePairing;
            request.pairing_attempt = snapshot.state.attempt;
            Submit(request);
        });
        lv_obj_set_height(pairing_cloud_, 96);
        Label(body_, WATCH_BINDING_REMOVE_REQUIREMENTS);
        pairing_remove_ = Button(body_, WATCH_BINDING_REMOVE, [this, snapshot]() {
            if (!SamePairing(snapshot, state_.pairing) || !snapshot.can_remove_binding || !visible_ ||
                page_ != WatchPage::kDevicePairing) return;
            WatchViewRequest request;
            request.action = WatchViewAction::kOpenBindingRemoval;
            request.pairing_attempt = snapshot.state.attempt;
            Submit(request);
        });
        lv_obj_set_height(pairing_remove_, 96);
        Label(body_, WATCH_CONTROL_REMOVE_REQUIREMENTS);
        pairing_forget_ = Button(body_, WATCH_CONTROL_REMOVE, [this, snapshot]() {
            if (!SamePairing(snapshot, state_.pairing) || !snapshot.forget_peer_generation || !visible_ ||
                page_ != WatchPage::kDevicePairing) return;
            WatchViewRequest request;
            request.action = WatchViewAction::kOpenControlRemoval;
            request.pairing_attempt = snapshot.state.attempt;
            request.pairing_peer_generation = snapshot.forget_peer_generation;
            Submit(request);
        });
        Label(body_, WATCH_SAVED_PHONES_REQUIREMENTS);
        pairing_saved_ = Button(body_, WATCH_SAVED_PHONES, [this, snapshot]() {
            if (!SamePairing(snapshot, state_.pairing) || !snapshot.can_inspect_saved_peers || !visible_ ||
                page_ != WatchPage::kDevicePairing) return;
            WatchViewRequest request;
            request.action = WatchViewAction::kOpenSavedControlPeers;
            request.pairing_attempt = snapshot.state.attempt;
            Submit(request);
        });
    }
}

void WatchView::RefreshPairingControls() {
    const bool available = handler_ && visible_ && state_.pairing.available && page_ == WatchPage::kDevicePairing;
    const auto enable = [](lv_obj_t* button, bool enabled) {
        if (!button) return;
        if (enabled) lv_obj_remove_state(button, LV_STATE_DISABLED);
        else lv_obj_add_state(button, LV_STATE_DISABLED);
    };
    enable(pairing_open_, available && state_.pairing.can_open && !pending_request_ && !state_.time.sleep_prepared);
    enable(pairing_remove_, available && state_.pairing.can_remove_binding && !pending_request_ && !state_.time.sleep_prepared);
    enable(pairing_cloud_, available && state_.pairing.can_authorize_cloud && !pending_request_ && !state_.time.sleep_prepared);
    enable(pairing_forget_, available && state_.pairing.forget_peer_generation && !pending_request_ && !state_.time.sleep_prepared);
    enable(pairing_saved_, available && state_.pairing.can_inspect_saved_peers && !pending_request_ && !state_.time.sleep_prepared);
    enable(pairing_saved_all_, available && state_.pairing.active && !pending_request_ && !state_.time.sleep_prepared);
    for (auto* button : pairing_saved_choices_)
        enable(button, available && state_.pairing.active && !pending_request_ && !state_.time.sleep_prepared);
    enable(pairing_accept_, available && state_.pairing.active && !pending_request_ && !state_.time.sleep_prepared);
    enable(pairing_reject_, available && state_.pairing.active);
    enable(pairing_cancel_, available && state_.pairing.active);
}

void WatchView::BuildNotifications() {
    using namespace Lang::Strings;
    const auto& snapshot = state_.notifications;
    if (!snapshot.available) { Label(body_, WATCH_NOTIFY_UNSUPPORTED); return; }
    auto* row = Row(body_, 68);
    auto* label = Label(row, WATCH_NOTIFICATIONS);
    lv_obj_set_width(label, 1);
    lv_obj_set_flex_grow(label, 1);
    notification_switch_ = lv_switch_create(row);
    lv_obj_set_size(notification_switch_, 60, 32);
    lv_obj_set_style_bg_color(notification_switch_, lv_color_hex(kAccent), LV_PART_INDICATOR | LV_STATE_CHECKED);
    Bind(notification_switch_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t*) {
        WatchViewRequest request;
        request.action = WatchViewAction::kSetNotificationsEnabled;
        request.enabled = lv_obj_has_state(notification_switch_, LV_STATE_CHECKED);
        Submit(request);
        RefreshNotificationControls();
    });
    if (snapshot.save_failed) Label(body_, WATCH_NOTIFY_SAVE_FAILED);
    const auto& client = snapshot.client;
    Label(body_, !client.enabled ? WATCH_NOTIFY_DISABLED :
        client.device_pairing_peer || client.control_pairing_peer ? WATCH_NOTIFY_UNAVAILABLE :
        snapshot.content.ready ? WATCH_NOTIFY_READY : NotificationStatus(client.state));
    if (!client.enabled || client.device_pairing_peer || client.control_pairing_peer) return;
    if (client.confirmation && (client.state == WatchAncsLinkState::kPeerConsent || client.state == WatchAncsLinkState::kConfirmNumber)) {
        const auto token = client.confirmation;
        if (client.state == WatchAncsLinkState::kConfirmNumber) {
            char code[16];
            std::snprintf(code, sizeof(code), "%06u", static_cast<unsigned>(client.passkey));
            Label(body_, code, digits_font_);
        }
        const auto confirm = [this, token](bool accept) {
            if (token != state_.notifications.client.confirmation) return;
            WatchViewRequest request;
            request.action = WatchViewAction::kConfirmNotificationPeer;
            request.notification_confirmation = token;
            request.enabled = accept;
            Submit(request);
        };
        notification_accept_ = Button(body_, client.state == WatchAncsLinkState::kConfirmNumber ?
            WATCH_NOTIFY_MATCH : WATCH_NOTIFY_ALLOW, [confirm]() { confirm(true); });
        notification_reject_ = Button(body_, WATCH_NOTIFY_REJECT, [confirm]() { confirm(false); });
    }
    if (!snapshot.content.ready || snapshot.content.source == WatchNotificationSource::kNone) return;
    const auto source = snapshot.content.source;
    const auto session = snapshot.content.session;
    const auto& notifications = snapshot.content.items;
    if (notifications.empty()) {
        Label(body_, WATCH_NOTIFY_EMPTY);
        return;
    }
    notification_clear_ = Button(body_, WATCH_NOTIFY_CLEAR, [this, source, session]() {
        if (state_.notifications.content.source != source || state_.notifications.content.session != session ||
            !state_.notifications.content.ready) return;
        WatchViewRequest request;
        request.action = WatchViewAction::kClearNotifications;
        request.notification_source = source;
        request.notification_session = session;
        Submit(request);
    });
    for (const auto& entry : notifications) {
        const auto uid = entry.uid;
        const auto revision = entry.revision;
        const std::string title = entry.details_ready ? (entry.title.empty() ? WATCH_NOTIFY_UNTITLED : entry.title) : WATCH_NOTIFY_LOADING;
        auto* button = Button(body_, title.c_str(), [this, source, session, uid, revision]() {
            if (pending_request_) return;
            selected_notification_source_ = source;
            selected_notification_session_ = session;
            selected_notification_uid_ = uid;
            selected_notification_revision_ = revision;
            if (!SelectedNotification()) return;
            Navigate(WatchPage::kNotificationDetail);
            WatchViewRequest request;
            request.action = WatchViewAction::kOpenNotification;
            request.notification_source = source;
            request.notification_session = session;
            request.notification_uid = uid;
            request.notification_revision = revision;
            Submit(request);
        });
        lv_obj_set_height(button, 96);
        auto* title_label = lv_obj_get_child(button, 0);
        lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
        lv_obj_set_height(title_label, 84);
        lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(title_label, lv_color_hex(entry.read ? kMuted : kText), 0);
        notification_items_.push_back(button);
    }
}

const WatchPhoneNotification* WatchView::SelectedNotification() const {
    const auto& snapshot = state_.notifications;
    const auto& content = snapshot.content;
    if (!snapshot.available || !snapshot.client.enabled || !content.ready ||
        selected_notification_source_ == WatchNotificationSource::kNone || content.source != selected_notification_source_ ||
        !selected_notification_session_ || content.session != selected_notification_session_) return nullptr;
    for (const auto& entry : content.items) {
        if (entry.uid == selected_notification_uid_ && entry.revision == selected_notification_revision_) return &entry;
    }
    return nullptr;
}

void WatchView::BuildNotificationDetail() {
    using namespace Lang::Strings;
    const auto* entry = SelectedNotification();
    if (!entry) { Label(body_, WATCH_NOTIFY_REMOVED); return; }
    if (!entry->details_ready) { Label(body_, WATCH_NOTIFY_LOADING); return; }
    Label(body_, entry->app_name.empty() ? entry->app_identifier.c_str() : entry->app_name.c_str());
    Label(body_, entry->title.empty() ? WATCH_NOTIFY_UNTITLED : entry->title.c_str());
    if (state_.notifications.content.source == WatchNotificationSource::kAndroid) {
        std::tm local{};
        if (entry->posted_at_ms && WatchLocalTime(entry->posted_at_ms, state_.time.state.timezone, local)) {
            char date[32];
            if (std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local)) Label(body_, date);
        }
    } else if (!entry->date.empty()) Label(body_, NotificationDate(entry->date).c_str());
    Label(body_, entry->message.c_str());
}

void WatchView::RefreshNotificationControls() {
    const auto& client = state_.notifications.client;
    const bool active = state_.notifications.available && handler_;
    const bool idle = active && !pending_request_ && !state_.time.sleep_prepared;
    const auto enable = [](lv_obj_t* object, bool allowed) {
        if (!object) return;
        if (allowed) lv_obj_remove_state(object, LV_STATE_DISABLED);
        else lv_obj_add_state(object, LV_STATE_DISABLED);
    };
    if (notification_switch_) {
        const bool checked = pending_request_ && pending_action_ == WatchViewAction::kSetNotificationsEnabled ? pending_enabled_ : client.enabled;
        if (checked) lv_obj_add_state(notification_switch_, LV_STATE_CHECKED);
        else lv_obj_remove_state(notification_switch_, LV_STATE_CHECKED);
        enable(notification_switch_, active && (checked || idle));
    }
    enable(notification_accept_, idle && client.enabled && !client.device_pairing_peer && !client.control_pairing_peer && client.confirmation);
    enable(notification_reject_, active && client.enabled && !client.device_pairing_peer && !client.control_pairing_peer && client.confirmation);
    const auto& content = state_.notifications.content;
    const bool ready = client.enabled && content.ready && content.session && content.source != WatchNotificationSource::kNone;
    enable(notification_clear_, active && ready);
    for (auto* item : notification_items_) enable(item, idle && ready);
}

void WatchView::RefreshControls() {
    const auto& control = state_.controls;
    for (size_t index = 0; index < level_sliders_.size(); ++index) {
        auto* slider = level_sliders_[index];
        const int saved = index == 0 ? control.brightness : control.volume;
        const bool enabled = handler_ && control.can_adjust && saved >= 0 && !pending_request_ && !state_.time.sleep_prepared;
        if (enabled) lv_obj_remove_state(slider, LV_STATE_DISABLED);
        else lv_obj_add_state(slider, LV_STATE_DISABLED);
        if (!pending_request_ && !lv_slider_is_dragged(slider)) {
            lv_slider_set_value(slider, saved < 0 ? 0 : saved, LV_ANIM_OFF);
            char value[12];
            if (saved < 0) std::snprintf(value, sizeof(value), "--");
            else std::snprintf(value, sizeof(value), "%d%%", saved);
            lv_label_set_text(level_values_[index], value);
        }
    }
    const bool voice_checked = pending_request_ && pending_action_ == WatchViewAction::kSetVoiceEnabled ?
        pending_enabled_ : control.voice_enabled;
    if (voice_checked) lv_obj_add_state(voice_switch_, LV_STATE_CHECKED);
    else lv_obj_remove_state(voice_switch_, LV_STATE_CHECKED);
    if (handler_ && (voice_checked || (!pending_request_ && control.can_adjust && !state_.time.sleep_prepared)))
        lv_obj_remove_state(voice_switch_, LV_STATE_DISABLED);
    else lv_obj_add_state(voice_switch_, LV_STATE_DISABLED);
    const bool wifi_pending = pending_request_ && pending_action_ == WatchViewAction::kSetWifiEnabled;
    const bool wifi_checked = wifi_pending ? pending_enabled_ : control.wifi_enabled;
    if (wifi_checked) lv_obj_add_state(wifi_switch_, LV_STATE_CHECKED);
    else lv_obj_remove_state(wifi_switch_, LV_STATE_CHECKED);
    const bool can_wifi = handler_ && control.can_adjust && !state_.time.sleep_prepared;
    if (can_wifi && (!pending_request_ || (wifi_pending && wifi_checked))) lv_obj_remove_state(wifi_switch_, LV_STATE_DISABLED);
    else lv_obj_add_state(wifi_switch_, LV_STATE_DISABLED);
    if (control.wifi_change_failed) lv_obj_remove_flag(wifi_retry_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(wifi_retry_, LV_OBJ_FLAG_HIDDEN);
    if (can_wifi && !pending_request_) lv_obj_remove_state(wifi_retry_, LV_STATE_DISABLED);
    else lv_obj_add_state(wifi_retry_, LV_STATE_DISABLED);
    lv_label_set_text(wifi_status_, wifi_pending ? Lang::Strings::WATCH_WIFI_PENDING :
        control.wifi_change_failed ? Lang::Strings::WATCH_WIFI_FAILED : !control.wifi_enabled ? Lang::Strings::WATCH_WIFI_OFF :
        state_.wifi_connected ? Lang::Strings::WATCH_WIFI_CONNECTED : Lang::Strings::WATCH_WIFI_NOT_CONNECTED);
    lv_label_set_text(controls_status_, control.voice_save_failed ? Lang::Strings::WATCH_VOICE_UNSAVED :
        control.brightness < 0 || control.volume < 0 ? Lang::Strings::WATCH_CONTROLS_UNAVAILABLE :
        !control.can_adjust ? Lang::Strings::WATCH_CONTROLS_BUSY : "");
}

void WatchView::BuildRecordings() {
    using namespace Lang::Strings;
    auto* navigation = Row(root_, 56);
    lv_obj_set_style_pad_hor(navigation, 16, 0);
    library_newer_ = Button(navigation, (std::string(FONT_AWESOME_ANGLE_LEFT) + " " + WATCH_LIBRARY_NEWER).c_str(), [this]() {
        if (!pending_request_ && state_.library.status == WatchLibraryStatus::kReady && state_.library.has_newer)
            LibraryCommand(WatchViewAction::kLoadNewerRecordings);
    }, false, true);
    library_older_ = Button(navigation, (std::string(WATCH_LIBRARY_OLDER) + " " + FONT_AWESOME_ANGLE_RIGHT).c_str(), [this]() {
        if (!pending_request_ && state_.library.status == WatchLibraryStatus::kReady && state_.library.has_older)
            LibraryCommand(WatchViewAction::kLoadOlderRecordings);
    }, false, true);
    for (auto* button : {library_newer_, library_older_}) {
        lv_obj_set_width(button, 1);
        lv_obj_set_flex_grow(button, 1);
    }
    library_refresh_ = Button(body_, WATCH_LIBRARY_REFRESH, [this]() {
        if (!pending_request_ && state_.library.status != WatchLibraryStatus::kLoading) LibraryCommand(WatchViewAction::kLoadRecordings);
    });
    if (state_.deletion.cleanup_available) {
        auto* cleanup = Button(body_, WATCH_RECORDING_DELETE_RETRY, [this]() {
            if (pending_request_ || !state_.deletion.cleanup_available) return;
            selected_recording_id_ = state_.deletion.recording_id;
            Navigate(WatchPage::kRecordingDeleteConfirm);
            // This entry authorizes metadata cleanup only, even if a same-named audio file reappeared.
            deletion_entry_ = {};
            deletion_entry_.id = state_.deletion.recording_id;
            deletion_entry_.file_name = deletion_entry_.id + ".opus.json";
            deletion_entry_.audio_available = false;
            deletion_request_ = state_.deletion.request;
        });
        lv_obj_set_height(cleanup, 94);
        lv_obj_set_flex_flow(cleanup, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(cleanup, 4, 0);
        auto* filename = Label(cleanup, (state_.deletion.recording_id + ".opus").c_str());
        lv_obj_set_height(filename, 38);
        lv_label_set_long_mode(filename, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(filename, lv_color_hex(kMuted), 0);
    }
    const auto& library = state_.library;
    if (library.status != WatchLibraryStatus::kReady) {
        Label(body_, library.status == WatchLibraryStatus::kLoading ? WATCH_LIBRARY_LOADING :
            library.status == WatchLibraryStatus::kUnavailable ? WATCH_LIBRARY_UNAVAILABLE :
            library.status == WatchLibraryStatus::kInterrupted ? WATCH_LIBRARY_INTERRUPTED : WATCH_LIBRARY_UNLOADED);
        return;
    }
    if (library.entries.empty()) {
        Label(body_, library.has_older || library.has_newer ? WATCH_LIBRARY_PAGE_EMPTY : WATCH_LIBRARY_EMPTY);
        return;
    }
    for (const auto& entry : library.entries) {
        std::tm local{};
        std::string title = entry.file_name;
        if (entry.started_at_unix_ms && WatchLocalTime(entry.started_at_unix_ms, state_.time.state.timezone, local)) {
            char date[40];
            std::snprintf(date, sizeof(date), "%04d-%02d-%02d  %02d:%02d", local.tm_year + 1900,
                local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
            title = date;
        }
        auto* button = Button(body_, title.c_str(), [this, id = entry.id]() {
            if (pending_request_ || state_.library.status != WatchLibraryStatus::kReady) return;
            selected_recording_id_ = id;
            Navigate(WatchPage::kRecordingDetail);
        });
        lv_obj_set_height(button, 94);
        lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(button, 4, 0);
        auto* name = lv_obj_get_child(button, 0);
        lv_obj_set_height(name, 38);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        char size[48];
        std::snprintf(size, sizeof(size), "  %llu KiB", static_cast<unsigned long long>((entry.size_bytes + 1023) / 1024));
        const std::string subtitle = !entry.audio_available ? WATCH_LIBRARY_AUDIO_MISSING :
            (entry.metadata_available ? Duration(entry.duration_ms) : "--:--:--") + std::string(size);
        auto* label = Label(button, subtitle.c_str());
        lv_obj_set_height(label, 38);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(label, lv_color_hex(kMuted), 0);
    }
}

void WatchView::BuildRecordingDetail() {
    using namespace Lang::Strings;
    const auto& entries = state_.library.entries;
    const auto found = std::find_if(entries.begin(), entries.end(), [this](const auto& entry) {
        return entry.id == selected_recording_id_;
    });
    if (found == entries.end()) { Label(body_, WATCH_LIBRARY_UNAVAILABLE); return; }
    const auto& entry = *found;
    Label(body_, entry.file_name.c_str());
    if (!entry.audio_available) {
        Label(body_, WATCH_LIBRARY_AUDIO_MISSING);
        char size[72];
        std::snprintf(size, sizeof(size), WATCH_LIBRARY_SIZE_FORMAT, static_cast<unsigned long long>(entry.size_bytes));
        Label(body_, size);
        recording_delete_ = Button(body_, WATCH_RECORDING_METADATA_CLEANUP, [this]() {
            if (CanDeleteRecording()) Navigate(WatchPage::kRecordingDeleteConfirm);
        });
        return;
    }
    recording_play_ = Button(body_, FONT_AWESOME_PLAY, [this]() {
        if (state_.playback.resources_held && state_.playback.recording_id == selected_recording_id_) {
            Navigate(WatchPage::kPlayback);
        } else if (state_.can_play_recording) {
            PlaybackCommand(WatchViewAction::kPlayRecording);
        }
    }, false, true);
    lv_obj_set_width(recording_play_, LV_PCT(100));
    auto* duration = Label(body_, entry.metadata_available ? Duration(entry.duration_ms).c_str() : WATCH_LIBRARY_DURATION_UNKNOWN,
        entry.metadata_available ? digits_font_ : text_font_);
    lv_obj_set_style_text_color(duration, lv_color_hex(kAccent), 0);
    char size[72];
    std::snprintf(size, sizeof(size), WATCH_LIBRARY_SIZE_FORMAT, static_cast<unsigned long long>(entry.size_bytes));
    Label(body_, size);
    std::tm local{};
    if (entry.started_at_unix_ms && WatchLocalTime(entry.started_at_unix_ms, state_.time.state.timezone, local)) {
        char date[40];
        std::snprintf(date, sizeof(date), "%04d-%02d-%02d  %02d:%02d", local.tm_year + 1900,
            local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
        Label(body_, date);
    } else Label(body_, WATCH_LIBRARY_DATE_UNKNOWN);
    Label(body_, entry.incomplete ? WATCH_RECORDING_INCOMPLETE :
        entry.metadata_available ? WATCH_RECORDER_SAVED : WATCH_LIBRARY_METADATA_UNAVAILABLE);
    Label(body_, WATCH_LIBRARY_SYNC_UNCONFIRMED);
    recording_delete_ = Button(body_, WATCH_RECORDING_DELETE, [this]() {
        if (CanDeleteRecording()) Navigate(WatchPage::kRecordingDeleteConfirm);
    });
}

void WatchView::PlaybackCommand(WatchViewAction action) {
    WatchViewRequest request;
    request.action = action;
    request.recording_id = selected_recording_id_;
    request.library_revision = state_.library.revision;
    request.playback_generation = state_.playback.generation;
    Submit(std::move(request));
}

void WatchView::BuildPlayback() {
    playback_name_ = Label(body_, "");
    playback_status_ = Label(body_, "");
    playback_position_ = Label(body_, "", digits_font_);
    lv_obj_set_style_text_color(playback_position_, lv_color_hex(kAccent), 0);
    playback_total_ = Label(body_, "");
    auto* seek_track = Row(body_, 40);
    lv_obj_set_style_pad_hor(seek_track, 18, 0);
    playback_seek_ = lv_slider_create(seek_track);
    lv_obj_set_size(playback_seek_, LV_PCT(100), 12);
    lv_slider_set_range(playback_seek_, 0, 1000);
    lv_obj_set_style_bg_color(playback_seek_, lv_color_hex(kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(playback_seek_, lv_color_hex(kAccent), LV_PART_KNOB);
    Bind(playback_seek_, LV_EVENT_RELEASED, [this](lv_event_t*) {
        if (lv_obj_has_state(playback_seek_, LV_STATE_DISABLED) || pending_request_) return;
        const auto fraction = static_cast<uint64_t>(lv_slider_get_value(playback_seek_));
        WatchViewRequest request;
        request.action = WatchViewAction::kSeekPlayback;
        request.playback_generation = state_.playback.generation;
        const auto total = state_.playback.total_samples;
        request.playback_target_sample = total / 1000 * fraction + total % 1000 * fraction / 1000;
        Submit(request);
    });
    auto* row = Row(body_, 56);
    playback_pause_ = Button(row, FONT_AWESOME_PAUSE, [this] {
        PlaybackCommand(state_.playback.state == RecordingPlaybackState::kPaused
            ? WatchViewAction::kResumePlayback : WatchViewAction::kPausePlayback);
    }, false, true);
    playback_stop_ = Button(row, FONT_AWESOME_STOP, [this] { PlaybackCommand(WatchViewAction::kStopPlayback); }, false, true);
    for (auto* button : {playback_pause_, playback_stop_}) {
        lv_obj_set_width(button, 1); lv_obj_set_flex_grow(button, 1);
    }
    auto* volume_row = Row(body_, 46);
    auto* volume_label = Label(volume_row, (std::string(FONT_AWESOME_VOLUME_HIGH) + "  " + Lang::Strings::WATCH_VOLUME).c_str(), icon_font_);
    lv_obj_set_width(volume_label, 1); lv_obj_set_flex_grow(volume_label, 1);
    playback_volume_value_ = Label(volume_row, "");
    lv_obj_set_width(playback_volume_value_, 76);
    lv_obj_set_style_text_align(playback_volume_value_, LV_TEXT_ALIGN_RIGHT, 0);
    auto* track = Row(body_, 54);
    lv_obj_set_style_pad_hor(track, 18, 0);
    playback_volume_ = lv_slider_create(track);
    lv_obj_set_size(playback_volume_, LV_PCT(100), 12);
    lv_slider_set_range(playback_volume_, 0, 100);
    lv_obj_set_style_bg_color(playback_volume_, lv_color_hex(kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(playback_volume_, lv_color_hex(kAccent), LV_PART_KNOB);
    Bind(playback_volume_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t*) {
        lv_label_set_text(playback_volume_value_, (std::to_string(lv_slider_get_value(playback_volume_)) + "%").c_str());
    });
    Bind(playback_volume_, LV_EVENT_RELEASED, [this](lv_event_t*) {
        if (lv_obj_has_state(playback_volume_, LV_STATE_DISABLED) || pending_request_) return;
        const int volume = lv_slider_get_value(playback_volume_);
        if (volume == state_.controls.volume) return;
        WatchViewRequest request;
        request.action = WatchViewAction::kSetPlaybackVolume;
        request.playback_generation = state_.playback.generation;
        request.level = WatchLevel::kVolume;
        request.level_value = volume;
        Submit(request);
    });
    Button(body_, Lang::Strings::WATCH_RECENT_RECORDINGS, [this] { Navigate(WatchPage::kRecordings); });
}

void WatchView::RefreshPlayback() {
    using namespace Lang::Strings;
    using State = RecordingPlaybackState;
    const auto& playback = state_.playback;
    const char* text = WATCH_PLAYBACK_EMPTY;
    switch (playback.state) {
        case State::kIdle: break;
        case State::kPreparing: text = WATCH_PLAYBACK_PREPARING; break;
        case State::kPlaying: text = WATCH_PLAYBACK_PLAYING; break;
        case State::kPausing: text = WATCH_PLAYBACK_PAUSING; break;
        case State::kPaused: text = WATCH_PLAYBACK_PAUSED; break;
        case State::kStopping: text = WATCH_PLAYBACK_STOPPING; break;
        case State::kEnded: text = WATCH_PLAYBACK_ENDED; break;
        case State::kStopped: text = WATCH_PLAYBACK_STOPPED; break;
        case State::kFailed: text = WATCH_PLAYBACK_FAILED; break;
        case State::kCleanupFailed: text = WATCH_PLAYBACK_CLEANUP_FAILED; break;
        case State::kSeeking: text = WATCH_PLAYBACK_SEEKING; break;
    }
    if (playback.state == State::kFailed) {
        if (playback.error == RecordingPlaybackError::kRead || playback.error == RecordingPlaybackError::kStorage)
            text = WATCH_PLAYBACK_READ_FAILED;
        else if (playback.error == RecordingPlaybackError::kDecode) text = WATCH_PLAYBACK_DECODE_FAILED;
        else if (playback.error == RecordingPlaybackError::kOutput) text = WATCH_PLAYBACK_OUTPUT_FAILED;
    }
    lv_label_set_text(playback_status_, text);
    lv_label_set_text(playback_name_, playback.recording_id.empty() ? "" : (playback.recording_id + ".opus").c_str());
    const uint64_t ms = playback.sample_rate ?
        playback.output_samples / playback.sample_rate * 1000 + playback.output_samples % playback.sample_rate * 1000 / playback.sample_rate : 0;
    lv_label_set_text(playback_position_, Duration(ms).c_str());
    const auto total_ms = playback.sample_rate ? playback.total_samples / playback.sample_rate * 1000 +
        playback.total_samples % playback.sample_rate * 1000 / playback.sample_rate : 0;
    lv_label_set_text(playback_total_, (std::string(WATCH_PLAYBACK_TOTAL) + ": " +
        (playback.duration_known ? Duration(total_ms) : "--:--:--")).c_str());
    lv_label_set_text(lv_obj_get_child(playback_pause_, 0), playback.state == State::kPaused ? FONT_AWESOME_PLAY : FONT_AWESOME_PAUSE);
    const bool available = handler_ && playback.generation && !state_.time.sleep_prepared;
    if (available && !pending_request_ && (playback.state == State::kPlaying || playback.state == State::kPaused))
        lv_obj_remove_state(playback_pause_, LV_STATE_DISABLED);
    else lv_obj_add_state(playback_pause_, LV_STATE_DISABLED);
    if (available && playback.resources_held && playback.state != State::kStopping)
        lv_obj_remove_state(playback_stop_, LV_STATE_DISABLED);
    else lv_obj_add_state(playback_stop_, LV_STATE_DISABLED);
    const bool can_seek = available && playback.resources_held && playback.duration_known && playback.total_samples &&
        !pending_request_ && (playback.state == State::kPlaying || playback.state == State::kPaused);
    if (can_seek) lv_obj_remove_state(playback_seek_, LV_STATE_DISABLED);
    else lv_obj_add_state(playback_seek_, LV_STATE_DISABLED);
    if (!lv_obj_has_state(playback_seek_, LV_STATE_PRESSED) &&
        !(pending_request_ && pending_action_ == WatchViewAction::kSeekPlayback)) {
        const int position = playback.total_samples ? static_cast<int>(std::min<long double>(1000,
            static_cast<long double>(playback.output_samples) * 1000 / playback.total_samples)) : 0;
        lv_slider_set_value(playback_seek_, position, LV_ANIM_OFF);
    }
    const bool can_volume = available && playback.resources_held && playback.volume_supported &&
        state_.controls.volume >= 0 && !pending_request_ && playback.volume.status != PlaybackVolumeStatus::kPending &&
        (playback.state == State::kPlaying || playback.state == State::kPaused);
    if (can_volume) lv_obj_remove_state(playback_volume_, LV_STATE_DISABLED);
    else lv_obj_add_state(playback_volume_, LV_STATE_DISABLED);
    if (!lv_obj_has_state(playback_volume_, LV_STATE_PRESSED) &&
        !(pending_request_ && pending_action_ == WatchViewAction::kSetPlaybackVolume)) {
        lv_slider_set_value(playback_volume_, std::max(0, state_.controls.volume), LV_ANIM_OFF);
        lv_label_set_text(playback_volume_value_, state_.controls.volume < 0 ? "--" :
            (std::to_string(state_.controls.volume) + "%").c_str());
    }
}

bool WatchView::CanDeleteRecording() const {
    if (!handler_ || pending_request_ || state_.time.sleep_prepared || !state_.can_delete_recording) return false;
    if (page_ == WatchPage::kRecordingDeleteConfirm && deletion_request_ &&
        state_.deletion.request == deletion_request_ && state_.deletion.result.status == RecordingDeleteStatus::kChanged)
        return false;
    if (page_ == WatchPage::kRecordingDeleteConfirm && deletion_request_ &&
        state_.deletion.request == deletion_request_ && state_.deletion.recording_id == deletion_entry_.id &&
        state_.deletion.cleanup_available) return true;
    if (state_.library.status != WatchLibraryStatus::kReady) return false;
    if (page_ == WatchPage::kRecordingDeleteConfirm && deletion_revision_ != state_.library.revision) return false;
    return std::any_of(state_.library.entries.begin(), state_.library.entries.end(), [this](const auto& entry) {
        return entry.id == (page_ == WatchPage::kRecordingDeleteConfirm ? deletion_entry_.id : selected_recording_id_);
    });
}

void WatchView::BuildRecordingDeleteConfirm() {
    using namespace Lang::Strings;
    Label(body_, deletion_entry_.file_name.c_str());
    Label(body_, deletion_entry_.audio_available ? WATCH_RECORDING_DELETE_WARNING : WATCH_RECORDING_METADATA_WARNING);
    recording_delete_status_ = Label(body_, "");
    recording_delete_ = Button(body_, WATCH_RECORDING_DELETE, [this]() {
        if (!CanDeleteRecording()) return;
        WatchViewRequest request;
        request.action = WatchViewAction::kDeleteRecording;
        request.recording_id = deletion_entry_.id;
        request.library_revision = deletion_revision_;
        request.metadata_only = !deletion_entry_.audio_available;
        if (deletion_request_ && state_.deletion.request == deletion_request_ &&
            state_.deletion.recording_id == deletion_entry_.id && state_.deletion.cleanup_available)
            request.cleanup_request = deletion_request_;
        if (request.cleanup_request) request.metadata_only = true;
        Submit(std::move(request));
    });
    recording_delete_cancel_ = Button(body_, WATCH_RECORDING_DELETE_CANCEL, [this]() {
        if (!pending_request_) Navigate(WatchPage::kRecordings);
    });
}

void WatchView::RefreshRecordingDelete() {
    using namespace Lang::Strings;
    if (CanDeleteRecording()) lv_obj_remove_state(recording_delete_, LV_STATE_DISABLED);
    else lv_obj_add_state(recording_delete_, LV_STATE_DISABLED);
    if (recording_delete_cancel_) {
        if (pending_request_) lv_obj_add_state(recording_delete_cancel_, LV_STATE_DISABLED);
        else lv_obj_remove_state(recording_delete_cancel_, LV_STATE_DISABLED);
    }
    if (!recording_delete_status_) return;
    const bool result_current = deletion_request_ && state_.deletion.request == deletion_request_ &&
        state_.deletion.recording_id == deletion_entry_.id;
    const auto status = result_current ? state_.deletion.result.status : RecordingDeleteStatus::kNone;
    const char* message = pending_request_ ? WATCH_WORKING :
        status == RecordingDeleteStatus::kPartial ? WATCH_RECORDING_DELETE_PARTIAL :
        status == RecordingDeleteStatus::kNotFound ? WATCH_RECORDING_DELETE_MISSING :
        status == RecordingDeleteStatus::kChanged || (!result_current && deletion_revision_ != state_.library.revision)
            ? WATCH_RECORDING_DELETE_CHANGED :
        status == RecordingDeleteStatus::kFailed ? WATCH_RECORDING_DELETE_FAILED :
        !state_.can_delete_recording ? WATCH_RECORDING_DELETE_BUSY : "";
    lv_label_set_text(recording_delete_status_, message);
    if (*message) lv_obj_remove_flag(recording_delete_status_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(recording_delete_status_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lv_obj_get_child(recording_delete_, 0),
        result_current && state_.deletion.cleanup_available ? WATCH_RECORDING_DELETE_RETRY :
        deletion_entry_.audio_available ? WATCH_RECORDING_DELETE : WATCH_RECORDING_METADATA_CLEANUP);
}

void WatchView::BuildRecorder() {
    lv_obj_remove_flag(body_, LV_OBJ_FLAG_SCROLLABLE);
    recorder_status_ = Label(body_, "");
    recorder_duration_ = Label(body_, "00:00:00", digits_font_);
    lv_obj_set_style_text_align(recorder_duration_, LV_TEXT_ALIGN_CENTER, 0);
    recorder_detail_ = Label(body_, "");
    lv_obj_set_height(recorder_detail_, 1);
    lv_obj_set_flex_grow(recorder_detail_, 1);
    lv_label_set_long_mode(recorder_detail_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(recorder_detail_, lv_color_hex(kMuted), 0);
    recorder_primary_ = Button(body_, "", [this]() {
        if (pending_request_) return;
        const auto command = state_.recorder.active ? (state_.recorder.paused
            ? WatchRecordingCommand::kResume : WatchRecordingCommand::kPause) : WatchRecordingCommand::kStart;
        if (state_.recorder.Can(command)) RecordingCommand(command);
    }, false, true);
    lv_obj_set_width(recorder_primary_, LV_PCT(100));
    auto* row = Row(body_, 48);
    recorder_stop_ = Button(row, "", [this]() { RecordingCommand(WatchRecordingCommand::kStop); }, false, true);
    recorder_highlight_ = Button(row, "", [this]() {
        if (!pending_request_ && state_.recorder.Can(WatchRecordingCommand::kHighlight)) {
            RecordingCommand(WatchRecordingCommand::kHighlight);
        }
    }, false, true);
    for (auto* button : {recorder_stop_, recorder_highlight_}) {
        lv_obj_set_width(button, 1);
        lv_obj_set_flex_grow(button, 1);
    }
    static const lv_style_prop_t properties[] = {LV_STYLE_BG_COLOR, LV_STYLE_OPA, LV_STYLE_RECOLOR_OPA, LV_STYLE_PROP_INV};
    static lv_style_transition_dsc_t immediate;
    lv_style_transition_dsc_init(&immediate, properties, lv_anim_path_linear, 0, 0, nullptr);
    for (auto* button : {recorder_primary_, recorder_stop_, recorder_highlight_}) {
        lv_obj_set_style_transition(button, &immediate, 0);
        lv_obj_set_style_transition(button, &immediate, LV_STATE_DISABLED);
        lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
        lv_obj_set_style_recolor_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_recolor_opa(button, LV_OPA_TRANSP, LV_STATE_DISABLED);
    }
}

void WatchView::RefreshRecorder() {
    using namespace Lang::Strings;
    const auto& recorder = state_.recorder;
    const char* status = recorder.control.starting ? WATCH_RECORDER_STARTING :
        recorder.stopping ? WATCH_RECORDER_STOPPING : recorder.transitioning ? WATCH_WORKING :
        recorder.paused ? WATCH_RECORDER_PAUSED : recorder.active ? WATCH_RECORDING :
        !recorder.storage_mounted ? WATCH_RECORDER_NO_STORAGE : recorder.resources_blocked ? WATCH_ASSISTANT_BUSY :
        WATCH_RECORDER_READY;
    lv_label_set_text(recorder_status_, status);
    lv_obj_set_style_text_color(recorder_status_, lv_color_hex(recorder.active ? kWarning : kAccent), 0);
    lv_label_set_text(recorder_duration_, Duration(recorder.duration_ms).c_str());
    std::string detail;
    switch (recorder.control.error) {
        case WatchRecordingError::kStartFailed: detail = WATCH_RECORDER_START_FAILED; break;
        case WatchRecordingError::kSaveFailed: detail = WATCH_RECORDER_SAVE_FAILED; break;
        case WatchRecordingError::kControlFailed: detail = WATCH_ACTION_FAILED; break;
        case WatchRecordingError::kNone:
            if (!recorder.active && !recorder.control.starting && recorder.last_saved) {
                detail = std::string(WATCH_RECORDER_SAVED) + "\n" + recorder.last_file;
            }
            break;
    }
    lv_label_set_text(recorder_detail_, detail.c_str());
    const auto primary = recorder.active ? (recorder.paused ? WatchRecordingCommand::kResume : WatchRecordingCommand::kPause)
        : WatchRecordingCommand::kStart;
    const std::string primary_text = std::string(primary == WatchRecordingCommand::kStart ? FONT_AWESOME_MICROPHONE :
        primary == WatchRecordingCommand::kPause ? FONT_AWESOME_PAUSE : FONT_AWESOME_PLAY) + "  " +
        (primary == WatchRecordingCommand::kStart ? WATCH_START : primary == WatchRecordingCommand::kPause ? WATCH_PAUSE : WATCH_RESUME);
    lv_label_set_text(lv_obj_get_child(recorder_primary_, 0), primary_text.c_str());
    const std::string stop = std::string(FONT_AWESOME_STOP) + "  " +
        (recorder.control.starting && !recorder.active ? WATCH_RECORDER_CANCEL : WATCH_RECORDER_STOP);
    lv_label_set_text(lv_obj_get_child(recorder_stop_, 0), stop.c_str());
    const std::string mark = std::string(FONT_AWESOME_LOCATION_DOT) + "  " + WATCH_RECORDER_MARK;
    lv_label_set_text(lv_obj_get_child(recorder_highlight_, 0), mark.c_str());
    const auto enable = [](lv_obj_t* object, bool enabled) {
        if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
        else lv_obj_add_state(object, LV_STATE_DISABLED);
    };
    const bool available = handler_ && !pending_request_ && !state_.time.sleep_prepared;
    enable(recorder_primary_, available && recorder.Can(primary));
    enable(recorder_highlight_, available && recorder.Can(WatchRecordingCommand::kHighlight));
    const bool pending_start = pending_request_ && pending_action_ == WatchViewAction::kRecordingCommand &&
        pending_recording_command_ == WatchRecordingCommand::kStart;
    const bool pending_stop = pending_request_ && pending_action_ == WatchViewAction::kRecordingCommand &&
        pending_recording_command_ == WatchRecordingCommand::kStop;
    enable(recorder_stop_, handler_ && !pending_stop && (recorder.Can(WatchRecordingCommand::kStop) || pending_start));
}

void WatchView::BuildAssistant() {
    lv_obj_remove_flag(body_, LV_OBJ_FLAG_SCROLLABLE);
    assistant_status_ = Label(body_, "");
    lv_obj_set_style_text_color(assistant_status_, lv_color_hex(kAccent), 0);
    assistant_history_ = lv_obj_create(body_);
    Plain(assistant_history_);
    lv_obj_set_size(assistant_history_, LV_PCT(100), 1);
    lv_obj_set_flex_grow(assistant_history_, 1);
    lv_obj_set_flex_flow(assistant_history_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(assistant_history_, 8, 0);
    lv_obj_set_style_pad_right(assistant_history_, 12, 0);
    lv_obj_set_scroll_dir(assistant_history_, LV_DIR_VER);
    Bind(assistant_history_, LV_EVENT_SCROLL_BEGIN, [](lv_event_t*) {});
    assistant_primary_ = Button(body_, "", [this]() {
        if (state_.assistant.phase == WatchAssistantPhase::kDisabled) {
            Navigate(WatchPage::kControls);
            return;
        }
        if (!pending_request_ && !assistant_click_handled_) {
            if (state_.assistant.can_connect) AssistantCommand(WatchViewAction::kPrepareAssistantNetwork);
            else if (state_.assistant.can_send) AssistantCommand(WatchViewAction::kAssistantSend);
        }
    }, false, true);
    Bind(assistant_primary_, LV_EVENT_PRESSED, [this](lv_event_t*) { AssistantPress(); });
    Bind(assistant_primary_, LV_EVENT_RELEASED, [this](lv_event_t*) { AssistantRelease(false); });
    Bind(assistant_primary_, LV_EVENT_PRESS_LOST, [this](lv_event_t*) { AssistantRelease(true); });
    lv_obj_set_size(assistant_primary_, LV_PCT(100), 52);
    auto* row = Row(body_, 48);
    assistant_end_ = Button(row, "", [this]() { AssistantCommand(WatchViewAction::kAssistantEnd); }, false, true);
    lv_obj_set_width(assistant_end_, 1);
    lv_obj_set_flex_grow(assistant_end_, 1);
    assistant_clear_ = Button(row, FONT_AWESOME_TRASH, [this]() {
        AssistantCommand(WatchViewAction::kAssistantClear);
    }, false, true);
    static const lv_style_prop_t immediate_properties[] = {LV_STYLE_BG_COLOR, LV_STYLE_BG_OPA, LV_STYLE_OPA,
        LV_STYLE_TEXT_OPA, LV_STYLE_BORDER_OPA, LV_STYLE_SHADOW_OPA, LV_STYLE_RECOLOR_OPA, LV_STYLE_PROP_INV};
    static lv_style_transition_dsc_t immediate;
    lv_style_transition_dsc_init(&immediate, immediate_properties, lv_anim_path_linear, 0, 0, nullptr);
    for (auto* button : {assistant_primary_, assistant_end_, assistant_clear_}) {
        lv_obj_set_style_transition(button, &immediate, 0);
        lv_obj_set_style_transition(button, &immediate, LV_STATE_DISABLED);
        lv_obj_set_style_transition(button, &immediate, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(button, lv_color_hex(kSurface), LV_STATE_DISABLED);
        lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
        lv_obj_set_style_recolor_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_recolor_opa(button, LV_OPA_TRANSP, LV_STATE_DISABLED);
        lv_obj_set_style_recolor_opa(button, LV_OPA_TRANSP, LV_STATE_PRESSED);
    }
    lv_obj_set_style_bg_color(assistant_primary_, lv_color_hex(kAccent), 0);
    lv_obj_set_style_bg_color(assistant_primary_, lv_color_hex(0x55CBAE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(assistant_primary_, lv_color_hex(kAccent), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(lv_obj_get_child(assistant_primary_, 0), lv_color_hex(kBackground), 0);
}

void WatchView::RefreshAssistant() {
    using namespace Lang::Strings;
    const auto& assistant = state_.assistant;
    const char* status = WATCH_ASSISTANT_READY;
    switch (assistant.phase) {
        case WatchAssistantPhase::kReady: break;
        case WatchAssistantPhase::kDisabled: status = WATCH_VOICE_OFF; break;
        case WatchAssistantPhase::kSetupRequired: status = WATCH_ASSISTANT_SETUP; break;
        case WatchAssistantPhase::kOffline: status = WATCH_ASSISTANT_OFFLINE; break;
        case WatchAssistantPhase::kBusy: status = WATCH_ASSISTANT_BUSY; break;
        case WatchAssistantPhase::kConnecting: status = WATCH_ASSISTANT_CONNECTING; break;
        case WatchAssistantPhase::kListening: status = WATCH_ASSISTANT_LISTENING; break;
        case WatchAssistantPhase::kThinking: status = WATCH_ASSISTANT_THINKING; break;
        case WatchAssistantPhase::kSpeaking: status = WATCH_ASSISTANT_SPEAKING; break;
        case WatchAssistantPhase::kStopping: status = WATCH_ASSISTANT_STOPPING; break;
        case WatchAssistantPhase::kFailed:
            switch (assistant.error) {
                case WatchAssistantError::kNone:
                case WatchAssistantError::kProtocol: status = WATCH_ASSISTANT_FAILED; break;
                case WatchAssistantError::kNetwork: status = WATCH_ASSISTANT_NETWORK_ERROR; break;
                case WatchAssistantError::kAudio: status = WATCH_ASSISTANT_AUDIO_ERROR; break;
                case WatchAssistantError::kTimeout: status = WATCH_ASSISTANT_TIMEOUT; break;
            }
            break;
    }
    lv_label_set_text(assistant_status_, status);
    lv_obj_set_style_text_color(assistant_status_, lv_color_hex(
        assistant.phase == WatchAssistantPhase::kFailed || assistant.phase == WatchAssistantPhase::kBusy ? kWarning : kAccent), 0);
    const char* primary_label = assistant.phase == WatchAssistantPhase::kDisabled ? WATCH_CONTROLS : assistant_press_id_ ? (assistant.can_send ? WATCH_ASSISTANT_RELEASE :
        assistant.phase == WatchAssistantPhase::kThinking ? WATCH_ASSISTANT_THINKING :
        assistant.phase == WatchAssistantPhase::kSpeaking ? WATCH_ASSISTANT_SPEAKING : WATCH_ASSISTANT_CONNECTING) :
        assistant.can_connect ? WATCH_ASSISTANT_CONNECT : assistant.phase == WatchAssistantPhase::kConnecting ? WATCH_ASSISTANT_CONNECTING :
        assistant.can_send ? WATCH_ASSISTANT_SEND :
        assistant.can_interrupt ? WATCH_ASSISTANT_HOLD_INTERRUPT : WATCH_ASSISTANT_HOLD;
    const std::string primary = std::string(assistant.phase == WatchAssistantPhase::kDisabled ? FONT_AWESOME_GEAR :
        assistant.can_connect || (assistant.phase == WatchAssistantPhase::kConnecting && !assistant_press_id_) ? FONT_AWESOME_WIFI :
        assistant.can_send && !assistant_press_id_ ? FONT_AWESOME_ARROW_UP : FONT_AWESOME_MICROPHONE) +
        "  " + primary_label;
    lv_label_set_text(lv_obj_get_child(assistant_primary_, 0), primary.c_str());
    const std::string end = std::string(FONT_AWESOME_STOP) + "  " + WATCH_ASSISTANT_END;
    lv_label_set_text(lv_obj_get_child(assistant_end_, 0), end.c_str());
    const auto enable = [](lv_obj_t* object, bool enabled) {
        if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
        else lv_obj_add_state(object, LV_STATE_DISABLED);
    };
    const bool can_request = handler_ && !state_.time.sleep_prepared;
    // Keep the pressed widget enabled until RELEASED/PRESS_LOST is delivered.
    enable(assistant_primary_, can_request && (assistant_press_id_ || (!pending_request_ &&
        (assistant.can_connect || assistant.can_talk || assistant.can_send || assistant.can_interrupt || assistant.phase == WatchAssistantPhase::kDisabled))));
    enable(assistant_end_, handler_ && (assistant.can_end ||
        (pending_request_ && pending_action_ != WatchViewAction::kAssistantEnd)));
    enable(assistant_clear_, can_request && !pending_request_ && !assistant.can_end &&
        assistant.phase != WatchAssistantPhase::kStopping && !assistant.messages.empty());
    if (assistant_rendered_revision_ == assistant.revision) return;
    const bool follow = assistant_rendered_revision_ == UINT64_MAX || lv_obj_get_scroll_bottom(assistant_history_) < 12;
    assistant_rendered_revision_ = assistant.revision;
    const int old_scroll = lv_obj_get_scroll_y(assistant_history_);
    lv_obj_clean(assistant_history_);
    if (assistant.messages.empty()) {
        auto* empty = Label(assistant_history_, WATCH_ASSISTANT_EMPTY);
        lv_obj_set_style_text_color(empty, lv_color_hex(kMuted), 0);
    }
    lv_obj_t* last = nullptr;
    for (const auto& message : assistant.messages) {
        const auto color = message.role == WatchAssistantRole::kUser ? kAccent : kMuted;
        auto* role = Label(assistant_history_, message.role == WatchAssistantRole::kUser ? WATCH_ASSISTANT_YOU : WATCH_ASSISTANT_NAME);
        lv_obj_set_style_text_color(role, lv_color_hex(color), 0);
        last = Label(assistant_history_, message.text.c_str());
    }
    lv_obj_update_layout(assistant_history_);
    if (follow && last) lv_obj_scroll_to_view(last, LV_ANIM_OFF);
    else lv_obj_scroll_to_y(assistant_history_, old_scroll, LV_ANIM_OFF);
}

void WatchView::BuildTimer() {
    using namespace Lang::Strings;
    const auto& time = state_.time.state.checkpoint;
    if (time.timer_configured) {
        digits_ = Label(body_, "", digits_font_);
        lv_obj_set_style_text_align(digits_, LV_TEXT_ALIGN_CENTER, 0);
        if (time.timer_running) {
            Button(body_, WATCH_PAUSE, [this]() { Command(WatchTimeCommand::kPauseTimer); }, true);
        } else if (time.timer_remaining_ms > 0) {
            Button(body_, WATCH_RESUME, [this]() { Command(WatchTimeCommand::kResumeTimer); }, true);
        }
        Button(body_, WATCH_CANCEL_TIMER, [this]() { Command(WatchTimeCommand::kCancelTimer); }, true);
        return;
    }
    auto* headings = Row(body_, 44);
    for (const auto* title : {WATCH_HOURS, WATCH_MINUTES, WATCH_SECONDS}) {
        auto* label = Label(headings, title); lv_obj_set_width(label, LV_PCT(30));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    auto* row = Row(body_, 116);
    hour_ = NumberPicker(row, 168, duration_draft_[0]);
    minute_ = NumberPicker(row, 59, duration_draft_[1]);
    second_ = NumberPicker(row, 59, duration_draft_[2]);
    Button(body_, WATCH_START, [this]() {
        duration_draft_ = {NumberValue(hour_), NumberValue(minute_), NumberValue(second_)};
        const uint64_t seconds = duration_draft_[0] * 3600ULL + duration_draft_[1] * 60ULL + duration_draft_[2];
        if (!seconds || seconds * 1000 > WatchTimeTools::kMaximumTimerMs) {
            failed_ = true; RefreshCommandState(); return;
        }
        Command(WatchTimeCommand::kStartTimer, seconds * 1000);
    }, true);
    auto* presets = Row(body_);
    for (const unsigned minutes : {1U, 5U, 15U}) {
        char text[32]; std::snprintf(text, sizeof(text), WATCH_MINUTE_PRESET, minutes);
        auto* button = Button(presets, text, [this, minutes]() { Command(WatchTimeCommand::kStartTimer, minutes * 60000); }, true);
        lv_obj_set_width(button, 1);
        lv_obj_set_flex_grow(button, 1);
        lv_obj_set_style_pad_hor(button, 4, 0);
    }
}

void WatchView::BuildStopwatch() {
    using namespace Lang::Strings;
    digits_ = Label(body_, "", digits_font_);
    lv_obj_set_style_text_align(digits_, LV_TEXT_ALIGN_CENTER, 0);
    const bool running = state_.time.state.checkpoint.stopwatch_running;
    Button(body_, running ? WATCH_PAUSE : WATCH_START, [this, running]() {
        Command(running ? WatchTimeCommand::kPauseStopwatch : WatchTimeCommand::kStartStopwatch);
    }, true);
    if (running) {
        auto* lap = Button(body_, WATCH_LAP, [this]() { Command(WatchTimeCommand::kAddLap); }, true);
        if (state_.time.state.checkpoint.laps.size() >= WatchTimeTools::kMaximumLaps) {
            commands_.pop_back(); lv_obj_add_state(lap, LV_STATE_DISABLED);
        }
    } else {
        Button(body_, WATCH_RESET, [this]() { Command(WatchTimeCommand::kResetStopwatch); }, true);
    }
    for (size_t index = 0; index < state_.time.state.checkpoint.laps.size(); ++index) {
        char text[80];
        std::snprintf(text, sizeof(text), WATCH_LAP_FORMAT, static_cast<unsigned>(index + 1),
            Duration(state_.time.state.checkpoint.laps[index], true).c_str());
        Label(body_, text);
    }
}

void WatchView::BuildAlarms() {
    for (size_t index = 0; index < state_.time.state.checkpoint.alarms.size(); ++index) {
        const auto& alarm = state_.time.state.checkpoint.alarms[index];
        char text[80];
        std::snprintf(text, sizeof(text), "%02u:%02u  %s", alarm.hour, alarm.minute,
            alarm.enabled ? Lang::Strings::WATCH_ON : Lang::Strings::WATCH_OFF);
        Button(body_, text, [this, index]() {
            alarm_index_ = index;
            alarm_draft_ = state_.time.state.checkpoint.alarms[index];
            Navigate(WatchPage::kAlarmEditor);
        });
    }
}

void WatchView::BuildAlarmEditor() {
    using namespace Lang::Strings;
    auto* row = Row(body_, 116);
    hour_ = NumberPicker(row, 23, alarm_draft_.hour);
    minute_ = NumberPicker(row, 59, alarm_draft_.minute);
    auto* enabled_row = Row(body_);
    auto* label = Label(enabled_row, WATCH_ENABLED); lv_obj_set_flex_grow(label, 1); lv_obj_set_width(label, 1);
    enabled_ = lv_switch_create(enabled_row); commands_.push_back(enabled_);
    Bind(enabled_, LV_EVENT_VALUE_CHANGED, [](lv_event_t*) {});
    if (alarm_draft_.enabled) { lv_obj_add_state(enabled_, LV_STATE_CHECKED); }
    Label(body_, WATCH_REPEAT);
    const std::array<const char*, 7> days{WATCH_SUNDAY, WATCH_MONDAY, WATCH_TUESDAY, WATCH_WEDNESDAY,
        WATCH_THURSDAY, WATCH_FRIDAY, WATCH_SATURDAY};
    auto* days_row = Row(body_, 104);
    lv_obj_set_flex_flow(days_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(days_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (size_t day = 0; day < days.size(); ++day) {
        weekdays_[day] = Button(days_row, days[day], []() {}, true);
        lv_obj_set_width(weekdays_[day], LV_PCT(22));
        lv_obj_add_flag(weekdays_[day], LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_style_bg_color(weekdays_[day], lv_color_hex(0x305A4D), LV_STATE_CHECKED);
        if (alarm_draft_.weekdays & (1U << day)) { lv_obj_add_state(weekdays_[day], LV_STATE_CHECKED); }
    }
    Button(body_, WATCH_SAVE, [this]() {
        CaptureDraft();
        WatchViewRequest request;
        request.action = WatchViewAction::kSetAlarm; request.alarm_index = alarm_index_; request.alarm = alarm_draft_;
        Submit(request);
    }, true);
}

void WatchView::BuildSettings() {
    using namespace Lang::Strings;
    if (state_.firmware.available) Button(body_, WATCH_FIRMWARE, [this] { Navigate(WatchPage::kFirmwareUpdate); });
    Label(body_, WATCH_TIME_ZONE);
    size_t count = 0; const auto* zones = WatchTimeZones(count);
    std::string options; uint32_t selected = 0;
    for (size_t index = 0; index < count; ++index) {
        if (index) { options += '\n'; }
        const char* label = TimeZoneLabel(zones[index].id);
        options += label ? label : zones[index].id;
        if (timezone_draft_ == zones[index].id) { selected = index; }
    }
    zone_ = lv_dropdown_create(body_);
    lv_dropdown_set_symbol(zone_, FONT_AWESOME_ANGLE_DOWN);
    lv_obj_set_style_text_font(zone_, icon_font_, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(zone_, lv_color_hex(kSurface), 0);
    lv_obj_set_style_text_color(zone_, lv_color_hex(kText), 0);
    auto* list = lv_dropdown_get_list(zone_);
    lv_obj_set_style_text_font(list, text_font_, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(kSurface), 0);
    lv_obj_set_style_text_color(list, lv_color_hex(kText), 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x35453F), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_max_height(list, 260, 0);
    lv_obj_set_width(zone_, LV_PCT(100));
    lv_dropdown_set_options(zone_, options.c_str()); lv_dropdown_set_selected(zone_, selected);
    commands_.push_back(zone_);
    Bind(zone_, LV_EVENT_VALUE_CHANGED, [](lv_event_t*) {});
    auto* row = Row(body_);
    auto* label = Label(row, WATCH_24_HOUR); lv_obj_set_flex_grow(label, 1); lv_obj_set_width(label, 1);
    format_ = lv_switch_create(row); commands_.push_back(format_);
    Bind(format_, LV_EVENT_VALUE_CHANGED, [](lv_event_t*) {});
    if (hour24_draft_) { lv_obj_add_state(format_, LV_STATE_CHECKED); }
    Button(body_, WATCH_SAVE, [this]() {
        size_t count = 0; const auto* zones = WatchTimeZones(count);
        const auto selected = lv_dropdown_get_selected(zone_);
        if (selected >= count) { return; }
        WatchViewRequest request; request.action = WatchViewAction::kSetPreferences;
        request.timezone = zones[selected].id; request.hour24 = lv_obj_has_state(format_, LV_STATE_CHECKED);
        Submit(request);
    }, true);
}

void WatchView::RefreshLabels() {
    using namespace Lang::Strings;
    if (page_ == WatchPage::kActivity) { RefreshActivity(); return; }
    if (page_ == WatchPage::kActivityHistory) { RefreshActivityHistory(); return; }
    const auto& time = state_.time.state.checkpoint;
    if (page_ == WatchPage::kFace) {
        std::tm local{};
        if (WatchLocalTime(state_.clock.unix_ms, state_.time.state.timezone, local)) {
            char clock[24];
            const int hour = state_.time.state.hour24 ? local.tm_hour : (local.tm_hour % 12 ? local.tm_hour % 12 : 12);
            std::snprintf(clock, sizeof(clock), "%02d:%02d", hour, local.tm_min);
            lv_label_set_text(digits_, clock);
            char date[64];
            std::snprintf(date, sizeof(date), "%04d-%02d-%02d%s%s", local.tm_year + 1900, local.tm_mon + 1,
                local.tm_mday, state_.time.state.hour24 ? "" : "  ",
                state_.time.state.hour24 ? "" : local.tm_hour < 12 ? WATCH_AM : WATCH_PM);
            lv_label_set_text(detail_, date);
        } else {
            lv_label_set_text(digits_, "--:--"); lv_label_set_text(detail_, WATCH_TIME_UNSYNCED);
        }
        char battery[80];
        if (state_.battery_percent < 0) { std::snprintf(battery, sizeof(battery), "%s", WATCH_BATTERY_UNKNOWN); }
        else { std::snprintf(battery, sizeof(battery), state_.charging ? WATCH_CHARGING_FORMAT : WATCH_BATTERY_FORMAT,
            std::clamp(state_.battery_percent, 0, 100)); }
        lv_label_set_text(battery_, battery);
        std::string connections;
        if (state_.wifi_connected) { connections += FONT_AWESOME_WIFI; }
        if (state_.ble_connected) { connections += "  " FONT_AWESOME_BLUETOOTH; }
        lv_label_set_text(connections_, connections.c_str());
        const char* recording_status = state_.recorder.stopping ? WATCH_RECORDER_STOPPING :
            state_.recorder.control.starting ? WATCH_RECORDER_STARTING : state_.recorder.paused ? WATCH_RECORDER_PAUSED : WATCH_RECORDING;
        std::string record = state_.recording ? std::string(recording_status) + "  " + Duration(state_.recording_ms) :
            state_.recorder.control.starting ? WATCH_RECORDER_STARTING : "";
        lv_label_set_text(record_, record.c_str());
    } else if (digits_ && page_ == WatchPage::kTimer) {
        lv_label_set_text(digits_, Duration(time.timer_remaining_ms).c_str());
    } else if (digits_ && page_ == WatchPage::kStopwatch) {
        lv_label_set_text(digits_, Duration(time.stopwatch_elapsed_ms).c_str());
    }
}
