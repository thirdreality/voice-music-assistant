# Vendored sanoTTS runtime (neural TTS)

Piperlite inference runtime from
[Ampixa/sanoTTS](https://github.com/Ampixa/sanoTTS) at
`f73b8382bc98a1c8de3488ba30d727ab82587828` (2026-10-12).
GPL-3.0-or-later (`LICENSE`); these files are not in upstream's
`LICENSE.MIT` list. See the repo root `todo.md` for licence follow-ups.

| File | Upstream | Changes |
|---|---|---|
| `include/snt_front_f32.h`, `src/snt_front_f32.c` | `mcu/include`, `mcu/src` | none |
| `include/snt_piperlite.h` | `mcu/include` | adds `snt_piperlite_set_parallel()` |
| `src/snt_piperlite.c` | `mcu/src` | see below |
| `include/snt_cp_id_tables.h` | `mcu/ports/wasm/cp_id_tables_multi.h` | amy table only |
| `tools/make_voice_files.py` | new | upstream voice -> `front_f32.bin` + `dec_q8.bin` |

`snt_piperlite.c` changes (same output, corr 0.9999999998 vs upstream):

- Conv1d: 4 output channels x 16 time steps register-blocked micro-kernel
  (NEON `vfmaq_laneq_f32`, scalar fallback), repacked weights, zero-padded
  input, no bounds checks in the inner loop.
- ConvTranspose1d: polyphase form (K == 2*stride) on the same micro-kernel
  instead of the scatter loop.
- Jobs dispatched through an optional parallel-for hook.
- `snt_piperlite_init_q8()`: binds an int8 decoder container (conv weights
  int8 per output channel, written by `tools/make_voice_files.py`). Weights
  are dequantized while repacking each conv; the math stays fp32.

Voice weights: `../../models/tts/amy/` (see `../../models/README.md`).
