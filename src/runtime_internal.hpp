#pragma once

#include <cctype>
#include <string>
#include <vector>

namespace keyloom {
namespace detail {

/// Shared text helpers. They live in a header so the runtime's translation
/// units can each use them without one of them owning the definition.

/// Lower-case the ASCII letters of `text`.
inline std::string lower_ascii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

/// Strip the ASCII whitespace at both ends of `text`.
inline std::string trim_ascii(std::string text) {
    size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0) ++first;
    size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) --last;
    return text.substr(first, last - first);
}

/// Split `text` at every `separator`. A trailing separator produces a trailing
/// empty field, so "a+b+" splits into three parts.
inline std::vector<std::string> split_on(const std::string& text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t found = text.find(separator, start);
        if (found == std::string::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, found - start));
        start = found + 1;
    }
    return parts;
}

}  // namespace detail
}  // namespace keyloom
