# Watch Recording Playback

## Current Components

`AudioSessionCoordinator` has independent local playback and paused playback
owners. They do not grant assistant capture/output permissions. Recording can
preempt preparing, active, paused, or failed playback; the transition retains
the interrupted purpose so the recording service cannot skip playback cleanup.

`LocalRecordingService::SetPlaybackHandoff` receives the new exclusive recording
generation and a shared cleanup timeout budget. Missing or failed cleanup blocks
capture and retains the fault lease. Explicit failed-start recovery also checks
that callback before assistant cleanup. A stale playback callback cannot release
the new recording owner. Application now binds this callback to the playback
worker on the Note Watch profile; registration failure disables playback.

`RecordingPacketReader` is a single-owner, pull-based file source using the
vendored Xiph.Org libogg 1.3.6 framing implementation. It validates Ogg CRC,
sequence, stream identity, continuation, Opus identification and comment headers.
It emits encoded packets, not PCM. It does not open a microphone, codec, network
connection, or speaker. The existing prompt demuxer is unchanged.

The reader is compiled only for the Note Watch UI board profile. Source identity,
archive hash and license are in `main/audio/demuxer/libogg/`.

`RecordingPlaybackDecoder` owns an independent ESP Opus decoder and decodes
directly at the speaker rate. It does not start wake-word, AFE, microphone capture
or a cloud assistant. `RecordingPlaybackTask` owns the actual file, decoder and
output on one joinable pthread. Start reserves the audio owner before creating
the worker; replay joins the previous worker before reusing its resources.

Application resolves a selected entry from the current catalog revision and
passes its validated filename and expected size to the worker. Recording,
directory scans and AP transfer cannot start a second playback session. The
worker acquires the storage/power lease before opening the file, not in the UI.
The recording detail provides explicit play. Opening a detail never auto plays.
The launcher has a separate playback page so stop remains reachable after
leaving the catalog. The page shows preparation, pause, stop, end and failures
from the worker snapshot rather than from command acceptance.

## Decoder, Output and Handoff

- Only mandatory Opus header gain is applied; no extra digital recording boost.
  Pre-skip may cross multiple packets. Standard EOS granules trim encoder padding.
  The exact legacy vendor `Audream firmware` selects decoded-sample timing instead
  of treating old input-rate granules as 48 kHz timestamps. This vendor marker is
  a compatibility hint, not authentication or proof of recording completeness.
- Pause drains output on the same worker before acknowledging paused state.
  Resume uses the same decoder, file and ownership generation. Nonblocking UI
  stop reports stopping while native operations return; it does not release the
  storage lease, open the microphone or claim completion early.
- Recording preempts the playback generation and waits for actual cleanup through
  `StopForRecording`. A timeout leaves the worker and resource lease intact.
  Decoder/output close failure enters a cleanup fault that requires explicit
  retry. Stale stop, resume or cleanup cannot release a newer recording owner.
- `AudioService` tracks its local output generation separately from assistant
  queues. Failed assistant quiescence during preparation remains owned; cleanup
  retries the drain before clearing that generation. Shared service shutdown,
  idle detection, assistant resume and the idle output timer respect this owner.
- The ES8311/Box playback path checks native I2S write status and byte count.
  It bypasses the SDK data wrapper that can report success during reconfiguration
  without writing data. ES8311 still uses its existing hardware volume setting;
  this is not an alternative software volume path for arbitrary codecs.
- Normal pause/end writes two DMA rings of silence before disabling TX to flush
  a partially filled current buffer and queued PCM. Silence is not counted in
  recording position. Drain uses a bounded 500 ms budget and checks each write;
  it is not a fixed sleep. Close attempts DAC close, TX disable/readback and PA
  low even if one step fails. Failed shutdown blocks new writes and can retry
  without trying to drain an already stopped TX channel.
- Live position counts PCM accepted by the output call and can lead acoustic
  playback by DMA latency. At normal end the drain must succeed before the end
  snapshot. The host FIFO model tests this contract, not physical I2S timing.
  SDK DAC open/close wrappers do not forward every register-level failure;
  actual DAC power, volume, mute and amplifier behavior still require device
  verification. TX/PA readback is not proof of all codec registers being correct.

## Player Volume

- The player has a 0-100 volume slider, including the existing zero-volume mute
  preference. It uses the same `audio/output_volume` setting as watch controls;
  it does not add a second volume setting or boost recorded PCM.
- A released slider submits its UI request ID and playback generation. Acceptance
  only queues one transaction. The playback worker executes it between PCM writes
  or while paused and publishes a separate completion receipt. The view matches
  both identities before acknowledging; repeated or superseded requests cannot
  acknowledge a different operation. The clock need not be synchronized.
- The worker retains audio and storage/power ownership throughout the hardware
  and NVS operations. AudioService rejects unprepared, stale, stopped or otherwise
  owned output. No volume change runs in the LVGL callback or enables a paused
  speaker. The stored preference is applied on the next output enable.
- The hardware setting is applied before persistence. An apply failure attempts
  to restore the old output setting. A save failure restores both output and NVS,
  including a potentially staged value after a failed commit. Successful rollback
  reports failure while playback remains usable. An unsuccessful rollback marks
  the volume unsafe and retires playback through its normal checked cleanup.
- Stop cancels a transaction that has not started. It cannot claim an in-flight
  hardware/NVS operation has been undone: the actual result is published, then
  output closes. Recording handoff waits for this closure; a timeout retains the
  old resources rather than opening the microphone across a live transaction.
- The SDK's `esp_codec_dev_set_out_vol` ignores `codec->set_vol` failures. The
  watch path now retains its SDK cache update and additionally invokes the actual
  ES8311 `set_vol` callback, requiring its I2C result. The default SDK curve is
  unchanged (0 maps to -96 dB; 1-100 use the -50 to 0 dB linear curve). The SDK's
  public conversion helper is reused; ES8311 still applies its own hardware gain
  compensation. This performs an additional register write, not software gain.
  Generic assistant and other-board volume paths are unchanged.
- Slider values are previews until completion. A failed save restores the shown
  persisted value and reports the failure. Dragging is not reset by clock ticks.
  Preparation, stopping, failed cleanup, missing preferences and unsupported
  volume backends disable the slider; stop remains reachable during a save.

## Resource and Error Contract

- The playback worker must hold the audio owner and storage lease through actual
  file, decoder and output closure. Cancellation requests do not release either.
- `Open` compares the actual regular-file size with the reviewed catalog size.
  The caller resolves a safe recording ID under storage's recording directory;
  the reader is not an arbitrary-path authorization boundary.
- `Next` clears the previous packet and returns one packet, pending, end, error,
  or cancelled. Continue after a packet's EOS flag to validate trailing data.
- Each call reads at most eight 2 KiB chunks or accepts sixteen Ogg pages before
  yielding. These are work limits, not a deadline on a blocked native SD call.
- Encoded packet/header reassembly is capped at 8 KiB before libogg page input.
  Large valid Ogg pages are supported; endless continued packets are rejected.
  Tests measure library buffer capacity below 200 KiB for maximum-page cases,
  and below 64 KiB for a multi-megabyte file written by the actual recorder.
  These figures exclude the decoder, output PCM, task stack and filesystem.
- CRC, sequence loss and unsupported stream/channels fail explicitly. There is
  no resynchronization that silently drops recorded speech. Mono mapping-family
  zero is supported, matching Note Watch recordings; chained and multiplexed
  streams are rejected. Metadata comments above the bound are not supported.
- Short reads, size changes and I/O failures are errors, never clean EOF.
  The size check is not a cryptographic identity check or filesystem transaction.
- Cancelled/error status is sticky until reopen. End can be rewound with an
  opaque bookmark from the same open reader; foreign or reopened-file bookmarks
  are rejected. Restoring a later page resets libogg's expected sequence before
  accepting that page, including when the target follows a continued packet.
  Close releases buffers and
  the descriptor; the owner must separately release hardware and its lease.

## Duration and Seeking

- Before enabling output, `RecordingPlaybackSource` scans encoded packets using
  libopus packet parsing and duration APIs. It does not decode PCM for indexing.
  Old Audream granules are ignored; standard pre-skip and final padding are
  removed from total duration. Missing catalog duration does not prevent playback.
  Standard cropped/live streams with a nonzero initial granule origin are
  explicitly unsupported rather than assigned an incorrect timeline.
- A fixed 128-point sparse index is decimated as needed. Reader bookmarks identify
  a safe page and the packet count to skip from it, including multi-packet pages
  and continuation. Indexing yields every 64 packets and on reader work limits;
  cancellation retains the storage lease until the same worker closes the file.
  Opening still scans the entire encoded file. Actual long-file SD startup
  latency and the combined index/decoder/task memory peak need device measurement.
- The worker locates a target from the sparse index, resets the independent
  decoder and discards preroll before sending the exact requested sample suffix.
  It follows [RFC 7845 section 4.6](https://www.rfc-editor.org/rfc/rfc7845.html#section-4.6):
  at least 80 ms preroll, or decoding from the beginning with full original
  pre-skip for near-start targets. It does not decode the entire preceding file.
- The existing checked output drain runs before repositioning. Paused seeks stay
  paused; preroll never reaches the speaker. Stop or recording preemption cannot
  release a file, decoder or output while a seek operation is still executing.
  Decoder reset, file or drain failure retires playback through checked cleanup.
- The seek slider queues a request with playback generation and UI request ID.
  Only the matching applied receipt updates confirmed position. While seeking,
  pause, volume and another seek are disabled; stop remains available. Dragging
  survives refresh, failures restore the confirmed position, and an unknown or
  zero total disables seeking. Seeking to the exact end emits no remaining audio.
- Normal end requires accepted PCM position to equal the indexed total. Output
  beyond that total is rejected before writing. This detects timing disagreement,
  not same-size content replacement; the source is not a file integrity hash.

## Existing File Compatibility

The legacy `OggOpusWriter` omits an EOS page. Complete-page EOF is therefore
reported separately as `kLegacyPageBoundary`, not as a validated EOS stream.
This permits reading older saved recordings, but cannot prove that complete
pages were not lost before the file was opened. A partial page or unfinished
packet remains an error. A header-only file is not playable audio.

That writer also accumulates `frame_samples` at the input sample rate
(16 kHz in `LocalRecordingService`), whereas Opus granules and pre-skip use a
48 kHz clock. Do not derive old-file playback duration from those granules.
The reader preserves the raw value and pre-skip/output-gain metadata; it does
not fix files in place. Playback uses decoded sample counts for the
legacy format and handles standard-stream trimming separately.

## New Watch Recording Format

The Note Watch UI profile now uses `WatchOpusWriter` in LocalRecordingService.
The recording-only profile, StickS3 writer and existing files remain on their
previous format; the legacy writer is not rewritten. The profile-dependent class
header loads ESP-IDF's sdkconfig explicitly so translation units agree on layout.

The new writer uses the same vendored libogg for page lacing, sequence and CRC,
following [libogg encoding](https://xiph.org/ogg/doc/libogg/encoding.html).
Actual libopus packet parsing/duration APIs determine the 48 kHz granule clock,
not the nominal capture frame size. Headers each end their own page, and the last
audio packet receives EOS on normal close, as specified by
[RFC 7845](https://www.rfc-editor.org/rfc/rfc7845.html).
One encoded packet (at most 8 KiB) is retained until the next packet or close.
Writes otherwise flush one packet at a time, not an ever-growing audio cache.

The distinct vendor `Audream watch Ogg Opus v2` prevents legacy granule correction
from being applied to new files. The reader requires EOS for this marker:
complete-page truncation of a v2 file is an error, not legacy EOF compatibility.
The marker is not cryptographic proof of origin or completeness.

Pre-skip remains zero and all supplied encoded samples are retained. The watch
codec worker now resets the shared encoder on a new recording callback identity,
an assistant generation change, or a recording/assistant/testing role change.
Pause/resume retains the recording callback identity even when capture generation
changes, so it does not reset the same file's prediction history. Callback
replacement still requires draining the old recording; the dispatch identity
check is an additional stale-packet fence, not a replacement for resource drain.
Reset and encode failures, invalid PCM sizes and empty/oversized encoder output
report `(nullptr, 0)` to the active recording callback. Failure remains sticky
for that stream; only a new stream can reset and try again. The recording service
retains the error across metadata retries instead of saving incomplete audio as
successful. Non-watch encoding behavior remains unchanged.

The encoder's lookahead is not queried and delayed input is not recovered by
encoding padding at stop. Gapless capture alignment remains separate work; a
guessed pre-skip would remove real samples. Input hardware read failures and
capture discontinuities need separate handling; this encoder check alone does
not prove every captured sample was retained.
Metadata recording duration remains the existing capture timer, while the player
uses the encoded duration. Those durations can differ when capture drops work.

Short writes, invalid packets, final packet failure, fflush and fclose errors
are sticky until an explicit new recording. Close attempts descriptor closure
even on failure and cannot become success merely by being repeated. Empty files
are not successful audio. Finalization success means the checked stdio operations
completed, not a claim of power-loss-safe SD durability. Loss on abrupt stop can
include the retained last packet and filesystem buffers.

LocalRecordingService latches packet and close failures across metadata retries,
persists additive `recording_complete=false`, and releases its session only after
actual capture drain, file closure and metadata save. Failed audio is retained
for inspection; the watch detail says it was not fully saved rather than showing
the saved label. Old metadata without the field remains compatible. This status
does not yet introduce phone-side quarantine or automatic recovery of partial
recordings; no client is claimed to enforce the new field.

## Verification

```bash
bash scripts/test_recording_reader.sh
bash scripts/test_recording_playback.sh
bash scripts/test_watch_opus_writer.sh
bash scripts/test_watch_runtime.sh
```

These compile desktop tests only, using the actual vendored framing C source and
actual recording writer. The reader script checks hashes of the original Xiph
files. ASAN/UBSAN cover packet equality, 5,000-packet round-trip, multi-packet and
continued pages, every byte of a damaged audio page, truncation, headers, stream
identity, cancellation, per-call work limits, oversized packets, file mutations,
maximum valid pages and bounded memory over a 10,000-packet recording.

Optional independent encoder interoperability test, when FFmpeg is installed:

```bash
ffmpeg -hide_banner -loglevel error -f lavfi \
  -i sine=frequency=440:sample_rate=16000:duration=2 -ac 1 \
  -c:a libopus -frame_duration 60 -y /tmp/audream-reader-standard.opus
AUDREAM_OPUS_FIXTURE=/tmp/audream-reader-standard.opus \
  bash scripts/test_recording_reader.sh
```

This confirms 34 packets, pre-skip 312 and explicit EOS. Also pass
`AUDREAM_OPUS_FIXTURE` to `test_recording_playback.sh` to verify exactly 32,000
output PCM samples at 16 kHz after trimming the same two-second file.

Playback tests compile the actual reader, decoder wrapper and pthread worker
against the managed ESP decoder declarations. A host API adapter uses reference
libopus for real PCM; it is not the ESP binary decoder. Thirteen groups cover sample
rates, pre-skip, EOS, mandatory gain, decode failure, actual files, replay,
nonblocking stop, pause, blocked writes/preparation, recording preemption and
cleanup retry, volume completion, rollback outcomes and recording preemption
during a blocked volume transaction. Additional coverage includes a 30,000-packet
duration index, bounded index size, seek cancellation, paused/backward seeks,
limited preroll decoding, decoder reset failure and recording preemption during
blocked seek drain. The optional FFmpeg fixture also tests targets at the start,
inside packets, one sample before the end and the exact end, with standard
pre-skip and padding. Near-start PCM is compared with the full linear decode.
Separate tests compile the actual Box and AudioService playback
methods with their real class headers. Their construction, quiescence and
hardware boundaries are controlled adapters, not a complete AudioService runtime.
Volume tests compile the actual SDK volume conversion C source and check all 101
levels, paused preference application and register errors hidden by the wrapper.
AudioService tests inject persistence and hardware failures, including a failed
commit that leaves a staged value and failure of the rollback itself. These are
not real NVS or I2C fault injection on a device.

The existing desktop LVGL suite now checks explicit catalog play, current
generation in controls, pending versus completed states, stable player widgets,
leaving/reopening playback, failure retry, asynchronous volume receipts, mute,
maximum volume, save failure and scrolling past an error footer in English and Chinese.
Seek tests cover unknown/known duration, drag stability, sample target conversion,
pending/foreign/applied/failed receipts, paused state and exact-end requests. These UI
tests inject worker snapshots; they do not execute the complete Application or
the board's long-press callback. Native Application binding is source-level
integration pending the authorized firmware and device acceptance below.

Writer tests use actual libogg and reference libopus at all five supported rates,
variable frame durations, single-packet/empty recordings, exact encoded-byte
round trips, EOS truncation and injected file I/O failures. The actual local
recording lifecycle suite runs for both old and watch writer profiles and checks
persisted failure across metadata retries. Optional FFmpeg/libopus interoperability
requires exactly 150 ms and the corresponding decoded PCM byte count at each
rate, without stderr diagnostics; the native FFmpeg decoder is also checked at
the watch's 16 kHz rate. On the local FFmpeg 8.1.1 build, the synthetic 8 kHz
variable-duration fixture produced duplicate output DTS with the native decoder,
despite monotonic input packet DTS and correct PCM length. The libopus decoder
did not reproduce it. This is an observed interoperability limitation, not a
claim that all decoders or rates are free of issues.

## Remaining Integration

Capture encoder delay alignment and full native device acceptance remain required.

`scripts/test_watch_stream_encoder.sh` compiles the actual production codec
worker, enqueue, drain predicate and callback setter for watch and non-watch
profiles under ASAN/UBSAN. The ESP encoder API is backed by reference libopus;
capture, startup, quiescence and unused decoder hardware are host boundaries.
It compares packet bytes with a fresh/continuous encoder, checks role and stream
changes, fences a blocked old-file callback, and injects reset/process/output
failures. This is not an ESP encoder binary, microphone or full firmware test.
Verify actual SD errors and long recordings, native
Opus decoding, pause tail continuity, speaker mute/volume, failed cleanup,
long-press recording preemption and power before declaring the player complete.
No firmware compilation/flash or app installation was performed for these host
tests. The legacy recording writer and StickS3 profile are unchanged; the watch retains
long-press recording priority and separate assistant ownership.
