# Watch Alarm and Timer Sound

## Behavior

The watch profile uses the persisted alarm/timer alert flags for its visual
banner. `Application` polls these flags after the time service and separately
requests local sound from `WatchAlertSound`. No network or audio file is needed.

An available audio session produces two 200 ms tones separated by 100 ms of
silence. Each tone has a 5 ms ramp. A chime may repeat every five seconds for up
to one minute from the event; the banner remains until acknowledged. If audio
is busy, the chime waits only within that window, not indefinitely. A new actual
timer/alarm event starts a new window, including an event using an already-set
alert bit. Restored pending alerts receive one new bounded window after boot.

The output uses the existing watch speaker volume, including mute. It never
raises the volume setting or applies gain to recordings. It does not open the
microphone, start AFE/wake word, decode Opus, read storage or connect to Wi-Fi.
The normal chime allocates one 20 ms mono PCM chunk (640 bytes at the watch's
16 kHz rate) and a 16 KiB worker stack, subject to the platform thread minimum.
These are requested sizes, not a measured native peak-memory result.

## Ownership and Cancellation

The player reserves the existing local-playback audio lease atomically, only
from idle. It cannot preempt recording, paused recording, recorded-file playback,
assistant sessions, transfer, maintenance, fault or sleep. Output preparation,
writes and checked close reuse `AudioService`'s watch output path. The lease
blocks deep sleep until actual native cleanup succeeds; no storage read lease
is needed because no file is opened.

Long-press recording can take responsibility for stopping this local output.
The recording handoff stops both the alert player and the recorded-file player
within one shared wait budget. A blocked native write/close is not cancellation
completion. The new recording must not start capture until cleanup succeeds;
old output cannot clear the new recording's generation. Recording preemption
also suppresses the old alert's remaining audible repetitions.

Acknowledgement silences the current sound request immediately, even if saving
the cleared alert flags later fails. Silence requests do not bypass native
drain. The banner shows `Stopping alert...` while audio remains active after
the time flags are cleared. Failed close retains a visible cleanup error and
the audio lease. Pressing the confirmation button retries cleanup, even if
calendar storage is unavailable or another UI command was pending. A stale
command receipt cannot complete a newer cancellation.

Preparation/write failure shows an unavailable-sound warning while preserving
the visual alert. A failed event does not repeatedly reopen the codec. Cleanup
failure waits for a stop/retry request; time-window expiry or clearing the alert
may request the first stop but does not constitute proof of release. The audio
worker is joined before captured hooks or application event handles are freed.
Unrecoverable native cleanup during object destruction is fail-stop, not a
successful hardware shutdown claim.

## Verification and Remaining Work

- `scripts/test_watch_alert_sound.sh` runs the real sound worker and audio
  coordinator under ASAN/UBSAN with controlled output hooks. Five groups cover
  waveform length/bounds/ramps, repeat limits, new events, busy states, deferred
  expiry, blocked writes, recording preemption, preparation/write/close faults,
  dismissal and invalid configurations at 16/48 kHz.
- `scripts/test_watch_ui.sh` covers the real LVGL banner, error/footer, pending
  stop, explicit retry and stale receipts in English and Chinese. It injects
  output snapshots; it is not a full Application or hardware execution test.
- Existing local playback output tests cover the shared AudioService and Box
  methods using driver/persistence substitutes. They do not prove real I2C,
  DMA, speaker loudness, mute audibility or power use.

Firmware compilation, ESP thread/native codec linking, actual wake-to-ring
timing, real long-press takeover and acoustic testing remain device acceptance
work. This adds audible local time alerts, not phone notifications, vibration,
ANCS or Android notification forwarding. The complete watch/assistant project
remains in progress; no firmware was built or flashed for these host tests.
