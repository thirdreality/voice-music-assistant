# Vendored eSpeak-NG (TTS) for A113X

`libespeak-ng.a`, `libucd.a` and `speak_lib.h`, GPL-3.0+ (COPYING).

Source: `csukuangfj/espeak-ng@ed530aa113046142eb5115cf2fc9157854d0ffe1`,
built with `USE_ASYNC/MBROLA/LIBSONIC/LIBPCAUDIO/KLATT/SPEECHPLAYER=OFF`,
Buildroot GCC 10.2, `-march=armv8-a+crc -mtune=cortex-a53`.
Voice data: `../../models/tts/` (installed by the package .mk).
