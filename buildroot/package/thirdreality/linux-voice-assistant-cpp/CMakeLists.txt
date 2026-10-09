cmake_minimum_required(VERSION 3.16)
project(linux-voice-assistant-cpp
        VERSION 0.0.1
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Generate api.pb.{h,cc} from vendored .proto files
#
# Buildroot puts host-protobuf's protoc on PATH and provides target
# libprotobuf via pkg-config. We avoid CMake's FindProtobuf because in
# Buildroot cross-compile it tends to mix host/target. Instead:
#  - locate protoc by name (host PATH, set by Buildroot)
#  - locate libprotobuf via pkg-config (target sysroot, set by toolchain file)
# ---------------------------------------------------------------------------

find_program(PROTOC_EXECUTABLE protoc REQUIRED)
message(STATUS "Found protoc: ${PROTOC_EXECUTABLE}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
message(STATUS "Found protobuf ${PROTOBUF_VERSION} via pkg-config")

set(PROTO_SRC_DIR  "${CMAKE_SOURCE_DIR}/proto")
set(PROTO_GEN_DIR  "${CMAKE_BINARY_DIR}/generated/proto")
file(MAKE_DIRECTORY "${PROTO_GEN_DIR}")

set(PROTO_FILES
    "${PROTO_SRC_DIR}/api_options.proto"
    "${PROTO_SRC_DIR}/api.proto"
)

set(PROTO_GEN_SRCS
    "${PROTO_GEN_DIR}/api_options.pb.cc"
    "${PROTO_GEN_DIR}/api.pb.cc"
)
set(PROTO_GEN_HDRS
    "${PROTO_GEN_DIR}/api_options.pb.h"
    "${PROTO_GEN_DIR}/api.pb.h"
)

# Single custom command that emits both .pb.cc/.pb.h pairs in one
# protoc invocation (protoc handles multiple inputs in one call,
# resolving the api_options import correctly).
add_custom_command(
    OUTPUT  ${PROTO_GEN_SRCS} ${PROTO_GEN_HDRS}
    COMMAND ${PROTOC_EXECUTABLE}
            --proto_path=${PROTO_SRC_DIR}
            --cpp_out=${PROTO_GEN_DIR}
            ${PROTO_FILES}
    DEPENDS ${PROTO_FILES}
    COMMENT "Generating ESPHome API protobuf C++ sources"
    VERBATIM
)

add_library(lva_proto OBJECT ${PROTO_GEN_SRCS})
target_include_directories(lva_proto PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(lva_proto PUBLIC PkgConfig::PROTOBUF)
target_compile_options(lva_proto PRIVATE
    -Wno-pedantic
    -Wno-unused-parameter
    -Wno-deprecated-declarations
)

# ---------------------------------------------------------------------------
# Main binary
# ---------------------------------------------------------------------------

# nlohmann_json: header-only JSON parser. Installed by the
# json-for-modern-cpp Buildroot package (staging only — no runtime
# cost). Provides the imported target `nlohmann_json::nlohmann_json`.
find_package(nlohmann_json CONFIG REQUIRED)

# libmpv: media playback for the MediaPlayerEntity (phase 3a) and
# the TTS announce player (phase 4). Provides libmpv.so.2.
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
message(STATUS "Found mpv ${MPV_VERSION} via pkg-config")

# libpulse-simple: synchronous PulseAudio capture API used by
# AudioCapture (phase 3b). The "simple" wrapper is sufficient — we
# only need a blocking read in a dedicated thread.
pkg_check_modules(PULSE_SIMPLE REQUIRED IMPORTED_TARGET libpulse-simple)
message(STATUS "Found libpulse-simple ${PULSE_SIMPLE_VERSION} via pkg-config")

# libpulse: asynchronous PulseAudio introspection/subscription API used by
# SendspinPlaybackMonitor to detect the official Sendspin playback stream.
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
message(STATUS "Found libpulse ${PULSE_VERSION} via pkg-config")

# webrtc-audio-processing 1.x: AGC + NoiseSuppression for the mic
# pipeline (phase 6). Without this, wake-word detection rate is
# noticeably worse than the Python build (which uses
# webrtc-noise-gain). The pkg-config name is "webrtc-audio-processing-1".
pkg_check_modules(WEBRTC_APM REQUIRED IMPORTED_TARGET webrtc-audio-processing-1)
message(STATUS "Found webrtc-audio-processing ${WEBRTC_APM_VERSION} via pkg-config")

# libcurl: OTA download + check-version HTTP POST (phase 7).
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
message(STATUS "Found libcurl ${LIBCURL_VERSION} via pkg-config")

# avahi-client: mDNS service registration (_esphomelib._tcp) for
# HA auto-discovery. avahi-daemon is already in defconfig.
pkg_check_modules(AVAHI_CLIENT REQUIRED IMPORTED_TARGET avahi-client)
message(STATUS "Found avahi-client ${AVAHI_CLIENT_VERSION} via pkg-config")

# alsa-lib: required by the aec_loopback_test dev tool, which opens
# the hardware loopback capture device (e.g. plughw:0,4) directly.
# A future change will also use this in the production AudioCapture
# for the ALSA capture backend that drives hardware-loopback AEC.
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
message(STATUS "Found alsa-lib ${ALSA_VERSION} via pkg-config")

# ---------------------------------------------------------------------------
# Vendored microfrontend + kissfft (phase 4 audio feature extractor)
# ---------------------------------------------------------------------------
# These C/C++ sources are the same TFLite Micro `microfrontend` and
# kissfft fixed-point sources pymicro-features v2.0.2 ships. We
# build them as a separate object library so we can apply
# microfrontend-specific compile flags (notably -DFIXED_POINT=16
# which kissfft hard-requires) without polluting the rest of the
# project's flags.
set(MICROFRONTEND_DIR
    "${CMAKE_SOURCE_DIR}/third_party/microfrontend/include")

set(MICROFRONTEND_SOURCES
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/fft.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/fft_util.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/filterbank.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/filterbank_util.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/frontend.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/frontend_util.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/kiss_fft_int16.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/log_lut.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/log_scale.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/log_scale_util.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/noise_reduction.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/noise_reduction_util.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/pcan_gain_control.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/pcan_gain_control_util.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/window.cc
    ${MICROFRONTEND_DIR}/tensorflow/lite/experimental/microfrontend/lib/window_util.cc
)

add_library(lva_microfrontend OBJECT ${MICROFRONTEND_SOURCES})
target_include_directories(lva_microfrontend PUBLIC
    ${MICROFRONTEND_DIR}
    ${MICROFRONTEND_DIR}/kissfft
)
target_compile_definitions(lva_microfrontend PUBLIC FIXED_POINT=16)
target_compile_options(lva_microfrontend PRIVATE
    -Wno-unused-parameter
    -Wno-unused-but-set-variable
    -Wno-unused-variable
    -Wno-pedantic
    -Wno-sign-compare
)

add_executable(linux-voice-assistant-cpp
    src/main.cpp
    src/protocol/Frame.cpp
    src/protocol/MessageRegistry.cpp
    src/protocol/Connection.cpp
    src/protocol/ApiServer.cpp
    src/protocol/MdnsPublisher.cpp
    src/state/Preferences.cpp
    src/state/ServerState.cpp
    src/audio/LibMpvPlayer.cpp
    src/audio/PcmRingBuffer.cpp
    src/audio/AudioCapture.cpp
    src/audio/SendspinPlaybackMonitor.cpp
    src/audio/ExternalWakeWord.cpp
    src/audio/MicroFeatures.cpp
    src/audio/MicroWakeWord.cpp
    src/audio/OpenWakeWord.cpp
    src/audio/OpenWakeWordFeatures.cpp
    src/audio/TfliteRuntime.cpp
    src/audio/WakeWordEngine.cpp
    src/audio/WakeWordScanner.cpp
    src/audio/WebRtcProcessor.cpp
    src/satellite/Satellite.cpp
    src/tr/HomeButton.cpp
    src/tr/LedRing.cpp
    src/tr/MusicAssistantControl.cpp
    src/tr/MicMuteGpio.cpp
    src/tr/OtaClient.cpp
    src/tr/SendspinSignal.cpp
    src/tr/SoundConfWatcher.cpp
    src/tr/Supervisor.cpp
    src/tr/SupervisorHttpServer.cpp
    src/tr/SysInfo.cpp
    src/tr/SystemVolume.cpp
    src/tr/TimeZoneSync.cpp
    src/entities/MuteSwitchEntity.cpp
    src/entities/ThinkingSoundEntity.cpp
    src/entities/NumberEntity.cpp
    src/entities/SelectEntity.cpp
    src/entities/MediaPlayerEntity.cpp
    src/entities/EventEntity.cpp
    src/entities/UpdateEntity.cpp
    src/entities/UpdateEntity_Ota.cpp
    src/util/Log.cpp
)

target_include_directories(linux-voice-assistant-cpp
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# The generated proto headers (api.pb.h) use deprecated enum values
# at file scope — the warning fires in any TU that includes them, not
# just where we call deprecated APIs. Suppress globally on the
# executable; we keep the localized push/pop in Connection.cpp for
# documentation of the one deprecated field we deliberately set.
target_compile_options(linux-voice-assistant-cpp PRIVATE
    -Wno-deprecated-declarations
)

# Threading is needed for the SIGTERM wait helper and (in later phases)
# the audio capture thread.
find_package(Threads REQUIRED)

target_link_libraries(linux-voice-assistant-cpp
    PRIVATE
        lva_proto
        lva_microfrontend
        nlohmann_json::nlohmann_json
        PkgConfig::MPV
        PkgConfig::PULSE_SIMPLE
        PkgConfig::PULSE
        PkgConfig::ALSA
        PkgConfig::WEBRTC_APM
        PkgConfig::LIBCURL
        PkgConfig::AVAHI_CLIENT
        Threads::Threads
        ${CMAKE_DL_LIBS}   # for dlopen/dlsym in TfliteRuntime
)

install(TARGETS linux-voice-assistant-cpp RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Dev tool: micro_features_dump (phase 4a verification)
# ---------------------------------------------------------------------------
# Small CLI that reads a 16 kHz mono S16LE WAV and prints C++
# MicroFeatures output as text — one frame per line, 40 floats. The
# host-side `test/verify_phase4a_features.py` compares against
# pymicro_features.
add_executable(micro_features_dump
    src/tools/micro_features_dump.cpp
    src/audio/MicroFeatures.cpp
    src/util/Log.cpp
)
target_include_directories(micro_features_dump PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(micro_features_dump PRIVATE
    lva_microfrontend
    Threads::Threads
)
install(TARGETS micro_features_dump RUNTIME DESTINATION bin)

# Dev tool: tflite_inspect (phase 4b verification)
# Prints shape/type/quant of all input/output tensors of a .tflite.
add_executable(tflite_inspect
    src/tools/tflite_inspect.cpp
    src/audio/TfliteRuntime.cpp
    src/util/Log.cpp
)
target_include_directories(tflite_inspect PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tflite_inspect PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
install(TARGETS tflite_inspect RUNTIME DESTINATION bin)

# Dev tool: wake_word_dump (phase 4c verification)
# Loads a microWakeWord JSON config and runs a WAV through the full
# C++ pipeline (features → quantize → invoke → window). Emits one
# line per invoke for cross-comparison with pymicro_wakeword.
add_executable(wake_word_dump
    src/tools/wake_word_dump.cpp
    src/audio/MicroFeatures.cpp
    src/audio/MicroWakeWord.cpp
    src/audio/TfliteRuntime.cpp
    src/util/Log.cpp
)
target_include_directories(wake_word_dump PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(wake_word_dump PRIVATE
    lva_microfrontend
    nlohmann_json::nlohmann_json
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
install(TARGETS wake_word_dump RUNTIME DESTINATION bin)

# Dev tool: aec_loopback_test
# Verifies hardware-loopback AEC using the same WebRTC APM 1.3 pipeline
# the production capture path will use once the ALSA backend lands.
# Opens an ALSA capture device (default plughw:0,4) at S16_LE 16 kHz 4ch,
# splits ch1->mic, (ch3+ch4)/2->ref, runs ProcessReverseStream then
# ProcessStream, and writes raw_mic.wav / ref.wav / aec_out.wav for
# A/B comparison. Prints per-second ERLE.
#
# Standalone TU: does not link the production AudioCapture or
# WebRtcProcessor, so the production binary is unaffected. Run with
# linux-voice-assistant-cpp stopped (or at least with no concurrent
# reader on hw:0,4 if your codec driver is exclusive).
add_executable(aec_loopback_test
    src/tools/aec_loopback_test.cpp
)
target_include_directories(aec_loopback_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aec_loopback_test PRIVATE
    PkgConfig::ALSA
    PkgConfig::WEBRTC_APM
    Threads::Threads
)
install(TARGETS aec_loopback_test RUNTIME DESTINATION bin)
