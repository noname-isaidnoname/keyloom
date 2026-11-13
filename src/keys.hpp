#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace loomwright {

/// Upper-case a key name and resolve its alias (`ctrl` -> `LEFTCTRL`).
std::string canonical_key_name(const std::string& name);

/// Validate a single key name (letters, digits, F1-F24, or a Linux input name)
/// and return its canonical spelling. Throws BackendError when unknown.
std::string validate_key_name(const std::string& name);

/// Linux input event code for a key name. Throws BackendError when the running
/// kernel's key table has no such key.
int key_code(const std::string& name);

/// Reverse lookup for an input event code: returns the KEY_ name without the
/// prefix (`A`, `LEFTCTRL`), the runtime mouse name (`MOUSE_LEFT`), or nothing
/// for codes that are neither.
std::optional<std::string> normalize_key_event(int code);

const char* key_name_for_code(int code);

/// Case-insensitive lookup of a mouse button (`left` -> 0). Returns nullptr
/// when the name is not one of the five buttons the language exposes.
const std::pair<std::string_view, int>* find_mouse_button(std::string_view name);

bool is_mouse_button(const std::string& name);

/// Canonical runtime name for a mouse event (`left` -> `MOUSE_LEFT`). The
/// checker validates the button first; this only normalises a valid name.
std::string canonical_mouse_event_key(const std::string& name);

}  // namespace loomwright
