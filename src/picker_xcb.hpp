#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace loomwright {

/// Test seam called once after an upload request is queued and before its reply
/// is checked. Production callers use pick_mouse_position() without a hook.
std::optional<std::pair<int, int>> pick_mouse_position_with_upload_hook(
    bool verbose, const std::function<void(const std::string&)>& upload_hook);

}  // namespace loomwright
