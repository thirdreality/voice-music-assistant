# linux-voice-assistant-cpp

C++ rewrite of [OHF-Voice/linux-voice-assistant](https://github.com/OHF-Voice/linux-voice-assistant.git),
a Python-based voice satellite for Home Assistant. This project is a
full native replacement targeting embedded Linux (Amlogic A113X,
aarch64, Buildroot).

## Features

- ESPHome native API server (TCP 6053, plain-text framing, protobuf)
- mDNS auto-discovery via avahi (_esphomelib._tcp)
- 11 entities: mute switch, thinking-sound switch, 3× wake-word
  sensitivity numbers, mic gain, mic volume, mic noise select,
  media player, home button event, firmware update
- Voice satellite state machine (wake → STT → intent → TTS playback →
  timer ring loop)
- Optional on-device STT + TTS (`--voice-backend local`); HA only
  resolves the intent. See "Local voice backend".
- MicroWakeWord inference (TFLM microfrontend + TFLite C runtime,
  dlopen)
- OpenWakeWord inference (melspectrogram + embedding TFLite models)
- WebRTC AGC + noise suppression + AEC (hardware-loopback echo
  cancellation when using ALSA capture backend with reference channels)
- ALSA capture backend with multi-channel support (mic + reference
  channels for AEC)
- PulseAudio capture backend (legacy fallback)
- LED ring animations via D-Bus
- Home button monitor (/dev/input/event0, single/double/triple press)
- Mic mute GPIO bridge (hardware slider ↔ HA switch)
- ALSA system volume sync via adckey_function.sh
- Sendspin duck/unduck (SIGUSR1/2) during voice pipelines
- sound.json external-change watcher (500 ms poll)
- Supervisor HTTP API on port 8086 (HMAC-MD5 signed commands)
- OTA: check version, download, MD5 verify, swupdate install
- Timezone sync from HA (GetTimeRequest/GetTimeResponse)

## Build

```
./go --docker trspk rebuild linux-voice-assistant-cpp
```

Full image:
```
./go --docker trspk <version>
```

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
--tts-data-dir <dir>       espeak-ng-data directory
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
src/tr/                     ThirdReality integration (LED, GPIO, home
                            button, volume, sendspin, supervisor, OTA,
                            timezone sync, sound.json watcher)
src/tools/                  Dev tools (micro_features_dump, tflite_inspect,
                            wake_word_dump, aec_loopback_test, stt_wav_test,
                            conversation_reply_test)
src/util/                   Log helper
third_party/microfrontend/  TFLM microfrontend + kissfft sources
third_party/tflite_c/       Prebuilt libtensorflowlite_c.so (GLIBC≤2.29)
third_party/whisper.cpp/    Prebuilt whisper.cpp + ggml static libs (A113X),
                            memory patch, model alignment script
third_party/espeak-ng/      Prebuilt eSpeak-NG static libs (A113X)
wakewords/microwakeword/    MicroWakeWord .tflite + .json models
wakewords/openwakeword/     OpenWakeWord .tflite + .json (+ mel/embedding)
sounds/                     UI feedback sounds (wake_word_triggered,
                            processing, timer_finished, mute_switch_on,
                            mute_switch_off)
```

## Dependencies (Buildroot)

host-protobuf, protobuf, json-for-modern-cpp, mpv, pulseaudio,
alsa-lib, webrtc-audio-processing, libcurl, avahi.
Plus the vendored libtensorflowlite_c.so (dlopen'd at runtime, not
linked) and liblva_speech.so (built here, installed to /usr/lib).

## Local voice backend

Selected in HA with the "Speech Processing" select (`Home Assistant` /
`3R Speaker`), saved as `voice_backend` in `/data/conf/sound.json`.
`--voice-backend` only sets the default.

- The espeak-ng data ships in `/usr/share/thirdreality/tts`. The STT
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
- "Speech Voice (Local TTS)" (US/UK/Scottish male and female, soft,
  whisper) and "Speech Rate (Local TTS)" (100-250 wpm) apply to the
  next reply without a restart. They only affect the local backend;
  with `Home Assistant` the voice is set in the HA Assist pipeline.

```
wake -> LocalStt (whisper.cpp) -> HA conversation.process -> EspeakTts -> mpv
```

- Input is the post-APM, post-mic-gain capture tap.
- ~16 MB anonymous memory between turns, ~85 MB peak during a decode.
- Engines: `third_party/whisper.cpp/README.md`, `third_party/espeak-ng/README.md`.

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
