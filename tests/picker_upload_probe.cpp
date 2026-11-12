#include "mousepos.hpp"
#include "picker_xcb.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {
void original_handler(int) {}
size_t fd_count() {
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) { (void)entry; ++count; }
    return count;
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    const bool disconnect = std::strcmp(argv[1], "disconnect") == 0;
    if (!disconnect && std::strcmp(argv[1], "cancel") != 0) return 1;
    struct sigaction action {};
    action.sa_handler = original_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) < 0 || sigaction(SIGTERM, &action, nullptr) < 0) return 1;
    const size_t before = fd_count();
    std::string screenshot;
    bool error_seen = false;
    try {
        const auto result = loomwright::pick_mouse_position_with_upload_hook(false, [&](const std::string& path) {
            screenshot = path;
            if (disconnect) {
                std::puts("uploading");
                std::fflush(stdout);
                char resume = 0;
                if (::read(STDIN_FILENO, &resume, 1) != 1) throw loomwright::PickerError("probe control failed");
            } else {
                std::raise(SIGINT);
            }
        });
        if (result || disconnect) return 1;
    } catch (const loomwright::PickerError& error) {
        error_seen = std::string(error.what()).find("X11 connection closed") != std::string::npos;
        if (!disconnect || !error_seen) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
    if (screenshot.empty() || ::access(screenshot.c_str(), F_OK) == 0 || fd_count() != before ||
        (disconnect && !error_seen)) return 1;
    for (int signal : {SIGINT, SIGTERM}) {
        struct sigaction after {};
        if (sigaction(signal, nullptr, &after) < 0 || after.sa_handler != original_handler) return 1;
    }
    std::puts(disconnect ? "ok   upload disconnect releases fd and screenshot" :
                           "ok   upload cancellation releases fd and screenshot");
    return 0;
}
