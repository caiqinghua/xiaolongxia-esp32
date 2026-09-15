# Watch Phone Notifications

## Current Implementation Boundary

`WatchAncsSession` is the bounded, transport-independent iOS ANCS protocol
engine. It is included only in the combined Note Watch build and host runtime
tests. `WatchAncsClient` now owns it and binds it to the existing Bluedroid host
under `CONFIG_AUDREAM_WATCH_ANCS`. Application now loads an opt-in preference and
connects the watch settings, pairing, list and detail pages to this client.
Collection defaults off. This is not a claim that native phone notifications
have passed real-device acceptance.

The remaining feature includes hardware validation of the native client, Android
notification access and authenticated forwarding, native disconnection/revocation handling, and
real phone/watch acceptance. Existing BLUFI custom requests must not be used as
an unauthenticated substitute for private notification delivery.

The Android app now contains the system listener and bounded in-memory relay
described in [the Android forwarding implementation](../../../android/docs/watch-phone-notifications.md).
Its manifest component is disabled by default. It does not yet have a secure
Android BLE adapter or user consent settings, so Android notifications
are not delivered to this watch. The bounded `WatchAndroidNotifications` protocol
receiver and Android AWN1 encoder now have a cross-language round-trip test; see
[the wire contract](../../../android/docs/watch-notification-wire.md). Its local
generation input is not an authentication implementation. The experimental
[native Android GATT service](watch-android-gatt.md) now shares the existing
peer pairing logic and now verifies API-signed owner grants in the application
task with a pinned RSA public key and protected binding generation. It remains
disabled by default. Bluetooth pairing alone cannot open the content gate. The ANCS implementation is not used to pretend
Android is an iPhone notification provider.

The original recording-only and StickS3 profiles remain unchanged. The engine
does not control the radio, wake the display, produce sounds, access a filesystem,
write logs, or keep the watch awake. Existing recording and power policy still
owns Bluetooth suspension. It does not promise missed-notification recovery
while Bluetooth is disabled or the watch is in deep sleep.

## Watch Controls and Privacy

The Apps and Controls pages expose Notifications. Unsupported builds display an
unavailable state instead of a working-looking toggle. `audream_watch/notify_enabled`
is a separate NVS boolean, defaulting to false on absence; invalid values and
read errors fail closed. Write and commit errors are visible and retryable.
No phone notification content is persisted in NVS.

An opt-out callback synchronously fences collection and clears the client's
private in-memory state without issuing Bluetooth or filesystem I/O under the
display lock. The Application owner/Poll then retires the physical connection;
the owner stops before saving the false preference. A failed save does not
resume collection this boot, but an older persisted opt-in can survive restart,
so the UI explicitly asks the user to retry before restarting. A newer toggle
request prevents an older queued request from granting permission. Opt-in is
applied only after a successful save and a current-request check.

Peer consent and numeric comparison use distinct current confirmation tokens.
The six-digit code preserves leading zeros. A pairing-request button makes a
pending prompt discoverable without navigating away from recording/assistant
automatically. A stale or expired confirmation is rejected, not reported as a
successful new connection. Closing the setting is available while enabling or
confirming is pending.

Device ownership pairing has a separate purpose and confirmation API in the
shared security client; see [device pairing](watch-device-pairing.md). It can run
with notification collection disabled. Its prompt is never offered by this
notification page or banner, and its authenticated peer lease cannot authorize
ANCS or Android notification content. Turning notifications off still clears
content without canceling an independently opened device-pairing window.

The list has bounded two-line titles, local read styling and a clear-watch-list
command; opening retrieves an app display name when permitted by the negotiated
MTU. Full title/body are scrollable in details. Open/read/clear operations carry
the session, UID and revision as appropriate. A revised or removed entry cannot
silently replace an already-open detail; the user must open it again. Snapshot
updates synchronously remove old LVGL content even while the display is hidden,
without waking it. Notification labels have inline recoloring disabled and no
link/action execution. Phone-local ANCS dates are formatted for readability,
not converted to UTC or interpreted as the watch's timezone.

There is no automatic notification sound, vibration, wake or cloud forwarding.
This UI does not authorize Android delivery or change the existing recording
Bluetooth shutdown policy. Application/native bindings still require firmware
and real-phone acceptance; desktop UI tests inject client snapshots.

## Native Bluedroid Binding

The new client starts only after the existing host has started. It registers a
separate GATT client application, not another host, controller, scanner or
advertiser. BLUFI retains advertising and provisioning events. A gated GAP
wrapper consumes security events so an example callback cannot automatically
approve numeric comparison or log passkeys/keys. Non-watch builds keep the old
callback, and NimBLE is not silently switched to Bluedroid.

The compile gate requires the combined watch profile, BLUFI, Bluedroid, GATTC
and SMP, and excludes the BLUFI example's own SMP characteristic policy. The
existing BLUFI control characteristic permissions are not changed by this work.
This does not solve authenticated Android forwarding or manufacturing ownership.

The client performs the following operations once its runtime permission is
enabled and a phone connects:

1. Ask for watch-side peer consent before opening a virtual GATT connection.
   Each connection requires consent; an old UI confirmation token cannot approve
   a later connection. No notification data is collected at this stage.
2. Request an MTU exchange, then request Secure Connections, MITM protection and
   bonding. Numeric comparison waits for explicit `Confirm` input. Reject
   unsupported passkey-entry/OOB flows and failed or weaker authentication.
3. Discover GATT Service Changed and ANCS. Subscribe to Service Changed first,
   Data Source second and Notification Source last, with authenticated descriptor
   writes and checked completion. Missing ANCS retains only Service Changed;
   missing required detail characteristics reports unavailable, not ready.
4. Buffer at most twenty eight-byte early source events while waiting for the
   final subscription acknowledgement. Overflow or failed authorization retires
   the connection without publishing these events. Only a successful final
   acknowledgement authorizes the engine and releases the buffered events.
5. Send Control Point requests with response and MITM authentication. Refuse
   app-name commands exceeding the negotiated ATT payload rather than allowing
   truncation at a small MTU. The attribute-only request fits the minimum MTU.
6. On failure, opt-out, host stopping, lost authentication, bond removal or service
   change, clear private state and retire the physical link. Service changes
   refresh the native attribute cache. No new session starts while the old
   connection is retiring. Existing radio shutdown owns actual host destruction.
7. Explicitly unregister notification registrations. A registration completing
   after retirement is unregistered against its captured peer and generation;
   it cannot subscribe the new phone. Registration cleanup failure latches an
   error until a completed host shutdown, rather than pretending resources were
   released and exhausting registration slots on retries.

Discovery, consent, MTU, pairing and subscription phases each have a 60-second
deadline. Both incoming callbacks and `Poll` enforce it, so a late callback
cannot reset an expired phase when Application's periodic poll was delayed.
Ready-state attribute requests retain the engine's five-second deadline.
Application's existing watch refresh polls the client; this is not a new
background timer or a new power blocker.

`SetEnabled` is only an in-memory permission operation. Its success means the
permission request was accepted, not that a phone is connected. It defaults off;
the future settings integration must load/persist an opt-in and fail closed on
storage errors. `Confirm` must be bound to the displayed current token and must
not be called automatically. A snapshot exposes pairing phase/code/token without
logging them. No UI, haptic or screen wake policy is provided by the client.

Public SDK calls and callback layouts are verified in host tests against the
installed ESP-IDF v5.5.4 headers. The actual host, callback delivery ordering,
RPA/identity address behavior, phone authorization and native GATT cache lifecycle
still require a real device/phone test. The adapter receives SDK callbacks directly;
it does not enqueue raw events and retag them with a later session.

## Wire Protocol and Bounded State

The implementation follows the [Apple ANCS specification](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/Specification/Specification.html).
Notification Source events identify additions, modifications, and removals.
Control Point requests obtain attributes through fragmented Data Source replies.
App names are queried separately and cached only within the current session.
Identifiers and content are cleared on session retirement.

Implementation limits and behavior:

- Disabled by default, with explicit authorizing, ready, disconnected, and failed
  states. A session token is a callback generation, not proof of authentication.
- Keep the latest twenty notification events. Duplicate UIDs replace their
  entry and revision. UID zero is valid. A modification invalidates old details
  and local read state, so stale text is not shown as current.
- Retrieve app identifier, title, message, and date. Title requests are capped
  at 120 bytes and message requests at 512 bytes. Local response caps are 255
  bytes for the app identifier and 15 bytes for the date. Date remains raw phone
  text; it is not assigned the watch's timezone or treated as UTC.
- One Control Point request in flight. A notification reply occupies at most
  919 bytes, including framing. Fragmentation can occur at any byte. Command,
  UID or app identifier, attribute order, length, UTF-8, and trailing bytes are
  checked before publication. Missing attributes may be empty strings.
- Normalize C0 controls in label text. App identifiers with control characters
  are rejected, not transformed into different identifiers for future commands.
  A truncated UTF-8 suffix is dropped only at a negotiated title/message limit.
  Interior invalid UTF-8 and incomplete unlimited fields fail the session.
- Data Source completion may precede the write acknowledgement. Details remain
  unpublished until both have completed successfully.
- `RequestAppName(session, uid, revision)` is an on-demand operation. Its
  immutable result is shared by retained notifications with that app identifier.
  Limit the lookup queue and cache to twenty apps, and app-name responses to
  512 bytes. Empty names are cached too, avoiding repeated empty queries.
  Notification-attribute requests take priority over app-name lookups.
- Modification, removal, eviction, or clear during a request does not cancel
  an on-wire response halfway through. Drain it, but publish only to the matching
  retained revision. Clearing cannot recreate a removed notification.
- `MarkRead` and `Clear` affect the watch's memory only. They do not mark messages
  as read on the phone, invoke notification actions, reject calls, or erase
  phone notifications. No positive/negative phone actions are implemented.

These are payload/queue bounds, not a measured ESP heap or task stack budget.
Strings, containers, an in-flight decode and snapshot copies add overhead; native
heap peaks must be measured with the list/detail UI and active radio.

## Native Adapter Contract

Before connecting this engine to the existing BLUFI host:

1. Preserve the existing host/GAP ownership. Do not initialize a second Bluetooth
   host or replace BLUFI's callback without forwarding its existing events.
   Select and test the actual configured stack, not just the SDK demo stack.
2. Require explicit watch opt-in, a verified encrypted/bonded peer and phone-side
   ANCS authorization. `Authorized()` must never be called merely because a BLE
   connection or BLUFI negotiation succeeded. Resolve pairing confirmation and
   owner selection in the UI before collecting private content.
3. Discover ANCS and handle Service Changed, service disappearance and optional
   characteristics. This engine's detail requests require both Control Point and
   Data Source. A Notification-Source-only peer must not be reported as having
   working title/body retrieval.
4. Subscribe Data Source before requesting attributes; subscribe Notification
   Source last. Early Notification Source events can arrive before the subscribe
   callback. Bound and associate any pending events with that subscription, then
   publish only after authorization/subscription success. Reject/clear them on
   failure instead of silently discarding the initial notification list.
5. Capture the session generation in native callback contexts. An old callback
   must not be relabeled using the current engine snapshot. After a timeout or
   protocol failure, retire the actual subscription/connection and drain its
   callback queue before beginning a new logical session. Token checks alone
   cannot identify an old physical reply relabeled with a new token.
6. Poll `NextRequest` with monotonic milliseconds, including while a request is
   pending. At five seconds, or on a backwards clock reading, the engine retires
   the session rather than assigning a late reply to another request. The
   platform must also bound authorization/discovery/subscription waits; the
   engine's request timeout does not cover these phases.
7. Report actual write completion, not enqueue success. Only a confirmed ANCS
   ATT error A0-A3 is `kRejected`; it promises no Data Source response. A rejected
   notification lookup removes only that matching revision; unrelated entries
   and a queued newer revision survive. A rejected app-name lookup leaves its
   notifications intact. I/O, permission, link, and ambiguous status failures
   are `kTransportFailure`, which clears private state and fails the session.
8. On disconnect, opt-out, owner change, authorization revocation, service
   disappearance, or radio suspension, clear engine state and the displayed
   snapshot. Do not persist, log, or send notification contents to the voice
   server or Note API. The UI disables LVGL text recoloring for phone text
   and must not execute links or phone actions implicitly.

The ESP-IDF v5.5.4 ANCS examples are useful references for UUIDs and native API
sequencing, not a production parser or permission policy. The existing BLUFI
custom-data handler has no authenticated owner context; Android forwarding needs
its own secure delivery integration rather than a new plaintext opcode.

## Host Verification

```sh
bash scripts/test_watch_notifications.sh
```

This compiles the actual engine using ASAN/UBSAN and warnings-as-errors. Ten
test groups cover disabled/authorizing states, UID zero, old session callbacks,
exact request bytes, every two-fragment boundary and single-byte fragments,
reply-before-write-ack, revision changes, removal/clear during a reply, bounded
retention and duplicate events, timeout and write failures, malformed frames,
field limits and UTF-8, app-name fragmentation/cache/retirement, and confirmed
request rejections without clearing unrelated notifications.

These are constructed wire fixtures, not captured iPhone traffic or a native
Bluetooth test. `scripts/test_watch_runtime.sh` includes them along with the
existing recording, playback, voice, clock, storage, motion and power tests.
Firmware builds, flashing, pairing, real phone permission revocation, radio
coexistence, power consumption and native memory use remain device acceptance
work, not conclusions from these host results.

The same script also compiles the actual `WatchAncsClient` with ESP-IDF public
headers, replacing only native I/O functions and the monotonic clock. Six
groups verify explicit consent/numeric comparison, weak-auth rejection,
MTU and subscription sequencing, early notifications, write/result failures,
service disappearance/change, request/phase timeouts, host retirement,
late registration cleanup, latched unregister failure and memory-only opt-out
before physical retirement. The BLUFI/Application
entry points are source-checked, not compiled as a complete host program in
these tests. No ESP-IDF firmware build or device installation was performed.

The existing settings/NVS test also exercises default-off, corruption, all
storage failure stages and retry for notification permission. Desktop LVGL tests
run in English and Simplified Chinese, covering absent support, pending toggles,
save failure, peer and numeric consent, UID zero, guarded details, hidden content
purge, revision replacement, disconnect, local clear, twenty rows and long
unbroken body text. Screenshots are generated through `test_watch_ui.sh`; these
are actual LVGL renders, not device screenshots or complete Application tests.
