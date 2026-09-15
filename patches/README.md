# Audream Transport Overlay

`esp-ml307-3.6.4-lifecycle.patch` targets the reviewed files from the locally
resolved `78/esp-ml307` 3.6.4 component. Original SDK sources and their license
remain in `managed_components`; this directory stores only Audream's changes.

The root CMake configuration enables the overlay only when
`CONFIG_AUDREAM_WATCH_UI` is set. Original recording-only builds do not copy,
patch, or redirect the transport component.

For enabled builds, `cmake/audream_transport_overlay.cmake`:

1. Verifies the SHA-256 of every file the patch changes.
2. Copies component sources, headers, and license into
   `<build>/audream_components/esp-ml307`.
3. Applies the patch with recounted hunks and verifies that all targeted files
   changed. Git discovery is stopped before the enclosing project worktree.
4. Redirects the entire component target's sources and include directories to
   the copy. Redirecting only a few implementation files would mix incompatible
   class definitions between constructors and their callers.

Do not update hashes merely to make a dependency upgrade configure successfully.
Review the upstream lifecycle implementation, regenerate the patch if needed,
and rerun the tests first. Never edit the downloaded managed component to bypass
the guard. The short-context patch is only safe with its exact-source checks.

## Host Verification

```sh
bash scripts/test_transport_lifecycle.sh
bash scripts/test_watch_runtime.sh
```

These scripts do not invoke ESP-IDF or produce firmware. The first configures a
host CMake target in both enabled and disabled modes, repeats enabled
configuration, and compiles the patched SDK TCP/TLS/WebSocket implementation
against simulated FreeRTOS, sockets, and TLS. It exercises real task overlap
using host threads and checks it with AddressSanitizer/UndefinedBehaviorSanitizer.

## Checked Close

`Tcp`, `EspTcp`, `EspSsl`, and `WebSocket` expose `DisconnectWithin(timeout_ms)`.
A false result retains the descriptor, receiver task, TLS context, and callback
storage. The owner must retry and must not reconnect or destroy the transport
until closure succeeds. Zero polls without waiting; `UINT32_MAX` preserves the
legacy indefinite join. Unsupported transports return false instead of claiming
that resources were released.

In the combined watch profile, `WebsocketProtocol` retires the old receive
session before requesting closure. A pending close rejects sends and reconnects
and is retried from the application's main-task clock tick. The original profile
continues using the legacy API; host protocol tests compile both profiles.

## Receive Limits

The patched receiver consumes incoming TCP chunks incrementally. It limits the
HTTP upgrade response to 8 KiB and individual frames and reassembled messages to
64 KiB. Several complete messages in one TCP chunk remain valid. The logical
receive buffer is at most 64 KiB plus ten header bytes; the assembled message has
a separate 64 KiB limit. These are not a total heap budget: container capacity,
the TCP chunk, and the application's receive/decode queues also consume memory.

Advertised lengths are checked before waiting for their bodies or allocating
payloads. Invalid continuation order, reserved bits/opcodes, masked server
frames, oversized/fragmented control frames, and nonminimal lengths retire the
receive session. Ping may interrupt a fragmented message. Close terminates
delivery immediately, including later frames in the same chunk. The receive
task never destroys its transport; the protocol owner completes checked closure.

Framing checks follow [RFC 6455 sections 5 and 10.4](https://www.rfc-editor.org/rfc/rfc6455.html).
This is not a claim of complete RFC compliance.

## Remaining Constraints

The void `Disconnect()` API and final destructors still wait for task completion.
Destroying an object before its checked close succeeds can therefore block.
Destroying a transport from its own receive callback is unsupported. The
combined profile now marshals recording and sleep closure to the main-task
owner with generation/deadline checks, then drains local audio. Transfer
preemption, every final protocol-reset path, and the complete assistant lease
lifecycle still need integration; this is not full handoff certification.

The HTTP status line is checked, but `Sec-WebSocket-Accept` authentication and
other upgrade-header checks remain to be implemented. UTF-8 validation, close
status/reason validation, a full graceful close handshake, DNS/TCP connect
cancellation, and IDF/physical-network validation are also outstanding. Socket
send/pong operations can still block; receive limits do not bound those calls.
