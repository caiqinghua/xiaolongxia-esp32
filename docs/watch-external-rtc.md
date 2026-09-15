# Watch External RTC

Status: source integration and desktop tests complete; target build and hardware acceptance pending.
Updated: 2026-09-15.

## Hardware and Scope

The Waveshare ESP32-S3 Touch AMOLED 2.06 schematic identifies a PCF85063ATL
on shared SDA GPIO15 / SCL GPIO14, with INT on GPIO39 and power from VCC-RTC
(AXP2101 RTCLDO). The recording-only profile and other board types do not
instantiate this driver. See the [board schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06/ESP32-S3-Touch-AMOLED-2.06.pdf).

GPIO39 is not an ESP32-S3 RTC GPIO eligible for EXT0/EXT1 deep-sleep wakeup.
Keep the existing internal wake timer; this change does not arm the external
alarm or claim touch/wrist/external-RTC deep-sleep wake support.
See [ESP-IDF v5.5 sleep modes](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/sleep_modes.html).

## Time Ownership

- `DeviceTimeService` remains the wall-clock owner. Existing monotonic timer,
  stopwatch, alarm scheduler and recording timestamp backfill are reused.
- Boot first initializes that service. Only when it lacks synchronized time
  does the watch attempt external RTC restoration. The existing minimum valid
  timestamp still applies; a warm-boot software clock is not overwritten.
- A successful existing device time-sync command queues an RTC update on the
  Application dispatcher, reading the latest software time when it executes.
  It does not perform I2C inside the BLE callback or add a protocol command.
  Its response acknowledges software time sync, not RTC persistence.
- Failed writes keep a pending flag for the next explicit sync or sleep flush.
  There is no periodic polling or infinite retry loop.
- Sleep preparation flushes once, then suspends RTC I/O while draining any
  admitted transaction. Deferred writes fail without bus access and retain
  the pending flag. Successful peripheral restoration resumes access and
  flushes pending time. An unsuccessful restoration leaves access suspended.

## Calendar Validity

`WatchPcf85063` uses address 0x51 and a single burst for calendar reads.
`WatchRtcRuntime` serializes transactions and uses 100 kHz I2C with bounded
100 ms calls. Register definitions and STOP/OS behavior follow the
[NXP PCF85063A datasheet](https://www.nxp.com/docs/en/data-sheet/PCF85063A.pdf).

The driver accepts UTC dates in 2000-2099. It rejects oscillator-stop, stopped
clock, test/reserved control bits, malformed BCD, impossible dates and
inconsistent weekdays. It reads valid 12-hour values but writes 24-hour UTC.
It preserves oscillator capacitance and correction configuration, and does not
change alarm/offset registers or issue a whole-chip reset.

RAM register 0x03 is reserved as an Audream UTC-format marker (0xA1). An
unmarked legacy clock could contain local time and is not restored merely
because its date looks valid. An explicit phone sync initializes this marker.
The marker is a format indicator, not cryptographic authentication.

Writes invalidate the marker, stop the calendar, program all seven fields,
resume, and verify readback before marking UTC. A partial calendar write leaves
the clock stopped and unmarked. Readback errors report failure rather than
inventing persistence success. Failed reads leave the caller's output unchanged.

## Verification

Run `bash scripts/test_watch_rtc.sh` for desktop ASan/UBSan tests. Coverage:

- Every civil date in 2000-2099 against the host UTC calendar, leap days and weekdays.
- UTC bounds, subsecond truncation, 12-hour AM/PM and invalid register states.
- Failure at each programming stage, partial writes and corrupt readback.
- Bus configuration, failed initialization, serialized concurrent writers,
  suspended I/O rejection and resume.

SDK I/O uses fixtures. These tests do not compile an ESP image or prove physical
oscillator stability, battery-backed retention, I2C timing or deep-sleep current.
On-device acceptance must cover initial sync, cold/warm boots, RTC power loss,
interrupted writes, sleep cancellation during sync, recording timestamp recovery
and unchanged physical long-press recording. No firmware flashing is performed
by the test script.
