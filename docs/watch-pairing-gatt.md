# Watch Device Pairing GATT v1

## Integration Status

`WatchPairingGattServer` runs on the existing BLUFI Bluedroid host behind
`AUDREAM_WATCH_DEVICE_PAIRING`, default off. It requires the combined watch,
shared native security client, GATTS and protected NVS configuration. Notification
collection remains an independent opt-in; it is not required to pair a device.
Host start/stop and application polling are wired under this gate. The common
GATTS dispatcher permits this service and AWN1 to coexist without replacing one
another's callbacks or changing BLUFI's internal profile callback.

This is **not a complete pairing flow**. The existing `PairingController` now
combines signing, HTTPS verification and checked storage on a joinable worker.
A desktop-tested physical confirmation page projects its public state.
Build-gated Application resource hooks and owner dispatch are connected; mobile
adapters and delivery recovery remain pending. Merely enabling the build option
does not provide an end-to-end mobile registration flow. The GATT server contains no key access,
network requests, storage writes or private logging. Host tests do not prove
real BLE timing or production readiness.

This is the cloud-binding transaction transport, not a second Add Device flow.
Follow [secure-device-addition.md](secure-device-addition.md) for the user's
requirement to encrypt ordinary device addition/control as well. The current
cloud window depends on Wi-Fi; offline BLE pairing must not inherit that gate.

## Attributes

Namespace: `a616ac71-9884-4d4a-9371-422ca13c4200`.

| Suffix | Role | Properties | Permissions |
| --- | --- | --- | --- |
| `4200` | Primary service | Discoverable | Public declaration |
| `4201` | Hello / select pairing purpose | Read, write with response | Public |
| `4202` | Bounded challenge / credential fragments | Write with response | Encrypted MITM |
| `4203` | Processing state / detached proof | Read | Encrypted MITM |

Negotiate an ATT MTU of at least 185 before accessing values. There are no
notifications, indications, CCC, prepared writes or read-offset assembly in v1.
Read offsets, oversized writes and prepared/execute writes are rejected. All
input must use writes with response. Native callback admission checks interface,
connection, address and the current secure pairing lease; a wire request ID
is only correlation, never authentication.

## Local Window and Hello

The physical application controller must load and validate the protected factory
identity, open both signing and native security windows, then call
`OpenLocalWindow(security_window, canonical_device_id)`. Failed setup must close
all acquired windows. Native security window IDs are monotonic within the process;
the transport validates the exact current generation. No remote opcode opens it.

Hello is exactly 46 bytes:

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 1 | Version `1` |
| 1 | 1 | `1` when a local window is open, otherwise `0` |
| 2 | 8 | Local window generation, unsigned little-endian; zero when closed |
| 10 | 36 | Canonical device UUID ASCII; zero-filled when closed |

Select the pairing purpose by writing nine bytes to Hello: opcode `1`, followed
by the observed local window generation as unsigned little-endian 64-bit. This
only selects the current phone's purpose. The watch still requires local peer
consent, Secure Connections/MITM/bonding and numeric comparison through
`ConfirmDevicePairing`. The notification confirmation API cannot approve it.

When the same live physical peer already has independently approved control
authorization, selecting a new cloud window reuses its authenticated link without
another BLE numeric comparison. This does not skip account/challenge approval.
`AuthorizeControlPeer` is local-only and is not exposed as a GATT opcode or
implicitly called on cloud success. Closing that window preserves approved
control; without that separate approval, existing disconnect behavior remains.

## Input Frames

Each `4202` write starts with this 14-byte header, followed by a nonempty chunk:

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 1 | Version `1` |
| 1 | 1 | Kind: `1` challenge, `2` credential |
| 2 | 8 | Positive request ID, unsigned LE; maximum `2^63 - 1` |
| 10 | 2 | Total payload bytes, unsigned LE |
| 12 | 2 | This chunk's byte offset, unsigned LE |
| 14 | Remaining | Payload bytes |

The full write must fit `min(512, MTU - 3)` bytes. The first fragment starts at
zero. Further fragments must keep the same kind, request and total and exactly
continue the previous offset; duplicates, gaps, overlap and overflow fail the
window. A fragmented payload has a ten-second deadline measured from its first
fragment, not extended by traffic. The enclosing local/security windows never
extend beyond 120 seconds. Only one payload exists at a time, at most 1024 bytes
for a challenge or 128 bytes for a credential.

1. After the secure handshake, send the API challenge's exact UTF-8 payload as
   kind `1`. Do not reserialize it. The transport only bounds and queues bytes;
   `PairingSession` remains responsible for schema, purpose, UUID, nonce and
   expiry validation. The application takes it once with `TakePending`, presents
   the account challenge for physical approval, signs through the protected
   signer, then calls `PublishProof` with the captured request.
2. Read `4203` until the same request reaches `proof_ready`. The phone submits
   that signature to the existing registration API. Bluetooth write success or
   an intermediate processing state is not registration success.
3. After registration, send the returned raw `adv_...` device credential as kind
   `2`, with the **same** request ID. It is accepted only after proof publication.
   The application takes it once, verifies it with `Client::VerifyPairing` against
   the trusted API and exact approved device/user/challenge, commits through checked `SaveBinding`, and
   calls `BindingCommitted` only after durable success. The controller worker is
   connected through Application; the transport alone cannot perform or pretend
   to perform these steps.

Credential verification uses `POST /v1/devices/{id}/pairing/verify`, not bootstrap.
Verification is read-only and leaves pending registration recoverable when local
storage fails. Bootstrap may activate only after a durable binding exists.
The HTTPS client and API receipt check are implemented; the controller must still
enforce the current local window/peer/approval and hold the power/storage lease
through verification, commit and worker cleanup.

The worker owns the returned payload, must not log or persist temporary secrets,
and must erase credential buffers after use. The transport overwrites its buffer
when handing off, canceling or resetting. Reads never return a credential.

## Status and Proof

`4203` returns exactly 74 bytes: version `1`, one-byte stage, eight-byte unsigned
LE request ID, then 64 proof bytes. Proof bytes are nonzero data only in stage
`proof_ready`; all other stages zero-fill that field. Re-reading a proof is
allowed within the same live window. This does not bypass the API's single-use
challenge protection.

| Stage | Meaning |
| --- | --- |
| 0 | closed |
| 1 | awaiting peer selection |
| 2 | authenticating / waiting for challenge |
| 3 | receiving challenge |
| 4 | challenge queued |
| 5 | challenge processing / local approval pending |
| 6 | proof ready |
| 7 | receiving credential |
| 8 | credential queued |
| 9 | credential verification / durable commit pending |
| 10 | durable binding reported by the application worker |
| 11 | one-shot credential write admitted; not yet a successful binding |

Status itself requires a current authenticated peer lease. Stages 0/1 cannot be
used as a public permission probe; use Hello to check window availability.
The `bound` state must not enable the assistant, start recording, create a Note,
or grant notification access. Those permissions retain their existing controls.

The worker calls `AdmitCredentialCommit` from `SaveBindingAuthorized`'s final
storage guard after HTTPS verification. Admission checks the live window and
secure peer without issuing BLE I/O, and can succeed only once. Only a successful
NVS write/commit may be followed by `BindingCommitted`. Disconnect or cancellation
after admission cannot undo a durable write; the application must retain that
outcome even when the final status cannot reach the phone.

## Cancellation and Recovery

Application results carry the window, physical peer lease, wire request ID and
kind. Old-window results are rejected; an old challenge rejection cannot cancel
the later credential-processing phase. Cancel, disconnect, host shutdown,
security loss, invalid framing or timeout erase queued data and proof and revoke
the native pairing window. A closed/unselected service does not disconnect an
unrelated ANCS/BLUFI connection. No callback retains an unbounded prepared-write
buffer or performs filesystem/cryptographic work.

The application controller still needs recording/sleep/intent cancellation and
durable delivery recovery. If registration succeeded but credential delivery was
lost, the phone must follow the API's pending-registration recovery with a fresh
physical proof. Do not report success based only on a write acknowledgement or
blindly replay credentials after a reconnect. An already active device requires
the documented explicit unbinding path.

## Verification

`scripts/test_watch_notifications.sh` compiles both native GATT services, the
shared dispatcher and the real peer security client against ESP-IDF public
headers. It replaces only native I/O/clock and controlled worker results.
Tests cover exact framing and queue handoff, intermediate versus committed
status, private-read gating, stale results, cancellation, fragment deadlines,
malformed/prepared writes and service callback coexistence. Existing notification
tests run alongside it. The separate signing/API contract tests validate actual
C++ cryptography; the GATT test's controlled proof is not a cryptographic test.
