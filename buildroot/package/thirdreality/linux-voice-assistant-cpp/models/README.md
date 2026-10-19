# Local voice models

Installed by `linux-voice-assistant-cpp.mk` for `--voice-backend local`.

- `stt/ggml-tiny.en-q5_1.bin` -> `/usr/share/thirdreality/stt/`
  whisper.cpp tiny.en q5_1 (MIT), from
  `huggingface.co/ggerganov/whisper.cpp` (sha256 of the original:
  `c77c5766f1cef09b6b7d47f21b546cbddd4157886b3b5d6d4f709e91e66c7c2b`),
  rewritten by `third_party/whisper.cpp/align-ggml-model.py` so LVA can
  mmap the weights.
- `tts/espeak-ng-data/` -> `/usr/share/thirdreality/tts/espeak-ng-data/`
  English subset of espeak-ng-data (GPL-3.0+): phontab, phondata,
  phonindex, intonations, en_dict, lang/gmw, voices.
