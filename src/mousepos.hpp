#pragma once

#include <exception>
#include <functional>
#include <stdexcept>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace loomwright {

/// The picker cannot start (no display, missing X11, no screenshot tool).
class PickerError : public std::runtime_error {
public:
    explicit PickerError(const std::string& message) : std::runtime_error(message) {}
};

constexpr int kTooltipOffset = 18;
constexpr int kPollMs = 16;
constexpr int kScreenshotTimeoutMs = 15000;
/// How long the window manager may take to map the overlay before we give up.
constexpr int kOverlayTimeoutMs = 5000;

/// Render a position in the same shape the .loom language uses.
std::string format_coordinates(int x, int y);

std::pair<int, int> clamp_to_screen(int x, int y, int width, int height);

/// Place a tooltip next to the pointer without leaving the screen: prefer below
/// and to the right, flip when there is no room, then clamp to the edge.
std::pair<int, int> tooltip_position(int x, int y, int screen_width, int screen_height, int tip_width,
                                     int tip_height, int offset = kTooltipOffset);

using ExecutableFinder = std::function<std::string(const std::string&)>;

/// Installed screenshot tools, in preference order.
std::vector<std::string> available_screenshot_tools(const ExecutableFinder& finder = ExecutableFinder());

/// Whole-desktop capture commands for the installed tools.
std::vector<std::vector<std::string>> screenshot_commands(const std::string& path,
                                                          const ExecutableFinder& finder = ExecutableFinder());

struct CaptureResult {
    std::string path;
    std::string tool;
};

/// Capture the desktop to a temporary PNG. The caller owns (and removes) the file.
/// The default looks the tools up on PATH; tests can inject a finder.
CaptureResult capture_desktop(const ExecutableFinder& finder = ExecutableFinder());

/// Show the screenshot overlay and return the picked position, or nothing when
/// the pick was cancelled.
std::optional<std::pair<int, int>> pick_mouse_position(bool verbose = false);

}  // namespace loomwright
