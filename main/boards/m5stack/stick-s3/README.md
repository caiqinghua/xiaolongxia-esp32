# M5Stack StickS3 Endurance Recorder

This board target is an isolated battery endurance test for the Note Watch local recording path.
It does not replace or alter the Note Watch board target.

## Runtime Behavior

- Captures the ES8311 microphone directly at 16 kHz mono.
- Does not initialize Wi-Fi, Bluetooth, the LCD, or the speaker amplifier while recording.
- Uses the recording power profile with an 80 MHz CPU ceiling and tickless idle enabled.
- Prints metadata for up to seven recent completed sessions after the internal Flash FAT filesystem is mounted.
- Starts recording when the Boot button is pressed.
- Rotates Ogg/Opus files at approximately 512 KiB and removes the oldest completed segment when space is needed.
- Stops after three consecutive battery samples at or below 5%, finalizes metadata, unmounts Flash, disables peripheral rails, and enters deep sleep.

While recording, a short Boot-button press prints the current elapsed time. A long press stops the test and finalizes metadata without entering deep sleep immediately.

## Metadata

Normal segment files and metadata remain compatible with the existing recording list:

```text
/sdcard/audream/recordings/<session-id>-s0000.opus
/sdcard/audream/recordings/<session-id>-s0000.opus.json
```

The completed endurance session is written separately:

```text
/sdcard/audream/recordings/<session-id>.session.json
/sdcard/audream/recordings/last-session.json
```

The session metadata records elapsed duration, encoded duration, retained duration, segment counts, overwrite count, start and end battery percentages, consumed battery percentage, stop reason, and whether audio remained complete. Elapsed duration remains the full endurance-test duration even when early audio segments have been overwritten.
