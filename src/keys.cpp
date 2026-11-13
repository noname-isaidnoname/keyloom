#include "keys.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <string_view>
#include <linux/input.h>

#include "errors.hpp"
#include "value.hpp"

namespace loomwright {
namespace {

struct KeyEntry {
    const char* name;
    int code;
};

struct ReverseKeyEntry {
    int code;
    const char* name;
};

#include "keys.inc"

static_assert(std::size(kKeyTable) > 300, "the generated key table looks truncated");

const KeyEntry* find_key_entry(const std::string& name) {
    const auto* begin = std::begin(kKeyTable);
    const auto* end = std::end(kKeyTable);
    // The table is sorted by name, so the lookup is a binary search. The
    // comparison works on views: building a string for every comparison would
    // allocate once per step.
    const auto* found = std::lower_bound(begin, end, name,
                                         [](const KeyEntry& entry, const std::string& needle) {
                                             return std::string_view(entry.name) < std::string_view(needle);
                                         });
    if (found == end || name != found->name) return nullptr;
    return found;
}

}  // namespace

const std::unordered_map<std::string, std::string>& key_aliases() {
    static const std::unordered_map<std::string, std::string> aliases = {
        {"CTRL", "LEFTCTRL"},   {"CONTROL", "LEFTCTRL"}, {"SHIFT", "LEFTSHIFT"},
        {"ALT", "LEFTALT"},     {"SUPER", "LEFTMETA"},   {"WIN", "LEFTMETA"},
        {"CMD", "LEFTMETA"},    {"ESC", "ESC"},          {"ENTER", "ENTER"},
        {"RETURN", "ENTER"},    {"BACKSPACE", "BACKSPACE"}, {"DEL", "DELETE"},
        {"INS", "INSERT"},      {"PGUP", "PAGEUP"},      {"PGDN", "PAGEDOWN"},
        {"SPACE", "SPACE"},     {"TAB", "TAB"},          {"CAPS", "CAPSLOCK"},
        {"PRTSC", "SYSRQ"},     {"PRINTSCREEN", "SYSRQ"}, {"PAUSE", "PAUSE"},
    };
    return aliases;
}

const std::unordered_map<std::string, int>& mouse_buttons() {
    static const std::unordered_map<std::string, int> buttons = {
        {"left", 0}, {"right", 1}, {"middle", 2}, {"side", 3}, {"extra", 4},
    };
    return buttons;
}

bool is_mouse_button(const std::string& name) {
    // The table has a handful of entries, so a case-insensitive comparison
    // without a lowercase copy of the name is cheaper than building one.
    for (const auto& entry : mouse_buttons()) {
        if (name.size() != entry.first.size()) continue;
        bool equal = true;
        for (size_t index = 0; index < name.size(); ++index) {
            const char left = static_cast<char>(std::tolower(static_cast<unsigned char>(name[index])));
            const char right = static_cast<char>(std::tolower(static_cast<unsigned char>(entry.first[index])));
            if (left != right) {
                equal = false;
                break;
            }
        }
        if (equal) return true;
    }
    return false;
}

namespace {

std::string upper_ascii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

std::string lower_ascii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool is_common_key(const std::string& canonical) {
    static const char* const kCommonKeys[] = {
        "ESC",       "ENTER",      "BACKSPACE",  "TAB",        "SPACE",      "MINUS",
        "EQUAL",     "LEFTBRACE",  "RIGHTBRACE", "BACKSLASH",  "SEMICOLON",  "APOSTROPHE",
        "GRAVE",     "COMMA",      "DOT",        "SLASH",      "CAPSLOCK",   "SYSRQ",
        "SCROLLLOCK","PAUSE",      "INSERT",     "HOME",       "PAGEUP",     "DELETE",
        "END",       "PAGEDOWN",   "RIGHT",      "LEFT",       "DOWN",       "UP",
        "NUMLOCK",   "LEFTCTRL",   "LEFTSHIFT",  "LEFTALT",    "LEFTMETA",   "RIGHTCTRL",
        "RIGHTSHIFT","RIGHTALT",   "RIGHTMETA",  "MENU",       "COMPOSE",    "KPENTER",
        "KPDOT",     "KPSLASH",    "KPASTERISK", "KPMINUS",    "KPPLUS",     "KP0",
        "KP1",       "KP2",        "KP3",        "KP4",        "KP5",        "KP6",
        "KP7",       "KP8",        "KP9",        "MUTE",       "VOLUMEDOWN", "VOLUMEUP",
        "PLAYPAUSE", "STOPCD",     "PREVIOUSSONG","NEXTSONG",
    };
    for (const char* key : kCommonKeys) {
        if (canonical == key) return true;
    }
    return false;
}

}  // namespace

std::string canonical_key_name(const std::string& name) {
    const std::string upper = upper_ascii(name);
    const auto& aliases = key_aliases();
    const auto found = aliases.find(upper);
    return found == aliases.end() ? upper : found->second;
}

std::string validate_key_name(const std::string& name) {
    const std::string canonical = canonical_key_name(name);
    bool valid = canonical.size() == 1 &&
                 (std::isalpha(static_cast<unsigned char>(canonical[0])) != 0 ||
                  std::isdigit(static_cast<unsigned char>(canonical[0])) != 0);
    if (!valid && canonical.size() >= 2 && canonical[0] == 'F') {
        const std::string digits = canonical.substr(1);
        if (std::all_of(digits.begin(), digits.end(),
                        [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
            const long number = std::stol(digits);
            valid = number >= 1 && number <= 24;
        }
    }
    valid = valid || is_common_key(canonical);
    if (!valid) {
        throw BackendError("Unknown key " + quote_string(name) +
                           ". Use names such as A, F8, CTRL, ESC, or LEFT.");
    }
    return canonical;
}

int key_code(const std::string& name) {
    const std::string canonical = validate_key_name(name);
    const KeyEntry* entry = find_key_entry(canonical);
    if (entry == nullptr) {
        throw BackendError("Key " + quote_string(name) +
                           " is not available in this kernel's evdev key table");
    }
    return entry->code;
}

const char* key_name_for_code(int code) {
    const ReverseKeyEntry probe{code, ""};
    const auto* begin = std::begin(kKeyReverse);
    const auto* end = std::end(kKeyReverse);
    const auto* found = std::lower_bound(begin, end, probe,
                                         [](const ReverseKeyEntry& entry, const ReverseKeyEntry& needle) {
                                             return entry.code < needle.code;
                                         });
    if (found == end || found->code != code) return nullptr;
    return found->name;
}

std::string canonical_mouse_event_key(const std::string& name) {
    std::string lowered = lower_ascii(name);
    const std::string prefix = "mouse_";
    if (lowered.rfind(prefix, 0) == 0) lowered = lowered.substr(prefix.size());
    return "MOUSE_" + upper_ascii(lowered);
}

std::optional<std::string> normalize_key_event(int code) {
    // Mouse buttons arrive as EV_KEY too. Give them a distinct MOUSE_ prefix so
    // BTN_LEFT cannot be confused with the LEFT arrow key.
    switch (code) {
        case BTN_LEFT: return std::string("MOUSE_LEFT");
        case BTN_RIGHT: return std::string("MOUSE_RIGHT");
        case BTN_MIDDLE: return std::string("MOUSE_MIDDLE");
        case BTN_SIDE: return std::string("MOUSE_SIDE");
        case BTN_EXTRA: return std::string("MOUSE_EXTRA");
        default: break;
    }
    const char* name = key_name_for_code(code);
    if (name == nullptr) return std::nullopt;
    return std::string(name);
}

}  // namespace loomwright
