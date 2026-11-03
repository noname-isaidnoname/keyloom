#include "subprocess.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "errors.hpp"

namespace keyloom {
namespace {

void make_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

std::string drain(int fd) {
    std::string out;
    char buffer[4096];
    while (true) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            out.append(buffer, static_cast<size_t>(count));
            continue;
        }
        if (count == 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        break;
    }
    return out;
}

bool write_all(int fd, const std::string& text) {
    size_t written = 0;
    while (written < text.size()) {
        const ssize_t count = ::write(fd, text.data() + written, text.size() - written);
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
            if (errno == EAGAIN) {
                pollfd entry{fd, POLLOUT, 0};
                ::poll(&entry, 1, 100);
            }
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace

std::string which(const std::string& name) {
    if (name.empty()) return {};
    if (name.find('/') != std::string::npos) {
        return ::access(name.c_str(), X_OK) == 0 ? name : std::string();
    }
    const char* path = std::getenv("PATH");
    const std::string search = path != nullptr ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= search.size()) {
        const size_t end = search.find(':', start);
        const std::string directory =
            end == std::string::npos ? search.substr(start) : search.substr(start, end - start);
        const std::string candidate = (directory.empty() ? std::string(".") : directory) + "/" + name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

CommandResult run_command(const std::vector<std::string>& argv, const std::string& input, int timeout_ms) {
    CommandResult result;
    if (argv.empty()) {
        result.spawn_failed = true;
        result.spawn_error = "empty command";
        return result;
    }

    int input_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    int error_pipe[2] = {-1, -1};
    if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0 || ::pipe(error_pipe) != 0) {
        result.spawn_failed = true;
        result.spawn_error = std::strerror(errno);
        for (int fd : {input_pipe[0], input_pipe[1], output_pipe[0], output_pipe[1], error_pipe[0], error_pipe[1]}) {
            if (fd >= 0) ::close(fd);
        }
        return result;
    }

    // argv must outlive the child's exec call; build it before forking.
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const auto& argument : argv) raw.push_back(const_cast<char*>(argument.c_str()));
    raw.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        result.spawn_failed = true;
        result.spawn_error = std::strerror(errno);
        for (int fd : {input_pipe[0], input_pipe[1], output_pipe[0], output_pipe[1], error_pipe[0], error_pipe[1]}) {
            ::close(fd);
        }
        return result;
    }
    if (pid == 0) {
        ::dup2(input_pipe[0], STDIN_FILENO);
        ::dup2(output_pipe[1], STDOUT_FILENO);
        ::dup2(error_pipe[1], STDERR_FILENO);
        // Close the parent's ends; a failure here cannot be reported usefully.
        ::close(input_pipe[1]);
        ::close(output_pipe[0]);
        ::close(error_pipe[0]);
        ::execvp(raw[0], raw.data());
        _exit(127);
    }

    ::close(input_pipe[0]);
    ::close(output_pipe[1]);
    ::close(error_pipe[1]);
    if (input.empty()) {
        ::close(input_pipe[1]);
        input_pipe[1] = -1;
    } else {
        make_nonblocking(input_pipe[1]);
    }
    make_nonblocking(output_pipe[0]);
    make_nonblocking(error_pipe[0]);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
    const bool bounded = timeout_ms > 0;
    bool child_exited = false;
    int status = 0;
    bool killed = false;

    int stdin_index = -1;
    while (true) {
        pollfd entries[3];
        int count = 0;
        entries[count++] = {output_pipe[0], POLLIN, 0};
        entries[count++] = {error_pipe[0], POLLIN, 0};
        stdin_index = -1;
        if (input_pipe[1] >= 0) {
            stdin_index = count;
            entries[count++] = {input_pipe[1], POLLOUT, 0};
        }

        int wait_ms = 200;
        if (bounded) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
            wait_ms = remaining <= 0 ? 0 : static_cast<int>(remaining);
        }
        const int ready = ::poll(entries, static_cast<nfds_t>(count), wait_ms);
        if (ready < 0 && errno != EINTR) break;

        if (stdin_index >= 0 && (entries[stdin_index].revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
            // A child that exits before reading is not an error; its status and
            // output are still collected below.
            write_all(input_pipe[1], input);
            ::close(input_pipe[1]);
            input_pipe[1] = -1;
        }
        if ((entries[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) result.out += drain(output_pipe[0]);
        if ((entries[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) result.err += drain(error_pipe[0]);

        if (!child_exited) {
            const pid_t waited = ::waitpid(pid, &status, WNOHANG);
            if (waited == pid) child_exited = true;
        }

        if (bounded && !child_exited) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0) {
                ::kill(pid, SIGKILL);
                killed = true;
            }
        }
        if (child_exited) {
            result.out += drain(output_pipe[0]);
            result.err += drain(error_pipe[0]);
            break;
        }
    }

    if (input_pipe[1] >= 0) ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(error_pipe[0]);
    if (!child_exited) {
        ::waitpid(pid, &status, 0);
    }
    result.timed_out = killed;
    if (WIFEXITED(status)) {
        result.returncode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.returncode = 128 + WTERMSIG(status);
    }
    return result;
}

bool spawn_detached(const std::vector<std::string>& argv, std::string& error) {
    if (argv.empty()) {
        error = "empty command";
        return false;
    }
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const auto& argument : argv) raw.push_back(const_cast<char*>(argument.c_str()));
    raw.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        ::setsid();
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }
        ::execvp(raw[0], raw.data());
        _exit(127);
    }
    // Reap the intermediate child; the grandchild is reparented to init.
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        error = "command not found";
        return false;
    }
    return true;
}

std::vector<std::string> split_shell_words(const std::string& text) {
    std::vector<std::string> words;
    std::string current;
    bool in_word = false;
    size_t index = 0;

    auto flush = [&]() {
        if (in_word) {
            words.push_back(current);
            current.clear();
            in_word = false;
        }
    };

    while (index < text.size()) {
        const char c = text[index];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            flush();
            ++index;
            continue;
        }
        if (c == '#') {
            // shlex.split leaves comments enabled: the rest of the line is dropped
            // only when the '#' starts a word.
            if (!in_word) {
                while (index < text.size() && text[index] != '\n') ++index;
                continue;
            }
        }
        if (c == '\\') {
            if (index + 1 >= text.size()) {
                throw BackendError("No escaped character");
            }
            current.push_back(text[index + 1]);
            in_word = true;
            index += 2;
            continue;
        }
        if (c == '\'') {
            in_word = true;
            ++index;
            bool closed = false;
            while (index < text.size()) {
                if (text[index] == '\'') {
                    closed = true;
                    ++index;
                    break;
                }
                current.push_back(text[index]);
                ++index;
            }
            if (!closed) throw BackendError("No closing quotation");
            continue;
        }
        if (c == '"') {
            in_word = true;
            ++index;
            bool closed = false;
            while (index < text.size()) {
                if (text[index] == '"') {
                    closed = true;
                    ++index;
                    break;
                }
                if (text[index] == '\\' && index + 1 < text.size() &&
                    (text[index + 1] == '"' || text[index + 1] == '\\')) {
                    current.push_back(text[index + 1]);
                    index += 2;
                    continue;
                }
                current.push_back(text[index]);
                ++index;
            }
            if (!closed) throw BackendError("No closing quotation");
            continue;
        }
        current.push_back(c);
        in_word = true;
        ++index;
    }
    flush();
    return words;
}

}  // namespace keyloom
