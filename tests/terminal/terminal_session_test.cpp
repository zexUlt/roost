// A session's terminal is reached only under its lock: what the render thread
// sees after the IO thread wrote, in one thread and in two; a restored
// session; and the debug check that a token belongs to its session.

#include "terminal/terminal_session.hpp"
#include "core/vt_render_state.hpp"
#include "core/vt_terminal.hpp"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QTest>

#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>

using roost::TerminalSession;
using roost::VtRenderState;
using roost::VtTerminal;

namespace {

constexpr quint16 COLUMNS = 80;
constexpr quint16 ROWS = 24;
constexpr qsizetype STREAM_BYTES = 24 << 20;
constexpr qsizetype MAX_CHUNK = 256;
// An ASan build renders about 180 frames over the stream, a dev build more.
constexpr int MIN_OVERLAPPED_FRAMES = 50;

std::unique_ptr<TerminalSession> NewSession() {
    auto session = TerminalSession::Create(COLUMNS, ROWS);
    if (!session) {
        qFatal("%s", qPrintable(session.error()));
    }
    return std::move(*session);
}

// Renders the way the render thread does: begin under the lock, the rest
// without it.
QList<VtRenderState::Row> Render(TerminalSession& session) {
    {
        const auto lock = session.Lock();
        const auto begun = session.BeginRenderUpdate(lock);
        if (!begun) {
            qFatal("%s", qPrintable(begun.error()));
        }
    }
    session.EndRenderUpdate();
    return session.RenderState().Rows();
}

QString RowText(const VtRenderState::Row& row) {
    QString text;
    for (const auto& cell : row.Cells) {
        text += cell.Text;
    }
    return text.trimmed();
}

QStringList Texts(const QList<VtRenderState::Row>& rows) {
    QStringList texts;
    for (const auto& row : rows) {
        texts.append(RowText(row));
    }
    return texts;
}

// Lines of text in several scripts, with some styling, long enough to scroll.
QByteArray Stream(std::uint32_t seed, qsizetype size) {
    std::mt19937 random(seed);
    const QList<QByteArray> words = {"hello",
                                     "world",
                                     "\xe6\xbc\xa2\xe5\xad\x97",
                                     "caf\xc3\xa9",
                                     "\x1b[1mbold\x1b[0m",
                                     "\x1b[38;2;1;2;3mrgb\x1b[0m"};
    QByteArray stream;
    while (stream.size() < size) {
        const int count = static_cast<int>(random() % 12) + 1;
        for (int i = 0; i < count; ++i) {
            const auto word =
                random() % static_cast<std::uint32_t>(words.size());
            stream += words[static_cast<qsizetype>(word)] + ' ';
        }
        stream += "\r\n";
    }
    return stream;
}

// Chunks of 1 to MAX_CHUNK bytes, so that many writes fall between frames.
std::vector<QByteArray> SmallChunks(const QByteArray& stream,
                                    std::uint32_t seed) {
    std::mt19937 random(seed);
    std::vector<QByteArray> chunks;
    qsizetype at = 0;
    while (at < stream.size()) {
        const auto length = std::min<qsizetype>(
            static_cast<qsizetype>(random() % MAX_CHUNK) + 1,
            stream.size() - at);
        chunks.push_back(stream.mid(at, length));
        at += length;
    }
    return chunks;
}

// Used only where debug-build checks run.
[[maybe_unused]] QStringList& Failures() {
    static QStringList failures;
    return failures;
}

[[maybe_unused]] void RecordFailure(const char* message) {
    Failures().append(QString::fromUtf8(message));
}

}  // namespace

class TerminalSessionTest : public QObject {
    Q_OBJECT

private slots:
    void WrittenTextIsRendered() {
        const auto session = NewSession();
        {
            const auto lock = session->Lock();
            session->Write(lock, "hello \xe6\xbc\xa2\r\nsecond");
        }
        const auto texts = Texts(Render(*session));
        QCOMPARE(texts.size(), ROWS);
        QCOMPARE(texts[0], QStringLiteral("hello 漢"));
        QCOMPARE(texts[1], QStringLiteral("second"));
    }

    void ResizeIsRendered() {
        const auto session = NewSession();
        {
            const auto lock = session->Lock();
            QVERIFY(session->Resize(lock, 20, 8));
        }
        QCOMPARE(Render(*session).size(), 8);
        QCOMPARE(session->RenderState().Columns(), quint16{20});
    }

    void RenderingWhileAnotherThreadWrites() {
        const auto session = NewSession();
        const auto stream = Stream(1, STREAM_BYTES);
        const auto chunks = SmallChunks(stream, 2);

        std::atomic<qsizetype> written = 0;
        std::atomic<bool> done = false;
        std::thread writer([&] {
            for (const auto& chunk : chunks) {
                {
                    const auto lock = session->Lock();
                    session->Write(lock, chunk);
                }
                written += chunk.size();
                std::this_thread::yield();
            }
            done = true;
        });

        // No QCOMPARE or QVERIFY while the writer lives: a failed one returns
        // from the slot and destroys what the thread still uses.
        QStringList problems;
        int frames = 0;
        int overlapped = 0;
        qsizetype seen = 0;
        while (!done) {
            const auto rows = Render(*session);
            if (rows.size() != ROWS || rows[0].Cells.size() != COLUMNS) {
                problems.append(
                    QStringLiteral("a frame of %1 rows").arg(rows.size()));
            }
            session->CleanRenderState();
            ++frames;
            // The writer wrote between this frame and the previous one.
            const auto now = written.load();
            if (now != seen) {
                ++overlapped;
                seen = now;
            }
        }
        writer.join();
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
        QVERIFY2(overlapped >= MIN_OVERLAPPED_FRAMES,
                 qPrintable(QStringLiteral("%1 of %2 frames overlapped")
                                .arg(overlapped)
                                .arg(frames)));

        // The same stream fed to a terminal at once ends in the same place.
        auto reference = VtTerminal::Create(COLUMNS, ROWS);
        QVERIFY(reference.has_value());
        reference->Write(stream);
        VtRenderState expected;
        QVERIFY(expected.BeginUpdate(*reference).has_value());
        expected.EndUpdate();

        QCOMPARE(Texts(Render(*session)), Texts(expected.Rows()));
        const auto lock = session->Lock();
        const auto dump = session->Dump(lock);
        const auto expectedDump = reference->Dump();
        QVERIFY(dump.has_value() && expectedDump.has_value());
        QVERIFY(*dump == *expectedDump);
    }

    void RestoredSessionRendersTheSame() {
        auto terminal = VtTerminal::Create(COLUMNS, ROWS);
        QVERIFY(terminal.has_value());
        terminal->Write("snap \xe6\xbc\xa2\x1b[1;31m red\x1b[0m\r\nline two");
        const auto snapshot = terminal->Snapshot();
        QVERIFY(snapshot.has_value());

        auto restored = TerminalSession::FromSnapshot(*snapshot);
        QVERIFY(restored.has_value());
        const auto original = NewSession();
        {
            const auto lock = original->Lock();
            original->Write(
                lock, "snap \xe6\xbc\xa2\x1b[1;31m red\x1b[0m\r\nline two");
        }
        const auto texts = Texts(Render(**restored));
        QCOMPARE(texts, Texts(Render(*original)));
        QCOMPARE(texts[0], QStringLiteral("snap 漢 red"));
        QCOMPARE(texts[1], QStringLiteral("line two"));
    }

    void RestoringGarbageFails() {
        QVERIFY(!TerminalSession::FromSnapshot("not a snapshot").has_value());
    }

    void TokenOfAnotherSessionIsRefused() {
#ifdef NDEBUG
        QSKIP("the check is compiled out of release builds");
#else
        Failures().clear();
        const auto& caught = Failures();
        const auto previous = TerminalSession::SetCheckHandler(&RecordFailure);

        const auto mine = NewSession();
        const auto other = NewSession();
        {
            const auto lock = other->Lock();
            mine->Write(lock, "must not land");
            QVERIFY(!mine->Resize(lock, 10, 10).has_value());
            QVERIFY(!mine->BeginRenderUpdate(lock).has_value());
            QVERIFY(!mine->Dump(lock).has_value());
        }
        QCOMPARE(caught.size(), 4);
        for (const auto& message : caught) {
            QVERIFY2(message.contains(QStringLiteral("another session")),
                     qPrintable(message));
        }
        // Nothing was written to the session that was given a foreign token.
        QCOMPARE(RowText(Render(*mine)[0]), QString());

        // The right token still works.
        {
            const auto lock = mine->Lock();
            mine->Write(lock, "fine");
        }
        QCOMPARE(caught.size(), 4);
        QCOMPARE(RowText(Render(*mine)[0]), QStringLiteral("fine"));

        TerminalSession::SetCheckHandler(previous);
#endif
    }
};

QTEST_GUILESS_MAIN(TerminalSessionTest)
#include "terminal_session_test.moc"
