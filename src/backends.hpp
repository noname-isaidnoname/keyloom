#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "subprocess.hpp"
#include "uinput.hpp"

namespace loomwright {

/// Everything the runtime needs from an output driver.
class OutputBackend {
public:
    virtual ~OutputBackend() = default;
    virtual void warp(int x, int y) = 0;
    virtual void move(int dx, int dy) = 0;
    virtual void button(const std::string& name, bool down) = 0;
    virtual void key(const std::string& name, bool down) = 0;
    virtual void type_text(const std::string& text) = 0;
    virtual void scroll(int x, int y) = 0;
    virtual void close() = 0;
    /// Class-style name reported by `--verbose` (UInputBackend, YdotoolBackend).
    virtual const char* name() const = 0;
};

/// Best-effort discovery of the socket ydotoold is listening on.
std::string ydotool_socket_path();

/// Key codes the uinput backend advertises. The BTN_* block is left out (except
/// the pointer buttons): a device that claims BTN_TOOL_PEN/BTN_STYLUS is
/// classified as a tablet by udev, and libinput then ignores its mouse and
/// keyboard events entirely.
std::vector<int> uinput_key_capabilities(bool keyboard);

class DryRunBackend : public OutputBackend {
public:
    void warp(int x, int y) override;
    void move(int dx, int dy) override;
    void button(const std::string& name, bool down) override;
    void key(const std::string& name, bool down) override;
    void type_text(const std::string& text) override;
    void scroll(int x, int y) override;
    void close() override {}
    const char* name() const override { return "DryRunBackend"; }
};

class UInputBackend;

using PointerFactory = std::function<std::unique_ptr<OutputBackend>(int width, int height, bool keyboard)>;

/// Keys, buttons and text through ydotoold, pointer motion through a real
/// absolute uinput device (ydotool 1.0.x's --absolute jumps to the corner first).
class YdotoolBackend : public OutputBackend {
public:
    using Runner = std::function<CommandResult(const std::vector<std::string>& argv, bool tolerate)>;

    YdotoolBackend(int width, int height, PointerFactory pointer_factory = PointerFactory(),
                   Runner runner = Runner(), std::string executable = std::string());

    void warp(int x, int y) override;
    void move(int dx, int dy) override;
    void button(const std::string& name, bool down) override;
    void key(const std::string& name, bool down) override;
    void type_text(const std::string& text) override;
    void scroll(int x, int y) override;
    void close() override;
    const char* name() const override { return "YdotoolBackend"; }

private:
    OutputBackend& pointer();
    std::string call(const std::vector<std::string>& argv, bool tolerate = false);

    std::string executable_;
    int width_;
    int height_;
    PointerFactory pointer_factory_;
    Runner runner_;
    std::unique_ptr<OutputBackend> pointer_;
    std::vector<std::string> down_buttons_;
    std::vector<std::string> down_keys_;
};

/// Direct /dev/uinput output. ABS_X/ABS_Y drive absolute pointer warps.
class UInputBackend : public OutputBackend {
public:
    UInputBackend(int width, int height, bool keyboard = true);
    ~UInputBackend() override;

    void warp(int x, int y) override;
    void move(int dx, int dy) override;
    void button(const std::string& name, bool down) override;
    void key(const std::string& name, bool down) override;
    void type_text(const std::string& text) override;
    void scroll(int x, int y) override;
    void close() override;
    const char* name() const override { return "UInputBackend"; }

private:
    std::unique_ptr<UInputDevice> device_;
    int width_;
    int height_;
    std::vector<std::string> down_buttons_;
    std::vector<std::string> down_keys_;
};

/// One raw Linux input event (the parts the runtime cares about).
struct RawEvent {
    int type = 0;
    int code = 0;
    int value = 0;
};

/// Where non-consumed physical events are replayed.
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void write_event(int type, int code, int value) = 0;
    virtual void close() = 0;
};

/// Replay physical input through uinput, minus the keys Loomwright consumes.
class PassthroughDevice : public EventSink {
public:
    explicit PassthroughDevice(const DeviceCapabilities& capabilities,
                               VirtualInputFactory factory = VirtualInputFactory());
    ~PassthroughDevice() override;

    PassthroughDevice(const PassthroughDevice&) = delete;
    PassthroughDevice& operator=(const PassthroughDevice&) = delete;

    void write_event(int type, int code, int value) override;
    void close() override;

private:
    std::unique_ptr<VirtualInput> device_;
    std::set<int> down_keys_;
    std::set<int> pending_releases_;
};

std::unique_ptr<OutputBackend> choose_backend(const std::string& name, int width, int height);

}  // namespace loomwright
