// Test helper: deliver one synthetic X event to a window.
//
// Xvfb and Xwayland differ in whether xdotool's XTEST clicks reach a rootless
// Xwayland window, so the picker integration test sends the event directly.
// The picker only reads the event fields, so this is the same code path a real
// click takes once the event arrives.
//
//   picker_event WINDOW_ID button BUTTON X Y
//   picker_event WINDOW_ID key KEYSYM
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void usage() {
    std::fprintf(stderr, "usage: picker_event WINDOW_ID button BUTTON X Y\n"
                         "       picker_event WINDOW_ID key KEYSYM\n"
                         "       picker_event WINDOW_ID pixel X Y\n"
                         "       picker_event WINDOW_ID close|destroy\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::fprintf(stderr, "picker_event: cannot open display\n");
        return 2;
    }
    const Window window = static_cast<Window>(std::strtoul(argv[1], nullptr, 0));
    if (window == 0) {
        std::fprintf(stderr, "picker_event: invalid window id\n");
        XCloseDisplay(display);
        return 2;
    }

    XEvent event{};
    bool send = false;
    const std::string kind = argv[2];
    if (kind == "pixel" && argc == 5) {
        XImage* image = XGetImage(display, DefaultRootWindow(display), std::atoi(argv[3]), std::atoi(argv[4]), 1, 1, AllPlanes, ZPixmap);
        if (image != nullptr) {
            std::printf("%06lx\n", XGetPixel(image, 0, 0) & 0xffffffUL);
            XDestroyImage(image);
            send = true;
        }
    } else if (kind == "button" && argc == 6) {
        const int x = std::atoi(argv[4]);
        const int y = std::atoi(argv[5]);
        event.xbutton.type = ButtonPress;
        event.xbutton.window = window;
        event.xbutton.root = DefaultRootWindow(display);
        event.xbutton.time = CurrentTime;
        event.xbutton.x = x;
        event.xbutton.y = y;
        event.xbutton.x_root = x;
        event.xbutton.y_root = y;
        event.xbutton.state = 0;
        event.xbutton.button = static_cast<unsigned>(std::atoi(argv[3]));
        event.xbutton.same_screen = True;
        XSendEvent(display, window, False, ButtonPressMask, &event);
        send = true;
    } else if (kind == "key" && argc == 4) {
        const KeySym symbol = XStringToKeysym(argv[3]);
        const KeyCode code = symbol == NoSymbol ? 0 : XKeysymToKeycode(display, symbol);
        if (code == 0) {
            std::fprintf(stderr, "picker_event: unknown keysym %s\n", argv[3]);
        } else {
            event.xkey.type = KeyPress;
            event.xkey.window = window;
            event.xkey.root = DefaultRootWindow(display);
            event.xkey.time = CurrentTime;
            event.xkey.state = 0;
            event.xkey.keycode = code;
            event.xkey.same_screen = True;
            XSendEvent(display, window, False, KeyPressMask, &event);
            send = true;
        }
    } else if (kind == "close" && argc == 3) {
        event.xclient.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = XInternAtom(display, "WM_PROTOCOLS", False);
        event.xclient.format = 32;
        event.xclient.data.l[0] = static_cast<long>(XInternAtom(display, "WM_DELETE_WINDOW", False));
        event.xclient.data.l[1] = CurrentTime;
        XSendEvent(display, window, False, NoEventMask, &event);
        send = true;
    } else if (kind == "destroy" && argc == 3) {
        XDestroyWindow(display, window);
        send = true;
    } else {
        usage();
    }

    XFlush(display);
    XCloseDisplay(display);
    return send ? 0 : 2;
}
