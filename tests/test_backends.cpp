#include "harness.hpp"

#include <algorithm>
#include <linux/input.h>
#include <vector>

#include "backends.hpp"

using namespace keyloom;

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
