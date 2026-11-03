#include "uinput.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "errors.hpp"

namespace keyloom {
namespace {

void check_device_node() {
    if (::access("/dev/uinput", F_OK) != 0) {
        throw BackendError(
            "/dev/uinput is unavailable. Load the uinput kernel module and grant access, or use ydotool.");
    }
    if (::access("/dev/uinput", W_OK) != 0) {
        throw BackendError(
            "/dev/uinput is not writable. Add your user to the 'input' group or install a udev rule, then log "
            "out and back in.");
    }
}

[[noreturn]] void throw_ioctl(const char* what) {
    const int saved = errno;
    throw BackendError(std::string("Unable to create /dev/uinput device: ") + what + ": " + std::strerror(saved));
}

void set_key_bits(int fd, const std::vector<int>& keys) {
    for (int code : keys) {
        if (::ioctl(fd, UI_SET_KEYBIT, code) < 0) throw_ioctl("UI_SET_KEYBIT");
    }
}

void set_abs_axes(int fd, const std::vector<AbsAxis>& axes) {
    for (const auto& axis : axes) {
        if (::ioctl(fd, UI_SET_ABSBIT, axis.code) < 0) throw_ioctl("UI_SET_ABSBIT");
        uinput_abs_setup setup{};
        setup.code = static_cast<__u16>(axis.code);
        setup.absinfo.minimum = axis.minimum;
        setup.absinfo.maximum = axis.maximum;
        setup.absinfo.fuzz = axis.fuzz;
        setup.absinfo.flat = axis.flat;
        setup.absinfo.resolution = axis.resolution;
        if (::ioctl(fd, UI_ABS_SETUP, &setup) < 0) throw_ioctl("UI_ABS_SETUP");
    }
}

/// libinput opens a freshly created uinput device asynchronously. Anything sent
/// before that is folded into the device's initial state instead of being
/// delivered: the first `cursor warp` of a script would silently do nothing, and
/// a replayed key could be swallowed. Wait for udev to finish with the device
/// (it writes this database entry while processing the add uevent) and then give
/// libinput a moment to open the node. Without udev, fall back to a fixed delay.
void wait_for_reader(int fd) {
    bool udev_seen = false;
    char sysname[64] = {};
    if (::ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) >= 0 && sysname[0] != '\0' &&
        ::access("/run/udev", F_OK) == 0) {
        const std::string database = std::string("/run/udev/data/+input:") + sysname;
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (::access(database.c_str(), F_OK) == 0) {
                udev_seen = true;
                break;
            }
            ::poll(nullptr, 0, 5);
        }
    }
    ::poll(nullptr, 0, udev_seen ? 40 : 150);
}

}  // namespace

UInputDevice::UInputDevice(const std::string& name, const std::string& phys, const std::vector<int>& keys,
                           const std::vector<int>& relative, const std::vector<AbsAxis>& absolute) {
    check_device_node();

    fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) throw_ioctl("open");

    if (::ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0) throw_ioctl("UI_SET_EVBIT EV_KEY");
    if (!relative.empty() && ::ioctl(fd_, UI_SET_EVBIT, EV_REL) < 0) throw_ioctl("UI_SET_EVBIT EV_REL");
    if (!absolute.empty() && ::ioctl(fd_, UI_SET_EVBIT, EV_ABS) < 0) throw_ioctl("UI_SET_EVBIT EV_ABS");

    set_key_bits(fd_, keys);
    for (int code : relative) {
        if (::ioctl(fd_, UI_SET_RELBIT, code) < 0) throw_ioctl("UI_SET_RELBIT");
    }
    set_abs_axes(fd_, absolute);

    // uinput_setup carries only the name; the physical path goes through
    // UI_SET_PHYS.
    if (!phys.empty()) {
        if (::ioctl(fd_, UI_SET_PHYS, const_cast<char*>(phys.c_str())) < 0) throw_ioctl("UI_SET_PHYS");
    }
    uinput_setup setup{};
    std::snprintf(setup.name, sizeof(setup.name), "%s", name.c_str());
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x4b4c;  // "KL"
    setup.id.product = 0x0001;
    setup.id.version = 1;
    if (::ioctl(fd_, UI_DEV_SETUP, &setup) < 0) throw_ioctl("UI_DEV_SETUP");
    if (::ioctl(fd_, UI_DEV_CREATE) < 0) throw_ioctl("UI_DEV_CREATE");
    wait_for_reader(fd_);
}

UInputDevice::~UInputDevice() { close(); }

void UInputDevice::write(int type, int code, int value) {
    if (fd_ < 0) return;
    input_event event{};
    event.type = static_cast<__u16>(type);
    event.code = static_cast<__u16>(code);
    event.value = value;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&event);
    buffer_.insert(buffer_.end(), bytes, bytes + sizeof(input_event));
    // Bound latency if a caller forgets to flush.
    if (buffer_.size() >= sizeof(input_event) * 64) flush();
}

void UInputDevice::flush() {
    if (fd_ < 0 || buffer_.empty()) return;
    size_t written = 0;
    while (written < buffer_.size()) {
        const ssize_t count = ::write(fd_, buffer_.data() + written, buffer_.size() - written);
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // uinput drains quickly; a short spin keeps report ordering intact.
            continue;
        }
        break;
    }
    buffer_.clear();
}

void UInputDevice::close() {
    if (fd_ < 0) return;
    flush();
    ::ioctl(fd_, UI_DEV_DESTROY);
    ::close(fd_);
    fd_ = -1;
}

std::unique_ptr<VirtualInput> create_uinput_from_capabilities(const std::string& name, const std::string& phys,
                                                             const DeviceCapabilities& capabilities) {
    return std::make_unique<UInputDevice>(name, phys, capabilities.keys, capabilities.relative,
                                          capabilities.absolute);
}

}  // namespace keyloom
