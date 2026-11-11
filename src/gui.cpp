#include "gui.hpp"

#include <X11/keysym.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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

template <typename T> using XcbReply = std::unique_ptr<T, decltype(&std::free)>;

xcb_atom_t atom(xcb_connection_t* connection, const char* name) {
    const auto cookie = xcb_intern_atom(connection, 0, static_cast<uint16_t>(std::strlen(name)), name);
    XcbReply<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(connection, cookie, nullptr), &std::free);
    if (!reply) throw RuntimeErrorKeyloom("GUI could not query X11 atoms");
    return reply->atom;
}

uint32_t color(xcb_connection_t* connection, xcb_colormap_t colormap, const char* name, uint32_t fallback) {
    const unsigned long rgb = std::strtoul(name + 1, nullptr, 16);
    const auto red = static_cast<uint16_t>(((rgb >> 16) & 0xff) * 257);
    const auto green = static_cast<uint16_t>(((rgb >> 8) & 0xff) * 257);
    const auto blue = static_cast<uint16_t>((rgb & 0xff) * 257);
    const auto cookie = xcb_alloc_color(connection, colormap, red, green, blue);
    XcbReply<xcb_alloc_color_reply_t> reply(xcb_alloc_color_reply(connection, cookie, nullptr), &std::free);
    return reply ? reply->pixel : fallback;
}

void foreground(xcb_connection_t* connection, xcb_gcontext_t gc, uint32_t value) {
    xcb_change_gc(connection, gc, XCB_GC_FOREGROUND, &value);
}

void draw_text(xcb_connection_t* connection, xcb_window_t window, xcb_gcontext_t gc,
               int x, int y, int font_width, const std::string& text) {
    // PolyText uses transparent glyphs, unlike ImageText's background fill.
    for (size_t offset = 0; offset < text.size();) {
        const auto length = static_cast<uint8_t>(std::min<size_t>(text.size() - offset, 254));
        std::vector<uint8_t> item{length, 0};
        item.insert(item.end(), text.begin() + static_cast<std::ptrdiff_t>(offset),
                    text.begin() + static_cast<std::ptrdiff_t>(offset + length));
        xcb_poly_text_8(connection, window, gc, static_cast<int16_t>(x), static_cast<int16_t>(y),
                        static_cast<uint32_t>(item.size()), item.data());
        x += static_cast<int>(length) * font_width;
        offset += length;
    }
}

}  // namespace

GuiHost::GuiHost(const GuiPanel& panel) : panel_(panel) {
    texts_.reserve(panel_.widgets.size());
    for (const auto& widget : panel_.widgets) texts_.push_back(widget.text);
}

GuiHost::~GuiHost() {
    close();
    if (connection_) {
        auto* connection = connection_->get();
        if (connection != nullptr && !connection_lost_locked()) {
            if (gc_ != 0) xcb_free_gc(connection, gc_);
            if (font_ != 0) xcb_close_font(connection, font_);
            xcb_flush(connection);
        }
        connection_->close();
    }
}

std::unique_ptr<GuiHost> create_wayland_gui(const GuiPanel& panel);

std::unique_ptr<GuiHost> GuiHost::create(const GuiPanel& panel, const std::string& compositor) {
    if (compositor == "wayland") return create_wayland_gui(panel);
    auto host = std::unique_ptr<GuiHost>(new GuiHost(panel));
    try {
        host->connection_ = std::make_unique<XcbConnection>();
    } catch (const RuntimeErrorKeyloom&) {
        throw RuntimeErrorKeyloom("GUI panel \"" + panel.title + "\" needs an X11 display (Xwayland is sufficient).");
    }
    auto* connection = host->connection_->get();
    const xcb_setup_t* setup = xcb_get_setup(connection);
    auto screens = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < host->connection_->screen_number() && screens.rem != 0; ++i) xcb_screen_next(&screens);
    if (screens.rem == 0) throw RuntimeErrorKeyloom("GUI could not find X11 screen");
    xcb_screen_t* screen = screens.data;

    host->background_ = color(connection, screen->default_colormap, "#ffffff", screen->black_pixel);
    host->control_ = color(connection, screen->default_colormap, "#eceff1", screen->black_pixel);
    host->control_text_ = color(connection, screen->default_colormap, "#111827", screen->black_pixel);
    host->border_ = color(connection, screen->default_colormap, "#9ca3af", screen->black_pixel);
    host->inner_ = color(connection, screen->default_colormap, "#ffffff", screen->black_pixel);
    host->accent_ = color(connection, screen->default_colormap, "#2563eb", screen->black_pixel);
    host->foreground_ = color(connection, screen->default_colormap, "#111827", screen->black_pixel);

    host->font_ = xcb_generate_id(connection);
    const auto open = xcb_open_font_checked(connection, host->font_, 5, "fixed");
    XcbReply<xcb_generic_error_t> font_error(xcb_request_check(connection, open), &std::free);
    if (font_error || host->connection_->error()) throw RuntimeErrorKeyloom("GUI panel needs the 'fixed' X11 font.");
    XcbReply<xcb_query_font_reply_t> font(xcb_query_font_reply(connection, xcb_query_font(connection, host->font_), nullptr), &std::free);
    if (!font) throw RuntimeErrorKeyloom("GUI could not query the 'fixed' X11 font.");
    host->font_width_ = font->max_bounds.character_width;
    host->font_ascent_ = font->font_ascent;
    host->font_descent_ = font->font_descent;
    host->gc_ = xcb_generate_id(connection);
    const uint32_t gc_values[] = {static_cast<uint32_t>(host->foreground_), host->font_};
    xcb_create_gc(connection, host->gc_, screen->root, XCB_GC_FOREGROUND | XCB_GC_FONT, gc_values);

    host->min_keycode_ = setup->min_keycode;
    host->refresh_keyboard_mapping();
    if (host->keysyms_.empty() || host->keysyms_per_keycode_ <= 0)
        throw RuntimeErrorKeyloom("GUI could not query X11 keyboard mapping.");

    host->layout();
    host->create_window();
    host->write_debug_rects();
    host->redraw_if_open_locked();
    if (host->connection_->error()) throw RuntimeErrorKeyloom("GUI X11 connection lost during startup.");
    return host;
}

int GuiHost::fd() const { return connection_ ? connection_->fd() : -1; }

void GuiHost::refresh_keyboard_mapping() {
    if (!connection_ || connection_->error() != 0) return;
    auto* connection = connection_->get();
    auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (int i = 0; i < connection_->screen_number(); ++i) xcb_screen_next(&screens);
    // Use setup min/max to cover the full range; MappingNotify can affect any keycode.
    const auto* setup = xcb_get_setup(connection);
    min_keycode_ = setup->min_keycode;
    const auto count = static_cast<uint8_t>(setup->max_keycode - setup->min_keycode + 1);
    XcbReply<xcb_get_keyboard_mapping_reply_t> mapping(
        xcb_get_keyboard_mapping_reply(connection, xcb_get_keyboard_mapping(connection, setup->min_keycode, count), nullptr), &std::free);
    if (!mapping) {
        keysyms_.clear();
        keysyms_per_keycode_ = 0;
        return;
    }
    keysyms_per_keycode_ = mapping->keysyms_per_keycode;
    const auto* syms = xcb_get_keyboard_mapping_keysyms(mapping.get());
    keysyms_.assign(syms, syms + xcb_get_keyboard_mapping_keysyms_length(mapping.get()));
}

void GuiHost::layout() {
    int widest_label = 0, widest_input = 0, widest_button = 0;
    for (const auto& widget : panel_.widgets) {
        const int width = font_width_ * static_cast<int>(widget.text.size());
        if (widget.kind == GuiWidget::Kind::Label) widest_label = std::max(widest_label, width);
        if (widget.kind == GuiWidget::Kind::Input) widest_input = std::max(widest_input, width);
        if (widget.kind == GuiWidget::Kind::Button) widest_button = std::max(widest_button, width);
    }
    width_ = std::min(760, std::max({240, widest_label + 2 * kPadding, widest_input + kInputExtraWidth, widest_button + 2 * kPadding}));
    int y = kPadding;
    rects_.clear();
    for (const auto& widget : panel_.widgets) {
        int height = kInputHeight;
        if (widget.kind == GuiWidget::Kind::Label) height = std::max(kLabelMinHeight, font_ascent_ + font_descent_ + 4);
        else if (widget.kind == GuiWidget::Kind::Button) height = kButtonHeight;
        Rect rect{kPadding, y, width_ - 2 * kPadding, height};
        if (widget.kind == GuiWidget::Kind::Button)
            rect.width = std::min(rect.width, font_width_ * static_cast<int>(widget.text.size()) + 32);
        rects_.push_back(rect);
        y += height + kRowGap;
    }
    height_ = y - kRowGap + kPadding;
}

void GuiHost::create_window() {
    auto* connection = connection_->get();
    auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (int i = 0; i < connection_->screen_number(); ++i) xcb_screen_next(&screens);
    const auto* screen = screens.data;
    wm_protocols_ = atom(connection, "WM_PROTOCOLS");
    wm_delete_window_ = atom(connection, "WM_DELETE_WINDOW");
    net_wm_state_ = atom(connection, "_NET_WM_STATE");
    net_wm_state_above_ = atom(connection, "_NET_WM_STATE_ABOVE");
    window_ = xcb_generate_id(connection);
    const uint32_t values[] = {static_cast<uint32_t>(background_), screen->black_pixel,
        XCB_BACKING_STORE_ALWAYS, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, window_, screen->root, 0, 0,
                      static_cast<uint16_t>(width_), static_cast<uint16_t>(height_), 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_BACKING_STORE | XCB_CW_EVENT_MASK, values);
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window_, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        static_cast<uint32_t>(panel_.title.size()), panel_.title.data());
    constexpr char class_name[] = "keyloom\0Keyloom\0";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window_, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                        sizeof(class_name) - 1, class_name);
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window_, wm_protocols_, XCB_ATOM_ATOM, 32, 1, &wm_delete_window_);
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window_, net_wm_state_, XCB_ATOM_ATOM, 32, 1, &net_wm_state_above_);
    xcb_map_window(connection, window_);
    xcb_flush(connection);
}

int GuiHost::widget_index_by_id(const std::string& id) const {
    for (size_t i = 0; i < panel_.widgets.size(); ++i) if (panel_.widgets[i].id == id) return static_cast<int>(i);
    return -1;
}

int GuiHost::widget_index_at(int x, int y) const {
    for (size_t i = 0; i < rects_.size(); ++i) {
        const auto& rect = rects_[i];
        if (x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height) return static_cast<int>(i);
    }
    return -1;
}

void GuiHost::focus_input(int index) {
    focused_input_ = index >= 0 && panel_.widgets.at(static_cast<size_t>(index)).kind == GuiWidget::Kind::Input ? index : -1;
}

std::string GuiHost::clipped_text(const std::string& text, int max_width) const {
    const size_t max_chars = static_cast<size_t>(std::max(0, max_width) / std::max(1, font_width_));
    return text.size() <= max_chars ? text : text.substr(text.size() - max_chars);
}

void GuiHost::draw_locked() {
    if (!connection_ || closed_ || connection_lost_locked()) return;
    auto* connection = connection_->get();
    xcb_clear_area(connection, 0, window_, 0, 0, 0, 0);
    for (size_t i = 0; i < panel_.widgets.size(); ++i) {
        const auto& widget = panel_.widgets[i];
        const auto& rect = rects_[i];
        const int text_y = rect.y + (widget.kind == GuiWidget::Kind::Label ? font_ascent_ + 2 :
                                    (rect.height + font_ascent_ - font_descent_) / 2);
        if (widget.kind == GuiWidget::Kind::Label) {
            foreground(connection, gc_, static_cast<uint32_t>(foreground_));
            draw_text(connection, window_, gc_, rect.x, text_y, font_width_, texts_[i]);
            continue;
        }
        const xcb_rectangle_t fill{static_cast<int16_t>(rect.x), static_cast<int16_t>(rect.y),
                                   static_cast<uint16_t>(rect.width), static_cast<uint16_t>(rect.height)};
        const xcb_rectangle_t outline{static_cast<int16_t>(rect.x), static_cast<int16_t>(rect.y),
                                      static_cast<uint16_t>(rect.width - 1), static_cast<uint16_t>(rect.height - 1)};
        foreground(connection, gc_, static_cast<uint32_t>(widget.kind == GuiWidget::Kind::Input ? inner_ : control_));
        xcb_poly_fill_rectangle(connection, window_, gc_, 1, &fill);
        foreground(connection, gc_, static_cast<uint32_t>(widget.kind == GuiWidget::Kind::Input && static_cast<int>(i) == focused_input_ ? accent_ : border_));
        xcb_poly_rectangle(connection, window_, gc_, 1, &outline);
        foreground(connection, gc_, static_cast<uint32_t>(widget.kind == GuiWidget::Kind::Input ? foreground_ : control_text_));
        const std::string text = widget.kind == GuiWidget::Kind::Input ? clipped_text(texts_[i], rect.width - 12) : texts_[i];
        const int text_x = widget.kind == GuiWidget::Kind::Input ? rect.x + 6 :
                           rect.x + std::max(0, (rect.width - font_width_ * static_cast<int>(text.size())) / 2);
        draw_text(connection, window_, gc_, text_x, text_y, font_width_, text);
    }
    xcb_flush(connection);
}

void GuiHost::redraw_if_open_locked() { if (!closed_) draw_locked(); }

void GuiHost::write_debug_rects() const {
    const char* path = std::getenv("KEYLOOM_GUI_DEBUG_RECTS");
    if (path == nullptr || *path == '\0') return;
    FILE* file = std::fopen(path, "w");
    if (file == nullptr) return;
    for (size_t i = 0; i < panel_.widgets.size(); ++i) {
        const auto& widget = panel_.widgets[i];
        const auto& rect = rects_[i];
        const char* kind = widget.kind == GuiWidget::Kind::Label ? "label" : widget.kind == GuiWidget::Kind::Input ? "input" : "button";
        std::fprintf(file, "%s %s %d %d %d %d\n", kind, widget.id.c_str(), rect.x, rect.y, rect.width, rect.height);
    }
    std::fclose(file);
}

void GuiHost::process_events() {
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connection_ || closed_) return;
        auto* connection = connection_->get();
        bool dirty = false;
        while (auto* raw = xcb_poll_for_event(connection)) {
            XcbReply<xcb_generic_event_t> event(raw, &std::free);
            const uint8_t type = event->response_type & 0x7f;
            if (type == XCB_MAPPING_NOTIFY) {
                const auto* mapping = reinterpret_cast<xcb_mapping_notify_event_t*>(event.get());
                if (mapping->request == XCB_MAPPING_KEYBOARD || mapping->request == XCB_MAPPING_MODIFIER) {
                    refresh_keyboard_mapping();
                }
                continue;
            }
            if (type == XCB_EXPOSE) { dirty = true; continue; }
            if (type == XCB_MAP_NOTIFY || type == XCB_UNMAP_NOTIFY) continue;
            if (type == XCB_DESTROY_NOTIFY && reinterpret_cast<xcb_destroy_notify_event_t*>(event.get())->window == window_) {
                window_ = 0; closed_ = true;
                events.push_back({EventKind::Close, panel_.title, "window"});
                break;
            }
            if (type == XCB_BUTTON_PRESS) {
                const auto* button = reinterpret_cast<xcb_button_press_event_t*>(event.get());
                if (button->detail != 1) continue;
                const int index = widget_index_at(button->event_x, button->event_y);
                if (index < 0) focus_input(-1);
                else if (panel_.widgets[static_cast<size_t>(index)].kind == GuiWidget::Kind::Input) focus_input(index);
                else if (panel_.widgets[static_cast<size_t>(index)].kind == GuiWidget::Kind::Button) {
                    focus_input(-1);
                    events.push_back({EventKind::Click, panel_.title, panel_.widgets[static_cast<size_t>(index)].id});
                }
                dirty = true;
                continue;
            }
            if (type == XCB_KEY_PRESS && focused_input_ >= 0) {
                const auto* key = reinterpret_cast<xcb_key_press_event_t*>(event.get());
                const int base = (static_cast<int>(key->detail) - min_keycode_) * keysyms_per_keycode_;
                if (base < 0 || static_cast<size_t>(base) >= keysyms_.size()) continue;
                xcb_keysym_t sym = keysyms_[static_cast<size_t>(base)];
                const bool shift = (key->state & XCB_MOD_MASK_SHIFT) != 0;
                const bool caps = (key->state & XCB_MOD_MASK_LOCK) != 0;
                if (shift && keysyms_per_keycode_ > 1 && static_cast<size_t>(base + 1) < keysyms_.size() && keysyms_[static_cast<size_t>(base + 1)] != 0)
                    sym = keysyms_[static_cast<size_t>(base + 1)];
                if (caps && sym >= 'a' && sym <= 'z') sym = sym - 'a' + 'A';
                else if (caps && shift && sym >= 'A' && sym <= 'Z') sym = sym - 'A' + 'a';
                auto& text = texts_.at(static_cast<size_t>(focused_input_));
                if (sym == XK_BackSpace) { if (!text.empty()) text.pop_back(); }
                else if (sym == XK_Delete) text.clear();
                else if (sym == XK_Tab) {
                    size_t next = static_cast<size_t>(focused_input_) + 1;
                    while (next % panel_.widgets.size() != static_cast<size_t>(focused_input_)) {
                        next %= panel_.widgets.size();
                        if (panel_.widgets[next].kind == GuiWidget::Kind::Input) break;
                        ++next;
                    }
                    focus_input(static_cast<int>(next % panel_.widgets.size()));
                } else if (sym >= 32 && sym <= 126 && !(key->state & XCB_MOD_MASK_CONTROL)) text.push_back(static_cast<char>(sym));
                dirty = true;
                continue;
            }
            if (type == XCB_CLIENT_MESSAGE) {
                const auto* message = reinterpret_cast<xcb_client_message_event_t*>(event.get());
                if (message->type == wm_protocols_ && message->data.data32[0] == wm_delete_window_) {
                    xcb_destroy_window(connection, window_);
                    xcb_flush(connection);
                    window_ = 0; closed_ = true;
                    events.push_back({EventKind::Close, panel_.title, "window"});
                    break;
                }
            }
        }
        if (!closed_ && connection_lost_locked()) {
            connection_->close();
            window_ = 0; closed_ = true;
            events.push_back({EventKind::Close, panel_.title, "window"});
        }
        if (dirty) draw_locked();
    }
    if (event_handler_) for (const auto& event : events) event_handler_(event);
}

bool GuiHost::connection_lost_locked() const { return !connection_ || connection_->error() != 0; }

void GuiHost::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || !connection_ || window_ == 0) return;
    if (!connection_lost_locked()) {
        xcb_destroy_window(connection_->get(), window_);
        xcb_flush(connection_->get());
    }
    window_ = 0; closed_ = true;
}

bool GuiHost::has_widget(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return widget_index_by_id(id) >= 0;
}

std::string GuiHost::value(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = widget_index_by_id(id);
    if (index < 0) throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(id) + " is not declared in a gui block.");
    if (panel_.widgets[static_cast<size_t>(index)].kind != GuiWidget::Kind::Input)
        throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(id) + " is not an input.");
    return texts_[static_cast<size_t>(index)];
}

void GuiHost::set_value(const std::string& id, const std::string& value) { set_text(id, value); }

void GuiHost::set_text(const std::string& id, const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = widget_index_by_id(id);
    if (index < 0) throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(id) + " is not declared in a gui block.");
    if (panel_.widgets[static_cast<size_t>(index)].kind == GuiWidget::Kind::Label)
        throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(id) + " is a label and has no variable text target.");
    texts_[static_cast<size_t>(index)] = text;
    if (!closed_) draw_locked();
}

void GuiHost::set_visible(bool visible) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connection_ || closed_ || connection_lost_locked()) return;
    if (visible) xcb_map_window(connection_->get(), window_);
    else xcb_unmap_window(connection_->get(), window_);
    xcb_flush(connection_->get());
}

}  // namespace keyloom
