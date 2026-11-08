#pragma once

#include <memory>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "uinput.hpp"

namespace keyloom {

class EventSink;

struct PendingInput {
    bool any = false;
    bool synchronization_lost = false;
    std::set<int> released_codes;
};

/// Drain a nonblocking evdev-compatible stream, retaining release information.
PendingInput drain_input_events(int fd);

/// Stop replay before returning the physical device to other consumers.
/// Every cleanup step is attempted, even if an earlier one fails.
void release_input_capture(EventSink* sink, const std::function<void()>& ungrab,
                           const std::function<void()>& close_device);

/// Grab only an idle device, and roll back if events appeared during the grab.
void grab_idle_input(const std::function<std::vector<int>()>& snapshot,
                     const std::function<PendingInput()>& discard_pending,
                     const std::function<void()>& grab, const std::function<void()>& ungrab);

/// A physical /dev/input/eventN device.
class InputDevice {
public:
    /// Open a device; returns null when it cannot be opened or read.
    static std::unique_ptr<InputDevice> open(const std::string& path);

    ~InputDevice();
    InputDevice(const InputDevice&) = delete;
    InputDevice& operator=(const InputDevice&) = delete;

    const std::string& path() const { return path_; }
    const std::string& name() const { return name_; }
    int fd() const { return fd_; }

    /// Keyboard-ish device: it can produce A and either Enter or Space.
    bool is_keyboard() const;
    bool is_mouse() const;
    /// Narrower test used by `--devices` (A and Enter).
    bool is_keyboard_device() const;

    const DeviceCapabilities& capabilities() const { return capabilities_; }
    /// Snapshot currently pressed EV_KEY codes after an evdev queue overrun.
    std::vector<int> pressed_key_codes() const;
    /// Drain this client's queued events, preserving release information for diagnostics.
    PendingInput discard_pending_events();

    void grab();
    void ungrab();
    void close();

private:
    InputDevice(int fd, std::string path, std::string name, DeviceCapabilities capabilities);

    int fd_ = -1;
    std::string path_;
    std::string name_;
    DeviceCapabilities capabilities_;
    bool grabbed_ = false;
};

/// /dev/input/event* paths in sorted order.
std::vector<std::string> list_input_devices();

}  // namespace keyloom
