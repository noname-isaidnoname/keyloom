#include "uinput.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "errors.hpp"
#include "unique_fd.hpp"

namespace loomwright {
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

void UInputDevice::setup(int fd, const std::string& name, const std::string& phys,
                          const std::vector<int>& keys, const std::vector<int>& relative,
                          const std::vector<AbsAxis>& absolute, const UInputSetupStep& step_hook) {
    const auto step = [&](const char* label, auto&& action) {
        if (step_hook) {
            const auto decision = step_hook(label);
            if (decision.has_value()) {
                if (*decision) {
                    errno = EIO;
                    throw_ioctl(label);
                }
                return;  // simulated success
            }
        }
        if (!action()) throw_ioctl(label);
    };

    step("UI_SET_EVBIT EV_KEY", [&] { return ::ioctl(fd, UI_SET_EVBIT, EV_KEY) >= 0; });
    if (!relative.empty()) step("UI_SET_EVBIT EV_REL", [&] { return ::ioctl(fd, UI_SET_EVBIT, EV_REL) >= 0; });
    if (!absolute.empty()) step("UI_SET_EVBIT EV_ABS", [&] { return ::ioctl(fd, UI_SET_EVBIT, EV_ABS) >= 0; });

    for (int code : keys) step("UI_SET_KEYBIT", [&] { return ::ioctl(fd, UI_SET_KEYBIT, code) >= 0; });
    for (int code : relative) step("UI_SET_RELBIT", [&] { return ::ioctl(fd, UI_SET_RELBIT, code) >= 0; });
    for (const auto& axis : absolute) {
        step("UI_SET_ABSBIT", [&] { return ::ioctl(fd, UI_SET_ABSBIT, axis.code) >= 0; });
        uinput_abs_setup abs{};
        abs.code = static_cast<__u16>(axis.code);
        abs.absinfo.minimum = axis.minimum;
        abs.absinfo.maximum = axis.maximum;
        abs.absinfo.fuzz = axis.fuzz;
        abs.absinfo.flat = axis.flat;
        abs.absinfo.resolution = axis.resolution;
        step("UI_ABS_SETUP", [&] { return ::ioctl(fd, UI_ABS_SETUP, &abs) >= 0; });
    }

    // uinput_setup carries only the name; the physical path goes through
    // UI_SET_PHYS.
    if (!phys.empty()) {
        step("UI_SET_PHYS", [&] { return ::ioctl(fd, UI_SET_PHYS, const_cast<char*>(phys.c_str())) >= 0; });
    }
    uinput_setup dev{};
    std::snprintf(dev.name, sizeof(dev.name), "%s", name.c_str());
    dev.id.bustype = BUS_USB;
    dev.id.vendor = 0x4b4c;  // "KL"
    dev.id.product = 0x0001;
    dev.id.version = 1;
    step("UI_DEV_SETUP", [&] { return ::ioctl(fd, UI_DEV_SETUP, &dev) >= 0; });
    step("UI_DEV_CREATE", [&] { return ::ioctl(fd, UI_DEV_CREATE) >= 0; });
    if (step_hook) return;  // simulated setup; no real device/reader to wait for
    wait_for_reader(fd);
}

UInputDevice::UInputDevice(const std::string& name, const std::string& phys, const std::vector<int>& keys,
                           const std::vector<int>& relative, const std::vector<AbsAxis>& absolute) {
    check_device_node();

    UniqueFd fd(::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC));
    if (fd.get() < 0) throw_ioctl("open");
    setup(fd.get(), name, phys, keys, relative, absolute);
    fd_ = fd.release();
}

UInputDevice::UInputDevice(AdoptUInputFd, int fd_raw, const std::string& name, const std::string& phys,
                           const std::vector<int>& keys, const std::vector<int>& relative,
                           const std::vector<AbsAxis>& absolute, UInputSetupStep step_hook) {
    UniqueFd fd(fd_raw);
    if (fd.get() < 0) throw BackendError("invalid uinput fd");
    setup(fd.get(), name, phys, keys, relative, absolute, step_hook);
    fd_ = fd.release();
}

UInputDevice::~UInputDevice() {
    try { close(); } catch (...) {}
}

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

void flush_uinput_buffer(int fd, std::vector<unsigned char>& buffer, const UInputWriter& writer) {
    size_t written = 0;
    while (written < buffer.size()) {
        const ssize_t count = writer(fd, buffer.data() + written, buffer.size() - written);
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        const int error = count == 0 ? EIO : errno;
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(written));
        throw BackendError("uinput write failed: " + std::string(std::strerror(error)));
    }
    buffer.clear();
}

void UInputDevice::flush() {
    if (fd_ < 0 || buffer_.empty()) return;
    flush_uinput_buffer(fd_, buffer_, ::write);
}

void UInputDevice::close() {
    if (fd_ < 0) return;
    std::exception_ptr failure;
    try { flush(); } catch (...) { failure = std::current_exception(); }
    ::ioctl(fd_, UI_DEV_DESTROY);
    ::close(fd_);
    fd_ = -1;
    if (failure) std::rethrow_exception(failure);
}

std::unique_ptr<VirtualInput> create_uinput_from_capabilities(const std::string& name, const std::string& phys,
                                                             const DeviceCapabilities& capabilities) {
    return std::make_unique<UInputDevice>(name, phys, capabilities.keys, capabilities.relative,
                                          capabilities.absolute);
}

}  // namespace loomwright
