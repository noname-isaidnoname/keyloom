#include "mousepos.hpp"

#include <X11/keysym.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <png.h>
#include <poll.h>
#include <unistd.h>
#include <vector>

#include "xcb_connection.hpp"
#include "picker_image.hpp"
#include "picker_xcb.hpp"

namespace keyloom {
namespace {

template <typename T> using Reply = std::unique_ptr<T, decltype(&std::free)>;
volatile sig_atomic_t cancelled_signal = 0;
void picker_signal(int) { cancelled_signal = 1; }

struct SignalGuard {
    struct sigaction old_int {};
    struct sigaction old_term {};
    bool have_int = false;
    bool have_term = false;
    SignalGuard() {
        struct sigaction action {};
        action.sa_handler = picker_signal;
        sigemptyset(&action.sa_mask);
        cancelled_signal = 0;
        if (sigaction(SIGINT, &action, &old_int) < 0) throw PickerError("Could not install picker SIGINT handler.");
        have_int = true;
        if (sigaction(SIGTERM, &action, &old_term) < 0) {
            sigaction(SIGINT, &old_int, nullptr);
            have_int = false;
            throw PickerError("Could not install picker SIGTERM handler.");
        }
        have_term = true;
    }
    ~SignalGuard() {
        if (have_term) sigaction(SIGTERM, &old_term, nullptr);
        if (have_int) sigaction(SIGINT, &old_int, nullptr);
    }
};

struct ScreenshotGuard {
    std::string path;
    ~ScreenshotGuard() { if (!path.empty()) ::unlink(path.c_str()); }
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
};

Image load_png(const std::string& path) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path.c_str()))
        throw PickerError("Could not read the desktop screenshot: " + std::string(image.message));
    image.format = PNG_FORMAT_RGBA;
    Image result;
    result.width = static_cast<int>(image.width);
    result.height = static_cast<int>(image.height);
    result.rgba.resize(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, result.rgba.data(), 0, nullptr)) {
        const std::string message = image.message;
        png_image_free(&image);
        throw PickerError("Could not decode the desktop screenshot: " + message);
    }
    png_image_free(&image);
    return result;
}

xcb_atom_t atom(xcb_connection_t* connection, const char* name) {
    const auto cookie = xcb_intern_atom(connection, 0, static_cast<uint16_t>(std::strlen(name)), name);
    Reply<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(connection, cookie, nullptr), &std::free);
    if (!reply) throw PickerError("The X11 connection closed while preparing the mouse position picker.");
    return reply->atom;
}

struct Picker {
    XcbConnection connection;
    xcb_screen_t* screen = nullptr;
    xcb_window_t overlay = 0;
    xcb_window_t tooltip = 0;
    xcb_gcontext_t gc = 0;
    xcb_pixmap_t pixmap = 0;
    xcb_font_t font = 0;
    int font_width = 8;
    int ascent = 12;
    int descent = 4;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<uint8_t> pixels;
    std::vector<xcb_keycode_t> escape_codes;
    std::string text;
    bool focused = false;
    bool cancelled = false;
    std::optional<std::pair<int, int>> result;

    Picker() {
        auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection.get()));
        for (int i = 0; i < connection.screen_number() && screens.rem; ++i) xcb_screen_next(&screens);
        if (!screens.rem) throw PickerError("Could not find an X11 screen for the mouse position picker.");
        screen = screens.data;
        width = std::max(1, static_cast<int>(screen->width_in_pixels));
        height = std::max(1, static_cast<int>(screen->height_in_pixels));
    }
    ~Picker() {
        if (!connection.get()) return;
        if (!connection.error()) {
            if (tooltip) xcb_destroy_window(connection.get(), tooltip);
            if (overlay) xcb_destroy_window(connection.get(), overlay);
            if (pixmap) xcb_free_pixmap(connection.get(), pixmap);
            if (gc) xcb_free_gc(connection.get(), gc);
            if (font) xcb_close_font(connection.get(), font);
            xcb_flush(connection.get());
        }
    }

    void check() const {
        if (connection.error()) throw PickerError("The X11 connection closed while the mouse position picker was open.");
    }

    void prepare_pixels(const Image& image) {
        const xcb_setup_t* setup = xcb_get_setup(connection.get());
        const xcb_visualtype_t* visual = nullptr;
        for (auto depths = xcb_screen_allowed_depths_iterator(screen); depths.rem; xcb_depth_next(&depths)) {
            for (auto visuals = xcb_depth_visuals_iterator(depths.data); visuals.rem; xcb_visualtype_next(&visuals)) {
                if (visuals.data->visual_id == screen->root_visual) visual = visuals.data;
            }
        }
        int bits = 0, padding = 0;
        for (auto formats = xcb_setup_pixmap_formats_iterator(setup); formats.rem; xcb_format_next(&formats)) {
            if (formats.data->depth == screen->root_depth) {
                bits = formats.data->bits_per_pixel;
                padding = formats.data->scanline_pad;
                break;
            }
        }
        if (!visual || visual->_class != XCB_VISUAL_CLASS_TRUE_COLOR)
            throw PickerError("Unsupported X11 screen format for the mouse position picker.");
        auto converted = convert_picker_pixels(image.rgba, image.width, image.height, width, height,
            PickerPixelFormat{bits, padding, setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST,
                              visual->red_mask, visual->green_mask, visual->blue_mask});
        stride = converted.stride;
        pixels = std::move(converted.bytes);
    }

    void prepare() {
        auto* c = connection.get();
        gc = xcb_generate_id(c);
        xcb_create_gc(c, gc, screen->root, 0, nullptr);
        overlay = xcb_generate_id(c);
        const uint32_t overlay_values[] = {screen->black_pixel, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
                                            XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
        xcb_create_window(c, XCB_COPY_FROM_PARENT, overlay, screen->root, 0, 0, static_cast<uint16_t>(width),
                          static_cast<uint16_t>(height), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, overlay_values);
        tooltip = xcb_generate_id(c);
        const uint32_t tip_values[] = {0x111111, 1, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_EXPOSURE};
        xcb_create_window(c, XCB_COPY_FROM_PARENT, tooltip, screen->root, 0, 0, 120, 40, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, tip_values);
        const auto type = atom(c, "_NET_WM_WINDOW_TYPE");
        const auto splash = atom(c, "_NET_WM_WINDOW_TYPE_SPLASH");
        const auto state = atom(c, "_NET_WM_STATE");
        const xcb_atom_t states[] = {atom(c, "_NET_WM_STATE_ABOVE"), atom(c, "_NET_WM_STATE_FULLSCREEN")};
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, overlay, type, XCB_ATOM_ATOM, 32, 1, &splash);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, overlay, state, XCB_ATOM_ATOM, 32, 2, states);
        constexpr char title[] = "Keyloom mouse position";
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, overlay, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, sizeof(title) - 1, title);

        font = xcb_generate_id(c);
        auto open = xcb_open_font_checked(c, font, 5, "fixed");
        Reply<xcb_generic_error_t> error(xcb_request_check(c, open), &std::free);
        if (!error) {
            Reply<xcb_query_font_reply_t> info(xcb_query_font_reply(c, xcb_query_font(c, font), nullptr), &std::free);
            if (info) {
                font_width = info->max_bounds.character_width;
                ascent = info->font_ascent;
                descent = info->font_descent;
                xcb_change_gc(c, gc, XCB_GC_FONT, &font);
            }
        } else font = 0;
        check();

        const auto* setup = xcb_get_setup(c);
        const auto count = static_cast<uint8_t>(setup->max_keycode - setup->min_keycode + 1);
        Reply<xcb_get_keyboard_mapping_reply_t> mapping(
            xcb_get_keyboard_mapping_reply(c, xcb_get_keyboard_mapping(c, setup->min_keycode, count), nullptr), &std::free);
        if (!mapping) throw PickerError("Could not read the picker keyboard mapping.");
        const auto* syms = xcb_get_keyboard_mapping_keysyms(mapping.get());
        for (int i = 0; i < count; ++i) {
            for (int j = 0; j < mapping->keysyms_per_keycode; ++j) {
                if (syms[i * mapping->keysyms_per_keycode + j] == XK_Escape) {
                    escape_codes.push_back(static_cast<xcb_keycode_t>(setup->min_keycode + i));
                    break;
                }
            }
        }
        text = format_coordinates(width / 2, height / 2);
    }

    void upload_screenshot(const std::function<void()>& upload_hook) {
        auto* c = connection.get();
        pixmap = xcb_generate_id(c);
        auto create = xcb_create_pixmap_checked(c, screen->root_depth, pixmap, screen->root,
                                                 static_cast<uint16_t>(width), static_cast<uint16_t>(height));
        Reply<xcb_generic_error_t> create_error(xcb_request_check(c, create), &std::free);
        check();
        if (create_error) throw PickerError("Could not allocate the picker screenshot pixmap.");
        const uint64_t max_bytes = static_cast<uint64_t>(xcb_get_maximum_request_length(connection.get())) * 4;
        if (stride <= 0 || max_bytes <= 64 || static_cast<uint64_t>(stride) > max_bytes - 64)
            throw PickerError("X11 request size is too small for the picker screenshot.");
        const uint64_t payload = std::min(max_bytes - 64, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()));
        const int rows = static_cast<int>(std::min(static_cast<uint64_t>(height), payload / static_cast<uint64_t>(stride)));
        bool hook_called = false;
        for (int y = 0; y < height; y += rows) {
            if (cancelled_signal) return;
            const int count = std::min(rows, height - y);
            const auto request = xcb_put_image_checked(c, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, gc, static_cast<uint16_t>(width),
                              static_cast<uint16_t>(count), 0, static_cast<int16_t>(y), 0, screen->root_depth,
                              static_cast<uint32_t>(static_cast<uint64_t>(stride) * static_cast<uint64_t>(count)),
                              pixels.data() + static_cast<size_t>(stride) * static_cast<size_t>(y));
            if (upload_hook && !hook_called) {
                hook_called = true;
                upload_hook();
            }
            Reply<xcb_generic_error_t> error(xcb_request_check(c, request), &std::free);
            check();
            if (error) throw PickerError("Could not upload the picker screenshot to X11.");
        }
        pixels.clear();
    }

    void draw_overlay() {
        xcb_copy_area(connection.get(), pixmap, overlay, gc, 0, 0, 0, 0,
                      static_cast<uint16_t>(width), static_cast<uint16_t>(height));
    }

    void draw_tooltip(int x, int y) {
        auto* c = connection.get();
        const int tip_width = font_width * static_cast<int>(text.size()) + 24;
        const int tip_height = ascent + descent + 14;
        const auto position = tooltip_position(x, y, width, height, tip_width, tip_height);
        const uint32_t geometry[] = {static_cast<uint32_t>(position.first), static_cast<uint32_t>(position.second),
                                     static_cast<uint32_t>(tip_width), static_cast<uint32_t>(tip_height), XCB_STACK_MODE_ABOVE};
        xcb_configure_window(c, tooltip, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                             XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_STACK_MODE, geometry);
        uint32_t foreground = 0x111111;
        xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &foreground);
        const xcb_rectangle_t fill{0, 0, static_cast<uint16_t>(tip_width), static_cast<uint16_t>(tip_height)};
        xcb_poly_fill_rectangle(c, tooltip, gc, 1, &fill);
        foreground = 0xf5f5f5;
        xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &foreground);
        const xcb_rectangle_t border{0, 0, static_cast<uint16_t>(tip_width - 1), static_cast<uint16_t>(tip_height - 1)};
        xcb_poly_rectangle(c, tooltip, gc, 1, &border);
        const uint8_t length = static_cast<uint8_t>(std::min<size_t>(text.size(), 254));
        std::vector<uint8_t> item{length, 0};
        item.insert(item.end(), text.begin(), text.begin() + length);
        xcb_poly_text_8(c, tooltip, gc, 12, static_cast<int16_t>(7 + ascent), static_cast<uint32_t>(item.size()), item.data());
    }

    bool viewable() {
        auto* c = connection.get();
        Reply<xcb_get_window_attributes_reply_t> attributes(
            xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, overlay), nullptr), &std::free);
        check();
        return attributes && attributes->map_state == XCB_MAP_STATE_VIEWABLE;
    }

    void run() {
        auto* c = connection.get();
        xcb_map_window(c, overlay);
        xcb_map_window(c, tooltip);
        draw_overlay();
        draw_tooltip(width / 2, height / 2);
        xcb_flush(c);
        check();
        bool done = false;
        const auto started = std::chrono::steady_clock::now();
        while (!done) {
            check();
            if (!focused && viewable()) {
                xcb_set_input_focus(c, XCB_INPUT_FOCUS_PARENT, overlay, XCB_CURRENT_TIME);
                xcb_flush(c);
                focused = true;
            }
            while (auto* raw = xcb_poll_for_event(c)) {
                Reply<xcb_generic_event_t> event(raw, &std::free);
                const uint8_t type = event->response_type & 0x7f;
                if (type == XCB_EXPOSE && reinterpret_cast<xcb_expose_event_t*>(event.get())->window == overlay) draw_overlay();
                else if (type == XCB_MAP_NOTIFY && reinterpret_cast<xcb_map_notify_event_t*>(event.get())->window == overlay) draw_overlay();
                else if (type == XCB_BUTTON_PRESS) {
                    const auto* button = reinterpret_cast<xcb_button_press_event_t*>(event.get());
                    if (button->detail == 1) result = clamp_to_screen(button->root_x, button->root_y, width, height);
                    else cancelled = true;
                    done = true;
                } else if (type == XCB_KEY_PRESS) {
                    const auto code = reinterpret_cast<xcb_key_press_event_t*>(event.get())->detail;
                    if (std::find(escape_codes.begin(), escape_codes.end(), code) != escape_codes.end()) { cancelled = true; done = true; }
                } else if (type == XCB_DESTROY_NOTIFY && reinterpret_cast<xcb_destroy_notify_event_t*>(event.get())->window == overlay) {
                    overlay = 0; cancelled = true; done = true;
                }
                if (done) break;
            }
            check();
            if (done) break;
            if (cancelled_signal) { cancelled = true; break; }
            Reply<xcb_query_pointer_reply_t> pointer(xcb_query_pointer_reply(c, xcb_query_pointer(c, overlay), nullptr), &std::free);
            check();
            if (pointer) {
                const auto point = clamp_to_screen(pointer->root_x, pointer->root_y, width, height);
                const auto next = format_coordinates(point.first, point.second);
                if (next != text) { text = next; draw_tooltip(point.first, point.second); xcb_flush(c); }
            }
            pollfd entry{connection.fd(), POLLIN, 0};
            ::poll(&entry, 1, kPollMs);
            // poll() may report a hangup before XCB has consumed EOF.
            if (entry.revents & (POLLHUP | POLLERR | POLLNVAL))
                throw PickerError("The X11 connection closed while the mouse position picker was open.");
            if (!viewable() && std::chrono::steady_clock::now() - started > std::chrono::seconds(kOverlayTimeoutMs / 1000))
                throw PickerError("The window manager never showed the mouse position picker overlay. Check that a window manager is running on this display, then try again.");
        }
    }
};

}  // namespace

std::optional<std::pair<int, int>> pick_mouse_position_with_upload_hook(
    bool verbose, const std::function<void(const std::string&)>& upload_hook) {
    std::unique_ptr<Picker> picker;
    try { picker = std::make_unique<Picker>(); }
    catch (const std::exception&) {
        throw PickerError("Cannot open a display for the mouse position picker. On Wayland, run it in a session with XWayland and DISPLAY set, or use keyloom --screen to calibrate coordinates manually.");
    }
    ScreenshotGuard screenshot;
    const CaptureResult capture = capture_desktop();
    screenshot.path = capture.path;
    if (verbose) std::cerr << "[keyloom] picker screenshot: " << capture.tool << "; screen bounds: " << picker->width << "x" << picker->height << "\n";
    const Image image = load_png(capture.path);
    SignalGuard signals;
    picker->check();
    picker->prepare_pixels(image);
    if (cancelled_signal) return std::nullopt;
    picker->prepare();
    if (cancelled_signal) return std::nullopt;
    picker->upload_screenshot(upload_hook ? std::function<void()>([&] { upload_hook(screenshot.path); }) : std::function<void()>());
    if (cancelled_signal) return std::nullopt;
    picker->run();
    return picker->cancelled ? std::nullopt : picker->result;
}

std::optional<std::pair<int, int>> pick_mouse_position_xcb(bool verbose) {
    return pick_mouse_position_with_upload_hook(verbose, {});
}

}  // namespace keyloom
