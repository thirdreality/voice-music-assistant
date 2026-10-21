# Third-party notices: linux-voice-assistant-cpp

The LVA sources in this package are Apache-2.0 (repo root `LICENSE`).
The binaries also contain or ship the components below. Because
espeak-ng and the sanoTTS runtime are GPL-3.0-or-later and are linked
into `liblva_speech.so`, that library and the `linux-voice-assistant-cpp`
binary that links it are distributed under GPL-3.0-or-later as a whole.
The complete corresponding source is this repository; the firmware can
be rebuilt and installed with `./go` (see the repo root `README.md`).

Installed on the device as
`/usr/share/thirdreality/licenses/linux-voice-assistant-cpp/`.

## Linked into liblva_speech.so / the LVA binary

| Component | Version | Licence | Licence text |
|---|---|---|---|
| eSpeak-NG (TTS phonemizer) | `csukuangfj/espeak-ng@ed530aa113046142eb5115cf2fc9157854d0ffe1` | GPL-3.0-or-later | `third_party/espeak-ng/COPYING` |
| sanoTTS piperlite runtime | `Ampixa/sanoTTS@f73b8382bc98a1c8de3488ba30d727ab82587828` (modified, see `third_party/sanotts/README.md`) | GPL-3.0-or-later | `third_party/sanotts/LICENSE` |
| whisper.cpp + ggml (STT) | `0c7be696371069e0baa2515ed7aa08d5a5473aa6` + `lva-whisper-memory.patch` | MIT | `third_party/whisper.cpp/LICENSE` |
| TFLite Micro microfrontend (wake-word features) | via `OHF-Voice/pymicro-features` v2.0.2 | Apache-2.0 | `third_party/microfrontend/LICENSE` |
| kissfft, (c) 2003-2010 Mark Borgerding | via `OHF-Voice/pymicro-features` v2.0.2 | BSD-3-Clause | `third_party/microfrontend/LICENSE.kissfft` |
| ESPHome native API `.proto` | aioesphomeapi v42.7.0 | MIT | upstream repository |

## Shipped alongside

| Component | Licence | Notes |
|---|---|---|
| `libtensorflowlite_c.so` (TensorFlow Lite C API) | Apache-2.0 | `third_party/tflite_c/README.md` |
| espeak-ng data (`/usr/share/thirdreality/tts/espeak-ng-data`) | GPL-3.0-or-later | English subset |
| TTS voice "amy" (`/usr/share/thirdreality/tts/amy`) | GPL-3.0 (sanoTTS weights) | Distilled from piper `en_US-amy-medium` (rhasspy/piper-voices) |
| Whisper tiny.en model (downloaded to `/data/models/stt`) | MIT | `huggingface.co/ggerganov/whisper.cpp` |
| microWakeWord models (`wakewords/microwakeword`) | Apache-2.0 | `esphome/micro-wake-word-models` |
| openWakeWord models (`wakewords/openwakeword`) | CC-BY-NC-SA-4.0 | `dscripka/openWakeWord` pretrained models; only used with `--wakeword-type open` |
| UI sounds (`/usr/share/thirdreality/sounds`) | CC-BY-4.0 | Home Assistant Voice PE sounds, (c) 2024 Clayton Charles Tapp |

Licence texts installed next to this file: `GPL-3.0.txt`,
`MIT-whisper.cpp.txt`, `Apache-2.0.txt`, `BSD-3-Clause-kissfft.txt`.
