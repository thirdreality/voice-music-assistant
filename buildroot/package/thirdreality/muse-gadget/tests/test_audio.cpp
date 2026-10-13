#include "AudioCapture.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL " << message << '\n';
        std::exit(1);
    }
}
}

int main() {
    const int16_t input[] = {100, 200, 300, 400, -100, -200, -300, -400};
    int16_t mono[2]{};
    muse::SelectMicrophoneChannel(input, 2, mono);
    Check(mono[0] == 100 && mono[1] == -100,
          "select mic0 without mixing in the other mic or playback references");
    // The ALSA null capture plugin does not use a physical microphone.
    muse::AudioCapture capture("null");
    for (int i = 0; i < 3; ++i) {
        Check(capture.Start([](const int16_t*, size_t) { return false; }),
              "start capture after callback stopped the preceding worker");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (capture.active() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Check(!capture.active(), "capture callback must stop its worker");
    }
    capture.Stop();
    Check(capture.Start([](const int16_t*, size_t) { return true; }),
          "restart after explicit stop");
    Check(!capture.Start([](const int16_t*, size_t) { return true; }),
          "reject concurrent recording");
    capture.Stop();
    capture.Stop();
    std::cout << "PASS muse-gadget audio lifecycle tests\n";
    return 0;
}
