# Watch Motion Integration

## Scope and Current Status

The Note Watch activity feature uses QMI8658 hardware step counting. A local,
default-off raise-to-wake policy is connected to the board and watch settings. Raw
motion data stays on the device. These modules do not
enable XiaoZhi wake words, AFE, cloud processing, or recording.

Implemented and desktop-tested:

- `main/watch/watch_qmi8658.*`: checked SensorLib initialization, pedometer start,
  counter/acceleration reads, and shutdown.
- `main/watch/watch_activity.*`: counter generations, bounded deltas, seven
  recently visited civil-date totals, undated totals, and an interruption flag.
- `main/watch/watch_activity_record.cc`: strict 64-byte checkpoint encoding.
- `WatchRaiseToWake`: away-to-view dwell, plausibility checks, cooldown, and a
  caller-controlled permission gate.
- `WatchMotionTask`: one joinable worker owning the sensor and checkpoint I/O,
  cached snapshots, recording/transfer suspension, and checked sleep handoff.
- `watch_motion_storage`: a single NVS checkpoint with checked reads, writes and
  commits, plus ordinary storage reservations and exclusive sleep ownership.
- An activity page showing today's steps, undated steps, interruption, paused,
  unavailable, sensor-error and storage-error states in English and Chinese.
- A scrollable retained-date history and non-destructive asynchronous storage
  retry, with completion supplied by the actual motion worker.
- Explicit confirmation and token-checked recovery of corrupt activity records,
  without clearing any other NVS keys.
- Screen-up calibration, an explicit raise-to-wake toggle, versioned NVS
  preferences, reboot restoration, and matching asynchronous save results.

The Note Watch board creates the worker under `CONFIG_AUDREAM_WATCH_UI`, publishes
its cached snapshot through `Application`, and waits for it before releasing
peripherals for deep sleep. Motion sources are conditional on that watch option;
the pinned SensorLib manifest dependency applies to ESP32-S3 targets. Existing
StickS3 behavior and recording button mappings are not changed.

**Still pending:** physical calibration/gesture/step/power validation and
deep-sleep wrist interrupt support. The firmware
target has not been compiled or flashed with this integration. Desktop tests do
not establish on-device step accuracy, native I2C behavior or a measured power
budget. The source now enables the pedometer when the watch worker is running;
this is not a claim that a device is already running this version.

## Reviewed Driver Source

The [Waveshare example](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/tree/main/examples/arduino/04_LVGL_QMI8658_ui)
uses SensorLib. This implementation reviews the newer driver, pinned to
[SensorLib f3cc656a71ea9500f12ecbd4c4078ce9bd0282ea](https://github.com/lewisxhe/SensorLib/tree/f3cc656a71ea9500f12ecbd4c4078ce9bd0282ea).
The adapter explicitly includes `sensor/imu/qmi8658/SensorQMI8658.hpp`, not the
different legacy `src/SensorQMI8658.hpp` interface.

The driver is not trusted to report all nested errors:

- `getStepCount()` returns zero on a failed read. The adapter instead checks the
  three-byte register read before publishing a new snapshot.
- Several setup methods ignore nested enable/disable return values. A transport
  decorator latches any failed register operation for the entire operation.
- `writeCommand()` can reach ACK after its first completion poll times out. The
  decorator requires an observed command-complete status before accepting ACK.
- `SensorHal` is deleted through a base pointer but lacks a virtual destructor.
  `patches/sensorlib-qmi8658-lifecycle.patch` fixes that lifetime issue and the
  custom transport's missing `override`. `cmake/audream_motion_overlay.cmake`
  checks reviewed source hashes and applies this patch to an isolated build
  overlay, redirecting both sources and public headers. Native adapter builds
  require the overlay's transitive compile marker; managed sources stay pristine.
  The dependency manifest has passed the local Component Manager parser, but its
  lockfile has not been regenerated through a firmware configure/build.

The initial pedometer profile follows SensorLib's 112 Hz six-axis runtime, with
its default walking thresholds rather than the shake-test thresholds. It uses
4 g acceleration and the API's 1000 DPS gyro range. This is **not** a validated
low-power profile. Stop disables the pedometer, gyro, and accelerometer and
checks the enable registers. `Running() == false` alone is not proof that the
physical sensor has stopped; the power owner must check `Stop()`'s result.

## Accounting Contract

After a verified counter reset, call `BeginCounter` immediately with the driver's
new nonzero generation. A generation must increase, and the prior acquisition
must be paused before another begins. Old generations cannot add steps.

An observation interval is at most 30 seconds. Deltas allow 8 steps/second plus
32 batched steps; larger jumps rebase the counter without adding phantom steps.
The 24-bit wrap is handled, but an unknown hardware reset is never treated as
16 million steps. These bounds reject gross corruption, not false step detection
by the hardware algorithm. A small positive delta after an undetectable sensor
reset cannot be distinguished from actual steps without additional hardware
reset evidence.

The caller supplies the civil-date identity from `WatchCalendarAt` using the
selected watch timezone. Deltas that cross a date change or a clock correction
are retained as undated: the counter provides no per-step timestamps. Later
clock synchronization does not silently assign old steps to today's date.
Returning to a retained date reuses its total. The checkpoint's interruption
flag describes the retained collection, not a guarantee of complete daily
tracking. Totals saturate at `UINT32_MAX` instead of wrapping.

The record is 64 bytes, little-endian: version/count/interrupted/reserved at
0..3, undated steps at 4..7, then seven signed-day/unsigned-step pairs. Unused
pairs must be zero. Truncation, unknown versions, duplicate dates and nonzero
reserved bytes are rejected without overwriting the caller's checkpoint.

## Worker, Persistence and Sleep

One worker samples every 250 ms while active. Recording states other than closed,
temporary AP transfer, and deep-sleep preparation pause counting. A transition
normally takes until the next poll plus in-flight sensor I/O; this is not a
synchronous guarantee that the IMU stops before the first recording sample.
After a final counter read, the worker pauses accounting and verifies sensor
shutdown. During an ordinary pause it checks for resumption once per second;
that wakeup overhead also needs device power measurement.

`audream_watch/activity_v1` stores the complete 64-byte checkpoint. Changed
checkpoints are saved at most once per 60-second active interval and on pause or
sleep entry. An unchanged record is not rewritten. Ordinary storage errors use
a 60-second retry interval while paused; an explicit sleep request may retry
sooner. A power interruption may lose uncommitted steps. The storage helper checks
NVS commit, preserves outputs after partial reads, and closes every opened handle.
A failed commit may already have changed flash, so retries write the complete
idempotent record. Corrupt history is not silently overwritten: counting remains
unavailable with a storage error if re-reading still produces unusable history.
Permanent corruption requires the separate, user-authorized reset workflow below;
ordinary retry never erases or replaces corrupt records.

Normal NVS I/O takes the existing power manager's storage reservation. During
`Suspend`, the retained peripheral-sleep transition owns exclusive access instead.
The worker acknowledges sleep only after verified sensor shutdown and successful
checkpoint storage (or preserved, unusable history). It then parks without polling
or peripheral access. A failed shutdown prevents sleep even if software reports
not running. A sensor that cannot initialize can therefore prevent deep sleep;
hardware failure policy and battery impact still need acceptance testing.

`Resume` waits for actual exclusive I/O completion before the power owner can
release its recovery reservation. A timeout is not cancellation of native I/O.
LVGL callbacks only consume the cached snapshot and never perform sensor or flash
operations. Counter startup captures its monotonic time and civil date together
after the hardware reset, including when startup crosses midnight.

## History and Storage Retry

The activity page opens a history list showing up to seven actual retained dates,
newest date first. These are not necessarily seven consecutive dates: missing
dates are not filled with synthetic zero counts. Dates use the watch calendar's
stored `year * 366 + day_of_year` identity; formatting validates leap years and
the supported 2020-2099 range without reinterpreting history in the current
timezone. Invalid identities display an unknown-date label. The latest RAM
checkpoint is published atomically with the worker snapshot; a storage warning
means these totals may not yet be durable. Step updates do not rebuild the list
or reset its scroll position, and the last date/count is reachable by scrolling.
Deep sleep and recording pauses do not promise continuous daily counting; the
activity page displays the interruption state explicitly.

When storage fails, the activity page exposes `Retry storage`. The LVGL callback
queues a request through Application and the board; it never performs NVS I/O.
If the worker has valid accounting state, retry saves that RAM checkpoint instead
of reloading an older disk record over uncommitted steps. If initial history was
unreadable, retry re-reads it. An absent record can initialize new history, but a
corrupt existing record cannot be treated as absent. The single worker owns the
operation and reports pending/succeeded/failed with the original request ID.
The UI only completes a matching non-pending response; enqueueing is not success.
Only one retry can be outstanding, and repeated/stale IDs are rejected. Deep
sleep rejects new requests and supersedes queued requests before they begin.
Native storage operations already in flight still finish under their existing
storage reservation; a timeout does not authorize pin release or another writer.

## Confirmed Corruption Recovery

A malformed checkpoint or invalid stored blob length offers `Reset step history`.
Transport/read/open errors do not: they remain eligible for non-destructive retry.
The confirmation page names the irreversible loss of dated and undated local
steps and explicitly excludes recordings, alarms and other settings. Opening the
page or cancelling before submission does not enqueue a reset or write storage.

Confirmation carries the worker's current nonzero repair token. Re-reading the
record changes or clears that token; a stale confirmation cannot reset newly
recovered history. Reset and retry are mutually exclusive and have separate
matching completion IDs. Neither enqueueing nor an in-flight write is success.
Once submitted, the cancel command is disabled because native I/O cannot be
undone by navigating away. Back navigation is not withdrawal of an accepted reset.

The same worker re-reads the record before replacing it. If it is valid or absent
now, normal accounting is restored without deleting recovered steps. Another I/O
failure aborts the reset. If corruption is confirmed again, and the sensor has
been initialized and verified stopped, the worker may commit an empty versioned
checkpoint to `audream_watch/activity_v1` only. It preserves the interruption flag;
the new history must not claim complete tracking earlier that day. Recording,
transfer or pending sleep cancels a reset before the replacement starts. Ordinary
storage reservations still cover each actual read or write.

A failed replacement commit may have changed flash. It remains a failed reset,
disables further reset authorization and requires a fresh read via retry. The
worker neither counts against an uncertain checkpoint nor writes stale history
back over it. If retry finds a valid empty replacement, it restores that record;
if corruption persists, a new confirmation token can be issued. This is not a
secure erase of old flash pages, a whole-NVS format or a device factory reset.
Unrecoverable NVS partition/hardware errors still require device maintenance.

## Raise-to-Wake Contract

The caller supplies acceleration projected onto the calibrated screen-normal
axis, and the acceleration magnitude, both in g. Never assume QMI8658 Z points
out of this board's display without verifying the board revision and mounting.

The policy requires an away pose for 400 ms, followed by a viewing pose for
350 ms within 3 seconds. It enforces a 5-second cooldown and resets gesture
progress after a sampling gap over 500 ms, implausible acceleration, or a closed
permission gate. These are initial UI policy thresholds requiring wrist tests,
not a claim that every lift is recognized.

The worker closes the gate during recording, transfer, sleep preparation, invalid
readings, and user-disabled wrist wake. Only a completed, permitted gesture may
request a ten-second display window through the power manager's final resource
gate. Already-visible screens are not continually renewed, and storage activity
does not extend display time. Deep-sleep wrist wake additionally requires
verified IMU power and interrupt wiring; this module does not implement it.

## Wrist Settings and Calibration

Open Controls > Raise to wake. With no saved preference the toggle is off and
cannot be enabled before calibration. Place the watch flat with its screen up,
then select Calibrate. The worker requires at least nine stable samples spanning
two seconds, within a fifteen-second deadline. It derives the screen normal from
all three acceleration axes, without guessing a board-axis sign. Gaps, motion,
bad samples, or sensor-generation changes reset the sample window; a resource
pause or deadline failure ends the request. Successful calibration saves only
the normal, with raise to wake still disabled. Enabling is a separate user action.

`WatchWristControl` is owned by the motion worker. Application and the board
forward UI requests to it; UI callbacks only queue commands and read snapshots.
Loads and saves run outside its request mutex, so a cancel or disable need not
wait for NVS. Request IDs increase strictly; only a matching completed write can
acknowledge the current request. Leaving the calibration page or hiding the watch
cancels pending wrist work. Cancellation preserves the last acknowledged normal
but requests disabled state; it does not silently restore an earlier enabled state.

Disabling takes effect in RAM immediately. A durable acknowledgment requires a
successful write and commit. An older enable write already in progress cannot be
undone: its result cannot re-enable RAM or complete a newer request, and the next
worker poll writes the newer disabled preference. Power loss before that newer
commit may still restore the earlier disk value. A failed save is not success,
and the page offers Retry disabling to persist a safe off state. Invalid/failed
loads keep the feature off; unreadable initial storage retries after thirty
seconds, or the user can explicitly save a disabled state. Sensor pauses are
shown separately, and do not silently change the stored enabled preference.

The independent key `audream_watch/wrist_v1` is a strict 16-byte record: version,
enabled, calibrated and reserved bytes, then three little-endian signed 32-bit
components scaled by 10,000. Invalid flags, lengths, reserved bytes and non-unit
normals are rejected. NVS supplies blob integrity; the code never persists a raw
C++ struct. Read/write reservations, handle cleanup and commit checks use the
same storage discipline as activity data without overwriting `activity_v1`.

## Desktop Verification

Use a clean checkout of the pinned revision; the test script rejects a different
revision or dirty sources, copies the source into a temporary directory, applies
the lifecycle patch there, and runs the actual QMI8658 driver against a simulated
register bus. It does not compile firmware or change `sdkconfig`.

```sh
SENSORLIB_SOURCE_DIR=/path/to/SensorLib bash scripts/test_watch_motion.sh
```

The tests cover zero versus failed reads, complete-read publication, each
initialization/start I/O failure, command completion timeout, ignored power-down
writes, counter generations/reset/wrap/gaps, date boundaries, record corruption,
saturation, and blocked/repeated/stale gestures. Worker tests cover pause/resume,
failed shutdown and persistence, blocked I/O during cancelled sleep, restored or
corrupt history, a date change during sensor startup, non-destructive retries,
blocked retry completion and sleep superseding a queued retry. Reset tests cover
stale confirmations, blocked commits, read errors, already-recovered records,
ambiguous commits, recording suspension and sleep during the final recheck.
The storage test links
the actual helper against SDK NVS headers and controlled function implementations.
Overlay tests verify source hashes, repeatability, source/header redirection and
the native compile marker. These are desktop tests, not an ESP-IDF target build.

The activity and worker tests also run in `scripts/test_watch_runtime.sh`;
`scripts/test_watch_wrist.sh` checks calibration consent, record validation,
reboot restoration, failed loads/saves, and cancellation during blocked writes.
The native NVS helper tests verify wrist/activity key isolation and reservation,
read, length, write and commit failures. The LVGL suite exercises the real wrist
controller through the setting page, including pending save feedback, stale
completion, failed-disable retry, navigation/hidden cancellation and motion pause.
`scripts/test_watch_ui.sh` exercises the activity page using actual desktop LVGL,
including long step counts, seven-date scrolling and errors in both locales.
Its real pthread worker/actual LVGL combination also checks that a blocked load
stays pending, a failed load does not overwrite history, and a successful retry
displays the recovered checkpoint. Confirmed reset is also exercised against the
actual worker and LVGL, including cancel without writes, stale confirmation,
pending I/O, actual replacement completion, and control visibility with an alarm
banner in both locales. Storage is controlled in that desktop test;
it is not native board/NVS validation. The driver tests require the
pinned source checkout and never report a skipped dependency as success.

Remaining acceptance gates:

1. Validate the complete native board build after firmware build authorization.
2. Add persisted wrist settings and gesture-to-power integration after verifying
   axes and the supported awake/standby modes; do not invent deep-sleep IRQ wiring.
3. Validate confirmed activity recovery with actual corrupt NVS blobs, commit
   errors and resets on hardware; desktop fixtures do not prove flash behavior.
4. Verify steps, axes, gestures, shutdown/recovery, recording/transfer priority,
   long-press recording and power on the physical Note Watch after flash approval.
