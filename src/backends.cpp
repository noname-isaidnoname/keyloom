#include "backends.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/input.h>
#include <unistd.h>

#include "errors.hpp"
#include "keys.hpp"
#include "value.hpp"

namespace keyloom {
namespace {

const std::vector<int>& pointer_buttons() {
    static const std::vector<int> buttons = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA};
    return buttons;
}

int button_code(const std::string& name) {
    const auto& buttons = mouse_buttons();
    const auto found = buttons.find(name);
    if (found == buttons.end()) throw BackendError("Unsupported mouse button " + py_repr_string(name));
    static const int kCodes[] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA};
    return kCodes[found->second];
}

void remove_name(std::vector<std::string>& names, const std::string& name) {
    names.erase(std::remove(names.begin(), names.end(), name), names.end());
}

}  // namespace

std::string ydotool_socket_path() {
    const char* explicit_socket = std::getenv("YDOTOOL_SOCKET");
    if (explicit_socket != nullptr && *explicit_socket != '\0') return explicit_socket;
    std::string runtime_dir;
    if (const char* directory = std::getenv("XDG_RUNTIME_DIR")) runtime_dir = directory;
    std::vector<std::string> candidates;
    if (!runtime_dir.empty()) candidates.push_back(runtime_dir + "/.ydotool_socket");
    candidates.emplace_back("/tmp/.ydotool_socket");
    for (const auto& candidate : candidates) {
        if (::access(candidate.c_str(), F_OK) == 0) return candidate;
    }
    return {};
}

void DryRunBackend::warp(int x, int y) { std::cout << "[dry-run] cursor warp " << x << ", " << y << "\n"; }

void DryRunBackend::move(int dx, int dy) { std::cout << "[dry-run] cursor move " << dx << ", " << dy << "\n"; }

void DryRunBackend::button(const std::string& name, bool down) {
    std::cout << "[dry-run] mouse " << name << (down ? " hold" : " free") << "\n";
}

void DryRunBackend::key(const std::string& name, bool down) {
    std::cout << "[dry-run] key " << name << (down ? " press" : " free") << "\n";
}

void DryRunBackend::type_text(const std::string& text) {
    std::cout << "[dry-run] type " << py_repr_string(text) << "\n";
}

void DryRunBackend::scroll(int x, int y) { std::cout << "[dry-run] scroll " << x << ", " << y << "\n"; }

YdotoolBackend::YdotoolBackend(int width, int height, PointerFactory pointer_factory, Runner runner,
                               std::string executable)
    : width_(std::max(1, width)), height_(std::max(1, height)),
      pointer_factory_(std::move(pointer_factory)), runner_(std::move(runner)) {
    executable_ = std::move(executable);
    if (executable_.empty()) executable_ = which("ydotool");
    if (executable_.empty()) {
        throw BackendError(
            "ydotool is not installed. Install ydotool and start ydotoold, or use --output-backend uinput.");
    }
    // The client only works with a live daemon; probe without failing hard.
    call({"--help"}, true);
}

std::string YdotoolBackend::call(const std::vector<std::string>& argv, bool tolerate) {
    std::vector<std::string> full;
    full.reserve(argv.size() + 1);
    full.push_back(executable_);
    full.insert(full.end(), argv.begin(), argv.end());

    CommandResult result;
    if (runner_) {
        result = runner_(full, tolerate);
    } else {
        result = run_command(full, std::string(), 4000);
    }
    if (result.spawn_failed || result.timed_out) {
        if (tolerate) return std::string();
        const std::string detail = result.spawn_failed ? result.spawn_error : std::string("timed out");
        throw BackendError("Could not talk to ydotool/ydotoold: " + detail + ". Is ydotoold running?");
    }
    if (result.returncode != 0 && !tolerate) {
        std::string detail = result.err;
        if (detail.empty()) detail = result.out;
        while (!detail.empty() && (detail.back() == '\n' || detail.back() == ' ')) detail.pop_back();
        throw BackendError("ydotool failed (" + std::to_string(result.returncode) + "): " +
                           (detail.empty() ? "no diagnostic" : detail) + "; ensure ydotoold is running");
    }
    std::string out = result.out;
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

OutputBackend& YdotoolBackend::pointer() {
    if (!pointer_) {
        if (pointer_factory_) {
            pointer_ = pointer_factory_(width_, height_, false);
        } else {
            try {
                pointer_ = std::make_unique<UInputBackend>(width_, height_, false);
            } catch (const BackendError& error) {
                throw BackendError(
                    "ydotool cannot warp the pointer reliably by itself (its --absolute mode jumps to the "
                    "top-left first). Keyloom needs write access to /dev/uinput for cursor motion; add your "
                    "user to the 'input' group, install a udev rule, or use a backend/environment where "
                    "/dev/uinput is writable. Details: " +
                    std::string(error.what()));
            }
        }
    }
    return *pointer_;
}

void YdotoolBackend::warp(int x, int y) { pointer().warp(x, y); }

void YdotoolBackend::move(int dx, int dy) { pointer().move(dx, dy); }

void YdotoolBackend::button(const std::string& name, bool down) {
    // ydotoold's click encoding uses its own button index (0 = left) plus 0x40 for
    // down and 0x80 for up, not the kernel BTN_* code.
    const auto& buttons = mouse_buttons();
    const auto found = buttons.find(name);
    if (found == buttons.end()) throw BackendError("Unsupported mouse button " + py_repr_string(name));
    const int code = found->second | (down ? 0x40 : 0x80);
    char encoded[16];
    std::snprintf(encoded, sizeof(encoded), "0x%x", code);
    call({"click", encoded});
    if (down) {
        down_buttons_.push_back(name);
    } else {
        remove_name(down_buttons_, name);
    }
}

void YdotoolBackend::key(const std::string& name, bool down) {
    const int code = key_code(name);
    call({"key", std::to_string(code) + (down ? ":1" : ":0")});
    if (down) {
        down_keys_.push_back(name);
    } else {
        remove_name(down_keys_, name);
    }
}

void YdotoolBackend::type_text(const std::string& text) {
    // ydotool's own escaping stays off: the .loom string is already decoded, so a
    // literal backslash must stay a backslash.
    call({"type", "-e", "0", "--", text});
}

void YdotoolBackend::scroll(int x, int y) { pointer().scroll(x, y); }

void YdotoolBackend::close() {
    const auto buttons = down_buttons_;
    const auto keys = down_keys_;
    for (const auto& name : buttons) {
        try {
            button(name, false);
        } catch (const std::exception&) {
        }
    }
    for (const auto& name : keys) {
        try {
            key(name, false);
        } catch (const std::exception&) {
        }
    }
    down_buttons_.clear();
    down_keys_.clear();
    if (pointer_) {
        try {
            pointer_->close();
        } catch (const std::exception&) {
        }
        pointer_.reset();
    }
}

std::vector<int> uinput_key_capabilities(bool keyboard) {
    std::vector<int> keys;
    if (keyboard) {
        keys.reserve(static_cast<size_t>(KEY_MAX) + 1 + pointer_buttons().size());
        for (int code = 0; code <= KEY_MAX; ++code) {
            // BTN_MISC..KEY_OK-1 is the button block: mice, joysticks, gamepads
            // and pen tools. Declaring BTN_TOOL_PEN or BTN_STYLUS makes udev tag
            // the device ID_INPUT_TABLET=1, and libinput then stops treating it
            // as a pointer or keyboard, so nothing the runtime injects works.
            // Only the pointer buttons below are added back.
            if (code >= BTN_MISC && code < KEY_OK) continue;
            keys.push_back(code);
        }
    }
    for (int code : pointer_buttons()) keys.push_back(code);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

UInputBackend::UInputBackend(int width, int height, bool keyboard)
    : width_(std::max(1, width)), height_(std::max(1, height)) {
    std::vector<int> keys = uinput_key_capabilities(keyboard);

    std::vector<AbsAxis> axes = {
        {ABS_X, 0, std::max(1, width_ - 1), 0, 0, 0},
        {ABS_Y, 0, std::max(1, height_ - 1), 0, 0, 0},
    };
    device_ = std::make_unique<UInputDevice>("Keyloom virtual pointer", "keyloom/uinput", keys,
                                             std::vector<int>{REL_X, REL_Y, REL_WHEEL, REL_HWHEEL}, axes);
}

UInputBackend::~UInputBackend() {
    try { close(); } catch (...) {}
}

void UInputBackend::warp(int x, int y) {
    const int clamped_x = std::max(0, std::min(width_ - 1, x));
    const int clamped_y = std::max(0, std::min(height_ - 1, y));
    device_->write(EV_ABS, ABS_X, clamped_x);
    device_->write(EV_ABS, ABS_Y, clamped_y);
    device_->write(EV_SYN, SYN_REPORT, 0);
    device_->flush();
}

void UInputBackend::move(int dx, int dy) {
    device_->write(EV_REL, REL_X, dx);
    device_->write(EV_REL, REL_Y, dy);
    device_->write(EV_SYN, SYN_REPORT, 0);
    device_->flush();
}

void UInputBackend::button(const std::string& name, bool down) {
    device_->write(EV_KEY, button_code(name), down ? 1 : 0);
    device_->write(EV_SYN, SYN_REPORT, 0);
    device_->flush();
    if (down) {
        down_buttons_.push_back(name);
    } else {
        remove_name(down_buttons_, name);
    }
}

void UInputBackend::key(const std::string& name, bool down) {
    device_->write(EV_KEY, key_code(name), down ? 1 : 0);
    device_->write(EV_SYN, SYN_REPORT, 0);
    device_->flush();
    if (down) {
        down_keys_.push_back(name);
    } else {
        remove_name(down_keys_, name);
    }
}

void UInputBackend::type_text(const std::string& text) {
    // US keyboard layout mapping. ydotool is the better choice for other layouts
    // and for Unicode text.
    static const std::string kShifted = "ABCDEFGHIJKLMNOPQRSTUVWXYZ~!@#$%^&*()_+{}|:\"<>?";
    static const std::unordered_map<std::string, std::string> kPlain = {
        {" ", "SPACE"},      {"\n", "ENTER"},   {"\t", "TAB"},        {"-", "MINUS"},
        {"=", "EQUAL"},      {"[", "LEFTBRACE"}, {"]", "RIGHTBRACE"}, {"\\", "BACKSLASH"},
        {";", "SEMICOLON"},  {"'", "APOSTROPHE"}, {"`", "GRAVE"},      {",", "COMMA"},
        {".", "DOT"},        {"/", "SLASH"},
    };
    static const std::unordered_map<std::string, std::string> kShiftedMap = {
        {"~", "GRAVE"},      {"!", "1"},         {"@", "2"},          {"#", "3"},
        {"$", "4"},          {"%", "5"},         {"^", "6"},          {"&", "7"},
        {"*", "8"},          {"(", "9"},         {")", "0"},          {"_", "MINUS"},
        {"+", "EQUAL"},      {"{", "LEFTBRACE"}, {"}", "RIGHTBRACE"}, {"|", "BACKSLASH"},
        {":", "SEMICOLON"},  {"\"", "APOSTROPHE"}, {"<", "COMMA"},    {">", "DOT"},
        {"?", "SLASH"},
    };

    // Iterate UTF-8 code points so multi-byte characters reach the same error
    // path as a single unknown character rather than byte soup.
    size_t index = 0;
    while (index < text.size()) {
        size_t length = 1;
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        if ((lead & 0xE0) == 0xC0) length = 2;
        else if ((lead & 0xF0) == 0xE0) length = 3;
        else if ((lead & 0xF8) == 0xF0) length = 4;
        if (index + length > text.size()) length = 1;
        const std::string character = text.substr(index, length);
        index += length;

        bool upper = false;
        std::string key_name;
        if (character.size() == 1) {
            const char c = character[0];
            upper = kShifted.find(c) != std::string::npos;
            if (std::isalpha(static_cast<unsigned char>(c)) != 0) {
                key_name = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            } else if (upper) {
                key_name = kShiftedMap.at(character);
            } else {
                const auto found = kPlain.find(character);
                key_name = found == kPlain.end() ? character : found->second;
            }
        } else {
            key_name = character;
        }

        if (upper) key("LEFTSHIFT", true);
        try {
            key(key_name, true);
            key(key_name, false);
        } catch (...) {
            if (upper) key("LEFTSHIFT", false);
            throw;
        }
        if (upper) key("LEFTSHIFT", false);
    }
}

void UInputBackend::scroll(int x, int y) {
    if (x != 0) device_->write(EV_REL, REL_HWHEEL, x);
    if (y != 0) device_->write(EV_REL, REL_WHEEL, y);
    device_->write(EV_SYN, SYN_REPORT, 0);
    device_->flush();
}

void UInputBackend::close() {
    if (!device_) return;
    const auto buttons = down_buttons_;
    const auto keys = down_keys_;
    for (const auto& name : buttons) {
        try {
            button(name, false);
        } catch (const std::exception&) {
        }
    }
    for (const auto& name : keys) {
        try {
            key(name, false);
        } catch (const std::exception&) {
        }
    }
    down_buttons_.clear();
    down_keys_.clear();
    auto device = std::move(device_);
    device->close();
}

PassthroughDevice::PassthroughDevice(const DeviceCapabilities& capabilities, VirtualInputFactory factory)
    : device_(factory ? factory("Keyloom passthrough", "keyloom/passthrough", capabilities)
                      : create_uinput_from_capabilities("Keyloom passthrough", "keyloom/passthrough",
                                                        capabilities)) {
    if (!device_) throw BackendError("passthrough device factory returned no device");
}

PassthroughDevice::~PassthroughDevice() { close(); }

void PassthroughDevice::write_event(int type, int code, int value) {
    if (!device_) return;
    if (type == EV_KEY && value == 1) {
        // A failed write can have partially reached the device. Conservatively
        // keep the key tracked until a release report is flushed successfully.
        down_keys_.insert(code);
        pending_releases_.erase(code);
    }
    device_->write(type, code, value);
    if (type == EV_KEY && value == 0) pending_releases_.insert(code);
    // A SYN report ends the frame; flushing there keeps event framing identical
    // while still collapsing one report into one write().
    if (type == EV_SYN) {
        device_->flush();
        for (int released : pending_releases_) down_keys_.erase(released);
        pending_releases_.clear();
    }
}

void PassthroughDevice::close() {
    if (!device_) return;
    std::exception_ptr failure;
    const auto remember = [&failure](auto operation) {
        try { operation(); } catch (...) { if (!failure) failure = std::current_exception(); }
    };
    for (int code : down_keys_) {
        remember([&] { device_->write(EV_KEY, code, 0); });
    }
    down_keys_.clear();
    pending_releases_.clear();
    remember([&] {
        device_->write(EV_SYN, SYN_REPORT, 0);
        device_->flush();
    });
    remember([&] { device_->close(); });
    device_.reset();
    if (failure) std::rethrow_exception(failure);
}

std::unique_ptr<OutputBackend> choose_backend(const std::string& name, int width, int height) {
    if (name == "ydotool") return std::make_unique<YdotoolBackend>(width, height);
    if (name == "uinput") return std::make_unique<UInputBackend>(width, height);
    if (name == "auto") {
        // Prefer ydotoold when it is actually running: better text support on
        // non-US layouts. Otherwise use a direct uinput device if possible.
        if (!which("ydotool").empty() && !ydotool_socket_path().empty()) {
            return std::make_unique<YdotoolBackend>(width, height);
        }
        if (::access("/dev/uinput", W_OK) == 0) return std::make_unique<UInputBackend>(width, height);
        if (!which("ydotool").empty()) return std::make_unique<YdotoolBackend>(width, height);
        return std::make_unique<UInputBackend>(width, height);
    }
    throw BackendError("Unknown output backend " + py_repr_string(name));
}

}  // namespace keyloom
