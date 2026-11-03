#include "cli.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "checker.hpp"
#include "docs.hpp"
#include "errors.hpp"
#include "input.hpp"
#include "mousepos.hpp"
#include "parser.hpp"
#include "runtime.hpp"
#include "subprocess.hpp"

#ifndef KEYLOOM_VERSION
#define KEYLOOM_VERSION "0.0.0"
#endif

namespace keyloom {
namespace {

const char* kMarker = "keyloom: ";

std::string expand_home(const std::string& name) {
    if (name.empty() || name.front() != '~') return name;
    const char* home = std::getenv("HOME");
    if (home == nullptr) return name;
    return std::string(home) + name.substr(1);
}

std::filesystem::path resolve_script_path(const std::string& name) {
    std::filesystem::path path(expand_home(name));
    if (!path.has_extension()) path += ".loom";
    return path;
}

Script load_script(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw RuntimeErrorKeyloom("Cannot read script " + path.string() + ": " +
                                  std::string(std::strerror(errno)));
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    try {
        return parse(buffer.str(), path.string());
    } catch (const ParseError& error) {
        throw RuntimeErrorKeyloom(path.string() + ": " + error.what());
    }
}

std::string init_template(const std::string& name) {
    return "loom \"" + name + "\" {\n"
           "    option input = \"evdev\"\n"
           "    option output = \"auto\"\n"
           "    option panic = \"PAUSE\"\n"
           "\n"
           "    when key F8 goes down {\n"
           "        log \"F8 pressed\"\n"
           "    }\n"
           "\n"
           "    when key F8 goes up {\n"
           "        log \"F8 released\"\n"
           "    }\n"
           "}\n";
}

int command_init(const std::string& name, bool force) {
    const std::filesystem::path path = resolve_script_path(name);
    std::string clean_name = path.stem().string();
    std::string escaped;
    for (char c : clean_name) {
        if (c == '\\' || c == '"') escaped.push_back('\\');
        escaped.push_back(c);
    }

    if (path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            std::cerr << kMarker << "cannot create " << path.parent_path().string() << ": " << error.message()
                      << "\n";
            return 2;
        }
    }
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists && !force) {
        std::cerr << "Refusing to overwrite " << path.string() << ". Use --force to replace it.\n";
        return 2;
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        std::cerr << kMarker << "cannot write " << path.string() << ": " << std::strerror(errno) << "\n";
        return 2;
    }
    stream << init_template(escaped);
    stream.close();
    std::cout << "Created " << path.string() << "\n";
    std::cout << "Next: keyloom --check " << path.string() << " && keyloom --run " << path.string() << "\n";
    return 0;
}

int command_check(const std::filesystem::path& path) {
    Script script;
    try {
        script = load_script(path);
    } catch (const std::exception& error) {
        std::cerr << kMarker << error.what() << "\n";
        return 2;
    }

    bool has_error = false;
    for (const auto& diagnostic : check_script(script)) {
        std::cout << path.string() << ": " << diagnostic.to_string() << "\n";
        if (diagnostic.level == "error") has_error = true;
    }
    if (has_error) return 2;
    std::cout << "OK: " << path.string() << " (" << script.name << ")\n";
    return 0;
}

int command_run(const std::filesystem::path& path, const CliArgs& args) {
    Script script;
    try {
        script = load_script(path);
    } catch (const std::exception& error) {
        std::cerr << kMarker << error.what() << "\n";
        return 2;
    }

    bool has_error = false;
    for (const auto& diagnostic : check_script(script)) {
        if (diagnostic.level != "ok") std::cout << path.string() << ": " << diagnostic.to_string() << "\n";
        if (diagnostic.level == "error") has_error = true;
    }
    if (has_error) {
        std::cerr << "Refusing to run: fix the errors above.\n";
        return 2;
    }

    Runtime::Options options;
    options.dry_run = args.dry_run;
    options.verbose = args.verbose;
    if (args.input_device) options.input_device = *args.input_device;
    if (args.output_backend) options.output_backend = *args.output_backend;
    if (args.screen_size) options.screen_size = *args.screen_size;

    Runtime runtime(std::move(script), options);
    try {
        Task task = runtime.run();
        runtime.scheduler().spawn(std::move(task));
        runtime.scheduler().run();
        if (runtime.failure()) std::rethrow_exception(runtime.failure());
    } catch (const std::exception& error) {
        std::cerr << kMarker << error.what() << "\n";
        return 1;
    }

    if (runtime.interrupted()) {
        std::cout << "\n[keyloom] Interrupted.\n";
        return 0;
    }
    return 0;
}

int command_devices() {
    const auto paths = list_input_devices();
    if (paths.empty()) {
        std::cout << "No /dev/input/event* devices found.\n";
        return 0;
    }
    for (const auto& path : paths) {
        auto device = InputDevice::open(path);
        if (!device) {
            std::cout << path << ": no read permission (or device disappeared)\n";
            continue;
        }
        const char* kind = device->is_keyboard_device() ? "keyboard"
                           : device->is_mouse()        ? "mouse"
                                                       : "other input";
        std::cout << path << ": " << device->name() << " — " << kind << "\n";
    }
    return 0;
}

std::string boolean_text(bool value) { return value ? "True" : "False"; }

int command_doctor() {
    std::cout << "Keyloom " << version_string() << " — Linux desktop automation diagnostics\n";
    const char* session = std::getenv("XDG_SESSION_TYPE");
    std::cout << "Session: " << (session != nullptr ? session : "unknown")
              << " | DISPLAY=" << boolean_text(std::getenv("DISPLAY") != nullptr)
              << " | WAYLAND_DISPLAY=" << boolean_text(std::getenv("WAYLAND_DISPLAY") != nullptr) << "\n";

    const auto bounds = detect_screen_size();
    std::cout << "Desktop bounds (best effort): " << bounds.first << "x" << bounds.second << "\n";

    int accessible = 0;
    for (const auto& path : list_input_devices()) {
        if (InputDevice::open(path)) ++accessible;
    }
    std::cout << "evdev input: " << accessible << " accessible event device(s)\n";

    const std::string ydotool = which("ydotool");
    const std::string ydotoold = which("ydotoold");
    std::cout << "ydotool: " << (ydotool.empty() ? "not found" : ydotool) << "\n";
    std::cout << "ydotoold: " << (ydotoold.empty() ? "not found" : ydotoold) << "\n";
    if (!ydotool.empty() && ydotoold.empty()) {
        std::cout << "  note: the ydotool client expects a running ydotoold daemon (which may be provided by a "
                     "system service).\n";
    }
    const bool uinput_ready = ::access("/dev/uinput", W_OK) == 0;
    std::cout << "/dev/uinput: " << (uinput_ready ? "present and writable" : "missing or not writable") << "\n";
    std::cout << "X11: " << (std::getenv("DISPLAY") != nullptr ? "available (keyloom --get-mouse-pos)"
                                                               : "MISSING — set DISPLAY for keyloom --get-mouse-pos")
              << "\n";

    const auto tools = available_screenshot_tools();
    std::cout << "screenshot tools (picker): ";
    if (tools.empty()) {
        std::cout << "NONE — install spectacle, grim, gnome-screenshot, scrot, or ImageMagick import\n";
    } else {
        for (size_t index = 0; index < tools.size(); ++index) {
            if (index > 0) std::cout << ", ";
            std::cout << tools[index];
        }
        std::cout << "\n";
    }
    std::cout << "wl-copy / wl-paste: " << boolean_text(!which("wl-copy").empty()) << " / "
              << boolean_text(!which("wl-paste").empty()) << "\n";
    if (accessible == 0) {
        std::cout << "Input note: Keyloom needs permission to read a keyboard from /dev/input/event*. See "
                     "README.md → Wayland and Xwayland setup.\n";
    }
    return 0;
}

int command_get_mouse_pos(const CliArgs& args, const PickerFunction& picker) {
    std::optional<std::pair<int, int>> position;
    try {
        position = picker ? picker(args.verbose) : pick_mouse_position(args.verbose);
    } catch (const PickerError& error) {
        std::cerr << kMarker << error.what() << "\n";
        return 2;
    }
    if (!position) {
        std::cerr << kMarker << "mouse position pick cancelled.\n";
        return 130;
    }
    std::cout << format_coordinates(position->first, position->second) << "\n";
    return 0;
}

int command_list_api() {
    for (const auto& group : api_reference()) {
        std::cout << group.first << ": ";
        for (size_t index = 0; index < group.second.size(); ++index) {
            if (index > 0) std::cout << ", ";
            std::cout << group.second[index];
        }
        std::cout << "\n";
    }
    return 0;
}

}  // namespace

const char* version_string() { return KEYLOOM_VERSION; }

std::string usage_text() {
    return "usage: keyloom [-h] [--init SCRIPT | --check PATH | --run PATH | --docs [TOPIC] | --devices |\n"
           "               --screen | --get-mouse-pos | --list-api | --doctor]\n"
           "               [--force] [--dry-run] [--input-device EVENT_PATH]\n"
           "               [--output-backend {auto,ydotool,uinput}] [--screen-size WIDTHxHEIGHT] [--verbose]\n"
           "               [--version] [script]\n";
}

std::string help_text() {
    return usage_text() +
           "\nBuild and run Linux desktop automation scripts in the Keyloom language.\n"
           "\n"
           "positional arguments:\n"
           "  script                script path (shorthand for --run PATH)\n"
           "\n"
           "options:\n"
           "  -h, --help            show this help message and exit\n"
           "  --init SCRIPT         create a starter .loom script\n"
           "  --check PATH          parse and statically check a script\n"
           "  --run PATH            run a script\n"
           "  --docs [TOPIC]        show language documentation\n"
           "  --devices             list visible input devices and permissions\n"
           "  --screen              print the detected desktop size\n"
           "  --get-mouse-pos       pick a pointer position with an on-screen tooltip\n"
           "  --list-api            list built-in API groups\n"
           "  --doctor              inspect input/output prerequisites\n"
           "  --force               overwrite an existing file with --init\n"
           "  --dry-run             log output actions instead of injecting them\n"
           "  --input-device EVENT_PATH\n"
           "                        listen to one /dev/input/eventN keyboard\n"
           "  --output-backend {auto,ydotool,uinput}\n"
           "                        override the script's output backend\n"
           "  --screen-size WIDTHxHEIGHT\n"
           "                        override detected pointer coordinate bounds\n"
           "  --verbose             print extra runtime details\n"
           "  --version             show the program version and exit\n"
           "\n"
           "Examples: keyloom --init demo | keyloom --check demo.loom | keyloom --run demo.loom\n"
           "          keyloom --get-mouse-pos\n";
}

CliArgs parse_cli(const std::vector<std::string>& args) {
    CliArgs parsed;
    std::string mode;

    auto note_mode = [&mode](const std::string& flag) {
        if (!mode.empty()) throw CliUsageError("argument " + flag + ": not allowed with argument " + mode);
        mode = flag;
    };

    for (size_t index = 0; index < args.size(); ++index) {
        std::string token = args[index];
        std::string inline_value;
        bool has_inline = false;
        if (token.rfind("--", 0) == 0) {
            const size_t equals = token.find('=');
            if (equals != std::string::npos) {
                inline_value = token.substr(equals + 1);
                token = token.substr(0, equals);
                has_inline = true;
            }
        }
        auto take_value = [&](const std::string& flag) -> std::string {
            if (has_inline) return inline_value;
            if (index + 1 >= args.size()) {
                throw CliUsageError("argument " + flag + ": expected one argument");
            }
            return args[++index];
        };

        if (token == "-h" || token == "--help") {
            parsed.help = true;
        } else if (token == "--version") {
            parsed.version = true;
        } else if (token == "--init") {
            note_mode(token);
            parsed.init = take_value(token);
        } else if (token == "--check") {
            note_mode(token);
            parsed.check = take_value(token);
        } else if (token == "--run") {
            note_mode(token);
            parsed.run = take_value(token);
        } else if (token == "--docs") {
            note_mode(token);
            if (has_inline) {
                parsed.docs = inline_value;
            } else if (index + 1 < args.size() && args[index + 1].rfind('-', 0) != 0) {
                parsed.docs = args[++index];
            } else {
                parsed.docs = "all";
            }
        } else if (token == "--devices") {
            note_mode(token);
            parsed.devices = true;
        } else if (token == "--screen") {
            note_mode(token);
            parsed.screen = true;
        } else if (token == "--get-mouse-pos") {
            note_mode(token);
            parsed.get_mouse_pos = true;
        } else if (token == "--list-api") {
            note_mode(token);
            parsed.list_api = true;
        } else if (token == "--doctor") {
            note_mode(token);
            parsed.doctor = true;
        } else if (token == "--force") {
            parsed.force = true;
        } else if (token == "--dry-run") {
            parsed.dry_run = true;
        } else if (token == "--verbose") {
            parsed.verbose = true;
        } else if (token == "--input-device") {
            parsed.input_device = take_value(token);
        } else if (token == "--output-backend") {
            const std::string value = take_value(token);
            if (value != "auto" && value != "ydotool" && value != "uinput") {
                throw CliUsageError("argument --output-backend: invalid choice: " + py_repr_string(value) +
                                    " (choose from 'auto', 'ydotool', 'uinput')");
            }
            parsed.output_backend = value;
        } else if (token == "--screen-size") {
            parsed.screen_size = take_value(token);
        } else if (!token.empty() && token.front() == '-') {
            throw CliUsageError("unrecognized arguments: " + token);
        } else if (parsed.script.has_value()) {
            throw CliUsageError("unrecognized arguments: " + token);
        } else {
            parsed.script = token;
        }
    }
    return parsed;
}

int run_cli(const std::vector<std::string>& args) { return run_cli(args, PickerFunction()); }

int run_cli(const std::vector<std::string>& args, const PickerFunction& picker) {
    CliArgs parsed;
    try {
        parsed = parse_cli(args);
    } catch (const CliUsageError& error) {
        std::cerr << usage_text();
        std::cerr << "keyloom: error: " << error.what() << "\n";
        return 2;
    }

    if (parsed.version) {
        std::cout << "keyloom " << version_string() << "\n";
        return 0;
    }
    if (parsed.help) {
        std::cout << help_text();
        return 0;
    }
    if (parsed.init) return command_init(*parsed.init, parsed.force);
    if (parsed.check) return command_check(resolve_script_path(*parsed.check));
    if (parsed.run) return command_run(resolve_script_path(*parsed.run), parsed);
    if (parsed.docs) {
        std::cout << render_docs(*parsed.docs) << "\n";
        return 0;
    }
    if (parsed.devices) return command_devices();
    if (parsed.screen) {
        const auto bounds = detect_screen_size();
        std::cout << bounds.first << "x" << bounds.second << "\n";
        return 0;
    }
    if (parsed.get_mouse_pos) return command_get_mouse_pos(parsed, picker);
    if (parsed.list_api) return command_list_api();
    if (parsed.doctor) return command_doctor();
    if (parsed.script) return command_run(resolve_script_path(*parsed.script), parsed);

    std::cout << help_text();
    return 0;
}

}  // namespace keyloom
