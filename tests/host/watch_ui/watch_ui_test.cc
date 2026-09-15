#include "watch/watch_view.h"
#include "watch/watch_motion_task.h"
#include "assets/lang_config.h"
#include <font_awesome.h>
#include <src/misc/lv_text_private.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <csignal>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>

LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_awesome_30_4);
using namespace Lang::Strings;

namespace {
void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void Tick() { lv_tick_inc(50); lv_timer_handler(); }
void CheckLabelBounds(lv_obj_t* object) {
    if (lv_obj_check_type(object, &lv_label_class) && !lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) {
        auto* parent = lv_obj_get_parent(object);
        Check(lv_obj_get_width(object) <= lv_obj_get_width(parent), "label width fits its parent");
        if (!lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE)) {
            Check(lv_obj_get_height(object) <= lv_obj_get_height(parent), "label height fits its non-scrolling parent");
        }
        if (lv_obj_check_type(parent, &lv_button_class)) {
            if (lv_obj_get_height(object) > lv_obj_get_content_height(parent)) {
                std::cerr << "Clipped button: " << lv_label_get_text(object)
                          << " label_height=" << lv_obj_get_height(object)
                          << " content_height=" << lv_obj_get_content_height(parent) << '\n';
            }
            Check(lv_obj_get_height(object) <= lv_obj_get_content_height(parent), "button label fits without clipping");
        }
        const auto* text = lv_label_get_text(object);
        const auto* font = lv_obj_get_style_text_font(object, LV_PART_MAIN);
        uint32_t offset = 0;
        while (text[offset]) {
            const uint32_t letter = lv_text_encoded_next(text, &offset);
            if (letter < 32) { continue; }
            lv_font_glyph_dsc_t glyph{};
            if (!lv_font_get_glyph_dsc(font, &glyph, letter, 0) || glyph.is_placeholder) {
                std::cerr << "Missing glyph U+" << std::hex << letter << " in " << text << '\n';
                Check(false, "all rendered labels have real glyphs");
            }
        }
    }
    for (uint32_t index = 0; index < lv_obj_get_child_count(object); ++index) {
        CheckLabelBounds(lv_obj_get_child(object, index));
    }
}
lv_obj_t* FindLabel(lv_obj_t* parent, const std::string& text) {
    if (lv_obj_check_type(parent, &lv_label_class) && text == lv_label_get_text(parent)) { return parent; }
    for (uint32_t index = 0; index < lv_obj_get_child_count(parent); ++index) {
        if (auto* result = FindLabel(lv_obj_get_child(parent, index), text)) { return result; }
    }
    return nullptr;
}
lv_obj_t* FindButton(lv_obj_t* object, const std::string& text) {
    if (lv_obj_check_type(object, &lv_button_class) && FindLabel(object, text)) return object;
    for (uint32_t index = 0; index < lv_obj_get_child_count(object); ++index)
        if (auto* button = FindButton(lv_obj_get_child(object, index), text)) return button;
    return nullptr;
}
void Click(WatchView& view, const std::string& text) {
    auto* button = FindButton(view.Root(), text);
    if (!button) std::cerr << "Missing button: " << text << '\n';
    Check(button != nullptr, "button text exists");
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    Tick();
}
void CollectSliders(lv_obj_t* parent, std::vector<lv_obj_t*>& sliders) {
    if (lv_obj_check_type(parent, &lv_slider_class)) sliders.push_back(parent);
    for (uint32_t index = 0; index < lv_obj_get_child_count(parent); ++index)
        CollectSliders(lv_obj_get_child(parent, index), sliders);
}
struct Storage : WatchTimeStorage {
    bool fail = false;
    WatchStateLoad Load(std::string&) override { return WatchStateLoad::kAbsent; }
    bool Save(const std::string&) override { return !fail; }
};
void Screenshot(lv_obj_t* object, const std::filesystem::path& path) {
    lv_obj_update_layout(object);
    CheckLabelBounds(object);
    auto* buffer = lv_snapshot_take(object, LV_COLOR_FORMAT_RGB888);
    Check(buffer != nullptr, "LVGL snapshot rendered");
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << buffer->header.w << ' ' << buffer->header.h << "\n255\n";
    size_t lit = 0;
    for (uint32_t y = 0; y < buffer->header.h; ++y) {
        const auto* row = buffer->data + y * buffer->header.stride;
        for (uint32_t x = 0; x < buffer->header.w; ++x) {
            const char rgb[3]{static_cast<char>(row[x * 3 + 2]), static_cast<char>(row[x * 3 + 1]), static_cast<char>(row[x * 3])};
            output.write(rgb, 3);
            if (row[x * 3] > 80 || row[x * 3 + 1] > 80 || row[x * 3 + 2] > 80) { ++lit; }
        }
    }
    Check(lit > 500, "render has visible nonblank content");
    lv_draw_buf_destroy(buffer);
}

void TestPixelShift(WatchViewState state, const std::filesystem::path& screenshots) {
    WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48,
        &font_awesome_30_4, [](const WatchViewRequest&) {});
    view.Update(state); Tick();
    char battery_text[80]; std::snprintf(battery_text, sizeof(battery_text), WATCH_BATTERY_FORMAT, state.battery_percent);
    auto* label = FindLabel(view.Root(), battery_text);
    auto* recorder = FindButton(view.Root(), WATCH_RECORDER);
    Check(label && recorder, "pixel shift tests the actual watch face");
    lv_obj_update_layout(view.Root());
    const auto button_x = lv_obj_get_x(recorder), button_y = lv_obj_get_y(recorder);
    const auto label_width = lv_obj_get_width(label), label_height = lv_obj_get_height(label);
    constexpr int offsets[][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}, {-2, 2}, {-2, 0}, {-2, -2}, {0, -2}, {2, -2}};
    for (unsigned index = 0; index < 9; ++index) {
        lv_tick_inc(60000U - lv_tick_get() % 60000U); Tick();
        const auto& offset = offsets[(lv_tick_get() / 60000U) % 9];
        lv_obj_update_layout(view.Root());
        Check(lv_obj_get_style_translate_x(label, LV_PART_MAIN) == offset[0] &&
            lv_obj_get_style_translate_y(label, LV_PART_MAIN) == offset[1], "passive text follows bounded minute offsets");
        Check(lv_obj_get_width(label) == label_width && lv_obj_get_height(label) == label_height,
            "pixel shift does not resize text or change wrapping");
        Check(lv_obj_get_x(recorder) == button_x && lv_obj_get_y(recorder) == button_y &&
            lv_obj_get_style_translate_x(recorder, LV_PART_MAIN) == 0 && lv_obj_get_style_translate_y(recorder, LV_PART_MAIN) == 0,
            "pixel shift keeps interactive hit targets stable");
        CheckLabelBounds(view.Root());
    }
    Screenshot(view.Root(), screenshots / "face-pixel-shift.ppm");
    const auto previous_x = lv_obj_get_style_translate_x(label, LV_PART_MAIN);
    const auto previous_y = lv_obj_get_style_translate_y(label, LV_PART_MAIN);
    view.SetVisible(false); lv_tick_inc(60000); Tick();
    Check(lv_obj_get_style_translate_x(label, LV_PART_MAIN) == previous_x && lv_obj_get_style_translate_y(label, LV_PART_MAIN) == previous_y,
        "sleep pauses pixel-shift rendering");
    view.SetVisible(true); Tick();
    view.Navigate(WatchPage::kControls); Tick(); Click(view, FONT_AWESOME_LOCK);
    auto* locked = FindLabel(view.Root(), WATCH_TOUCH_LOCKED);
    auto* unlock = FindButton(view.Root(), WATCH_TOUCH_UNLOCK);
    lv_tick_inc(60000); Tick();
    const auto& offset = offsets[(lv_tick_get() / 60000U) % 9];
    Check(locked && unlock && lv_obj_get_style_translate_x(locked, LV_PART_MAIN) == offset[0] &&
        lv_obj_get_style_translate_y(locked, LV_PART_MAIN) == offset[1], "lock-screen static labels also shift");
    Check(lv_obj_get_style_translate_x(unlock, LV_PART_MAIN) == 0 && lv_obj_get_style_translate_y(unlock, LV_PART_MAIN) == 0,
        "unlock target remains fixed");
    std::cout << "PASS: passive pixel shift, fixed input targets and sleep suspension\n";
}

void TestTouchLock(WatchViewState state, const std::filesystem::path& screenshots) {
    std::vector<WatchViewRequest> requests;
    WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48,
        &font_awesome_30_4, [&](const WatchViewRequest& request) { requests.push_back(request); });
    view.Update(state);
    view.Navigate(WatchPage::kControls); Tick();
    auto* page_root = view.Root();
    auto* hidden_navigation = FindButton(page_root, WATCH_TIME_SETTINGS);
    auto* lock = FindButton(page_root, FONT_AWESOME_LOCK);
    Check(lock && hidden_navigation, "control center exposes touch lock without a new application command");
    lv_obj_send_event(lock, LV_EVENT_CLICKED, nullptr);
    Check(view.TouchLocked() && lv_obj_has_flag(page_root, LV_OBJ_FLAG_HIDDEN), "touch lock hides the page immediately");
    lv_obj_send_event(hidden_navigation, LV_EVENT_CLICKED, nullptr);
    Check(view.Page() == WatchPage::kControls && requests.empty(), "queued page actions cannot pass through the lock");
    Tick();
    auto* unlock = FindButton(view.Root(), WATCH_TOUCH_UNLOCK);
    Check(unlock != nullptr, "lock surface has an explicit unlock command");
    const auto event = [&](lv_event_code_t code) { lv_obj_send_event(unlock, code, nullptr); };
    event(LV_EVENT_CLICKED); event(LV_EVENT_LONG_PRESSED); event(LV_EVENT_RELEASED);
    Check(view.TouchLocked(), "clicks and synthetic long-press notifications cannot unlock");
    event(LV_EVENT_PRESSED); lv_tick_inc(1999); event(LV_EVENT_RELEASED);
    Check(view.TouchLocked(), "short hold cannot unlock");
    event(LV_EVENT_PRESSED); lv_tick_inc(2000); event(LV_EVENT_PRESS_LOST); event(LV_EVENT_RELEASED);
    Check(view.TouchLocked(), "dragging away cancels the unlock hold");
    event(LV_EVENT_PRESSED); lv_tick_inc(2000);
    view.SetVisible(false); event(LV_EVENT_RELEASED);
    view.SetVisible(true); event(LV_EVENT_RELEASED); Tick();
    Check(view.TouchLocked() && !lv_obj_has_flag(view.Root(), LV_OBJ_FLAG_HIDDEN) &&
        lv_obj_has_flag(page_root, LV_OBJ_FLAG_HIDDEN), "sleep and wake retain the lock but discard the old hold");
    event(LV_EVENT_PRESSED); lv_tick_inc(2000);
    state.time.sleep_prepared = true; view.Update(state);
    state.time.sleep_prepared = false; view.Update(state);
    event(LV_EVENT_RELEASED);
    Check(view.TouchLocked(), "sleep preparation cancels a hold even between render ticks");
    state.recording = true; view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_RECORDING), "external recording status remains visible while touch is locked");
    Screenshot(view.Root(), screenshots / "touch-locked-recording.ppm");
    state.time.state.checkpoint.timer_alert = true; view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_TIMER_FINISHED), "timer alerts remain visible without unlocking touch");
    event(LV_EVENT_PRESSED); lv_tick_inc(1000); Tick();
    Screenshot(view.Root(), screenshots / "touch-unlock-progress.ppm");
    lv_tick_inc(1000); Tick();
    Check(view.TouchLocked(), "completed hold stays locked until release");
    event(LV_EVENT_RELEASED); event(LV_EVENT_CLICKED);
    Check(!view.TouchLocked() && view.Root() == page_root && requests.empty(),
        "unlock does not dispatch recording, navigation or cloud commands");
    Tick();
    Check(view.Page() == WatchPage::kControls, "unlock preserves the underlying page");
    Click(view, FONT_AWESOME_LOCK);
    event(LV_EVENT_PRESSED);
    // A wrapped LVGL tick is elapsed time, not a wall-clock comparison.
    const auto now = lv_tick_get();
    lv_tick_inc(UINT32_MAX - now - 999);
    event(LV_EVENT_PRESS_LOST); event(LV_EVENT_PRESSED);
    lv_tick_inc(2000); event(LV_EVENT_RELEASED);
    Check(!view.TouchLocked(), "unlock hold handles tick wraparound");
    Check(requests.empty(), "touch lock never changes the hardware recording path");
    std::cout << "PASS: touch lock admission, sleep, hold cancellation, status and release isolation\n";
}

void TestDevicePairingUi(WatchViewState state, const std::filesystem::path& screenshots) {
    using Phase = watch_voice::PairingPhase;
    using Error = watch_voice::PairingError;
    state.time = {};
    state.notifications = {};
    state.alert_sound = {};
    std::vector<WatchViewRequest> requests;
    WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48,
        &font_awesome_30_4, [&](const WatchViewRequest& request) { requests.push_back(request); });
    view.Update(state);
    view.Navigate(WatchPage::kControls); Tick();
    Check(!FindButton(view.Root(), WATCH_PAIR_DEVICE), "unwired pairing capability has no misleading entry");
    view.Navigate(WatchPage::kDevicePairing); Tick();
    Check(FindLabel(view.Root(), WATCH_PAIR_UNAVAILABLE) && !FindButton(view.Root(), WATCH_PAIR_START),
        "direct navigation cannot offer unsupported pairing");
    watch_voice::PairingState controller;
    WatchAncsClientSnapshot peer;
    const auto update = [&](bool active = true, bool can_open = false) {
        state.pairing = BuildWatchPairingSnapshot(controller, true, active, can_open, peer);
        view.Update(state); Tick();
    };
    view.Navigate(WatchPage::kControls); Tick();
    update(false, true);
    Check(FindButton(view.Root(), WATCH_PAIR_DEVICE), "new pairing capability refreshes the open control center");
    Click(view, WATCH_PAIR_DEVICE);
    Check(view.Page() == WatchPage::kDevicePairing, "control center opens device pairing");
    state.pairing.can_open = false;
    state.pairing.network_required = true;
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_PAIR_NETWORK_REQUIRED), "offline pairing explains the existing Wi-Fi prerequisite");
    const auto offline_count = requests.size();
    Click(view, WATCH_PAIR_START);
    Check(requests.size() == offline_count, "offline pairing cannot submit a fake opening");
    Screenshot(view.Root(), screenshots / "device-pairing-network.ppm");
    update(false, true);
    Click(view, WATCH_PAIR_START);
    Check(requests.size() == 1 && requests.back().action == WatchViewAction::kOpenDevicePairing && requests.back().pairing_control_only,
        "pairing starts only with a physical command and does not require initialized timer storage");
    const auto open_id = requests.back().id;
    view.Navigate(WatchPage::kFace); Tick();
    Check(requests.back().action == WatchViewAction::kCancelDevicePairing &&
        requests.back().pairing_open_request == open_id, "leaving cancels a queued opening by request identity");
    view.Complete(open_id, true);
    Check(FindLabel(view.Root(), WATCH_WORKING), "late open acknowledgement cannot clear cancellation");
    view.Complete(requests.back().id, true);
    controller.attempt = 12;
    controller.phase = Phase::kAwaitPeer;
    controller.device_id = "1666f423-5647-4a0b-98b4-475e12514bc5";
    peer.confirmation = 20;
    peer.state = WatchAncsLinkState::kPeerConsent;
    peer.device_pairing_peer = false;
    update(); view.Navigate(WatchPage::kDevicePairing); Tick();
    Check(!FindButton(view.Root(), WATCH_NOTIFY_ALLOW), "notification peer cannot receive device consent");
    peer.device_pairing_peer = true;
    update();
    Click(view, WATCH_NOTIFY_ALLOW);
    Check(requests.back().action == WatchViewAction::kConfirmDevicePairingPeer &&
        requests.back().pairing_attempt == 12 && requests.back().pairing_confirmation == 20 &&
        requests.back().pairing_request == 0 && requests.back().enabled,
        "device peer approval is distinct from account and notification permissions");
    view.Complete(requests.back().id, true);
    peer.confirmation = 21;
    peer.state = WatchAncsLinkState::kConfirmNumber;
    peer.passkey = 42;
    update();
    Check(FindLabel(view.Root(), "000042"), "numeric comparison preserves leading zeros");
    Screenshot(view.Root(), screenshots / "device-pairing-code.ppm");
    Click(view, WATCH_NOTIFY_MATCH);
    const auto confirm_id = requests.back().id;
    Check(requests.back().pairing_confirmation == 21 && requests.back().pairing_attempt == 12,
        "code confirmation carries its rendered attempt and peer token");
    const auto before_duplicate = requests.size();
    Click(view, WATCH_NOTIFY_MATCH);
    Check(requests.size() == before_duplicate, "pending numeric approval cannot be submitted twice");
    Click(view, WATCH_NOTIFY_REJECT);
    Check(!requests.back().enabled && requests.back().pairing_confirmation == 21,
        "rejection remains available during pending approval");
    view.Complete(confirm_id, true);
    view.Complete(requests.back().id, true);
    peer.passkey = 1000000;
    update();
    Check(!FindButton(view.Root(), WATCH_NOTIFY_MATCH), "invalid comparison code cannot be approved");
    peer = {};
    controller.phase = Phase::kAwaitApproval;
    controller.offer = {31, controller.device_id, "b3fb1891-38c8-4332-9109-a55258194c4a",
        "da1a93c1-6a88-4920-b4b6-602ee1314f18"};
    update();
    Check(FindLabel(view.Root(), controller.offer.user_id), "account approval shows the exact challenged account");
    auto* body = lv_obj_get_parent(FindLabel(view.Root(), controller.offer.user_id));
    lv_obj_update_layout(body);
    lv_obj_scroll_to_y(body, 1000, LV_ANIM_OFF);
    ++controller.offer.request;
    update();
    body = lv_obj_get_parent(FindLabel(view.Root(), controller.offer.user_id));
    Check(lv_obj_get_scroll_y(body) == 0, "new account requests reset scroll to the account disclosure");
    Screenshot(view.Root(), screenshots / "device-pairing-account.ppm");
    Click(view, WATCH_PAIR_BIND_ACCOUNT);
    Check(requests.back().action == WatchViewAction::kApproveDevicePairingAccount &&
        requests.back().pairing_attempt == 12 && requests.back().pairing_request == 32 &&
        !requests.back().pairing_confirmation && requests.back().enabled,
        "account approval is a separate explicit action bound to its challenge");
    view.Complete(requests.back().id, true);
    Check(!FindLabel(view.Root(), WATCH_PAIR_BOUND), "accepted approval is not durable binding success");
    controller.offer.device_id = "different-device";
    update();
    Check(!FindLabel(view.Root(), controller.offer.user_id) && !FindButton(view.Root(), WATCH_PAIR_BIND_ACCOUNT),
        "account request for another device is not presented for approval");
    controller.offer.device_id = controller.device_id;
    state.time.sleep_prepared = true;
    update();
    const auto before_sleep = requests.size();
    Click(view, WATCH_PAIR_BIND_ACCOUNT);
    Check(requests.size() == before_sleep, "sleep preparation forbids even synthetic account approval");
    Click(view, WATCH_NOTIFY_REJECT);
    Check(requests.back().action == WatchViewAction::kApproveDevicePairingAccount && !requests.back().enabled,
        "sleep preparation still permits rejection");
    view.Complete(requests.back().id, true);
    state.time.sleep_prepared = false;
    controller.phase = Phase::kVerifying;
    update();
    Check(!FindLabel(view.Root(), controller.offer.user_id) && !FindButton(view.Root(), WATCH_PAIR_BIND_ACCOUNT),
        "consumed account approval is removed immediately");
    Screenshot(view.Root(), screenshots / "device-pairing-verifying.ppm");
    view.SetVisible(false);
    Check(requests.back().action == WatchViewAction::kCancelDevicePairing && requests.back().pairing_attempt == 12,
        "hiding the pairing page cancels the exact active attempt");
    view.Complete(requests.back().id, true);
    controller.phase = Phase::kFailed;
    controller.error = Error::kTimeout;
    update(false, true);
    Check(!FindLabel(view.Root(), controller.offer.user_id), "hidden pairing never retains account text");
    view.SetVisible(true); Tick();
    Check(FindLabel(view.Root(), WATCH_PAIR_TIMEOUT), "expiry is not a generic cancellation");
    Screenshot(view.Root(), screenshots / "device-pairing-expired.ppm");
    controller.error = Error::kStorage;
    update(false, true);
    Check(FindLabel(view.Root(), WATCH_PAIR_STORAGE_ERROR) && !FindButton(view.Root(), WATCH_PAIR_START),
        "uncertain durable storage cannot invite blind retry");
    Screenshot(view.Root(), screenshots / "device-pairing-storage.ppm");
    state.pairing.can_open = true;
    view.Update(state); Tick();
    Click(view, WATCH_PAIR_START);
    Check(requests.back().action == WatchViewAction::kOpenDevicePairing && requests.back().pairing_control_only,
        "explicit owner admission after cloud storage failure opens local control only");
    view.Complete(requests.back().id, true);
    controller.error = Error::kAlreadyBound;
    update(false, true);
    Check(FindLabel(view.Root(), WATCH_PAIR_ALREADY_BOUND) && !FindButton(view.Root(), WATCH_PAIR_START),
        "existing account binding requires explicit unbind");
    controller.error = Error::kNone;
    controller.phase = Phase::kBound;
    controller.binding_generation = 0;
    update(false);
    Check(!FindLabel(view.Root(), WATCH_PAIR_BOUND), "bound state without durable generation fails closed");
    controller.binding_generation = 4;
    update(false);
    Check(FindLabel(view.Root(), WATCH_PAIR_BOUND), "only durable binding reports success");
    Screenshot(view.Root(), screenshots / "device-pairing-bound.ppm");
    controller = {};
    controller.attempt = 13;
    controller.control_only = true;
    controller.phase = Phase::kAwaitPeer;
    peer = {};
    peer.state = WatchAncsLinkState::kPeerConsent;
    peer.confirmation = 30;
    peer.device_pairing_peer = true;
    update();
    Check(!FindButton(view.Root(), WATCH_NOTIFY_ALLOW), "cloud consent cannot approve a local control peer");
    peer.device_pairing_peer = false;
    peer.control_pairing_peer = true;
    update();
    Click(view, WATCH_NOTIFY_ALLOW);
    Check(requests.back().pairing_attempt == 13 && requests.back().pairing_confirmation == 30,
        "local control confirmation uses the same owner dispatcher with a fresh attempt");
    view.Complete(requests.back().id, true);
    controller.phase = Phase::kControlReady;
    update(false);
    Check(!FindLabel(view.Root(), WATCH_PAIR_CONTROL_READY), "control success requires an authenticated peer generation");
    controller.control_peer_generation = 77;
    update(false);
    Check(FindLabel(view.Root(), WATCH_PAIR_CONTROL_READY) && !FindLabel(view.Root(), WATCH_PAIR_BOUND),
        "local secure connection is not cloud account binding");
    Check(!FindButton(view.Root(), WATCH_PAIR_START), "an authorized control connection does not offer duplicate pairing");
    Check(!FindLabel(view.Root(), WATCH_PAIR_ACCOUNT_ID), "offline control pairing has no account approval");
    Screenshot(view.Root(), screenshots / "device-pairing-control-ready.ppm");
    state.pairing.can_authorize_cloud = true;
    view.Update(state); Tick();
    Click(view, WATCH_PAIR_CLOUD_START);
    Check(requests.back().action == WatchViewAction::kOpenDevicePairing && !requests.back().pairing_control_only,
        "optional cloud authorization reuses the pairing dispatcher without opening another local pairing flow");
    view.Complete(requests.back().id, true);
    state.pairing.can_remove_binding = true;
    view.Update(state); Tick();
    Click(view, WATCH_BINDING_REMOVE);
    Check(requests.back().action == WatchViewAction::kOpenBindingRemoval,
        "clear button requests inspection rather than deleting the binding");
    const auto removal_open = requests.back().id;
    view.Navigate(WatchPage::kFace); Tick();
    Check(requests.back().action == WatchViewAction::kCancelDevicePairing &&
        requests.back().pairing_open_request == removal_open, "leaving cancels queued binding inspection");
    view.Complete(removal_open, true);
    view.Complete(requests.back().id, true);
    controller = {};
    controller.attempt = 14;
    controller.removing_binding = true;
    controller.binding_generation = 4;
    controller.phase = Phase::kAwaitRemoval;
    controller.device_id = "1666f423-5647-4a0b-98b4-475e12514bc5";
    update(); view.Navigate(WatchPage::kDevicePairing); Tick();
    Check(FindLabel(view.Root(), WATCH_BINDING_REMOVE_NOTICE), "removal explains local-only effects before confirmation");
    Screenshot(view.Root(), screenshots / "device-pairing-remove-confirm.ppm");
    Click(view, WATCH_BINDING_REMOVE);
    Check(requests.back().action == WatchViewAction::kConfirmBindingRemoval && requests.back().enabled &&
        requests.back().pairing_attempt == 14 && requests.back().binding_generation == 4,
        "removal confirmation carries the rendered attempt and exact stored generation");
    const auto removal_confirm = requests.back().id;
    Click(view, WATCH_NOTIFY_REJECT);
    Check(requests.back().action == WatchViewAction::kConfirmBindingRemoval && !requests.back().enabled,
        "removal rejection can supersede a pending confirmation");
    view.Complete(removal_confirm, true);
    view.Complete(requests.back().id, true);
    controller.phase = Phase::kBindingRemoved;
    controller.binding_generation = 0;
    update(false);
    Check(!FindLabel(view.Root(), WATCH_BINDING_REMOVED), "removal without a durable generation never reports success");
    controller.binding_generation = 5;
    update(false);
    Check(FindLabel(view.Root(), WATCH_BINDING_REMOVED), "committed tombstone reports local removal success");
    Screenshot(view.Root(), screenshots / "device-pairing-removed.ppm");
    state.pairing.forget_peer_generation = 77;
    view.Update(state); Tick();
    Click(view, WATCH_CONTROL_REMOVE);
    Check(requests.back().action == WatchViewAction::kOpenControlRemoval && requests.back().pairing_peer_generation == 77,
        "forget entry pins the current native phone rather than a cloud device identity");
    const auto forget_open = requests.back().id;
    view.Navigate(WatchPage::kFace); Tick();
    Check(requests.back().action == WatchViewAction::kCancelDevicePairing && requests.back().pairing_open_request == forget_open,
        "leaving cancels a queued phone removal opening");
    view.Complete(forget_open, true);
    view.Complete(requests.back().id, true);
    controller = {};
    controller.attempt = 15;
    controller.removing_control = true;
    controller.control_peer_generation = 77;
    controller.phase = Phase::kAwaitControlRemoval;
    update(); view.Navigate(WatchPage::kDevicePairing); Tick();
    Check(FindLabel(view.Root(), WATCH_CONTROL_REMOVE_NOTICE), "phone removal explains retained recordings and cloud authorization");
    Screenshot(view.Root(), screenshots / "device-pairing-forget-confirm.ppm");
    auto* forget_body = lv_obj_get_parent(FindLabel(view.Root(), WATCH_CONTROL_REMOVE_NOTICE));
    lv_obj_scroll_to_y(forget_body, 1000, LV_ANIM_OFF); Tick();
    Screenshot(view.Root(), screenshots / "device-pairing-forget-bottom.ppm");
    state.time.sleep_prepared = true;
    update();
    const auto before_forget_sleep = requests.size();
    Click(view, WATCH_CONTROL_REMOVE);
    Check(requests.size() == before_forget_sleep, "sleep preparation prevents phone removal confirmation");
    state.time.sleep_prepared = false;
    update();
    Click(view, WATCH_CONTROL_REMOVE);
    Check(requests.back().action == WatchViewAction::kConfirmControlRemoval && requests.back().enabled &&
        requests.back().pairing_attempt == 15 && requests.back().pairing_peer_generation == 77,
        "forget confirmation carries its rendered attempt and phone generation");
    const auto forget_confirm = requests.back().id;
    const auto before_forget_duplicate = requests.size();
    Click(view, WATCH_CONTROL_REMOVE);
    Check(requests.size() == before_forget_duplicate, "pending phone removal confirmation is not duplicated");
    Click(view, WATCH_NOTIFY_REJECT);
    Check(requests.back().action == WatchViewAction::kConfirmControlRemoval && !requests.back().enabled,
        "rejection can fence a queued phone removal confirmation");
    view.Complete(forget_confirm, true);
    view.Complete(requests.back().id, true);
    controller.phase = Phase::kRemovingControl;
    update();
    Check(FindLabel(view.Root(), WATCH_CONTROL_REMOVING) && !FindButton(view.Root(), WATCH_PAIR_CANCEL) &&
        !FindButton(view.Root(), WATCH_CONTROL_REMOVE), "started removal does not offer a misleading undo button");
    view.Navigate(WatchPage::kFace); Tick();
    Check(requests.back().action == WatchViewAction::kCancelDevicePairing && requests.back().pairing_attempt == 15,
        "leaving an active removal still fences the owner's exact operation");
    view.Complete(requests.back().id, true);
    controller.phase = Phase::kControlRemoved;
    controller.control_peer_generation = 0;
    update(false); view.Navigate(WatchPage::kDevicePairing); Tick();
    Check(!FindLabel(view.Root(), WATCH_CONTROL_REMOVED), "phone removal success requires a native peer generation");
    controller.control_peer_generation = 77;
    update(false);
    Check(FindLabel(view.Root(), WATCH_CONTROL_REMOVED), "verified native phone removal reports success");
    Screenshot(view.Root(), screenshots / "device-pairing-forgotten.ppm");
    controller.phase = Phase::kFailed;
    controller.error = Error::kControlRemoval;
    update(false);
    Check(FindLabel(view.Root(), WATCH_CONTROL_REMOVE_FAILED) && !FindLabel(view.Root(), WATCH_PAIR_VERIFY_ERROR),
        "native removal failure is not presented as cloud verification failure");
    const auto before_unavailable_forget = requests.size();
    Click(view, WATCH_CONTROL_REMOVE);
    Check(requests.size() == before_unavailable_forget, "unavailable phone removal cannot submit a fake request");
    controller = {};
    controller.attempt = 16;
    update(false);
    state.pairing.can_inspect_saved_peers = true;
    view.Update(state); Tick();
    Click(view, WATCH_SAVED_PHONES);
    Check(requests.back().action == WatchViewAction::kOpenSavedControlPeers,
        "saved phones opens inspection without removing a phone");
    const auto saved_open = requests.back().id;
    view.Navigate(WatchPage::kFace); Tick();
    Check(requests.back().action == WatchViewAction::kCancelDevicePairing && requests.back().pairing_open_request == saved_open,
        "leaving fences the queued saved-phone inspection");
    view.Complete(saved_open, true); view.Complete(requests.back().id, true);
    controller.removing_control = controller.removing_saved_control = true;
    controller.phase = Phase::kAwaitSavedPeer;
    for (uint8_t i = 0; i < 32; ++i) controller.saved_peers.push_back({uint64_t(100 + i), {1,2,3,4,5,i}});
    update(); view.Navigate(WatchPage::kDevicePairing); Tick();
    Check(FindButton(view.Root(), "01:02:03:04:05:00") && FindButton(view.Root(), "01:02:03:04:05:1F"),
        "all supported saved phones are available in the scrollable list");
    Screenshot(view.Root(), screenshots / "device-pairing-saved-phones.ppm");
    lv_obj_scroll_to_view(FindButton(view.Root(), "01:02:03:04:05:1F"), LV_ANIM_OFF); Tick();
    Screenshot(view.Root(), screenshots / "device-pairing-saved-phones-bottom.ppm");
    state.time.sleep_prepared = true; update();
    const auto before_saved_sleep = requests.size();
    Click(view, "01:02:03:04:05:1F");
    Check(requests.size() == before_saved_sleep, "sleep preparation blocks saved-phone selection");
    state.time.sleep_prepared = false; update();
    Click(view, "01:02:03:04:05:1F");
    Check(requests.back().action == WatchViewAction::kSelectSavedControlPeer && requests.back().pairing_peer_generation == 131 &&
        requests.back().pairing_attempt == 16, "saved-phone selection carries the rendered token and attempt");
    view.Complete(requests.back().id, true);
    controller.control_peer_generation = 131;
    controller.phase = Phase::kAwaitControlRemoval;
    update();
    Check(FindLabel(view.Root(), WATCH_SAVED_PHONE_CONFIRM) && FindLabel(view.Root(), "01:02:03:04:05:1F"),
        "second confirmation identifies the selected saved phone");
    Screenshot(view.Root(), screenshots / "device-pairing-saved-confirm.ppm");
    lv_obj_scroll_to_view(FindButton(view.Root(), WATCH_CONTROL_REMOVE), LV_ANIM_OFF); Tick();
    Screenshot(view.Root(), screenshots / "device-pairing-saved-confirm-bottom.ppm");
    Click(view, WATCH_CONTROL_REMOVE);
    Check(requests.back().action == WatchViewAction::kConfirmControlRemoval && requests.back().pairing_peer_generation == 131,
        "saved removal reuses explicit native removal confirmation");
    view.Complete(requests.back().id, true);
    controller.control_peer_generation = 0;
    controller.phase = Phase::kAwaitSavedPeer;
    update();
    Click(view, WATCH_SAVED_PHONES_REMOVE_ALL);
    Check(requests.back().action == WatchViewAction::kSelectAllSavedControlPeers && requests.back().pairing_attempt == 16,
        "saved-phone list offers an explicit non-destructive all-phone selection");
    view.Complete(requests.back().id, true);
    controller.removing_all_control = true;
    controller.phase = Phase::kAwaitAllControlRemoval;
    update();
    Check(FindLabel(view.Root(), WATCH_SAVED_PHONES_REMOVE_ALL_CONFIRM) &&
        FindLabel(view.Root(), WATCH_SAVED_PHONES_REMOVE_ALL_NOTICE),
        "all-phone removal has a separate consequence confirmation");
    Screenshot(view.Root(), screenshots / "device-pairing-saved-remove-all-confirm.ppm");
    lv_obj_scroll_to_view(FindButton(view.Root(), WATCH_SAVED_PHONES_REMOVE_ALL), LV_ANIM_OFF); Tick();
    Screenshot(view.Root(), screenshots / "device-pairing-saved-remove-all-confirm-bottom.ppm");
    Click(view, WATCH_SAVED_PHONES_REMOVE_ALL);
    Check(requests.back().action == WatchViewAction::kConfirmAllControlRemoval && requests.back().enabled,
        "all-phone removal requires explicit confirmation");
    view.Complete(requests.back().id, true);
    controller.phase = Phase::kRemovingAllControl;
    update();
    Check(FindLabel(view.Root(), WATCH_SAVED_PHONES_REMOVING_ALL) && !FindButton(view.Root(), WATCH_PAIR_CANCEL),
        "started all-phone removal cannot claim that cancellation will restore removed bonds");
    controller.phase = Phase::kAllControlRemoved;
    update(false);
    Check(FindLabel(view.Root(), WATCH_SAVED_PHONES_REMOVED_ALL),
        "verified all-phone removal reports its distinct durable result");
    Screenshot(view.Root(), screenshots / "device-pairing-saved-remove-all-complete.ppm");
    controller.saved_peers.clear(); controller.control_peer_generation = 0;
    controller.removing_all_control = false;
    controller.phase = Phase::kAwaitSavedPeer; update();
    Check(FindLabel(view.Root(), WATCH_SAVED_PHONES_EMPTY) && !FindButton(view.Root(), WATCH_CONTROL_REMOVE) &&
        !FindButton(view.Root(), WATCH_SAVED_PHONES_REMOVE_ALL),
        "empty approved list has no destructive action");
    controller.phase = Phase::kCancelled; update(false);
    peer.enabled = true;
    state.notifications = BuildWatchNotificationSnapshot(peer);
    view.Update(state); view.Navigate(WatchPage::kNotifications); Tick();
    Check(!FindButton(view.Root(), WATCH_NOTIFY_ALLOW) && !FindButton(view.Root(), WATCH_NOTIFY_MATCH),
        "notification page cannot approve a local control pairing");
    std::cout << "PASS: device pairing permissions, account identity, cancellation, durable results and rendering\n";
}

void TestPowerModeUi(WatchViewState state, const std::filesystem::path& screenshots) {
    state.time.initialized = true;
    state.time.sleep_prepared = false;
    state.time.storage_error = state.time.interrupted = false;
    state.time.state.alarm_alerts = 0;
    state.time.state.checkpoint.timer_alert = false;
    state.alert_sound = {};
    state.notifications = {};
    state.wifi_connected = true;
    state.power_mode = {1, 1, 8, "watch_standby", "", WatchPowerModeMessage::kNone, false, true};
    std::vector<WatchViewRequest> requests;
    WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48,
        &font_awesome_30_4, [&](const WatchViewRequest& request) { requests.push_back(request); });
    view.Update(state);
    view.Navigate(WatchPage::kControls); Tick();
    Click(view, WATCH_POWER_MODE);
    Check(view.Page() == WatchPage::kPowerSettings, "control center opens power settings");
    std::function<lv_obj_t*(lv_obj_t*)> dropdown = [&](lv_obj_t* object) -> lv_obj_t* {
        if (lv_obj_check_type(object, &lv_dropdown_class)) return object;
        for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
            if (auto* result = dropdown(lv_obj_get_child(object, i))) return result;
        return nullptr;
    };
    auto* options = dropdown(view.Root());
    Check(options != nullptr && lv_dropdown_get_option_count(options) == 3, "three power modes are selectable");
    Check(FindLabel(view.Root(), WATCH_POWER_APPLIED), "applied revision is shown");
    auto* save = FindButton(view.Root(), WATCH_SAVE);
    auto* reload = FindButton(view.Root(), FONT_AWESOME_ARROWS_ROTATE);
    lv_obj_update_layout(view.Root());
    Check(lv_obj_get_x(reload) >= 0 && lv_obj_get_x(save) >= lv_obj_get_x(reload) + lv_obj_get_width(reload) &&
        lv_obj_get_x(save) + lv_obj_get_width(save) <= lv_obj_get_content_width(lv_obj_get_parent(save)),
        "refresh and save buttons are visible without overlap or clipping");
    Check(lv_obj_has_state(save, LV_STATE_DISABLED), "unchanged mode cannot be saved");
    lv_dropdown_set_selected(options, 1);
    lv_obj_send_event(options, LV_EVENT_VALUE_CHANGED, nullptr); Tick();
    Check(!lv_obj_has_state(save, LV_STATE_DISABLED), "changed power draft can be saved");
    Screenshot(view.Root(), screenshots / "power-mode-draft.ppm");
    Click(view, WATCH_SAVE);
    Check(requests.size() == 1 && requests.back().action == WatchViewAction::kSetPowerMode &&
        requests.back().settings_revision == 1 && requests.back().binding_generation == 8 &&
        requests.back().power_mode == "battery_saver", "power selection carries its rendered revision and binding");
    view.Complete(requests.back().id, true);
    state.power_mode.pending = true;
    state.power_mode.can_change = false;
    state.power_mode.message = WatchPowerModeMessage::kSaving;
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_SAVING), "accepted UI command is not displayed as applied");
    Check(lv_obj_has_state(options, LV_STATE_DISABLED), "selection disabled during mutation");
    Screenshot(view.Root(), screenshots / "power-mode-saving.ppm");
    state.power_mode.revision = 2;
    state.power_mode.power_mode = "battery_saver";
    state.power_mode.message = WatchPowerModeMessage::kNone;
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_POWER_PENDING), "saved settings await device acknowledgement");
    Screenshot(view.Root(), screenshots / "power-mode-pending.ppm");
    state.power_mode.applied_revision = 2;
    state.power_mode.pending = false;
    state.power_mode.can_change = true;
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_POWER_APPLIED), "acknowledged settings show applied");
    lv_dropdown_set_selected(options, 2);
    lv_obj_send_event(options, LV_EVENT_VALUE_CHANGED, nullptr); Tick();
    Check(lv_obj_has_state(save, LV_STATE_DISABLED), "stale draft cannot silently adopt a newer revision");
    const auto count = requests.size();
    Click(view, WATCH_SAVE);
    Check(requests.size() == count, "even a synthetic click cannot submit a stale draft");
    Click(view, FONT_AWESOME_ARROWS_ROTATE);
    Check(lv_dropdown_get_selected(options) == 1, "explicit refresh loads the canonical mode");
    lv_dropdown_set_selected(options, 2);
    lv_obj_send_event(options, LV_EVENT_VALUE_CHANGED, nullptr); Tick();
    Check(!lv_obj_has_state(save, LV_STATE_DISABLED), "refreshed draft can be edited");
    state.power_mode.message = WatchPowerModeMessage::kConflict;
    state.power_mode.revision = state.power_mode.applied_revision = 3;
    view.Update(state); Tick();
    Screenshot(view.Root(), screenshots / "power-mode-conflict.ppm");
    Click(view, FONT_AWESOME_ARROWS_ROTATE);
    Check(FindLabel(view.Root(), WATCH_POWER_APPLIED), "explicit canonical refresh clears the stale-draft prompt");
    state.wifi_connected = false;
    state.power_mode.message = WatchPowerModeMessage::kNone;
    state.power_mode.can_change = false;
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_POWER_OFFLINE) && lv_obj_has_state(save, LV_STATE_DISABLED),
        "offline page cannot pretend to save a local shadow setting");
    Screenshot(view.Root(), screenshots / "power-mode-offline.ppm");
    state.wifi_connected = true;
    state.power_mode.binding_generation = 9;
    state.power_mode.can_change = true;
    view.Update(state); Tick();
    Check(lv_obj_has_state(save, LV_STATE_DISABLED), "new owner cannot inherit the old draft");
    Click(view, FONT_AWESOME_ANGLE_LEFT);
    Check(view.Page() == WatchPage::kControls, "power settings returns to control center");
}

void TestNotificationUi(WatchViewState state, const std::filesystem::path& screenshots) {
    state.time.initialized = false;
    state.time.sleep_prepared = false;
    state.time.storage_error = state.time.interrupted = false;
    state.time.state.alarm_alerts = 0;
    state.time.state.checkpoint.timer_alert = false;
    state.alert_sound = {};
    std::vector<WatchViewRequest> requests;
    WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48,
        &font_awesome_30_4, [&](const WatchViewRequest& request) { requests.push_back(request); });
    view.Update(state);
    view.Navigate(WatchPage::kNotifications); Tick();
    Check(FindLabel(view.Root(), WATCH_NOTIFY_UNSUPPORTED), "unsupported firmware does not offer fake notification consent");
    auto& n = state.notifications;
    n.available = true;
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_NOTIFY_DISABLED), "notifications start disabled");
    auto find_switch = [&]() {
        std::function<lv_obj_t*(lv_obj_t*)> find = [&](lv_obj_t* object) -> lv_obj_t* {
            if (lv_obj_check_type(object, &lv_switch_class)) return object;
            for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
                if (auto* result = find(lv_obj_get_child(object, i))) return result;
            return nullptr;
        };
        return find(view.Root());
    };
    auto toggle = [&](bool enabled) {
        auto* control = find_switch();
        Check(control && !lv_obj_has_state(control, LV_STATE_DISABLED), "notification switch is usable");
        if (enabled) lv_obj_add_state(control, LV_STATE_CHECKED);
        else lv_obj_remove_state(control, LV_STATE_CHECKED);
        lv_obj_send_event(control, LV_EVENT_VALUE_CHANGED, nullptr);
    };
    toggle(true);
    Check(requests.back().action == WatchViewAction::kSetNotificationsEnabled && requests.back().enabled,
        "notification opt in works without initialized clock");
    const auto enable_id = requests.back().id;
    toggle(false);
    const auto disable_id = requests.back().id;
    Check(disable_id != enable_id && !requests.back().enabled, "opt out supersedes pending opt in");
    view.Complete(enable_id, true);
    Check(FindLabel(view.Root(), WATCH_SAVING), "stale enable completion cannot clear pending disable");
    n.save_failed = true;
    view.Update(state); view.Complete(disable_id, false); Tick();
    Check(FindLabel(view.Root(), WATCH_NOTIFY_SAVE_FAILED), "failed persistence is visible without enabling collection");
    Screenshot(view.Root(), screenshots / "notifications-save-failed.ppm");
    n.save_failed = false;
    n.client.enabled = true;
    n.client.state = WatchAncsLinkState::kPeerConsent;
    n.client.confirmation = 41;
    view.Update(state); Tick();
    Click(view, WATCH_NOTIFY_ALLOW);
    Check(requests.back().action == WatchViewAction::kConfirmNotificationPeer &&
        requests.back().notification_confirmation == 41 && requests.back().enabled, "peer consent carries the current token");
    view.Complete(requests.back().id, true);
    n.client.state = WatchAncsLinkState::kConfirmNumber;
    n.client.confirmation = 42;
    n.client.passkey = 1234;
    view.Navigate(WatchPage::kFace); view.Update(state); Tick();
    Check(view.Page() == WatchPage::kFace && FindButton(view.Root(), WATCH_NOTIFY_PAIR_REQUEST),
        "numeric comparison is discoverable without stealing the current page");
    Click(view, WATCH_NOTIFY_PAIR_REQUEST);
    Check(FindLabel(view.Root(), "001234"), "numeric comparison preserves six digits including leading zeros");
    Screenshot(view.Root(), screenshots / "notifications-pairing.ppm");
    Click(view, WATCH_NOTIFY_MATCH);
    Check(requests.back().notification_confirmation == 42 && requests.back().enabled, "numeric confirmation uses its own token");
    Click(view, WATCH_NOTIFY_REJECT);
    Check(requests.back().notification_confirmation == 42 && !requests.back().enabled, "rejection stays usable during pending confirmation");
    view.Complete(requests.back().id, true);
    n.client.device_pairing_peer = true;
    n.client.confirmation = 43;
    view.Update(state); Tick();
    Check(!FindButton(view.Root(), WATCH_NOTIFY_MATCH) && !FindButton(view.Root(), WATCH_NOTIFY_REJECT),
        "notification page cannot confirm device ownership pairing");
    Check(!FindLabel(view.Root(), WATCH_NOTIFY_COMPARE) && FindLabel(view.Root(), WATCH_NOTIFY_UNAVAILABLE),
        "notification status does not ask for a code belonging to device pairing");
    Screenshot(view.Root(), screenshots / "notifications-during-device-pairing.ppm");
    view.Navigate(WatchPage::kFace); Tick();
    Check(!FindButton(view.Root(), WATCH_NOTIFY_PAIR_REQUEST),
        "device pairing is not presented as a notification permission request");
    n.client.device_pairing_peer = false;
    n.client.confirmation = 0;
    n.client.state = WatchAncsLinkState::kReady;
    n.content.ready = true;
    n.content.source = WatchNotificationSource::kAncs;
    n.content.session = 8;
    WatchPhoneNotification entry;
    entry.uid = 0; entry.revision = 10; entry.details_ready = true;
    entry.title = "Meeting notes"; entry.app_identifier = "ai.audream.app";
    entry.message = "Private meeting message"; entry.date = "20260914T091000";
    n.content.items = {entry};
    view.Update(state); view.Navigate(WatchPage::kNotifications); Tick();
    Screenshot(view.Root(), screenshots / "notifications-list.ppm");
    Click(view, entry.title);
    Check(requests.back().action == WatchViewAction::kOpenNotification && requests.back().notification_uid == 0 &&
        requests.back().notification_revision == 10 && requests.back().notification_session == 8 &&
        requests.back().notification_source == WatchNotificationSource::kAncs,
        "opening UID zero carries session and revision guards");
    Check(FindLabel(view.Root(), entry.message), "detail renders retained phone content");
    Check(FindLabel(view.Root(), "2026-09-14 09:10:00"), "phone wall time is formatted without guessing a UTC offset");
    view.Complete(requests.back().id, true);
    Screenshot(view.Root(), screenshots / "notification-detail.ppm");
    view.SetVisible(false);
    n.content.items[0].revision = 11;
    n.content.items[0].message = "Replacement message";
    view.Update(state);
    Check(!FindLabel(view.Root(), entry.message) && !FindLabel(view.Root(), "Replacement message") &&
        FindLabel(view.Root(), WATCH_NOTIFY_REMOVED), "hidden detail is purged and cannot retag a revised notification");
    Check(lv_obj_has_flag(view.Root(), LV_OBJ_FLAG_HIDDEN), "privacy redraw cannot wake the display");
    view.SetVisible(true); Tick();
    Click(view, FONT_AWESOME_ANGLE_LEFT);
    Click(view, entry.title); view.Complete(requests.back().id, true);
    Check(FindLabel(view.Root(), "Replacement message"), "revised notification requires an explicit new open");
    n.client.enabled = false;
    n.client.state = WatchAncsLinkState::kDisabled;
    n.content = {};
    view.Update(state);
    Check(!FindLabel(view.Root(), "Replacement message") && FindLabel(view.Root(), WATCH_NOTIFY_REMOVED),
        "opt out purges visible detail without waiting for timer render");
    n.client.enabled = true;
    n.client.state = WatchAncsLinkState::kReady;
    n.content.session = 9;
    n.content.source = WatchNotificationSource::kAncs;
    n.content.ready = true;
    entry.revision = 12;
    n.content.items = {entry};
    view.Navigate(WatchPage::kNotifications); view.Update(state); Tick();
    Click(view, WATCH_NOTIFY_CLEAR);
    Check(requests.back().action == WatchViewAction::kClearNotifications && requests.back().notification_session == 9 &&
        requests.back().notification_source == WatchNotificationSource::kAncs,
        "clear is a guarded local action not a phone dismissal");
    n.content.items.clear();
    view.Update(state); view.Complete(requests.back().id, true); Tick();
    Check(FindLabel(view.Root(), WATCH_NOTIFY_EMPTY), "empty state follows actual cleared snapshot");
    Screenshot(view.Root(), screenshots / "notifications-empty.ppm");
    entry.title = "A long notification title with enough words to wrap across multiple lines in the bounded list row";
    entry.message = "#ff0000 This is literal phone text#\n" + std::string(470, 'A');
    n.content.items.assign(20, entry);
    for (uint32_t i = 0; i < 20; ++i) n.content.items[i].uid = i;
    view.Update(state); Tick();
    Screenshot(view.Root(), screenshots / "notifications-long-list.ppm");
    auto* clear = FindButton(view.Root(), WATCH_NOTIFY_CLEAR);
    Check(clear != nullptr, "long list retains its local clear command");
    auto* first_row = lv_obj_get_child(lv_obj_get_parent(clear), lv_obj_get_index(clear) + 1);
    Check(first_row && lv_obj_check_type(first_row, &lv_button_class), "first truncated notification row exists");
    lv_obj_send_event(first_row, LV_EVENT_CLICKED, nullptr); Tick();
    view.Complete(requests.back().id, true);
    Check(FindLabel(view.Root(), entry.title), "opening a truncated row retains the complete title");
    auto* message_label = FindLabel(view.Root(), entry.message);
    Check(message_label && !lv_label_get_recolor(message_label), "phone inline markup stays literal");
    lv_obj_update_layout(view.Root());
    lv_obj_scroll_to_y(lv_obj_get_parent(message_label), LV_COORD_MAX, LV_ANIM_OFF);
    Screenshot(view.Root(), screenshots / "notification-long-detail-bottom.ppm");
    Check(lv_obj_get_scroll_bottom(lv_obj_get_parent(message_label)) <= 0,
        "long unbroken notification body remains scrollable to its bottom");
    n.client.state = WatchAncsLinkState::kOffline;
    n.content = {};
    view.Update(state);
    Check(!FindLabel(view.Root(), entry.message), "disconnect removes displayed phone text synchronously on snapshot update");
    std::cout << "PASS: notification consent, guarded details, local clear, hidden privacy and storage feedback\n";
}

void TestAndroidNotificationUi(WatchViewState state, const std::filesystem::path& screenshots) {
    state.time.initialized = true; state.time.sleep_prepared = false;
    state.time.state.alarm_alerts = 0; state.time.state.checkpoint.timer_alert = false;
    state.time.state.timezone = "Asia/Shanghai";
    state.alert_sound = {};
    WatchAncsClientSnapshot client;
    client.enabled = true; client.android_peer = true; client.link_generation = 17;
    client.state = WatchAncsLinkState::kAndroidAuthorizing;
    WatchAndroidNotificationSnapshot android;
    android.state = WatchAndroidNotificationState::kReady;
    android.session = 8; android.peer_lease = 17;
    WatchAndroidNotification entry;
    entry.uid = 1; entry.revision = 10; entry.posted_at_ms = 1726310400000ULL;
    entry.package_name = "ai.audream.android"; entry.app_name = "Audream";
    entry.title = "Android meeting"; entry.message = "Android private message";
    android.entries = {entry};
    state.notifications = BuildWatchNotificationSnapshot(client, android);
    Check(state.notifications.content.ready && state.notifications.content.source == WatchNotificationSource::kAndroid &&
        state.notifications.client.state == WatchAncsLinkState::kAndroidAuthorizing,
        "authorized Android content does not overwrite the native security state");
    Check(state.notifications.content.items.front().posted_at_ms == entry.posted_at_ms &&
        state.notifications.content.items.front().app_identifier == entry.package_name,
        "Android projection preserves package and UTC timestamp");
    auto stale = android;
    stale.peer_lease = 16;
    Check(BuildWatchNotificationSnapshot(client, stale).content.items.empty(), "mixed physical peer snapshots fail closed");
    stale.peer_lease = 0;
    Check(!BuildWatchNotificationSnapshot(client, stale).content.ready, "protocol-only snapshots cannot claim a secure peer");
    for (auto phase : {WatchAncsLinkState::kOffline, WatchAncsLinkState::kPeerConsent,
            WatchAncsLinkState::kConfirmNumber, WatchAncsLinkState::kFailed}) {
        auto pending = client; pending.state = phase;
        Check(BuildWatchNotificationSnapshot(pending, android).content.items.empty(), "non-authorized peers cannot project private content");
    }
    auto disabled = client; disabled.enabled = false;
    Check(BuildWatchNotificationSnapshot(disabled, android).content.items.empty(), "disabled collection hides even a ready native snapshot");
    for (auto phase : {WatchAndroidNotificationState::kDisabled, WatchAndroidNotificationState::kDisconnected,
            WatchAndroidNotificationState::kFailed}) {
        stale = android; stale.state = phase;
        Check(BuildWatchNotificationSnapshot(client, stale).content.items.empty(), "retired Android engine cannot supply UI content");
    }
    std::vector<WatchViewRequest> requests;
    WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48,
        &font_awesome_30_4, [&](const WatchViewRequest& request) { requests.push_back(request); });
    view.Update(state); view.Navigate(WatchPage::kNotifications); Tick();
    Check(FindLabel(view.Root(), Lang::Strings::WATCH_NOTIFY_READY), "Android ready state is visible");
    Screenshot(view.Root(), screenshots / "android-notifications-list.ppm");
    Click(view, entry.title);
    Check(requests.back().notification_source == WatchNotificationSource::kAndroid &&
        requests.back().notification_session == 8 && requests.back().notification_uid == 1 &&
        requests.back().notification_revision == 10, "Android open carries source, session and item identity");
    view.Complete(requests.back().id, true);
    Check(FindLabel(view.Root(), entry.message) && FindLabel(view.Root(), "2024-09-14 18:40:00"),
        "Android UTC timestamp is rendered in the selected watch timezone");
    Screenshot(view.Root(), screenshots / "android-notification-detail.ppm");
    state.time.state.timezone = "UTC";
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), "2024-09-14 10:40:00"), "an open Android detail follows timezone changes");
    android.entries[0].read = true;
    state.notifications = BuildWatchNotificationSnapshot(client, android);
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), entry.message), "marking read preserves the selected revision");
    view.SetVisible(false);
    auto iphone = client; iphone.android_peer = false; iphone.state = WatchAncsLinkState::kReady;
    iphone.notifications.session = android.session; iphone.notifications.state = WatchAncsState::kReady;
    WatchPhoneNotification other;
    other.uid = entry.uid; other.revision = entry.revision; other.details_ready = true;
    other.title = "iPhone meeting"; other.message = "iPhone private message"; other.date = "20260914T091000";
    iphone.notifications.notifications = {other};
    state.notifications = BuildWatchNotificationSnapshot(iphone, android);
    Check(state.notifications.client.notifications.notifications.empty() && state.notifications.content.items.size() == 1,
        "projection retains one content vector and chooses only the active phone platform");
    view.Update(state);
    Check(!FindLabel(view.Root(), entry.message) && !FindLabel(view.Root(), other.message) &&
        FindLabel(view.Root(), Lang::Strings::WATCH_NOTIFY_REMOVED), "colliding platform identifiers cannot reopen private details");
    Check(lv_obj_has_flag(view.Root(), LV_OBJ_FLAG_HIDDEN), "platform switch privacy update does not wake the screen");
    view.SetVisible(true);
    state.notifications = BuildWatchNotificationSnapshot(client, android);
    view.Update(state); view.Navigate(WatchPage::kNotifications); Tick();
    Click(view, Lang::Strings::WATCH_NOTIFY_CLEAR);
    Check(requests.back().notification_source == WatchNotificationSource::kAndroid &&
        requests.back().notification_session == 8 && requests.back().action == WatchViewAction::kClearNotifications,
        "Android clear dispatches only to its local engine");
    android.entries.clear(); state.notifications = BuildWatchNotificationSnapshot(client, android);
    view.Update(state); view.Complete(requests.back().id, true); Tick();
    Check(FindLabel(view.Root(), Lang::Strings::WATCH_NOTIFY_EMPTY), "authorized Android empty list is not a connecting state");
    android.entries = {entry}; state.notifications = BuildWatchNotificationSnapshot(client, android);
    view.Update(state); Tick(); Click(view, entry.title); view.Complete(requests.back().id, true);
    view.SetVisible(false);
    client.state = WatchAncsLinkState::kOffline;
    state.notifications = BuildWatchNotificationSnapshot(client, android);
    view.Update(state);
    Check(!FindLabel(view.Root(), entry.message) && FindLabel(view.Root(), Lang::Strings::WATCH_NOTIFY_REMOVED),
        "Android disconnect purges hidden detail synchronously");
    std::cout << "PASS: Android notification projection, source isolation, timezone, local actions and hidden privacy\n";
}

void TestAlertSoundUi(WatchViewState state, const std::filesystem::path& screenshots) {
    state.time.storage_error = state.time.interrupted = false;
    state.time.initialized = true; state.time.sleep_prepared = false;
    state.time.state.alarm_alerts = 0;
    state.time.state.checkpoint.timer_alert = true;
    state.alert_sound = {true, false, false, 5};
    std::vector<WatchViewRequest> requests;
    WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48,
        &font_awesome_30_4, [&](const WatchViewRequest& request) { requests.push_back(request); });
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_TIMER_FINISHED), "audible timer keeps its visual alert");
    Click(view, FONT_AWESOME_CHECK);
    Check(requests.back().command == WatchTimeCommand::kDismissAlerts, "alert confirmation requests real dismissal");
    const auto first = requests.back().id;
    state.time.state.checkpoint.timer_alert = false;
    view.Update(state); view.Complete(first, true); Tick();
    Check(FindLabel(view.Root(), WATCH_ALERT_STOPPING), "cleared timer does not hide an output still stopping");
    Screenshot(view.Root(), screenshots / "alert-sound-stopping.ppm");
    state.alert_sound.failed = state.alert_sound.cleanup_failed = true;
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_ALERT_CLEANUP_FAILED) && FindLabel(view.Root(), WATCH_ALERT_SOUND_FAILED),
        "failed hardware cleanup stays visible and retryable after timer dismissal");
    Screenshot(view.Root(), screenshots / "alert-sound-cleanup-failed.ppm");
    auto* label = FindLabel(view.Root(), WATCH_ALERT_CLEANUP_FAILED);
    lv_area_t bounds; lv_obj_get_coords(label, &bounds);
    Check(bounds.y1 >= 64 && bounds.y2 < 140, "cleanup status fits inside the alert banner");
    state.time.initialized = false;
    view.Update(state); Tick();
    Click(view, FONT_AWESOME_CHECK); const auto retry = requests.back().id;
    Check(retry > first, "audio cleanup retry does not depend on initialized calendar storage");
    Click(view, FONT_AWESOME_CHECK); const auto latest = requests.back().id;
    Check(latest > retry, "alert cancellation remains available while a command is pending");
    view.Complete(retry, true); Tick();
    Check(FindLabel(view.Root(), WATCH_SAVING), "obsolete dismissal receipt cannot finish the latest request");
    state.alert_sound.active = state.alert_sound.cleanup_failed = false;
    view.Update(state); view.Complete(latest, true); Tick();
    Check(!FindLabel(view.Root(), WATCH_ALERT_CLEANUP_FAILED) && !FindLabel(view.Root(), WATCH_ALERT_STOPPING),
        "alert sound banner clears only after output is actually released");
}

void TestPlaybackUi(WatchViewState state, const std::filesystem::path& screenshots) {
    using State = RecordingPlaybackState;
    using Volume = PlaybackVolumeStatus;
    state.time.initialized = state.time.sleep_prepared = false;
    state.library = {}; state.deletion = {}; state.playback = {};
    state.can_play_recording = true;
    state.controls.volume = 70; state.controls.can_adjust = false;
    std::vector<WatchViewRequest> requests;
    WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48,
        &font_awesome_30_4, [&](const WatchViewRequest& request) { requests.push_back(request); });
    view.Update(state); view.Navigate(WatchPage::kRecordings); Tick();
    const auto load = requests.back().id;
    WatchRecordingEntry entry;
    entry.id = "saved-recording"; entry.file_name = entry.id + ".opus"; entry.size_bytes = 40000;
    state.library.status = WatchLibraryStatus::kReady; state.library.revision = 7;
    state.library.entries = {entry};
    view.Update(state); view.Complete(load, true); Tick();
    Click(view, entry.file_name);
    Check(requests.size() == 1, "opening a detail never auto plays");
    state.library.entries[0].incomplete = true; ++state.library.revision;
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_RECORDING_INCOMPLETE) && !FindLabel(view.Root(), WATCH_RECORDER_SAVED),
        "incomplete recording is not presented as a successful save");
    Screenshot(view.Root(), screenshots / "recording-incomplete.ppm");
    state.library.entries[0].incomplete = false; state.library.revision = 7;
    view.Update(state); Tick();
    Click(view, FONT_AWESOME_PLAY);
    const auto play = requests.back();
    Check(play.action == WatchViewAction::kPlayRecording && play.recording_id == entry.id && play.library_revision == 7,
        "play carries catalog identity without requiring synchronized time");
    Click(view, FONT_AWESOME_PLAY);
    Check(requests.back().id == play.id, "pending play cannot start a second worker");
    state.can_play_recording = false;
    state.playback.recording_id = entry.id; state.playback.generation = 17;
    state.playback.resources_held = true; state.playback.sample_rate = 16000;
    state.playback.volume_supported = true;
    state.playback.state = State::kPreparing;
    view.Update(state); view.Complete(play.id, true); Tick();
    Check(view.Page() == WatchPage::kPlayback && FindLabel(view.Root(), WATCH_PLAYBACK_PREPARING),
        "accepted playback is preparing, not falsely completed");
    Check(lv_obj_has_state(FindButton(view.Root(), FONT_AWESOME_PAUSE), LV_STATE_DISABLED), "cannot pause before output is ready");
    std::vector<lv_obj_t*> volume_sliders;
    CollectSliders(view.Root(), volume_sliders);
    Check(volume_sliders.size() == 2, "player has separate seek and volume sliders");
    auto* volume_slider = volume_sliders[1];
    auto* seek_slider = volume_sliders[0];
    Check(lv_obj_has_state(seek_slider, LV_STATE_DISABLED), "unknown duration cannot accept seek");
    Check(lv_obj_has_state(volume_slider, LV_STATE_DISABLED), "preparing output cannot accept volume");
    Check(lv_slider_get_min_value(volume_slider) == 0 && lv_slider_get_max_value(volume_slider) == 100,
        "player volume includes mute and maximum");
    Screenshot(view.Root(), screenshots / "playback-preparing.ppm");
    state.playback.state = State::kPlaying; state.playback.output_samples = 16000ULL * 3723;
    view.Update(state); Tick();
    auto* position = FindLabel(view.Root(), "01:02:03");
    Check(position, "position uses output samples rather than wall clock");
    Click(view, FONT_AWESOME_PAUSE);
    const auto pause = requests.back();
    Check(pause.action == WatchViewAction::kPausePlayback && pause.playback_generation == 17, "pause identifies output owner");
    state.playback.state = State::kPausing;
    view.Update(state); view.Complete(pause.id, true); Tick();
    Check(FindLabel(view.Root(), WATCH_PLAYBACK_PAUSING) && !FindLabel(view.Root(), WATCH_PLAYBACK_PAUSED),
        "pause acceptance is distinct from output drain completion");
    state.playback.state = State::kPaused; view.Update(state); Tick();
    Check(FindLabel(view.Root(), "01:02:03") == position, "updates do not recreate the player");
    state.playback.duration_known = true; state.playback.total_samples = 16000ULL * 7200;
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), (std::string(WATCH_PLAYBACK_TOTAL) + ": 02:00:00").c_str()) &&
        !lv_obj_has_state(seek_slider, LV_STATE_DISABLED), "verified total enables seek without synchronized wall clock");
    lv_obj_add_state(seek_slider, LV_STATE_PRESSED);
    lv_slider_set_value(seek_slider, 500, LV_ANIM_OFF);
    view.Update(state); Tick();
    Check(lv_slider_get_value(seek_slider) == 500, "snapshot refresh does not reset seek drag");
    lv_obj_remove_state(seek_slider, LV_STATE_PRESSED);
    lv_obj_send_event(seek_slider, LV_EVENT_RELEASED, nullptr); Tick();
    const auto seek = requests.back();
    Check(seek.action == WatchViewAction::kSeekPlayback && seek.playback_generation == 17 &&
        seek.playback_target_sample == 16000ULL * 3600, "seek maps slider to exact current owner sample timeline");
    state.playback.state = State::kSeeking;
    state.playback.seek = {seek.id, seek.playback_target_sample, PlaybackSeekStatus::kPending};
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_WORKING) && FindLabel(view.Root(), WATCH_PLAYBACK_SEEKING) &&
        FindLabel(view.Root(), "01:02:03") && lv_obj_has_state(seek_slider, LV_STATE_DISABLED),
        "pending seek does not falsely publish target position");
    Check(!lv_obj_has_state(FindButton(view.Root(), FONT_AWESOME_STOP), LV_STATE_DISABLED), "seeking retains stop control");
    lv_obj_send_event(seek_slider, LV_EVENT_RELEASED, nullptr);
    Check(requests.back().id == seek.id, "pending seek cannot be duplicated");
    state.playback.seek = {seek.id - 1, 0, PlaybackSeekStatus::kApplied};
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_WORKING), "unrelated seek receipt cannot complete current request");
    state.playback.seek = {seek.id, seek.playback_target_sample, PlaybackSeekStatus::kApplied};
    state.playback.output_samples = seek.playback_target_sample; state.playback.state = State::kPaused;
    view.Update(state); Tick();
    Check(!FindLabel(view.Root(), WATCH_WORKING) && FindLabel(view.Root(), "01:00:00") &&
        FindLabel(view.Root(), WATCH_PLAYBACK_PAUSED) && lv_slider_get_value(seek_slider) == 500,
        "matching worker receipt publishes target and preserves paused state");
    Screenshot(view.Root(), screenshots / "playback-seek-applied.ppm");
    lv_slider_set_value(seek_slider, 1000, LV_ANIM_OFF);
    lv_obj_send_event(seek_slider, LV_EVENT_RELEASED, nullptr); Tick();
    const auto end_seek = requests.back();
    Check(end_seek.playback_target_sample == state.playback.total_samples, "slider endpoint requests exact end");
    state.playback.seek = {end_seek.id, end_seek.playback_target_sample, PlaybackSeekStatus::kFailed};
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_ACTION_FAILED) && lv_slider_get_value(seek_slider) == 500 &&
        FindLabel(view.Root(), "01:00:00"), "failed seek restores confirmed position without claiming completion");
    Screenshot(view.Root(), screenshots / "playback-paused.ppm");
    Check(!lv_obj_has_state(volume_slider, LV_STATE_DISABLED) && lv_slider_get_value(volume_slider) == 70,
        "paused player volume is independent of idle-only controls");
    lv_obj_add_state(volume_slider, LV_STATE_PRESSED);
    lv_slider_set_value(volume_slider, 0, LV_ANIM_OFF);
    lv_obj_send_event(volume_slider, LV_EVENT_VALUE_CHANGED, nullptr);
    view.Update(state); Tick();
    Check(lv_slider_get_value(volume_slider) == 0, "clock ticks cannot reset a volume drag");
    lv_obj_remove_state(volume_slider, LV_STATE_PRESSED);
    lv_obj_send_event(volume_slider, LV_EVENT_RELEASED, nullptr); Tick();
    const auto mute = requests.back();
    Check(mute.action == WatchViewAction::kSetPlaybackVolume && mute.playback_generation == 17 &&
        mute.level == WatchLevel::kVolume && mute.level_value == 0, "mute targets current playback without synchronized time");
    state.playback.volume = {mute.id, 0, Volume::kPending}; view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_SAVING) && lv_obj_has_state(volume_slider, LV_STATE_DISABLED),
        "accepted volume stays pending until actual worker completion");
    Check(!lv_obj_has_state(FindButton(view.Root(), FONT_AWESOME_STOP), LV_STATE_DISABLED), "stop remains available during volume save");
    lv_obj_send_event(volume_slider, LV_EVENT_RELEASED, nullptr);
    Check(requests.back().id == mute.id, "pending volume cannot be duplicated");
    state.playback.volume = {mute.id - 1, 0, Volume::kApplied}; view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_SAVING), "unrelated volume receipt cannot acknowledge the UI request");
    state.playback.volume = {mute.id, 0, Volume::kApplied}; state.controls.volume = 0;
    view.Update(state); Tick();
    Check(!FindLabel(view.Root(), WATCH_SAVING) && !lv_obj_has_state(volume_slider, LV_STATE_DISABLED) &&
        lv_slider_get_value(volume_slider) == 0 && FindLabel(view.Root(), WATCH_PLAYBACK_PAUSED),
        "persisted mute completes without resuming playback");
    lv_slider_set_value(volume_slider, 100, LV_ANIM_OFF);
    lv_obj_send_event(volume_slider, LV_EVENT_RELEASED, nullptr); Tick();
    const auto maximum = requests.back();
    Check(maximum.level_value == 100 && maximum.action == WatchViewAction::kSetPlaybackVolume, "maximum volume request supported");
    state.playback.volume = {maximum.id, 100, Volume::kFailed}; view.Update(state); Tick();
    Check(lv_slider_get_value(volume_slider) == 0 && FindLabel(view.Root(), WATCH_SAVE_FAILED),
        "failed volume transaction restores the saved UI value instead of claiming success");
    lv_obj_scroll_to_view_recursive(volume_slider, LV_ANIM_OFF); Tick();
    lv_area_t volume_area{}, error_area{};
    lv_obj_get_coords(volume_slider, &volume_area);
    lv_obj_get_coords(FindLabel(view.Root(), WATCH_SAVE_FAILED), &error_area);
    Check(volume_area.y1 >= 0 && volume_area.y2 < error_area.y1, "volume slider remains reachable above the save error footer");
    Screenshot(view.Root(), screenshots / "playback-volume-failed.ppm");
    Click(view, FONT_AWESOME_PLAY);
    Check(requests.back().action == WatchViewAction::kResumePlayback && requests.back().playback_generation == 17,
        "resume retains the same owner");
    state.playback.state = State::kPlaying;
    view.Update(state); view.Complete(requests.back().id, true); Tick();
    view.Navigate(WatchPage::kFace); Tick(); view.Navigate(WatchPage::kApps); Tick(); Click(view, WATCH_PLAYBACK);
    Check(view.Page() == WatchPage::kPlayback, "output controls remain reachable outside the catalog");
    volume_sliders.clear(); CollectSliders(view.Root(), volume_sliders);
    volume_slider = volume_sliders[1];
    lv_slider_set_value(volume_slider, 30, LV_ANIM_OFF);
    lv_obj_send_event(volume_slider, LV_EVENT_RELEASED, nullptr); Tick();
    const auto cancelled_volume = requests.back();
    Check(cancelled_volume.action == WatchViewAction::kSetPlaybackVolume, "volume remains available when reopening player");
    Click(view, FONT_AWESOME_STOP);
    const auto stop = requests.back();
    Check(stop.action == WatchViewAction::kStopPlayback && stop.playback_generation == 17, "stop carries exact owner");
    state.playback.state = State::kStopping;
    state.playback.volume = {cancelled_volume.id, 30, Volume::kCancelled};
    view.Update(state); view.Complete(stop.id, true); Tick();
    Check(FindLabel(view.Root(), WATCH_PLAYBACK_STOPPING) && !FindLabel(view.Root(), WATCH_PLAYBACK_STOPPED),
        "stop acceptance is not resource release");
    Check(!FindLabel(view.Root(), WATCH_SAVE_FAILED) && lv_obj_has_state(volume_slider, LV_STATE_DISABLED),
        "superseded volume receipt cannot fail stop or enable volume during cleanup");
    state.playback.state = State::kCleanupFailed; state.playback.error = RecordingPlaybackError::kCleanup;
    view.Update(state); Tick();
    Check(!lv_obj_has_state(FindButton(view.Root(), FONT_AWESOME_STOP), LV_STATE_DISABLED), "failed cleanup permits retry");
    Screenshot(view.Root(), screenshots / "playback-cleanup-failed.ppm");
    Click(view, FONT_AWESOME_STOP);
    Check(requests.back().playback_generation == 17, "retry retains failed output identity");
    state.playback.resources_held = false; state.playback.state = State::kFailed;
    state.playback.error = RecordingPlaybackError::kDecode;
    view.Update(state); view.Complete(requests.back().id, true); Tick();
    Check(FindLabel(view.Root(), WATCH_PLAYBACK_DECODE_FAILED) && !FindLabel(view.Root(), WATCH_PLAYBACK_ENDED),
        "decode failure remains a failure after cleanup");
    state.playback = {}; view.Update(state); Tick();
    Check(FindLabel(view.Root(), WATCH_PLAYBACK_EMPTY), "empty player does not imply success");
}

void TestOrphanCleanupDiskUi(WatchViewState state, const std::filesystem::path& screenshots) {
    char pattern[] = "/tmp/audream-orphan-ui-XXXXXX";
    const auto* created = mkdtemp(pattern); Check(created, "temporary recording directory created");
    const std::filesystem::path directory(created);
    const std::string id = "interrupted-delete";
    const auto audio = (directory / (id + ".opus")).string();
    const auto metadata = audio + ".json";
    auto write = [](const std::string& path, size_t bytes) {
        std::ofstream file(path, std::ios::binary); file << std::string(bytes, 'a');
        Check(file.good(), "test recording file written");
    };
    write(audio, 100); write(metadata, 40);
    struct FailMetadata : RecordingDeleteIo {
        int Remove(const char* path) override {
            if (std::string(path).find(".json") != std::string::npos) { errno = EIO; return -1; }
            return RecordingDeleteIo::Remove(path);
        }
    } failing;
    Check(DeleteRecordingFiles(audio, 100, false, failing).status == RecordingDeleteStatus::kPartial,
        "fixture leaves real metadata after audio deletion");
    {
        WatchRecordingLibrary library;
        RecordingDeleteIo io;
        std::vector<WatchViewRequest> requests;
        state.deletion = {};
        state.can_delete_recording = true;
        state.time.sleep_prepared = false;
        state.library = {};
        WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48, &font_awesome_30_4,
            [&](const auto& request) { requests.push_back(request); });
        auto load = [&]() {
            const auto request = requests.back();
            Check(request.action == WatchViewAction::kLoadRecordings, "view requested a fresh catalog scan");
            Check(library.Begin(directory.string()), "real recording directory opens");
            while (library.Step()) {}
            state.library = library.Snapshot();
            view.Update(state); view.Complete(request.id, true); Tick();
        };
        view.Update(state); view.Navigate(WatchPage::kRecordings); Tick(); load();
        Check(state.library.entries.size() == 1 && !state.library.entries.front().audio_available && !state.deletion.request,
            "fresh catalog discovers leftover metadata without a runtime deletion receipt");
        Screenshot(view.Root(), screenshots / "recordings-orphan.ppm");
        Click(view, WATCH_LIBRARY_AUDIO_MISSING);
        Check(FindLabel(view.Root(), WATCH_LIBRARY_AUDIO_MISSING) && !FindLabel(view.Root(), WATCH_RECORDER_SAVED),
            "orphan details never claim playable or saved audio");
        Click(view, WATCH_RECORDING_METADATA_CLEANUP);
        Check(FindLabel(view.Root(), WATCH_RECORDING_METADATA_WARNING) && std::filesystem::exists(metadata),
            "opening cleanup confirmation leaves actual metadata untouched");
        Screenshot(view.Root(), screenshots / "recording-orphan-confirm.ppm");
        auto* confirmation_body = lv_obj_get_parent(FindLabel(view.Root(), WATCH_RECORDING_METADATA_WARNING));
        lv_obj_scroll_to_y(confirmation_body, 100000, LV_ANIM_OFF); Tick();
        lv_area_t back_bounds;
        lv_obj_get_coords(FindButton(view.Root(), WATCH_RECORDING_DELETE_CANCEL), &back_bounds);
        Check(back_bounds.y1 >= 64 && back_bounds.y2 < 502,
            "orphan confirmation back button remains reachable below the full filename and warning");
        Screenshot(view.Root(), screenshots / "recording-orphan-confirm-bottom.ppm");
        Click(view, WATCH_RECORDING_DELETE_CANCEL); load();
        Check(std::filesystem::exists(metadata), "canceling orphan cleanup preserves the file");
        Click(view, WATCH_LIBRARY_AUDIO_MISSING); Click(view, WATCH_RECORDING_METADATA_CLEANUP);
        write(audio, 100);
        Click(view, WATCH_RECORDING_METADATA_CLEANUP);
        auto request = requests.back();
        Check(request.action == WatchViewAction::kDeleteRecording && request.recording_id == id &&
            request.metadata_only && !request.cleanup_request && request.library_revision == library.Snapshot().revision,
            "orphan confirmation independently authorizes metadata only against the displayed revision");
        auto result = DeleteRecordingFiles(audio, 40, request.metadata_only, io);
        Check(result.status == RecordingDeleteStatus::kChanged && std::filesystem::exists(audio) && std::filesystem::exists(metadata),
            "actual deletion refuses the audio file recreated after orphan confirmation");
        state.deletion = {request.id, id, result, false};
        view.Update(state); view.Complete(request.id, false); Tick();
        Check(FindLabel(view.Root(), WATCH_RECORDING_DELETE_CHANGED), "disk identity change reaches the confirmation UI");
        Click(view, WATCH_RECORDING_DELETE_CANCEL); load();
        Check(state.library.entries.size() == 1 && state.library.entries.front().audio_available,
            "fresh scan replaces the orphan with the actual audio entry without duplication");
        std::filesystem::remove(audio);
        Click(view, WATCH_LIBRARY_REFRESH); load();
        Click(view, WATCH_LIBRARY_AUDIO_MISSING); Click(view, WATCH_RECORDING_METADATA_CLEANUP);
        Click(view, WATCH_RECORDING_METADATA_CLEANUP); request = requests.back();
        result = DeleteRecordingFiles(audio, 40, request.metadata_only, io);
        Check(result.status == RecordingDeleteStatus::kDeleted && result.removed_bytes == 40 && !std::filesystem::exists(metadata),
            "confirmed orphan cleanup removes only the remaining metadata");
        library.Forget(id); state.library = library.Snapshot();
        state.deletion = {request.id, id, result, false};
        view.Update(state); view.Complete(request.id, true); Tick(); load();
        Check(state.library.entries.empty() && FindLabel(view.Root(), WATCH_LIBRARY_EMPTY),
            "actual cleanup completion returns to the empty scanned catalog");
    }
    std::filesystem::remove_all(directory);
}

void TestRecordingDeleteUi(WatchViewState state, const std::filesystem::path& screenshots) {
    std::vector<WatchViewRequest> requests;
    state.can_delete_recording = true;
    state.time.sleep_prepared = false;
    state.library.status = WatchLibraryStatus::kReady;
    state.library.has_older = state.library.has_newer = false;
    WatchRecordingEntry entry;
    entry.id = "saved-record"; entry.file_name = "saved-record.opus";
    entry.size_bytes = 100; entry.duration_ms = 60000; entry.metadata_available = true;
    state.library.entries = {entry}; ++state.library.revision;
    WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48, &font_awesome_30_4,
        [&](const auto& request) { requests.push_back(request); });
    view.Update(state); view.Navigate(WatchPage::kRecordings); Tick();
    view.Complete(requests.back().id, true); Tick();
    Click(view, entry.file_name);
    state.can_delete_recording = false; view.Update(state); Tick();
    const auto before = requests.size();
    Click(view, WATCH_RECORDING_DELETE);
    Check(view.Page() == WatchPage::kRecordingDetail && requests.size() == before,
        "recording or transfer disables entry to deletion confirmation");
    state.can_delete_recording = true; view.Update(state); Tick();
    Click(view, WATCH_RECORDING_DELETE);
    Check(view.Page() == WatchPage::kRecordingDeleteConfirm && requests.size() == before,
        "opening confirmation has no deletion side effect");
    Check(FindLabel(view.Root(), WATCH_RECORDING_DELETE_WARNING), "confirmation states local scope and unconfirmed phone sync");
    auto* body = lv_obj_get_parent(FindLabel(view.Root(), WATCH_RECORDING_DELETE_WARNING));
    lv_obj_scroll_to_y(body, 100000, LV_ANIM_OFF); Tick();
    lv_area_t bounds;
    lv_obj_get_coords(lv_obj_get_parent(FindLabel(view.Root(), WATCH_RECORDING_DELETE_CANCEL)), &bounds);
    Check(bounds.y1 >= 64 && bounds.y2 < 502, "confirmation back button is reachable below long warning text");
    Screenshot(view.Root(), screenshots / "recording-delete-confirm.ppm");
    Click(view, WATCH_RECORDING_DELETE_CANCEL);
    Check(requests.back().action == WatchViewAction::kLoadRecordings && requests.size() == before + 1,
        "cancel reloads the list without issuing delete");
    view.Complete(requests.back().id, true); Tick();
    Click(view, entry.file_name); Click(view, WATCH_RECORDING_DELETE);
    ++state.library.revision; view.Update(state); Tick();
    const auto stale_count = requests.size();
    Click(view, WATCH_RECORDING_DELETE);
    Check(requests.size() == stale_count && FindLabel(view.Root(), WATCH_RECORDING_DELETE_CHANGED),
        "changed library invalidates the old confirmation");
    Click(view, WATCH_RECORDING_DELETE_CANCEL); view.Complete(requests.back().id, true); Tick();
    Click(view, entry.file_name); Click(view, WATCH_RECORDING_DELETE); Click(view, WATCH_RECORDING_DELETE);
    const auto deletion = requests.back();
    Check(deletion.action == WatchViewAction::kDeleteRecording && deletion.recording_id == entry.id &&
        deletion.library_revision == state.library.revision && deletion.cleanup_request == 0,
        "confirmed deletion contains the selected file and reviewed revision");
    const auto deleting_count = requests.size();
    Click(view, WATCH_RECORDING_DELETE); Click(view, WATCH_RECORDING_DELETE_CANCEL); Click(view, FONT_AWESOME_ANGLE_LEFT);
    Check(requests.size() == deleting_count && view.Page() == WatchPage::kRecordingDeleteConfirm,
        "in-flight deletion cannot be duplicated or falsely canceled");
    view.Complete(deletion.id + 100, true);
    Check(view.Page() == WatchPage::kRecordingDeleteConfirm, "unrelated completion cannot report deletion success");
    state.deletion = {deletion.id, entry.id, {RecordingDeleteStatus::kPartial, 100, true, true}, true};
    state.library.entries.clear(); ++state.library.revision;
    view.Update(state); view.Complete(deletion.id, false); Tick();
    Check(view.Page() == WatchPage::kRecordingDeleteConfirm && FindLabel(view.Root(), WATCH_RECORDING_DELETE_PARTIAL),
        "partial deletion remains explicit and does not navigate as success");
    body = lv_obj_get_parent(FindLabel(view.Root(), WATCH_RECORDING_DELETE_WARNING));
    lv_obj_scroll_to_y(body, 100000, LV_ANIM_OFF); Tick();
    Screenshot(view.Root(), screenshots / "recording-delete-partial.ppm");
    Click(view, WATCH_RECORDING_DELETE_CANCEL);
    state.library.entries = {entry}; ++state.library.revision;
    view.Update(state);
    view.Complete(requests.back().id, true); Tick();
    Check(FindButton(view.Root(), WATCH_RECORDING_DELETE_RETRY), "partial cleanup remains accessible after leaving confirmation");
    Click(view, WATCH_RECORDING_DELETE_RETRY);
    Check(view.Page() == WatchPage::kRecordingDeleteConfirm && FindLabel(view.Root(), WATCH_RECORDING_DELETE_PARTIAL),
        "reopening cleanup restores the original result and file identity");
    Click(view, WATCH_RECORDING_DELETE_RETRY);
    const auto retry = requests.back();
    Check(retry.action == WatchViewAction::kDeleteRecording && retry.cleanup_request == deletion.id && retry.recording_id == entry.id,
        "cleanup retries reference the partial result even if same-named audio reappears in the library");
    Check(retry.metadata_only, "runtime cleanup retries cannot authorize audio deletion");
    state.deletion = {retry.id, entry.id, {RecordingDeleteStatus::kChanged, 0, false, false}, false};
    view.Update(state); view.Complete(retry.id, false); Tick();
    Check(FindLabel(view.Root(), WATCH_RECORDING_DELETE_CHANGED) &&
        lv_obj_has_state(FindButton(view.Root(), WATCH_RECORDING_METADATA_CLEANUP), LV_STATE_DISABLED),
        "recreated audio requires a new confirmation instead of another cleanup retry");
    Click(view, WATCH_RECORDING_DELETE_CANCEL);
    state.library.entries = {entry}; ++state.library.revision;
    view.Update(state); view.Complete(requests.back().id, true); Tick();
    Click(view, entry.file_name); Click(view, WATCH_RECORDING_DELETE); Click(view, WATCH_RECORDING_DELETE);
    const auto confirmed = requests.back();
    Check(confirmed.cleanup_request == 0, "new confirmation does not reuse old cleanup authorization");
    state.deletion = {confirmed.id, entry.id, {RecordingDeleteStatus::kDeleted, 140, true, true}, false};
    state.library.entries.clear(); ++state.library.revision;
    view.Update(state); view.Complete(confirmed.id, true); Tick();
    Check(view.Page() == WatchPage::kRecordings && requests.back().action == WatchViewAction::kLoadRecordings,
        "only actual completed deletion returns to a freshly requested library");
    view.Complete(requests.back().id, true); Tick();
}

void TestMotionWorkerUi(WatchViewState state, const std::filesystem::path& screenshots, bool corrupt_mode = false) {
    using namespace std::chrono_literals;
    std::atomic<bool> fail_load{!corrupt_mode}, block_load{false}, loading{false}, corrupt{corrupt_mode};
    std::atomic<unsigned> saves{0};
    WatchActivityCheckpoint seed;
    seed.count = 1;
    seed.days[0] = {2026 * 366, 42};
    auto record = *EncodeWatchActivityCheckpoint(seed);
    const auto epoch = std::chrono::steady_clock::now();
    WatchMotionTask task({
        []() { return true; }, []() { return uint64_t(1); },
        [](WatchImuReading&) { return false; }, []() { return true; },
        [&](WatchActivityRecord& output, bool) {
            loading = true;
            while (block_load) std::this_thread::sleep_for(1ms);
            if (fail_load) return WatchMotionLoad::kFailed;
            if (corrupt) { output.fill(0xff); return WatchMotionLoad::kLoaded; }
            output = record;
            return WatchMotionLoad::kLoaded;
        },
        [&](const WatchActivityRecord& output, bool) { ++saves; record = output; corrupt = false; return true; },
        [&]() {
            return WatchMotionClock{static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch).count()), 2026 * 366};
        },
        [corrupt_mode]() { return !corrupt_mode; },
    });
    Check(task.Start(), "real motion worker starts for LVGL integration");
    std::vector<WatchViewRequest> queued;
    WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48,
        &font_awesome_30_4, [&](const auto& request) { queued.push_back(request); });
    view.Navigate(WatchPage::kActivity);
    const auto pump = [&](auto done) {
        const auto until = std::chrono::steady_clock::now() + 3s;
        while (!done() && std::chrono::steady_clock::now() < until) {
            state.motion = task.Snapshot();
            view.Update(state); Tick();
            std::this_thread::sleep_for(1ms);
        }
        Check(done(), "motion worker and LVGL reach the expected state");
        state.motion = task.Snapshot();
        view.Update(state); Tick();
    };
    pump([&]() { return task.Snapshot().storage_error; });
    if (corrupt_mode) {
        const auto token = task.Snapshot().repair_token;
        Check(token != 0, "actual corrupt record offers an authorized recovery token");
        Click(view, WATCH_ACTIVITY_RESET);
        Check(view.Page() == WatchPage::kActivityResetConfirm && queued.empty() && saves == 0,
            "opening confirmation does not request or perform deletion");
        Screenshot(view.Root(), screenshots / "activity-reset-confirm.ppm");
        auto* cancel_button = lv_obj_get_parent(FindLabel(view.Root(), WATCH_ACTIVITY_RESET_CANCEL));
        lv_obj_scroll_to_view(cancel_button, LV_ANIM_OFF); Tick();
        lv_area_t cancel_bounds, content_bounds;
        lv_obj_get_coords(cancel_button, &cancel_bounds);
        lv_obj_get_content_coords(lv_obj_get_parent(cancel_button), &content_bounds);
        Check(cancel_bounds.y1 >= content_bounds.y1 && cancel_bounds.y2 <= content_bounds.y2,
            "cancel remains fully reachable with an active timer alert");
        Screenshot(view.Root(), screenshots / "activity-reset-confirm-bottom.ppm");
        Click(view, WATCH_ACTIVITY_RESET_CANCEL);
        Check(view.Page() == WatchPage::kActivity && queued.empty() && saves == 0,
            "cancel leaves corrupt activity storage unchanged");
        Click(view, WATCH_ACTIVITY_RESET);
        state.motion.repair_token = token + 1;
        view.Update(state); Tick();
        Click(view, WATCH_ACTIVITY_RESET_CONFIRM);
        Check(queued.empty(), "changed history invalidates the old confirmation");
        state.motion = task.Snapshot(); view.Update(state); Tick();
        block_load = true;
        loading = false;
        Click(view, WATCH_ACTIVITY_RESET_CONFIRM);
        Check(queued.size() == 1 && queued.back().action == WatchViewAction::kResetCorruptActivityStorage &&
            queued.back().repair_token == token && task.ResetCorruptStorage(queued.back().id, token),
            "explicit confirmation forwards its token to the real worker");
        pump([&]() { return loading.load(); });
        Check(task.Snapshot().reset_pending && FindLabel(view.Root(), WATCH_WORKING) && saves == 0,
            "blocked recheck keeps confirmation pending without optimistic deletion");
        Click(view, WATCH_ACTIVITY_RESET_CANCEL);
        Check(view.Page() == WatchPage::kActivityResetConfirm, "cancel is disabled after committing the operation");
        Screenshot(view.Root(), screenshots / "activity-reset-pending.ppm");
        block_load = false;
        pump([&]() { return !task.Snapshot().reset_pending; });
        Check(task.Snapshot().reset_succeeded && task.Snapshot().initialized && task.Snapshot().history.count == 0 &&
            view.Page() == WatchPage::kActivity && saves == 1,
            "successful actual reset returns to activity only after replacement storage succeeds");
        Screenshot(view.Root(), screenshots / "activity-reset-complete.ppm");
        Check(task.Suspend(2000), "reset integration leaves the worker parked");
        return;
    }
    block_load = true;
    loading = false;
    Click(view, WATCH_ACTIVITY_RETRY);
    Check(queued.size() == 1 && task.RetryStorage(queued.back().id), "queued UI command reaches actual worker");
    pump([&]() { return loading.load(); });
    Check(FindLabel(view.Root(), WATCH_WORKING) && task.Snapshot().retry_pending,
        "blocked native storage keeps actual UI request pending");
    block_load = false;
    pump([&]() { return !task.Snapshot().retry_pending; });
    Check(FindLabel(view.Root(), WATCH_ACTION_FAILED) && saves == 0,
        "actual load failure reaches UI without rewriting history");
    fail_load = false;
    Click(view, WATCH_ACTIVITY_RETRY);
    Check(queued.size() == 2 && task.RetryStorage(queued.back().id), "failed UI request permits explicit retry");
    pump([&]() { return !task.Snapshot().retry_pending; });
    Check(task.Snapshot().retry_succeeded && FindLabel(view.Root(), "42") && !FindLabel(view.Root(), WATCH_WORKING),
        "actual recovered worker checkpoint reaches activity UI");
    Click(view, WATCH_ACTIVITY_HISTORY);
    Check(FindLabel(view.Root(), "2026-01-01") && FindLabel(view.Root(), std::string(WATCH_ACTIVITY_STEPS) + ": 42"),
        "history renders the recovered worker checkpoint");
    Check(task.Suspend(2000), "motion integration leaves the worker parked");
}
}

void TestWristSettingsUi(WatchViewState state, const std::filesystem::path& screenshots) {
    state.time.initialized = false;
    state.time.sleep_prepared = false;
    state.time.storage_error = state.time.interrupted = false;
    state.motion.initialized = true;
    state.motion.activity.status = WatchMotionStatus::kRunning;
    WatchWristPreferences saved;
    bool present = false, fail_save = false;
    unsigned writes = 0;
    WatchWristControl control({
        [&](WatchWristPreferences& preferences) {
            preferences = saved;
            return present ? WatchWristLoad::kLoaded : WatchWristLoad::kAbsent;
        },
        [&](const WatchWristPreferences& preferences) {
            ++writes;
            if (fail_save) return false;
            saved = preferences; present = true; return true;
        },
        []() {},
    });
    control.Poll(nullptr, 0, true, false);
    std::vector<WatchViewRequest> requests;
    WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48,
        &font_awesome_30_4, [&](const WatchViewRequest& request) {
            requests.push_back(request);
            Check(request.action == WatchViewAction::kWristCommand &&
                control.Request(request.id, request.wrist_action, request.enabled), "UI wrist request accepted by the owner");
        });
    const auto refresh = [&]() { state.wrist = control.Snapshot(); view.Update(state); Tick(); };
    const auto toggle = [&]() {
        auto* label = FindLabel(view.Root(), WATCH_WRIST_WAKE);
        // The page heading repeats the setting label; locate the actual switch by type.
        Check(label != nullptr, "wrist setting is visible");
        std::function<lv_obj_t*(lv_obj_t*)> find = [&](lv_obj_t* object) -> lv_obj_t* {
            if (lv_obj_check_type(object, &lv_switch_class)) return object;
            for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
                if (auto* result = find(lv_obj_get_child(object, i))) return result;
            return nullptr;
        };
        auto* result = find(view.Root());
        Check(result != nullptr, "wrist switch exists");
        return result;
    };
    const auto set_enabled = [&](bool enabled) {
        auto* object = toggle();
        Check(!lv_obj_has_state(object, LV_STATE_DISABLED), "wrist switch can be operated");
        if (enabled) lv_obj_add_state(object, LV_STATE_CHECKED);
        else lv_obj_remove_state(object, LV_STATE_CHECKED);
        lv_obj_send_event(object, LV_EVENT_VALUE_CHANGED, nullptr); Tick();
    };
    refresh(); view.Navigate(WatchPage::kControls); Tick();
    Click(view, WATCH_WRIST_WAKE);
    Check(view.Page() == WatchPage::kWristSettings, "controls open wrist settings");
    Check(lv_obj_has_state(toggle(), LV_STATE_DISABLED) && FindLabel(view.Root(), WATCH_WRIST_CALIBRATION_REQUIRED),
        "uncalibrated wrist setting cannot be enabled");
    Screenshot(view.Root(), screenshots / "wrist-uncalibrated.ppm");
    Click(view, WATCH_WRIST_CALIBRATE);
    WatchImuReading sample{1, 0, 0, 0, -1};
    control.Poll(&sample, 1000, true, false); refresh();
    Check(FindLabel(view.Root(), WATCH_WRIST_KEEP_STILL) &&
        !lv_obj_has_state(FindButton(view.Root(), WATCH_WRIST_CANCEL), LV_STATE_DISABLED), "calibration can be canceled while collecting");
    Screenshot(view.Root(), screenshots / "wrist-calibrating.ppm");
    for (uint64_t now = 1250; now <= 3000; now += 250) control.Poll(&sample, now, true, false);
    refresh();
    Check(present && writes == 1 && saved.normal && !saved.enabled &&
        !lv_obj_has_state(toggle(), LV_STATE_CHECKED) && FindLabel(view.Root(), WATCH_WRIST_CALIBRATED),
        "saved calibration never implicitly enables raise to wake");
    set_enabled(true);
    Check(!lv_obj_has_state(toggle(), LV_STATE_CHECKED) && !saved.enabled, "enable waits for storage acknowledgment");
    control.Poll(nullptr, 3500, true, false); refresh();
    Check(saved.enabled && lv_obj_has_state(toggle(), LV_STATE_CHECKED), "durable enable reaches the switch");
    Screenshot(view.Root(), screenshots / "wrist-enabled.ppm");
    fail_save = true;
    set_enabled(false); refresh();
    Check(!lv_obj_has_state(toggle(), LV_STATE_CHECKED), "disable is immediate in memory");
    control.Poll(nullptr, 4000, true, false); refresh();
    Check(saved.enabled && !control.Snapshot().enabled && FindLabel(view.Root(), WATCH_SAVE_FAILED),
        "failed disable is explicit and does not claim persistence");
    Screenshot(view.Root(), screenshots / "wrist-save-failed.ppm");
    fail_save = false;
    Click(view, WATCH_WRIST_RETRY_DISABLE);
    control.Poll(nullptr, 4250, true, false); refresh();
    Check(!saved.enabled && !control.Snapshot().storage_error, "failed disable has a direct durable retry");
    Click(view, WATCH_WRIST_CALIBRATE);
    control.Poll(&sample, 4500, true, false); refresh();
    const auto old_request = requests.back().id;
    Click(view, WATCH_WRIST_CANCEL); refresh();
    Check(requests.back().id > old_request && requests.back().wrist_action == WatchWristAction::kCancel,
        "cancel supersedes the old calibration request");
    view.Complete(old_request, true);
    Check(lv_obj_has_state(FindButton(view.Root(), WATCH_WRIST_CALIBRATE), LV_STATE_DISABLED),
        "stale success cannot complete the cancellation");
    control.Poll(nullptr, 4750, true, false); refresh();
    Check(!saved.enabled && control.Snapshot().succeeded, "cancel durably disables the old enabled preference");
    Click(view, WATCH_WRIST_CALIBRATE);
    control.Poll(&sample, 5000, true, false); refresh();
    view.Navigate(WatchPage::kControls); Tick();
    Check(requests.back().wrist_action == WatchWristAction::kCancel, "leaving calibration requests cancellation");
    control.Poll(nullptr, 5250, true, false); refresh();
    Click(view, WATCH_WRIST_WAKE);
    Click(view, WATCH_WRIST_CALIBRATE);
    control.Poll(&sample, 5500, true, false); refresh();
    view.SetVisible(false);
    Check(requests.back().wrist_action == WatchWristAction::kCancel, "hiding the screen cancels calibration");
    control.Poll(nullptr, 5750, true, true); refresh();
    view.SetVisible(true); Tick();
    state.motion.activity.status = WatchMotionStatus::kPaused; refresh();
    Check(FindLabel(view.Root(), WATCH_WRIST_PAUSED) && lv_obj_has_state(toggle(), LV_STATE_DISABLED),
        "paused motion does not offer a misleading enable control");
    Screenshot(view.Root(), screenshots / "wrist-paused.ppm");
    std::puts("Watch wrist UI: calibration, persistence acknowledgment, failure, cancellation, and pause checks passed");
}

void TestTouchLockPointer(WatchViewState state) {
    std::vector<WatchViewRequest> requests;
    WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48,
        &font_awesome_30_4, [&](const WatchViewRequest& request) { requests.push_back(request); });
    view.Update(state); view.Navigate(WatchPage::kControls); Tick();
    Click(view, FONT_AWESOME_LOCK);
    auto* unlock = FindButton(view.Root(), WATCH_TOUCH_UNLOCK);
    lv_obj_update_layout(view.Root());
    lv_area_t bounds; lv_obj_get_coords(unlock, &bounds);
    struct Touch { lv_point_t point; bool pressed = false; } touch{
        {(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2}};
    const auto center = touch.point;
    auto* input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(input, &touch);
    lv_indev_set_read_cb(input, [](lv_indev_t* device, lv_indev_data_t* data) {
        const auto& sample = *static_cast<Touch*>(lv_indev_get_user_data(device));
        data->point = sample.point;
        data->state = sample.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    });
    touch.pressed = true; Tick();
    for (int index = 0; index < 45; ++index) Tick();
    touch.point = {2, 2}; Tick();
    touch.pressed = false; Tick();
    Check(view.TouchLocked(), "pointer drag outside cancels a completed unlock hold");
    touch.point = center; touch.pressed = true; Tick();
    state.recording = true; view.Update(state); Tick();
    lv_obj_update_layout(view.Root());
    lv_area_t updated_bounds; lv_obj_get_coords(unlock, &updated_bounds);
    Check(updated_bounds.x1 == bounds.x1 && updated_bounds.x2 == bounds.x2 &&
        updated_bounds.y1 == bounds.y1 && updated_bounds.y2 == bounds.y2,
        "recording status updates cannot move a pressed unlock target");
    for (int index = 0; index < 45; ++index) Tick();
    Check(view.TouchLocked(), "real pointer remains locked while held down");
    touch.pressed = false; Tick();
    Check(!view.TouchLocked() && view.Page() == WatchPage::kControls && requests.empty(),
        "pointer release unlocks without activating the underlying slider or button");
    auto* back = FindButton(view.Root(), FONT_AWESOME_ANGLE_LEFT);
    lv_obj_update_layout(view.Root()); lv_obj_get_coords(back, &bounds);
    touch.point = {(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    touch.pressed = true; Tick(); touch.pressed = false; Tick();
    Check(view.Page() == WatchPage::kFace, "next independent pointer press works after unlock");
    lv_indev_delete(input);
    std::cout << "PASS: pointer-driven unlock, drag cancellation and no click-through\n";
}

void TestHiddenWatchTouchWake(const WatchViewState& state) {
    WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48,
        &font_awesome_30_4, [](const WatchViewRequest&) {});
    view.Update(state); Tick();
    auto* button = FindButton(view.Root(), FONT_AWESOME_WATCH);
    Check(button != nullptr, "wake test launcher exists");
    lv_area_t bounds;
    lv_obj_get_coords(button, &bounds);
    struct Touch {
        lv_point_t point;
        bool pressed = false;
        int wakes = 0;
        lv_obj_t* root;
    } touch{{(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2}, false, 0, view.Root()};
    auto* input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(input, &touch);
    lv_indev_set_read_cb(input, [](lv_indev_t* device, lv_indev_data_t* data) {
        auto& sample = *static_cast<Touch*>(lv_indev_get_user_data(device));
        data->point = sample.point;
        data->state = sample.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    });
    lv_indev_add_event_cb(input, [](lv_event_t* event) {
        auto* device = static_cast<lv_indev_t*>(lv_event_get_user_data(event));
        auto& sample = *static_cast<Touch*>(lv_indev_get_user_data(device));
        if (lv_obj_has_flag(sample.root, LV_OBJ_FLAG_HIDDEN)) {
            ++sample.wakes;
            lv_indev_wait_release(device);
        }
    }, LV_EVENT_PRESSED, input);
    view.SetVisible(false); Tick();
    touch.pressed = true; Tick();
    Check(touch.wakes == 1, "input device receives touch while the watch root is hidden");
    view.SetVisible(true); Tick();
    touch.pressed = false; Tick();
    Check(view.Page() == WatchPage::kFace, "wake touch cannot click a newly revealed control");
    touch.pressed = true; Tick();
    touch.pressed = false; Tick();
    Check(view.Page() == WatchPage::kApps, "the next touch operates the restored watch normally");
    lv_indev_delete(input);
}

void TestFirmwareUi(WatchViewState state, const std::filesystem::path& screenshots) {
    using watch_update::Phase;
    state.time.initialized = false;
    state.time.sleep_prepared = state.time.storage_error = state.time.interrupted = false;
    state.time.state.alarm_alerts = 0;
    state.time.state.checkpoint.timer_alert = false;
    state.alert_sound = {};
    state.notifications = {};
    state.firmware = {};
    state.firmware.available = state.firmware.can_check = true;
    state.firmware.blocked = WatchUpdateBlock::kNone;
    state.firmware.current_version = "2.2.3";
    std::vector<WatchViewRequest> requests;
    WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48,
        &font_awesome_30_4, [&](const auto& request) { requests.push_back(request); });
    view.Update(state); view.Navigate(WatchPage::kSettings); Tick();
    Click(view, WATCH_FIRMWARE);
    Check(view.Page() == WatchPage::kFirmwareUpdate && requests.empty(), "opening updates never starts a network request");
    Screenshot(view.Root(), screenshots / "firmware-idle.ppm");
    Click(view, WATCH_UPDATE_CHECK);
    auto check = requests.back();
    Check(check.action == WatchViewAction::kCheckFirmware, "firmware check does not depend on initialized calendar state");
    Click(view, FONT_AWESOME_ANGLE_LEFT);
    auto cancelled = requests.back();
    Check(cancelled.action == WatchViewAction::kCancelFirmware && cancelled.firmware_request == check.id,
        "leaving before admission cancels the exact queued check request");
    view.Complete(check.id, true); Tick();
    Check(FindLabel(view.Root(), WATCH_WORKING), "late check admission cannot clear newer cancellation");
    view.Complete(cancelled.id, true); Tick();
    view.Navigate(WatchPage::kFirmwareUpdate); Tick();
    Click(view, WATCH_UPDATE_CHECK); check = requests.back();
    state.firmware.request = check.id;
    state.firmware.network_preparing = true;
    state.firmware.can_check = false;
    view.Update(state); view.Complete(check.id, true); Tick();
    Check(FindLabel(view.Root(), WATCH_UPDATE_CONNECTING), "network preparation has its own status");
    Screenshot(view.Root(), screenshots / "firmware-connecting.ppm");
    state.firmware.network_preparing = false;
    state.firmware.can_install = true;
    state.firmware.update.phase = Phase::kAvailable;
    state.firmware.update.attempt = 4;
    state.firmware.update.sequence = 20;
    state.firmware.update.version = "3.0.0";
    state.firmware.update.bytes = 4000000;
    view.Update(state); Tick();
    const auto before_review = requests.size();
    Click(view, WATCH_UPDATE_REVIEW);
    Check(view.Page() == WatchPage::kFirmwareConfirm && requests.size() == before_review,
        "reviewing a version requires a separate physical install confirmation");
    auto* firmware_body = lv_obj_get_parent(FindButton(view.Root(), WATCH_UPDATE_INSTALL));
    lv_obj_scroll_to_y(firmware_body, 100000, LV_ANIM_OFF); Tick();
    lv_area_t cancel_bounds;
    lv_obj_get_coords(FindButton(view.Root(), WATCH_UPDATE_CANCEL), &cancel_bounds);
    Check(cancel_bounds.y1 >= 64 && cancel_bounds.y2 < 502, "firmware confirmation bottom action is reachable");
    Screenshot(view.Root(), screenshots / "firmware-confirm.ppm");
    ++state.firmware.update.sequence;
    state.firmware.update.version = "3.0.1";
    view.Update(state); Tick();
    Click(view, WATCH_UPDATE_INSTALL);
    Check(requests.size() == before_review && FindLabel(view.Root(), WATCH_UPDATE_EXPIRED),
        "a changed available version cannot replace the version under confirmation");
    Click(view, FONT_AWESOME_ANGLE_LEFT); Click(view, WATCH_UPDATE_REVIEW);
    Click(view, WATCH_UPDATE_INSTALL);
    auto install = requests.back();
    Check(install.action == WatchViewAction::kInstallFirmware && install.firmware_attempt == 4 &&
        install.firmware_sequence == 21 && install.firmware_request == check.id, "installation identifies the reviewed attempt and sequence");
    state.firmware.update.phase = Phase::kInstalling;
    state.firmware.update.active = true;
    state.firmware.update.progress = 100;
    state.firmware.update.bytes_per_second = 4096;
    state.firmware.can_install = false;
    view.Update(state); view.Complete(install.id, true); Tick();
    Check(FindLabel(view.Root(), "99%  4 KiB/s"), "unconfirmed progress never displays 100 percent");
    Click(view, WATCH_UPDATE_CANCEL);
    cancelled = requests.back();
    state.firmware.update.cancelling = true;
    view.Update(state); view.Complete(cancelled.id, true); Tick();
    Check(FindLabel(view.Root(), WATCH_UPDATE_STOPPING), "cancel admission does not pretend the worker has stopped");
    Screenshot(view.Root(), screenshots / "firmware-stopping.ppm");
    state.firmware.update.cancelling = false;
    state.firmware.update.commit_started = true;
    state.firmware.update.phase = Phase::kCommitting;
    view.Update(state); Tick();
    const auto before_commit = requests.size();
    Click(view, WATCH_UPDATE_CANCEL);
    view.SetVisible(false); view.SetVisible(true); Tick();
    Check(requests.size() == before_commit, "late cancellation and hiding cannot cancel boot selection");
    Screenshot(view.Root(), screenshots / "firmware-committing.ppm");
    state.firmware.update.active = false;
    state.firmware.update.boot_selected = true;
    state.firmware.update.phase = Phase::kRebootRequired;
    state.firmware.can_reboot = true;
    view.Update(state); Tick();
    Check(FindLabel(view.Root(), "100%  4 KiB/s"), "only confirmed boot selection displays completion");
    Click(view, WATCH_UPDATE_RESTART);
    Check(requests.back().action == WatchViewAction::kRebootFirmware && requests.back().firmware_attempt == 4,
        "restart is an explicit scoped command");
    view.Complete(requests.back().id, false); Tick();
    Screenshot(view.Root(), screenshots / "firmware-reboot.ppm");
    for (const auto phase : {Phase::kNoUpdate, Phase::kCheckFailed, Phase::kNotConfigured, Phase::kCancelled,
                            Phase::kExpired, Phase::kInstallFailed, Phase::kCommitUncertain, Phase::kResourceFault}) {
        state.firmware.update.phase = phase;
        state.firmware.update.commit_started = phase == Phase::kCommitUncertain;
        state.firmware.update.boot_selected = false;
        state.firmware.can_reboot = false;
        view.Update(state); Tick();
        const auto count = requests.size();
        Click(view, WATCH_UPDATE_INSTALL); Click(view, WATCH_UPDATE_RESTART);
        Check(requests.size() == count, "error and no-update states cannot install or reboot");
        Screenshot(view.Root(), screenshots / ("firmware-state-" + std::to_string(static_cast<int>(phase)) + ".ppm"));
    }
    state.firmware.update = {};
    state.firmware.can_check = true;
    view.Update(state); view.Navigate(WatchPage::kFirmwareUpdate); Tick();
    Click(view, WATCH_UPDATE_CHECK); check = requests.back();
    view.SetVisible(false);
    Check(requests.back().action == WatchViewAction::kCancelFirmware && requests.back().firmware_request == check.id,
        "hiding updates cancels a queued check without waiting for a runtime snapshot");
    view.Complete(requests.back().id, true);
    state.firmware.update.phase = Phase::kCancelled;
    view.Update(state); view.SetVisible(true); Tick();
    state.firmware.update = {};
    view.Update(state); view.Navigate(WatchPage::kFirmwareUpdate); Tick();
    auto* target = lv_obj_get_child(lv_obj_get_parent(FindLabel(view.Root(), WATCH_UPDATE_TARGET)), 1);
    Check(lv_obj_check_type(target, &lv_dropdown_class), "update target selector exists");
    const auto choose = [&](uint32_t index) {
        lv_dropdown_set_selected(target, index);
        lv_obj_send_event(target, LV_EVENT_VALUE_CHANGED, nullptr);
        Tick();
    };
    const auto before_select = requests.size();
    choose(1);
    Check(requests.size() == before_select + 1 && requests.back().action == WatchViewAction::kSelectUpdateTarget &&
        requests.back().update_target == watch_update::Target::kModels,
        "model selection sends a typed owner request without downloading");
    Check(lv_dropdown_get_selected(target) == 0 && lv_obj_has_state(target, LV_STATE_DISABLED),
        "target selection waits for owner acknowledgement");
    view.Complete(requests.back().id, false); Tick();
    Check(lv_dropdown_get_selected(target) == 0 && !lv_obj_has_state(target, LV_STATE_DISABLED),
        "failed target selection restores the actual component");
    choose(1);
    state.firmware.update.target = watch_update::Target::kModels;
    state.firmware.current_version.clear(); state.firmware.current_builtin = true;
    view.Update(state); view.Complete(requests.back().id, true); Tick();
    Check(lv_dropdown_get_selected(target) == 1 && FindLabel(view.Root(), WATCH_UPDATE_BUILT_IN),
        "model target shows its own installed identity");
    lv_dropdown_open(target); Tick();
    Check(lv_dropdown_is_open(target) && !lv_obj_has_flag(lv_dropdown_get_list(target), LV_OBJ_FLAG_HIDDEN),
        "both update targets are exposed in an open selector");
    Screenshot(lv_screen_active(), screenshots / "models-target-options.ppm");
    lv_dropdown_close(target); Tick();
    Screenshot(view.Root(), screenshots / "models-idle.ppm");
    Click(view, WATCH_UPDATE_CHECK); check = requests.back();
    Check(check.update_target == watch_update::Target::kModels, "model check preserves the selected target");
    const auto while_checking = requests.size();
    choose(0);
    Check(requests.size() == while_checking && lv_dropdown_get_selected(target) == 1,
        "queued check prevents target switching");
    state.firmware.request = check.id;
    state.firmware.update.attempt = 4; state.firmware.update.sequence = 20;
    state.firmware.update.version = "models-3"; state.firmware.update.bytes = 2000000;
    state.firmware.update.phase = Phase::kAvailable;
    state.firmware.can_check = false; state.firmware.can_install = true;
    view.Update(state); view.Complete(check.id, true); Tick();
    Click(view, WATCH_UPDATE_REVIEW);
    Check(FindLabel(view.Root(), WATCH_UPDATE_MODELS) && !FindLabel(view.Root(), WATCH_UPDATE_TARGET),
        "confirmation names the reviewed component without a target selector");
    const auto before_cross_target = requests.size();
    state.firmware.update.target = watch_update::Target::kFirmware;
    view.Update(state); Tick(); Click(view, WATCH_UPDATE_INSTALL);
    Check(requests.size() == before_cross_target && FindLabel(view.Root(), WATCH_UPDATE_EXPIRED),
        "same version and sequence on another target cannot satisfy model confirmation");
    state.firmware.update.target = watch_update::Target::kModels;
    view.Update(state); Tick();
    auto* models_body = lv_obj_get_parent(FindButton(view.Root(), WATCH_UPDATE_INSTALL));
    lv_obj_scroll_to_y(models_body, 0, LV_ANIM_OFF); Tick();
    Screenshot(view.Root(), screenshots / "models-confirm-top.ppm");
    lv_obj_scroll_to_y(models_body, 100000, LV_ANIM_OFF); Tick();
    lv_obj_get_coords(FindButton(view.Root(), WATCH_UPDATE_CANCEL), &cancel_bounds);
    Check(cancel_bounds.y1 >= 64 && cancel_bounds.y2 < 502, "model confirmation bottom action is reachable");
    Screenshot(view.Root(), screenshots / "models-confirm-bottom.ppm");
    Click(view, WATCH_UPDATE_INSTALL); install = requests.back();
    Check(install.update_target == watch_update::Target::kModels && install.firmware_request == check.id &&
        install.firmware_sequence == 20 && install.firmware_attempt == 4, "model install binds target and reviewed release");
    state.firmware.update.phase = Phase::kInstalling; state.firmware.update.active = true;
    state.firmware.can_install = false;
    view.Update(state); view.Complete(install.id, true); Tick();
    Click(view, WATCH_UPDATE_CANCEL);
    Check(requests.back().update_target == watch_update::Target::kModels && requests.back().firmware_request == check.id,
        "model cancellation preserves target and root request");
    view.Complete(requests.back().id, true);
    state.firmware.update.active = false; state.firmware.update.commit_started = true;
    state.firmware.update.boot_selected = true; state.firmware.update.phase = Phase::kRebootRequired;
    state.firmware.can_reboot = true;
    view.Update(state); Tick(); Click(view, WATCH_UPDATE_RESTART);
    Check(requests.back().update_target == watch_update::Target::kModels &&
        requests.back().action == WatchViewAction::kRebootFirmware, "model activation uses the scoped restart command");
    view.Complete(requests.back().id, false); Tick();
    Screenshot(view.Root(), screenshots / "models-reboot.ppm");
    std::cout << "PASS: firmware and model update UI, scoped confirmation, cancellation, progress and rendering\n";
}

int main(int argc, char** argv) {
    std::signal(SIGALRM, [](int) {
        constexpr char message[] = "FAIL: watch UI test timed out\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
        _exit(2);
    });
    alarm(30);
    Check(argc == 2, "screenshot directory supplied");
    std::filesystem::create_directories(argv[1]);
    lv_init();
    auto* display = lv_display_create(410, 502);
    std::vector<uint32_t> pixels(410 * 502);
    lv_display_set_buffers(display, pixels.data(), nullptr, pixels.size() * sizeof(uint32_t), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, [](lv_display_t* target, const lv_area_t*, uint8_t*) { lv_display_flush_ready(target); });
    Storage storage;
    WatchTimeService service(storage);
    WatchViewState state;
    state.clock = {1000, 7, 1789293600000ULL};
    state.battery_percent = 82;
    state.wifi_connected = state.ble_connected = true;
    Check(service.Initialize(state.clock), "UI time service initializes");
    state.time = service.Snapshot(state.clock);
    TestHiddenWatchTouchWake(state);
    TestPixelShift(state, argv[1]);
    TestTouchLock(state, argv[1]);
    TestTouchLockPointer(state);
    TestWristSettingsUi(state, argv[1]);
    std::vector<WatchViewRequest> requests;
    {
        WatchView view(lv_screen_active(), &font_puhui_30_4, &lv_font_montserrat_48,
            &font_awesome_30_4, [&](const WatchViewRequest& request) { requests.push_back(request); });
        view.Update(state); Tick();
        char battery_text[80]; std::snprintf(battery_text, sizeof(battery_text), WATCH_BATTERY_FORMAT, 82);
        auto* battery_label = FindLabel(view.Root(), battery_text);
        Check(battery_label != nullptr, "battery status label rendered");
        Check(lv_color_brightness(lv_obj_get_style_text_color(battery_label, LV_PART_MAIN)) > 150,
            "body text is readable on a dark background");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "face.ppm");
        Click(view, FONT_AWESOME_WATCH);
        Check(view.Page() == WatchPage::kApps, "watch launcher opens apps");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "apps.ppm");
        Click(view, WATCH_ACTIVITY);
        Check(view.Page() == WatchPage::kActivity, "activity opens from the app launcher");
        Check(FindLabel(view.Root(), "--") && FindLabel(view.Root(), WATCH_MOTION_UNAVAILABLE),
            "uninitialized motion does not fabricate zero steps");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "activity-unavailable.ppm");
        state.motion.initialized = true;
        state.motion.activity = {WatchMotionStatus::kRunning, 20000, 8234, 0, false};
        view.Update(state); Tick();
        Check(FindLabel(view.Root(), "8234") && FindLabel(view.Root(), WATCH_MOTION_RUNNING),
            "activity renders the cached hardware-derived total");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "activity.ppm");
        state.motion.activity.steps = UINT32_MAX;
        state.motion.activity.incomplete = true;
        state.motion.activity.status = WatchMotionStatus::kPaused;
        view.Update(state); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "activity-paused.ppm");
        state.motion.activity.day.reset();
        state.motion.activity.undated_steps = UINT32_MAX;
        state.motion.storage_error = true;
        view.Update(state); Tick();
        Check(FindLabel(view.Root(), "--") && FindLabel(view.Root(), WATCH_SAVE_FAILED),
            "unsynchronized time and storage failure remain visible");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "activity-storage-error.ppm");
        Check(requests.empty(), "activity rendering performs no hardware commands or fake writes");
        Click(view, WATCH_ACTIVITY_RETRY);
        Check(requests.size() == 1 && requests.back().action == WatchViewAction::kRetryActivityStorage,
            "storage retry is queued for the actual worker");
        const auto retry = requests.back().id;
        Check(FindLabel(view.Root(), WATCH_WORKING), "queued retry does not pretend storage succeeded");
        state.motion.retry_request = retry + 1;
        state.motion.retry_succeeded = true;
        view.Update(state); Tick();
        Check(FindLabel(view.Root(), WATCH_WORKING), "other retry result cannot acknowledge pending request");
        state.motion.retry_request = retry;
        state.motion.retry_pending = true;
        state.motion.retry_succeeded = false;
        view.Update(state); Tick();
        Click(view, WATCH_ACTIVITY_RETRY);
        Check(requests.size() == 1, "pending storage retry cannot be duplicated");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "activity-retry-pending.ppm");
        state.motion.retry_pending = false;
        view.Update(state); Tick();
        Check(FindLabel(view.Root(), WATCH_ACTION_FAILED), "actual worker failure remains visible");
        Click(view, WATCH_ACTIVITY_RETRY);
        state.motion.retry_request = requests.back().id;
        state.motion.retry_succeeded = true;
        state.motion.storage_error = false;
        view.Update(state); Tick();
        Check(!FindLabel(view.Root(), WATCH_WORKING), "matching completed worker result ends pending state");
        Check(lv_obj_has_flag(lv_obj_get_parent(FindLabel(view.Root(), WATCH_ACTIVITY_RETRY)), LV_OBJ_FLAG_HIDDEN),
            "storage retry hides only after storage actually recovers");
        requests.clear();
        Click(view, WATCH_ACTIVITY_HISTORY);
        Check(view.Page() == WatchPage::kActivityHistory && FindLabel(view.Root(), WATCH_ACTIVITY_HISTORY_EMPTY),
            "empty history does not synthesize dated zero totals");
        state.motion.history.count = 7;
        state.motion.history.incomplete = true;
        for (size_t i = 0; i < 7; ++i) state.motion.history.days[i] = {2026 * 366 + int32_t(i * 2), uint32_t(i + 1)};
        state.motion.history.days[6].steps = UINT32_MAX;
        view.Update(state); Tick();
        Check(FindLabel(view.Root(), "2026-01-13") && FindLabel(view.Root(), "2026-01-01"), "all seven actual dates displayed");
        Check(!FindLabel(view.Root(), "2026-01-12"), "missing calendar dates are not invented");
        auto* latest = FindLabel(view.Root(), "2026-01-13");
        auto* oldest = FindLabel(view.Root(), "2026-01-01");
        Check(lv_obj_get_y(lv_obj_get_parent(latest)) < lv_obj_get_y(lv_obj_get_parent(oldest)), "history sorted newest first");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "activity-history-top.ppm");
        auto* history_body = lv_obj_get_parent(lv_obj_get_parent(oldest));
        lv_obj_scroll_to_view(lv_obj_get_parent(oldest), LV_ANIM_OFF); Tick();
        const auto history_scroll = lv_obj_get_scroll_y(history_body);
        Check(history_scroll > 0, "history can scroll to its seventh record");
        lv_area_t last_bounds, body_bounds;
        lv_obj_get_coords(lv_obj_get_parent(oldest), &last_bounds);
        lv_obj_get_content_coords(history_body, &body_bounds);
        Check(last_bounds.y1 >= body_bounds.y1 && last_bounds.y2 <= body_bounds.y2,
            "seventh date and step count are fully visible after scrolling");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "activity-history-bottom.ppm");
        state.motion.history.days[6].steps = 9123;
        view.Update(state); Tick();
        Check(lv_obj_get_scroll_y(history_body) == history_scroll, "step updates preserve history scroll position");
        Click(view, FONT_AWESOME_ANGLE_LEFT);
        Check(view.Page() == WatchPage::kActivity, "history back returns to activity");
        Check(requests.empty(), "history navigation has no hardware or storage side effects");
        view.Navigate(WatchPage::kApps); Tick();
        Click(view, WATCH_TIMER);
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "timer-picker.ppm");
        char preset[32]; std::snprintf(preset, sizeof(preset), WATCH_MINUTE_PRESET, 5U);
        Click(view, preset);
        Check(requests.size() == 1 && requests[0].duration_ms == 300000, "preset requests real timer duration");
        Check(!service.Snapshot(state.clock).state.checkpoint.timer_running, "UI does not fabricate successful timer state");
        std::snprintf(preset, sizeof(preset), WATCH_MINUTE_PRESET, 1U);
        Click(view, preset);
        Check(requests.size() == 1, "duplicate command blocked while pending");
        view.Complete(requests[0].id + 1, true);
        Check(FindLabel(view.Root(), WATCH_SAVING) != nullptr, "stale completion ignored");
        auto request = requests.back();
        Check(service.Execute(request.command, state.clock, request.duration_ms), "queued command persisted");
        state.time = service.Snapshot(state.clock); view.Update(state); view.Complete(request.id, true); Tick();
        Check(FindLabel(view.Root(), "00:05:00") != nullptr, "persisted timer rendered");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "timer-running.ppm");
        Click(view, WATCH_PAUSE); request = requests.back();
        storage.fail = true;
        view.Complete(request.id, service.Execute(request.command, state.clock));
        Check(FindLabel(view.Root(), WATCH_SAVE_FAILED) != nullptr, "save failure visible");
        Check(service.Snapshot(state.clock).state.checkpoint.timer_running, "failed pause not acknowledged");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "save-failed.ppm");
        storage.fail = false;
        view.Navigate(WatchPage::kStopwatch); Tick(); Click(view, WATCH_START); request = requests.back();
        Check(service.Execute(request.command, state.clock), "stopwatch request accepted");
        state.clock.rtc_ms += 65000; state.clock.unix_ms += 65000;
        state.time = service.Snapshot(state.clock); view.Update(state); view.Complete(request.id, true); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "stopwatch.ppm");
        view.Navigate(WatchPage::kAlarms); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "alarms.ppm");
        Click(view, std::string("07:00  ") + WATCH_OFF);
        auto* hour_up = lv_obj_get_parent(FindLabel(view.Root(), FONT_AWESOME_ANGLE_UP));
        lv_obj_send_event(hour_up, LV_EVENT_SHORT_CLICKED, nullptr);
        Check(FindLabel(view.Root(), "08") != nullptr, "hour stepper increments");
        state.time.state.checkpoint.timer_alert = true;
        view.Update(state); Tick();
        Check(FindLabel(view.Root(), "08") != nullptr, "incoming alert does not discard alarm draft");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "timer-alert.ppm");
        state.time.state.checkpoint.timer_alert = false;
        view.Update(state); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "alarm-editor.ppm");
        Click(view, WATCH_SAVE); request = requests.back();
        Check(request.action == WatchViewAction::kSetAlarm && request.alarm_index == 0 && request.alarm.hour == 8,
            "alarm editor dispatches edited alarm after alert redraw");
        Check(view.Page() == WatchPage::kAlarmEditor, "editor stays open pending save");
        Check(service.SetAlarm(request.alarm_index, request.alarm, state.clock), "alarm saved");
        state.time = service.Snapshot(state.clock); view.Update(state); view.Complete(request.id, true); Tick();
        Check(view.Page() == WatchPage::kAlarms, "editor closes only after save success");
        view.Navigate(WatchPage::kSettings); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "settings.ppm");
        auto* dropdown = lv_obj_get_child(lv_obj_get_parent(FindLabel(view.Root(), WATCH_TIME_ZONE)), 1);
        Check(lv_obj_check_type(dropdown, &lv_dropdown_class), "time zone selector exists");
        lv_font_glyph_dsc_t indicator{};
        Check(lv_font_get_glyph_dsc(lv_obj_get_style_text_font(dropdown, LV_PART_INDICATOR),
            &indicator, 0xf107, 0) && !indicator.is_placeholder, "dropdown indicator has a real glyph");
        lv_dropdown_open(dropdown); Tick();
        Check(!lv_obj_has_flag(lv_dropdown_get_list(dropdown), LV_OBJ_FLAG_HIDDEN), "time zone options open");
        Screenshot(lv_screen_active(), std::filesystem::path(argv[1]) / "time-zone-options.ppm");
        lv_dropdown_close(dropdown); Tick();
        lv_dropdown_set_selected(dropdown, 12);
        lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
        Click(view, WATCH_SAVE); request = requests.back();
        Check(request.action == WatchViewAction::kSetPreferences && request.timezone == "America/Los_Angeles" && request.hour24,
            "localized time zone selection preserves the canonical persisted ID");
        Check(service.SetPreferences(request.timezone, request.hour24, state.clock), "settings persist");
        view.Complete(request.id, true);
        view.SetVisible(false); Tick();
        Check(lv_obj_has_flag(view.Root(), LV_OBJ_FLAG_HIDDEN), "screen-off hides UI and pauses render timer");
        view.SetVisible(true); Tick();
        Check(!lv_obj_has_flag(view.Root(), LV_OBJ_FLAG_HIDDEN), "screen-on restores UI");

        state.controls = {75, 70, true};
        state.wifi_connected = false;
        state.time.initialized = false;
        view.Update(state); view.Navigate(WatchPage::kControls); Tick();
        std::vector<lv_obj_t*> sliders;
        CollectSliders(view.Root(), sliders);
        Check(sliders.size() == 2, "controls have dedicated brightness and volume sliders");
        Check(lv_slider_get_min_value(sliders[0]) == 10 && lv_slider_get_min_value(sliders[1]) == 0,
            "brightness cannot black out the display and volume supports mute");
        Check(lv_slider_get_value(sliders[0]) == 75 && lv_slider_get_value(sliders[1]) == 70,
            "controls reflect persisted runtime values");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "controls-ready.ppm");
        const auto before_drag = requests.size();
        lv_slider_set_value(sliders[0], 40, LV_ANIM_OFF);
        lv_obj_send_event(sliders[0], LV_EVENT_VALUE_CHANGED, nullptr);
        Check(requests.size() == before_drag, "dragging does not repeatedly persist a preference");
        lv_obj_send_event(sliders[0], LV_EVENT_RELEASED, nullptr);
        request = requests.back();
        Check(request.action == WatchViewAction::kSetLevel && request.level == WatchLevel::kBrightness && request.level_value == 40,
            "release submits brightness without requiring clock initialization");
        lv_obj_send_event(sliders[0], LV_EVENT_RELEASED, nullptr);
        Check(requests.size() == before_drag + 1, "duplicate release is blocked while saving");
        Check(state.controls.brightness == 75, "UI has not fabricated a saved brightness");
        view.Complete(request.id, false); Tick();
        Check(lv_slider_get_value(sliders[0]) == 75 && FindLabel(view.Root(), WATCH_SAVE_FAILED),
            "save failure restores the confirmed value and shows failure");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "controls-save-failed.ppm");
        lv_slider_set_value(sliders[1], 0, LV_ANIM_OFF);
        lv_obj_send_event(sliders[1], LV_EVENT_RELEASED, nullptr);
        request = requests.back();
        Check(request.level == WatchLevel::kVolume && request.level_value == 0, "mute is an explicit persisted volume");
        state.controls.volume = 0;
        view.Update(state);
        view.Complete(request.id, true); Tick();
        Check(FindLabel(view.Root(), "0%"), "acknowledged mute is displayed");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "controls-muted.ppm");
        auto* wifi_switch = lv_obj_get_child(lv_obj_get_parent(FindLabel(view.Root(), WATCH_WIFI)), 2);
        Check(lv_obj_check_type(wifi_switch, &lv_switch_class), "WiFi policy uses a switch");
        Check(FindLabel(view.Root(), WATCH_WIFI_NOT_CONNECTED), "enabled WiFi is not represented as connected");
        lv_obj_remove_state(wifi_switch, LV_STATE_CHECKED);
        lv_obj_send_event(wifi_switch, LV_EVENT_VALUE_CHANGED, nullptr);
        request = requests.back();
        Check(request.action == WatchViewAction::kSetWifiEnabled && !request.enabled,
            "WiFi disable is submitted without requiring clock initialization");
        Check(state.controls.wifi_enabled && FindLabel(view.Root(), WATCH_WIFI_PENDING), "pending stop is not a confirmed radio stop");
        state.controls.wifi_enabled = false;
        state.controls.wifi_change_failed = true;
        view.Update(state); view.Complete(request.id, false); Tick();
        Check(FindLabel(view.Root(), WATCH_WIFI_FAILED) && !FindLabel(view.Root(), WATCH_WIFI_OFF),
            "failed stop is not reported as WiFi off");
        Check(FindLabel(view.Root(), WATCH_ACTION_FAILED), "radio failure is not mislabeled as only a save failure");
        lv_obj_scroll_to_view(wifi_switch, LV_ANIM_OFF); Tick(); Tick(); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "controls-wifi-failed.ppm");
        auto* wifi_retry = lv_obj_get_child(lv_obj_get_parent(FindLabel(view.Root(), WATCH_WIFI)), 1);
        lv_obj_send_event(wifi_retry, LV_EVENT_CLICKED, nullptr);
        Check(requests.back().action == WatchViewAction::kSetWifiEnabled && !requests.back().enabled,
            "failed disable can be retried without enabling the radio");
        state.controls.wifi_change_failed = false;
        view.Update(state); view.Complete(requests.back().id, true); Tick();
        Check(FindLabel(view.Root(), WATCH_WIFI_OFF), "confirmed disabled state is displayed");
        lv_obj_add_state(wifi_switch, LV_STATE_CHECKED);
        lv_obj_send_event(wifi_switch, LV_EVENT_VALUE_CHANGED, nullptr);
        const auto wifi_enable = requests.back();
        Check(wifi_enable.enabled && !lv_obj_has_state(wifi_switch, LV_STATE_DISABLED), "pending WiFi enable is cancelable");
        lv_obj_remove_state(wifi_switch, LV_STATE_CHECKED);
        lv_obj_send_event(wifi_switch, LV_EVENT_VALUE_CHANGED, nullptr);
        const auto wifi_cancel = requests.back();
        Check(!wifi_cancel.enabled && wifi_cancel.id != wifi_enable.id, "WiFi off supersedes pending enable");
        view.Complete(wifi_enable.id, true);
        Check(FindLabel(view.Root(), WATCH_WIFI_PENDING), "stale WiFi acknowledgement does not finish cancellation");
        view.Complete(wifi_cancel.id, true); Tick(); Tick(); Tick();
        lv_obj_scroll_to_view(wifi_switch, LV_ANIM_OFF); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "controls-wifi-off.ppm");
        state.controls.wifi_enabled = true;
        state.wifi_connected = true;
        view.Update(state); Tick(); Tick(); Tick();
        Check(FindLabel(view.Root(), WATCH_WIFI_CONNECTED), "association is reported independently of policy");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "controls-wifi-connected.ppm");
        auto* voice_switch = lv_obj_get_child(lv_obj_get_parent(FindLabel(view.Root(), WATCH_ASSISTANT)), 1);
        Check(lv_obj_check_type(voice_switch, &lv_switch_class), "assistant permission uses a switch");
        Check(!lv_obj_has_state(voice_switch, LV_STATE_CHECKED), "assistant starts disabled without opt in");
        lv_obj_add_state(voice_switch, LV_STATE_CHECKED);
        lv_obj_send_event(voice_switch, LV_EVENT_VALUE_CHANGED, nullptr);
        const auto pending_enable = requests.back();
        Check(pending_enable.action == WatchViewAction::kSetVoiceEnabled && pending_enable.enabled && !state.controls.voice_enabled,
            "enabling is a pending request not a runtime permission change");
        Check(!lv_obj_has_state(voice_switch, LV_STATE_DISABLED), "pending enable remains cancelable");
        lv_obj_remove_state(voice_switch, LV_STATE_CHECKED);
        lv_obj_send_event(voice_switch, LV_EVENT_VALUE_CHANGED, nullptr);
        request = requests.back();
        Check(request.action == WatchViewAction::kSetVoiceEnabled && !request.enabled && request.id != pending_enable.id,
            "disable supersedes pending enable");
        view.Complete(pending_enable.id, true);
        Check(lv_obj_has_state(voice_switch, LV_STATE_DISABLED), "old enable acknowledgement cannot reopen the pending control");
        view.Complete(request.id, true);
        lv_obj_add_state(voice_switch, LV_STATE_CHECKED);
        lv_obj_send_event(voice_switch, LV_EVENT_VALUE_CHANGED, nullptr);
        state.controls.voice_enabled = true;
        view.Update(state);
        view.Complete(requests.back().id, true); Tick(); Tick(); Tick(); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "controls-assistant-on.ppm");
        auto* time_settings = lv_obj_get_parent(FindLabel(view.Root(), WATCH_TIME_SETTINGS));
        lv_obj_scroll_to_view(time_settings, LV_ANIM_OFF); Tick();
        lv_area_t settings_bounds{};
        lv_obj_get_coords(time_settings, &settings_bounds);
        Check(settings_bounds.y1 >= 0 && settings_bounds.y2 < 502, "all controls remain reachable after adding assistant permission");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "controls-bottom.ppm");
        state.controls.can_adjust = false;
        view.Update(state); Tick();
        Check(lv_obj_has_state(sliders[0], LV_STATE_DISABLED) && lv_obj_has_state(sliders[1], LV_STATE_DISABLED),
            "active audio ownership disables preference commands");
        const auto blocked_requests = requests.size();
        lv_obj_remove_state(wifi_switch, LV_STATE_CHECKED);
        lv_obj_send_event(wifi_switch, LV_EVENT_VALUE_CHANGED, nullptr);
        Check(requests.size() == blocked_requests && lv_obj_has_state(wifi_switch, LV_STATE_CHECKED),
            "recording or transfer ownership rejects a late WiFi toggle event");
        lv_slider_set_value(sliders[0], 50, LV_ANIM_OFF);
        lv_obj_send_event(sliders[0], LV_EVENT_RELEASED, nullptr);
        Check(requests.size() == blocked_requests, "late release cannot bypass resource blocking");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "controls-busy.ppm");
        Check(!lv_obj_has_state(voice_switch, LV_STATE_DISABLED), "busy audio cannot prevent disabling the assistant");
        lv_obj_remove_state(voice_switch, LV_STATE_CHECKED);
        lv_obj_send_event(voice_switch, LV_EVENT_VALUE_CHANGED, nullptr);
        Check(!requests.back().enabled, "busy disable reaches the runtime owner");
        state.controls.voice_enabled = false;
        state.controls.voice_save_failed = true;
        view.Update(state);
        view.Complete(requests.back().id, false); Tick(); Tick(); Tick(); Tick();
        Check(!lv_obj_has_state(voice_switch, LV_STATE_CHECKED) && FindLabel(view.Root(), WATCH_VOICE_UNSAVED),
            "failed persistence does not undo runtime microphone disable");
        lv_obj_scroll_to_view(time_settings, LV_ANIM_OFF); Tick();
        lv_area_t save_error_bounds{};
        lv_obj_get_coords(time_settings, &settings_bounds);
        lv_obj_get_coords(FindLabel(view.Root(), WATCH_SAVE_FAILED), &save_error_bounds);
        Check(settings_bounds.y2 < save_error_bounds.y1, "fixed save error does not hide the reachable final setting");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "controls-assistant-unsaved.ppm");
        state.controls = {-1, -1, true};
        view.Update(state); Tick();
        Check(FindLabel(view.Root(), WATCH_CONTROLS_UNAVAILABLE), "storage failures are not shown as default successful settings");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "controls-unavailable.ppm");
        state.time.initialized = true;

        WatchAssistantState assistant;
        state.assistant = assistant.Snapshot(AudioSessionMode::kIdle, false, false, true, true, false, false);
        view.Update(state); view.Navigate(WatchPage::kAssistant); Tick(); Tick();
        Check(FindLabel(view.Root(), WATCH_VOICE_OFF), "assistant off is distinct from connectivity failure");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "assistant-disabled.ppm");
        Click(view, std::string(FONT_AWESOME_GEAR) + "  " + WATCH_CONTROLS);
        Check(view.Page() == WatchPage::kControls, "disabled assistant links to explicit settings without enabling capture");
        const auto update_assistant = [&](AudioSessionMode mode, bool closing = false, bool pending = false,
            bool configured = true, bool connected = true) {
            state.assistant = assistant.Snapshot(mode, closing, pending, configured, connected, false);
            view.Update(state); Tick();
        };
        update_assistant(AudioSessionMode::kIdle);
        view.Navigate(WatchPage::kAssistant); Tick();
        Check(FindLabel(view.Root(), WATCH_ASSISTANT_READY), "assistant readiness comes from runtime state");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "assistant-ready.ppm");
        const std::string talk = std::string(FONT_AWESOME_MICROPHONE) + "  " + WATCH_ASSISTANT_HOLD;
        state.time.initialized = false;
        view.Update(state); Tick();
        auto* talk_button = lv_obj_get_parent(FindLabel(view.Root(), talk));
        lv_obj_send_event(talk_button, LV_EVENT_PRESSED, nullptr); request = requests.back();
        Check(request.action == WatchViewAction::kAssistantPress && request.press_token,
            "press action does not depend on clock persistence readiness");
        state.time.initialized = true;
        Check(FindLabel(view.Root(), WATCH_ASSISTANT_READY), "UI does not fabricate listening before runtime ack");
        const auto count = requests.size();
        lv_obj_send_event(talk_button, LV_EVENT_PRESSED, nullptr);
        Check(requests.size() == count, "repeated start is blocked while pending");
        update_assistant(AudioSessionMode::kAssistantConnecting);
        Check(!lv_obj_has_state(talk_button, LV_STATE_DISABLED), "pending connection preserves release events");
        view.Complete(request.id, true); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "assistant-connecting.ppm");
        update_assistant(AudioSessionMode::kAssistantListening);
        auto* send_label = FindLabel(view.Root(), std::string(FONT_AWESOME_MICROPHONE) + "  " + WATCH_ASSISTANT_RELEASE);
        auto* send_button = lv_obj_get_parent(send_label);
        Check(!lv_obj_has_state(send_button, LV_STATE_DISABLED), "listening enables send immediately");
        Check(lv_obj_get_style_opa(send_button, LV_PART_MAIN) == LV_OPA_COVER &&
            lv_obj_get_style_recolor_opa(send_button, LV_PART_MAIN) == LV_OPA_TRANSP,
            "enabled send restores full contrast without a stale disabled recolor");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "assistant-listening.ppm");
        lv_obj_send_event(talk_button, LV_EVENT_RELEASED, nullptr);
        request = requests.back();
        Check(request.action == WatchViewAction::kAssistantRelease, "release finishes the held capture");
        const auto release_count = requests.size();
        lv_obj_send_event(talk_button, LV_EVENT_CLICKED, nullptr);
        Check(requests.size() == release_count, "release does not dispatch a second click command");
        assistant.SetUserText("What is on my calendar today?");
        update_assistant(AudioSessionMode::kAssistantThinking);
        view.Complete(request.id, true); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "assistant-thinking.ppm");
        assistant.AppendAssistantText("You have a meeting at 10:00 and a review at 15:00.");
        update_assistant(AudioSessionMode::kAssistantSpeaking);
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "assistant-speaking.ppm");
        lv_obj_send_event(talk_button, LV_EVENT_PRESSED, nullptr);
        request = requests.back();
        Check(request.action == WatchViewAction::kAssistantPress, "held interrupt requests a new checked session");
        view.Navigate(WatchPage::kFace); Tick();
        Check(requests.back().action == WatchViewAction::kAssistantCancelPress && requests.back().id != request.id,
            "leaving assistant cancels even while a new turn is pending");
        view.Complete(request.id, true);
        view.Complete(requests.back().id, true);
        view.Navigate(WatchPage::kAssistant); Tick();
        update_assistant(AudioSessionMode::kTransitioning, true);
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "assistant-stopping.ppm");
        assistant.Fail(WatchAssistantError::kAudio);
        update_assistant(AudioSessionMode::kIdle);
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "assistant-audio-failed.ppm");
        update_assistant(AudioSessionMode::kLocalRecording);
        auto* primary = lv_obj_get_parent(FindLabel(view.Root(), talk));
        Check(lv_obj_has_state(primary, LV_STATE_DISABLED), "recording disables assistant microphone");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "assistant-busy.ppm");
        update_assistant(AudioSessionMode::kIdle, false, false, true, false);
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "assistant-offline.ppm");
        state.assistant.can_connect = true;
        view.Update(state); Tick();
        const auto before_connect = requests.size();
        const std::string connect = std::string(FONT_AWESOME_WIFI) + "  " + WATCH_ASSISTANT_CONNECT;
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "assistant-connect.ppm");
        auto* connect_button = lv_obj_get_parent(FindLabel(view.Root(), connect));
        lv_obj_send_event(connect_button, LV_EVENT_PRESSED, nullptr);
        lv_obj_send_event(connect_button, LV_EVENT_RELEASED, nullptr);
        Click(view, connect);
        Check(requests.size() == before_connect + 1 &&
            requests.back().action == WatchViewAction::kPrepareAssistantNetwork,
            "offline connect never sends a microphone press or release");
        view.Complete(requests.back().id, true);
        state.assistant.can_connect = false;
        state.assistant.phase = WatchAssistantPhase::kConnecting;
        state.assistant.can_end = true;
        view.Update(state); Tick();
        Check(lv_obj_has_state(connect_button, LV_STATE_DISABLED), "connecting cannot start microphone capture");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "assistant-network-preparing.ppm");
        view.Navigate(WatchPage::kFace); Tick();
        Check(requests.back().action == WatchViewAction::kAssistantEnd, "leaving cancels network preparation");
        view.Complete(requests.back().id, true);
        state.assistant.can_end = false;
        view.Update(state); view.Navigate(WatchPage::kAssistant); Tick();
        update_assistant(AudioSessionMode::kIdle, false, false, false, true);
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "assistant-setup.ppm");
        assistant.Clear();
        for (int i = 0; i < 8; ++i) {
            assistant.BeginTurn();
            assistant.SetUserText(std::string(480, 'A'));
            assistant.AppendAssistantText(std::string(480, 'B'));
        }
        update_assistant(AudioSessionMode::kIdle);
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "assistant-long-history.ppm");
        auto* history = lv_obj_get_parent(FindLabel(view.Root(), std::string(480, 'B')));
        lv_obj_scroll_to_y(history, 0, LV_ANIM_OFF); Tick();
        assistant.AppendAssistantText("A new sentence while reviewing earlier messages.");
        update_assistant(AudioSessionMode::kIdle);
        Check(lv_obj_get_scroll_y(history) == 0, "new replies do not pull a reader away from earlier messages");
        auto* end_label = FindLabel(view.Root(), std::string(FONT_AWESOME_STOP) + "  " + WATCH_ASSISTANT_END);
        lv_area_t control_bounds{};
        lv_obj_get_coords(lv_obj_get_parent(end_label), &control_bounds);
        Check(control_bounds.y1 >= 0 && control_bounds.y2 < 502, "session controls remain visible with long history");
        Click(view, FONT_AWESOME_TRASH); request = requests.back();
        Check(request.action == WatchViewAction::kAssistantClear, "clear only requests volatile history deletion");
        assistant.Clear(); update_assistant(AudioSessionMode::kIdle);
        view.Complete(request.id, true); Tick();
        Check(FindLabel(view.Root(), WATCH_ASSISTANT_EMPTY), "cleared transcript reflects acknowledged runtime state");

        talk_button = lv_obj_get_parent(FindLabel(view.Root(), talk));
        lv_obj_send_event(talk_button, LV_EVENT_PRESSED, nullptr);
        const auto delayed_press = requests.back();
        update_assistant(AudioSessionMode::kAssistantConnecting);
        lv_obj_send_event(talk_button, LV_EVENT_RELEASED, nullptr);
        const auto early_release = requests.back();
        Check(early_release.action == WatchViewAction::kAssistantRelease &&
            early_release.press_token == delayed_press.press_token, "release supersedes a pending connection request");
        view.Complete(delayed_press.id, true);
        Check(lv_obj_has_state(talk_button, LV_STATE_DISABLED), "obsolete press completion cannot reenable a pending release");
        update_assistant(AudioSessionMode::kIdle);
        view.Complete(early_release.id, true); Tick();
        lv_obj_send_event(talk_button, LV_EVENT_PRESSED, nullptr);
        lv_obj_send_event(talk_button, LV_EVENT_PRESS_LOST, nullptr);
        Check(requests.back().action == WatchViewAction::kAssistantCancelPress, "dragging outside cancels a press");
        view.Complete(requests.back().id, true); Tick();
        lv_obj_send_event(talk_button, LV_EVENT_PRESSED, nullptr);
        view.SetVisible(false);
        Check(requests.back().action == WatchViewAction::kAssistantCancelPress, "hiding the screen cancels a held gesture");
        view.Complete(requests.back().id, true);
        view.SetVisible(true); Tick();
        update_assistant(AudioSessionMode::kAssistantListening);
        talk_button = lv_obj_get_parent(FindLabel(view.Root(), std::string(FONT_AWESOME_ARROW_UP) + "  " + WATCH_ASSISTANT_SEND));
        lv_obj_send_event(talk_button, LV_EVENT_PRESSED, nullptr);
        lv_obj_send_event(talk_button, LV_EVENT_RELEASED, nullptr);
        Click(view, std::string(FONT_AWESOME_ARROW_UP) + "  " + WATCH_ASSISTANT_SEND);
        Check(requests.back().action == WatchViewAction::kAssistantSend, "non-gesture listening retains explicit send");
        view.Complete(requests.back().id, true);
        update_assistant(AudioSessionMode::kIdle);

        talk_button = lv_obj_get_parent(FindLabel(view.Root(), talk));
        lv_obj_send_event(talk_button, LV_EVENT_PRESSED, nullptr);
        const auto rebuilding_press = requests.back();
        state.time.state.checkpoint.timer_alert = !state.time.state.checkpoint.timer_alert;
        view.Update(state); Tick();
        Check(requests.back().action == WatchViewAction::kAssistantCancelPress &&
            requests.back().press_token == rebuilding_press.press_token,
            "rebuilding a pressed widget cancels its token before deleting events");
        view.Complete(requests.back().id, true);

        state.recorder.storage_mounted = true;
        view.Update(state); view.Navigate(WatchPage::kRecorder); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recorder-ready.ppm");
        const std::string record_start = std::string(FONT_AWESOME_MICROPHONE) + "  " + WATCH_START;
        const std::string record_stop = std::string(FONT_AWESOME_STOP) + "  " + WATCH_RECORDER_STOP;
        const std::string record_cancel = std::string(FONT_AWESOME_STOP) + "  " + WATCH_RECORDER_CANCEL;
        const std::string record_pause = std::string(FONT_AWESOME_PAUSE) + "  " + WATCH_PAUSE;
        const std::string record_resume = std::string(FONT_AWESOME_PLAY) + "  " + WATCH_RESUME;
        const std::string record_mark = std::string(FONT_AWESOME_LOCATION_DOT) + "  " + WATCH_RECORDER_MARK;
        state.time.initialized = false;
        view.Update(state); Tick();
        Click(view, record_start); request = requests.back();
        Check(request.action == WatchViewAction::kRecordingCommand && request.recording_command == WatchRecordingCommand::kStart,
            "recording starts without clock initialization");
        Check(FindLabel(view.Root(), WATCH_RECORDER_READY), "queued start does not fake active capture");
        const auto start_id = request.id;
        const size_t start_count = requests.size();
        Click(view, record_start);
        Check(requests.size() == start_count, "duplicate recording start is suppressed");
        Click(view, record_stop); request = requests.back();
        Check(request.recording_command == WatchRecordingCommand::kStop && request.id != start_id,
            "stop can cancel an enqueued recording start");
        const size_t stop_count = requests.size();
        Click(view, record_stop);
        Check(requests.size() == stop_count, "duplicate pending stop is suppressed");
        view.Complete(start_id, true); Tick();
        Check(FindLabel(view.Root(), WATCH_WORKING), "stale start acknowledgement cannot clear pending stop");
        view.Complete(request.id, true);
        state.time.initialized = true;
        state.recorder.control.starting = true;
        view.Update(state); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recorder-starting.ppm");
        Click(view, record_cancel); request = requests.back();
        Check(request.recording_command == WatchRecordingCommand::kStop, "pending board retries can be canceled");
        state.recorder.control.starting = false;
        state.recorder.active = true;
        state.recorder.duration_ms = 3723000;
        view.Update(state); view.Complete(request.id, true); Tick();
        Check(FindLabel(view.Root(), "01:02:03"), "recorder displays captured duration");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recorder-active.ppm");
        Click(view, record_mark); request = requests.back();
        Check(request.recording_command == WatchRecordingCommand::kHighlight, "marker action uses recording control");
        view.Complete(request.id, true);
        Click(view, record_pause); request = requests.back();
        Check(request.recording_command == WatchRecordingCommand::kPause, "pause action is dispatched");
        state.recorder.paused = true;
        view.Update(state); view.Complete(request.id, true); Tick();
        Check(lv_obj_has_state(lv_obj_get_parent(FindLabel(view.Root(), record_mark)), LV_STATE_DISABLED),
            "paused recording cannot add a marker");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recorder-paused.ppm");
        Click(view, record_resume); request = requests.back();
        Check(request.recording_command == WatchRecordingCommand::kResume, "resume action is dispatched");
        view.Complete(request.id, true);
        const size_t before_back = requests.size();
        view.Navigate(WatchPage::kFace); Tick();
        Check(requests.size() == before_back, "leaving recorder does not stop local recording");
        view.Navigate(WatchPage::kRecorder); Tick();
        state.recorder.stopping = state.recorder.transitioning = state.recorder.resources_blocked = true;
        state.recorder.control.error = WatchRecordingError::kSaveFailed;
        view.Update(state); Tick();
        Check(!lv_obj_has_state(lv_obj_get_parent(FindLabel(view.Root(), record_stop)), LV_STATE_DISABLED),
            "failed drain keeps stop available for retry");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recorder-stop-failed.ppm");
        Click(view, record_stop); request = requests.back();
        view.Complete(request.id, false); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recorder-command-failed.ppm");
        auto* stop_label = FindLabel(view.Root(), record_stop);
        lv_obj_get_coords(lv_obj_get_parent(stop_label), &control_bounds);
        Check(control_bounds.y1 >= 0 && control_bounds.y2 < 502, "recorder stop remains visible with an error");
        state.recorder = {};
        state.recorder.storage_mounted = true;
        state.recorder.last_saved = true;
        state.recorder.last_file = "Audream-20260913-120000.opus";
        state.recorder.duration_ms = 3723000;
        view.Update(state); view.Navigate(WatchPage::kRecorder); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recorder-saved.ppm");
        state.recorder.storage_mounted = false;
        view.Update(state); Tick();
        Check(lv_obj_has_state(lv_obj_get_parent(FindLabel(view.Root(), record_start)), LV_STATE_DISABLED),
            "missing storage disables screen recording start");
        const size_t unavailable_count = requests.size();
        Click(view, record_start);
        Check(requests.size() == unavailable_count, "disabled storage action cannot enqueue a start");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recorder-no-storage.ppm");
        state.recorder.storage_mounted = true;
        state.recorder.control.error = WatchRecordingError::kStartFailed;
        view.Update(state); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recorder-start-failed.ppm");

        state.time.initialized = false;
        view.Update(state); view.Navigate(WatchPage::kRecordings); Tick();
        request = requests.back();
        Check(request.action == WatchViewAction::kLoadRecordings, "opening recordings requests actual storage without clock initialization");
        state.library.status = WatchLibraryStatus::kLoading;
        ++state.library.revision;
        view.Update(state); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recordings-loading.ppm");
        const size_t loading_count = requests.size();
        Click(view, WATCH_LIBRARY_REFRESH);
        Check(requests.size() == loading_count, "loading suppresses duplicate scans");
        state.library.status = WatchLibraryStatus::kReady;
        ++state.library.revision;
        view.Update(state); view.Complete(request.id, true); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recordings-empty.ppm");
        const auto newer_label = std::string(FONT_AWESOME_ANGLE_LEFT) + " " + WATCH_LIBRARY_NEWER;
        const auto older_label = std::string(WATCH_LIBRARY_OLDER) + " " + FONT_AWESOME_ANGLE_RIGHT;
        Check(lv_obj_has_state(lv_obj_get_parent(FindLabel(view.Root(), older_label)), LV_STATE_DISABLED),
            "empty library disables older page navigation");
        const size_t empty_count = requests.size();
        Click(view, older_label); Click(view, newer_label);
        Check(requests.size() == empty_count, "unavailable pages do not enqueue scans");
        for (unsigned index = 0; index < WatchRecordingLibrary::kMaxEntries; ++index) {
            WatchRecordingEntry entry;
            entry.id = "record-" + std::to_string(index);
            entry.file_name = entry.id + ".opus";
            entry.size_bytes = 250000;
            entry.duration_ms = 123000;
            entry.metadata_available = index != 1;
            state.library.entries.push_back(entry);
        }
        ++state.library.revision;
        state.library.has_older = true;
        state.time.initialized = true;
        view.Update(state); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recordings-ready.ppm");
        auto* list_body = lv_obj_get_parent(lv_obj_get_parent(FindLabel(view.Root(), WATCH_LIBRARY_REFRESH)));
        lv_obj_scroll_to_y(list_body, 210, LV_ANIM_OFF); Tick();
        const int list_scroll = lv_obj_get_scroll_y(list_body);
        const size_t before_detail = requests.size();
        Click(view, "record-0.opus");
        Check(view.Page() == WatchPage::kRecordingDetail && requests.size() == before_detail,
            "opening cached detail does not begin playback or another scan");
        Check(FindLabel(view.Root(), WATCH_LIBRARY_SYNC_UNCONFIRMED), "local file never implies confirmed phone sync");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recording-detail.ppm");
        Click(view, FONT_AWESOME_ANGLE_LEFT);
        Check(view.Page() == WatchPage::kRecordings && requests.size() == before_detail, "detail back reuses the same index");
        list_body = lv_obj_get_parent(lv_obj_get_parent(FindLabel(view.Root(), WATCH_LIBRARY_REFRESH)));
        Check(lv_obj_get_scroll_y(list_body) == list_scroll, "detail back preserves list scroll position");
        Click(view, "record-1.opus");
        Check(FindLabel(view.Root(), WATCH_LIBRARY_METADATA_UNAVAILABLE), "damaged metadata has an explicit unavailable state");
        Check(!FindLabel(view.Root(), WATCH_RECORDER_SAVED), "damaged metadata never displays committed recording success");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recording-metadata-missing.ppm");
        Click(view, FONT_AWESOME_ANGLE_LEFT);
        list_body = lv_obj_get_parent(lv_obj_get_parent(FindLabel(view.Root(), WATCH_LIBRARY_REFRESH)));
        lv_obj_scroll_to_y(list_body, 100000, LV_ANIM_OFF); Tick();
        auto* last_label = FindLabel(view.Root(), state.library.entries.back().file_name);
        Check(last_label != nullptr, "last recent recording exists");
        lv_obj_get_coords(last_label, &control_bounds);
        Check(control_bounds.y1 >= 64 && control_bounds.y2 < 502, "last recent recording is reachable by scrolling");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recordings-bottom.ppm");
        auto* older_button = lv_obj_get_parent(FindLabel(view.Root(), older_label));
        lv_obj_get_coords(older_button, &control_bounds);
        Check(control_bounds.y1 >= 64 && control_bounds.y2 < 502, "page navigation stays visible at the bottom of the list");
        const auto newest_entries = state.library.entries;
        const auto newest_revision = state.library.revision;
        Click(view, older_label); request = requests.back();
        Check(request.action == WatchViewAction::kLoadOlderRecordings && request.library_revision == newest_revision,
            "older scan carries the displayed library revision");
        const size_t paging_count = requests.size();
        Click(view, older_label); Click(view, newer_label); Click(view, "record-0.opus");
        Check(requests.size() == paging_count && view.Page() == WatchPage::kRecordings,
            "pending pagination blocks duplicate scans and stale detail navigation");
        state.library.entries = {newest_entries.back()};
        state.library.entries[0].id = "older-record";
        state.library.entries[0].file_name = "older-record.opus";
        state.library.has_older = false; state.library.has_newer = true;
        ++state.library.revision;
        view.Update(state); view.Complete(request.id, true); Tick();
        list_body = lv_obj_get_parent(lv_obj_get_parent(FindLabel(view.Root(), WATCH_LIBRARY_REFRESH)));
        Check(lv_obj_get_scroll_y(list_body) == 0 && FindLabel(view.Root(), "older-record.opus"),
            "new page starts at the top and shows its actual entries");
        lv_obj_update_layout(view.Root());
        lv_obj_get_coords(lv_obj_get_parent(FindLabel(view.Root(), newer_label)), &control_bounds);
        Check(control_bounds.y1 >= 64 && control_bounds.y2 < 502,
            "short pages keep their return navigation inside the viewport");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recordings-older.ppm");
        Click(view, "older-record.opus"); Click(view, FONT_AWESOME_ANGLE_LEFT);
        Check(FindLabel(view.Root(), "older-record.opus") && requests.size() == paging_count,
            "detail back preserves the older page without refreshing newest recordings");
        state.library.entries.clear(); ++state.library.revision;
        view.Update(state); Tick();
        Check(FindLabel(view.Root(), WATCH_LIBRARY_PAGE_EMPTY) && !FindLabel(view.Root(), WATCH_LIBRARY_EMPTY),
            "an empty page with a return cursor is not an empty library");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recordings-page-empty.ppm");
        Click(view, newer_label); request = requests.back();
        Check(request.action == WatchViewAction::kLoadNewerRecordings && request.library_revision == state.library.revision,
            "newer scan carries the current page revision");
        state.library.entries = newest_entries;
        state.library.has_older = true; state.library.has_newer = false;
        ++state.library.revision;
        view.Update(state); view.Complete(request.id, true); Tick();
        Check(FindLabel(view.Root(), "record-0.opus"), "newer completion restores the returned page");
        Click(view, WATCH_LIBRARY_REFRESH); request = requests.back();
        Check(request.action == WatchViewAction::kLoadRecordings, "refresh requests a new index");
        Click(view, "record-0.opus");
        Check(view.Page() == WatchPage::kRecordings, "stale rows cannot open while a replacement scan is pending");
        const auto load_id = request.id;
        state.library.status = WatchLibraryStatus::kLoading; ++state.library.revision;
        view.Update(state); Tick();
        view.Navigate(WatchPage::kFace); Tick(); request = requests.back();
        Check(request.action == WatchViewAction::kCancelRecordings, "leaving recordings cancels the directory scan");
        view.Complete(load_id, true);
        Check(FindLabel(view.Root(), WATCH_WORKING), "old scan completion cannot clear a later cancellation");
        state.library.status = WatchLibraryStatus::kInterrupted; ++state.library.revision;
        view.Update(state); view.Complete(request.id, true); Tick();
        view.Navigate(WatchPage::kRecordings); Tick(); request = requests.back();
        state.library.status = WatchLibraryStatus::kUnavailable; ++state.library.revision;
        view.Update(state); view.Complete(request.id, false); Tick();
        Check(!FindLabel(view.Root(), WATCH_LIBRARY_EMPTY), "failed storage read is not an empty library");
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recordings-unavailable.ppm");
        state.library.status = WatchLibraryStatus::kInterrupted; ++state.library.revision;
        view.Update(state); Tick();
        Screenshot(view.Root(), std::filesystem::path(argv[1]) / "recordings-interrupted.ppm");
        for (const auto& page_label : {older_label, newer_label}) {
            state.library.status = WatchLibraryStatus::kReady;
            state.library.has_older = state.library.has_newer = true;
            ++state.library.revision; view.Update(state); Tick();
            Click(view, page_label); const auto pending_page = requests.back().id;
            view.Navigate(WatchPage::kFace); Tick(); request = requests.back();
            Check(request.action == WatchViewAction::kCancelRecordings,
                "leaving either page direction cancels the pending scan");
            view.Complete(pending_page, true);
            Check(FindLabel(view.Root(), WATCH_WORKING), "obsolete page completion cannot clear cancellation");
            state.library.status = WatchLibraryStatus::kInterrupted; ++state.library.revision;
            view.Update(state); view.Complete(request.id, true); Tick();
            view.Navigate(WatchPage::kRecordings); Tick(); request = requests.back();
            state.library.status = WatchLibraryStatus::kReady; ++state.library.revision;
            view.Update(state); view.Complete(request.id, true); Tick();
        }
    }
    TestRecordingDeleteUi(state, argv[1]);
    TestAlertSoundUi(state, argv[1]);
    TestPowerModeUi(state, argv[1]);
    TestFirmwareUi(state, argv[1]);
    TestDevicePairingUi(state, argv[1]);
    TestNotificationUi(state, argv[1]);
    TestAndroidNotificationUi(state, argv[1]);
    TestPlaybackUi(state, argv[1]);
    TestOrphanCleanupDiskUi(state, argv[1]);
    TestMotionWorkerUi(state, argv[1]);
    TestMotionWorkerUi(state, argv[1], true);
    lv_display_delete(display);
    lv_deinit();
    std::cout << "PASS: watch LVGL navigation, persistence results, and rendering\n";
}
