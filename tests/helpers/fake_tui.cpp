// A stand-in for an agent's TUI in tests. It draws with cursor addressing,
// asks its terminal questions and counts the answers. Each line has a row of
// its own and is redrawn in place:
//
//   1  fake tui cwd=<working directory> greeting=<$FAKE_TUI_GREETING>
//   2  size <columns>x<rows>                  redrawn on SIGWINCH
//   3  tty <yes|no> term=<$TERM> fds=<list>   controlling terminal; open
//                                             descriptors from 3 up
//   4  answers da1=<n> cpr=<n>                 answers to its queries so far
//   5  input <bytes> total=<n>                 the last input that was not an
//                                              answer, escaped; bytes of such
//                                              input so far
//   7  reply <bytes>                          what came back after the last
//                                              query that was asked with Q,
//                                              escaped
//
// At start it asks DA1 (CSI c) and CPR (CSI 6 n) once. Keys: q asks both
// again, Q<bytes> sends <bytes> to the terminal as a query of its own (an OSC
// 11
// ?, a colour scheme report) and shows everything that is read after it on
// line 7 (the last 60 characters), x prints "bye" and exits with 0, e exits
// with 7. With --ignore-hangup it ignores SIGHUP and the end of its input and
// waits to be killed.

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

volatile std::sig_atomic_t RESIZED = 0;
// The write end of a pipe the loop polls with its input: a resize wakes the
// wait wherever it comes, before the wait or in it. -1 if there is no pipe.
volatile std::sig_atomic_t RESIZE_WAKE = -1;

void OnResize(int /*signal*/) {
    const int saved = errno;
    RESIZED = 1;
    if (RESIZE_WAKE != -1) {
        const char byte = 0;
        // Full pipe: a wake is already pending, nothing is lost.
        [[maybe_unused]] const ssize_t n = ::write(RESIZE_WAKE, &byte, 1);
    }
    errno = saved;
}

// A pipe with both ends non-blocking and closed on exec; on failure both
// ends are -1.
bool MakeWakePipe(int (&ends)[2]) {
    if (::pipe(ends) != 0) {
        ends[0] = ends[1] = -1;
        return false;
    }
    for (const int end : ends) {
        const int flags = ::fcntl(end, F_GETFL);
        if (flags == -1 || ::fcntl(end, F_SETFL, flags | O_NONBLOCK) == -1 ||
            ::fcntl(end, F_SETFD, FD_CLOEXEC) == -1) {
            ::close(ends[0]);
            ::close(ends[1]);
            ends[0] = ends[1] = -1;
            return false;
        }
    }
    return true;
}

void Put(std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t n = ::write(STDOUT_FILENO, bytes.data(), bytes.size());
        if (n <= 0) {
            if (n == -1 && errno == EINTR) {
                continue;
            }
            return;
        }
        bytes.remove_prefix(static_cast<size_t>(n));
    }
}

void Line(int row, const std::string& text) {
    Put("\x1b[" + std::to_string(row) + ";1H\x1b[2K" + text);
}

std::string Escaped(std::string_view bytes) {
    std::string text;
    for (const char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 0x20 && byte < 0x7f && c != '\\') {
            text += c;
        } else {
            static constexpr char HEX[] = "0123456789abcdef";
            text += "\\x";
            text += HEX[byte >> 4];
            text += HEX[byte & 0xf];
        }
    }
    return text;
}

void DrawSize() {
    winsize size{};
    ::ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
    Line(2, "size " + std::to_string(size.ws_col) + "x" +
                std::to_string(size.ws_row));
}

void DrawTerminal() {
    const int tty = ::open("/dev/tty", O_RDWR | O_NOCTTY);
    if (tty != -1) {
        ::close(tty);
    }
    std::string fds;
    for (int fd = 3; fd < 1024; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1) {
            fds += (fds.empty() ? "" : ",") + std::to_string(fd);
        }
    }
    const char* term = std::getenv("TERM");
    Line(3, std::string("tty ") + (tty != -1 ? "yes" : "no") +
                " term=" + (term != nullptr ? term : "") + " fds=" + fds);
}

void Ask() { Put("\x1b[c\x1b[6n"); }

// Answers: DA1 is CSI ? ... c, CPR is CSI <row> ; <column> R.
struct Answers {
    int Da1 = 0;
    int Cpr = 0;
    std::string Pending;
    std::string Input;
    size_t InputBytes = 0;

    // Takes the bytes; an unfinished sequence waits for the next ones.
    void Feed(std::string_view bytes) {
        Pending += bytes;
        const size_t before = Pending.size();
        size_t answered = 0;
        while (!Pending.empty()) {
            if (Pending[0] != '\x1b') {
                const size_t escape = Pending.find('\x1b');
                Input = Pending.substr(0, escape);
                Pending.erase(0, escape);
                continue;
            }
            if (Pending.size() < 2) {
                break;
            }
            if (Pending[1] != '[') {
                Input = Pending.substr(0, 2);
                Pending.erase(0, 2);
                continue;
            }
            const size_t end = Pending.find_first_not_of("0123456789;?", 2);
            if (end == std::string::npos) {
                break;
            }
            if (Pending[end] == 'c' && Pending[2] == '?') {
                ++Da1;
                answered += end + 1;
            } else if (Pending[end] == 'R') {
                ++Cpr;
                answered += end + 1;
            } else {
                Input = Pending.substr(0, end + 1);
            }
            Pending.erase(0, end + 1);
        }
        InputBytes += before - Pending.size() - answered;
    }
};

}  // namespace

int main(int argc, char** argv) {
    const bool ignoreHangUp =
        argc > 1 && std::string_view(argv[1]) == "--ignore-hangup";
    if (ignoreHangUp) {
        std::signal(SIGHUP, SIG_IGN);
    }
    struct sigaction action{};
    action.sa_handler = OnResize;
    sigaction(SIGWINCH, &action, nullptr);

    termios mode{};
    if (::tcgetattr(STDIN_FILENO, &mode) == 0) {
        ::cfmakeraw(&mode);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &mode);
    }

    char cwd[4096] = {};
    if (::getcwd(cwd, sizeof(cwd)) == nullptr) {
        cwd[0] = '\0';
    }
    const char* greeting = std::getenv("FAKE_TUI_GREETING");
    Put("\x1b[2J");
    Line(1, std::string("fake tui cwd=") + cwd +
                " greeting=" + (greeting != nullptr ? greeting : ""));
    DrawSize();
    DrawTerminal();
    Answers answers;
    bool querying = false;
    std::string reply;
    Line(4, "answers da1=0 cpr=0");
    Ask();

    // Made after the descriptors are reported (line 3 lists any above 2). A
    // resize before it has set RESIZED, which the loop checks first.
    int wake[2] = {-1, -1};
    if (MakeWakePipe(wake)) {
        RESIZE_WAKE = wake[1];
    }

    // A resize that comes between the check below and poll() has written to
    // the wake pipe, so poll() returns at once and the redraw is not lost.
    // (Not a signal mask with pselect(): ThreadSanitizer runs a handler late,
    // outside a wait it does not know, and the wake was lost.) Without the
    // pipe the wait is on the input alone, as before.
    for (;;) {
        if (RESIZED != 0) {
            RESIZED = 0;
            DrawSize();
        }
        pollfd waits[2] = {
            {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
            {.fd = wake[0], .events = POLLIN, .revents = 0},
        };
        const nfds_t count = wake[0] != -1 ? 2 : 1;
        if (::poll(waits, count, -1) == -1) {
            if (errno == EINTR) {
                continue;  // a signal: RESIZED or the pipe tells a resize
            }
            // Any other error is left to read(), which ends the loop below.
        } else if (count == 2 && (waits[1].revents & POLLIN) != 0) {
            // Until EAGAIN: one wake for any number of resizes.
            char drained[64];
            while (::read(wake[0], drained, sizeof(drained)) > 0) {
            }
            if ((waits[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;  // a resize alone
            }
        }
        char buffer[4096];
        const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (n <= 0) {
            if (n == -1 && errno == EINTR) {
                continue;
            }
            if (ignoreHangUp) {
                for (;;) {
                    ::pause();
                }
            }
            return 0;
        }
        const std::string_view bytes(buffer, static_cast<size_t>(n));
        if (bytes.size() > 1 && bytes[0] == 'Q') {
            Put(bytes.substr(1));
            querying = true;
            reply.clear();
            Line(7, "reply ");
            continue;
        }
        if (querying) {
            reply += bytes;
            // The end of it: a line that wraps is two rows of the screen.
            const std::string shown = Escaped(reply);
            Line(7,
                 "reply " + (shown.size() > 60 ? shown.substr(shown.size() - 60)
                                               : shown));
            continue;
        }
        if (bytes == "x") {
            Line(6, "bye\r\n");
            return 0;
        }
        if (bytes == "e") {
            return 7;
        }
        if (bytes == "q") {
            Ask();
            continue;
        }
        answers.Feed(bytes);
        Line(4, "answers da1=" + std::to_string(answers.Da1) +
                    " cpr=" + std::to_string(answers.Cpr));
        if (!answers.Input.empty()) {
            Line(5, "input " + Escaped(answers.Input) +
                        " total=" + std::to_string(answers.InputBytes));
            answers.Input.clear();
        }
    }
}
