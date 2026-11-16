// A focused TerminalItem in a real QQuickWindow on the offscreen platform:
// key events leave as the bytes the program expects, by the terminal's modes
// at that moment; a paste follows bracketed paste mode; the grid size follows
// the item and the font. Nothing here draws.

#include "core/vt_terminal.hpp"
#include "qml_api/terminal_item.hpp"
#include "terminal/qt_key_mapping.hpp"
#include "terminal/renderer.hpp"
#include "terminal/terminal_session.hpp"

#include <QKeyEvent>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

#include <memory>

using roost::TerminalItem;
using roost::TerminalSession;

namespace {

constexpr quint16 COLUMNS = 20;
constexpr quint16 ROWS = 8;

std::shared_ptr<TerminalSession> NewSession() {
    auto session = TerminalSession::Create(COLUMNS, ROWS);
    if (!session) {
        qFatal("%s", qPrintable(session.error()));
    }
    return std::shared_ptr<TerminalSession>(std::move(*session));
}

void Write(TerminalSession& session, QByteArrayView bytes) {
    const auto lock = session.Lock();
    session.Write(lock, bytes);
}

// The modifier Qt delivers for the physical Control key, written out per
// platform on purpose: deriving it from the options under test would pass
// whatever they said. On macOS Qt names the Command key Control and the
// Control key Meta.
#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifier CONTROL_KEY = Qt::MetaModifier;
#else
constexpr Qt::KeyboardModifier CONTROL_KEY = Qt::ControlModifier;
#endif

}  // namespace

class TerminalItemInputTest : public QObject {
    Q_OBJECT

private:
    void Show(std::shared_ptr<TerminalSession> session) {
        Session_ = std::move(session);
        Item_ = new TerminalItem(Window_.contentItem());
        Item_->setSession(Session_);
        const auto cell = roost::MeasureCell(Item_->font());
        Item_->setSize(QSizeF(COLUMNS * cell.Width, ROWS * cell.Height));
        Window_.resize(COLUMNS * cell.Width, ROWS * cell.Height);
        Window_.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Window_));
        QVERIFY(QTest::qWaitForWindowActive(&Window_));
        Item_->forceActiveFocus();
        QVERIFY(QTest::qWaitFor([&] { return Item_->hasActiveFocus(); }));
    }

    // Delivered the way the window delivers a keyboard event to its focus item.
    bool Send(QEvent::Type type, int key, Qt::KeyboardModifiers modifiers = {},
              const QString& text = {}) {
        QKeyEvent event(type, key, modifiers, text);
        event.setAccepted(false);
        QCoreApplication::sendEvent(&Window_, &event);
        return event.isAccepted();
    }

    bool Press(int key, Qt::KeyboardModifiers modifiers = {},
               const QString& text = {}) {
        return Send(QEvent::KeyPress, key, modifiers, text);
    }

    bool Release(int key, Qt::KeyboardModifiers modifiers = {},
                 const QString& text = {}) {
        return Send(QEvent::KeyRelease, key, modifiers, text);
    }

    static QByteArray Last(const QSignalSpy& spy) {
        return spy.last().at(0).toByteArray();
    }

private slots:
    void cleanup() {
        Window_.hide();
        delete Item_;
        Item_ = nullptr;
        Session_.reset();
    }

    void KeysFollowTheTerminalModes() {
        Show(NewSession());
        QSignalSpy input(Item_, &TerminalItem::input);

        QVERIFY(Press(Qt::Key_Up));
        QCOMPARE(input.count(), 1);
        QCOMPARE(Last(input), QByteArray("\x1b[A"));

        // The program switches to application cursor keys; the next key is
        // encoded by the new mode, without anyone telling the item.
        Write(*Session_, "\x1b[?1h");
        QVERIFY(Press(Qt::Key_Up));
        QCOMPARE(input.count(), 2);
        QCOMPARE(Last(input), QByteArray("\x1bOA"));

        Write(*Session_, "\x1b[?1l");
        QVERIFY(Press(Qt::Key_Up));
        QCOMPARE(Last(input), QByteArray("\x1b[A"));
    }

    void TextAndControlKeys() {
        Show(NewSession());
        QSignalSpy input(Item_, &TerminalItem::input);

        QVERIFY(Press(Qt::Key_A, {}, "a"));
        QCOMPARE(Last(input), QByteArray("a"));
        QVERIFY(Press(Qt::Key_A, Qt::ShiftModifier, "A"));
        QCOMPARE(Last(input), QByteArray("A"));
        QVERIFY(Press(Qt::Key_C, CONTROL_KEY, "\x03"));
        QCOMPARE(Last(input), QByteArray("\x03"));
        QVERIFY(Press(Qt::Key_Return, {}, "\r"));
        QCOMPARE(Last(input), QByteArray("\r"));
        QVERIFY(Press(Qt::Key_Backtab, Qt::ShiftModifier, "\t"));
        QCOMPARE(Last(input), QByteArray("\x1b[Z"));
        QCOMPARE(input.count(), 5);
    }

    void OptionActsAsAlt() {
        Show(NewSession());
        QSignalSpy input(Item_, &TerminalItem::input);

        // macOS composes with Option: Qt gives "≈" for Option+X. As Alt the
        // key is ESC x.
#ifdef Q_OS_MACOS
        const QString composed = QString::fromUtf8("\xe2\x89\x88");
#else
        const QString composed = "x";
#endif
        QVERIFY(Press(Qt::Key_X, Qt::AltModifier, composed));
        QCOMPARE(input.count(), 1);
        QCOMPARE(Last(input), QByteArray("\x1bx"));
    }

#ifdef Q_OS_MACOS
    void CommandKeyTypesNothing() {
        Show(NewSession());
        QSignalSpy input(Item_, &TerminalItem::input);

        // Qt::ControlModifier is Command on macOS: Cmd+X is the application's,
        // not a Ctrl+X, and not the letter either. (Cmd+C is the copy key,
        // taken by the item; see terminal_item_selection_test.)
        QVERIFY(!Press(Qt::Key_X, Qt::ControlModifier, "x"));
        QCOMPARE(input.count(), 0);
    }
#endif

    void NothingEmittedForWhatEncodesToNothing() {
        Show(NewSession());
        QSignalSpy input(Item_, &TerminalItem::input);

        // A release in legacy mode, a modifier alone, a dead key: no bytes,
        // and the event is left for others (shortcuts, focus handling).
        QVERIFY(!Release(Qt::Key_A, {}, "a"));
        QVERIFY(!Press(Qt::Key_Shift, Qt::ShiftModifier));
        QVERIFY(!Press(Qt::Key_Dead_Acute));
        QCOMPARE(input.count(), 0);

        // And a key press does not stop being one after them.
        QVERIFY(Press(Qt::Key_B, {}, "b"));
        QCOMPARE(input.count(), 1);
    }

    void ReleaseIsReportedWhenTheProgramAsksForIt() {
        Show(NewSession());
        // Kitty keyboard protocol: disambiguate and report events.
        Write(*Session_, "\x1b[>3u");
        QSignalSpy input(Item_, &TerminalItem::input);

        QVERIFY(Press(Qt::Key_Escape, {}, "\x1b"));
        QCOMPARE(Last(input), QByteArray("\x1b[27u"));
        QVERIFY(Release(Qt::Key_Escape, {}, "\x1b"));
        QCOMPARE(Last(input), QByteArray("\x1b[27;1:3u"));
        QCOMPARE(input.count(), 2);
    }

    void InputDoesNotReachTheLocalTerminal() {
        // Only the output the PTY echoes changes the screen (design.md §3).
        Show(NewSession());
        const auto dumpOf = [&] {
            const auto lock = Session_->Lock();
            return Session_->Dump(lock).value_or(QByteArray("error"));
        };
        const auto before = dumpOf();
        Press(Qt::Key_A, {}, "a");
        Press(Qt::Key_Return, {}, "\r");
        Item_->paste("pasted");
        QCOMPARE(dumpOf(), before);
    }

    void WithoutASessionKeysAreIgnored() {
        Item_ = new TerminalItem(Window_.contentItem());
        Item_->setSize(QSizeF(100, 100));
        Window_.resize(100, 100);
        Window_.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Window_));
        QVERIFY(QTest::qWaitForWindowActive(&Window_));
        Item_->forceActiveFocus();
        QVERIFY(QTest::qWaitFor([&] { return Item_->hasActiveFocus(); }));
        QSignalSpy input(Item_, &TerminalItem::input);

        QVERIFY(!Press(Qt::Key_A, {}, "a"));
        Item_->paste("x");
        QCOMPARE(input.count(), 0);
    }

    void PasteFollowsBracketedPasteMode() {
        Show(NewSession());
        QSignalSpy input(Item_, &TerminalItem::input);

        // Off: the text, newlines as the Enter key sends them.
        Item_->paste("one\ntwo");
        QCOMPARE(Last(input), QByteArray("one\rtwo"));

        // On (CSI ? 2004 h): wrapped, newlines kept.
        Write(*Session_, "\x1b[?2004h");
        Item_->paste("one\ntwo");
        QCOMPARE(Last(input), QByteArray("\x1b[200~one\ntwo\x1b[201~"));

        Write(*Session_, "\x1b[?2004l");
        Item_->paste("three");
        QCOMPARE(Last(input), QByteArray("three"));
        QCOMPARE(input.count(), 3);
    }

    void PasteCannotBreakOutOfTheBracket() {
        Show(NewSession());
        Write(*Session_, "\x1b[?2004h");
        QSignalSpy input(Item_, &TerminalItem::input);

        // The end marker inside the text would end the paste early and run
        // the rest as typed. The ESC is replaced, so the marker is gone.
        Item_->paste("a\x1b[201~rm -rf x");
        QCOMPARE(input.count(), 1);
        const auto bytes = Last(input);
        QVERIFY(bytes.startsWith("\x1b[200~"));
        QVERIFY(bytes.endsWith("\x1b[201~"));
        QCOMPARE(bytes.count('\x1b'), 2);
    }

    void PasteOfNothingEmitsNothing() {
        Show(NewSession());
        QSignalSpy input(Item_, &TerminalItem::input);
        Item_->paste({});
        QCOMPARE(input.count(), 0);
    }

    void PasteKeepsUtf8() {
        Show(NewSession());
        QSignalSpy input(Item_, &TerminalItem::input);
        Item_->paste(QString::fromUtf8("\xd1\x8e\xf0\x9f\x98\x80"));
        QCOMPARE(Last(input), QByteArray("\xd1\x8e\xf0\x9f\x98\x80"));
    }

    void GridFollowsTheItemSize() {
        Show(NewSession());
        const auto cell = roost::MeasureCell(Item_->font());
        QCOMPARE(Item_->columns(), int{COLUMNS});
        QCOMPARE(Item_->rows(), int{ROWS});
        QSignalSpy changed(Item_, &TerminalItem::gridSizeChanged);

        // A cell and a half more: one more whole cell, in both directions.
        Item_->setSize(QSizeF(COLUMNS * cell.Width + cell.Width * 1.5,
                              ROWS * cell.Height + cell.Height * 1.5));
        QCOMPARE(Item_->columns(), COLUMNS + 1);
        QCOMPARE(Item_->rows(), ROWS + 1);
        QCOMPARE(changed.count(), 1);

        // Just short of a cell less: floor, not round.
        Item_->setSize(QSizeF((COLUMNS + 1) * cell.Width - 1,
                              (ROWS + 1) * cell.Height - 1));
        QCOMPARE(Item_->columns(), int{COLUMNS});
        QCOMPARE(Item_->rows(), int{ROWS});
        QCOMPARE(changed.count(), 2);

        // A change that keeps the grid is not a change of it.
        Item_->setSize(QSizeF(COLUMNS * cell.Width + 1, ROWS * cell.Height));
        QCOMPARE(Item_->columns(), int{COLUMNS});
        QCOMPARE(changed.count(), 2);

        // One side only.
        Item_->setWidth(10 * cell.Width);
        QCOMPARE(Item_->columns(), 10);
        QCOMPARE(Item_->rows(), int{ROWS});
        QCOMPARE(changed.count(), 3);
    }

    void GridIsAtLeastOneAndAtMostTheTerminalsLimit() {
        Show(NewSession());
        const auto cell = roost::MeasureCell(Item_->font());

        Item_->setSize(QSizeF(0, 0));
        QCOMPARE(Item_->columns(), 1);
        QCOMPARE(Item_->rows(), 1);
        Item_->setSize(QSizeF(cell.Width * 0.5, cell.Height * 0.5));
        QCOMPARE(Item_->columns(), 1);
        QCOMPARE(Item_->rows(), 1);

        const auto limit = int{roost::VtTerminal::MAX_SIZE};
        Item_->setSize(
            QSizeF((limit + 50.0) * cell.Width, (limit + 50.0) * cell.Height));
        QCOMPARE(Item_->columns(), limit);
        QCOMPARE(Item_->rows(), limit);
    }

    void GridFollowsTheFont() {
        Show(NewSession());
        const auto before = roost::MeasureCell(Item_->font());
        QSignalSpy changed(Item_, &TerminalItem::gridSizeChanged);

        auto font = Item_->font();
        font.setPointSizeF(font.pointSizeF() * 2);
        Item_->setFont(font);
        const auto after = roost::MeasureCell(Item_->font());
        QVERIFY(after != before);

        // The item keeps its size; the cells got bigger, so fewer fit.
        const auto fit = [](qreal size, int cell) {
            return static_cast<int>(size / cell);
        };
        QCOMPARE(Item_->columns(), fit(Item_->width(), after.Width));
        QCOMPARE(Item_->rows(), fit(Item_->height(), after.Height));
        QVERIFY(Item_->columns() < int{COLUMNS});
        QCOMPARE(changed.count(), 1);

        // The same font again is no change.
        Item_->setFont(font);
        QCOMPARE(changed.count(), 1);
    }

    void TheItemDoesNotResizeTheSession() {
        Show(NewSession());
        const auto cell = roost::MeasureCell(Item_->font());
        Item_->setSize(QSizeF(5 * cell.Width, 3 * cell.Height));
        QCOMPARE(Item_->columns(), 5);
        // The owner sends session.resize, which resizes the terminal; until
        // then the terminal is as it was.
        const auto lock = Session_->Lock();
        QVERIFY(Session_->BeginRenderUpdate(lock));
        Session_->EndRenderUpdate();
        QCOMPARE(Session_->RenderState().Columns(), COLUMNS);
    }

private:
    QQuickWindow Window_;
    TerminalItem* Item_ = nullptr;
    std::shared_ptr<TerminalSession> Session_;
};

QTEST_MAIN(TerminalItemInputTest)
#include "terminal_item_input_test.moc"
