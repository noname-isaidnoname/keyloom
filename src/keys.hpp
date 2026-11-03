#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace keyloom {

/// Keyboard aliases accepted in scripts: CTRL, SHIFT, WIN, ESC, PGUP, ...
const std::unordered_map<std::string, std::string>& key_aliases();

/// Upper-case a key name and resolve its alias (`ctrl` -> `LEFTCTRL`).
std::string canonical_key_name(const std::string& name);

/// Validate a single key name (letters, digits, F1-F24, or a Linux input name)
/// and return its canonical spelling. Throws BackendError when unknown.
std::string validate_key_name(const std::string& name);

/// Linux input event code for a key name. Throws BackendError when the running
/// kernel's key table has no such key.
int key_code(const std::string& name);

/// Reverse lookup for an input event code: returns the KEY_ name without the
/// prefix (`A`, `LEFTCTRL`), or nothing for codes that are not keys.
std::optional<std::string> normalize_key_event(int code);

const char* key_name_for_code(int code);

/// Mouse buttons in the order the language exposes them.
const std::unordered_map<std::string, int>& mouse_buttons();

bool is_mouse_button(const std::string& name);

}  // namespace keyloom
