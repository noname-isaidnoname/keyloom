#include <xcb/xcb.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

#include "xcb_connection.hpp"

int main() {
    keyloom::XcbConnection connection;
    const int fd = connection.fd();
    std::puts("ready");
    std::fflush(stdout);
    char start = 0;
    if (::read(STDIN_FILENO, &start, 1) != 1) {
        return 1;
    }
    const auto cookie = xcb_get_input_focus(connection.get());
    xcb_flush(connection.get());
    std::puts("requesting");
    std::fflush(stdout);
    xcb_get_input_focus_reply_t* reply = xcb_get_input_focus_reply(connection.get(), cookie, nullptr);
    const bool failed = reply == nullptr && connection.error() != 0;
    std::free(reply);
    connection.close();
    connection.close();
    errno = 0;
    const bool fd_closed = ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
    if (!failed || !fd_closed) {
        std::fprintf(stderr, "XCB did not report failure and release its connection fd\n");
        return 1;
    }
    std::puts("ok   XCB reports in-flight server loss and closes its fd");
    return 0;
}
