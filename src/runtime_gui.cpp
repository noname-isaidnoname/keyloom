#include "runtime.hpp"

#include <algorithm>
#include <fstream>

#include "errors.hpp"
#include "subprocess.hpp"


namespace keyloom {
namespace {

}  // namespace


Task Runtime::gui_loop(GuiHost* host) {
    while (host != nullptr && !host->is_closed() && !scheduler_.stop_requested()) {
        try {
            host->process_events();
        } catch (const std::exception& error) {
            runtime_error_ = std::string("GUI event handling failed: ") + error.what();
            say(runtime_error_);
            scheduler_.request_stop();
            co_return;
        }
        if (host->is_closed() || scheduler_.stop_requested()) break;
        const int fd = host->fd();
        if (fd < 0) break;
        try {
            co_await readable(fd);
        } catch (const TaskCancelled&) {
            throw;
        } catch (const std::exception& error) {
            if (host->is_closed() || scheduler_.stop_requested()) break;
            runtime_error_ = std::string("GUI connection failed: ") + error.what();
            say(runtime_error_);
            scheduler_.request_stop();
            break;
        }
    }
}

void Runtime::dispatch_gui_event(const GuiHost::Event& event) {
    const std::string edge = event.kind == GuiHost::EventKind::Click ? "clicked" : "close";
    const std::string key = event.kind == GuiHost::EventKind::Click ? event.widget_id : "window";
    bool handled = false;
    for (const auto& candidate : script_.events) {
        if (candidate.event_kind == "gui" && candidate.event_key == key && candidate.edge == edge) {
            handled = true;
            spawn_background(safe_dispatch(candidate, key, edge));
        }
    }
    if (!handled && event.kind == GuiHost::EventKind::Close) scheduler_.request_stop();
}

void Runtime::gui_alert(const std::string& message, const std::string& title) {
    const std::string executable = which("zenity");
    if (!executable.empty()) {
        const CommandResult result =
            run_command({executable, "--info", "--title=" + title, "--text=" + message}, std::string(), 120000);
        if (result.returncode != 0 && !result.spawn_failed) {
            throw RuntimeErrorKeyloom("GUI alert failed with exit code " + std::to_string(result.returncode));
        }
        return;
    }
    const std::string kdialog = which("kdialog");
    if (!kdialog.empty()) {
        const CommandResult result =
            run_command({kdialog, "--title", title, "--msgbox", message}, std::string(), 120000);
        if (result.returncode != 0 && !result.spawn_failed) {
            throw RuntimeErrorKeyloom("GUI alert failed with exit code " + std::to_string(result.returncode));
        }
        return;
    }
    const std::string yad = which("yad");
    if (!yad.empty()) {
        const CommandResult result =
            run_command({yad, "--info", "--title=" + title, "--text=" + message}, std::string(), 120000);
        if (result.returncode != 0 && !result.spawn_failed) {
            throw RuntimeErrorKeyloom("GUI alert failed with exit code " + std::to_string(result.returncode));
        }
        return;
    }
    throw RuntimeErrorKeyloom("GUI dialogs need zenity, kdialog, or yad.");
}

std::string Runtime::gui_input(const std::string& prompt, const std::string& default_value, const std::string& title) {
    const std::string executable = which("zenity");
    if (!executable.empty()) {
        const CommandResult result = run_command(
            {executable, "--entry", "--title=" + title, "--text=" + prompt, "--entry-text=" + default_value},
            std::string(), 120000);
        if (result.returncode != 0) return std::string();
        std::string value = result.out;
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        return value;
    }
    const std::string kdialog = which("kdialog");
    if (!kdialog.empty()) {
        const CommandResult result =
            run_command({kdialog, "--title", title, "--inputbox", prompt, default_value}, std::string(), 120000);
        if (result.returncode != 0) return std::string();
        std::string value = result.out;
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        return value;
    }
    const std::string yad = which("yad");
    if (!yad.empty()) {
        const CommandResult result = run_command(
            {yad, "--entry", "--title=" + title, "--text=" + prompt, "--entry-text=" + default_value},
            std::string(), 120000);
        if (result.returncode != 0) return std::string();
        std::string value = result.out;
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        return value;
    }
    throw RuntimeErrorKeyloom("GUI input needs zenity, kdialog, or yad.");
}

bool Runtime::gui_confirm(const std::string& question, const std::string& title) {
    const std::string executable = which("zenity");
    if (!executable.empty()) {
        const CommandResult result =
            run_command({executable, "--question", "--title=" + title, "--text=" + question}, std::string(), 120000);
        return result.returncode == 0;
    }
    const std::string kdialog = which("kdialog");
    if (!kdialog.empty()) {
        const CommandResult result =
            run_command({kdialog, "--title", title, "--yesno", question}, std::string(), 120000);
        return result.returncode == 0;
    }
    const std::string yad = which("yad");
    if (!yad.empty()) {
        const CommandResult result =
            run_command({yad, "--question", "--title=" + title, "--text=" + question}, std::string(), 120000);
        return result.returncode == 0;
    }
    throw RuntimeErrorKeyloom("GUI confirmation needs zenity, kdialog, or yad.");
}

std::map<std::string, std::string> Runtime::active_window_info() const {
    std::map<std::string, std::string> info;
    info["title"] = std::string();
    info["class"] = std::string();
    info["pid"] = std::string();
    info["process"] = std::string();

    const std::string xdotool = which("xdotool");
    if (!xdotool.empty()) {
        const CommandResult window_id = run_command({xdotool, "getactivewindow"}, std::string(), 250);
        if (window_id.returncode == 0) {
            std::string id = window_id.out;
            while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ' || id.back() == '\t')) {
                id.pop_back();
            }
            if (!id.empty()) {
                const CommandResult title = run_command({xdotool, "getwindowname", id}, std::string(), 250);
                if (title.returncode == 0) {
                    std::string value = title.out;
                    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
                    info["title"] = value;
                }
                const CommandResult window_class = run_command({xdotool, "getwindowclassname", id}, std::string(), 250);
                if (window_class.returncode == 0) {
                    std::string value = window_class.out;
                    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
                    info["class"] = value;
                }
                const CommandResult pid = run_command({xdotool, "getwindowpid", id}, std::string(), 250);
                if (pid.returncode == 0) {
                    std::string value = pid.out;
                    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' ||
                                             value.back() == '\t')) {
                        value.pop_back();
                    }
                    if (!value.empty() &&
                        std::all_of(value.begin(), value.end(), [](char c) {
                            return std::isdigit(static_cast<unsigned char>(c)) != 0;
                        })) {
                        info["pid"] = value;
                    }
                }
            }
        }
    }

    if (info["pid"].empty()) {
        const std::string kdotool = which("kdotool");
        if (!kdotool.empty()) {
            const CommandResult active = run_command({kdotool, "getactivewindow"}, std::string(), 250);
            if (active.returncode == 0) {
                std::string id = active.out;
                while (!id.empty() &&
                       (id.back() == '\n' || id.back() == '\r' || id.back() == ' ' || id.back() == '\t')) {
                    id.pop_back();
                }
                if (!id.empty()) {
                    const CommandResult title = run_command({kdotool, "getwindowname", id}, std::string(), 250);
                    if (title.returncode == 0) {
                        std::string value = title.out;
                        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
                        info["title"] = value;
                    }
                    const CommandResult window_class = run_command({kdotool, "getwindowclassname", id}, std::string(), 250);
                    if (window_class.returncode == 0) {
                        std::string value = window_class.out;
                        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
                        info["class"] = value;
                    }
                    const CommandResult pid = run_command({kdotool, "getwindowpid", id}, std::string(), 250);
                    if (pid.returncode == 0) {
                        std::string value = pid.out;
                        while (!value.empty() &&
                               (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' ||
                                value.back() == '\t')) {
                            value.pop_back();
                        }
                        if (!value.empty() &&
                            std::all_of(value.begin(), value.end(), [](char c) {
                                return std::isdigit(static_cast<unsigned char>(c)) != 0;
                            })) {
                            info["pid"] = value;
                        }
                    }
                }
            }
        }
    }

    if (!info["pid"].empty()) {
        std::ifstream comm("/proc/" + info["pid"] + "/comm");
        std::string process;
        comm >> process;
        if (!comm.fail()) info["process"] = process;
    }
    return info;
}

}  // namespace keyloom
