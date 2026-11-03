#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace keyloom {

class CliUsageError : public std::runtime_error {
public:
    explicit CliUsageError(const std::string& message) : std::runtime_error(message) {}
};

struct CliArgs {
    std::optional<std::string> init;
    std::optional<std::string> check;
    std::optional<std::string> run;
    std::optional<std::string> docs;
    std::optional<std::string> script;
    std::optional<std::string> input_device;
    std::optional<std::string> output_backend;
    std::optional<std::string> screen_size;

    bool devices = false;
    bool screen = false;
    bool get_mouse_pos = false;
    bool list_api = false;
    bool doctor = false;
    bool help = false;
    bool version = false;
    bool force = false;
    bool dry_run = false;
    bool verbose = false;
};

/// How `--get-mouse-pos` is run in tests: return a picked position, std::nullopt
/// for a cancel, or throw PickerError for a startup failure. An empty function
/// calls the real picker.
using PickerFunction = std::function<std::optional<std::pair<int, int>>(bool)>;

CliArgs parse_cli(const std::vector<std::string>& args);
std::string usage_text();
std::string help_text();
const char* version_string();
int run_cli(const std::vector<std::string>& args);
int run_cli(const std::vector<std::string>& args, const PickerFunction& picker);

}  // namespace keyloom
