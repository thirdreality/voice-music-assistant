# Sendspin v0.9 Migration Plan

## Scope

This document tracks the migration of the ThirdReality Sendspin client from
`sendspin-cpp` v0.7.0 to v0.9.3. It separates required compatibility and
reliability work from optional upstream features.

The target behavior must preserve:

- TAP play/pause control through the Unix domain socket.
- TTS interruption without changing Sendspin playback state.
- DUCK/UNDUCK during voice-assistant interactions.
- Shared system volume and mute behavior.
- Metadata, color, and music-reactive LED behavior.
- Zero-interaction discovery and playback until formal pairing is designed.

## Current Migration Status

Completed in the working tree:

- [x] Update `sendspin-cpp` from v0.7.0 to v0.9.3.
- [x] Replace `start_server()` with `start()` and `stop()`.
- [x] Remove the configured `client_id`.
- [x] Add blob persistence for the key-derived client identity.
- [x] Migrate static delay to output delay.
- [x] Migrate Visualizer request fields from `support` to `stream`.
- [x] Enable unpaired access to preserve the existing user experience.
- [x] Pass a clean Buildroot package build.

## Required Legacy-Code Fixes

Implementation is complete. Items still unchecked in this section require
hardware or fault-injection validation rather than additional migration code.

### P0: PulseAudio Thread Safety

Previous problem:

`on_audio_write()` runs on the Sendspin sync thread. Stream lifecycle and volume
callbacks run on the main thread. The old atomic `pa_simple*` did not stop
`flush()` or `free()` from racing an in-flight `pa_simple_write()`.

Affected state:

- `pa_`
- `frame_size_`
- `current_rate_`
- `current_channels_`
- `last_volume_`

Tasks:

- [x] Replace the unsafe shared `pa_simple*` lifecycle.
- [x] Serialize playback open, write, and close through a threaded PA mainloop.
- [x] Remove all cross-thread non-atomic state access.
- [x] Ensure providers and listeners outlive `SendspinClient`.
- [ ] Verify rapid play/stop, track changes, reconnects, and shutdown.

Acceptance criteria:

- No use-after-free or data race between callbacks.
- Playback can stop while an audio write is active.
- Repeated start/stop cycles do not leak PulseAudio streams.

### P0: Bounded and Interruptible Audio Writes

Previous problem:

`on_audio_write()` ignored `timeout_ms`. `pa_simple_write()` could block longer
than the callback contract and prevent `client.stop()` from completing.

Tasks:

- [x] Use a non-blocking `pa_stream` output path with bounded polling.
- [x] Honor the supplied `timeout_ms`.
- [x] Make shutdown stop new writes and close the stream safely.
- [ ] Verify shutdown during playback and after PulseAudio failure.

Acceptance criteria:

- `client.stop()` completes within a defined bound.
- The init script does not need SIGKILL during normal shutdown.

### P1: Build Only Used Roles

Problem:

Upstream enables the Source Role by default. The application does not register
it, but its capture and Opus encoder code is still linked.

Tasks:

- [x] Set `SENDSPIN_ENABLE_SOURCE=OFF` explicitly.
- [x] Set `SENDSPIN_ENABLE_OPUS=ON` explicitly.
- [x] Keep Artwork disabled.
- [x] Remove the ineffective `USE_ZLIB=ON` option.
- [x] Remove the unused Buildroot `zlib` dependency.
- [x] Compare binary size before and after the change.

### P1: Correct Color and Visualizer Semantics

Problem:

`on_color_clear()` currently calls `LedColorController::clear()`, which marks
playback as stopped. Upstream defines color clear as palette invalidation only.

Tasks:

- [x] Reset to a neutral/default palette without changing playback state.
- [x] Reserve full LED clear for group playback `STOPPED`.
- [x] Handle `on_visualizer_stream_clear()` explicitly.

Problem:

Visualizer data callbacks run on a dedicated drain thread. They currently run
D-Bus commands, create animation files, and write sysfs synchronously.

Tasks:

- [x] Make Visualizer callbacks publish only lightweight pending state.
- [x] Perform D-Bus, animation, and sysfs work from the main-loop `tick()`.
- [x] Coalesce loudness updates so stale frames are not rendered.
- [x] Preserve the existing LED enable/disable and suppression behavior.

### P1: Handle Controller Command Rejection

Problem:

`ControllerRole::send_command()` now returns `false` when no controller owner is
active, a command is unsupported, or the command queue is full.

Tasks:

- [x] Check every PLAY and PAUSE result.
- [x] Add rate-limited failure logging.
- [x] Do not retry unsupported commands.
- [x] Drop a rejected TAP command and report it through rate-limited logging.

### P1: Buildroot Reproducibility

Tasks:

- [x] Add hashes for v0.9.3, every transitive archive, and every license.
- [x] Remove network-only dependency resolution from the configure phase.
- [x] Pin ArduinoJson, micro-flac, micro-opus, IXWebSocket, and noise-c.
- [x] Make offline rebuilds use the Buildroot download cache.
- [x] Ensure `legal-info` includes every third-party dependency.

### P2: Persistence Hardening and Tests

Tasks:

- [ ] Test identity stability across process and device restarts.
- [ ] Test legacy `static_delay_ms` migration.
- [ ] Test truncated, missing, and incorrectly sized blobs.
- [x] Enforce blob mode `0600` and directory mode `0700`.
- [x] Verify factory reset removes identity and pairing records.
- [x] Update the stale persistence comments in the source.

### P2: Version and Diagnostics Cleanup

Tasks:

- [x] Replace the stale `MA 2.8+` comment.
- [x] Report the actual wrapper version instead of `1.0.0`.
- [x] Handle the `system()` results.
- [x] Add startup logs for protocol version, client ID, and access mode.

## Shared Sound Configuration Ownership

The C++ LVA is the normal writer for `/data/conf/sound.json`.

- [x] Sendspin sends absolute volume updates over `/tmp/lva-control.sock`.
- [x] `tr-buttond` sends absolute volume and microphone-mute updates.
- [x] Configuration requests require an `OK` acknowledgement.
- [x] Senders fall back to direct writes only when LVA is unavailable.
- [x] LVA and every fallback writer use `/tmp/sound_config.lock`.
- [x] Fallback updates are absolute and therefore safe after a lost ACK.
- [x] LVA merges the latest on-disk JSON while holding the shared lock.

This preserves physical button behavior when LVA is stopped without allowing
normal concurrent read-modify-write cycles.

## Optional Upstream Features Not Implemented

### Formal Pairing and Trust Management

Current state:

Unpaired access is enabled. Noise encrypts the connection, but the device does
not restrict playback control to an explicitly authorized Music Assistant
server.

Future tasks:

- [ ] Select a physical pairing-confirmation gesture.
- [ ] Avoid TAP single press and the 7.5-second factory-reset gesture.
- [ ] Implement pairing-window open/close callbacks.
- [ ] Implement pairing success, failure, and trust-state reporting.
- [ ] Provide a way to expose the pairing token or pairing code.
- [ ] Disable unpaired access after the complete flow is validated.
- [ ] Define how users revoke an individual paired server.

Recommendation: implement after the v0.9 playback migration is stable.

### Extended Visualizer Effects

Not currently requested or rendered:

- [ ] Spectrum bins.
- [ ] Peak/onset events.
- [ ] Dominant frequency and amplitude.
- [ ] Downbeat-specific effects.

Recommendation: optional. Keep the current beat and loudness implementation
until the callback threading has been corrected.

### Mono Opus Playback

- [ ] Decide whether Music Assistant sends 48 kHz mono Opus to this product.
- [ ] Add `{OPUS, 1, 48000, 16}` only if required.
- [ ] Validate PulseAudio channel configuration and volume processing.

Recommendation: optional unless a real stream requires it.

### Source Role

The upstream Source Role sends captured microphone audio to a Sendspin server.
It is not currently registered by this application.

Recommendation: do not implement. It overlaps the HA voice pipeline and could
compete for the ALSA capture device.

### Artwork Role

Recommendation: keep disabled. The product has no display surface for artwork.

### Runtime Restart and Availability APIs

- [ ] Add runtime restart only if configuration can change without restarting
      the process.
- [ ] Add availability or leave controls only when a product requirement uses
      them.

Recommendation: no current requirement.

## Compatibility Requirements

- Music Assistant must be v2.11.0b4 or newer.
- Sendspin v0.9.x is not wire-compatible with the v0.7 protocol.
- The first upgraded boot creates a new key-derived client ID.
- Music Assistant may show the upgraded speaker as a new player.
- The generated identity must persist under `/data/conf/sendspin-state/`.
- Factory reset intentionally generates a new identity on the next boot.

## Hardware Validation Matrix

- [ ] Discovery after a clean boot.
- [ ] Discovery and stable identity after a second boot.
- [ ] FLAC playback at 44.1 kHz and 48 kHz.
- [ ] Opus playback at 48 kHz.
- [ ] PCM playback at 44.1 kHz and 48 kHz.
- [ ] TAP starts and pauses Sendspin.
- [ ] TTS ducking and unducking do not stop Sendspin.
- [ ] TAP during TTS stops only TTS.
- [ ] Volume changes propagate in both directions.
- [ ] Mute changes propagate correctly.
- [ ] Track changes do not race PulseAudio stream replacement.
- [ ] Music Assistant reconnects after a network interruption.
- [ ] Sendspin exits cleanly during active playback.
- [ ] LED enable/disable survives reboot.
- [ ] Beat, loudness, metadata, and color LEDs remain synchronized.
- [ ] Factory reset clears the Sendspin identity and pairing state.

## Implementation Order

1. Fix PulseAudio thread safety and bounded writes.
2. Disable unused roles and remove stale dependencies.
3. Fix LED callback threading and color-clear semantics.
4. Handle controller command failures.
5. Add Buildroot hashes and reproducible dependency handling.
6. Harden and test persistence.
7. Run the complete hardware validation matrix.
8. Consider formal pairing and optional Visualizer features.
