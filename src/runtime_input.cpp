#include "runtime.hpp"

#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>

#include "errors.hpp"
#include "keys.hpp"
#include "runtime_internal.hpp"


namespace keyloom {
namespace {

using detail::lower_ascii;
using detail::split_on;

/// True when `event` is the binding for the physical key or mouse button `key`.
bool event_binds_key(const Node& event, const std::string& key) {
    if (event.event_kind == "key") return canonical_key(event.event_key) == key;
    if (event.event_kind == "mouse") return canonical_mouse_event_key(event.event_key) == key;
    return false;
}

}  // namespace


std::vector<std::unique_ptr<InputDevice>> Runtime::open_input_devices(bool include_mice) {
    std::vector<std::string> paths;
    if (!options_.input_device.empty()) {
        paths.push_back(options_.input_device);
    } else {
        paths = list_input_devices();
    }

    std::vector<std::unique_ptr<InputDevice>> devices;
    std::set<std::string> seen;
    for (const auto& path : paths) {
        if (!seen.insert(path).second) continue;
        auto device = InputDevice::open(path);
        if (!device) continue;
        const std::string name = lower_ascii(device->name());
        // Never react to our own injected devices, or to the evdev proxies the
        // Python remappers create when both tools run at once.
        if (name.find("keyloom") != std::string::npos || name.find("ydotool") != std::string::npos ||
            name.find("virtual pointer") != std::string::npos ||
            name.find("remap_clicks") != std::string::npos) {
            continue;
        }
        const bool keyboard = device->is_keyboard();
        const bool mouse = include_mice && device->is_mouse();
        if (!keyboard && !mouse) continue;
        devices.push_back(std::move(device));
    }
    return devices;
}

Task Runtime::read_device(InputDevice* device, std::shared_ptr<PassthroughDevice> passthrough) {
    const std::string path = device->path();
    struct Guard {
        Runtime* runtime;
        InputDevice* device;
        EventSink* passthrough;
        std::string path;

        ~Guard() {
            std::exception_ptr failure;
            const auto remember = [&failure](auto operation) {
                try { operation(); } catch (...) { if (!failure) failure = std::current_exception(); }
            };
            remember([&] {
                release_input_capture(passthrough, [this] { device->ungrab(); }, [this] { device->close(); });
            });
            if (!runtime->scheduler_.stop_requested()) {
                remember([&] { runtime->disconnect_device(path); });
            }
            if (failure) {
                try {
                    std::rethrow_exception(failure);
                } catch (const std::exception& error) {
                    runtime->runtime_error_ = "input disconnect cleanup failed for " + path + ": " + error.what();
                    runtime->say(runtime->runtime_error_);
                } catch (...) {
                    runtime->runtime_error_ = "input disconnect cleanup failed for " + path + ": unknown error";
                    runtime->say(runtime->runtime_error_);
                }
                runtime->scheduler_.request_stop();
            }
            runtime->device_down_.erase(path);
            runtime->device_raw_down_.erase(path);
            runtime->desynchronized_devices_.erase(path);
        }
    } guard{this, device, passthrough.get(), path};

    std::vector<unsigned char> buffer(sizeof(input_event) * 64);
    while (true) {
        co_await readable(device->fd());
        const ssize_t bytes = ::read(device->fd(), buffer.data(), buffer.size());
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            say("Input device " + path + " disconnected: " + std::strerror(errno));
            co_return;
        }
        if (bytes == 0) {
            say("Input device " + path + " disconnected: end of file");
            co_return;
        }
        const auto* raw = reinterpret_cast<const input_event*>(buffer.data());
        const size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
        std::vector<RawEvent> events;
        events.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            events.push_back(RawEvent{raw[index].type, raw[index].code, raw[index].value});
        }
        process_events(events, passthrough.get(), path, [device] { return device->pressed_key_codes(); });
    }
}

void Runtime::process_events(const std::vector<RawEvent>& events, EventSink* sink, const std::string& path,
                             const std::function<std::vector<int>()>& snapshot) {
    for (const auto& event : events) {
        if (event.type == EV_SYN && event.code == SYN_DROPPED) {
            desynchronized_devices_.insert(path);
            continue;
        }
        if (desynchronized_devices_.count(path) != 0) {
            if (event.type != EV_SYN || event.code != SYN_REPORT) continue;
            try {
                if (!snapshot) throw RuntimeErrorKeyloom("no key state snapshot available");
                const auto codes = snapshot();
                const std::set<int> current(codes.begin(), codes.end());
                const auto previous = device_raw_down_[path];
                std::vector<RawEvent> changes;
                for (int code : previous) {
                    if (current.count(code) == 0) changes.push_back({EV_KEY, code, 0});
                }
                for (int code : current) {
                    if (previous.count(code) == 0) changes.push_back({EV_KEY, code, 1});
                }
                changes.push_back({EV_SYN, SYN_REPORT, 0});
                desynchronized_devices_.erase(path);
                process_events(changes, sink, path, snapshot);
                if (scheduler_.stop_requested()) return;
                // EVIOCGKEY flushes queued EV_KEY events in the kernel, but cannot
                // flush events already copied into this read batch. All remaining
                // events in the batch predate the snapshot and must not be replayed
                // or processed, including non-key events.
                break;
            } catch (const std::exception& error) {
                runtime_error_ = "input resynchronization failed for " + path + ": " + error.what();
                say(runtime_error_);
                scheduler_.request_stop();
                return;
            }
        }
        if (sink != nullptr && !is_consumed_key_event(event.type, event.code)) {
            try {
                sink->write_event(event.type, event.code, event.value);
            } catch (const std::exception& error) {
                runtime_error_ = "input passthrough failed for " + path + ": " + error.what();
                say(runtime_error_);
                scheduler_.request_stop();
                return;
            }
        }
        if (event.type != EV_KEY || event.value == 2) continue;
        if (event.value == 1) device_raw_down_[path].insert(event.code);
        else if (event.value == 0) device_raw_down_[path].erase(event.code);
        else continue;
        const auto name = normalize_key_event(event.code);
        if (!name) continue;
        const std::string key = canonical_key(*name);
        if (options_.verbose) {
            say("input " + key + (event.value == 1 ? " down" : " up") + " from " + path);
        }
        try {
            physical_key(path, key, event.value == 1);
        } catch (const std::exception& error) {
            runtime_error_ = "input handling failed for " + path + ": " + error.what();
            say(runtime_error_);
            scheduler_.request_stop();
            return;
        }
        if (scheduler_.stop_requested()) return;
    }
}

void Runtime::seed_device_keys(const std::string& path, const std::vector<int>& codes) {
    for (int code : codes) {
        device_raw_down_[path].insert(code);
        const auto name = normalize_key_event(code);
        if (!name) continue;
        const std::string key = canonical_key(*name);
        device_down_[path].insert(key);
        pressed_keys_.insert(key);
    }
}

void Runtime::disconnect_device(const std::string& path) {
    std::exception_ptr failure;
    const auto found = device_down_.find(path);
    if (found != device_down_.end()) {
        const std::vector<std::string> released(found->second.begin(), found->second.end());
        for (const auto& key : released) {
            try { physical_key(path, key, false); } catch (...) { if (!failure) failure = std::current_exception(); }
        }
        device_down_.erase(path);
    }
    device_raw_down_.erase(path);
    desynchronized_devices_.erase(path);
    if (failure) std::rethrow_exception(failure);
}

void Runtime::physical_key(const std::string& path, const std::string& key, bool down) {
    auto& per_device = device_down_[path];
    if (down) {
        const bool already = pressed_keys_.find(key) != pressed_keys_.end();
        per_device.insert(key);
        pressed_keys_.insert(key);
        if (already) return;
        if (!panic_key_.empty() && key == panic_key_) {
            say("Panic key " + key + " pressed.");
            scheduler_.request_stop();
            return;
        }
        for (const auto& remap : script_.remaps) {
            if (canonical_key(remap.source) != key) continue;
            if (active_remaps_.find(key) != active_remaps_.end()) return;
            std::vector<std::string> names;
            for (const auto& part : split_on(remap.target, '+')) names.push_back(canonical_key(part));
            std::vector<std::string> held;
            try {
                for (const auto& name : names) {
                    hold_key(name);
                    held.push_back(name);
                }
            } catch (...) {
                for (auto it = held.rbegin(); it != held.rend(); ++it) {
                    try { free_key(*it); } catch (...) {}
                }
                throw;
            }
            active_remaps_.emplace(key, std::move(names));
            return;
        }
        for (const auto& event : script_.events) {
            if (!event_binds_key(event, key)) continue;
            // A chord only fires while every modifier of the chord is held.
            if (!chord_modifiers_held(event)) continue;
            if (!hotkey_enabled(event)) continue;
            if (event.edge == "down") {
                spawn_background(safe_dispatch(event, key, "down"));
            } else if (event.edge == "up") {
                active_hotkey_ups_[key].push_back(&event);
            } else if (event.edge == "held") {
                held_tasks_[{key, event.line}] = spawn_held(event, key);
            }
        }
    } else {
        per_device.erase(key);
        bool still_down = false;
        for (const auto& entry : device_down_) {
            if (entry.second.find(key) != entry.second.end()) still_down = true;
        }
        if (still_down || pressed_keys_.find(key) == pressed_keys_.end()) return;
        pressed_keys_.erase(key);
        const auto remap_it = active_remaps_.find(key);
        if (remap_it != active_remaps_.end()) {
            const std::vector<std::string> names = std::move(remap_it->second);
            active_remaps_.erase(remap_it);
            std::exception_ptr failure;
            for (auto it = names.rbegin(); it != names.rend(); ++it) {
                try { free_key(*it); } catch (...) { if (!failure) failure = std::current_exception(); }
            }
            if (failure) std::rethrow_exception(failure);
            return;
        }
        for (auto it = held_tasks_.begin(); it != held_tasks_.end();) {
            if (it->first.first == key) {
                scheduler_.cancel(it->second);
                it = held_tasks_.erase(it);
            } else {
                ++it;
            }
        }
        const auto active_up = active_hotkey_ups_.find(key);
        if (active_up != active_hotkey_ups_.end()) {
            const auto events = std::move(active_up->second);
            active_hotkey_ups_.erase(active_up);
            for (const Node* event : events) spawn_background(safe_dispatch(*event, key, "up"));
        }
    }
}

}  // namespace keyloom
