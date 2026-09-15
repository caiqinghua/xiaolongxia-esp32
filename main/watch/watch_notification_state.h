#pragma once

#include "watch_ancs_session.h"
#include "watch_android_notifications.h"

enum class WatchAncsLinkState {
    kOffline, kDisabled, kPeerConsent, kOpening, kNegotiating, kPairing, kConfirmNumber,
    kDiscovering, kSubscribing, kReady, kUnavailable, kFailed, kAndroidAuthorizing,
    kDevicePairingReady, kControlReady
};

struct WatchAncsClientSnapshot {
    bool enabled = false;
    WatchAncsLinkState state = WatchAncsLinkState::kOffline;
    uint64_t confirmation = 0;
    uint32_t passkey = 0;
    WatchAncsSnapshot notifications;
    bool android_peer = false;
    uint64_t link_generation = 0;
    bool device_pairing_peer = false;
    bool control_pairing_peer = false;
};

enum class WatchNotificationSource { kNone, kAncs, kAndroid };

struct WatchNotificationContent {
    WatchNotificationSource source = WatchNotificationSource::kNone;
    uint64_t session = 0;
    bool ready = false;
    std::vector<WatchPhoneNotification> items;
};

struct WatchNotificationSnapshot {
    bool available = false;
    bool save_failed = false;
    WatchAncsClientSnapshot client;
    WatchNotificationContent content;
};

// Presentation never promotes the shared security client's handshake state.
// Snapshots from different physical peers cannot be combined into a ready list.
inline WatchNotificationSnapshot BuildWatchNotificationSnapshot(
    WatchAncsClientSnapshot client, WatchAndroidNotificationSnapshot android = {}) {
    WatchNotificationContent content;
    if (client.enabled && !client.control_pairing_peer && !client.device_pairing_peer && !client.android_peer && client.state == WatchAncsLinkState::kReady &&
        client.notifications.state == WatchAncsState::kReady && client.notifications.session) {
        content.source = WatchNotificationSource::kAncs;
        content.session = client.notifications.session;
        content.ready = true;
        content.items = std::move(client.notifications.notifications);
    } else if (client.enabled && !client.control_pairing_peer && !client.device_pairing_peer && client.android_peer &&
        client.state == WatchAncsLinkState::kAndroidAuthorizing && client.link_generation &&
        android.peer_lease == client.link_generation &&
        android.state == WatchAndroidNotificationState::kReady && android.session) {
        content.source = WatchNotificationSource::kAndroid;
        content.session = android.session;
        content.ready = true;
        content.items.reserve(android.entries.size());
        for (auto& entry : android.entries) {
            WatchPhoneNotification item;
            item.uid = entry.uid; item.revision = entry.revision;
            item.read = entry.read; item.details_ready = true;
            item.app_identifier = std::move(entry.package_name);
            item.app_name = std::move(entry.app_name);
            item.title = std::move(entry.title); item.message = std::move(entry.message);
            item.posted_at_ms = entry.posted_at_ms;
            content.items.push_back(std::move(item));
        }
    }
    client.notifications.notifications.clear();
    return {true, false, std::move(client), std::move(content)};
}
