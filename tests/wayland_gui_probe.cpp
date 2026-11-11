#include "gui.hpp"

#include <cstdlib>
#include <cstdio>
#include <poll.h>
#include <unistd.h>

int main() {
    using namespace keyloom;
    validate_compositor("wayland");
    if (resolve_compositor() != "wayland") return 1;
    ::setenv("KEYLOOM_WAYLAND_TEST_INTERACTION", "1", 1);
    GuiPanel panel{"Native Wayland test", {
        {GuiWidget::Kind::Label, "", "Name", 1},
        {GuiWidget::Kind::Input, "name", "initial", 1},
        {GuiWidget::Kind::Button, "submit", "Submit", 1}}, 1};
    int pipefd[2];
    if (::pipe(pipefd) != 0) return 1;
    auto host = GuiHost::create(panel, "wayland");
    ::close(pipefd[1]);
    pollfd pipe_read{pipefd[0], POLLIN | POLLHUP, 0};
    if (::poll(&pipe_read, 1, 1000) <= 0 || !(pipe_read.revents & POLLHUP)) return 1;
    ::close(pipefd[0]);
    if (host->value("name") != "initial") return 1;
    host->set_text("name", "updated");
    if (host->value("name") != "updated") return 1;
    int clicks = 0, closes = 0;
    host->set_event_handler([&](const GuiHost::Event& event) {
        if (event.kind == GuiHost::EventKind::Click && event.widget_id == "submit") ++clicks;
        if (event.kind == GuiHost::EventKind::Close) ++closes;
    });
    host->set_visible(false);
    host->set_visible(true);
    for (int i = 0; i < 100 && !host->is_closed(); ++i) {
        pollfd fd{host->fd(), POLLIN, 0};
        if (::poll(&fd, 1, 100) > 0) host->process_events();
    }
    if (!host->is_closed() || clicks != 1 || closes != 1 || host->value("name") != "typed") return 1;
    host->set_text("name", "after close");
    if (host->value("name") != "after close") return 1;
    std::puts("ok   native Wayland panel input, click, visibility and close");
}
