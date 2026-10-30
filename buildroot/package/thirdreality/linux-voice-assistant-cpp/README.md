# linux-voice-assistant-cpp

C++ rewrite of [OHF-Voice/linux-voice-assistant](https://github.com/OHF-Voice/linux-voice-assistant.git),
a Python-based voice satellite for Home Assistant. This project is a
full native replacement targeting embedded Linux (Amlogic A113X,
aarch64, Buildroot).

## Features

- ESPHome native API server (TCP 6053, plain-text framing, protobuf) and
  mDNS discovery (`_esphomelib._tcp`)
- Voice satellite: wake word -> STT -> intent -> TTS, timers, continued
  conversation. STT/TTS run in HA by default or on the device (see
  "Local voice backend")
- MicroWakeWord and OpenWakeWord (TFLite C runtime, dlopen)
- WebRTC AGC, noise suppression and AEC on the ALSA capture path (mic +
  hardware loopback reference channels); PulseAudio capture as fallback
- One HA media player for mpv (HA media, TTS, announcements) and
  Sendspin (Music Assistant), see "Sendspin"
- ThirdReality hardware: LED ring (D-Bus), mic mute GPIO, system volume
  (`tr-volume`), tap/home gestures forwarded by `tr-buttond` over
  `/tmp/lva-control.sock`
- `sound.json` watcher (500 ms poll) for changes made by other tools
- Supervisor HTTP API on port 8086 (HMAC-MD5 signed commands), OTA
  (version check, download, MD5 verify, swupdate)
- Timezone sync from HA

### HA entities

| Entity | object_id |
|---|---|
| Media Player | `linux_voice_assistant_media_player` |
| Mute | `mute` |
| Thinking Sound | `thinking_sound` |
| Wake Word 1 / 2 Sensitivity | `wake_word_1_sensitivity`, `wake_word_2_sensitivity` |
| Stop Word Sensitivity | `stop_word_sensitivity` |
| Continue Conversation Delay | `continue_conversation_delay` |
| Mic Auto Gain / Mic Noise Suppression / Mic Volume | `mic_gain`, `mic_noise`, `mic_volume` |
| Speech Processing | `speech_processing` |
| Speech Rate (Local TTS) | `speech_rate` |
| Sendspin | `sendspin` |
| Home Button (event) | `thirdreality_home_button` |
| Firmware Update | `thirdreality_firmware_update` |
| Allow Background Internet Access | `allow_background_internet` |

### Background internet access

**Allow Background Internet Access** defaults to **on** and is saved in
`/data/conf/network.json` across reboots and OTA. Turning it off stops
automatic firmware checks, public NTP fallback and automatic speech-model
downloads. Local HA, DHCP NTP and cached models remain usable; Wi-Fi status
requires only association and a local IP. Connectivity probes and offline
LED effects have been removed. Voice starts after Wi-Fi has a local IP and
the initial NTP attempt finishes. If NTP is unavailable, HA can set the clock
and refresh it hourly; an NTP-synchronized clock takes priority.

Explicit firmware checks/installs, speech-model downloads and requested
media playback may still access the internet. If an automatic speech-model
download is blocked or cancelled, speech processing falls back to HA;
select **3R Speaker** again to retry. Re-enabling the switch does not resume it.

This controls application requests over IPv4/IPv6, not firewall rules.
DHCP servers and services configured in HA remain under administrator control.
Firmware checks send the device model, version and MAC address to the OTA
service; default speech models come from GitHub release assets.

## Build

```
./go --docker trspk rebuild linux-voice-assistant-cpp
```

Full image:
```
./go --docker trspk <version>
```

sendspin-cpp is linked statically. After changing the `sendspin-cpp`
package, rebuild it and then this package; Buildroot does not rebuild
dependents on its own.

## Runtime invocation

Launched by `S99ha-speaker`:
```
/usr/bin/linux-voice-assistant-cpp \
    --name "$spk_name" \
    --port 6053
```

All options:
```
--name <name>              Device name (from device.json)
--port <port>              TCP port (default: 6053)
--host <addr>              Bind address (default: 0.0.0.0)
--preferences-file <p>     Prefs file (default: /data/conf/sound.json)
--audio-device <dev>       PulseAudio device (default: system default)
--wakeword-type <type>     "micro" or "open" (default: micro)
--wakeword-models <ids>    Comma-separated model IDs (default: okay_nabu)
--capture-backend <b>      Capture backend: alsa|pulse (default: alsa)
--capture-alsa-device <d>  ALSA capture device (default: hw:0,4)
--capture-mic-channel <n>  0-based mic channel in ALSA stream (default: 0)
--capture-ref-channels <c> Comma-separated 0-based ref channels for AEC;
                           empty or "none" disables AEC (default: 2,3)
--continue-conversation-delay <s>
                           Seconds to wait after TTS finishes before opening
                           the mic for a continued conversation (default: 0.5)
--voice-backend <b>        ha|local (default: ha). ha = stream audio to the HA
                           Assist pipeline; local = on-device STT + TTS.
                           Default only; the HA select overrides it. local
                           uses ha until the STT model is downloaded.
--stt-model <path>         whisper.cpp ggml model (default: downloaded to
                           /data/models/stt/ggml-tiny.en-q5_1.bin; a custom
                           path is used as is, never downloaded)
--stt-model-url <url>      Download URL for the default STT model (testing a
                           mirror; size and sha256 must still match)
--stt-threads <n>          STT decode threads, 1-4 (default: 4)
--tts-threads <n>          TTS decode threads, 1-4 (default: 2)
--tts-data-dir <dir>       espeak-ng-data directory (TTS phonemizer)
                           (default: /usr/share/thirdreality/tts/espeak-ng-data)
--conversation-agent <id>  HA conversation agent, e.g. conversation.home_assistant
                           (default: HA's default agent)
--stt-dump-wav <path>      Write each STT utterance to this WAV (diagnostics)
--debug                    Enable debug logging, also written to
                           /usr/share/thirdreality/log/lva.log (5 MiB cap)
--help                     Show help
```

The wake-word model directory is derived from `--wakeword-type`:
`/usr/share/thirdreality/wakewords/microwakeword/` or `.../openwakeword/`.

## Source layout

```
proto/                      Vendored api.proto + api_options.proto
                            (aioesphomeapi v42.7.0)
src/main.cpp                CLI, startup, epoll main loop
src/protocol/               ESPHome wire framing, TCP server, mDNS
src/state/                  Preferences (sound.json) + ServerState
src/entities/               ESPHome entities
src/audio/                  AudioCapture, LibMpvPlayer, PcmRingBuffer,
                            MicroFeatures, MicroWakeWord, OpenWakeWord,
                            OpenWakeWordFeatures, WakeWordScanner,
                            WakeWordEngine, TfliteRuntime,
                            WebRtcProcessor, IAudioPlayer
src/satellite/              Voice pipeline state machine, LocalStt worker,
                            conversation.process reply parser
src/sendspin/               SendspinHub: sendspin-cpp client, PulseAudio
                            sink, identity files, music LED
src/tr/                     ThirdReality integration (control socket, LED,
                            mic mute GPIO, volume, model download,
                            supervisor, OTA, timezone sync, sound.json
                            watcher)
src/tools/                  Dev tools (micro_features_dump, tflite_inspect,
                            wake_word_dump, aec_loopback_test, stt_wav_test,
                            tts_wav_test, conversation_reply_test)
src/util/                   Log helper
third_party/microfrontend/  TFLM microfrontend + kissfft sources
third_party/tflite_c/       Prebuilt libtensorflowlite_c.so (GLIBC <= 2.29)
third_party/whisper.cpp/    Prebuilt whisper.cpp + ggml static libs (A113X),
                            memory patch, model alignment script
third_party/espeak-ng/      Prebuilt eSpeak-NG static libs (A113X), used as
                            the TTS phonemizer
third_party/sanotts/        sanoTTS piperlite runtime (neural TTS), NEON
                            kernels, voice file converter
wakewords/microwakeword/    MicroWakeWord .tflite + .json models
wakewords/openwakeword/     OpenWakeWord .tflite + .json (+ mel/embedding)
sounds/                     UI feedback sounds (wake_word_triggered,
                            processing, timer_finished, mute_switch_on,
                            mute_switch_off)
```

## Dependencies (Buildroot)

host-protobuf, protobuf, json-for-modern-cpp, mpv, pulseaudio,
alsa-lib, webrtc-audio-processing, libcurl, avahi (daemon + libdns_sd
compatibility), tr-volume, sendspin-cpp (static, staging only).
Plus the vendored libtensorflowlite_c.so (dlopen'd at runtime, not
linked) and liblva_speech.so (built here, installed to /usr/lib).

## Sendspin

LVA runs the Sendspin player (sendspin-cpp) in process, following
ESPHome's sendspin hub + media_source + speaker_source design.

- The HA media player state follows the active source. A Sendspin stream
  reads PLAYING from stream start to stream end.
- PAUSE / PLAY go to the whole Sendspin group. STOP leaves the group
  (this speaker only). `play_media sendspin://current` resumes the group;
  `sendspin://host:port` connects to that server.
- mpv and Sendspin take the speaker from each other. Voice turns and HA
  announcements duck Sendspin (-12 dB).
- Volume and mute are synced both ways with the server.
- Tap: abort TTS, else pause / resume. Double tap: music LED on/off
  (`led_disabled` in `/data/conf/sendspin.json`, off by default).
- The "Sendspin" switch (`sendspin_enabled` in `sound.json`, default on)
  starts / stops the client. `_sendspin._tcp` (port 8928, TXT
  `path=/sendspin`) is published only while it runs. A failed start is
  retried every 5 s.
- Identity and pairing live in `/data/conf/sendspin-state/`; keep it to
  keep the Music Assistant player. The client sends the device MAC, so
  Music Assistant hides the duplicate HA media player.
- Requires Music Assistant 2.10.7+ or 2.11.0b4+ (Sendspin 1.0.0-rc1).

## Local voice backend

Selected in HA with the "Speech Processing" select (`Home Assistant` /
`3R Speaker`), saved as `voice_backend` in `/data/conf/sound.json`.
`--voice-backend` only sets the default.

- The TTS voice (`amy`, 2.8 MB) and the espeak-ng phonemizer data ship
  in `/usr/share/thirdreality/tts`; see `models/README.md`. The STT
  model (31 MB) is downloaded to `/data/models/stt` from the `v1.2.4`
  GitHub release; see `models/README.md`. LVA starts the download on
  every start until the model is installed, whatever the backend. The
  download resumes after a drop and the file is installed only after its
  sha256 matches.
- Picking `3R Speaker` before the model is installed keeps HA in use and
  shows an HA notification with the download progress. When the
  download finishes (or fails) another notification replaces it.
- Switching backend restarts LVA (once the current voice turn ends);
  the init script's monitor starts it again within ~10 s.
- "Speech Rate (Local TTS)" (100-250 wpm, 165 = normal speed) applies
  to the next reply without a restart. It only affects the local
  backend; with `Home Assistant` the voice is set in the HA Assist
  pipeline.

```
wake -> LocalStt (whisper.cpp) -> HA conversation.process -> SanoTts -> mpv
```

- Input is the post-APM, post-mic-gain capture tap.
- ~16 MB anonymous memory between turns, ~85 MB peak during an STT
  decode; TTS adds ~13 MB while synthesizing. TTS takes about as long as
  the reply it speaks (RTF ~1.0 on 2 threads).
- Engines: `third_party/whisper.cpp/README.md`,
  `third_party/sanotts/README.md`, `third_party/espeak-ng/README.md`.

### HA setup: allow device actions

The `ha` backend uses the Assist pipeline protocol
(`VoiceAssistantRequest`), which needs no extra permission. The `local`
backend instead sends `conversation.process` as a
`HomeassistantActionRequest` and waits for the
`HomeassistantActionResponse`. HA only runs it when the ESPHome option
"Allow the device to perform Home Assistant actions" is enabled. This
option is off by default for newly added devices.

When the option is off, HA drops the request without replying. HA shows a
repair issue ("... is not permitted to perform Home Assistant actions"),
and the device logs:

```
conversation.process timed out (call_id=N); check that 'Allow the device
to perform Home Assistant actions' is enabled for this device in HA
```

To enable it:

1. Go to Settings -> Devices & services -> ESPHome.
2. Open this device and click Configure.
3. Check "Allow the device to perform Home Assistant actions".

HA reconnects to the device on its own. A working turn logs
`conversation reply (continue=N): '...'`.
