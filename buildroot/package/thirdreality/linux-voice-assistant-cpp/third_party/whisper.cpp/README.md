# Vendored whisper.cpp (STT) for A113X

Static libs + headers of [whisper.cpp](https://github.com/ggml-org/whisper.cpp),
MIT (LICENSE).

- Upstream commit `0c7be696371069e0baa2515ed7aa08d5a5473aa6` +
  `lva-whisper-memory.patch`
- Buildroot GCC 10.2; CMake `-DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=OFF
  -DGGML_CPU_ARM_ARCH=armv8-a+crc -DGGML_OPENMP=OFF -DWHISPER_SDL2=OFF
  -DWHISPER_CURL=OFF -DCMAKE_BUILD_TYPE=Release`

## lva-whisper-memory.patch

1. Decoder logits for the last batch row only (compute buffer 96 -> 6 MB,
   no 93 MB logits reserve).
2. Env caps `WHISPER_MAX_AUDIO_CTX`, `WHISPER_MAX_TEXT_CTX`,
   `WHISPER_MAX_PROMPT_TOKENS` size KV caches and compute buffers.
   WhisperStt sets 750 / 128 / 16. Unset = upstream behavior.
3. `whisper_init_from_file_mmap_no_state()`: weights mmapped from a model
   rewritten by `align-ggml-model.py`.
