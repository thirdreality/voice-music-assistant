# Local voice models

Used by the local voice backend.

- `stt/ggml-tiny.en-q5_1.bin` is not installed in the rootfs. LVA
  downloads it to `/data/models/stt/` from the asset of the same name on
  the `v1.2.4` GitHub release (URL, size and sha256 in `src/main.cpp`).
  Later firmware keeps pointing at `v1.2.4`; never delete or replace that
  asset. To change the model, upload it under a new file name to a new
  release and update all three. whisper.cpp tiny.en q5_1 (MIT), from
  `huggingface.co/ggerganov/whisper.cpp` (sha256 of the original:
  `c77c5766f1cef09b6b7d47f21b546cbddd4157886b3b5d6d4f709e91e66c7c2b`),
  rewritten by `third_party/whisper.cpp/align-ggml-model.py` so LVA can
  mmap the weights.
- `tts/espeak-ng-data/` -> `/usr/share/thirdreality/tts/espeak-ng-data/`
  English subset of espeak-ng-data (GPL-3.0+): phontab, phondata,
  phonindex, intonations, en_dict, lang/gmw, voices.
