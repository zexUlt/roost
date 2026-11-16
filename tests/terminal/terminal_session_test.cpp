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
constexpr qsizetype STREAM_BYTES = 1 << 20;

std::unique_ptr<TerminalSession> NewSession() {
    auto session = TerminalSession::Create(COLUMNS, ROWS);
    if (!session) {
        qFatal("%s", qPrintable(session.error()));
    }
    return std::move(*session);
}

// Renders the way the render thread does: begin under the lock, the rest
// without it.
QList<VtRenderState::Row> Render(TerminalSession& session,
                                 VtRenderState& state) {
    {
        const auto lock = session.Lock();
        const auto begun = session.BeginRenderUpdate(lock, state);
        if (!begun) {
            qFatal("%s", qPrintable(begun.error()));
        }
    }
    state.EndUpdate();
    return state.Rows();
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
            stream += words[random() % words.size()] + ' ';
        }
        stream += "\r\n";
    }
    return stream;
}

std::vector<QByteArray> Chunks(const QByteArray& stream, std::uint32_t seed) {
    std::mt19937 random(seed);
    std::vector<QByteArray> chunks;
    qsizetype at = 0;
    while (at < stream.size()) {
        const auto length = std::min<qsizetype>(
            static_cast<qsizetype>(random() % 4096) + 1, stream.size() - at);
        chunks.push_back(stream.mid(at, length));
        at += length;
    }
    return chunks;
}

QStringList& Failures() {
    static QStringList failures;
    return failures;
}

void RecordFailure(const char* message) {
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
        VtRenderState state;
        const auto texts = Texts(Render(*session, state));
        QCOMPARE(texts.size(), ROWS);
        QCOMPARE(texts[0], QStringLiteral("hello 漢"));
        QCOMPARE(texts[1], QStringLiteral("second"));
    }

    void ResizeIsRendered() {
        const auto session = NewSession();
        VtRenderState state;
        {
            const auto lock = session->Lock();
            QVERIFY(session->Resize(lock, 20, 8));
        }
        QCOMPARE(Render(*session, state).size(), 8);
        QCOMPARE(state.Columns(), quint16{20});
    }

    void RenderingWhileAnotherThreadWrites() {
        const auto session = NewSession();
        const auto stream = Stream(1, STREAM_BYTES);
        const auto chunks = Chunks(stream, 2);

        std::atomic<bool> done = false;
        std::thread writer([&] {
            for (const auto& chunk : chunks) {
                const auto lock = session->Lock();
                session->Write(lock, chunk);
            }
            done = true;
        });

        VtRenderState state;
        int frames = 0;
        while (!done) {
            const auto rows = Render(*session, state);
            QCOMPARE(rows.size(), ROWS);
            QCOMPARE(rows[0].Cells.size(), COLUMNS);
            state.Clean();
            ++frames;
        }
        writer.join();
        QVERIFY(frames > 0);

        // The same stream fed to a terminal at once ends in the same place.
        auto reference = VtTerminal::Create(COLUMNS, ROWS);
        QVERIFY(reference.has_value());
        reference->Write(stream);
        VtRenderState expected;
        QVERIFY(expected.BeginUpdate(*reference).has_value());
        expected.EndUpdate();

        QCOMPARE(Texts(Render(*session, state)), Texts(expected.Rows()));
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
        VtRenderState restoredState;
        VtRenderState originalState;
        const auto texts = Texts(Render(**restored, restoredState));
        QCOMPARE(texts, Texts(Render(*original, originalState)));
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
            VtRenderState state;
            QVERIFY(!mine->BeginRenderUpdate(lock, state).has_value());
            QVERIFY(!mine->Dump(lock).has_value());
        }
        QCOMPARE(caught.size(), 4);
        for (const auto& message : caught) {
            QVERIFY2(message.contains(QStringLiteral("another session")),
                     qPrintable(message));
        }
        // Nothing was written to the session that was given a foreign token.
        VtRenderState state;
        QCOMPARE(RowText(Render(*mine, state)[0]), QString());

        // The right token still works.
        {
            const auto lock = mine->Lock();
            mine->Write(lock, "fine");
        }
        QCOMPARE(caught.size(), 4);
        QCOMPARE(RowText(Render(*mine, state)[0]), QStringLiteral("fine"));

        TerminalSession::SetCheckHandler(previous);
#endif
    }
};

QTEST_GUILESS_MAIN(TerminalSessionTest)
#include "terminal_session_test.moc"
