#include "keys.hpp"

#include <algorithm>
#include <array>
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

struct KeyAlias {
    std::string_view name;
    std::string_view canonical;
};

constexpr std::array<KeyAlias, 21> kKeyAliases = {{
    {"ALT", "LEFTALT"},    {"BACKSPACE", "BACKSPACE"}, {"CAPS", "CAPSLOCK"},
    {"CMD", "LEFTMETA"},   {"CONTROL", "LEFTCTRL"},    {"CTRL", "LEFTCTRL"},
    {"DEL", "DELETE"},     {"ENTER", "ENTER"},         {"ESC", "ESC"},
    {"INS", "INSERT"},     {"PAUSE", "PAUSE"},         {"PGDN", "PAGEDOWN"},
    {"PGUP", "PAGEUP"},    {"PRINTSCREEN", "SYSRQ"},   {"PRTSC", "SYSRQ"},
    {"RETURN", "ENTER"},   {"SHIFT", "LEFTSHIFT"},     {"SPACE", "SPACE"},
    {"SUPER", "LEFTMETA"}, {"TAB", "TAB"},             {"WIN", "LEFTMETA"},
}};

constexpr bool key_aliases_sorted() {
    for (size_t index = 1; index < kKeyAliases.size(); ++index) {
        if (kKeyAliases[index - 1].name >= kKeyAliases[index].name) return false;
    }
    return true;
}

static_assert(key_aliases_sorted(), "the alias table must stay sorted by name");

std::string_view find_alias(std::string_view upper) {
    const auto* found = std::lower_bound(kKeyAliases.begin(), kKeyAliases.end(), upper,
                                         [](const KeyAlias& entry, std::string_view needle) {
                                             return entry.name < needle;
                                         });
    if (found == kKeyAliases.end() || found->name != upper) return {};
    return found->canonical;
}

constexpr std::array<std::pair<std::string_view, int>, 5> kMouseButtons = {{
    {"extra", 4}, {"left", 0}, {"middle", 2}, {"right", 1}, {"side", 3},
}};

const std::pair<std::string_view, int>* find_mouse_button(std::string_view name) {
    const auto* found = std::lower_bound(kMouseButtons.begin(), kMouseButtons.end(), name,
                                         [](const std::pair<std::string_view, int>& entry,
                                            std::string_view needle) { return entry.first < needle; });
    if (found == kMouseButtons.end() || found->first != name) return nullptr;
    return found;
}

bool is_mouse_button(const std::string& name) {
    // The table has a handful of entries, so a case-insensitive comparison
    // without a lowercase copy of the name is cheaper than building one.
    for (const auto& entry : kMouseButtons) {
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

constexpr std::array<std::string_view, 64> kCommonKeys = {{
    "APOSTROPHE", "BACKSLASH", "BACKSPACE", "CAPSLOCK", "COMMA", "COMPOSE",
    "DELETE", "DOT", "DOWN", "END", "ENTER", "EQUAL", "ESC", "GRAVE",
    "HOME", "INSERT", "KP0", "KP1", "KP2", "KP3", "KP4", "KP5", "KP6", "KP7",
    "KP8", "KP9", "KPASTERISK", "KPDOT", "KPENTER", "KPMINUS", "KPPLUS",
    "KPSLASH", "LEFT", "LEFTALT", "LEFTBRACE", "LEFTCTRL", "LEFTMETA",
    "LEFTSHIFT", "MENU", "MINUS", "MUTE", "NEXTSONG", "NUMLOCK", "PAGEDOWN",
    "PAGEUP", "PAUSE", "PLAYPAUSE", "PREVIOUSSONG", "RIGHT", "RIGHTALT",
    "RIGHTBRACE", "RIGHTCTRL", "RIGHTMETA", "RIGHTSHIFT", "SCROLLLOCK",
    "SEMICOLON", "SLASH", "SPACE", "STOPCD", "SYSRQ", "TAB", "UP",
    "VOLUMEDOWN", "VOLUMEUP",
}};

static_assert(std::ranges::is_sorted(kCommonKeys), "the common-key table must stay sorted");

bool is_common_key(const std::string& canonical) {
    return std::binary_search(kCommonKeys.begin(), kCommonKeys.end(), std::string_view(canonical));
}

}  // namespace

std::string canonical_key_name(const std::string& name) {
    const std::string upper = upper_ascii(name);
    const std::string_view alias = find_alias(upper);
    return alias.empty() ? upper : std::string(alias);
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
    if (lowered.starts_with(prefix)) lowered = lowered.substr(prefix.size());
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
