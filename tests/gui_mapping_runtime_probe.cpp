// Exercises GuiHost MappingNotify handling: runs an event loop while the test
// harness changes XKB layout via setxkbmap (triggering MappingNotify). Exits 0
// if the GUI stays alive and processes events without error.
#include "gui.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>

int main() {
    keyloom::GuiPanel panel;
    panel.title = "Mapping probe";
    panel.widgets.push_back({keyloom::GuiWidget::Kind::Label, "label", "mapping probe", 0});
    panel.widgets.push_back({keyloom::GuiWidget::Kind::Input, "text", "", 1});

    auto host = keyloom::GuiHost::create(panel);
    if (!host) {
        std::fprintf(stderr, "failed to create GUI host\n");
        return 1;
    }
    host->set_visible(true);

    // Run while the harness changes the keyboard layout, processing events.
    // The harness switches the layout twice and waits for this probe, so the
    // probe only has to outlive those changes, not a fixed generous window.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < deadline) {
        host->process_events();
        if (host->is_closed()) {
            std::fprintf(stderr, "GUI closed unexpectedly during mapping probe\n");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    host->set_visible(false);
    host->close();
    std::puts("ok   GUI survives runtime MappingNotify/layout changes");
    return 0;
}
