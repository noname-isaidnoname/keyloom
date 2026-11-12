#pragma once

#include <memory>
#include <xcb/xcb.h>

namespace loomwright {

/// An XCB connection that can report server loss without Xlib's fatal I/O path.
class XcbConnection {
public:
    explicit XcbConnection(const char* display = nullptr);
    XcbConnection(const XcbConnection&) = delete;
    XcbConnection& operator=(const XcbConnection&) = delete;

    xcb_connection_t* get() const { return connection_.get(); }
    int screen_number() const { return screen_; }
    int fd() const;
    int error() const;
    void close() noexcept { connection_.reset(); }

private:
    struct Disconnect {
        void operator()(xcb_connection_t* connection) const noexcept;
    };
    std::unique_ptr<xcb_connection_t, Disconnect> connection_;
    int screen_ = 0;
};

}  // namespace loomwright
