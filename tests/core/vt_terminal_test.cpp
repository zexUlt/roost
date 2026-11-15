// The daemon's terminal ends up where a terminal fed the whole stream at once
// does, however the stream is cut; a snapshot restores it exactly, even when
// taken inside an escape sequence; queries get one answer each.

#include "core/vt_terminal.hpp"

#include <QByteArray>
#include <QList>
#include <QTest>

#include <cstdint>
#include <expected>
#include <random>

using roost::VtTerminal;

namespace {

constexpr quint16 COLUMNS = 80;
constexpr quint16 ROWS = 24;

// What a TUI writes: text in several scripts, cursor addressing, styles,
// scroll regions, the alternate screen, titles, hyperlinks and queries.
QByteArray Stream(std::uint32_t seed, int pieces) {
    std::mt19937 random(seed);
    const auto upTo = [&random](int last) {
        return std::uniform_int_distribution<int>(0, last)(random);
    };
    const QList<QByteArray> texts = {
        "hello",
        "привет",
        "漢字",
        "🙂",
        "é",
        "tab\there",
        "x\by",
        "a long line that goes past the right edge of an eighty column "
        "screen and wraps onto the next row of it",
    };
    QByteArray stream;
    for (int i = 0; i < pieces; ++i) {
        switch (upTo(13)) {
            case 0:
            case 1:
            case 2:
                stream += texts[upTo(static_cast<int>(texts.size()) - 1)];
                break;
            case 3:
                stream += "\r\n";
                break;
            case 4:
                stream += "\x1b[" + QByteArray::number(upTo(30)) + ";" +
                          QByteArray::number(upTo(90)) + "H";
                break;
            case 5:
                stream +=
                    QList<QByteArray>{"\x1b[0m",        "\x1b[1;31m",
                                      "\x1b[4;42m",     "\x1b[38;2;10;200;30m",
                                      "\x1b[48;5;123m", "\x1b[7m"}[upTo(5)];
                break;
            case 6:
                stream += QList<QByteArray>{"\x1b[K",  "\x1b[1J", "\x1b[2K",
                                            "\x1b[3P", "\x1b[2@", "\x1b[L",
                                            "\x1b[M"}[upTo(6)];
                break;
            case 7:
                stream += QList<QByteArray>{"\x1b[5;20r", "\x1b[r", "\x1bM",
                                            "\x1b[3S", "\x1b[2T"}[upTo(4)];
                break;
            case 8:
                stream += upTo(1) == 0 ? "\x1b[?1049h" : "\x1b[?1049l";
                break;
            case 9:
                stream += "\x1b]0;title " + QByteArray::number(i) + "\x07";
                break;
            case 10:
                stream += "\x1b]8;;https://example.com/" +
                          QByteArray::number(i) + "\x1b\\link\x1b]8;;\x1b\\";
                break;
            case 11:
                stream += QList<QByteArray>{"\x1b[?25l",
                                            "\x1b[?25h",
                                            "\x1b[?2004h",
                                            "\x1b[?7l",
                                            "\x1b[?7h",
                                            "\x1b[4h",
                                            "\x1b[4l",
                                            "\x1b"
                                            "7",
                                            "\x1b"
                                            "8"}[upTo(8)];
                break;
            case 12:
                stream += QList<QByteArray>{"\x1b[c", "\x1b[6n", "\x1b[>c",
                                            "\x1bP+q544e\x1b\\"}[upTo(3)];
                break;
            default:
                stream += "\x1b[2J\x1b[H";
                break;
        }
    }
    return stream;
}

// The stream cut at random places, single bytes included.
QList<QByteArrayView> Cut(QByteArrayView stream, std::uint32_t seed) {
    std::mt19937 random(seed);
    std::uniform_int_distribution<qsizetype> length(1, 64);
    QList<QByteArrayView> pieces;
    for (qsizetype at = 0; at < stream.size();) {
        const qsizetype size = std::min(length(random), stream.size() - at);
        pieces.append(stream.sliced(at, size));
        at += size;
    }
    return pieces;
}

// QVERIFY2 builds its message even when the check passes.
template <typename TValue>
QString ErrorOf(const std::expected<TValue, QString>& result) {
    return result ? QString() : result.error();
}

VtTerminal Make() {
    auto terminal = VtTerminal::Create(COLUMNS, ROWS);
    if (!terminal) {
        qFatal("%s", qPrintable(ErrorOf(terminal)));
    }
    return std::move(*terminal);
}

QByteArray SnapshotOf(const VtTerminal& terminal) {
    auto snapshot = terminal.Snapshot();
    if (!snapshot) {
        qFatal("%s", qPrintable(ErrorOf(snapshot)));
    }
    return *snapshot;
}

QByteArray DumpOf(const VtTerminal& terminal) {
    auto dump = terminal.Dump();
    if (!dump) {
        qFatal("%s", qPrintable(dump.error()));
    }
    return *dump;
}

// Both screens, the active one first. Mode 47 switches screens without
// clearing or moving anything, so the two terminals compared go through the
// same switches.
QByteArray Screens(VtTerminal& terminal) {
    QByteArray screens = DumpOf(terminal);
    terminal.Write("\x1b[?47l");
    screens += "\n--- primary\n" + DumpOf(terminal);
    terminal.Write("\x1b[?47h");
    screens += "\n--- alternate\n" + DumpOf(terminal);
    return screens;
}

}  // namespace

class VtTerminalTest : public QObject {
    Q_OBJECT

private slots:
    void ACutStreamEndsWhereTheWholeOneDoes();
    void AStreamFedByteByByteEndsWhereTheWholeOneDoes();
    void ASnapshotRestoresTheTerminal();
    void ASnapshotCutInASequenceIsFinishedByTheRest();
    void ALongSequenceCutAnywhereIsFinishedByTheRest();
    void ASequenceOverTheLimitIsSnapshottedOnceItEnds();
    void ADumpTellsApartWhatTheScreenHides();
    void ClearingTheHandlerSilencesTheTerminal();
    void AHandlerMayClearItself();
    void AMovedTerminalStillAnswers();
    void AQueryGetsOneAnswer();
    void AQuerySplitAcrossWritesGetsOneAnswer();
    void ResizeChangesTheSize();
    void ResizeAnswersASizeReportAskedFor();
    void ASizeOutOfRangeIsRefused();
    void JunkIsNotASnapshot();
    void ATruncatedSnapshotIsRefused();
    void BytesAfterASnapshotAreRefused();
};

void VtTerminalTest::ACutStreamEndsWhereTheWholeOneDoes() {
    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
        const QByteArray stream = Stream(seed, 3'000);
        VtTerminal whole = Make();
        whole.Write(stream);
        VtTerminal cut = Make();
        for (const QByteArrayView piece : Cut(stream, seed)) {
            cut.Write(piece);
        }
        QCOMPARE(Screens(cut), Screens(whole));
    }
}

void VtTerminalTest::AStreamFedByteByByteEndsWhereTheWholeOneDoes() {
    const QByteArray stream = Stream(42, 2'000);
    VtTerminal whole = Make();
    whole.Write(stream);
    VtTerminal bytes = Make();
    for (qsizetype i = 0; i < stream.size(); ++i) {
        bytes.Write(QByteArrayView(stream).sliced(i, 1));
    }
    QCOMPARE(Screens(bytes), Screens(whole));
}

void VtTerminalTest::ASnapshotRestoresTheTerminal() {
    VtTerminal original = Make();
    original.Write(Stream(7, 5'000));

    auto restored = VtTerminal::FromSnapshot(SnapshotOf(original));
    QVERIFY2(restored, qPrintable(ErrorOf(restored)));
    QCOMPARE(DumpOf(*restored), DumpOf(original));
    // A restored terminal can be snapshotted in turn.
    auto again = VtTerminal::FromSnapshot(SnapshotOf(*restored));
    QVERIFY2(again, qPrintable(ErrorOf(again)));

    // And they go on as the original does.
    const QByteArray more = Stream(8, 500);
    for (VtTerminal* terminal : {&original, &*restored, &*again}) {
        terminal->Write(more);
    }
    const QByteArray expected = Screens(original);
    QCOMPARE(Screens(*restored), expected);
    QCOMPARE(Screens(*again), expected);
}

void VtTerminalTest::ASnapshotCutInASequenceIsFinishedByTheRest() {
    const QByteArray stream = Stream(3, 400);
    VtTerminal whole = Make();
    whole.Write(stream);
    const QByteArray expected = Screens(whole);

    // Every cut point, so every state of the parser and the UTF-8 decoder.
    for (qsizetype at = 0; at <= stream.size(); ++at) {
        VtTerminal daemon = Make();
        daemon.Write(QByteArrayView(stream).first(at));
        auto window = VtTerminal::FromSnapshot(SnapshotOf(daemon));
        QVERIFY2(window, qPrintable(ErrorOf(window)));
        window->Write(QByteArrayView(stream).sliced(at));
        const QByteArray actual = Screens(*window);
        if (actual != expected) {
            qWarning("cut at byte %lld of %lld", static_cast<long long>(at),
                     static_cast<long long>(stream.size()));
            QCOMPARE(actual, expected);
        }
    }
}

void VtTerminalTest::ALongSequenceCutAnywhereIsFinishedByTheRest() {
    // A clipboard write far longer than the sequences of Stream(). Its tail,
    // if lost, would come out as text.
    const QByteArray payload(100'000, 'A');
    const QByteArray stream =
        "before \x1b]52;c;" + payload + "\x1b\\ after\r\n";
    VtTerminal whole = Make();
    whole.Write(stream);
    const QByteArray expected = Screens(whole);
    QVERIFY(!expected.contains("AAAA"));

    for (const qsizetype at :
         {qsizetype{9}, qsizetype{12}, qsizetype{50'000}, stream.size() - 30,
          stream.size() - 20, stream.size() - 15, stream.size() - 9}) {
        VtTerminal daemon = Make();
        daemon.Write(QByteArrayView(stream).first(at));
        auto window = VtTerminal::FromSnapshot(SnapshotOf(daemon));
        QVERIFY2(window, qPrintable(ErrorOf(window)));
        window->Write(QByteArrayView(stream).sliced(at));
        QVERIFY2(Screens(*window) == expected,
                 qPrintable(QStringLiteral("cut at byte %1").arg(at)));
    }
}

void VtTerminalTest::ASequenceOverTheLimitIsSnapshottedOnceItEnds() {
    VtTerminal terminal = Make();
    terminal.Write("\x1b]52;c;" +
                   QByteArray(VtTerminal::MAX_CONTINUATION_BYTES + 16, 'A'));
    QVERIFY(!terminal.Snapshot());
    terminal.Write(
        "\x07"
        "after");
    auto restored = VtTerminal::FromSnapshot(SnapshotOf(terminal));
    QVERIFY2(restored, qPrintable(ErrorOf(restored)));
    QCOMPARE(DumpOf(*restored), DumpOf(terminal));
}

void VtTerminalTest::ADumpTellsApartWhatTheScreenHides() {
    // Same text, different state: the comparisons above would miss these.
    for (const QByteArray& extra :
         {QByteArray("\x1b[1;1H"), QByteArray("\x1b[?2004h"),
          QByteArray("\x1b[1;31m"), QByteArray("\x1b(0"),
          QByteArray("\x1b[3g")}) {
        VtTerminal plain = Make();
        plain.Write("text");
        VtTerminal other = Make();
        other.Write("text" + extra);
        QVERIFY2(DumpOf(other) != DumpOf(plain),
                 extra.toPercentEncoding().constData());
    }
    // Setting the region homes the cursor; with the cursor put back, only
    // the region differs.
    {
        VtTerminal plain = Make();
        plain.Write("text\x1b[1;5H");
        VtTerminal other = Make();
        other.Write("text\x1b[5;10r\x1b[1;5H");
        QVERIFY(DumpOf(other) != DumpOf(plain));
    }
}

void VtTerminalTest::ClearingTheHandlerSilencesTheTerminal() {
    VtTerminal terminal = Make();
    int answers = 0;
    terminal.SetResponseHandler([&answers](QByteArrayView) { ++answers; });
    terminal.SetResponseHandler(nullptr);
    terminal.Write("\x1b[c\x1b[6n");
    QCOMPARE(answers, 0);
}

void VtTerminalTest::AHandlerMayClearItself() {
    VtTerminal terminal = Make();
    int answers = 0;
    terminal.SetResponseHandler([&terminal, &answers](QByteArrayView) {
        ++answers;
        terminal.SetResponseHandler(nullptr);
    });
    terminal.Write("\x1b[c\x1b[6n");
    terminal.Write("\x1b[c");
    QCOMPARE(answers, 1);
}

void VtTerminalTest::AMovedTerminalStillAnswers() {
    QList<QByteArray> answers;
    VtTerminal first = Make();
    first.SetResponseHandler([&answers](QByteArrayView answer) {
        answers.append(answer.toByteArray());
    });
    VtTerminal second = std::move(first);
    second.Write("\x1b[c");
    VtTerminal third = Make();
    third = std::move(second);
    third.Write("\x1b[6n");
    QCOMPARE(answers, (QList<QByteArray>{"\x1b[?62;22c", "\x1b[1;1R"}));
}

void VtTerminalTest::AQueryGetsOneAnswer() {
    VtTerminal terminal = Make();
    QList<QByteArray> answers;
    terminal.SetResponseHandler([&answers](QByteArrayView answer) {
        answers.append(answer.toByteArray());
    });

    terminal.Write("\x1b[c");
    terminal.Write("abc\x1b[5;7H\x1b[6n");
    QCOMPARE(answers, (QList<QByteArray>{"\x1b[?62;22c", "\x1b[5;7R"}));
}

void VtTerminalTest::AQuerySplitAcrossWritesGetsOneAnswer() {
    VtTerminal terminal = Make();
    QList<QByteArray> answers;
    terminal.SetResponseHandler([&answers](QByteArrayView answer) {
        answers.append(answer.toByteArray());
    });

    terminal.Write("\x1b");
    terminal.Write("[");
    QVERIFY(answers.isEmpty());
    terminal.Write("6n");
    QCOMPARE(answers, (QList<QByteArray>{"\x1b[1;1R"}));
}

void VtTerminalTest::ResizeChangesTheSize() {
    VtTerminal terminal = Make();
    QList<QByteArray> answers;
    terminal.SetResponseHandler([&answers](QByteArrayView answer) {
        answers.append(answer.toByteArray());
    });

    const auto resized = terminal.Resize(100, 30);
    QVERIFY2(resized, qPrintable(ErrorOf(resized)));
    // The cursor stops at the bottom right corner.
    terminal.Write("\x1b[999;999H\x1b[6n");
    QCOMPARE(answers, (QList<QByteArray>{"\x1b[30;100R"}));
}

void VtTerminalTest::ResizeAnswersASizeReportAskedFor() {
    VtTerminal terminal = Make();
    QList<QByteArray> answers;
    terminal.SetResponseHandler([&answers](QByteArrayView answer) {
        answers.append(answer.toByteArray());
    });
    // In-band resize notifications; the daemon reports no pixel sizes.
    terminal.Write("\x1b[?2048h");
    answers.clear();
    QVERIFY(terminal.Resize(100, 30));
    QCOMPARE(answers, (QList<QByteArray>{"\x1b[48;30;100;0;0t"}));
}

void VtTerminalTest::ASizeOutOfRangeIsRefused() {
    constexpr quint16 limit = VtTerminal::MAX_SIZE;
    QVERIFY(!VtTerminal::Create(0, ROWS));
    QVERIFY(!VtTerminal::Create(COLUMNS, 0));
    QVERIFY(!VtTerminal::Create(limit + 1, ROWS));
    QVERIFY(!VtTerminal::Create(COLUMNS, limit + 1));
    QVERIFY(VtTerminal::Create(limit, 1));
    QVERIFY(VtTerminal::Create(1, limit));
    VtTerminal terminal = Make();
    QVERIFY(!terminal.Resize(0, ROWS));
    QVERIFY(!terminal.Resize(COLUMNS, 0));
    QVERIFY(!terminal.Resize(limit + 1, ROWS));
    QVERIFY(!terminal.Resize(COLUMNS, limit + 1));
    QVERIFY(terminal.Resize(limit, 1));
}

void VtTerminalTest::JunkIsNotASnapshot() {
    QVERIFY(!VtTerminal::FromSnapshot(""));
    QVERIFY(!VtTerminal::FromSnapshot("not a snapshot at all"));
}

void VtTerminalTest::ATruncatedSnapshotIsRefused() {
    VtTerminal terminal = Make();
    terminal.Write(Stream(5, 300));
    const QByteArray snapshot = SnapshotOf(terminal);
    for (const qsizetype size :
         {snapshot.size() / 4, snapshot.size() / 2, snapshot.size() - 1}) {
        QVERIFY2(
            !VtTerminal::FromSnapshot(QByteArrayView(snapshot).first(size)),
            qPrintable(QStringLiteral("%1 bytes of %2")
                           .arg(size)
                           .arg(snapshot.size())));
    }
}

void VtTerminalTest::BytesAfterASnapshotAreRefused() {
    VtTerminal terminal = Make();
    terminal.Write(Stream(5, 300));
    const QByteArray snapshot = SnapshotOf(terminal);
    QVERIFY(VtTerminal::FromSnapshot(snapshot));
    const auto followed = VtTerminal::FromSnapshot(snapshot + "x");
    QVERIFY(!followed);
    QCOMPARE(followed.error(), QString("1 bytes follow the snapshot"));
}

QTEST_GUILESS_MAIN(VtTerminalTest)
#include "vt_terminal_test.moc"
