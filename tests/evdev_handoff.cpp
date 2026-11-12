#include "input.hpp"
#include "keys.hpp"
#include "subprocess.hpp"
#include "uinput.hpp"

#include <filesystem>
#include <iostream>
#include <linux/input.h>
#include <linux/uinput.h>
#include <memory>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace loomwright;

namespace {

// A lone BTN_0 is not a keyboard, mouse, or gamepad. We also
// require udev to confirm that it has no input-device classification before
// emitting anything. All writes happen while a test reader owns EVIOCGRAB.
constexpr int kTestCode = BTN_0;

std::string event_path(int fd) {
    char sysname[64]{};
    if (::ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0) return {};
    const std::filesystem::path directory = std::filesystem::path("/sys/devices/virtual/input") / sysname;
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("event", 0) == 0 && ::access(("/dev/input/" + name).c_str(), R_OK) == 0)
                return "/dev/input/" + name;
        }
        ::poll(nullptr, 0, 10);
    }
    return {};
}

bool unclassified(const std::string& path) {
    if (which("udevadm").empty()) return false;
    const std::string name = std::filesystem::path(path).filename().string();
    const auto result = run_command({"udevadm", "info", "--query=property", "--path=/sys/class/input/" + name}, "", 3000);
    if (result.returncode != 0) return false;
    bool input = false;
    std::istringstream lines(result.out);
    for (std::string line; std::getline(lines, line);) {
        if (line == "ID_INPUT=1") input = true;
        if (line.rfind("ID_INPUT_", 0) == 0 && line.size() >= 2 && line.substr(line.size() - 2) == "=1") {
            std::cout << "skip: test device classified as " << line << '\n';
            return false;
        }
    }
    return input;
}

bool saw_value(int fd, int expected) {
    pollfd descriptor{fd, POLLIN, 0};
    if (::poll(&descriptor, 1, 1000) <= 0) return false;
    input_event events[32]{};
    const ssize_t bytes = ::read(fd, events, sizeof(events));
    if (bytes <= 0) return false;
    for (size_t i = 0; i < static_cast<size_t>(bytes) / sizeof(input_event); ++i)
        if (events[i].type == EV_KEY && events[i].code == kTestCode && events[i].value == expected) return true;
    return false;
}

void emit(UInputDevice& device, int value) {
    device.write(EV_KEY, kTestCode, value);
    device.write(EV_SYN, SYN_REPORT, 0);
    device.flush();
}

struct Safety {
    UInputDevice& writer;
    InputDevice& observer;
    InputDevice& target;
    bool observer_grabbed = false;
    bool target_grabbed = false;
    bool down = false;
    ~Safety() {
        if (down) {
            try {
                if (!observer_grabbed && !target_grabbed) { observer.grab(); observer_grabbed = true; }
                // Never emit a cleanup event unless a test reader owns the grab.
                if (observer_grabbed || target_grabbed) emit(writer, 0);
            } catch (...) {}
        }
        target.ungrab();
        observer.ungrab();
    }
};

}  // namespace

int main() {
    if (::access("/dev/uinput", W_OK) != 0 || ::access("/dev/input", R_OK) != 0) {
        std::cout << "skip: uinput or evdev access unavailable\n";
        return 77;
    }
    bool isolated = false;
    try {
        UInputDevice writer("Loomwright isolated handoff test", "loomwright/test/handoff", {kTestCode}, {}, {});
        const std::string path = event_path(writer.fd());
        if (path.empty() || !unclassified(path)) {
            std::cout << "skip: test device is not confirmed unclassified and readable (path: " << path << ")\n";
            return 77;
        }
        auto observer = InputDevice::open(path);
        auto target = InputDevice::open(path);
        if (!observer || !target) return 77;
        Safety safety{writer, *observer, *target};
        observer->grab();
        safety.observer_grabbed = true;
        isolated = true;
        bool observer_saw_down = false;
        std::string error;
        try {
            grab_idle_input([&] { return target->pressed_key_codes(); },
                            [&] { return target->discard_pending_events(); },
                            [&] {
                                // The test observer represents the desktop; no
                                // event is emitted while neither reader is grabbed.
                                safety.down = true;
                                emit(writer, 1);
                                observer_saw_down = saw_value(observer->fd(), 1);
                                observer->ungrab();
                                safety.observer_grabbed = false;
                                target->grab();
                                safety.target_grabbed = true;
                                emit(writer, 0);
                                safety.down = false;
                            },
                            [&] { target->ungrab(); safety.target_grabbed = false; });
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        const auto name = normalize_key_event(kTestCode);
        const std::string label = name ? *name : "code " + std::to_string(kTestCode);
        if (!observer_saw_down || error.find("release of " + label) == std::string::npos ||
            error.find("physically tap and release") == std::string::npos || safety.target_grabbed) {
            std::cerr << "handoff detection or rollback failed: " << error << '\n';
            return 1;
        }
        pollfd descriptor{observer->fd(), POLLIN, 0};
        if (::poll(&descriptor, 1, 50) > 0 && saw_value(observer->fd(), 0)) {
            std::cerr << "observer unexpectedly received the grabbed release\n";
            return 1;
        }
        if (!target->pressed_key_codes().empty()) {
            std::cerr << "test input remained held after rollback\n";
            return 1;
        }
        std::cout << "ok   isolated evdev split handoff reports the intercepted release\n";
        return 0;
    } catch (const std::exception& error) {
        if (!isolated) {
            std::cout << "skip: isolated evdev setup unavailable: " << error.what() << '\n';
            return 77;
        }
        std::cerr << "isolated evdev handoff: " << error.what() << '\n';
        return 1;
    }
}
