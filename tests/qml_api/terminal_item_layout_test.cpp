// A focused TerminalItem in a real QQuickWindow on the offscreen platform, fed
// the keys of the Russian layout: a letter is the key at its position on the
// US layout, found by the native code of the event, so Ctrl+С is the interrupt
// and Ctrl+Shift+С (Command+С on macOS) copies. The events carry both
// platforms' codes, as the same row must pass on every runner.

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

constexpr int CYR_S = 0x0421;
// The C key: kVK_ANSI_C, and KEY_C + 8.
constexpr quint32 MAC_C = 0x08;
constexpr quint32 XKB_C = 54;

std::shared_ptr<TerminalSession> NewSession() {
    auto session = TerminalSession::Create(COLUMNS, ROWS);
    if (!session) {
        qFatal("%s", qPrintable(session.error()));
    }
    return std::shared_ptr<TerminalSession>(std::move(*session));
}

// The modifier Qt delivers for the physical Control key, written out per
// platform on purpose: on macOS Qt names the Command key Control and the
// Control key Meta.
#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifier CONTROL_KEY = Qt::MetaModifier;
constexpr Qt::KeyboardModifiers COPY_MODS = Qt::ControlModifier;
#else
constexpr Qt::KeyboardModifier CONTROL_KEY = Qt::ControlModifier;
constexpr Qt::KeyboardModifiers COPY_MODS =
    Qt::ControlModifier | Qt::ShiftModifier;
#endif

}  // namespace

class TerminalItemLayoutTest : public QObject {
    Q_OBJECT

private:
    void Show() {
        Session_ = NewSession();
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

    // A press of the physical C key on the Russian layout, carrying the codes
    // of both platforms.
    bool PressRussianC(Qt::KeyboardModifiers modifiers, const QString& text) {
        QKeyEvent event(QEvent::KeyPress, CYR_S, modifiers, XKB_C, MAC_C, 0,
                        text);
        event.setAccepted(false);
        QCoreApplication::sendEvent(&Window_, &event);
        return event.isAccepted();
    }

private slots:
    void cleanup() {
        Window_.hide();
        delete Item_;
        Item_ = nullptr;
        Session_.reset();
    }

    void CtrlRussianCIsTheInterrupt_data() {
        QTest::addColumn<QString>("text");
        // What Qt may give under Ctrl on a Russian layout.
        QTest::newRow("letter") << QString("с");
        QTest::newRow("control_code") << QString("\x03");
        QTest::newRow("empty") << QString();
    }

    void CtrlRussianCIsTheInterrupt() {
        QFETCH(QString, text);
        Show();
        QSignalSpy input(Item_, &TerminalItem::input);

        QVERIFY(PressRussianC(CONTROL_KEY, text));
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.last().at(0).toByteArray(), QByteArray("\x03"));
    }

    void TypingRussianStaysText() {
        Show();
        QSignalSpy input(Item_, &TerminalItem::input);

        QVERIFY(PressRussianC({}, "с"));
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.last().at(0).toByteArray(), QString("с").toUtf8());
    }

    void CopyShortcutOnTheRussianLayoutCopies() {
        Show();
        QSignalSpy input(Item_, &TerminalItem::input);

        // Taken by the item and never sent on, with no text of the layout
        // leaking to the program.
        QVERIFY(PressRussianC(COPY_MODS, "с"));
        QCOMPARE(input.count(), 0);
    }

private:
    QQuickWindow Window_;
    TerminalItem* Item_ = nullptr;
    std::shared_ptr<TerminalSession> Session_;
};

QTEST_MAIN(TerminalItemLayoutTest)
#include "terminal_item_layout_test.moc"
