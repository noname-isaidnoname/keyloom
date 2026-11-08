#include "gui.hpp"
#include "errors.hpp"

#include <cstdio>
#include <filesystem>
#include <unistd.h>

static size_t fd_count() {
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++count;
    }
    return count;
}

int main() {
    const size_t before = fd_count();
    std::puts("ready");
    std::fflush(stdout);
    char start = 0;
    if (::read(STDIN_FILENO, &start, 1) != 1) return 1;
    std::puts("creating");
    std::fflush(stdout);
    try {
        keyloom::GuiPanel panel;
        panel.title = "Interrupted startup";
        auto host = keyloom::GuiHost::create(panel);
        std::fprintf(stderr, "GUI startup unexpectedly succeeded\n");
        return 1;
    } catch (const keyloom::RuntimeErrorKeyloom&) {
    }
    if (fd_count() != before) {
        std::fprintf(stderr, "GUI startup leaked an fd\n");
        return 1;
    }
    std::puts("ok   interrupted GUI startup reports failure and closes its fd");
    return 0;
}
