#include "gui.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <utility>

#include "errors.hpp"
#include "value.hpp"

namespace keyloom {
namespace {

constexpr int kPadding = 16;
constexpr int kRowGap = 8;
constexpr int kInputHeight = 30;
constexpr int kButtonHeight = 30;
constexpr int kLabelMinHeight = 22;
constexpr int kInputExtraWidth = 96;

unsigned long allocate_named_color(Display* display, const char* name, Colormap colormap) {
    XColor screen_color{};
    XColor exact_color{};
    if (XAllocNamedColor(display, colormap, name, &screen_color, &exact_color) == 0) {
        return XBlackPixel(display, DefaultScreen(display));
    }
    return screen_color.pixel;
}

bool is_printable_ascii(char c) {
    const unsigned char byte = static_cast<unsigned char>(c);
    return byte >= 32 && byte <= 126;
}

}  // namespace

GuiHost::GuiHost(const GuiPanel& panel) : panel_(panel) {
    texts_.reserve(panel_.widgets.size());
    for (const auto& widget : panel_.widgets) texts_.push_back(widget.text);
}

GuiHost::~GuiHost() {
    close();
    if (font_ != 0) {
        XFreeFont(display_, reinterpret_cast<XFontStruct*>(font_));
        font_ = 0;
    }
    if (gc_ != 0) {
        XFreeGC(display_, gc_);
        gc_ = 0;
    }
    if (display_ != nullptr) {
        XCloseDisplay(display_);
        display_ = nullptr;
    }
}

std::unique_ptr<GuiHost> GuiHost::create(const GuiPanel& panel) {
    auto host = std::unique_ptr<GuiHost>(new GuiHost(panel));
    host->display_ = XOpenDisplay(nullptr);
    if (host->display_ == nullptr) {
        throw RuntimeErrorKeyloom("GUI panel \"" + panel.title + "\" needs an X11 display (Xwayland is sufficient).");
    }

    const int screen = DefaultScreen(host->display_);
    const Colormap colormap = DefaultColormap(host->display_, screen);
    host->background_ = allocate_named_color(host->display_, "#ffffff", colormap);
    host->control_ = allocate_named_color(host->display_, "#eceff1", colormap);
    host->control_text_ = allocate_named_color(host->display_, "#111827", colormap);
    host->border_ = allocate_named_color(host->display_, "#9ca3af", colormap);
    host->inner_ = allocate_named_color(host->display_, "#ffffff", colormap);
    host->accent_ = allocate_named_color(host->display_, "#2563eb", colormap);
    host->foreground_ = allocate_named_color(host->display_, "#111827", colormap);
    host->gc_ = XCreateGC(host->display_, DefaultRootWindow(host->display_), 0, nullptr);
    host->font_ = reinterpret_cast<Font>(XLoadQueryFont(host->display_, "fixed"));
    if (host->font_ == 0) {
        throw RuntimeErrorKeyloom("GUI panel needs the 'fixed' X11 font.");
    }
    XSetFont(host->display_, host->gc_, reinterpret_cast<XFontStruct*>(host->font_)->fid);

    host->layout();
    host->create_window();
    host->write_debug_rects();
    host->redraw_if_open_locked();
    return host;
}

int GuiHost::fd() const {
    return display_ == nullptr ? -1 : XConnectionNumber(display_);
}

void GuiHost::layout() {
    XFontStruct* font = reinterpret_cast<XFontStruct*>(font_);
    int widest_label = 0;
    int widest_input = 0;
    int widest_button = 0;
    for (const auto& widget : panel_.widgets) {
        const unsigned char* text = reinterpret_cast<const unsigned char*>(widget.text.c_str());
        const int text_width = XTextWidth(font, reinterpret_cast<const char*>(text), static_cast<int>(widget.text.size()));
        if (widget.kind == GuiWidget::Kind::Label) widest_label = std::max(widest_label, text_width);
        if (widget.kind == GuiWidget::Kind::Input) widest_input = std::max(widest_input, text_width);
        if (widget.kind == GuiWidget::Kind::Button) widest_button = std::max(widest_button, text_width);
    }

    width_ = std::max({240, widest_label + 2 * kPadding, widest_input + kInputExtraWidth, widest_button + 2 * kPadding});
    width_ = std::min(width_, 760);
    int y = kPadding;
    rects_.clear();
    rects_.reserve(panel_.widgets.size());
    for (const auto& widget : panel_.widgets) {
        int height = kInputHeight;
        if (widget.kind == GuiWidget::Kind::Label) {
            height = std::max(kLabelMinHeight, font->ascent + font->descent + 4);
        } else if (widget.kind == GuiWidget::Kind::Button) {
            height = kButtonHeight;
        }
        Rect rect{kPadding, y, width_ - 2 * kPadding, height};
        if (widget.kind == GuiWidget::Kind::Button) {
            rect.width = std::min(width_ - 2 * kPadding, XTextWidth(font, widget.text.c_str(), static_cast<int>(widget.text.size())) + 32);
        }
        rects_.push_back(rect);
        y += height + kRowGap;
    }
    height_ = y - kRowGap + kPadding;
}

void GuiHost::create_window() {
    const int screen = DefaultScreen(display_);
    const Window root = DefaultRootWindow(display_);
    wm_delete_window_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
    net_wm_state_ = XInternAtom(display_, "_NET_WM_STATE", False);
    net_wm_state_above_ = XInternAtom(display_, "_NET_WM_STATE_ABOVE", False);

    XSetWindowAttributes attributes{};
    attributes.background_pixel = background_;
    attributes.border_pixel = XBlackPixel(display_, screen);
    attributes.backing_store = Always;
    attributes.event_mask = ExposureMask | ButtonPressMask | KeyPressMask | StructureNotifyMask;
    window_ = XCreateWindow(display_, root, 0, 0, static_cast<unsigned>(width_), static_cast<unsigned>(height_), 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWBackPixel | CWBorderPixel | CWBackingStore | CWEventMask, &attributes);
    XStoreName(display_, window_, panel_.title.c_str());

    XClassHint class_hint{};
    class_hint.res_name = const_cast<char*>("keyloom");
    class_hint.res_class = const_cast<char*>("Keyloom");
    XSetClassHint(display_, window_, &class_hint);

    XSetWMProtocols(display_, window_, &wm_delete_window_, 1);
    XChangeProperty(display_, window_, net_wm_state_, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&net_wm_state_above_), 1);

    XMapWindow(display_, window_);
    XFlush(display_);
    mapped_ = true;
}

int GuiHost::widget_index_by_id(const std::string& id) const {
    for (size_t index = 0; index < panel_.widgets.size(); ++index) {
        if (panel_.widgets.at(index).id == id) return static_cast<int>(index);
    }
    return -1;
}

int GuiHost::widget_index_at(int x, int y) const {
    for (size_t index = 0; index < rects_.size(); ++index) {
        const Rect& rect = rects_.at(index);
        if (x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void GuiHost::focus_input(int index) {
    if (index >= 0 && panel_.widgets.at(static_cast<size_t>(index)).kind == GuiWidget::Kind::Input) {
        focused_input_ = index;
    } else {
        focused_input_ = -1;
    }
}

std::string GuiHost::clipped_text(const std::string& text, int max_width) const {
    XFontStruct* font = reinterpret_cast<XFontStruct*>(font_);
    if (XTextWidth(font, text.c_str(), static_cast<int>(text.size())) <= max_width) return text;
    std::string clipped = text;
    while (!clipped.empty() &&
           XTextWidth(font, clipped.c_str(), static_cast<int>(clipped.size())) > max_width) {
        clipped.erase(clipped.begin());
    }
    return clipped;
}

void GuiHost::draw_locked() {
    if (display_ == nullptr || closed_) return;
    XClearWindow(display_, window_);
    for (size_t index = 0; index < panel_.widgets.size(); ++index) {
        const GuiWidget& widget = panel_.widgets.at(index);
        const Rect& rect = rects_.at(index);
        XFontStruct* font = reinterpret_cast<XFontStruct*>(font_);
        const int text_y = rect.y + (widget.kind == GuiWidget::Kind::Label ? font->ascent + 2
                                      : (rect.height + font->ascent - font->descent) / 2);

        if (widget.kind == GuiWidget::Kind::Label) {
            XSetForeground(display_, gc_, foreground_);
            const std::string& text = texts_[index];
            XDrawString(display_, window_, gc_, rect.x, text_y, text.c_str(), static_cast<int>(text.size()));
            continue;
        }

        if (widget.kind == GuiWidget::Kind::Input) {
            const bool focused = static_cast<int>(index) == focused_input_;
            XSetForeground(display_, gc_, inner_);
            XFillRectangle(display_, window_, gc_, rect.x, rect.y, static_cast<unsigned>(rect.width), static_cast<unsigned>(rect.height));
            XSetForeground(display_, gc_, focused ? accent_ : border_);
            XDrawRectangle(display_, window_, gc_, rect.x, rect.y, static_cast<unsigned>(rect.width - 1),
                           static_cast<unsigned>(rect.height - 1));
            XSetForeground(display_, gc_, foreground_);
            const std::string visible = clipped_text(texts_[index], rect.width - 12);
            XDrawString(display_, window_, gc_, rect.x + 6, text_y, visible.c_str(), static_cast<int>(visible.size()));
            continue;
        }

        XSetForeground(display_, gc_, control_);
        XFillRectangle(display_, window_, gc_, rect.x, rect.y, static_cast<unsigned>(rect.width), static_cast<unsigned>(rect.height));
        XSetForeground(display_, gc_, border_);
        XDrawRectangle(display_, window_, gc_, rect.x, rect.y, static_cast<unsigned>(rect.width - 1),
                       static_cast<unsigned>(rect.height - 1));
        XSetForeground(display_, gc_, control_text_);
        const std::string& text = texts_[index];
        const int text_width = XTextWidth(font, text.c_str(), static_cast<int>(text.size()));
        const int text_x = rect.x + std::max(0, (rect.width - text_width) / 2);
        XDrawString(display_, window_, gc_, text_x, text_y, text.c_str(), static_cast<int>(text.size()));
    }
    XFlush(display_);
}

void GuiHost::redraw_if_open_locked() {
    if (closed_) return;
    draw_locked();
}

void GuiHost::write_debug_rects() const {
    const char* path = std::getenv("KEYLOOM_GUI_DEBUG_RECTS");
    if (path == nullptr || *path == '\0') return;
    FILE* file = std::fopen(path, "w");
    if (file == nullptr) return;
    for (size_t index = 0; index < panel_.widgets.size(); ++index) {
        const auto& widget = panel_.widgets.at(index);
        const auto& rect = rects_.at(index);
        const char* kind = widget.kind == GuiWidget::Kind::Label ? "label"
                           : widget.kind == GuiWidget::Kind::Input ? "input" : "button";
        std::fprintf(file, "%s %s %d %d %d %d\n", kind, widget.id.c_str(), rect.x, rect.y, rect.width, rect.height);
    }
    std::fclose(file);
}

void GuiHost::process_events() {
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (display_ == nullptr || closed_) return;
        bool dirty = false;
        while (XPending(display_) > 0) {
            XEvent event{};
            XNextEvent(display_, &event);
            if (event.type == Expose) {
                if (event.xexpose.count == 0) dirty = true;
                continue;
            }
            if (event.type == MapNotify) {
                mapped_ = true;
                continue;
            }
            if (event.type == ButtonPress) {
                if (event.xbutton.button != Button1) continue;
                const int index = widget_index_at(event.xbutton.x, event.xbutton.y);
                if (index < 0) {
                    focus_input(-1);
                    dirty = true;
                    continue;
                }
                const GuiWidget& widget = panel_.widgets.at(static_cast<size_t>(index));
                if (widget.kind == GuiWidget::Kind::Input) {
                    focus_input(index);
                    dirty = true;
                    continue;
                }
                if (widget.kind == GuiWidget::Kind::Button) {
                    focus_input(-1);
                    dirty = true;
                    events.push_back({EventKind::Click, panel_.title, widget.id});
                    continue;
                }
                dirty = true;
                continue;
            }
            if (event.type == KeyPress && focused_input_ >= 0) {
                std::string& text = texts_.at(static_cast<size_t>(focused_input_));
                char buffer[32]{};
                KeySym keysym{};
                const int length = XLookupString(&event.xkey, buffer, sizeof(buffer) - 1, &keysym, nullptr);
                if (keysym == XK_BackSpace) {
                    if (!text.empty()) text.pop_back();
                    dirty = true;
                } else if (keysym == XK_Delete) {
                    text.clear();
                    dirty = true;
                } else if (keysym == XK_Tab) {
                    size_t next = static_cast<size_t>(focused_input_) + 1;
                    for (;;) {
                        if (next >= panel_.widgets.size()) next = 0;
                        if (panel_.widgets.at(next).kind == GuiWidget::Kind::Input) break;
                        ++next;
                        if (next == static_cast<size_t>(focused_input_)) break;
                    }
                    focus_input(static_cast<int>(next));
                    dirty = true;
                } else if (length > 0 && is_printable_ascii(buffer[0])) {
                    text.append(buffer, static_cast<size_t>(length));
                    dirty = true;
                }
                continue;
            }
            if (event.type == ClientMessage &&
                static_cast<Atom>(event.xclient.message_type) == XInternAtom(display_, "WM_PROTOCOLS", False) &&
                static_cast<Atom>(event.xclient.data.l[0]) == wm_delete_window_) {
                XDestroyWindow(display_, window_);
                closed_ = true;
                events.push_back({EventKind::Close, panel_.title, "window"});
            }
        }
        if (dirty) draw_locked();
    }

    if (event_handler_) {
        for (const auto& event : events) event_handler_(event);
    }
}

void GuiHost::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || display_ == nullptr || window_ == 0) return;
    XDestroyWindow(display_, window_);
    XFlush(display_);
    closed_ = true;
}

bool GuiHost::has_widget(const std::string& widget_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return widget_index_by_id(widget_id) >= 0;
}

std::string GuiHost::value(const std::string& widget_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = widget_index_by_id(widget_id);
    if (index < 0) {
        throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(widget_id) + " is not declared in a gui block.");
    }
    const auto& widget = panel_.widgets.at(static_cast<size_t>(index));
    if (widget.kind != GuiWidget::Kind::Input) {
        throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(widget_id) + " is not an input.");
    }
    return texts_.at(static_cast<size_t>(index));
}

void GuiHost::set_value(const std::string& widget_id, const std::string& value) {
    set_text(widget_id, value);
}

void GuiHost::set_text(const std::string& widget_id, const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = widget_index_by_id(widget_id);
    if (index < 0) {
        throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(widget_id) + " is not declared in a gui block.");
    }
    const auto& widget = panel_.widgets.at(static_cast<size_t>(index));
    if (widget.kind == GuiWidget::Kind::Label) {
        throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(widget_id) + " is a label and has no variable text target.");
    }
    texts_[static_cast<size_t>(index)] = text;
    if (!closed_) draw_locked();
}

void GuiHost::set_visible(bool visible) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (display_ == nullptr || closed_) return;
    if (visible) {
        XMapWindow(display_, window_);
        mapped_ = true;
    } else {
        XUnmapWindow(display_, window_);
        mapped_ = false;
    }
    XFlush(display_);
}

}  // namespace keyloom
