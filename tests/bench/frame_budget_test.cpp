// The frame benchmark of design.md section 9 (M3, "tests"): what the GUI
// thread spends per frame while ten sessions each take 1 MB/s of TUI-like
// output, fed by one writer thread the way IoThread feeds a window.
//
// Two scenarios share the measuring code:
//  * Gated (the M3 criterion): the window shows one TerminalItem, which draws
//    session 0; the other nine warm sessions are only written to
//    (design.md section 3). p95 of the GUI-thread time must be within 4 ms.
//  * Not gated: ten visible items. Informational only; if its p95 stays far
//    over the budget, see design.md section 13, M3 item 3 (glyph atlas).
//
// The test catches regressions; it does not prove 60 Hz. The offscreen
// platform gives no RHI, so Qt Quick draws with the software renderer, whose
// render thread exists only with QSG_RENDER_LOOP=threaded (prior-art 5.1);
// CMake sets it for this test, and the test fails if the loop is not
// threaded. Software rendering is not the speed of a real screen.
//
// Whole-thread check: the phases above see only the work of the scene graph
// and of the update slot. Any other GUI-thread work (a timer, a model, a
// handler) is invisible to them, so the gate also takes the GUI thread's CPU
// time over the measured window, per frame, and adds the mean time blocked in
// sync (the thread burns no CPU while blocked). That sum, the mean GUI busy
// time per frame, must be within the same budget.
//
// What is measured, per frame: the GUI thread's time, in three phases (see
// FrameProbe): the polish of the items, the sync, in which the GUI thread is
// blocked while the render thread runs updatePaintNode, and the slots that
// handle the Updated signals. Rendering proper is not in it.

#include "qml_api/terminal_item.hpp"
#include "terminal/renderer.hpp"
#include "terminal/terminal_session.hpp"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QtLogging>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>
#elif defined(__linux__)
#include <ctime>
#endif

using roost::TerminalItem;
using roost::TerminalSession;

namespace {

using Clock = std::chrono::steady_clock;

// Warm sessions of the window, all fed.
constexpr int SESSION_COUNT = 10;

// The gated scenario: one item fills the window.
constexpr quint16 GATED_COLUMNS = 200;
constexpr quint16 GATED_ROWS = 50;
// The ungated scenario: every session has an item, in a grid.
constexpr quint16 GRID_TERMINAL_COLUMNS = 100;
constexpr quint16 GRID_TERMINAL_ROWS = 30;
constexpr int GRID_COLUMNS = 2;

// The streams run for WARMUP_MS before the measurement, so that the first
// frames, which build every row of every item, are not in it.
constexpr int WARMUP_MS = 1000;
constexpr int MEASURE_MS = 5000;

// What each session must take, and the size of one write of the writer
// thread (a socket read in the IO thread is of this order).
constexpr double STREAM_BYTES_PER_SECOND = 1'000'000;
// Timing slack of the throughput check: the rate is measured over a window
// that starts and ends a little after the writer's own clock.
constexpr double STREAM_TOLERANCE = 0.99;
// ROOST_FRAME_BUDGET=report is for a shared CI runner, a virtual machine that
// can stop the writer thread for a while: the 5 s window of a session has been
// seen at 97.9% of STREAM_BYTES_PER_SECOND there, against 99.97..100.02%
// otherwise. The rate is then printed and a session need only have made real
// progress: a writer that is stalled or blocked writes nothing at all, so
// half of the rate is far from what a slow runner does and far from a stall.
constexpr double STREAM_REPORT_FLOOR = 0.5;
constexpr int CHUNK_BYTES = 2048;
// The writer cycles through this much generated output per session.
constexpr int POOL_BYTES = 512 * 1024;
// How long the writer sleeps when no session is due.
constexpr int WRITER_IDLE_US = 100;

// design.md §9: GUI-thread work per frame must stay within 4 ms. The test
// checks the 95th percentile of the frames of the run against it.
constexpr double P95_BUDGET_MS = 4.0;
// The same budget for the mean GUI busy time per frame (CPU plus sync).
constexpr double BUSY_BUDGET_MS = 4.0;

// How long the window may take to draw its first frame.
constexpr int FIRST_FRAME_TIMEOUT_MS = 20000;

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

// CPU time (user + system) of the calling thread, in ns; -1 where the
// platform has no way to read it.
std::int64_t ThreadCpuNs() {
#if defined(__APPLE__)
    const mach_port_t port = mach_thread_self();
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    const auto status =
        thread_info(port, THREAD_BASIC_INFO,
                    reinterpret_cast<thread_info_t>(&info), &count);
    mach_port_deallocate(mach_task_self(), port);
    if (status != KERN_SUCCESS) {
        return -1;
    }
    const auto toNs = [](const time_value_t& value) {
        return static_cast<std::int64_t>(value.seconds) * 1'000'000'000 +
               static_cast<std::int64_t>(value.microseconds) * 1000;
    };
    return toNs(info.user_time) + toNs(info.system_time);
#elif defined(__linux__)
    timespec now{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) {
        return -1;
    }
    return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
#else
    return -1;
#endif
}

// A deterministic xorshift: the same stream on every run and machine.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : State_(seed * 2654435761U + 88172645) {}

    std::uint32_t Next() {
        State_ ^= State_ << 13;
        State_ ^= State_ >> 7;
        State_ ^= State_ << 17;
        return static_cast<std::uint32_t>(State_ >> 11);
    }

    // In [0, bound): an index, hence unsigned.
    std::size_t Below(std::size_t bound) { return Next() % bound; }

private:
    std::uint64_t State_;
};

// Something like what a TUI agent prints: coloured and bold status lines
// drawn at fixed places, cleared and redrawn, wide CJK and emoji, and
// ordinary text lines that scroll the screen.
QByteArray MakeStream(int seed, int bytes, quint16 terminalRows) {
    static const std::array<const char*, 8> WORDS = {
        "running",
        "tool_use",
        "Edit(src/terminal/renderer.cpp)",
        "thinking",
        "waiting for approval",
        "git diff --stat",
        "tests passed",
        "reading file"};
    static const std::array<const char*, 6> WIDE = {
        "日本語のテキスト",  "中文字符测试",
        "한국어 텍스트",     "完了 ✅ 進行中 ⏳",
        "🚀 deploy 🎉 done", "👩‍💻 pair 🧑‍🔧 fix"};

    Rng rng(static_cast<std::uint64_t>(seed) + 1);
    QByteArray out;
    out.reserve(bytes + 512);
    while (out.size() < bytes) {
        switch (rng.Below(6)) {
            case 0:
                // A status line at a fixed row: move, clear, colour, text.
                out += QByteArray("\x1b[") +
                       QByteArray::number(1 + rng.Below(terminalRows)) +
                       ";1H\x1b[2K\x1b[38;5;" +
                       QByteArray::number(rng.Below(256)) + "m" +
                       WORDS[rng.Below(8)] + "\x1b[0m";
                break;
            case 1:
                // Truecolor foreground on a coloured background, bold.
                out += "\x1b[1;38;2;" + QByteArray::number(rng.Below(256)) +
                       ";" + QByteArray::number(rng.Below(256)) + ";" +
                       QByteArray::number(rng.Below(256)) + ";48;5;" +
                       QByteArray::number(rng.Below(256)) + "m " +
                       WORDS[rng.Below(8)] + " \x1b[0m ";
                break;
            case 2:
                // Wide characters in the middle of a line.
                out += QByteArray("\x1b[") +
                       QByteArray::number(1 + rng.Below(terminalRows)) + ";" +
                       QByteArray::number(1 + rng.Below(60)) + "H" +
                       WIDE[rng.Below(6)];
                break;
            case 3:
                // Erase to the end of the line, then the cursor back.
                out += "\x1b[K\x1b[" + QByteArray::number(1 + rng.Below(8)) +
                       "D\x1b[4m" + WORDS[rng.Below(8)] + "\x1b[24m";
                break;
            default:
                // Plain lines that scroll: the bottom row repeats.
                out += QByteArray("\x1b[") + QByteArray::number(terminalRows) +
                       ";1H\r\n" + "\x1b[3" + QByteArray::number(rng.Below(8)) +
                       "m" + WORDS[rng.Below(8)] + ' ' + WORDS[rng.Below(8)] +
                       ' ' + QByteArray::number(rng.Below(100000)) + "\x1b[0m";
                break;
        }
    }
    return out;
}

double Percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0;
    }
    // Nearest rank.
    const auto rank = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(sorted.size())));
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

// What IoThread does for the sessions of a window, in one writer thread: it
// takes each session in turn, puts a chunk in under that session's lock, and
// then raises an Updated unless one is already in flight
// (IoThread::MarkUpdated). Only a shown session has anyone to tell: the GUI
// thread acknowledges before it reads (IoThread::AcknowledgeUpdate).
class Feed : public QObject {
    Q_OBJECT

public:
    struct Stream {
        std::shared_ptr<TerminalSession> Session;
        QByteArray Pool;
        // Whether an item shows the session: only then is Updated raised.
        bool Shown = false;
        // Set while an Updated is in flight.
        std::atomic<bool> Pending{true};
        std::atomic<std::int64_t> Written{0};
    };

    void Add(std::shared_ptr<TerminalSession> session, int seed,
             quint16 terminalRows, bool shown) {
        auto& stream = Streams_.emplace_back(std::make_unique<Stream>());
        stream->Session = std::move(session);
        stream->Pool = MakeStream(seed, POOL_BYTES, terminalRows);
        stream->Shown = shown;
    }

    std::size_t Size() const { return Streams_.size(); }

    // The same as IoThread::AcknowledgeUpdate.
    void Acknowledge(std::size_t index) {
        Streams_[index]->Pending.store(false);
    }

    void Start() {
        Writer_ = std::thread([this] { Run(); });
    }

    void Stop() {
        Stop_.store(true);
        if (Writer_.joinable()) {
            Writer_.join();
        }
    }

    ~Feed() override { Stop(); }

    std::int64_t TotalWritten(std::size_t index) const {
        return Streams_[index]->Written.load();
    }

    // The time the writer has spent writing (not sleeping), in ns.
    std::int64_t BusyNs() const { return BusyNs_.load(); }

signals:
    // Emitted from the writer thread; the receiver lives in the GUI thread,
    // so the connection is queued.
    void Updated(int index);

private:
    // Writes every session at STREAM_BYTES_PER_SECOND, catching up if it
    // falls behind, round-robin in chunks.
    void Run() {
        const auto start = Clock::now();
        std::vector<qsizetype> offsets(Streams_.size(), 0);
        std::vector<std::int64_t> written(Streams_.size(), 0);
        std::int64_t busyNs = 0;
        while (!Stop_.load()) {
            const auto elapsed =
                std::chrono::duration<double>(Clock::now() - start).count();
            bool wroteAny = false;
            for (std::size_t index = 0; index < Streams_.size(); ++index) {
                if (static_cast<double>(written[index]) >
                    elapsed * STREAM_BYTES_PER_SECOND) {
                    continue;
                }
                const auto begin = Clock::now();
                auto& stream = *Streams_[index];
                const auto size = std::min<qsizetype>(
                    CHUNK_BYTES, stream.Pool.size() - offsets[index]);
                const QByteArrayView chunk(
                    stream.Pool.constData() + offsets[index], size);
                {
                    const auto lock = stream.Session->Lock();
                    stream.Session->Write(lock, chunk);
                }
                offsets[index] = (offsets[index] + size) % stream.Pool.size();
                written[index] += static_cast<std::int64_t>(size);
                stream.Written.store(written[index]);
                if (stream.Shown && !stream.Pending.exchange(true)) {
                    emit Updated(static_cast<int>(index));
                }
                busyNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                              Clock::now() - begin)
                              .count();
                BusyNs_.store(busyNs);
                wroteAny = true;
            }
            if (!wroteAny) {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(WRITER_IDLE_US));
            }
        }
    }

    std::vector<std::unique_ptr<Stream>> Streams_;
    std::thread Writer_;
    std::atomic<bool> Stop_{false};
    std::atomic<std::int64_t> BusyNs_{0};
};

// One frame's GUI-thread time, in ms, by phase.
struct Frame {
    // The polish of the items since the previous frame's sync.
    double Polish = 0;
    // The sync: the GUI thread blocked while the render thread runs
    // updatePaintNode.
    double Sync = 0;
    // The update slots that ran since the previous frame's sync.
    double Slots = 0;

    double Total() const { return Polish + Sync + Slots; }
};

// Collects the frames. A frame's GUI-thread cost is the sum of three phases,
// each timed where it happens:
//
//  * sync: beforeSynchronizing to afterSynchronizing. The render thread emits
//    both while the GUI thread is blocked in the scene graph's sync, and in
//    between runs every updatePaintNode, where a TerminalItem rebuilds its
//    dirty rows. Rendering proper starts after afterSynchronizing and runs
//    without the GUI thread, so it is not in the number. (If the previous
//    frame were still rendering when the sync is requested, the GUI thread
//    would wait for it before beforeSynchronizing: that wait is not counted
//    either.)
//  * polish: the duration of every updatePolish of the items, summed. The
//    scene graph has no signal for the start of the polish
//    (QQuickWindow::afterAnimating comes after it), so the items time
//    themselves: see BenchItem.
//  * slots: the time in the update slot, the GUI-thread work that the IO
//    thread's Updated signal causes.
//
// Between frames the GUI thread sleeps in the event loop, which is idle, not
// work, so the wall time between frames is not used. Each phase is reset when
// a sync ends, so a phase belongs to the frame that follows it.
class FrameProbe : public QObject {
    Q_OBJECT

public:
    explicit FrameProbe(QQuickWindow* window)
        : GuiThread_(QThread::currentThread()) {
        // Render thread, from here the GUI thread is blocked. If the loop is
        // not threaded these run in the GUI thread, which RenderThreadSeen
        // tells apart.
        connect(
            window, &QQuickWindow::beforeSynchronizing, this,
            [this] { SyncStartNs_.store(NowNs()); }, Qt::DirectConnection);
        connect(
            window, &QQuickWindow::beforeRendering, this,
            [this] {
                if (QThread::currentThread() != GuiThread_) {
                    RenderThreadSeen_.store(true);
                }
            },
            Qt::DirectConnection);
        connect(
            window, &QQuickWindow::afterSynchronizing, this,
            [this] { EndFrame(); }, Qt::DirectConnection);
    }

    // GUI thread: the cost of one polish or one update slot.
    void AddPolishNs(std::int64_t ns) { PolishNs_.fetch_add(ns); }
    void AddSlotNs(std::int64_t ns) { SlotNs_.fetch_add(ns); }

    bool RenderThreadSeen() const { return RenderThreadSeen_.load(); }

    // Starts recording from now, dropping what came before.
    void StartRecording() {
        const std::scoped_lock lock(Mutex_);
        Frames_.clear();
        Recording_ = true;
    }

    std::vector<Frame> StopRecording() {
        const std::scoped_lock lock(Mutex_);
        Recording_ = false;
        return std::exchange(Frames_, {});
    }

private:
    // Render thread, with the GUI thread blocked.
    void EndFrame() {
        const auto now = NowNs();
        const Frame frame{
            .Polish = static_cast<double>(PolishNs_.exchange(0)) / 1e6,
            .Sync = static_cast<double>(now - SyncStartNs_.load()) / 1e6,
            .Slots = static_cast<double>(SlotNs_.exchange(0)) / 1e6};
        const std::scoped_lock lock(Mutex_);
        if (Recording_) {
            Frames_.push_back(frame);
        }
    }

    QThread* GuiThread_;
    std::atomic<std::int64_t> PolishNs_{0};
    std::atomic<std::int64_t> SlotNs_{0};
    std::atomic<std::int64_t> SyncStartNs_{0};
    std::atomic<bool> RenderThreadSeen_{false};
    std::mutex Mutex_;
    bool Recording_ = false;
    std::vector<Frame> Frames_;
};

// A TerminalItem that reports the time of its polish to the probe. It changes
// nothing else.
class BenchItem : public TerminalItem {
public:
    BenchItem(QQuickItem* parent, FrameProbe& probe)
        : TerminalItem(parent), Probe_(probe) {}

protected:
    void updatePolish() override {
        const auto start = NowNs();
        TerminalItem::updatePolish();
        Probe_.AddPolishNs(NowNs() - start);
    }

private:
    FrameProbe& Probe_;
};

void RunFor(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// What one scenario shows and how it is laid out.
struct Scenario {
    const char* Name;
    quint16 Columns;
    quint16 Rows;
    // How many sessions have an item; the first ones.
    int VisibleCount;
    // The items are laid out in this many columns.
    int GridColumns;
};

struct Summary {
    double P50 = 0;
    double P95 = 0;
    double P99 = 0;
    double Max = 0;
};

// What a scenario measured.
struct Result {
    Summary Gui;
    // Mean GUI busy time per frame, ms: CpuPerFrame + MeanSync. Negative
    // CpuPerFrame (and Busy) where the platform cannot read thread CPU time.
    double CpuPerFrame = -1;
    double MeanSync = 0;
    double Busy = -1;
    double Seconds = 0;
    std::size_t FrameCount = 0;
    int Swapped = 0;
    double SlowestBytesPerSecond = 0;
    double FastestBytesPerSecond = 0;
    // The share of the measured time the writer thread spent writing.
    double WriterBusy = 0;
};

}  // namespace

class FrameBudgetTest : public QObject {
    Q_OBJECT

private slots:
    // The M3 criterion of design.md section 13: the GUI thread spends at most
    // 4 ms per frame while ten sessions receive 1 MB/s each. One item shows
    // session 0; the other nine are only written to.
    void GuiThreadFrameCostStaysInBudget() {
        const Scenario scenario{.Name = "gated",
                                .Columns = GATED_COLUMNS,
                                .Rows = GATED_ROWS,
                                .VisibleCount = 1,
                                .GridColumns = 1};
        Result result;
        Measure(scenario, result);
        if (QTest::currentTestFailed()) {
            return;
        }
#ifndef NDEBUG
        // An unoptimized build with lock checks is several times slower than
        // the product (p95 of ~6.5 ms against ~3.4 ms in release): the budget
        // is a property of the optimized build, so here the time is printed
        // and not checked.
        qInfo(
            "frame budget [gated]: unoptimized build, p95 %.3f ms against "
            "the %.1f ms budget is not checked",
            result.Gui.P95, P95_BUDGET_MS);
#else
        if (qEnvironmentVariable("ROOST_FRAME_BUDGET") == "report") {
            // A shared CI runner is a slower, noisy virtual machine: the time
            // is printed there, and the budget is checked on a developer's
            // machine (see ci.yml).
            qInfo(
                "frame budget [gated]: ROOST_FRAME_BUDGET=report, p95 %.3f ms "
                "and busy %.3f ms against the %.1f ms budget are not checked",
                result.Gui.P95, result.Busy, P95_BUDGET_MS);
            return;
        }
        QVERIFY2(result.Busy >= 0,
                 "no GUI-thread CPU time on this platform: the whole-thread "
                 "check cannot run");
        QVERIFY2(result.Busy <= BUSY_BUDGET_MS,
                 qPrintable(QString("mean GUI busy %1 ms per frame is over "
                                    "the budget of %2 ms")
                                .arg(result.Busy)
                                .arg(BUSY_BUDGET_MS)));
        QVERIFY2(result.Gui.P95 <= P95_BUDGET_MS,
                 qPrintable(QString("p95 %1 ms is over the budget of %2 ms "
                                    "(design.md section 9)")
                                .arg(result.Gui.P95)
                                .arg(P95_BUDGET_MS)));
#endif
    }

    // NOT GATED: ten visible items, 100x30 each, the same writer. The time is
    // printed for information and never fails the test; the writer falling
    // behind still does. If this number matters one day, the answer is the
    // glyph atlas of design.md section 13, M3 item 3.
    void TenVisibleItemsAreReportedNotGated() {
        const Scenario scenario{.Name = "ten visible items (not gated)",
                                .Columns = GRID_TERMINAL_COLUMNS,
                                .Rows = GRID_TERMINAL_ROWS,
                                .VisibleCount = SESSION_COUNT,
                                .GridColumns = GRID_COLUMNS};
        Result result;
        Measure(scenario, result);
        if (QTest::currentTestFailed()) {
            return;
        }
        qInfo(
            "frame budget [%s]: NOT GATED, informational: p95 %.3f ms "
            "against the %.1f ms budget is not checked",
            scenario.Name, result.Gui.P95, P95_BUDGET_MS);
    }

private:
    // Runs one scenario and prints what it measured. It fails on a broken
    // setup or on a writer that falls behind, and leaves the time check to
    // the caller.
    void Measure(const Scenario& scenario, Result& result) {
        // The threaded loop, not the basic one, which draws in the GUI thread
        // and would measure rendering as well.
        QCOMPARE(qEnvironmentVariable("QSG_RENDER_LOOP"), QString("threaded"));

        QQuickWindow window;
        FrameProbe probe(&window);
        Feed feed;
        // Runs first on every exit, including the early returns of a failed
        // QVERIFY, because it is declared after what it tears down.
        //  * The writer stops: no more chunks, no more Updated, and the
        //    ones in flight are dropped, so nothing asks the window for a
        //    frame while it goes away.
        //  * Hiding the window under the threaded loop waits for the render
        //    thread to stop, so after this no handler of the probe can run
        //    (the probe is destroyed before the window).
        //  * The window's posted events are dropped before it is destroyed.
        //    Without this, ~QQuickWindow sometimes waited forever for the
        //    software threaded loop's render thread
        //    (QSGSoftwareThreadedRenderLoop::windowDestroyed): 2 of 40 runs
        //    of a failure after the warm-up under load, 0 of 80 with it.
        const auto teardown = qScopeGuard([&] {
            feed.Stop();
            QObject::disconnect(&feed, &Feed::Updated, &window, nullptr);
            window.hide();
            QCoreApplication::removePostedEvents(&window);
        });
        std::vector<BenchItem*> items;

        const int rowCount =
            (scenario.VisibleCount + scenario.GridColumns - 1) /
            scenario.GridColumns;
        qreal cellWidth = 0;
        qreal cellHeight = 0;
        for (int index = 0; index < SESSION_COUNT; ++index) {
            auto created =
                TerminalSession::Create(scenario.Columns, scenario.Rows);
            if (!created) {
                QFAIL(qPrintable(created.error()));
            }
            std::shared_ptr<TerminalSession> session = std::move(*created);
            const bool shown = index < scenario.VisibleCount;
            feed.Add(session, index, scenario.Rows, shown);
            if (!shown) {
                continue;
            }

            auto* item = new BenchItem(window.contentItem(), probe);
            item->setSession(session);
            cellWidth = item->cellWidth();
            cellHeight = item->cellHeight();
            const auto width = scenario.Columns * cellWidth;
            const auto height = scenario.Rows * cellHeight;
            item->setSize(QSizeF(width, height));
            item->setPosition(QPointF((index % scenario.GridColumns) * width,
                                      (index / scenario.GridColumns) * height));
            items.push_back(item);
        }
        window.resize(static_cast<int>(scenario.GridColumns * scenario.Columns *
                                       cellWidth),
                      static_cast<int>(rowCount * scenario.Rows * cellHeight));

        // The update path of SingleSession::OnUpdated: acknowledge, then
        // update(). It is timed, since it is GUI-thread work. The window is
        // the context: queued Updated events die with it, so none reaches
        // this lambda after the feed and the items are gone.
        connect(&feed, &Feed::Updated, &window, [&](int index) {
            const auto start = NowNs();
            feed.Acknowledge(static_cast<std::size_t>(index));
            items[static_cast<std::size_t>(index)]->update();
            probe.AddSlotNs(NowNs() - start);
        });

        QSignalSpy swapped(&window, &QQuickWindow::frameSwapped);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(swapped.count() > 0 || swapped.wait(FIRST_FRAME_TIMEOUT_MS));

        // The scene graph is up. The render loop must be the threaded one,
        // on the software backend, and the render thread a different one.
        QCOMPARE(window.rendererInterface()->graphicsApi(),
                 QSGRendererInterface::Software);

        // Every pending flag starts set: the first acknowledgement is the
        // GUI's own, as for a channel IoThread has just opened.
        for (std::size_t index = 0; index < feed.Size(); ++index) {
            feed.Acknowledge(index);
        }
        feed.Start();
        RunFor(WARMUP_MS);

        std::vector<std::int64_t> before;
        for (std::size_t index = 0; index < feed.Size(); ++index) {
            before.push_back(feed.TotalWritten(index));
        }
        const auto busyBefore = feed.BusyNs();
        const auto framesBefore = swapped.count();
        const auto cpuBefore = ThreadCpuNs();
        probe.StartRecording();
        QElapsedTimer clock;
        clock.start();
        RunFor(MEASURE_MS);
        const auto cpuAfter = ThreadCpuNs();
        const auto seconds = static_cast<double>(clock.nsecsElapsed()) / 1e9;
        const auto recorded = probe.StopRecording();
        const auto framesSwapped = swapped.count() - framesBefore;
        const auto busyNs = feed.BusyNs() - busyBefore;

        std::vector<double> rates;
        for (std::size_t index = 0; index < feed.Size(); ++index) {
            rates.push_back(
                static_cast<double>(feed.TotalWritten(index) - before[index]) /
                seconds);
        }
        feed.Stop();

        QVERIFY2(probe.RenderThreadSeen(),
                 "beforeRendering ran in the GUI thread: the render loop is "
                 "not threaded");
        QVERIFY2(recorded.size() > 10, "too few frames to measure anything");

        const auto column = [&](double Frame::* field) {
            std::vector<double> values;
            for (const auto& frame : recorded) {
                values.push_back(frame.*field);
            }
            std::ranges::sort(values);
            return values;
        };
        std::vector<double> frames;
        for (const auto& frame : recorded) {
            frames.push_back(frame.Total());
        }
        std::ranges::sort(frames);
        result.Gui = {.P50 = Percentile(frames, 0.50),
                      .P95 = Percentile(frames, 0.95),
                      .P99 = Percentile(frames, 0.99),
                      .Max = frames.back()};
        result.MeanSync = std::accumulate(recorded.begin(), recorded.end(), 0.0,
                                          [](double sum, const Frame& frame) {
                                              return sum + frame.Sync;
                                          }) /
                          static_cast<double>(recorded.size());
        if (cpuBefore >= 0 && cpuAfter >= 0) {
            result.CpuPerFrame = static_cast<double>(cpuAfter - cpuBefore) /
                                 1e6 / static_cast<double>(recorded.size());
            result.Busy = result.CpuPerFrame + result.MeanSync;
        }
        result.Seconds = seconds;
        result.FrameCount = frames.size();
        result.Swapped = static_cast<int>(framesSwapped);
        result.SlowestBytesPerSecond = *std::ranges::min_element(rates);
        result.FastestBytesPerSecond = *std::ranges::max_element(rates);
        result.WriterBusy = static_cast<double>(busyNs) / 1e9 / seconds;

        const auto* name = scenario.Name;
        qInfo(
            "frame budget [%s]: %zu frames in %.2f s (%.1f frames/s, %d "
            "swapped), %d of %d sessions shown at %dx%d",
            name, result.FrameCount, seconds,
            static_cast<double>(result.FrameCount) / seconds, result.Swapped,
            scenario.VisibleCount, SESSION_COUNT, scenario.Columns,
            scenario.Rows);
        qInfo(
            "frame budget [%s]: GUI-thread ms per frame: p50 %.3f  p95 %.3f "
            " p99 %.3f  max %.3f",
            name, result.Gui.P50, result.Gui.P95, result.Gui.P99,
            result.Gui.Max);
        qInfo(
            "frame budget [%s]: mean GUI busy per frame %.3f ms (thread CPU "
            "%.3f + sync %.3f) against the %.1f ms budget",
            name, result.Busy, result.CpuPerFrame, result.MeanSync,
            BUSY_BUDGET_MS);
        qInfo(
            "frame budget [%s]: p50 by phase: polish %.3f  sync %.3f  "
            "slots %.3f",
            name, Percentile(column(&Frame::Polish), 0.5),
            Percentile(column(&Frame::Sync), 0.5),
            Percentile(column(&Frame::Slots), 0.5));
        qInfo(
            "frame budget [%s]: per session %.0f..%.0f bytes/s (%d "
            "sessions, %.0f bytes/s in all); writer thread busy %.1f%%",
            name, result.SlowestBytesPerSecond, result.FastestBytesPerSecond,
            SESSION_COUNT, std::accumulate(rates.begin(), rates.end(), 0.0),
            result.WriterBusy * 100);

        // The streams must really have been consumed: a test that stalls the
        // writer would pass trivially, and a writer that cannot keep up with
        // the sessions is a finding of its own (design.md section 14). On a
        // shared runner the rate is only printed above and checked against
        // the floor (see STREAM_REPORT_FLOOR).
        const bool report =
            qEnvironmentVariable("ROOST_FRAME_BUDGET") == "report";
        const auto required = STREAM_BYTES_PER_SECOND *
                              (report ? STREAM_REPORT_FLOOR : STREAM_TOLERANCE);
        QVERIFY2(result.SlowestBytesPerSecond >= required,
                 qPrintable(QString("the writer fell behind: %1 bytes/s for "
                                    "the slowest session, %2 required")
                                .arg(result.SlowestBytesPerSecond)
                                .arg(required)));
    }
};

QTEST_MAIN(FrameBudgetTest)
#include "frame_budget_test.moc"
