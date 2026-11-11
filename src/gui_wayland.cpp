#include "gui.hpp"

#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "errors.hpp"
#include "value.hpp"

#ifdef KEYLOOM_WAYLAND_GUI
#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>
#include <wayland-client.h>
#endif

namespace keyloom {

std::string resolve_compositor() {
    const char* session = std::getenv("XDG_SESSION_TYPE");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    if ((!session || std::string(session) != "x11") &&
        ((session && std::string(session) == "wayland") || (wayland && *wayland))) {
        try {
            validate_compositor("wayland");
            return "wayland";
        } catch (const RuntimeErrorKeyloom&) {
            // Preserve support for Xwayland-only builds and unavailable sockets.
        }
    }
    return "x11";
}

void validate_compositor(const std::string& compositor) {
    if (compositor == "x11") {
        try {
            XcbConnection connection;
        } catch (const RuntimeErrorKeyloom&) {
            throw RuntimeErrorKeyloom("option compositor = \"x11\" requires an accessible X11 or Xwayland display.");
        }
        return;
    }
    if (compositor != "wayland") throw RuntimeErrorKeyloom("Unknown compositor: " + compositor);
    const char* session = std::getenv("XDG_SESSION_TYPE");
    if (session && std::string(session) == "x11")
        throw RuntimeErrorKeyloom("option compositor = \"wayland\" cannot run in an X11 session.");
#ifdef KEYLOOM_WAYLAND_GUI
    if (std::getenv("WAYLAND_SOCKET")) {
        // libwayland consumes WAYLAND_SOCKET, which belongs to our caller and
        // cannot be reused by separate panel processes. Probe the named socket
        // instead and let each helper make its own connection.
        const char* name = std::getenv("WAYLAND_DISPLAY");
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!name || !*name || (name[0] != '/' && (!runtime || runtime[0] != '/')))
            throw RuntimeErrorKeyloom("Wayland panels need a named WAYLAND_DISPLAY when WAYLAND_SOCKET is set.");
        const std::string path = name[0] == '/' ? std::string(name) : std::string(runtime) + "/" + name;
        sockaddr_un address{};
        if (path.size() >= sizeof(address.sun_path))
            throw RuntimeErrorKeyloom("Wayland display socket path is too long.");
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) throw RuntimeErrorKeyloom("Could not open a Wayland display socket.");
        const int connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        ::close(fd);
        if (connected < 0) throw RuntimeErrorKeyloom("option compositor = \"wayland\" requires an accessible Wayland display.");
    } else {
        wl_display* display = wl_display_connect(nullptr);
        if (!display) throw RuntimeErrorKeyloom("option compositor = \"wayland\" requires an accessible Wayland display.");
        wl_display_disconnect(display);
    }
#else
    throw RuntimeErrorKeyloom("Native Wayland panels are unavailable: rebuild with GTK3 and wayland-client development files.");
#endif
}

#ifdef KEYLOOM_WAYLAND_GUI
namespace {

class WaylandWorker final : public GuiHost {
public:
    explicit WaylandWorker(const GuiPanel& panel) : GuiHost(panel), panel_(panel) {
        // GTK is used only for this backend. Never silently fall back to X11.
        static bool initialized = false;
        if (!initialized) {
            gdk_set_allowed_backends("wayland");
            initialized = gtk_init_check(nullptr, nullptr) && GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default());
        }
        if (!initialized)
            throw RuntimeErrorKeyloom("GUI panel \"" + panel.title + "\" needs an accessible native Wayland display.");
        window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window_), panel.title.c_str());
        gtk_window_set_default_size(GTK_WINDOW(window_), 240, -1);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(box), 16);
        gtk_container_add(GTK_CONTAINER(window_), box);
        for (const auto& widget : panel.widgets) {
            GtkWidget* control = nullptr;
            texts_.push_back(widget.text);
            if (widget.kind == GuiWidget::Kind::Label) {
                control = gtk_label_new(widget.text.c_str());
                gtk_label_set_xalign(GTK_LABEL(control), 0.0f);
            } else if (widget.kind == GuiWidget::Kind::Input) {
                control = gtk_entry_new();
                gtk_entry_set_text(GTK_ENTRY(control), widget.text.c_str());
                auto binding = std::make_unique<Binding>();
                binding->host = this;
                binding->index = texts_.size() - 1;
                g_signal_connect(control, "changed", G_CALLBACK(input_changed), binding.get());
                bindings_.push_back(std::move(binding));
            } else {
                control = gtk_button_new_with_label(widget.text.c_str());
                auto binding = std::make_unique<Binding>();
                binding->host = this;
                binding->id = widget.id;
                g_signal_connect(control, "clicked", G_CALLBACK(button_clicked), binding.get());
                bindings_.push_back(std::move(binding));
            }
            gtk_box_pack_start(GTK_BOX(box), control, FALSE, FALSE, 0);
            controls_.push_back(control);
        }
        g_signal_connect(window_, "destroy", G_CALLBACK(window_destroyed), this);
        gtk_widget_show_all(window_);
    }

    ~WaylandWorker() override { close(); }
    void set_input_handler(std::function<void(const std::string&, const std::string&)> handler) {
        input_handler_ = std::move(handler);
    }
#ifdef KEYLOOM_WAYLAND_TEST_HOOKS
    void test_interaction() {
        for (size_t i = 0; i < panel_.widgets.size(); ++i) {
            if (panel_.widgets[i].kind == GuiWidget::Kind::Input)
                gtk_entry_set_text(GTK_ENTRY(controls_[i]), "typed");
            if (panel_.widgets[i].kind == GuiWidget::Kind::Button)
                gtk_button_clicked(GTK_BUTTON(controls_[i]));
        }
    }
    void test_destroy() { if (window_) gtk_widget_destroy(window_); }
#endif
    int fd() const override { return -1; }
    bool is_closed() const override { return closed_; }

    void process_events() override {
        // GTK may have several poll fds and timers; iterate its default context
        // without blocking the scheduler and bound work to avoid starving tasks.
        for (int i = 0; i < 32 && g_main_context_pending(nullptr); ++i)
            g_main_context_iteration(nullptr, FALSE);
        auto events = std::move(events_);
        events_.clear();
        for (const auto& event : events) emit_event(event);
    }

    void close() override {
        if (!window_) return;
        closing_ = true;
        gtk_widget_destroy(window_);
        window_ = nullptr;
        closed_ = true;
    }

    bool has_widget(const std::string& id) const override { return index(id) >= 0; }
    std::string value(const std::string& id) const override {
        const int i = checked_index(id);
        if (panel_.widgets[static_cast<size_t>(i)].kind != GuiWidget::Kind::Input)
            throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(id) + " is not an input.");
        return texts_[static_cast<size_t>(i)];
    }
    void set_value(const std::string& id, const std::string& value) override { set_text(id, value); }
    void set_text(const std::string& id, const std::string& text) override {
        const int i = checked_index(id);
        const auto kind = panel_.widgets[static_cast<size_t>(i)].kind;
        if (kind == GuiWidget::Kind::Label)
            throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(id) + " is a label and has no variable text target.");
        texts_[static_cast<size_t>(i)] = text;
        if (closed_) return;
        GtkWidget* control = controls_[static_cast<size_t>(i)];
        if (kind == GuiWidget::Kind::Input) gtk_entry_set_text(GTK_ENTRY(control), text.c_str());
        else gtk_button_set_label(GTK_BUTTON(control), text.c_str());
    }
    void set_visible(bool visible) override {
        if (!window_) return;
        if (visible) gtk_widget_show(window_);
        else gtk_widget_hide(window_);
    }

private:
    struct Binding { WaylandWorker* host; std::string id; size_t index = 0; };
    static void input_changed(GtkEditable* editable, gpointer data) {
        auto* binding = static_cast<Binding*>(data);
        binding->host->texts_[binding->index] = gtk_entry_get_text(GTK_ENTRY(editable));
        if (binding->host->input_handler_)
            binding->host->input_handler_(binding->host->panel_.widgets[binding->index].id,
                                         binding->host->texts_[binding->index]);
    }
    static void button_clicked(GtkButton*, gpointer data) {
        const auto* binding = static_cast<Binding*>(data);
        binding->host->events_.push_back({EventKind::Click, binding->host->title(), binding->id});
    }
    static void window_destroyed(GtkWidget*, gpointer data) {
        auto* host = static_cast<WaylandWorker*>(data);
        host->window_ = nullptr;
        host->closed_ = true;
        if (!host->closing_) host->events_.push_back({EventKind::Close, host->title(), "window"});
    }
    int index(const std::string& id) const {
        for (size_t i = 0; i < panel_.widgets.size(); ++i)
            if (panel_.widgets[i].id == id) return static_cast<int>(i);
        return -1;
    }
    int checked_index(const std::string& id) const {
        const int i = index(id);
        if (i < 0) throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(id) + " is not declared in a gui block.");
        return i;
    }
    GuiPanel panel_;
    GtkWidget* window_ = nullptr;
    bool closed_ = false;
    bool closing_ = false;
    std::vector<GtkWidget*> controls_;
    std::vector<std::string> texts_;
    std::vector<std::unique_ptr<Binding>> bindings_;
    std::vector<Event> events_;
    std::function<void(const std::string&, const std::string&)> input_handler_;
};

// One sequenced packet contains a type, a 32-bit identifier length, then the
// identifier and payload. The socket is private to the parent and its child.
constexpr size_t kMaxPacket = 65536;
struct Message { char type = 0; std::string id; std::string text; };

bool send_message(int fd, char type, const std::string& id = {}, const std::string& text = {}) {
    if (id.size() + text.size() + 5 > kMaxPacket) return false;
    std::string packet(5, '\0');
    packet[0] = type;
    const uint32_t length = static_cast<uint32_t>(id.size());
    std::memcpy(packet.data() + 1, &length, sizeof(length));
    packet += id;
    packet += text;
    ssize_t written;
    do { written = ::send(fd, packet.data(), packet.size(), MSG_NOSIGNAL | MSG_DONTWAIT); }
    while (written < 0 && errno == EINTR);
    return written == static_cast<ssize_t>(packet.size());
}

// 1 = message, 0 = closed, -1 = no data, -2 = malformed/error.
int receive_message(int fd, Message& message) {
    char packet[kMaxPacket];
    ssize_t count;
    do { count = ::recv(fd, packet, sizeof(packet), MSG_DONTWAIT); } while (count < 0 && errno == EINTR);
    if (count == 0) return 0;
    if (count < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : -2;
    if (count < 5) return -2;
    uint32_t length;
    std::memcpy(&length, packet + 1, sizeof(length));
    if (length > static_cast<size_t>(count - 5)) return -2;
    message.type = packet[0];
    message.id.assign(packet + 5, length);
    message.text.assign(packet + 5 + length, static_cast<size_t>(count - 5) - length);
    return 1;
}

void close_inherited_fds(int keep) {
#ifdef SYS_close_range
    const bool low = keep <= 3 || ::syscall(SYS_close_range, 3U, static_cast<unsigned int>(keep - 1), 0U) == 0;
    const bool high = ::syscall(SYS_close_range, static_cast<unsigned int>(keep + 1), UINT_MAX, 0U) == 0;
    if (low && high) return;
#endif
    long limit = ::sysconf(_SC_OPEN_MAX);
    if (limit < 0) limit = 65536;
    for (long fd = 3; fd < limit; ++fd) if (fd != keep) ::close(static_cast<int>(fd));
}

int run_wayland_worker(int fd, const GuiPanel& panel) {
    try {
#ifdef KEYLOOM_WAYLAND_TEST_HOOKS
        if (const char* fail = std::getenv("KEYLOOM_TEST_FAIL_WAYLAND_STARTUP"); fail && std::string(fail) == "1")
            throw RuntimeErrorKeyloom("injected Wayland panel startup failure");
#endif
        WaylandWorker host(panel);
        bool connected = true;
        host.set_input_handler([&](const std::string& id, const std::string& text) {
            if (!send_message(fd, 'I', id, text)) connected = false;
        });
        host.set_event_handler([&](const GuiHost::Event& event) {
            if (!send_message(fd, event.kind == GuiHost::EventKind::Click ? 'C' : 'X', event.widget_id))
                connected = false;
        });
        if (!send_message(fd, 'R')) return 1;
        while (connected && !host.is_closed()) {
            host.process_events();
            pollfd item{fd, POLLIN, 0};
            const int ready = ::poll(&item, 1, 10);
            if (ready < 0 && errno != EINTR) break;
            if (ready <= 0) continue;
            Message message;
            const int received = receive_message(fd, message);
            if (received == 0 || received == -2) break;
            if (received < 0) continue;
            if (message.type == 'Q') break;
            if (message.type == 'T') host.set_text(message.id, message.text);
            else if (message.type == 'V') host.set_visible(message.text == "1");
#ifdef KEYLOOM_WAYLAND_TEST_HOOKS
            else if (message.type == 'D') {
                host.test_interaction();
                host.process_events();
                host.test_destroy();
                host.process_events();
            }
#endif
            else break;
        }
        return 0;
    } catch (const std::exception& error) {
        (void)send_message(fd, 'E', {}, error.what());
        return 1;
    }
}

class WaylandProxy final : public GuiHost {
public:
    explicit WaylandProxy(const GuiPanel& panel) : GuiHost(panel), panel_(panel) {
        for (const auto& widget : panel.widgets) texts_.push_back(widget.text);
        int sockets[2];
        if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) < 0)
            throw RuntimeErrorKeyloom("Cannot create Wayland panel socket: " + std::string(std::strerror(errno)));
        for (int& socket : sockets) {
            if (socket >= 3) continue;
            const int duplicate = ::fcntl(socket, F_DUPFD_CLOEXEC, 3);
            if (duplicate < 0) {
                const int error = errno;
                ::close(sockets[0]); ::close(sockets[1]);
                throw RuntimeErrorKeyloom("Cannot prepare Wayland panel socket: " + std::string(std::strerror(error)));
            }
            ::close(socket);
            socket = duplicate;
        }
        const pid_t parent = ::getpid();
        child_ = ::fork();
        if (child_ < 0) {
            const int error = errno;
            ::close(sockets[0]); ::close(sockets[1]);
            throw RuntimeErrorKeyloom("Cannot start Wayland panel: " + std::string(std::strerror(error)));
        }
        if (child_ == 0) {
            ::close(sockets[0]);
            close_inherited_fds(sockets[1]);
            // The helper makes a fresh connection to the named display.
            ::unsetenv("WAYLAND_SOCKET");
            (void)::prctl(PR_SET_PDEATHSIG, SIGTERM);
            if (::getppid() != parent) _exit(1);
            sigset_t empty;
            sigemptyset(&empty);
            ::sigprocmask(SIG_SETMASK, &empty, nullptr);
            ::signal(SIGTERM, SIG_DFL);
            ::signal(SIGINT, SIG_IGN);
            const int result = run_wayland_worker(sockets[1], panel);
            ::close(sockets[1]);
            _exit(result);
        }
        ::close(sockets[1]);
        fd_ = sockets[0];
        pollfd item{fd_, POLLIN, 0};
        int ready;
        do { ready = ::poll(&item, 1, 10000); } while (ready < 0 && errno == EINTR);
        Message message;
        const int received = ready > 0 ? receive_message(fd_, message) : -1;
        if (received != 1 || message.type != 'R') {
            const std::string detail = received == 1 && message.type == 'E' ? message.text : "startup failed or timed out";
            close();
            throw RuntimeErrorKeyloom("Native Wayland panel: " + detail);
        }
    }
    ~WaylandProxy() override { close(); }
    int fd() const override { return fd_; }
    bool is_closed() const override { return closed_; }
    void process_events() override {
        while (!closed_) {
            Message message;
            const int received = receive_message(fd_, message);
            if (received == -1) return;
            if (received <= 0) throw RuntimeErrorKeyloom("Native Wayland panel connection lost.");
            if (message.type == 'I') {
                const int i = index(message.id);
                if (i >= 0) texts_[static_cast<size_t>(i)] = message.text;
            } else if (message.type == 'C') {
                emit_event({EventKind::Click, title(), message.id});
            } else if (message.type == 'X') {
                closed_ = true;
                emit_event({EventKind::Close, title(), "window"});
            } else if (message.type == 'E') {
                throw RuntimeErrorKeyloom("Native Wayland panel: " + message.text);
            } else {
                throw RuntimeErrorKeyloom("Invalid message from native Wayland panel.");
            }
        }
    }
    void close() override {
        if (fd_ >= 0) {
            (void)send_message(fd_, 'Q');
            ::close(fd_);
            fd_ = -1;
        }
        closed_ = true;
        if (child_ <= 0) return;
        int status = 0;
        for (int i = 0; i < 100; ++i) {
            const pid_t result = ::waitpid(child_, &status, WNOHANG);
            if (result == child_ || (result < 0 && errno == ECHILD)) { child_ = -1; return; }
            ::usleep(1000);
        }
        (void)::kill(child_, SIGKILL);
        while (::waitpid(child_, &status, 0) < 0 && errno == EINTR) {}
        child_ = -1;
    }
    bool has_widget(const std::string& id) const override { return index(id) >= 0; }
    std::string value(const std::string& id) const override {
        const int i = checked_index(id);
        if (panel_.widgets[static_cast<size_t>(i)].kind != GuiWidget::Kind::Input)
            throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(id) + " is not an input.");
        return texts_[static_cast<size_t>(i)];
    }
    void set_value(const std::string& id, const std::string& value) override { set_text(id, value); }
    void set_text(const std::string& id, const std::string& text) override {
        const int i = checked_index(id);
        if (panel_.widgets[static_cast<size_t>(i)].kind == GuiWidget::Kind::Label)
            throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(id) + " is a label and has no variable text target.");
        if (!closed_ && !send_message(fd_, 'T', id, text))
            throw RuntimeErrorKeyloom("Could not send text to native Wayland panel.");
        texts_[static_cast<size_t>(i)] = text;
    }
    void set_visible(bool visible) override {
        if (!closed_ && !send_message(fd_, 'V', {}, visible ? "1" : "0"))
            throw RuntimeErrorKeyloom("Could not change native Wayland panel visibility.");
#ifdef KEYLOOM_WAYLAND_TEST_HOOKS
        const char* test = std::getenv("KEYLOOM_WAYLAND_TEST_INTERACTION");
        if (visible && !closed_ && test && std::string(test) == "1" && !test_requested_) {
            test_requested_ = true;
            if (!send_message(fd_, 'D')) throw RuntimeErrorKeyloom("Could not trigger Wayland test interaction.");
        }
#endif
    }
private:
    int index(const std::string& id) const {
        for (size_t i = 0; i < panel_.widgets.size(); ++i)
            if (panel_.widgets[i].id == id) return static_cast<int>(i);
        return -1;
    }
    int checked_index(const std::string& id) const {
        const int i = index(id);
        if (i < 0) throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(id) + " is not declared in a gui block.");
        return i;
    }
    GuiPanel panel_;
    std::vector<std::string> texts_;
    int fd_ = -1;
    pid_t child_ = -1;
    bool closed_ = false;
#ifdef KEYLOOM_WAYLAND_TEST_HOOKS
    bool test_requested_ = false;
#endif
};

}  // namespace
#endif

std::unique_ptr<GuiHost> create_wayland_gui(const GuiPanel& panel) {
#ifdef KEYLOOM_WAYLAND_GUI
    return std::make_unique<WaylandProxy>(panel);
#else
    (void)panel;
    throw RuntimeErrorKeyloom("Native Wayland panels are unavailable: rebuild with GTK3 and wayland-client development files.");
#endif
}

}  // namespace keyloom
