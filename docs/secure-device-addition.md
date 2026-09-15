# Unified Secure Device Addition

Status: required product behavior; not yet implemented end to end.
Updated: 2026-09-15.

## Product Contract

The existing iOS/Android Add Device flow remains the only device-addition flow.
For Audream-controlled firmware, adding a device must establish an authenticated,
encrypted BLE relationship before reporting success or starting ordinary device
commands. Cloud voice registration is a separate authorization step, not another
BLE pairing flow and not a prerequisite for recording or local audio sync.

- Scan, select the physical device, obtain watch-side consent and complete BLE
  LE Secure Connections with MITM protection and bonding in that same flow.
- Reuse the established secure physical connection for control, provisioning,
  BLE audio transfer and optional cloud credential delivery. Do not reconnect
  through an unprotected parallel BLUFI path after confirmation.
- Ordinary reconnect restores encryption using the existing approved bond. A
  saved device, matching name/MAC, successful GATT connection or Android
  `BOND_BONDED` alone is not evidence that this connection is ready for traffic.
- Do not silently downgrade to plaintext on rejection, timeout, missing bond,
  security failure or unsupported firmware. Preserve the saved device and its
  local recordings, and show a recoverable security/firmware error.
- Devices saved before migration need secure-link establishment, not removal
  and re-addition. Avoid data deletion, automatic bond deletion or factory reset.
- Pairing does not enable notifications, cloud voice, always-on listening or
  Note generation. A local recording still follows the existing Note workflow.
- BLE addition must work without internet, a synchronized clock, factory cloud
  identity or configured Wi-Fi. These can be required later for cloud binding.

Scope: NoteWatch and other Audream-controlled firmware with a verified secure
profile and physical confirmation mechanism. CB08 and AI08 retain their vendor
protocols; do not claim encryption or break their connection flow by applying
an Audream-only security requirement. A screenless model needs its own verified
physical/OOB approval mechanism; never substitute a fixed public PIN.

## Current Evidence and Gaps

| Existing source | Verified behavior | Required change |
| --- | --- | --- |
| iOS `DevicePairingManager` and `DeviceControlService` | Experimental NoteWatch addition uses the existing control connection and saves only after protected read and subscription; legacy paths remain separate | Verify OS confirmation, cancellation, approved bond restoration and real reconnects before production enablement |
| Android `DevicesViewModel.pair` and `completePairing` | Now require the existing control manager's ready handshake before saving; the setup UI checks completion's result | Verify actual OS pairing, cancellation and persistence races on a phone; secure mode is experimental |
| iOS `DeviceControlProtocol.encodeBLUFICustomFrame` | Sets the BLUFI frame-control byte to zero | This is not BLUFI application-layer encryption; require the authenticated link before writes and responses |
| Android `DeviceControlManager` | Experimental NoteWatch path reads protected readiness on the same GATT before subscribing or sending commands | Complete native/mobile acceptance and approved bond recovery before production enablement |
| Firmware `WatchAncsClient::ConfigureSecurity` | Already configures SC, MITM, bonding and 16-byte maximum keys | Reuse the native security owner; do not introduce a competing GAP callback or auto-confirm numeric comparison |
| Firmware `WatchAncsClient` device-pairing purpose | Local approval is persisted before control admission; approved bonds restore only after native authentication and fingerprint recheck in the experimental path | Verify physical pairing, privacy-address rotation, reboot persistence and removal on phones |
| Firmware `Application::CanOpenWatchPairing` | Local control mode bypasses Wi-Fi/API prerequisites; cloud mode retains them; board startup already starts a control-only host | Verify asynchronous registration, advertising and startup/restart on hardware; pairing does not create a second host |
| Firmware `Blufi` | Application callbacks and queued work check connection/security tickets; a pinned SDK overlay now protects GATT attributes and subscriptions | Finish SDK callback/fragment/packet generation propagation through every queue, then verify actual encrypted traffic |

ESP-IDF v5.5.4's local
`components/bt/common/btc/profile/esp/blufi/bluedroid_host/esp_blufi.c` has
`CONFIG_BT_BLUFI_BLE_SMP_ENABLE`: it applies `GATT_PERM_WRITE_ENC_MITM` to the
phone-to-device characteristic. The original source creates the response
characteristic and CCC with ordinary read/write permissions. Enabling this one
option is not sufficient evidence that all unsolicited outbound data is gated.
Audit the exact pinned SDK, the notification send path and application callbacks
before enabling the production secure profile. Do not patch the developer's
global ESP-IDF installation manually; any necessary SDK adaptation must be
project-managed, version-checked and reproducible.

The current `AUDREAM_WATCH_ANCS` Kconfig also depends on
`!BT_BLUFI_BLE_SMP_ENABLE` to avoid the original BLUFI SMP policy. Do not simply
enable that SDK option: reconcile the single security event owner and incoming
permissions together. The experimental overlay below avoids that SDK SMP policy;
no production secure BLUFI profile is enabled by this work yet.

### Native Control Lifetime Implemented

`WatchAncsClient::AuthorizeControlPeer(window, lease)` is a local-owner API for
explicit control approval. It requires the current live pairing window and
physical peer generation after successful SC/MITM authentication. Neither cloud
binding nor notification consent invokes it automatically. `ControlPeerLease`
checks the exact native connection/address; it is zero before approval and after
disconnect, host shutdown or an observed security failure/downgrade.

An approved connection remains `kControlReady` after cloud-window cancellation
or expiry. Opening another cloud window can reuse that same authenticated peer
without requesting BLE numeric confirmation again. Cloud request/window tokens
still change; old requests cannot use the new window. Native pairing GATT cleanup
now preserves this connection instead of permanently waiting for disconnection.
Notification collection stays independently disabled.

Four new desktop groups exercise the real native security owner and GATT server:
admission, independent lifetime, revocation and actual GATT window reuse. Only
SDK I/O is replaced. The watch entry now uses the offline path below, but mobile
Add Device is not verified end to end. The later approved-bond integration below
adds persistence and restoration; real reboot behavior is not yet verified.
Do not advertise secure addition or encrypted audio transfer based on this test.

### Offline Watch Entry Implemented

The existing `PairingController::Open(true)` handles local control consent. The
watch Start Pairing action selects this mode through the existing Application
dispatcher. It does not read factory identity or cloud binding, open cloud GATT
delivery, obtain a trusted wall clock, sign a challenge, call HTTP or write NVS.
The same owner/resource checks and monotonic 120-second deadline still apply.
Application requires an initialized control-only BLE host; it does not start a
second radio or bypass recording, transfer and sleep ownership.

Source inspection confirms `Application::Initialize` reaches `board.StartNetwork()`
in watch mode. The Waveshare 2.06 override starts `Blufi::init(false)` before
optional Wi-Fi startup; this existing path must not be reimplemented as another
pairing host. The recording-status path also reuses that host after capture.
This is source-level wiring evidence, not proof of on-device advertisement or
successful asynchronous GATT registration.

`OpenControlPairingWindow` supports opening before or after physical connection.
`ConfirmControlPairing` accepts only the exact local window/confirmation and
shares the existing native SC/MITM confirmation path. Notification and cloud
confirmation cannot approve it. A successful authentication produces a
`ControlPairingLease` for the current physical generation, not a cloud binding.
`CancelControlPairing` revokes pending consent or a just-authenticated grant
before owner completion; an old window cannot revoke a newer grant.

The UI reports Secure Bluetooth connected only with a successful control lease,
does not offer duplicate pairing while that connection remains authorized, and
offers retry after the native connection is lost. Notification controls cannot
approve local control pairing. Cloud binding/storage failure does not prevent
an independently admitted local attempt; local pairing cannot repair or replace
the cloud binding.

Desktop coverage adds two native offline groups, seven controller scenarios and
four actual Application scenarios, including cancellation around completion,
offline operation, permission isolation and recording ownership. These tests
replace hardware I/O; they do not prove mobile or RF behavior. The experimental
build flag remains disabled by default. BLUFI SDK admission and approved
bond restoration are still required before enabling secure Add Device.

### BLUFI Application Admission Implemented

`BlufiControlSession` binds admitted work to a monotonically increasing local
connection generation, native connection ID, exact peer address and native
control-authorization lease. Secure mode requires a fresh native-owner lookup
at admission and validation; a GATT connection alone is insufficient. Legacy
profiles do not invoke native security, but also discard obsolete queued work
after disconnect/reconnect, including when connection IDs and addresses repeat.

The actual `Blufi` connect/disconnect/deinit callbacks maintain that lifetime.
Custom requests capture a ticket before queueing, revalidate before dispatch to
`AudreamDeviceControl`, and retain that same ticket for the response. Recording
events require an admitted current peer. With the experimental device-pairing
flag enabled, non-lifecycle BLUFI application events require control admission,
including Wi-Fi credentials, scans, status and connection commands. Asynchronous
Wi-Fi jobs and scan results retain their initiating peer instead of borrowing
whichever peer is connected when the work finishes. A provisioning request
already admitted to execution can persist credentials; revocation is not a
rollback of an operation already performed.

`scripts/test_blufi_control.sh` tests the real ticket implementation with
ASan/UBSan. The native security suite also combines this implementation with
the actual `WatchAncsClient` state machine, replacing SDK I/O only, to test
denial before authentication and immediate invalidation after control revocation.
The complete host runtime suite includes both. It does not compile the entire
`blufi.cpp` translation unit or prove real SDK callback ordering.

**Still not end-to-end secure:** these checks run at the application boundary.
ESP-IDF queues callbacks and outbound packets internally. The reviewed SDK
`blufi_prf.c` and `bluedroid_host/esp_blufi.c` now propagate grants through the BTC
callback/API queue, but lower packet queues still need generation propagation.
The overlay below also adds authenticated
attribute permissions and subscription handling. BLUFI negotiation/fragment
handling also precedes application callbacks.
An application check before `esp_blufi_send_custom_data` is not proof that a
queued packet cannot cross a later reconnect. Implement a version-pinned,
project-local overlay through those queues before enabling this experimental profile.

### Pinned SDK Host Overlay Implemented

`cmake/audream_blufi_overlay.cmake` replaces exactly seven sources in the
`bt` component when `AUDREAM_WATCH_DEVICE_PAIRING` is selected: the four BLUFI
host/API/profile/parser sources, two BTA GATTS API/action sources and the L2CAP
link source for the disconnect cleanup described below. It requires
ESP-IDF version 5.5.4, each reviewed source hash and the BTA argument header hash,
applies a repository patch to build-local
copies and preserves all unrelated component sources. Default
profiles do not create an overlay. The shared native security owner is required;
SDK BLUFI automatic SMP confirmation is explicitly incompatible. No developer
SDK installation is edited.

The patched host requires encrypted MITM permissions on the write, read and CCC
attributes, and separately checks the real native control lease using the
unpacked connection ID and exact peer. Neither an encrypted notification-purpose
link nor a GATT connection implicitly grants Audream control. Prepared/execute
writes are rejected; normal BLUFI frames retain their existing protocol path.
CCC accepts only explicit enable/disable values and binds enablement to the
native lease. New connections and teardown clear subscriptions. New physical
connections also reject a still-visible prior native lease until the asynchronous
native client catches up, rather than accepting old authorization by address.

Outbound notification entry and the congestion retry loop require the same
current lease and subscription. Revocation while waiting stops that send. This
does not alone protect lower-level queued packets. The BTA notification queue
is now stamped as described below. Shared native-state synchronization and
GATT/L2CAP disconnect behavior still require verification before release.

`scripts/test_blufi_overlay.sh` configures enabled/disabled target replacement,
rejects a different SDK version, changed upstream source, a missing compiled source and incompatible SMP,
and verifies the original SDK hash is unchanged. It compiles and runs the actual
patched C host source with SDK types/I/O fixtures under ASan/UBSan, checking
attribute permissions, reads/writes, malformed CCC, prepared-write rejection,
wrong connection, subscription reset and revocation during congestion. The
native suite separately links the actual C bridge to `WatchAncsClient`. This is
not a complete ESP build, SDK ABI validation or RF/mobile acceptance test.

### BTC Queue Provenance Implemented

All four reviewed BLUFI translation units wrap `btc_transfer_context`. Non-lifecycle
messages carry the native control lease captured at admission; dispatch rejects
revoked or replaced leases and invokes the SDK's existing deep-free hook. The
original argument payload remains at offset zero so its copy/free hooks continue
to operate. Other BTC profiles use their original allocation and argument layout.
Initialization, teardown and connect/disconnect retain SDK lifecycle delivery;
these lifecycle events are not themselves generation-stamped by this change.

Async custom responses, provisioning reports and scan results use explicit-lease
SDK entry points. They supply the originating application's ticket, rather than
capturing a possibly newer peer at the point of enqueue. Both admission and
dispatch validate that same lease. Ordinary SDK entry points remain available.

The actual SDK BTC header is four bytes followed by inline arguments. The lease
is read with `memcpy`, not an unaligned `uint64_t` load. Desktop fixtures reproduce
this argument offset and exercise the actual patched API source with ASan/UBSan:
revocation, reconnect, payload deep copy, stale explicit grants, lifecycle delivery,
foreign profile passthrough and failed queue posting with deep-copy cleanup.
These fixtures do not establish complete target ABI compatibility. Shared
native-state synchronization, lower GATT/L2CAP lifetime, mobile integration and persistent approved bonds
remain release blockers.

### Receive Fragment Ownership Implemented

The reviewed SDK `btc_blufi_recv_handler` now ties its aggregation buffer to the
native control lease. A changed or revoked lease discards pending fragments and
resets receive sequence/security mode before accepting a fresh authorized stream.
It does not reset the independent send sequence. Existing host disconnect cleanup
continues to free the aggregation buffer.

Fragments require a complete two-byte remaining-length field and nonempty content,
consistent message type, and exactly the expected remaining length. A final
fragment must have the same type and fill the declared total length. Malformed
input, checksum/decrypt failure and sequence errors discard the partial buffer.
Encrypted/checksummed frames are rejected when the required callback is missing;
the parser must not silently treat such data as unchecked plaintext. Authorization
is rechecked after security callbacks and before complete-message dispatch.

The overlay host test extracts this exact patched receive function and its local
helpers from the hash-checked SDK source, compiles them with the real SDK internal
BLUFI header, and substitutes allocation, crypto and protocol-dispatch I/O.
ASan/UBSan tests cover valid multi-fragment assembly, grant changes, zero/short
headers, wrong type/remaining length, final-length mismatch, allocation failure,
missing/failed crypto callbacks, truncation and revocation during decrypt/ACK.
This is not compilation of the entire profile translation unit or a concurrency
proof. The SDK invokes the BLUFI host callback from BTA while application callbacks
and public sends traverse BTC. Execution provenance is now preserved as below;
the BTA queue now carries it as below; lower-stack lifetime verification remains.

### Execution Authorization Implemented

`watch_blufi_scope.c` supplies a task-local execution scope shared by the patched
SDK and the actual application. BTA host callbacks, receive processing and send
fragmentation establish a scope; BTC call/callback dispatch uses the lease from
the queued envelope. Public sends and protocol callbacks inherit that original
lease rather than recapturing a replacement peer. Nested operations cannot upgrade
their parent's authorization. A zero-lease lifecycle scope denies ordinary data;
it is distinct from an unscoped call. All wrapper returns restore the prior scope.

The actual application's native control lookup compares against the execution
lease before capturing or validating a request ticket. SDK read/write/CCC admission,
notification entry and the send fragment loop also honor the same scope. Revocation
inside a callback invalidates subsequent work without rolling back effects already
performed. The send loop stops before allocating the next fragment after revocation.
Async application work still carries explicit tickets across task/queue boundaries;
task-local scope does not replace those tickets.

ESP-IDF 5.5.4 includes compiler TLS support and `__thread` tests in
`components/freertos/test_apps/freertos/port/test_thread_local.c`. The new C source
is included only with experimental device pairing. Desktop tests run the real scope
implementation, actual patched public API/host, and extracted reviewed receive/send
functions under ASan/UBSan. They cover nested grant denial, zero-lease denial,
scope restoration, revocation during dispatch/crypto/send, and concurrent task
isolation over 100 runs. This does not verify the complete ESP build, target TLS
allocation or all SDK shared-state data races.

The outbound BTA queue is in
`host/bluedroid/bta/gatt/bta_gatts_api.c::BTA_GATTS_HandleValueIndication` and
`bta_gatts_act.c::bta_gatts_indicate_handle`. The former copies payload bytes and
posts them; the latter resolves the connection and calls GATT. The integration
below carries the original authorization instead of substituting a newer peer.
Shared host state and lower GATT/L2CAP disconnect cleanup still require an audit.

### BTA Notification Queue Implemented

BLUFI now calls a dedicated project-managed BTA enqueue function that copies the
payload together with the original control lease and subscription epoch. It uses
the standard BTA indication prefix plus a private trailing envelope, identified
by the otherwise zeroed standard producer header fields. The reviewed internal
argument header is hash-checked; no public SDK structure is edited. Unmarked
messages targeting the BLUFI response attribute are rejected at the BTA consumer.
Other GATTS attributes retain their standard indication/notification behavior.

Before dispatch and again after resolving the GATT connection, the consumer
checks the original lease, connection, attribute and subscription. Reconnect,
authorization replacement, unsubscribe/resubscribe or service teardown invalidates
queued data. CCC writes and lifecycle invalidation advance the subscription epoch;
epoch exhaustion fails closed without wrapping. Rejected messages do not generate
an application callback carrying the old payload.

The BLUFI path bypasses the public GATTS BTC producer, so it allocates one BTU
credit without decrementing a BTC credit belonging to another producer. Nonblocking
`btu_task_post` avoids deadlock when an ACK originates on BTU itself. Post failure
frees the message, releases that credit and emits a queue-rejection warning; SDK
enqueue success is not a guarantee of over-the-air delivery. The consumer
releases the credit once on every tagged path, including missing service/connection
and rejected metadata. The original standard producer remains unchanged.

Desktop tests compile extracted patched BTA producer/consumer functions and the
actual SDK message/header type definitions, substituting GATT and queue I/O.
ASan/UBSan coverage includes payload copying, stale grant/connection/attribute,
unsubscribe/resubscribe, rejection after connection lookup, malformed envelopes,
standard-producer bypass denial, unaffected foreign profiles, allocation/post
failure, maximum payload and balanced credits/allocation cleanup. Separate actual
host tests validate subscription epochs and exhaustion. These are not full SDK
translation-unit builds or RF/concurrency/stack-shutdown acceptance. Remaining
shared-state and lower-stack audits cannot be inferred from these fixtures.

BLUFI frame encryption and BLE link encryption are different layers. An
unencrypted BLUFI frame may travel inside an encrypted BLE link; conversely,
having AES callbacks in firmware does not show that the phone negotiated or uses
them. Prefer the existing native SC/MITM security implementation instead of
inventing a new key exchange. See the
[Espressif BLUFI guide](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32s3/api-guides/ble/blufi.html)
for the distinct application-layer security negotiation.

## Android Protected Control Read

The experimental firmware's BLUFI response characteristic (`FF02`) now returns
exactly `41 44 53 43 01 07` on an authorized read: `ADSC`, profile version 1,
and flags for Secure Connections (bit 0), MITM authentication (bit 1), and
explicit local control approval (bit 2). Native admission also requires the
configured bonding authentication mode. The read retains encrypted MITM attribute
permissions and a current native control lease. CCC reads retain their existing
two-byte subscription format. Ordinary legacy firmware is not modified.

This value is a versioned firmware assertion, not a cryptographic attestation or
an independent key exchange. OS security and native control authorization remain
mandatory; a matching name, saved address or cached bond is not proof of readiness.

Android enables this path only with
`-PAUDREAM_ANDROID_SECURE_CONTROL_EXPERIMENTAL=true`, currently for NoteWatch with
the matching experimental firmware. The default remains false while firmware,
iOS integration and approved-bond storage now have host coverage below; actual
pairing and persistence acceptance remain incomplete. Vendor devices
and screenless models do not inherit an unverified physical confirmation scheme.

The existing manager starts a protected read before notification subscription,
validates the exact response, then permits Ready only after subscription succeeds.
It uses the same physical GATT throughout; no second connection or manual numeric
auto-confirmation is added. Refusal, unsupported/legacy responses, service changes,
disconnect and the 120-second connection/handshake deadline invalidate the session.
No ordinary control commands or incoming control payloads are admitted before Ready.
The existing Add Device completion checks readiness before saving and displays
localized confirmation/failure text. Returning/skipping cancels the setup connection.
Saved devices and recordings are not erased by these failures.

The Kotlin handshake unit suite covers read/subscription ordering, malformed and
legacy responses, failed reads with cached payloads, all closed phases, replacement
connections and the unchanged vendor subscription path. Gradle compiles the actual
manager and UI as part of JVM tests. These tests do not exercise Android Bluetooth
callbacks, pairing prompts, hardware encryption, UI layout or process death.
Real phone acceptance remains outstanding; this is not a
production-ready secure addition claim.

## iOS Protected Control Read

With `AUDREAM_WATCH_SECURE_CONTROL_EXPERIMENTAL` explicitly enabled, the existing
NoteWatch Add Device screen delegates connection ownership to `DeviceControlService`.
The discovery manager only scans; it does not create a second GATT connection or
save a device from its `didConnect` callback. The control manager reads the same
protected `FF02` readiness value described above, then enables notifications.
Only successful completion of both steps allows Ready, ordinary control traffic
and persistence of the added device. Previously saved device metadata is reused.

The per-connection `DeviceSecureControl` state machine rejects malformed, legacy,
failed, duplicate and out-of-order results. A 120-second connection/handshake
deadline, disconnect, service invalidation and observed security errors close the
handshake without falling back. Callback routing checks the active peripheral
object; the read value is copied before scheduling main-actor work. These checks
are not proof of every CoreBluetooth callback ordering on a real device.

An addition attempt has its own token. Leaving/retrying the setup flow cancels
that pending attempt; an obsolete token cannot cancel a newer attempt. An already
ready connection reused by addition is preserved. Cancellation leaves the closed
handshake in force until disconnection, preventing delayed callbacks from entering
the legacy branch after the target device is cleared. Failed addition does not
delete saved devices, recordings or OS bonds. The OS owns pairing UI; the app does
not automatically confirm comparison codes or invent another exchange.

The compile-time flag remains off by default and applies only to NoteWatch with
matching experimental firmware. CB08/AI08 and screenless legacy profiles do not
receive this requirement. Approved bond persistence/recovery still needs device
acceptance, as do BLE startup and asynchronous registration; this flag must not be presented as production
security or silently enabled for existing users.

`scripts/test_device_secure_control_ios.sh` compiles and executes the actual Swift
handshake with the flag both off and on, including malformed/cached-error payloads,
ordering, cancellation and replacement-state isolation. The real iOS project also
builds for the simulator with the secure flag enabled. Pure state tests and a
simulator build do not exercise system pairing prompts, real GATT encryption,
radio loss, UI rendering, process death or bond migration. The default simulator
configuration also builds successfully. No app was installed.

## Mobile Cloud Pairing API Contract

The existing iOS API client and Android Retrofit interface now expose
`POST /v1/devices/pairing-challenges` and `POST /v1/devices/register`. iOS also
exposes a pairing-scoped `/v1/auth/me` call; Android reuses its existing `me` call.
These methods do not create another Add Device flow or require the network for
offline BLE addition. They are the HTTP boundary used by the experimental iOS
workflow below; Android's cloud pairing coordinator remains unfinished.

Both mobile model implementations require canonical device/user/challenge IDs,
the expected signing domain, Ed25519/UTF-8, bounded challenge lifetime and body,
and matching payload identities. They preserve the original challenge bytes for
the device signature. Registration validates a 64-byte proof and bounded firmware
version. A returned credential must match the expected device and the `pending`
status, with a positive credential version and the firmware-compatible token
format. Credential descriptions are redacted. No credential is stored by these
models or sent to a BLE peer merely because HTTP registration succeeded.

iOS checks the caller's captured session token and API endpoint before sending
and after receiving a pairing request, rejects cancellation, and does not reuse
cached responses. A stale 401 cannot clear a different account's session. Pairing
409 responses remain server errors, rather than being converted to voice-settings
revision conflicts. Android's added methods are raw Retrofit calls: the future
coordinator must invoke the validation methods and enforce equivalent operation,
account and endpoint ownership checks around each suspension point.

`scripts/test_device_pairing_mobile.sh` generates synthetic challenges and
credentials through the real local FastAPI/store implementation. Swift encodes
a registration body which that server accepts while leaving the device pending;
both mobile model contracts consume the same server response. A Swift HTTP fixture
compiles the production request methods with app storage and URLSession I/O
replaced, and covers original request bodies, identity lookup, cancellation,
account/endpoint changes, conflicts and unauthorized handling. Android executes
its real Retrofit declarations against response fixtures. These tests do not
contact production or establish a physical BLE connection.

The iOS workflow below connects these calls to the existing approved GATT
connection, with monotonic expiry, captured context and a required bound response.
Android still needs that integration. The complete mobile/server/device
verification and persistence transaction needs real-device acceptance. Cancelled
cloud pairing must not erase an already approved local control relationship.
Existing device migration and removal/bond cleanup remain open.

## iOS Same-Connection Cloud Transport

`DeviceCloudPairingTransport` now implements the firmware hello/selection,
challenge fragments, proof polling, credential fragments and bound-status read.
`DeviceControlService.makeCloudPairingTransport` requires its existing approved
NoteWatch control connection and an explicit authorization-context predicate.
The adapter uses the existing central, peripheral, delegate and security handshake;
it does not scan, reconnect or replace the control characteristics. The iOS
workflow and assistant settings below now call this transport; Android's equivalent
GATT delivery remains unfinished.

The adapter routes the pairing service separately and checks the actual peripheral,
service and characteristic objects. Control loss, service invalidation and shared
request failure cancel the transaction. Individual pairing ATT errors do not
themselves disconnect an independently approved control connection. After closing,
the owner retains a tombstone until physical disconnect, rather than allowing an
old ATT callback to be treated as a new cloud transaction. A later cloud retry on
this conservative implementation requires reconnecting the saved device, not
another Add Device operation.

Every fragment is sent with response and is at most 182 bytes: CoreBluetooth's
with-response maximum can include prepared writes, which the firmware forbids.
The protocol requires a negotiated MTU of at least 185 and the firmware checks it;
the app's maximum-write value alone is not proof of the negotiated MTU. Each ATT
operation and entire fragment batch have a 10-second deadline, within a monotonic
120-second transaction bound. Status reads must match the opaque request number;
only firmware `bound` completes credential delivery. The iOS workflow additionally
enforces server challenge expiry and pins account/endpoint ownership through its
supplied predicate.

`credentialDeliveryStarted` records that a credential write may have reached the
device. A failure after that point is not proof that durable binding failed or was
rolled back. `isBound` records an observed device acknowledgement and is not cleared
by subsequent local cancellation. Recovery still needs authoritative device/server
state, especially when the final response is lost during NVS commit.

`scripts/test_device_pairing_transport_ios.sh` compiles the actual Swift transport
and the existing native GATT/security test executable with real SDK public types.
They exchange hello, selection and fragments over a local fixture process; native
reassembly must match the exact Swift payload, and Swift waits through real GATT
processing/committing stages. Only the application worker result is simulated
(synthetic signature and commit admission), not a full server/HTTPS/NVS round trip.
The script also tests malformed frames, wrong-request proofs, peer/context loss,
cancellation, fragment/transaction deadlines and late operation replies, and runs
the existing native notification/pairing regression suite. The same Swift exchange
also runs with `CONFIG_AUDREAM_WATCH_DEVICE_PAIRING=1` and protected NVS configuration,
linking the real approval-storage implementation and native libsodium; only bond
enumeration and NVS hardware I/O use fixtures. This second run checks that an
actually approved native control lease survives cloud-window cleanup. Set
`SODIUM_SOURCE_DIR` to the existing configured native libsodium source when running
the script. Both configurations passed with fatal sanitizer failures enabled.
This does not exercise
CoreBluetooth radio behavior or system pairing prompts. The actual iOS project
builds for the simulator with the secure flag both off and on; neither app
installation nor firmware flashing occurs.

## iOS Explicit Cloud Authorization Workflow

`DeviceCloudPairingWorkflow` sequences current-user lookup, discovery/selection
on the approved connection, the API challenge, device-side approval/signature,
registration and credential delivery. It returns the cloud device ID only after
the transport observes `bound`. The whole workflow is bounded to 120 monotonic
seconds; challenge expiry starts before its HTTP request, so response latency
does not extend the reported lifetime. Context and task cancellation are checked
between every asynchronous stage and supplied to each GATT operation.

`DeviceControlService.makeCloudVoiceAuthorization` wires this workflow to the
real API methods and existing physical connection. It requires the experimental
NoteWatch configuration, the currently selected saved device, a ready protected
control connection, actual firmware metadata, and an HTTPS API endpoint. It pins
the original token and endpoint; device removal/selection changes or security loss
invalidate the attempt. It does not create another central or Add Device flow.

The existing iOS assistant settings expose explicit cloud authorization for that
selected watch under the experimental flag. A confirmation dialog, localized
progress, cancellation, failure and uncertain-commit states are connected. Leaving
the page or changing account/server/device retires the UI attempt; old callbacks
cannot update a replacement attempt. The existing device list is refreshed after
an acknowledged binding. Authorization does not enable voice settings, create
Notes or enable conversation retention. Offline secure Add Device is unchanged.

After a credential write has started, failure or cancellation is reported as an
uncertain outcome, not proof of rollback. Recovery may require checking the watch
and refreshing its server state; automatic retry/unbind is not attempted. As with
the transport, a retry after a retired connection-local attempt requires explicitly
reconnecting the already saved device. Production flags remain disabled until
the complete migration and real-phone acceptance are satisfied.

The workflow contract runs against the actual native GATT fixture in both ordinary
and protected-approval-storage configurations. HTTP callbacks are test fixtures
that assert identity, request ordering and exact proof; the real HTTP API contract
is independently covered by `test_device_pairing_mobile.sh`. Workflow tests cover
success, late HTTP completion after cancellation, context changes before credential
delivery, challenge latency/expiry, duplicate runs, reentrant cancellation before
registration and uncertain delivery. These are not an installed SwiftUI usability
test, a real RF test or a complete production HTTPS/private-key/NVS test.

## Approved Bond Record Foundation

`watch_control_bond` is included in the experimental device-pairing build and
implements native bond identification and protected approval records. The native
owner now uses these records through the integration below; actual phone and
reboot acceptance remains unfinished.

`FindNativeBond` reads ESP-IDF's public bond list for an address obtained from the
native connection. It matches the SDK bond address or the supplied identity-key
address, rejects ambiguous matches and requires a 16-byte nonzero LTK marked
authenticated by the pinned SDK. When PID keys are present, the identity address
and nonzero IRK are included. A domain-separated SHA-256 fingerprint covers the
identity/type, LTK and optional IRK; replacing the keys cannot inherit the old
approval merely because the address is unchanged. Raw key copies are wiped and
are not logged or written to a second store. No custom RPA resolver is added;
actual privacy-address resolution by the SDK still needs phone testing.

The adapter bounds enumeration to 32 bonds and rejects count changes, SDK errors,
invalid address types and weak/incomplete key records. Bond enumeration is not an
atomic snapshot of the active link. Neither a matching fingerprint nor the SDK's
stored authentication level proves that this live connection used SC/MITM and the
same keys. The native owner must check current authentication, connection identity
and generation before restoration, and must not auto-accept a new comparison code
just because a previous bond exists.

Approvals use the separate `audream_ble` NVS namespace and require the existing
encrypted-NVS/flash-encryption protection policy; they do not read cloud identity,
credentials, Wi-Fi or wall-clock time. Records store the full fingerprint and an
approved/revoked marker. NVS's short key uses a hash prefix, but a full-record
mismatch fails closed rather than overwriting a colliding entry. Approval requires
a final native-owner admission callback, even for an already approved record.
Missing records are not permissions. Corrupt records are not silently repaired.
Write/commit errors disable all record use for the current boot because NVS may
have changed despite an error. A failed revocation is not a successful durable
removal and must be surfaced by the eventual owner/UI integration.

`scripts/test_control_bond.sh` builds the actual module against real SDK public
types and the existing native libsodium library, replacing only bond-list, NVS and
protection I/O. Six executions cover protected/unprotected builds, a fixed hash
vector, address aliases, replaced keys, malformed/ambiguous native entries,
record corruption/prefix collision, admission rejection, idempotence, and set/
commit failures during approval and revocation. ASan/UBSan errors are fatal.
The tests do not verify ESP NVS persistence, real pairing, RPA rotation or reboot
restoration. The owner integration below adds separate native-state coverage;
mobile remove/retry and inactive-bond record cleanup remain required.

## Native Approved Bond Restoration

Under the existing experimental device-pairing flag, `WatchAncsClient::Poll`
serializes bond lookup and NVS work outside its security-state mutex. Results retain
the original physical link generation and peer. A later connection cannot consume
an old lookup or save result, even when the address repeats.

After explicit local confirmation and SC/MITM/bond authentication, control remains
unavailable until the approval is written and the SDK fingerprint is rechecked.
The local controller's window remains pending during persistence. Cancelling or
expiring that attempt during a successful new write schedules immediate revocation;
the stale write cannot authorize a replacement connection. Cancellation after the
write but before local-controller completion also revokes the new provisional
approval. Reusing an already approved bond does not turn cancellation into removal
of a pre-existing relationship. A write error is not a successful addition.

On reconnect, a matching approved fingerprint only allows a native encryption
request. It does not grant control. A successful native authentication event must
include SC, MITM and bonding; another poll checks the same fingerprint and approval
again before granting a fresh per-connection lease. Early phone-initiated encryption
is handled without assuming that `esp_ble_set_encryption` returning ESP_OK proves
completion. The reviewed SDK `smp_utils.c::smp_proc_pairing_cmpl` selects the stored
authentication mode when no new pairing mode was negotiated, and `btc_dm.c`
forwards it in `ESP_GAP_BLE_AUTH_CMPL_EVT`. Actual callback timing remains a phone
acceptance requirement, not something proven by injecting the event in a test.

New numeric-comparison or key-distribution events during restoration are rejected;
restoration never auto-confirms a replacement pairing. Missing approval leaves the
explicit pairing path available, while authentication errors, changed keys,
timeouts and storage errors deny control. Turning off notifications does not revoke
an otherwise valid control restoration and does not enable notification collection.

Successful native removal of the current peer revokes its known fingerprint.
A successful native clear-all-bonds event clears the separate approval namespace,
including when no peer is connected. Cleanup runs before another lookup; failure
denies further restoration. This does not yet implement the mobile Remove Device
command or find orphaned records for arbitrary already-disconnected native peers.

`scripts/test_control_bond_native.sh` compiles the actual owner, actual record store,
BLUFI authorization bridge and real SDK public types, replacing native/NVS/clock I/O.
Five groups cover local durable admission, ordinary and early-auth restoration,
simulated host restart, independent notification opt-out, weak authentication,
new comparison/key events, key replacement before/during restoration and during
save, expiry during commit, cancellation before completion and inside commit,
connection replacement during lookup/recheck, current-peer removal, clear-all,
and approval-write failure. A separate execution tests clear-all commit failure.
All run with fatal ASan/UBSan. The full host-runtime entry now requires the existing
native libsodium build via `SODIUM_SOURCE_DIR` and runs both bond suites.

These tests do not establish real NVS atomicity across power loss, controller radio
encryption, privacy-address rotation or originating-link identity of every delayed
SDK security callback. Initial BLE availability, mobile migration/removal, orphan
cleanup, SDK lifecycle audit and full hardware acceptance remain open. No production
profile is enabled and no ESP firmware is built or flashed by these host tests.

### Secure Host Startup Failure Handling

With experimental device pairing enabled, failure to initialize the native
security owner, Android notification service or pairing GATT service now aborts
BLUFI startup instead of only logging an error and continuing to profile init.
Default notification-only builds retain their existing startup policy.

On a synchronous startup failure, the application session is deactivated and
its control ticket reset before cleanup. All native owners retire, then the
code checks actual Bluedroid status and disables/deinitializes only the stages
that were reached. The failure path has not accepted a BLUFI profile-init
request, so it does not enqueue profile deinit. Only a confirmed uninitialized
host permits controller cleanup. Failed host/controller cleanup latches restart
rejection for this boot instead of reusing partial state.

`scripts/test_blufi_startup.sh` extracts the real application lifecycle methods
and compiles them with real SDK host status definitions and platform I/O
fixtures, using fatal ASan/UBSan. It covers eight startup failure points,
cleanup ordering, retry after successful cleanup, failure latching, a disabling
host, offline control-only startup and the default build policy. It is included
in `test_watch_runtime.sh`. The test does not execute the complete SDK, prove
callback quiescence, cover a later asynchronous profile-registration failure,
or establish radio-level acceptance.

### Asynchronous Startup Admission

Native GATTC registration now consumes one completion for the pending local
startup. Wrong app IDs do not consume it; a failure, invalid interface or
60-second monotonic timeout closes it. Duplicate results cannot replace a
registered interface or recover a failed attempt. Notification preference
changes cannot extend the registration deadline. This does not distinguish an
old SDK completion delivered during a later host startup; callback quiescence
across a full restart remains a separate requirement.

`CanOpenPairingWindow` is a memory-only snapshot of the same native predicate
used when actually opening the window. Application checks this together with
`Blufi::IsControlReady` before enabling or accepting the existing Start Pairing
action. A profile pending/failed state cannot acquire the BLE control lease.
The experimental BLUFI profile state accepts one init result, does not advertise
on a failed result, ignores duplicates after completion/stop, and requires
normal host cleanup before retrying an asynchronous failure. Default builds
retain their existing profile callback behavior.

BLUFI startup also has a 60-second monotonic deadline, beginning before controller
initialization. Readiness queries, repeated init and completion callbacks all
check the original deadline. Repeated init cannot extend it; a completion at or
after the boundary, or after a clock rollback, cannot enable advertising or grant
the application control lease. The phase and timestamp share a short mutex;
no SDK or lifecycle calls occur under that mutex. A successfully completed
startup does not expire later merely because 60 seconds have passed. This is
admission timeout handling, not an autonomous host teardown/restart watchdog.
Normal deinit must finish before a new attempt begins.

Tests use actual native code and SDK GATT types for registration and timeout;
extracted real BLUFI lifecycle/profile-handler/lease methods for profile states;
and actual Application/controller code with native I/O fixtures for both
unavailable states. They assert no resource reservation or identity/HTTP work
before readiness. Approved bond restoration and recording handoff still require
their separate regressions and hardware acceptance.

The startup suite additionally covers the exact timeout boundary, large monotonic
timestamps, rollback, missing callbacks observed through each admission entry,
retry only after cleanup, and concurrent completion/stop. The same host test runs
with experimental security both disabled and enabled, under fatal ASan/UBSan.

The suite now extracts the real `_host_deinit` and `_controller_deinit` methods
as well, with the pinned public Bluedroid and controller status types (ESP32-S3
uses the SDK's `esp32c3` controller include directory). The experimental teardown
checks the resulting host state before publishing HostStopped. Controller
teardown requires an uninitialized host, accepts an already-idle controller,
skips disable for an initialized-only controller, and stops on disable failure
instead of replacing that error with a later deinit success. Successful deinit
must also leave the controller idle. Invalid/intermediate states or mismatched
success/status leave the lifecycle failure latch set and prohibit restart.
Tests cover five operation failures, four unchanged-state success responses,
ordering of native owner retirement, and cleanup/retry after startup timeout.

This does not make SDK teardown time-bounded. In the pinned SDK,
`esp_bluedroid_disable/deinit` call `future_await`, which waits with
`OSI_SEM_MAX_TIMEOUT`. Deinit later frees the BTC thread; `osi_thread_stop` waits
one second before forcibly deleting a nonjoining task. These paths require
callback, lock and pending-allocation lifetime review before adding an automatic
recovery task. A desktop return-value fixture does not prove graceful task
quiescence, and a timeout must not free a future still used by the SDK.

One concrete callback/lifecycle lock cycle is now removed in the experimental
path: `deinit()` holds the lifecycle mutex while awaiting SDK host disable,
whereas a queued BTC GET_WIFI_LIST callback calls `start_wifi_scan()`. That
callback now tries the mutex and rejects a busy request without starting a scan
or reporting success, rather than blocking the BTC task needed by disable.
The regression calls the actual public `deinit()` (not just its unlocked private
helper), makes the host-disable fixture wait for a separate BTC-like scan callback,
and verifies that callback returns while shutdown still owns the mutex. Shutdown
then completes, and a new scan can be admitted after restart. The protected scan
body and its authorization/Wi-Fi gates are unchanged; its SDK I/O is a fixture in
this test. This removes this specific wait cycle, not every SDK callback or every
possible hung-future condition. Default builds retain their previous locking.

The pinned upstream `esp_blufi.c` emitted `ESP_BLUFI_INIT_OK` immediately after
requesting `BTA_GATTS_StartService`, without waiting for completion. The
experimental overlay now advances through registration, service creation, write
characteristic, read characteristic, descriptor and service-start completion.
Only the matching `BTA_GATTS_START_EVT` with successful status emits INIT_OK.
The pinned `bta_gatts_act.c::bta_gatts_start_service` produces that event after
checking the `GATTS_StartService` return value.

Each creation result checks its expected stage, status and valid handle. Events
for other interfaces, services or unexpected characteristic UUIDs cannot advance
the operation. Creation failure is checked before accessing SDK fields that may
be uninitialized on that failure path. Failure is reported once; duplicate or
stopped results cannot re-enable the same startup. Deinit invalidates the stage
before queuing service teardown. The stage is published with the existing host
security snapshot under its short critical section; no SDK calls occur while
that lock is held. The ordinary transport lease additionally requires READY.
Failure to enqueue the result leaves the stage failed and data denied.

`sdk_test` compiles the actual patched host with platform I/O fixtures and covers
all six failure stages, invalid handles, foreign/duplicate events, stop during
every stage, result-queue failure and control denial before READY. Existing
permission, CCC, send-revocation and concurrent snapshot tests remain. All five
SDK overlay executables now use fatal ASan/UBSan, and the wrapper verifies the
global SDK hashes remain unchanged. The fixtures are not a complete SDK target
build or callback ABI/ordering proof.

Still required: full-host restart callback provenance, lower-layer queue cleanup,
autonomous cleanup/recovery after a missing startup callback, physical advertising/connection and mobile
acceptance. The completion callback fix is not proof of these wider requirements
and does not enable the experimental profile for production.

## Integration Order

### Android Add Device Attempt Admission

Android addition now pins an opaque attempt token to the original physical GATT
object. A Ready event for a replacement connection with the same device ID cannot
complete that attempt. The asynchronous ready waiter also checks the captured
physical connection, rather than accepting the latest device-ID state alone.

Cancellation, leaving the setup screen and ViewModel disposal retire the pending
attempt. Cleanup conditionally removes only that original connection, never its
replacement. Old attempt tokens cannot cancel or complete a retry. The store
checks and consumes admission inside its serialized preferences update, so a
cancelled attempt queued behind another update cannot subsequently add/select a
device. Once an update is admitted, cancellation is not a rollback of persistence
already in progress; storage errors are reported rather than shown as success.
No local recording, existing saved-device record or platform bond is deleted.

JVM coverage checks original-versus-equal/replacement identity, missing connections,
readiness, one-time admission, cancellation before a queued update, independent
retry tokens and concurrent cancellation/admission. These are state-machine tests,
not Android DataStore disk-failure or Bluetooth RF tests. The existing secure
handshake tests still require protected-read and subscription success and reject
old firmware without falling back. Experimental flags remain disabled by default.

### Android Same-Connection Cloud Transport

Android now has a cloud-pairing wire parser, coroutine channel and adapter in
the original `DeviceControlManager`. The adapter only accepts an already-ready
experimental NoteWatch connection and an explicit authorization-context predicate.
It pins the original GATT/session and uses the control/notification mutex, rather
than creating another connection, pairing owner or ATT queue. The existing GATT
callbacks route reads/writes by exact characteristic object; disconnect, service
invalidation and control failure retire the transaction. Closing only the cloud
channel does not disconnect the approved control link. Closed transactions stay
reserved until physical disconnect, preventing stale callbacks entering a retry.

The channel validates the 46-byte identity hello, opaque window selection,
74-byte status and positive request ID, preserves the challenge's exact UTF-8
bytes, caps writes at 182 bytes and requires negotiated MTU >= 185. The entire
transport lasts at most 120 seconds; ATT operations and a whole fragment group
have non-extending ten-second limits. Shared-lock waits count toward the timeout.
Only proof-ready and bound stages finish their respective operations; pending,
processing and committing are not success. Delivery uncertainty and an observed
bound result survive local cancellation, so closing cannot imply rollback.

`scripts/test_device_pairing_transport_android.sh` connects the actual Kotlin
channel to the existing actual native GATT/security test binary, in ordinary and
protected-bond configurations. It checks exact reassembly, large request IDs,
commit polling and retained control authorization. SDK/NVS I/O and application
worker signing/commit results are fixtures, as with the iOS contract. A separate
JVM suite tests malformed wire data, order, cancellation, wrong peers/requests,
mutex contention, timeout and uncertain credential delivery. Neither suite runs
the Android BluetoothGatt adapter against a phone or proves a complete real
API/private-key/HTTPS/device-NVS transaction. The Android HTTP workflow and
explicit authorization UI are now wired as described below; production flags remain off.

### Android Account Workflow and Settings Entry

`DeviceCloudPairingWorkflow` uses the original Retrofit API for account identity,
challenge and registration, then the approved-link GATT channel for proof and
credential delivery. Account and device UUIDs, exact challenge bytes, pending
registration, firmware version and final bound status are validated at the
appropriate boundaries. The 120-second workflow lifetime includes account HTTP;
the challenge TTL includes its HTTP response latency instead of restarting when
the response arrives. Cancellation invalidates context and cancels the workflow's
own child job, not an unrelated caller scope. A late response cannot advance into
the next step. Possible credential commitment is reported separately from ordinary
failure/cancellation, and cancelling an observed binding does not imply rollback.

The existing Android voice settings page exposes an explicit confirmation for the
currently selected, saved NoteWatch only in the secure experimental build. It
requires an HTTPS API endpoint, current sign-in, actual reported firmware and the
existing secure control connection. It pins the API instance/authorization header,
observes saved selection and endpoint independently of foreground lifecycle, and
retires work on disposal, account/server/device change or connection loss. The
system pairing prompt becoming temporarily inactive is not itself cancellation.
Old disposed UI callbacks cannot clear a replacement screen's busy state. After
bound, the page refreshes the real cloud device list; it neither enables the
assistant nor creates Notes or changes transcript retention.

Eight workflow JVM tests cover request order, identity changes at HTTP boundaries,
expiry/clock rollback, cancellation before start and during ignored HTTP
cancellation, invalid identities/credentials and uncertain delivery. The native
contract now also runs the full workflow through the actual GATT/security fixture
in both configurations. HTTP functions and worker signing/commit remain fixtures
there. Separately, `test_device_pairing_mobile.sh` exercises the actual Retrofit
request adapter with real local-server contract output and verifies request paths,
headers, bodies and unmodified HTTP 409 errors. It does not run a physical phone
or prove production HTTP/private-key/NVS end-to-end behavior.

Still required: mobile lifecycle/UI and real OS prompt acceptance, complete real
service/device cryptographic and persistent-storage integration, interrupted
binding recovery and device/bond removal or migration. No app has been installed
and no ESP target firmware has been compiled or flashed by these tests.

### Remaining Integration Steps

1. Extend the existing native security owner with a locally approved control
   relationship independent of notifications and cloud credentials. Maintain
   physical connection identity and non-reused generation checks. Reject a
   different phone even if its address or request number resembles an old one.
2. Gate the real BLUFI command/response, Wi-Fi provisioning and BLE file channel
   on that relationship. SDK attribute enforcement and outbound admission must
   both be tested. Revoke queued work and subscriptions on disconnect or loss of
   security; a callback from an old connection cannot authorize a new one.
3. Integrate that handshake into the existing iOS/Android control managers and
   Add Device UI. Use the OS pairing UI; no automatic numeric confirmation.
   Persist success only after the protected endpoint is usable. Keep permission,
   cancellation, retry and account/device switches within the existing flow.
4. Deliver the existing cloud challenge/proof/credential transaction over the
   same approved connection when cloud voice is requested. Reuse registration,
   verification and protected binding; cloud failure must not erase the local
   BLE relationship or require another Add Device operation.
5. Migrate existing saved devices and implement explicit removal/bond cleanup.
   Local removal, cloud unbind and erasing recordings remain distinct operations
   with explicit consequences and failure reporting.

Do not claim that BLE encryption protects Wi-Fi/TCP audio transfer. Audit its
existing authentication/encryption separately; do not downgrade BLE safeguards
when switching transfer transports.

## Watch-Local Cloud Binding Recovery

The existing pairing page now exposes optional cloud authorization through
`Open(false)` and a separate two-step local binding removal through the same
controller and Application dispatcher. Local secure addition still uses
`Open(true)` and does not require either cloud action.

Removal requires the assistant to be disabled and recording, transfers and
assistant work to be idle. It inspects the protected binding on the existing
worker, then requires a physical confirmation for that exact attempt and stored
generation. It needs a storage reservation but no BLE/network lease, wall clock,
factory identity or HTTP. Pending assistant authorization is fenced and an idle
protocol must close before inspection is admitted.

The authorized NVS write commits a versioned removal tombstone. Cancellation
before write admission prevents the write; cancellation after admission cannot
claim to undo it. Storage errors remain uncertain, not successful removal.
Subsequent explicit cloud authorization uses the next generation. This does not
revoke server account access, delete native BLE bonds or recordings, erase the
factory signing key, or guarantee physical erasure of old flash cells. Those
separate operations must not be implied by the UI.

Desktop coverage includes 15 controller and nine Application removal scenarios,
including offline recovery, rejection, stale confirmation, recording/resource
exclusion, storage failures, cancellation and generation-safe rebinding. Actual
mobile, radio and encrypted NVS acceptance remains outstanding.

## Native Current-Peer Removal

`WatchAncsClient::ForgetControlPeer(peer_generation)` is a local-owner operation
for a currently authenticated, durably approved control phone. It rejects stale
or zero generations, provisional approvals, active pairing windows and concurrent
approval storage work. It is not exposed as an unauthenticated GATT command.

Admission immediately retires the physical link and invalidates control tickets.
The existing poll path first persists revocation of that exact fingerprint, then
resolves the SDK's stored address and requests `esp_ble_remove_bond_device`.
The stored address may differ from the live identity/privacy address. A changed
fingerprint or unstable native list prevents deleting a different bond.

SDK request acceptance is not completion. A matching successful callback must be
followed by a native list check showing the target absent. A 10-second timeout
retains the outstanding request fence until its callback or a confirmed complete
host stop. Late callbacks do not rewrite a reported failure as success. Reconnect
and new pairing admission remain fenced while deletion is in flight. A queued
approval revocation survives host stop, even if the SDK deletion never starts.

This API currently has 17 ASan/UBSan native fixture scenarios, plus the existing
notification, bond storage and pairing controller regressions. SDK radio and NVS
I/O remain fixtures; they do not prove real bond persistence or controller queue
drain behavior. Only the current phone is supported at this layer; saved offline
phone selection and all-phone reset are not implemented by it.

The existing watch pairing page now provides a two-step current-phone removal
through `PairingController` and the Application dispatcher. Confirmation pins
both the original attempt and native peer generation. The assistant must be off
and recording, transfer and authorization work idle. The operation reserves BLE
and storage, but needs no network, factory identity or cloud credential change.
It is not invoked implicitly by deleting a phone-list record.

Cancellation before native admission prevents removal. Once admitted, leaving
the page cannot undo revocation: the worker observes the native outcome and
waits for local I/O to finish before releasing its reservations. A native SDK
timeout remains a failure with a fence owned by the long-lived native client;
worker completion does not claim the SDK queue has drained. Late failure cannot
be presented as cancellation or successful removal. An idle cloud protocol,
cloud authorization, recordings and other phones remain unchanged. The phone
OS's own bond record requires a separate action in its Bluetooth settings.

Twelve controller and twelve Application scenarios cover this integration,
including stale/duplicate confirmation, cancellation before/after admission,
pending I/O, resource failures, recording handoff and failure reporting. English
and Chinese LVGL interaction/layout tests pass. These are desktop tests with
native I/O boundaries, not physical pairing or phone OS acceptance. Production
flags remain disabled; offline-phone selection and all-phone reset remain open.

## Acceptance Evidence

### Lower-Stack Source Audit (2026-09-15)

Inspected the local pinned ESP-IDF v5.5.4 source after the current overlay
regression passed. Paths below are relative to `components/bt/host/bluedroid/stack`.
These observations are source evidence, not a lower-stack runtime test:

- `gatt/gatt_api.c::GATTS_HandleValueNotification` validates registration and
  connection state, sets `auth_req` to NONE and builds the ATT notification.
  It is not an independent Audream control-authorization check. Retain the
  project BTA admission checks; protected characteristic permissions alone do
  not secure unsolicited outgoing notifications.
- `gatt/att_protocol.c::attp_send_msg_to_l2cap` forwards fixed-channel ATT by
  peer address. `l2cap/l2c_api.c::L2CA_SendFixedChnlData` rejects a missing or
  disconnecting link, then can enqueue into the fixed CCB transmit queue.
  It does not carry the project control lease or subscription epoch. A BTA
  admission check therefore does not prove post-enqueue revocation behavior.
- `l2cap/l2c_utils.c::l2cu_process_fixed_disc_cback` clears a normal fixed CCB
  pointer and releases it before notifying the upper layer. `l2cu_release_ccb`
  frees its transmit queue; `l2cu_release_lcb` also frees the link transmit queue.
  Pending-CCB exceptions and the reconnect paths in `l2c_link_hci_disc_comp`
  require separate verification; do not generalize normal cleanup to every path.
- `gatt/gatt_utils.c::gatt_cleanup_upon_disc` closes the channel, clears pending
  encryption/prepared-write work and invokes registered disconnect callbacks
  before freeing its TCB. Controller/HCI queues and host-stop ordering are not
  established by this source review.

Next acceptance must distinguish data rejected before admission, data admitted
but still queued during revocation, and data already sent to the same approved
peer. Exercise unsubscribe/resubscribe, disconnect with pending work, address
and connection-ID reuse, host restart and encryption loss. Do not claim sent
bytes can be recalled, or that a source-only audit has proved RF confidentiality.
No additional global SDK changes were made by this review.

### Offline Approved Phone Removal: Native Layer (2026-09-15)

`WatchAncsClient::InspectSavedControlPeers` reads approved native bonds on a
worker while disconnected, with notification collection and pairing disabled.
It returns stored addresses and opaque selection tokens, never native keys.
Protected NVS approvals and authenticated native key fingerprints must agree;
inconsistent lists, weak or malformed bonds, storage errors and uncertain writes
fail closed without returning a partial list. This is not a legacy-bond migration
or an enumeration of every unapproved OS bond.

Each selection expires after 120 seconds. Refresh, connection lifecycle changes,
host retirement and monotonic clock rollback invalidate the selection. Removing
one selected phone reuses the current-peer removal state machine: revoke its
approval, verify the same native fingerprint/address, request native removal,
check the matching SDK completion and verify absence. Neither a request accepted
by the SDK nor a local UI action alone counts as success. Other phones, cloud
authorization and recordings are not deleted.

Inspection shares the existing bond-I/O exclusion with recovery and removal.
A native test reproduced recovery starting inside an unfinished list read;
`PollControlBond` and new pairing-window admission now reject that busy state.
Lifecycle changes during inspection discard the result rather than publishing
choices belonging to an obsolete connection context.

`test_control_bond_native.sh` adds 16 saved-phone scenarios alongside the existing
17 current-phone removal scenarios. `test_control_bond.sh` separately checks list
filtering, protected/unprotected storage, native list errors, duplicate/oversized
lists, read races and uncertain writes. These desktop sanitizer tests use real
client/storage logic with SDK/NVS I/O fixtures, not actual radio or persistence.

The saved-phone list is now wired to the existing physical pairing page,
`PairingController` and Application dispatcher. Inspection requires the assistant
and notifications to be off, no current BLE connection, and idle audio/transfer
resources. It retains BLE/storage leases without requiring Wi-Fi, a trusted wall
clock, factory identity or HTTP. It never disconnects a peer just to open the list.

Rows identify native stored addresses; no phone name is invented from a bond.
Selection pins both the rendered operation attempt and opaque native token, then
opens a separate confirmation naming that address. Empty lists have no removal
button. The controller rechecks selection validity before confirmation and the
native worker rechecks the original bond at removal. Cancel/leave before admission
prevents deletion; after admission the existing irreversible removal pipeline owns
the result. A recording handoff keeps its newer audio generation when the old
pairing worker releases resources. Terminal results clear the displayed list.

`test_pairing_controller.sh` adds 13 controller and 11 Application scenarios:
inspection cancellation/join, unavailable resources, stale/expired selections,
rejection, storage failure, pending native I/O, queued-open cancellation and recording
handoff. Native bond I/O is a fixture in these controller tests and is separately
tested above. English and Chinese desktop LVGL tests cover the 32-entry list,
scrolling, explicit confirmation, sleep blocking, queued-open cancellation and
empty state. Screenshots are under `/tmp/audream-watch-saved-phones-shots` and
`/tmp/audream-watch-saved-phones-zh-shots`. These are not phone or target tests.

Full bond reset, legacy migration and phone-side OS bond cleanup remain separate
work. `kEraseAndRemove` remains a simulated legacy command and must not be
presented as a real secure erase implementation. Production flags remain off.

### Pending-Reconnect Buffer Cleanup Implemented (2026-09-15)

The source-audit exception above now has a reproducer. The actual reviewed
`l2c_link_hci_disc_comp` retains fixed CCB transmit buffers when a pending LE
reconnect succeeds, although it clears the link-level queue. Running the actual
`l2cu_get_next_buffer_to_send` after changing the link handle selects an old
buffer for header construction. Header output and RF transmission are fixtures;
this demonstrates stale data eligibility, not an observed radio disclosure.

The experimental pinned overlay now adds a seventh source, `l2c_link.c`. On
physical LE disconnect completion it drains fixed-channel transmit queues and
the link transmit queue before any pending reconnect is started, and clears
the emptied channels' congestion flags. It leaves queue objects and pending
connection metadata for the SDK's existing reconnect/cleanup path. No success
callback is synthesized for discarded data. BTC/BTU producer credit counters
are not decremented again; those queues have separate ownership.

This cleanup applies to fixed LE channels in the experimental firmware, not
just BLUFI payload bytes: a completed physical connection cannot preserve queued
ATT/SMP bytes for another connection. BR/EDR branches are unchanged, and default
profiles do not replace this SDK file at all. The developer's global SDK remains
unchanged. A changed lower-stack source hash is explicitly rejected by tests.

`scripts/test_blufi_lower_stack.sh` compiles the original and patched disconnect
functions, the original CCB cleanup/fixed disconnect functions and the actual
send selector with ASan/UBSan. It checks normal and failed reconnect cleanup,
pending successful reconnect, buffer eligibility under a replacement handle,
absence of queued bytes before reconnect initiation, classic-transport isolation,
null queues, duplicate disconnect and balanced frees. SDK structs are reduced
fixtures and the outer LCB release is a fixture: this is not target ABI, complete
stack teardown or HCI/controller acceptance. The baseline intentionally reproduces
the old defect; it is not included in target firmware.

Remaining work includes post-admission revocation before physical disconnect,
native lifecycle callbacks on retained CCBs, controller queue/host-stop ordering,
and real mobile/RF verification. Keep production secure pairing disabled until
the complete acceptance criteria are met.

- Both mobile platforms: new device, previously saved device, approved bonded
  reconnect, wrong peer, rejected/mismatched code, expired confirmation, stale
  callback, interruption and permission denial. No false added/ready state.
- Firmware: control and provisioning writes rejected before authentication;
  no outgoing audio, events, credentials or stored response leaks before secure
  admission, including early CCC writes and reconnects.
- Offline addition works; refusing notifications/cloud voice does not prevent
  local recording and secure sync. Cloud verification still requires explicit
  account approval and cannot treat a BLE bond as account ownership.
- Recording/sleep/radio teardown retain existing ownership rules. Cloud-window
  cleanup does not disconnect an otherwise authorized control session.
- Real iOS/Android and watch verification, including a packet capture and
  reconnect after reboot, is required. Desktop fixtures alone do not establish
  RF encryption, bond persistence, OS prompt behavior or power consumption.

Desktop-tested watch runtime changes are described above; no firmware build/flash,
app installation or deployment was performed. The existing implementation ledger must
keep this migration open until both mobile platforms and firmware are connected
and verified.
