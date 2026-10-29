cmake_minimum_required(VERSION 3.16)
project(linux-voice-assistant-cpp
        VERSION 0.0.1
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

# Protobuf codegen. FindProtobuf mixes host/target when cross-compiling, so
# use protoc from PATH and the target libprotobuf via pkg-config.

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

find_package(nlohmann_json CONFIG REQUIRED)

pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
message(STATUS "Found mpv ${MPV_VERSION} via pkg-config")

pkg_check_modules(PULSE_SIMPLE REQUIRED IMPORTED_TARGET libpulse-simple)
message(STATUS "Found libpulse-simple ${PULSE_SIMPLE_VERSION} via pkg-config")

pkg_check_modules(WEBRTC_APM REQUIRED IMPORTED_TARGET webrtc-audio-processing-1)
message(STATUS "Found webrtc-audio-processing ${WEBRTC_APM_VERSION} via pkg-config")

pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
message(STATUS "Found libcurl ${LIBCURL_VERSION} via pkg-config")

pkg_check_modules(AVAHI_CLIENT REQUIRED IMPORTED_TARGET avahi-client)
message(STATUS "Found avahi-client ${AVAHI_CLIENT_VERSION} via pkg-config")

pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
message(STATUS "Found alsa-lib ${ALSA_VERSION} via pkg-config")

pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
message(STATUS "Found libpulse ${PULSE_VERSION} via pkg-config")

# Static, staging only. Its Cflags carry the role defines that the public
# headers depend on.
pkg_check_modules(SENDSPIN REQUIRED IMPORTED_TARGET sendspin-cpp)
message(STATUS "Found sendspin-cpp ${SENDSPIN_VERSION} via pkg-config")

find_package(Threads REQUIRED)

# Vendored TFLite Micro microfrontend + kissfft. Separate object library so
# FIXED_POINT=16 (required by kissfft) stays local.
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

# lva_speech: on-device STT (whisper.cpp) + TTS (eSpeak-NG) from prebuilt
# archives (third_party/*/README.md). --exclude-libs keeps their symbols private.
set(WHISPER_CPP_DIR "${CMAKE_SOURCE_DIR}/third_party/whisper.cpp")
add_library(whisper_cpp INTERFACE)
target_include_directories(whisper_cpp INTERFACE "${WHISPER_CPP_DIR}/include")
target_link_libraries(whisper_cpp INTERFACE
    -Wl,--start-group
    "${WHISPER_CPP_DIR}/lib/libwhisper.a"
    "${WHISPER_CPP_DIR}/lib/libggml.a"
    "${WHISPER_CPP_DIR}/lib/libggml-cpu.a"
    "${WHISPER_CPP_DIR}/lib/libggml-base.a"
    -Wl,--end-group
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
)

set(ESPEAK_NG_DIR "${CMAKE_SOURCE_DIR}/third_party/espeak-ng")
add_library(espeak_ng INTERFACE)
target_include_directories(espeak_ng INTERFACE "${ESPEAK_NG_DIR}/include")
target_link_libraries(espeak_ng INTERFACE
    "${ESPEAK_NG_DIR}/lib/libespeak-ng.a"
    "${ESPEAK_NG_DIR}/lib/libucd.a"
)

# sanoTTS piperlite runtime (third_party/sanotts/README.md).
set(SANOTTS_DIR "${CMAKE_SOURCE_DIR}/third_party/sanotts")
add_library(sanotts STATIC
    ${SANOTTS_DIR}/src/snt_front_f32.c
    ${SANOTTS_DIR}/src/snt_piperlite.c
)
set_target_properties(sanotts PROPERTIES
    LINKER_LANGUAGE C
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99
)
target_include_directories(sanotts PUBLIC ${SANOTTS_DIR}/include)
target_compile_options(sanotts PRIVATE -O3 -ffast-math -Wno-pedantic)
target_link_libraries(sanotts PUBLIC m)

add_library(lva_speech SHARED
    src/audio/WhisperStt.cpp
    src/audio/SanoTts.cpp
)
target_include_directories(lva_speech PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lva_speech PRIVATE whisper_cpp espeak_ng sanotts
    Threads::Threads)
target_link_options(lva_speech PRIVATE
    -Wl,--exclude-libs,ALL
    -Wl,-Bsymbolic
)
install(TARGETS lva_speech LIBRARY DESTINATION lib)

add_executable(linux-voice-assistant-cpp
    src/main.cpp
    src/protocol/Frame.cpp
    src/protocol/MessageRegistry.cpp
    src/protocol/Connection.cpp
    src/protocol/ApiServer.cpp
    src/protocol/MdnsPublisher.cpp
    src/state/Preferences.cpp
    src/state/ServerState.cpp
    src/state/ServerState_Notify.cpp
    src/audio/LibMpvPlayer.cpp
    src/audio/PcmRingBuffer.cpp
    src/audio/AudioCapture.cpp
    src/audio/ExternalWakeWord.cpp
    src/audio/MicroFeatures.cpp
    src/audio/MicroWakeWord.cpp
    src/audio/OpenWakeWord.cpp
    src/audio/OpenWakeWordFeatures.cpp
    src/audio/TfliteRuntime.cpp
    src/audio/WakeWordEngine.cpp
    src/audio/WakeWordScanner.cpp
    src/audio/WebRtcProcessor.cpp
    src/audio/WavFile.cpp
    src/satellite/ConversationReply.cpp
    src/satellite/LocalStt.cpp
    src/satellite/Satellite.cpp
    src/sendspin/SendspinHub.cpp
    src/tr/ControlListener.cpp
    src/tr/LedRing.cpp
    src/tr/MicMuteGpio.cpp
    src/tr/ModelDownloader.cpp
    src/tr/OtaClient.cpp
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
    src/entities/SendspinSwitchEntity.cpp
    src/entities/MediaPlayerEntity.cpp
    src/entities/EventEntity.cpp
    src/entities/UpdateEntity.cpp
    src/entities/UpdateEntity_Ota.cpp
    src/util/Log.cpp
    src/util/Sha256.cpp
)

target_include_directories(linux-voice-assistant-cpp
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# Diagnostic logging (memprobe sampler, Sendspin audio stats, AEC rebuild
# memory). Off by default; see src/util/Diag.h.
option(LVA_DIAG_LOG "Compile in diagnostic logging" OFF)
if(LVA_DIAG_LOG)
    target_sources(linux-voice-assistant-cpp PRIVATE src/util/MemProbe.cpp)
    target_compile_definitions(linux-voice-assistant-cpp PRIVATE LVA_DIAG_LOG=1)
endif()

# api.pb.h uses deprecated enum values at file scope, so the warning fires in
# every TU that includes it.
target_compile_options(linux-voice-assistant-cpp PRIVATE
    -Wno-deprecated-declarations
)

target_link_libraries(linux-voice-assistant-cpp
    PRIVATE
        lva_proto
        lva_microfrontend
        nlohmann_json::nlohmann_json
        PkgConfig::MPV
        PkgConfig::PULSE_SIMPLE
        PkgConfig::ALSA
        PkgConfig::WEBRTC_APM
        PkgConfig::LIBCURL
        PkgConfig::AVAHI_CLIENT
        PkgConfig::PULSE
        PkgConfig::SENDSPIN
        lva_speech
        Threads::Threads
        ${CMAKE_DL_LIBS}
)

install(TARGETS linux-voice-assistant-cpp RUNTIME DESTINATION bin)

# Dev tool: dump MicroFeatures for a WAV (one 40-float frame per line).
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

# Dev tool: print tensor shapes/types/quantization of a .tflite.
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

# Dev tool: run a WAV through a microWakeWord model, one line per invoke.
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

# Dev tool: hardware-loopback AEC check (writes mic/ref/AEC WAVs, prints ERLE).
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

# Unit test: exit 0 = pass.
add_executable(conversation_reply_test
    src/tools/conversation_reply_test.cpp
    src/satellite/ConversationReply.cpp
)
target_include_directories(conversation_reply_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(conversation_reply_test PRIVATE
    nlohmann_json::nlohmann_json
)

# Dev tool: run WAV files through WhisperStt.
add_executable(stt_wav_test
    src/tools/stt_wav_test.cpp
    src/util/Log.cpp
)
target_include_directories(stt_wav_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(stt_wav_test PRIVATE lva_speech Threads::Threads)
install(TARGETS stt_wav_test RUNTIME DESTINATION bin)

# Dev tool: synthesize text with SanoTts to WAV files.
add_executable(tts_wav_test
    src/tools/tts_wav_test.cpp
    src/audio/WavFile.cpp
    src/util/Log.cpp
)
target_include_directories(tts_wav_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tts_wav_test PRIVATE lva_speech Threads::Threads)
install(TARGETS tts_wav_test RUNTIME DESTINATION bin)
