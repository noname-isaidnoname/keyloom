#include "mousepos.hpp"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <png.h>
#include <poll.h>
#include <csignal>
#include <unistd.h>

#include "errors.hpp"
#include "subprocess.hpp"

namespace keyloom {
namespace {

// Preference order: Spectacle covers KDE, grim covers wlroots, the rest are X11
// and GNOME fallbacks.
const char* const kScreenshotTools[] = {"spectacle", "grim", "gnome-screenshot", "scrot", "import"};

volatile sig_atomic_t g_cancel_requested = 0;

void handle_cancel_signal(int) { g_cancel_requested = 1; }

// Xlib's default error handler prints "X Error of failed request" and calls
// exit(1), which turns a harmless protocol race into a crash. The picker only
// has one such race: XSetInputFocus against a window the window manager has
// not mapped yet (BadMatch). Losing that race must not kill the picker; at
// worst Esc does not cancel and right-click still does. Record the error and
// keep going.
int g_last_x_error_code = 0;
unsigned long g_last_x_error_request = 0;

int record_x_error(Display*, XErrorEvent* event) {
    g_last_x_error_code = event->error_code;
    g_last_x_error_request = event->request_code;
    return 0;
}

struct XErrorTrap {
    XErrorHandler previous = nullptr;

    XErrorTrap() {
        g_last_x_error_code = 0;
        g_last_x_error_request = 0;
        previous = XSetErrorHandler(record_x_error);
    }
    ~XErrorTrap() {
        if (previous != nullptr) XSetErrorHandler(previous);
    }
    XErrorTrap(const XErrorTrap&) = delete;
    XErrorTrap& operator=(const XErrorTrap&) = delete;
};

struct ScreenshotGuard {
    std::string path;
    ~ScreenshotGuard() {
        if (!path.empty()) ::unlink(path.c_str());
    }
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
};

Image load_png(const std::string& path) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_file(&image, path.c_str()) == 0) {
        throw PickerError("Could not read the desktop screenshot: " + std::string(image.message));
    }
    image.format = PNG_FORMAT_RGBA;
    Image result;
    result.width = static_cast<int>(image.width);
    result.height = static_cast<int>(image.height);
    result.rgba.resize(PNG_IMAGE_SIZE(image));
    if (png_image_finish_read(&image, nullptr, result.rgba.data(), 0, nullptr) == 0) {
        png_image_free(&image);
        throw PickerError("Could not decode the desktop screenshot: " + std::string(image.message));
    }
    return result;
}

int shift_for(unsigned long mask) {
    int shift = 0;
    while (mask != 0 && (mask & 1UL) == 0) {
        mask >>= 1;
        ++shift;
    }
    return shift;
}

/// Nearest-neighbour resample to the X screen size, converted to TrueColor pixels.
std::vector<unsigned long> to_x_pixels(const Image& image, int width, int height, Visual* visual) {
    std::vector<unsigned long> pixels(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    if (image.width <= 0 || image.height <= 0) return pixels;

    const int red_shift = shift_for(visual->red_mask);
    const int green_shift = shift_for(visual->green_mask);
    const int blue_shift = shift_for(visual->blue_mask);

    for (int y = 0; y < height; ++y) {
        const int source_y = std::min(image.height - 1, y * image.height / height);
        for (int x = 0; x < width; ++x) {
            const int source_x = std::min(image.width - 1, x * image.width / width);
            const size_t offset = (static_cast<size_t>(source_y) * static_cast<size_t>(image.width) +
                                   static_cast<size_t>(source_x)) *
                                  4;
            const unsigned long red = image.rgba[offset];
            const unsigned long green = image.rgba[offset + 1];
            const unsigned long blue = image.rgba[offset + 2];
            pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
                ((red << red_shift) & visual->red_mask) | ((green << green_shift) & visual->green_mask) |
                ((blue << blue_shift) & visual->blue_mask);
        }
    }
    return pixels;
}

/// EWMH properties are ATOM lists, not strings. Writing them as UTF8_STRING is
/// silently ignored by the window manager, which leaves the splash decorated
/// and short of the screen edge (clicks on the title bar never reach the picker).
void set_atom_property(Display* display, Window window, const char* name, const std::vector<Atom>& values) {
    const Atom property = XInternAtom(display, name, False);
    XChangeProperty(display, window, property, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(values.data()), static_cast<int>(values.size()));
}

struct Picker {
    Display* display = nullptr;
    int screen = 0;
    int width = 0;
    int height = 0;
    Window overlay = 0;
    Window tooltip = 0;
    GC gc = nullptr;
    XFontStruct* font = nullptr;
    XImage* image = nullptr;
    std::vector<unsigned long> pixels;
    std::string text;
    std::optional<std::pair<int, int>> result;
    bool cancelled = false;
    bool focused = false;
    int tooltip_width = 0;
    int tooltip_height = 0;

    ~Picker() { teardown(); }

    void teardown() {
        if (display == nullptr) return;
        if (image != nullptr) {
            // The pixels live in a std::vector, so keep XDestroyImage from
            // free()ing that buffer.
            image->data = nullptr;
            XDestroyImage(image);
            image = nullptr;
        }
        if (font != nullptr) {
            XFreeFont(display, font);
            font = nullptr;
        }
        if (gc != nullptr) {
            XFreeGC(display, gc);
            gc = nullptr;
        }
        if (tooltip != 0) XDestroyWindow(display, tooltip);
        if (overlay != 0) XDestroyWindow(display, overlay);
        XCloseDisplay(display);
        display = nullptr;
    }

    void draw_overlay() {
        if (image == nullptr) return;
        XPutImage(display, overlay, gc, image, 0, 0, 0, 0, static_cast<unsigned>(width),
                  static_cast<unsigned>(height));
    }

    /// Focus only after the window is viewable. XSetInputFocus on an unmapped
    /// window raises BadMatch; a managed splash window is mapped by the window
    /// manager asynchronously, so the first attempt almost always races.
    bool overlay_viewable() const {
        XWindowAttributes attributes{};
        if (XGetWindowAttributes(display, overlay, &attributes) == 0) return false;
        return attributes.map_state == IsViewable;
    }

    void focus_if_viewable() {
        if (focused) return;
        if (!overlay_viewable()) return;
        XSetInputFocus(display, overlay, RevertToParent, CurrentTime);
        focused = true;
        XFlush(display);
    }

    void measure_tooltip() {
        const int text_width = font != nullptr ? XTextWidth(font, text.c_str(), static_cast<int>(text.size()))
                                               : static_cast<int>(text.size()) * 8;
        const int ascent = font != nullptr ? font->ascent : 12;
        const int descent = font != nullptr ? font->descent : 4;
        tooltip_width = text_width + 24;
        tooltip_height = ascent + descent + 14;
    }

    void draw_tooltip(int x, int y) {
        measure_tooltip();
        const auto position = tooltip_position(x, y, width, height, tooltip_width, tooltip_height);
        XMoveResizeWindow(display, tooltip, position.first, position.second, static_cast<unsigned>(tooltip_width),
                          static_cast<unsigned>(tooltip_height));
        XSetForeground(display, gc, 0x111111);
        XFillRectangle(display, tooltip, gc, 0, 0, static_cast<unsigned>(tooltip_width),
                       static_cast<unsigned>(tooltip_height));
        XSetForeground(display, gc, 0xf5f5f5);
        XDrawRectangle(display, tooltip, gc, 0, 0, static_cast<unsigned>(tooltip_width) - 1,
                       static_cast<unsigned>(tooltip_height) - 1);
        const int ascent = font != nullptr ? font->ascent : 12;
        XDrawString(display, tooltip, gc, 12, 7 + ascent, text.c_str(), static_cast<int>(text.size()));
        XRaiseWindow(display, tooltip);
    }

    void run() {
        XSelectInput(display, overlay, ExposureMask | ButtonPressMask | KeyPressMask | StructureNotifyMask);
        XSelectInput(display, tooltip, ButtonPressMask | ExposureMask);
        XMapRaised(display, overlay);
        XMapRaised(display, tooltip);
        draw_overlay();
        draw_tooltip(width / 2, height / 2);
        XFlush(display);

        bool done = false;
        const auto started = std::chrono::steady_clock::now();
        while (!done) {
            focus_if_viewable();
            while (XPending(display) > 0) {
                XEvent event;
                XNextEvent(display, &event);
                switch (event.type) {
                    case MapNotify:
                        if (event.xmap.window == overlay) {
                            draw_overlay();
                            focus_if_viewable();
                        }
                        break;
                    case Expose:
                        if (event.xexpose.window == overlay) draw_overlay();
                        break;
                    case ButtonPress:
                        if (event.xbutton.button == Button1) {
                            result = clamp_to_screen(event.xbutton.x_root, event.xbutton.y_root, width, height);
                        } else {
                            cancelled = true;
                        }
                        done = true;
                        break;
                    case KeyPress: {
                        const KeySym symbol = XLookupKeysym(&event.xkey, 0);
                        if (symbol == XK_Escape) {
                            cancelled = true;
                            done = true;
                        }
                        break;
                    }
                    case DestroyNotify:
                        // The window manager took the overlay away; there is
                        // nothing left to click on.
                        if (event.xdestroywindow.window == overlay) {
                            cancelled = true;
                            done = true;
                        }
                        break;
                    default:
                        break;
                }
            }
            if (done) break;
            if (g_cancel_requested != 0) {
                cancelled = true;
                break;
            }

            Window root = 0;
            Window child = 0;
            int root_x = 0;
            int root_y = 0;
            int window_x = 0;
            int window_y = 0;
            unsigned int mask = 0;
            if (XQueryPointer(display, overlay, &root, &child, &root_x, &root_y, &window_x, &window_y, &mask) !=
                0) {
                const auto clamped = clamp_to_screen(root_x, root_y, width, height);
                const std::string next = format_coordinates(clamped.first, clamped.second);
                if (next != text) {
                    text = next;
                    draw_tooltip(clamped.first, clamped.second);
                    XFlush(display);
                }
            }

            pollfd entry{ConnectionNumber(display), POLLIN, 0};
            ::poll(&entry, 1, kPollMs);

            if (!overlay_viewable() &&
                std::chrono::steady_clock::now() - started > std::chrono::seconds(kOverlayTimeoutMs / 1000)) {
                throw PickerError(
                    "The window manager never showed the mouse position picker overlay. Check that a "
                    "window manager is running on this display, then try again.");
            }
        }
        XFlush(display);
    }
};

}  // namespace

std::string format_coordinates(int x, int y) {
    return std::to_string(x) + ", " + std::to_string(y);
}

std::pair<int, int> clamp_to_screen(int x, int y, int width, int height) {
    const int bounded_width = std::max(1, width);
    const int bounded_height = std::max(1, height);
    return {std::max(0, std::min(bounded_width - 1, x)), std::max(0, std::min(bounded_height - 1, y))};
}

std::pair<int, int> tooltip_position(int x, int y, int screen_width, int screen_height, int tip_width,
                                     int tip_height, int offset) {
    const int bounded_width = std::max(1, screen_width);
    const int bounded_height = std::max(1, screen_height);
    const int bounded_tip_width = std::max(1, tip_width);
    const int bounded_tip_height = std::max(1, tip_height);

    int tip_x = x + offset;
    int tip_y = y + offset;
    if (tip_x + bounded_tip_width > bounded_width) tip_x = x - bounded_tip_width - offset;
    if (tip_y + bounded_tip_height > bounded_height) tip_y = y - bounded_tip_height - offset;
    tip_x = std::max(0, std::min(bounded_width - bounded_tip_width, tip_x));
    tip_y = std::max(0, std::min(bounded_height - bounded_tip_height, tip_y));
    return {tip_x, tip_y};
}

std::vector<std::string> available_screenshot_tools(const ExecutableFinder& finder) {
    const ExecutableFinder lookup = finder ? finder : ExecutableFinder([](const std::string& name) {
        return which(name);
    });
    std::vector<std::string> tools;
    for (const char* name : kScreenshotTools) {
        if (!lookup(name).empty()) tools.emplace_back(name);
    }
    return tools;
}

std::vector<std::vector<std::string>> screenshot_commands(const std::string& path,
                                                          const ExecutableFinder& finder) {
    std::vector<std::vector<std::string>> commands;
    for (const auto& name : available_screenshot_tools(finder)) {
        if (name == "spectacle") {
            commands.push_back({"spectacle", "-b", "-n", "-f", "--scaled", "-o", path});
        } else if (name == "grim") {
            commands.push_back({"grim", path});
        } else if (name == "gnome-screenshot") {
            commands.push_back({"gnome-screenshot", "-f", path});
        } else if (name == "scrot") {
            commands.push_back({"scrot", path});
        } else if (name == "import") {
            commands.push_back({"import", "-window", "root", path});
        }
    }
    return commands;
}

CaptureResult capture_desktop(const ExecutableFinder& finder) {
    char template_path[] = "/tmp/keyloom-picker-XXXXXX";
    const int handle = ::mkstemp(template_path);
    if (handle < 0) throw PickerError(std::string("Could not create a screenshot file: ") + std::strerror(errno));
    ::close(handle);
    const std::string path = std::string(template_path) + ".png";
    ::rename(template_path, path.c_str());

    const auto commands = screenshot_commands(path, finder);
    if (commands.empty()) {
        ::unlink(path.c_str());
        throw PickerError(
            "The mouse position picker needs a screenshot tool so the desktop stays visible while you pick. "
            "Install spectacle (KDE), grim (wlroots), gnome-screenshot, scrot, or ImageMagick's import.");
    }

    std::vector<std::string> failures;
    for (const auto& argv : commands) {
        const CommandResult result = run_command(argv, std::string(), kScreenshotTimeoutMs);
        if (result.spawn_failed || result.timed_out) {
            failures.push_back(argv.front() + ": " + (result.spawn_failed ? result.spawn_error : "timed out"));
            continue;
        }
        if (result.returncode == 0 && ::access(path.c_str(), R_OK) == 0) {
            return CaptureResult{path, argv.front()};
        }
        std::string detail = result.err;
        while (!detail.empty() && (detail.back() == '\n' || detail.back() == ' ')) detail.pop_back();
        failures.push_back(argv.front() + ": " +
                           (detail.empty() ? "exit " + std::to_string(result.returncode) : detail));
    }

    ::unlink(path.c_str());
    std::string detail;
    for (const auto& failure : failures) {
        if (!detail.empty()) detail += "; ";
        detail += failure;
    }
    throw PickerError("Could not capture the desktop for the mouse position picker. " + detail);
}

std::optional<std::pair<int, int>> pick_mouse_position(bool verbose) {
    if (std::getenv("DISPLAY") == nullptr) {
        throw PickerError(
            "Cannot open a display for the mouse position picker. On Wayland, run it in a session with "
            "XWayland and DISPLAY set, or use keyloom --screen to calibrate coordinates manually.");
    }

    XErrorTrap x_errors;
    Picker picker;
    picker.display = XOpenDisplay(nullptr);    if (picker.display == nullptr) {
        throw PickerError(
            "Cannot open a display for the mouse position picker. On Wayland, run it in a session with "
            "XWayland and DISPLAY set, or use keyloom --screen to calibrate coordinates manually.");
    }

    picker.screen = DefaultScreen(picker.display);
    picker.width = std::max(1, DisplayWidth(picker.display, picker.screen));
    picker.height = std::max(1, DisplayHeight(picker.display, picker.screen));

    ScreenshotGuard screenshot;
    const CaptureResult capture = capture_desktop();
    screenshot.path = capture.path;
    if (verbose) {
        std::cerr << "[keyloom] picker screenshot: " << capture.tool << "; screen bounds: " << picker.width << "x"
                  << picker.height << "\n";
    }

    const Image image = load_png(capture.path);
    Visual* visual = DefaultVisual(picker.display, picker.screen);
    picker.pixels = to_x_pixels(image, picker.width, picker.height, visual);

    const Window root = RootWindow(picker.display, picker.screen);
    picker.gc = XCreateGC(picker.display, root, 0, nullptr);

    XSetWindowAttributes overlay_attributes{};
    overlay_attributes.background_pixel = BlackPixel(picker.display, picker.screen);
    overlay_attributes.event_mask = ExposureMask | ButtonPressMask | KeyPressMask;
    picker.overlay = XCreateWindow(picker.display, root, 0, 0, static_cast<unsigned>(picker.width),
                                   static_cast<unsigned>(picker.height), 0,
                                   CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWEventMask,
                                   &overlay_attributes);

    XSetWindowAttributes tooltip_attributes{};
    tooltip_attributes.override_redirect = True;
    tooltip_attributes.background_pixel = 0x111111;
    tooltip_attributes.event_mask = ButtonPressMask | ExposureMask;
    picker.tooltip = XCreateWindow(picker.display, root, 0, 0, 120, 40, 0, CopyFromParent, InputOutput,
                                   CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask,
                                   &tooltip_attributes);

    // A managed splash window still receives input above native Wayland windows;
    // an override-redirect window does not. The EWMH properties must be ATOM
    // lists, and fullscreen keeps the window manager from drawing a title bar
    // over the top strip of the screen.
    set_atom_property(picker.display, picker.overlay, "_NET_WM_WINDOW_TYPE",
                      {XInternAtom(picker.display, "_NET_WM_WINDOW_TYPE_SPLASH", False)});
    set_atom_property(picker.display, picker.overlay, "_NET_WM_STATE",
                      {XInternAtom(picker.display, "_NET_WM_STATE_ABOVE", False),
                       XInternAtom(picker.display, "_NET_WM_STATE_FULLSCREEN", False)});
    const char* title = "Keyloom mouse position";
    XStoreName(picker.display, picker.overlay, title);

    picker.font = XLoadQueryFont(picker.display, "-misc-fixed-bold-r-normal--20-*-100-100-c-0-iso8859-1");
    if (picker.font == nullptr) picker.font = XLoadQueryFont(picker.display, "fixed");
    if (picker.font != nullptr) XSetFont(picker.display, picker.gc, picker.font->fid);

    picker.image = XCreateImage(picker.display, visual, static_cast<unsigned>(DefaultDepth(picker.display, picker.screen)),
                                ZPixmap, 0, reinterpret_cast<char*>(picker.pixels.data()),
                                static_cast<unsigned>(picker.width), static_cast<unsigned>(picker.height), 32, 0);
    if (picker.image == nullptr) throw PickerError("Could not create the overlay image for the mouse position picker.");

    picker.text = format_coordinates(picker.width / 2, picker.height / 2);

    struct sigaction action {};
    action.sa_handler = handle_cancel_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    g_cancel_requested = 0;

    picker.run();

    if (verbose && g_last_x_error_code != 0) {
        std::cerr << "[keyloom] picker: ignored X error " << g_last_x_error_code << " (request "
                  << g_last_x_error_request << ")\n";
    }

    if (picker.cancelled) return std::nullopt;
    return picker.result;
}

}  // namespace keyloom
