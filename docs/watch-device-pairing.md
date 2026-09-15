# Watch Device Identity and Pairing

## Implemented Boundary

The firmware contains a protected, read-only factory identity loader and a
locally approved Ed25519 challenge signer. The factory tool prepares one unique
identity per device; the API still enrolls only the public key. Desktop tests
use the real firmware C++ signer and native libsodium against the actual device
registration routes with temporary storage and keys.

This is not yet a usable phone pairing flow. A build-gated
[native GATT transport](watch-pairing-gatt.md) now provides bounded payload and
result handoff. `PairingController` now composes the real signer, HTTPS verification,
protected binding and joinable worker with 29 cloud and seven offline desktop
integration scenarios.
Its physical pairing/confirmation page is implemented with desktop LVGL tests;
The build-gated `watch_pairing_application.cc` now supplies owner dispatch,
resource hooks and recording/sleep handoff. Mobile adapters and interrupted-
delivery recovery remain incomplete; no production pairing capability is
advertised. Never expose
`OpenLocalWindow` or `ApproveLocally` as remote commands to make a test pass.

The user's unified secure Add Device requirement is authoritative:
[secure-device-addition.md](secure-device-addition.md). `Open(false)` performs
cloud binding and requires already connected Wi-Fi. `Open(true)` establishes
local control without Wi-Fi, cloud identity or a synchronized clock. Both reuse
the same controller and require an initialized control-only BLE host. They must
not become separate Add Device flows. The native client supports an explicitly
approved control relationship that survives cloud windows, but mobile Add Device,
BLUFI admission and persistent bond restoration remain unconnected. Cloud binding
never grants control implicitly.

The shared native BLE security client supports an independent device-pairing
purpose, described below. It supplies authenticated physical-peer leases to the
native transport, not notification or account authorization.

Long-press recording, recording-to-Note sync, notification consent, the local
assistant permission and cloud voice enabled remain independent. Pairing alone
does not enable voice, create Notes or authorize notification collection.

## Pairing Controller Contract

`watch_pairing_controller` is the existing controller to integrate, not a reason
to create another provisioning implementation. Its owner-thread operations are
`Open`, `ConfirmPeer`, `ApproveAccount`, `Poll`, `Snapshot` and `StopWithin`.
Only `Cancel` is callable from another thread; it changes memory-only intent.
The owner must outlive callbacks and join before destroying the controller.
LVGL/GATT callbacks enqueue owner commands rather than calling these operations
while holding their own locks.

Opening reserves resources once for the complete 120-second window. Local mode
opens native control consent without a worker, NVS, signer, cloud GATT or HTTP.
Its completion is `kControlReady` with a physical generation, never `kBound`.
Cancellation before owner completion revokes even a just-authenticated local
grant; closing a completed local result does not revoke the accepted connection.

In cloud mode, the prepare
worker reads the current binding and protected factory identity; a bound device
is not silently overwritten. It requires a trusted clock and then opens the
native security/GATT window. Peer consent and numeric comparison are distinct
from approving the account UUID/challenge shown by the controller. No signature
is published before that explicit account approval.

Credential handling uses the same physical lease/window/wire request as the
approved challenge. The real HTTPS client verifies the exact device, user and
challenge before `SaveBindingAuthorized` checks the binding generation and final
GATT admission. No bootstrap, token issuance or Note creation occurs here.

Cancellation before final admission prevents the write. Cancellation or physical
disconnect after admission cannot undo NVS. A successfully committed binding is
reported as bound even if cancellation hides the worker's normal return value or
the last BLE response is lost. A failed/uncertain NVS operation remains a storage
failure, including during cancellation; credentials stay unusable until an
authoritative reload after reboot. Repeating `StopWithin` on a terminal operation
does not erase its final result. A live successful connection has at most five
seconds to read the bound status before the local window closes.

The caller's reservation is not released merely because cancellation was
requested. A blocked HTTP/NVS operation retains its captures and reservation
until the actual worker exits and is joined. `StopWithin` returning false means
resources are still held; it is not permission to power down, destroy objects or
start a conflicting hardware operation. The destructor also joins before release.

Application integration supplies these existing hooks:

- `acquire` / `release`: balanced storage, BLE control lease, network/power and
  dedicated preemptible `kDevicePairing` audio ownership for the whole window,
  released on the owner after join. A stale release cannot drop a newer
  recording/transfer generation. Real maintenance remains non-preemptible.
- `eligible`: thread-safe memory-only revocation for recording, sleep, transfer,
  changed ownership and, in cloud mode only, disabled Wi-Fi; no
  binding/display/BLE locks or I/O.
- `unix_seconds`: synchronized trusted device time, zero when unavailable.
- `prepare_tls_clock`: worker-side preparation of the verified POSIX TLS clock.

The controller does not itself open Wi-Fi, initialize BLE or invent a clock.
Application checks mode-specific prerequisites rather than starting drivers
inside resource acquisition. Recording
and sleep handoff callers must not invoke owner-only operations concurrently;
they must request cancellation and obtain confirmed owner-side cleanup before
using the resources. Mobile delivery recovery and physical manufacturing remain
separate acceptance work.

`scripts/test_pairing_controller.sh` uses the real controller, PairingSession,
factory identity loader, native libsodium signatures, HTTP client/parser, NVS
binding implementation and pthread task. Only transport/native I/O are replaced;
the tests do not model real RF timing, flash encryption hardware or ESP TLS.
`scripts/test_watch_pairing_contract.sh` in the root now runs this suite before
the existing real-API factory/signature contracts. Set `SODIUM_SOURCE_DIR` as for
the existing native pairing tests; no ESP build or factory write is performed.

## Physical Pairing Page

`WatchPage::kDevicePairing` reuses the controller's public `PairingState` through
`BuildWatchPairingSnapshot`; it does not introduce a second authentication state
machine. The control-center entry is shown only when the owner advertises the
capability. With the build option off the snapshot stays unavailable. The manual
Start Pairing button requests local control mode, without a network prerequisite.
Cloud binding still requires the verified network/identity path. Mobile secure
addition remains incomplete; this watch page alone is not an end-to-end flow.

The page has separate physical actions for opening, BLE peer consent/numeric
comparison, and binding the challenged Audream account. Account UUID and device
UUID are displayed without truncation. The comparison code retains all six digits.
Pairing UI cannot approve a notification-purpose peer; notification UI cannot
approve a device-purpose peer. Account approval is not displayed as success:
verification/persistence remain pending until the controller reports a durable
binding generation. Storage uncertainty forbids blind retry; an existing binding
requires explicit unbinding in the app.

Each controller opening has a non-wrapping process-local `attempt`. Confirmation,
account approval and cancellation requests capture that attempt along with the
separate peer token or account request. The owner dispatcher must check these
values against its current controller before acting. A cancellation that races a
queued opening carries `pairing_open_request`, the original UI request ID: the
owner must associate it with that opening, not cancel an arbitrary later window.
The Application dispatcher now checks those fields and fences cancellation before
owner queue consumption. UI guards alone are not the security boundary. Native
hardware and mobile end-to-end verification remain outstanding.

Back navigation and hiding the page request cancellation, including an opening
that has not yet been acknowledged. Rejection/cancellation stay usable during
pending work or sleep preparation; positive approval does not. Repeated approval
cannot issue another request while one is pending. State changes immediately
remove stale account/code widgets even while hidden, and reset pairing scroll to
the top so a replacement request cannot inherit an old confirmation position.
Callbacks enqueue intent only; they perform no BLE, NVS, signing or HTTP I/O.

`scripts/test_watch_ui.sh` tests these interactions with real desktop LVGL,
AddressSanitizer/UndefinedBehaviorSanitizer, label/glyph bounds and nonblank
snapshots in `en-US` and `zh-CN`. It exercises UI projection and request delivery
to a capturing handler, not an actual Application or mobile binding flow.

## Factory Identity

The record is `audream_ident/identity_v1` in the existing protected default NVS.
It is exactly 233 bytes:

| Bytes | Meaning |
| --- | --- |
| 0..3 | ASCII `AID1` |
| 4..39 | Canonical API device UUID, ASCII, no terminator |
| 40 | Hardware ID length, 1..128 |
| 41..168 | ASCII hardware ID, zero-padded |
| 169..200 | Raw 32-byte Ed25519 public key |
| 201..232 | Secret 32-byte Ed25519 seed |

Hardware IDs permit ASCII letters, digits, `.`, `_`, `:` and `-`. The device
derives the public key from the seed and compares it with the enrolled key before
exposing public identity or signing. Shape, padding, UUID or key mismatches fail
closed. Private buffers are wiped; the firmware has no identity write API, does
not log secrets, and does not manufacture identities on boot.

Both `CONFIG_NVS_ENCRYPTION` and
`CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC` must be configured, and runtime flash
encryption must actually be enabled. An unprotected build refuses to read secret
bytes. As with voice binding storage, configuration alone is not evidence of
production physical security. Secure Boot, signed updates, key custody and
debug/download policy require a separate manufacturing procedure and hardware
acceptance. These changes do not burn eFuses or enable encryption automatically.

### Preparing Factory Inputs

Run with an existing Python environment containing `cryptography`. Keep output
outside the repository on a protected manufacturing machine. `prepare` creates a
new owner-only directory and refuses to overwrite an existing identity:

```sh
server/.venv/bin/python firmware/note-watch-206/tools/voice_identity/factory_identity.py \
  prepare --directory /secure/manufacturing/WATCH-01 --hardware-id WATCH-01
```

Only send `pairing-public-key.base64` and the public hardware/model metadata to
the administrative enrollment operator. Never send `pairing-seed.bin` to the
API, phone, logs or source control. Run the existing enrollment command against
the intended database and retain its JSON stdout as `enrollment.json`; it now
includes the enrolled public key so a different key cannot be silently finalized:

```sh
server/.venv/bin/python -m server.ops.provision_voice_device \
  --hardware-id WATCH-01 --model waveshare-s3-touch-amoled-2.06 \
  --public-key /secure/manufacturing/WATCH-01/pairing-public-key.base64 \
  --protocol-version 1
server/.venv/bin/python firmware/note-watch-206/tools/voice_identity/factory_identity.py \
  finalize --directory /secure/manufacturing/WATCH-01 \
  --enrollment /secure/manufacturing/enrollment.json
```

`finalize` verifies the exact hardware ID, UUID, public key and private seed and
writes owner-only `factory-identity.nvs.csv`. This CSV contains the private seed:
treat it as a secret, not as a public manufacturing manifest. The file is an
input for a protected **factory** NVS image, not a firmware updater. Do not flash
it over an existing watch's NVS: doing so would erase recordings' metadata,
network settings, ownership and other namespaces. Provisioning an existing
development watch needs a separate explicit migration procedure. No enrollment,
image generation, flashing or production modification is run automatically.

## Signing Rules

`PairingSession` has a two-minute window opened only by local physical intent.
It accepts at most one pending challenge, bound to a nonzero secure peer
generation. That generation must come from authenticated BLE Secure Connections,
not a client-controlled ID, MAC address or unauthenticated BLUFI message.

The exact server payload is parsed as bounded, flat JSON (maximum 1024 bytes).
The purpose must be `audream-pairing-v1`; device/user/challenge IDs must be
canonical, the device UUID must match factory identity, and the challenge must
expire within the next 120 seconds according to a trusted device clock. Missing
clock synchronization cannot be replaced with an arbitrary time supplied by the
phone. The signer signs the original UTF-8 bytes, never reserialized JSON.

The local confirmation uses a monotonically increasing request ID. An accepted
challenge does not itself authorize signing: `ApproveLocally` must come from the
physical UI showing that request. `TakeProof` requires the same secure peer and
request, revalidates the factory identity, and consumes approval even if signing
fails. Cancellation, disconnect, a new window, expiry or clock rollback revoke
old approval. Caller must cancel on recording, sleep, reset or security loss.

The application controller must also bind pending credential delivery to
this approved challenge and peer, verify the server-issued credential via
`Client::VerifyPairing` and the trusted API, and commit through `SaveBinding` with its generation fence and
checked storage result. A BLE acknowledgement cannot stand in for durable binding
or activated bootstrap. The current API permits fresh proof recovery of a
pending credential; active devices require explicit unbinding first.
The verification endpoint checks the exact registration receipt without activating
the device. Do not call bootstrap before successful local storage; otherwise a
failed NVS write could make pending-delivery recovery unavailable.

## Native Peer Security

`WatchAncsClient` owns the existing shared GAP security handshake. Device pairing
does not use the notification opt-in as a prerequisite or an implicit grant:

- The local controller opens `OpenDevicePairingWindow`, returning a nonzero
  generation for a fixed 120-second window. BLE must already be running and
  registered; this call does not start a host, scan, or enable notifications.
  Established ANCS or Android notification sessions cannot be taken over.
- The pairing GATT adapter selects only the current ESP connection
  and address with `SelectDevicePairingPeer`. A public protocol-selection write
  must never open the local window, approve comparison, or grant account access.
- Selection invalidates the older notification prompt. Local peer consent and
  numeric comparison go through `ConfirmDevicePairing`, not the notification
  `Confirm` API. Notification UI cannot display or approve these prompts.
- The phone is the GATT client. The watch requests encrypted Secure Connections,
  MITM protection and bonding without opening ANCS or subscribing to phone data.
  Only accepted authentication yields a nonzero `DevicePairingPeerLease` for the
  exact connection/address. The lease cannot authorize or renew Android content.
- Changing notification consent leaves device pairing independent. Disabling
  notifications still clears notification content. It does not cancel an
  explicitly opened device-pairing window, and pairing never enables collection.
- Handshake phases expire after 60 seconds; the overall window cannot extend
  past 120 seconds, even with successful traffic. Authentication downgrade,
  disconnection, host shutdown, timeout or clock rollback invalidate the lease.
  `CloseDevicePairingWindow(window)` only cancels the captured window; a stale
  queued cancellation cannot close a newer one. Window IDs survive host restarts
  within the same process and never wrap.

The application controller must open both the signing window and the native
security window under physical intent and close both on recording, sleep,
cancel or security loss. BLE comparison establishes the physical connection;
approval of the API account challenge remains a separate physical action.
There is no production local-window caller yet. The new GATT service is separately
build-gated off by default and no complete pairing UI capability is advertised.
The notification build gate remains unchanged.

Run `bash firmware/note-watch-206/scripts/test_watch_notifications.sh` for the
actual native security client compiled against ESP-IDF public headers with only
native I/O/clock replaced. Four additional groups check independent consent,
purpose isolation, stale cancellations, deadlines, retirement and failed/weaker
authentication. Existing ANCS and Android notification tests still run. Desktop
LVGL tests also check that notification pages and banners cannot offer the wrong
pairing confirmation; neither test substitutes for physical phone/watch pairing.

## Crypto Dependency and Verification

Firmware pins [Espressif libsodium 1.0.22~1](https://components.espressif.com/components/espressif/libsodium/versions/1.0.22~1/readme?language=en).
The exact component archive has SHA-256
`d8bbd5dddbc1744869ba1a5c7202cddc4204a6fd63cae6f59f7c3522ac8e9f27` and references
`espressif/idf-extra-components` commit
`1e32ecf000d79db25389d44d29d04b61b72154d4`. Seed derivation and detached signatures
use the [documented libsodium signing API](https://doc.libsodium.org/public-key_cryptography/public-key_signatures), not custom cryptography.

Native dependency preparation is separate from firmware builds:

```sh
bash firmware/note-watch-206/scripts/prepare_host_pairing_crypto.sh
export SODIUM_SOURCE_DIR="$HOME/.cache/audream/host-pairing-crypto-1.0.22-1/source/libsodium/libsodium"
bash scripts/test_watch_pairing_contract.sh
```

The host preparation builds only a native static library in a dedicated cache;
it does not install system dependencies or invoke ESP-IDF. Tests cover protected
storage failures, corrupted identity, challenge validation, physical approval,
peer isolation, single-use signing, cancellation and deadlines. The API contract
uses factory-generated inputs and the C++ signature to register/activate a test
device, rejects challenge substitution and replay, and confirms voice stays off.
ESP flash/NVS operations and physical gestures remain controlled host fixtures,
not proof of completed hardware or mobile pairing.
