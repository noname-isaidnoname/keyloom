#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <sys/types.h>

namespace loomwright {

struct AbsAxis {
    int code = 0;
    int minimum = 0;
    int maximum = 0;
    int fuzz = 0;
    int flat = 0;
    int resolution = 0;
};

/// The kernel capabilities of a physical input device, as needed to mirror it.
struct DeviceCapabilities {
    std::vector<int> keys;
    std::vector<int> relative;
    std::vector<AbsAxis> absolute;
};

using UInputWriter = std::function<ssize_t(int, const void*, size_t)>;
/// Flush buffered bytes, retaining any unwritten suffix on failure.
void flush_uinput_buffer(int fd, std::vector<unsigned char>& buffer, const UInputWriter& writer);

/// A virtual device that accepts Linux input events.
class VirtualInput {
public:
    virtual ~VirtualInput() = default;
    virtual void write(int type, int code, int value) = 0;
    /// Push buffered events to the kernel as one report.
    virtual void flush() = 0;
    virtual void close() = 0;
};

/// A /dev/uinput device. Events are buffered and written in one syscall per
/// report, which is markedly cheaper than one write() per event.
struct AdoptUInputFd {
    explicit AdoptUInputFd() = default;
};

/// Test hook: return true to simulate ioctl failure at `step`, false to simulate
/// success (skip real ioctl), or nullopt to run the real ioctl.
using UInputSetupStep = std::function<std::optional<bool>(const char* step)>;

class UInputDevice : public VirtualInput {
public:
    UInputDevice(const std::string& name, const std::string& phys, const std::vector<int>& keys,
                 const std::vector<int>& relative, const std::vector<AbsAxis>& absolute);
    /// Adopt an already-open fd and run uinput setup; used for deterministic tests.
    UInputDevice(AdoptUInputFd, int fd, const std::string& name, const std::string& phys,
                 const std::vector<int>& keys, const std::vector<int>& relative,
                 const std::vector<AbsAxis>& absolute, UInputSetupStep step_hook = {});
    ~UInputDevice() override;

    UInputDevice(const UInputDevice&) = delete;
    UInputDevice& operator=(const UInputDevice&) = delete;

    void write(int type, int code, int value) override;
    void flush() override;
    void close() override;

    int fd() const { return fd_; }

private:
    void setup(int fd, const std::string& name, const std::string& phys, const std::vector<int>& keys,
               const std::vector<int>& relative, const std::vector<AbsAxis>& absolute,
               const UInputSetupStep& step_hook = {});

    int fd_ = -1;
    std::vector<unsigned char> buffer_;  // packed input_event records
};

using VirtualInputFactory =
    std::function<std::unique_ptr<VirtualInput>(const std::string& name, const std::string& phys,
                                                const DeviceCapabilities& capabilities)>;

/// Build a real uinput device mirroring the supplied capabilities.
std::unique_ptr<VirtualInput> create_uinput_from_capabilities(const std::string& name, const std::string& phys,
                                                             const DeviceCapabilities& capabilities);

}  // namespace loomwright
