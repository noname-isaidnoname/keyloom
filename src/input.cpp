#include "input.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>

#include "backends.hpp"
#include "errors.hpp"
#include "keys.hpp"

namespace keyloom {
namespace {

constexpr size_t kKeyBits = (KEY_MAX / 8) + 1;
constexpr size_t kRelBits = (REL_MAX / 8) + 1;
constexpr size_t kAbsBits = (ABS_MAX / 8) + 1;

bool bit_set(const std::vector<unsigned char>& bits, int code) {
    const size_t index = static_cast<size_t>(code);
    if (index / 8 >= bits.size()) return false;
    return (bits[index / 8] & (1u << (index % 8))) != 0;
}

std::vector<int> collect_bits(const std::vector<unsigned char>& bits, int maximum) {
    std::vector<int> codes;
    for (int code = 0; code <= maximum; ++code) {
        if (bit_set(bits, code)) codes.push_back(code);
    }
    return codes;
}

std::string device_name(int fd) {
    char buffer[256] = {0};
    if (::ioctl(fd, EVIOCGNAME(sizeof(buffer) - 1), buffer) < 0) return std::string();
    return std::string(buffer);
}

DeviceCapabilities read_capabilities(int fd) {
    DeviceCapabilities capabilities;

    std::vector<unsigned char> keys(kKeyBits, 0);
    if (::ioctl(fd, EVIOCGBIT(EV_KEY, keys.size()), keys.data()) >= 0) {
        capabilities.keys = collect_bits(keys, KEY_MAX);
    }

    std::vector<unsigned char> relative(kRelBits, 0);
    if (::ioctl(fd, EVIOCGBIT(EV_REL, relative.size()), relative.data()) >= 0) {
        capabilities.relative = collect_bits(relative, REL_MAX);
    }

    std::vector<unsigned char> absolute(kAbsBits, 0);
    if (::ioctl(fd, EVIOCGBIT(EV_ABS, absolute.size()), absolute.data()) >= 0) {
        for (int code : collect_bits(absolute, ABS_MAX)) {
            input_absinfo info{};
            if (::ioctl(fd, EVIOCGABS(code), &info) == 0) {
                capabilities.absolute.push_back(
                    AbsAxis{code, info.minimum, info.maximum, info.fuzz, info.flat, info.resolution});
            }
        }
    }
    return capabilities;
}

}  // namespace

void release_input_capture(EventSink* sink, const std::function<void()>& ungrab,
                           const std::function<void()>& close_device) {
    std::exception_ptr failure;
    const auto attempt = [&failure](const auto& operation) {
        try { operation(); } catch (...) { if (!failure) failure = std::current_exception(); }
    };
    if (sink != nullptr) attempt([sink] { sink->close(); });
    attempt(ungrab);
    attempt(close_device);
    if (failure) std::rethrow_exception(failure);
}

void grab_idle_input(const std::function<std::vector<int>()>& snapshot,
                     const std::function<PendingInput()>& discard_pending,
                     const std::function<void()>& grab, const std::function<void()>& ungrab) {
    if (!snapshot().empty())
        throw RuntimeErrorKeyloom("release all held keys and buttons before starting key consumption");
    // Drain non-key events left behind by EVIOCGKEY, then take the last key
    // snapshot before the grab. Never call EVIOCGKEY after grabbing: it would
    // flush a transient press/release before we could notice it.
    discard_pending();
    if (!snapshot().empty())
        throw RuntimeErrorKeyloom("release all held keys and buttons before starting key consumption");
    grab();
    try {
        PendingInput pending;
        try {
            pending = discard_pending();
        } catch (const std::exception& error) {
            throw RuntimeErrorKeyloom(std::string("could not inspect input after grabbing: ") + error.what() +
                ". A key release may have been intercepted; if a key appears stuck, physically tap and release it");
        }
        if (pending.any) {
            std::string released;
            for (int code : pending.released_codes) {
                const auto name = normalize_key_event(code);
                if (!released.empty()) released += ", ";
                released += name ? *name : "code " + std::to_string(code);
            }
            std::string message = "input changed during the grab; release all keys and buttons and retry";
            if (!released.empty())
                message += ". The desktop may have missed release of " + released +
                           "; if it appears stuck, physically tap and release it";
            else if (pending.synchronization_lost)
                message += ". Input events were lost; if a key appears stuck, physically tap and release it";
            throw RuntimeErrorKeyloom(message);
        }
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        try { ungrab(); } catch (...) {}
        std::rethrow_exception(failure);
    }
}

InputDevice::InputDevice(int fd, std::string path, std::string name, DeviceCapabilities capabilities)
    : fd_(fd), path_(std::move(path)), name_(std::move(name)), capabilities_(std::move(capabilities)) {}

InputDevice::~InputDevice() { close(); }

std::unique_ptr<InputDevice> InputDevice::open(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return nullptr;

    std::string name = device_name(fd);
    DeviceCapabilities capabilities = read_capabilities(fd);
    return std::unique_ptr<InputDevice>(new InputDevice(fd, path, std::move(name), std::move(capabilities)));
}

bool InputDevice::is_keyboard() const {
    const bool has_a = std::find(capabilities_.keys.begin(), capabilities_.keys.end(), KEY_A) !=
                       capabilities_.keys.end();
    const bool has_enter = std::find(capabilities_.keys.begin(), capabilities_.keys.end(), KEY_ENTER) !=
                           capabilities_.keys.end();
    const bool has_space = std::find(capabilities_.keys.begin(), capabilities_.keys.end(), KEY_SPACE) !=
                           capabilities_.keys.end();
    return has_a && (has_enter || has_space);
}

bool InputDevice::is_mouse() const {
    const auto& keys = capabilities_.keys;
    const bool has_left = std::find(keys.begin(), keys.end(), BTN_LEFT) != keys.end();
    const bool has_right = std::find(keys.begin(), keys.end(), BTN_RIGHT) != keys.end();
    return has_left && has_right;
}

bool InputDevice::is_keyboard_device() const {
    const bool has_a = std::find(capabilities_.keys.begin(), capabilities_.keys.end(), KEY_A) !=
                       capabilities_.keys.end();
    const bool has_enter = std::find(capabilities_.keys.begin(), capabilities_.keys.end(), KEY_ENTER) !=
                           capabilities_.keys.end();
    return has_a && has_enter;
}

std::vector<int> InputDevice::pressed_key_codes() const {
    std::vector<unsigned char> keys(kKeyBits, 0);
    if (::ioctl(fd_, EVIOCGKEY(keys.size()), keys.data()) < 0) {
        throw std::system_error(errno, std::generic_category(), "EVIOCGKEY");
    }
    return collect_bits(keys, KEY_MAX);
}

PendingInput drain_input_events(int fd) {
    PendingInput pending;
    input_event events[64];
    size_t count = 0;
    while (true) {
        const ssize_t bytes = ::read(fd, events, sizeof(events));
        if (bytes > 0) {
            pending.any = true;
            const size_t event_count = static_cast<size_t>(bytes) / sizeof(input_event);
            for (size_t i = 0; i < event_count; ++i) {
                if (events[i].type == EV_KEY && events[i].value == 0)
                    pending.released_codes.insert(events[i].code);
                if (events[i].type == EV_SYN && events[i].code == SYN_DROPPED)
                    pending.synchronization_lost = true;
            }
            count += static_cast<size_t>(bytes) / sizeof(input_event);
            if (count >= 4096) throw RuntimeErrorKeyloom("too many pending input events while grabbing");
            continue;
        }
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return pending;
        if (bytes == 0) throw RuntimeErrorKeyloom("input device disconnected while grabbing");
        throw std::system_error(errno, std::generic_category(), "reading pending input events");
    }
}

PendingInput InputDevice::discard_pending_events() { return drain_input_events(fd_); }

void InputDevice::grab() {
    if (fd_ < 0 || grabbed_) return;
    if (::ioctl(fd_, EVIOCGRAB, 1) < 0) {
        throw std::system_error(errno, std::generic_category());
    }
    grabbed_ = true;
}

void InputDevice::ungrab() {
    if (fd_ < 0 || !grabbed_) return;
    ::ioctl(fd_, EVIOCGRAB, 0);
    grabbed_ = false;
}

void InputDevice::close() {
    if (fd_ < 0) return;
    ungrab();
    ::close(fd_);
    fd_ = -1;
}

std::vector<std::string> list_input_devices() {
    std::vector<std::string> paths;
    DIR* directory = ::opendir("/dev/input");
    if (directory == nullptr) return paths;
    while (dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (name.rfind("event", 0) != 0) continue;
        paths.push_back("/dev/input/" + name);
    }
    ::closedir(directory);
    std::sort(paths.begin(), paths.end());
    return paths;
}

}  // namespace keyloom
