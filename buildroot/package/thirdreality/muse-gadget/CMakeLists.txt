cmake_minimum_required(VERSION 3.16)
project(muse-gadget LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(OPENSSL REQUIRED IMPORTED_TARGET openssl)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(muse-gadget-core STATIC
    src/ConfigStore.cpp
    src/OpenSslCryptoBackend.cpp
    src/MuseClient.cpp
    src/PairingSession.cpp
    src/WavWriter.cpp
    src/ReplyTracker.cpp
    src/WyomingTts.cpp
    third_party/noise_core/src/ClientSession.cpp
    third_party/noise_core/src/InitiatorHandshake.cpp
    third_party/noise_core/src/ServiceCodec.cpp
    third_party/noise_core/src/Status.cpp
    third_party/noise_core/src/Transport.cpp
    third_party/noise_core/src/TransportFrameCodec.cpp
)
target_include_directories(muse-gadget-core PUBLIC
    src
    third_party/noise_core/include
)
target_compile_options(muse-gadget-core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(muse-gadget-core PUBLIC
    nlohmann_json::nlohmann_json
    PkgConfig::OPENSSL
    PkgConfig::LIBCURL
    PkgConfig::PULSE
)

add_executable(muse-gadget
    src/main.cpp
    src/AudioCapture.cpp
)
target_compile_definitions(muse-gadget PRIVATE
    MUSE_GADGET_VERSION="${MUSE_GADGET_VERSION}"
)
target_compile_options(muse-gadget PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(muse-gadget PRIVATE
    muse-gadget-core
    PkgConfig::ALSA
    Threads::Threads
)

install(TARGETS muse-gadget RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(muse-gadget-tests tests/test_main.cpp)
    target_link_libraries(muse-gadget-tests PRIVATE muse-gadget-core)
    add_test(NAME muse-gadget-tests COMMAND muse-gadget-tests)

    add_executable(muse-gadget-crypto-tests tests/test_crypto.cpp)
    target_link_libraries(muse-gadget-crypto-tests PRIVATE muse-gadget-core)
    add_test(NAME muse-gadget-crypto-tests COMMAND muse-gadget-crypto-tests)

    add_executable(muse-gadget-audio-tests tests/test_audio.cpp src/AudioCapture.cpp)
    target_include_directories(muse-gadget-audio-tests PRIVATE src)
    target_link_libraries(muse-gadget-audio-tests PRIVATE PkgConfig::ALSA Threads::Threads)
    add_test(NAME muse-gadget-audio-tests COMMAND muse-gadget-audio-tests)

    add_executable(muse-gadget-button-tests tests/test_buttons.cpp)
    target_include_directories(muse-gadget-button-tests PRIVATE ../tr-buttond/src)
    target_link_options(muse-gadget-button-tests PRIVATE
        -Wl,--wrap=access -Wl,--wrap=system -Wl,--wrap=sendto)
    add_test(NAME muse-gadget-button-tests COMMAND muse-gadget-button-tests)

    add_executable(muse-gadget-reply-tests tests/test_reply.cpp)
    target_link_libraries(muse-gadget-reply-tests PRIVATE muse-gadget-core)
    add_test(NAME muse-gadget-reply-tests COMMAND muse-gadget-reply-tests)

    add_executable(muse-gadget-tts-tests tests/test_tts.cpp)
    target_link_libraries(muse-gadget-tts-tests PRIVATE muse-gadget-core Threads::Threads)
    add_test(NAME muse-gadget-tts-tests COMMAND muse-gadget-tts-tests)
endif()
