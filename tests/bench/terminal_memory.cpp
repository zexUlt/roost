// Memory of one libghostty-vt terminal as the window holds it: a
// TerminalSession (terminal, render state, mutex) at 200x50. Prints the
// per-terminal growth of the process footprint and RSS after each state, in
// MiB. Not a test: run it by hand, once per process, since the allocator keeps
// freed memory between runs inside one (docs/prior-art.md §2.5).
//
//   terminal_memory [sessions]

#include "terminal/terminal_session.hpp"

#include <QByteArray>
#include <QString>

#include <mach/mach.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using roost::TerminalSession;

namespace {

constexpr quint16 COLUMNS = 200;
constexpr quint16 ROWS = 50;
constexpr int DEFAULT_SESSIONS = 20;
constexpr double MIB = 1024.0 * 1024.0;
// Lines written between two checks of the scrollback size.
constexpr int FILL_CHUNK_LINES = 500;
// Lines written after the scrollback stopped growing, to be sure it is full.
constexpr int FILL_EXTRA_LINES = 5000;
constexpr int MAX_FILL_LINES = 2'000'000;

struct Usage {
    double FootprintMiB = 0;
    double RssMiB = 0;
};

Usage Measure() {
    Usage usage;
    task_vm_info_data_t vm{};
    mach_msg_type_number_t vmCount = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&vm),
                  &vmCount) == KERN_SUCCESS) {
        usage.FootprintMiB = static_cast<double>(vm.phys_footprint) / MIB;
    }
    mach_task_basic_info_data_t basic{};
    mach_msg_type_number_t basicCount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&basic),
                  &basicCount) == KERN_SUCCESS) {
        usage.RssMiB = static_cast<double>(basic.resident_size) / MIB;
    }
    return usage;
}

// Deterministic: the same bytes in every run.
class Rng {
public:
    std::uint32_t Next() {
        State_ = State_ * 1664525U + 1013904223U;
        return State_ >> 8;
    }

private:
    std::uint32_t State_ = 12345;
};

// A line of what an agent TUI prints: colored words, CJK, emoji, a bold or
// dim run now and then. Ends without a line break.
QByteArray StyledLine(Rng& rng, int columns) {
    static const char* const WORDS[] = {"const",
                                        "result",
                                        "session",
                                        "terminal",
                                        "render",
                                        "Edit",
                                        "src/core/vt_terminal.cpp",
                                        "tokens",
                                        "42%",
                                        "ok",
                                        "error:",
                                        "diff"};
    static const char* const WIDE[] = {"日本語", "ターミナル", "セッション",
                                       "端末",   "完了",       "🚀",
                                       "✅",     "🔥",         "👩‍💻"};
    QByteArray line;
    int used = 0;
    while (used < columns - 12) {
        const std::uint32_t r = rng.Next();
        switch (r % 4) {
            case 0:
                line += "\x1b[38;2;" + QByteArray::number(r % 256) + ';' +
                        QByteArray::number((r >> 8) % 256) + ';' +
                        QByteArray::number((r >> 16) % 256) + 'm';
                break;
            case 1:
                line += "\x1b[" + QByteArray::number(31 + (r >> 4) % 7) + 'm';
                break;
            case 2:
                line +=
                    "\x1b[48;5;" + QByteArray::number((r >> 4) % 256) + ";1m";
                break;
            default:
                line += (r >> 4) % 3 == 0 ? "\x1b[2;3m" : "\x1b[0m";
                break;
        }
        const char* word = (r >> 6) % 3 == 0
                               ? WIDE[(r >> 8) % std::size(WIDE)]
                               : WORDS[(r >> 8) % std::size(WORDS)];
        line += word;
        line += ' ';
        used += static_cast<int>(QByteArray(word).size()) + 1;
    }
    line += "\x1b[0m";
    return line;
}

void Write(TerminalSession& session, const QByteArray& bytes) {
    const auto lock = session.Lock();
    session.Write(lock, bytes);
}

// Lines in the terminal, scrollback included, counted in its dump.
qsizetype TotalRows(TerminalSession& session) {
    const auto lock = session.Lock();
    const auto dump = session.Dump(lock);
    if (!dump) {
        std::fprintf(stderr, "dump: %s\n", qPrintable(dump.error()));
        std::exit(1);
    }
    return dump->count('\n') + 1;
}

void FillScreen(TerminalSession& session, Rng& rng) {
    QByteArray bytes = "\x1b[2J";
    for (int row = 1; row <= ROWS; ++row) {
        bytes += "\x1b[" + QByteArray::number(row) + ";1H";
        bytes += StyledLine(rng, COLUMNS);
    }
    Write(session, bytes);
}

// Scrolls lines into the scrollback until it stops growing, then some more.
// Returns the rows it holds.
qsizetype FillScrollback(TerminalSession& session, Rng& rng) {
    qsizetype last = TotalRows(session);
    qsizetype peak = last;
    int sincePeak = 0;
    for (int lines = 0; lines < MAX_FILL_LINES; lines += FILL_CHUNK_LINES) {
        QByteArray bytes;
        for (int i = 0; i < FILL_CHUNK_LINES; ++i) {
            bytes += "\r\n";
            bytes += StyledLine(rng, COLUMNS);
        }
        Write(session, bytes);
        last = TotalRows(session);
        if (last > peak) {
            peak = last;
            sincePeak = 0;
        } else {
            sincePeak += FILL_CHUNK_LINES;
        }
        // Older pages are dropped whole, so the size no longer rises once the
        // limit is reached.
        if (sincePeak >= FILL_EXTRA_LINES) {
            break;
        }
    }
    return peak;
}

void Render(TerminalSession& session) {
    {
        const auto lock = session.Lock();
        if (const auto begun = session.BeginRenderUpdate(lock); !begun) {
            std::fprintf(stderr, "render: %s\n", qPrintable(begun.error()));
            std::exit(1);
        }
    }
    session.EndRenderUpdate();
}

}  // namespace

int main(int argc, char** argv) {
    const int count = argc > 1 ? std::atoi(argv[1]) : DEFAULT_SESSIONS;
    if (count < 1) {
        std::fprintf(stderr, "usage: terminal_memory [sessions]\n");
        return 2;
    }

    const Usage base = Measure();
    std::vector<std::unique_ptr<TerminalSession>> sessions;
    for (int i = 0; i < count; ++i) {
        auto session = TerminalSession::Create(COLUMNS, ROWS);
        if (!session) {
            std::fprintf(stderr, "create: %s\n", qPrintable(session.error()));
            return 1;
        }
        sessions.push_back(std::move(*session));
    }

    std::printf("%d sessions of %dx%d\n", count, COLUMNS, ROWS);
    std::printf("%-22s %14s %10s\n", "state", "footprint MiB", "RSS MiB");
    const auto report = [&](const char* state) {
        const Usage now = Measure();
        std::printf("%-22s %14.2f %10.2f\n", state,
                    (now.FootprintMiB - base.FootprintMiB) / count,
                    (now.RssMiB - base.RssMiB) / count);
    };

    report("created");
    Rng rng;
    for (const auto& session : sessions) {
        FillScreen(*session, rng);
    }
    report("screen");
    qsizetype rows = 0;
    for (const auto& session : sessions) {
        rows = FillScrollback(*session, rng);
    }
    report("scrollback full");
    for (const auto& session : sessions) {
        Render(*session);
    }
    report("after render update");
    std::printf("peak rows while filling, scrollback included: %lld\n",
                static_cast<long long>(rows));
    return 0;
}
