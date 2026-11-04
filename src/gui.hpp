#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct _XDisplay;
using Display = _XDisplay;
using Atom = unsigned long;
using Window = unsigned long;
using Font = unsigned long;
struct _XGC;
using GC = _XGC*;

#include "ast.hpp"

namespace keyloom {

/// A persistent, native X11 panel built from the declarative widgets in a
/// `gui "title" { ... }` block. GuiHost owns the connection for the panel; it
/// does not provide a general toolkit or style system.
class GuiHost {
public:
    enum class EventKind { Click, Close };

    struct Event {
        EventKind kind = EventKind::Click;
        std::string panel_title;
        std::string widget_id;
    };

    GuiHost(const GuiHost&) = delete;
    GuiHost& operator=(const GuiHost&) = delete;
    ~GuiHost();

    static std::unique_ptr<GuiHost> create(const GuiPanel& panel);

    int fd() const;
    const std::string& title() const { return panel_.title; }
    bool is_closed() const { return closed_; }

    void set_event_handler(std::function<void(const Event&)> handler) { event_handler_ = std::move(handler); }

    /// Read all pending X events, redraw dirty widgets and dispatch semantic
    /// click/close notifications. Must be called when the X connection is
    /// readable; may also be called once after creation.
    void process_events();

    void close();
    bool has_widget(const std::string& widget_id) const;
    std::string value(const std::string& widget_id) const;
    void set_value(const std::string& widget_id, const std::string& value);
    void set_text(const std::string& widget_id, const std::string& text);
    void set_visible(bool visible);

private:
    struct Rect {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    explicit GuiHost(const GuiPanel& panel);

    void layout();
    void draw_locked();
    void create_window();
    void write_debug_rects() const;
    int widget_index_by_id(const std::string& id) const;
    int widget_index_at(int x, int y) const;
    void focus_input(int index);
    void redraw_if_open_locked();
    std::string clipped_text(const std::string& text, int max_width) const;

    const GuiPanel& panel_;
    Display* display_ = nullptr;
    Window window_ = 0;
    Atom wm_delete_window_ = 0;
    Atom net_wm_state_ = 0;
    Atom net_wm_state_above_ = 0;
    GC gc_ = 0;
    Font font_ = 0;
    unsigned long background_ = 0;
    unsigned long foreground_ = 0;
    unsigned long control_ = 0;
    unsigned long control_text_ = 0;
    unsigned long border_ = 0;
    unsigned long inner_ = 0;
    unsigned long accent_ = 0;
    int width_ = 0;
    int height_ = 0;
    int focused_input_ = -1;
    bool closed_ = false;
    bool mapped_ = false;
    std::vector<Rect> rects_;
    std::vector<std::string> texts_;
    std::function<void(const Event&)> event_handler_;
    mutable std::mutex mutex_;
};

}  // namespace keyloom
