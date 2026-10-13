// Exercise the real button dispatch without running system actions or services.
#define main buttond_main
#include "tr-buttond.cpp"
#undef main

#include <vector>

namespace {
bool paired = true;
int system_actions = 0;
std::vector<std::string> commands;
void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}
}

extern "C" int __wrap_access(const char*, int) { return paired ? 0 : -1; }
extern "C" int __wrap_system(const char*) { ++system_actions; return 0; }
extern "C" ssize_t __wrap_sendto(int, const void* data, size_t size, int,
                                 const sockaddr*, socklen_t) {
    commands.emplace_back(static_cast<const char*>(data), size);
    return static_cast<ssize_t>(size);
}

int main() {
    GestureKey tap(kTapWindowTapMs, "longpressTap", OnTap);
    GestureKey home(kTapWindowHomeMs, "longpressHome", OnHome);
    SimpleKey up("Volup", "longpressVolup"), down("Voldown", "longpressVoldown"),
              mute("Mute", "longpressMute");
    input_event event{};
    event.type = EV_KEY;
    event.code = kKeyHome;
    event.value = 1;
    HandleKey(event, 100, tap, home, up, down, mute);
    home.Tick(10100);
    Check(system_actions == 0, "Muse PTT hold must not trigger Home system action");
    event.value = 2;
    HandleKey(event, 10200, tap, home, up, down, mute);
    event.value = 0;
    HandleKey(event, 11000, tap, home, up, down, mute);
    home.Tick(12000);
    Check(commands == std::vector<std::string>({"BUTTON_DOWN", "BUTTON_UP"}),
          "paired Home gesture must route only one press and release to Muse");

    paired = false;
    commands.clear();
    event.value = 1;
    HandleKey(event, 20000, tap, home, up, down, mute);
    event.value = 0;
    HandleKey(event, 20010, tap, home, up, down, mute);
    home.Tick(20510);
    Check(commands == std::vector<std::string>({"HOME_SINGLE"}),
          "unpaired Home must preserve Home Assistant dispatch");
    event.value = 1;
    HandleKey(event, 30000, tap, home, up, down, mute);
    home.Tick(40000);
    Check(system_actions == 1, "unpaired Home must preserve existing long press");
    std::puts("PASS muse-gadget button routing tests");
    return 0;
}
