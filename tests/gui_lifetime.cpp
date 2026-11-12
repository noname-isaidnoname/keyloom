#include "gui.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unistd.h>

namespace {

int unrelated_errors = 0;
int record_other_error(Display*, XErrorEvent*) {
    ++unrelated_errors;
    return 0;
}

Window find_window(Display* display, const char* title) {
    Window root = DefaultRootWindow(display);
    Window parent = 0;
    Window* children = nullptr;
    unsigned int count = 0;
    if (!XQueryTree(display, root, &root, &parent, &children, &count)) return 0;
    Window found = 0;
    for (unsigned int i = 0; i < count; ++i) {
        char* name = nullptr;
        if (XFetchName(display, children[i], &name) && name != nullptr) {
            if (std::strcmp(name, title) == 0) found = children[i];
            XFree(name);
        }
        if (found != 0) break;
    }
    if (children != nullptr) XFree(children);
    return found;
}

}  // namespace

int main() {
    XSetErrorHandler(record_other_error);
    std::unique_ptr<loomwright::GuiHost> host;
    {
        loomwright::GuiPanel panel;
        panel.title = "Owned panel";
        panel.widgets.push_back({loomwright::GuiWidget::Kind::Input, "name", "original", 1});
        host = loomwright::GuiHost::create(panel);
        panel.title = "changed";
        panel.widgets.clear();
        if (host->title() != "Owned panel" || !host->has_widget("name")) {
            std::fprintf(stderr, "GUI retained mutable caller-owned panel\n");
            return 1;
        }
    }
    if (host->value("name") != "original") {
        std::fprintf(stderr, "GUI lost panel after its creator returned\n");
        return 1;
    }
    host->set_text("name", "updated");
    if (host->value("name") != "updated") return 1;
    std::puts("ok   GUI owns its panel after caller destruction");

    int closes = 0;
    host->set_event_handler([&](const loomwright::GuiHost::Event& event) {
        if (event.kind == loomwright::GuiHost::EventKind::Close) ++closes;
    });
    Display* other = XOpenDisplay(nullptr);
    if (other == nullptr) return 1;
    const Window window = find_window(other, "Owned panel");
    if (window == 0) return 1;
    bool rendered = false;
    for (int i = 0; i < 50 && !rendered; ++i) {
        host->process_events();
        XImage* image = XGetImage(other, window, 0, 0, 50, 50, AllPlanes, ZPixmap);
        if (image == nullptr) return 1;
        rendered = XGetPixel(image, 1, 1) != XGetPixel(image, 16, 16);
        if (i == 49 && !rendered) std::fprintf(stderr, "background=%lu border=%lu inner=%lu\n",
            XGetPixel(image, 1, 1), XGetPixel(image, 16, 16), XGetPixel(image, 18, 18));
        XDestroyImage(image);
        if (!rendered) ::usleep(10000);
    }
    if (!rendered) {
        std::fprintf(stderr, "GUI input border was not rendered\n");
        return 1;
    }
    std::puts("ok   GUI renders input border");
    XEvent click{};
    click.xbutton.type = ButtonPress;
    click.xbutton.window = window;
    click.xbutton.button = Button1;
    click.xbutton.x = 20;
    click.xbutton.y = 20;
    XSendEvent(other, window, False, ButtonPressMask, &click);
    XEvent key{};
    key.xkey.type = KeyPress;
    key.xkey.window = window;
    key.xkey.keycode = XKeysymToKeycode(other, XK_a);
    key.xkey.state = ShiftMask;
    XSendEvent(other, window, False, KeyPressMask, &key);
    XSync(other, False);
    for (int i = 0; i < 50 && host->value("name") != "updatedA"; ++i) {
        host->process_events();
        ::usleep(1000);
    }
    if (host->value("name") != "updatedA") {
        std::fprintf(stderr, "GUI shifted keyboard input failed\n");
        return 1;
    }
    std::puts("ok   GUI handles shifted keyboard input");
    XDestroyWindow(other, static_cast<Window>(0x7fffffff));
    XSync(other, False);
    if (unrelated_errors != 1) {
        std::fprintf(stderr, "unrelated X error was not forwarded\n");
        return 1;
    }
    XDestroyWindow(other, window);
    XSync(other, False);
    XCloseDisplay(other);

    // The server has destroyed the window, but GuiHost has not yet consumed
    // DestroyNotify. These requests must not terminate the process.
    host->set_text("name", "after destruction");
    host->set_visible(false);
    host->set_visible(true);
    for (int i = 0; i < 100 && !host->is_closed(); ++i) {
        host->process_events();
        if (!host->is_closed()) ::usleep(1000);
    }
    if (!host->is_closed() || closes != 1) {
        std::fprintf(stderr, "external destroy did not dispatch exactly one close\n");
        return 1;
    }
    host->close();
    host.reset(); // Synchronizes pending X errors before destroying the display.
    std::puts("ok   GUI operations tolerate an already-destroyed window");
    return 0;
}
