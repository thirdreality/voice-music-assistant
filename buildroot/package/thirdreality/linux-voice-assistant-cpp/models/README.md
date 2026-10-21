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
  phonindex, intonations, en_dict, lang/gmw. Used only to turn text into
  phonemes for the TTS voice.
- `tts/amy/{front_f32,dec_q8}.bin` -> `/usr/share/thirdreality/tts/amy/`
  (2.8 MB). sanoTTS piperlite voice "amy" (`third_party/sanotts/README.md`),
  generated from upstream `web/voices/amy/{front,dec}_f32.bin` at the same
  revision by `third_party/sanotts/tools/make_voice_files.py`. Both files
  are mmapped and used in place; nothing is written to `/data`.
  - `front_f32.bin`: duration + acoustic students, upstream bytes
    unchanged. Kept fp32: int8 here changes predicted durations.
  - `dec_q8.bin`: decoder, conv weights int8 per output channel, biases
    fp32. Waveform corr >= 0.9995 (SNR ~30 dB) vs fp32; whisper
    transcripts unchanged.

  Upstream fp32 sha256: front
  `9bc304dd1db30b17a2efb751795137c6d26f25aaf9c96f05bc1dec7c86e0ce23`,
  dec `f520bcbf278607343e68170a06619a465a333d2d67476c6823d130e91580281b`.
  Voice data licence: see the repo root `todo.md`.
