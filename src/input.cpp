#include "input.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>

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

bool InputDevice::is_keyboard_device() const {
    const bool has_a = std::find(capabilities_.keys.begin(), capabilities_.keys.end(), KEY_A) !=
                       capabilities_.keys.end();
    const bool has_enter = std::find(capabilities_.keys.begin(), capabilities_.keys.end(), KEY_ENTER) !=
                           capabilities_.keys.end();
    return has_a && has_enter;
}

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
