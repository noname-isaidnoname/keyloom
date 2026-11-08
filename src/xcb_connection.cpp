#include "xcb_connection.hpp"

#include <string>

#include "errors.hpp"

namespace keyloom {

void XcbConnection::Disconnect::operator()(xcb_connection_t* connection) const noexcept {
    if (connection != nullptr) xcb_disconnect(connection);
}

XcbConnection::XcbConnection(const char* display) {
    connection_.reset(xcb_connect(display, &screen_));
    if (!connection_) throw RuntimeErrorKeyloom("cannot allocate XCB connection");
    const int status = xcb_connection_has_error(connection_.get());
    if (status != 0) throw RuntimeErrorKeyloom("cannot open XCB display (error " + std::to_string(status) + ")");
}

int XcbConnection::fd() const {
    return connection_ ? xcb_get_file_descriptor(connection_.get()) : -1;
}

int XcbConnection::error() const {
    return connection_ ? xcb_connection_has_error(connection_.get()) : XCB_CONN_ERROR;
}

}  // namespace keyloom
