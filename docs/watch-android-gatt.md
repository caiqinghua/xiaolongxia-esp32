# Native Android Notification Service

## Integration State

`WatchAndroidGattServer` registers an AWN1 service on the existing Bluedroid host.
The build option `AUDREAM_WATCH_ANDROID_NOTIFICATIONS` defaults off and requires
the combined watch profile's ANCS/shared-peer implementation plus GATTS. BLUFI
host start/stop and Application notification opt-in/opt-out/Poll are wired under
this gate. It does not start a second radio, scan, advertise independently or
change the original BLUFI characteristic permissions. Recording-only and StickS3
profiles are not changed.

The local ESP-IDF v5.5.4 source uses internal `BTA_GATTS_AppRegister` with
`blufi_profile_cb` for BLUFI in
`components/bt/common/btc/profile/esp/blufi/bluedroid_host/esp_blufi.c`.
The new service uses public `esp_ble_gatts_register_callback`/app registration;
it does not overwrite that internal BLUFI callback. This source inspection does
not replace real coexistence, discovery/cache timing and radio teardown tests.

**Not production ready:** the server now signs short-lived device-owner proofs,
and the application task verifies them with a pinned key and protected binding
before calling `ApplyOwnerAuthorization`. The Android GATT adapter/runner now
exists with an experimental settings caller and process runtime. Android data is
connected to the common watch notification view; real-device acceptance remains incomplete.
The setting remains default off and
the additional service is also build-gated off; pairing alone cannot admit AWN1
content. Keep these distinctions when reporting progress or shipping a build.

## Attributes

UUID namespace: `a616ac71-9884-4d4a-9371-422ca13c4100`.

| Suffix | Role | Properties | Permissions |
| --- | --- | --- | --- |
| `4100` | Primary service | Discoverable | Public declaration |
| `4101` | Hello and status | Read, write with response | Public protocol-selection write; read requires encrypted MITM |
| `4102` | AWN1 data | Write with response | Encrypted MITM |
| `4103` | Applied replies | Indicate | No public content read |
| `2902` after reply | CCC | Read, write | Encrypted MITM |
| `4104` | Binary server owner proof | Write with response | Encrypted MITM |

Application responses are used for values and CCC. Declarations use stack auto
responses. No write-without-response property, prepared/execute write buffering,
long-read offset assembly, phone actions or private-content logs are provided.
Bad prepared writes cannot accumulate a second unbounded snapshot buffer. The
receiver checks the exact GATTS interface, connection and peer address in direct
SDK callbacks; owner results additionally carry a monotonic local generation.
Do not introduce deferred callbacks that resolve an old address against a newer
generation at execution time.

## Handshake

1. Android connects to the normal watch/BLUFI advertiser and discovers this
   additional service. Reserve a positive wire session ID without granting
   notification collection. The Android runner uses `reserveLink`, not a premature
   `beginAuthenticatedLink` call.
2. Before confirming the watch peer prompt, write nine bytes to Hello: opcode
   `1` followed by the proposed unsigned little-endian 64-bit session ID, in the
   positive signed-64 range. This is a public protocol-selection operation only.
   It is accepted only for a protected, loaded device binding and the current
   enabled peer in the shared client's `PeerConsent` state, once per physical
   connection. It captures the current local binding generation and invalidates the older
   confirmation token so an old prompt cannot approve a new mode.
3. The watch asks for phone consent, then requests encryption directly for the
   selected Android peer. Android is the GATT client for AWN1; it negotiates the
   MTU. The watch does not open an ANCS virtual connection, request a duplicate
   MTU exchange, discover an Apple service, or require an Android ANCS emulator.
4. The existing shared GAP handler requires Secure Connections, MITM and bonding.
   Numeric comparison still needs explicit user confirmation; weaker/failed
   authentication or lost security cannot produce a peer lease. Unsupported
   passkey/OOB paths are rejected. Non-Android connections retain the original
   ANCS flow. User-facing pairing text now says phone, not iPhone.
5. After pairing, Android negotiates MTU at least 185 and subscribes to confirmed
   reply indications with CCC bytes `02 00`. Reads and CCC writes must come from
   the same authenticated peer lease. A smaller MTU cannot authorize delivery.
6. Hello reads return 74 bytes: version `2`, owner-authorized flag (`0` or `1`),
   the proposed 8-byte session ID, a fresh 32-byte device challenge generated
   using `esp_fill_random`, then a 32-byte reverse device-possession MAC (all zero
   until verified). The challenge is
   not a credential or proof by itself. Repeat reads do not rotate it or extend
   the authorization window. Long reads/offsets are rejected.
7. Android obtains the signed-in owner's grant from
   `POST /v1/devices/{device_id}/notifications/owner-grant`, including a separate
   fresh 32-byte phone nonce as `peer_challenge`. It sends the phone nonce followed
   by the decoded binary proof on `4104`, never the API's expected reverse MAC.
   Write acknowledgements mean queued fragments, not verified ownership. At most
   638 bytes and one proof envelope are buffered; a fixed
   five-second deadline covers assembly and application. The application task
   reserves storage/power, loads the protected identity, verifies the server RSA
   signature with Mbed TLS, and applies only the captured `OwnerRequest` (local
   generation, wire session, peer lease/address, challenge and binding generation).
   Storage serialization excludes local rebind during verification/application.
   The watch computes the reverse MAC using its current bound credential under
   the same storage lock. The Hello flag becomes `1` only after this succeeds.
   Android checks the exact session, challenge and expected MAC in constant time
   before allowing notification collection. A flag alone cannot authorize it. See the complete
   [server proof format and trust provisioning](../../../server/docs/device-notification-owner-grants.md).
8. A verified denial or later revocation can call `RevokeOwnerAuthorization` with
   the captured request. It clears the selected session and requests physical
   disconnect. A stale grant/revoke cannot affect a replacement request or peer.

The Android adapter must not reuse the existing unencrypted BLUFI custom channel
for proofs or credentials. It must also bind the phone's signed-in account and
explicit package consent, reconcile persisted owner/device state, and provide
revocation when accounts or device ownership change. A Bluetooth bond authenticates
the physical peer; it does not authenticate the Audream account or Android app.

## Delivery and Retirement

Data uses [AWN1](../../../android/docs/watch-notification-wire.md). Only the
authorized session with a current secure peer lease and CCC indication subscription
can deliver it. Each ATT write value is at most `min(512, MTU - 3)` bytes. The
minimum negotiated MTU is a policy boundary, not a measured throughput guarantee.
Five-second transactions and maximum-size snapshots still need radio measurements.

One applied response is in flight at a time. The service queues an indication
only after the full snapshot validates and its write response is successfully
enqueued. A failed indication enqueue or confirmation clears/retires the session.
The indication is not marked complete merely because it was queued. Missing
confirmation has a five-second deadline. A protocol heartbeat uses the same
application ACK path and renews the shared peer lease only after application.

`ESP_GATTS_RESPONSE_EVT` has a handle/status but no connection/transaction ID.
It must not be treated as a current-session response when it could belong to an
older connection. The service checks synchronous enqueue results, uses the
single outstanding indication confirmation, and enforces session expiry instead
of retagging that anonymous event. No tests claim asynchronous stack delivery
success from `esp_ble_gatts_send_response` returning `ESP_OK`.

The owner handshake is bounded to sixty seconds from Hello. Once admitted, the
protocol's thirty-second idle lease and fixed five-second receive deadline apply.
The independent owner grant expires within 300 seconds of challenge creation;
no notification or heartbeat extends it. Encrypted Hello opcode `2` plus the
unchanged wire session creates a fresh challenge for renewal, resetting its flag
to `0` without discarding the existing list. A new valid grant preserves the
receiver session/read/clear state. An incomplete renewal does not extend the old
owner deadline. Renew before expiry; a stale result cannot revoke or authorize
the replacement challenge. The API denies new grants after unbind, but an already
issued offline grant remains bounded by its original lease. Local binding change
invalidates pending and admitted requests on the next callback/Poll/read.
Opt-out immediately clears memory without SDK calls under a UI lock. Poll or
the next native callback retires the selected physical link outside that lock.
Disabling an unused AWN1 service does not disconnect a phone using ANCS. Host
stop, peer-security loss, protocol/confirmation failure or CCC removal retires
an admitted Android session. No old content is persisted, resumed or uploaded.

The native callback holds its mutex while consulting the shared peer client;
the shared client never calls back into this GATTS service while holding its
mutex. Host and Application hooks call the two owners sequentially. Owner proof
verification releases the GATTS mutex before acquiring the binding storage mutex;
the bounded verified application then takes GATTS followed by the peer mutex.
GATTS callbacks consult only the memory-only binding generation and never read
NVS or acquire its storage mutex. Preserve this lock order in UI/Android integration.

## Shared Watch View

Application builds the visible notification snapshot from the current peer
client and native Android server, without changing either protocol's state.
Android content requires an enabled shared client in `kAndroidAuthorizing`, a
ready receiver session, and an exact match between the client's physical link
generation and the server's validated peer lease. A missing or mismatching lease
produces no private content. iPhone content still requires a ready ANCS session
and a non-Android peer. Projection moves text into one presentation vector rather
than retaining an additional raw ANCS copy in the view state.

The shared list and detail support app/title/message, read state and local clear.
Actions capture an explicit `kAncs` or `kAndroid` source plus session, UID and item
revision. Application dispatches only to that source; missing source is not
guessed from the current phone. Android open marks the item read locally and
does not request an ANCS app name. Clear never dismisses a phone notification.
Selected details also retain source, so equal IDs from different platforms do
not retag private text. Disable/disconnect/revision updates purge rendered details
even while the screen is hidden; they do not wake it.

Android's UTC epoch timestamp uses `WatchLocalTime` and the configured watch
timezone; changing that setting rebuilds an open detail. Invalid/unavailable
timestamps are omitted. ANCS retains its phone-local date without guessing a UTC
offset. Long-text scrolling and literal-text rendering are shared with iPhone.

## Verification

`scripts/test_watch_notifications.sh` builds the actual receiver, GATTS server
and shared security/ANCS client against the installed ESP-IDF public headers,
with ASAN/UBSAN and replacement SDK I/O/clock/RNG. Five added native test groups
cover unpaired/unowned denial, stale grants/revokes, mode-specific pairing,
security downgrade, chunked atomic application, confirmed replies, read/clear,
MTU boundaries, unsupported writes, deadlines, opt-out, registration/service/
response/indication failures, old-peer events and iPhone isolation. The existing
ANCS protocol/client tests are retained. Native Android tests also project the
actual received list/read state and check peer lease mismatch and retirement.

`scripts/test_watch_ui.sh` renders actual LVGL views in English and Chinese with
injected snapshots. Android coverage includes authorization/peer mismatch,
timestamps and timezone changes, explicit source in read/clear requests,
equal-ID platform switches, empty lists and hidden disconnect cleanup. The
existing iPhone consent, UID-zero, long-text and privacy tests remain in place.
Application dispatch is source-reviewed, not executed as a full firmware task.

That fast test substitutes a rejecting crypto boundary. Root script
`scripts/test_watch_notification_owner_contract.sh` separately runs real API-issued
RSA 2048/3072/4096 proofs through the actual Mbed TLS verifier, including corrupted
fields/signatures, every truncation, wrong device/key/nonce, signed invalid leases
and RSA-PSS rejection. It also compiles the real GATTS application verification
path with the pinned ephemeral public key, proving admission, renewed-grant
acceptance without list reset, stale-proof rejection and binding invalidation.
NVS and SDK I/O remain test boundaries; binding tests separately exercise real
storage code and mutual exclusion with removal. No production private keys are used.

This is not an ESP firmware build or an over-the-air test. Actual Blufi/Application
host hooks, SMP prompts, trusted device/key provisioning, discoverability/cache invalidation, Android
process/service lifecycle, radio coexistence, peak memory and standby power remain
integration/device acceptance gates. UI tests use injected snapshots and do not
exercise the real account verifier or Bluetooth controller.
