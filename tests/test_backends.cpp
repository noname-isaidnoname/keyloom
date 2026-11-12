#include "harness.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <optional>
#include <unistd.h>
#include <vector>

#include "backends.hpp"
#include "errors.hpp"
#include "uinput.hpp"
#include "unique_fd.hpp"
#include "xcb_connection.hpp"

using namespace loomwright;

namespace {

bool contains(const std::vector<int>& values, int value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

// Advertising BTN_TOOL_PEN or BTN_STYLUS makes udev tag the whole uinput device
// ID_INPUT_TABLET=1, and libinput then ignores its pointer and keyboard events.
// The 0..KEY_MAX sweep must skip the BTN_* block and add only the pointer
// buttons back.
TEST_CASE(uinput_keyboard_capabilities_skip_pen_and_other_button_codes) {
    const std::vector<int> keys = uinput_key_capabilities(true);

    CHECK_TRUE(contains(keys, KEY_A));
    CHECK_TRUE(contains(keys, KEY_ENTER));
    CHECK_TRUE(contains(keys, KEY_LEFTSHIFT));
    CHECK_TRUE(contains(keys, KEY_F12));
    CHECK_TRUE(contains(keys, KEY_OK));

    CHECK_TRUE(contains(keys, BTN_LEFT));
    CHECK_TRUE(contains(keys, BTN_RIGHT));
    CHECK_TRUE(contains(keys, BTN_MIDDLE));
    CHECK_TRUE(contains(keys, BTN_SIDE));
    CHECK_TRUE(contains(keys, BTN_EXTRA));

    CHECK_TRUE(!contains(keys, BTN_TOOL_PEN));
    CHECK_TRUE(!contains(keys, BTN_STYLUS));
    CHECK_TRUE(!contains(keys, BTN_TOUCH));
    CHECK_TRUE(!contains(keys, BTN_GAMEPAD));
    CHECK_TRUE(!contains(keys, BTN_JOYSTICK));

    CHECK_TRUE(std::is_sorted(keys.begin(), keys.end()));
    CHECK_TRUE(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
}

TEST_CASE(uinput_pointer_capabilities_are_only_pointer_buttons) {
    const std::vector<int> keys = uinput_key_capabilities(false);
    CHECK_EQ(keys.size(), static_cast<size_t>(5));
    CHECK_TRUE(contains(keys, BTN_LEFT));
    CHECK_TRUE(contains(keys, BTN_RIGHT));
    CHECK_TRUE(contains(keys, BTN_MIDDLE));
    CHECK_TRUE(contains(keys, BTN_SIDE));
    CHECK_TRUE(contains(keys, BTN_EXTRA));
    CHECK_TRUE(!contains(keys, KEY_A));
    CHECK_TRUE(!contains(keys, BTN_TOOL_PEN));
}

TEST_CASE(uinput_setup_failure_closes_adopted_fd) {
    int fds[2];
    CHECK_EQ(::pipe(fds), 0);
    ::close(fds[1]);
    const int adopted = fds[0];
    CHECK_THROWS(UInputDevice(AdoptUInputFd(), adopted, "test", "test", {KEY_A}, {}, {}), BackendError);
    errno = 0;
    CHECK_EQ(::fcntl(adopted, F_GETFD), -1);
    CHECK_EQ(errno, EBADF);
}

namespace {

void check_uinput_step_failure(const char* target, bool use_relative = false, bool use_absolute = false,
                               bool use_phys = false) {
    int fds[2];
    CHECK_EQ(::pipe(fds), 0);
    ::close(fds[1]);
    const int adopted = fds[0];
    std::string seen;
    UInputSetupStep hook = [&](const char* step) -> std::optional<bool> {
        if (std::strcmp(step, target) == 0) { seen = step; return true; }
        return false;  // force success so setup reaches the target on a non-uinput fd
    };
    std::vector<int> rel = use_relative ? std::vector<int>{REL_X} : std::vector<int>{};
    std::vector<AbsAxis> abs = use_absolute ? std::vector<AbsAxis>{{ABS_X, 0, 1919, 0, 0, 0}} : std::vector<AbsAxis>{};
    std::string phys = use_phys ? "test/phys" : "";
    try {
        UInputDevice(AdoptUInputFd(), adopted, "test", phys, {KEY_A}, rel, abs, hook);
        CHECK_TRUE(false);
    } catch (const BackendError& error) {
        CHECK_EQ(seen, std::string(target));
        CHECK_TRUE(std::string(error.what()).find(target) != std::string::npos);
    }
    errno = 0;
    CHECK_EQ(::fcntl(adopted, F_GETFD), -1);
    CHECK_EQ(errno, EBADF);
}

}  // namespace

TEST_CASE(uinput_setup_step_injection_closes_fd_and_reports_step) {
    check_uinput_step_failure("UI_SET_EVBIT EV_KEY");
    check_uinput_step_failure("UI_SET_KEYBIT");
    check_uinput_step_failure("UI_SET_EVBIT EV_REL", true);
    check_uinput_step_failure("UI_SET_RELBIT", true);
    check_uinput_step_failure("UI_SET_EVBIT EV_ABS", false, true);
    check_uinput_step_failure("UI_SET_ABSBIT", false, true);
    check_uinput_step_failure("UI_ABS_SETUP", false, true);
    check_uinput_step_failure("UI_SET_PHYS", false, false, true);
    check_uinput_step_failure("UI_DEV_SETUP");
    check_uinput_step_failure("UI_DEV_CREATE");
}

TEST_CASE(real_uinput_device_happy_path_transitions_when_writable) {
    if (::access("/dev/uinput", W_OK) != 0) return;  // environment-dependent
    UInputDevice device("Loomwright test", "loomwright/test/real-uinput-smoke", {KEY_F24}, {}, {});
    device.write(EV_KEY, KEY_F24, 1);
    device.write(EV_SYN, SYN_REPORT, 0);
    device.write(EV_KEY, KEY_F24, 0);
    device.write(EV_SYN, SYN_REPORT, 0);
    device.flush();
    device.close();
}

TEST_CASE(uinput_flush_reports_failure_and_retains_only_unwritten_bytes) {
    std::vector<unsigned char> buffer{1, 2, 3, 4};
    int calls = 0;
    UInputWriter writer = [&calls](int, const void*, size_t) -> ssize_t {
        ++calls;
        if (calls == 1) return 2;
        errno = EAGAIN;
        return -1;
    };
    CHECK_THROWS(flush_uinput_buffer(1, buffer, writer), BackendError);
    CHECK_TRUE(buffer == std::vector<unsigned char>({3, 4}));
    flush_uinput_buffer(1, buffer, [](int, const void*, size_t size) -> ssize_t {
        return static_cast<ssize_t>(size);
    });
    CHECK_TRUE(buffer.empty());
}

TEST_CASE(unique_fd_closes_descriptor_on_setup_exception) {
    int fds[2];
    CHECK_EQ(::pipe(fds), 0);
    try {
        UniqueFd owned(fds[0]);
        throw BackendError("injected setup failure");
    } catch (const BackendError&) {
    }
    errno = 0;
    CHECK_EQ(::fcntl(fds[0], F_GETFD), -1);
    CHECK_EQ(errno, EBADF);
    ::close(fds[1]);
}

TEST_CASE(xcb_connection_reports_invalid_display) {
    CHECK_THROWS(XcbConnection("invalid-loomwright-display"), RuntimeErrorLoomwright);
}
