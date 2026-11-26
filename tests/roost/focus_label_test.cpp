// The label of the keyboard in the head of a panel (SessionsPanel.qml,
// TerminalPanel.qml, CommandPalette.qml): "Input in list" and "Input in
// terminal", a dot of `focus` and the words, only in the head of the panel that
// has the keyboard. In the real window against a real roostd.

#include <QDir>
#include <QFuture>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

#include <optional>
#include <tuple>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"
#include "qml_api/theme.hpp"

using namespace roost;
using namespace roost::testing;

class FocusLabelTest : public LayoutWindow {
    Q_OBJECT

private:
    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

    // The label of the panel, or null when the panel has none in its head.
    QQuickItem* LabelOf(const QString& instance, const char* name) {
        QQuickItem* panel = PanelByInstance(instance);
        return panel == nullptr ? nullptr : panel->findChild<QQuickItem*>(name);
    }

    // Whether the label is drawn: made, not hidden and not faded out (the
    // label of the list keeps its place by `opacity`).
    bool Shown(const QString& instance, const char* name) {
        const QQuickItem* label = LabelOf(instance, name);
        return label != nullptr && label->isVisible() && label->opacity() > 0;
    }

    // The panel is `width` wide: the list of the pair takes the rest. The
    // panel loses the keyboard with the layout, the caller gives it back.
    void NarrowTerminalTo(int width) {
        const int now = static_cast<int>(SlotByInstance("terminal")->width());
        const int list = static_cast<int>(SlotByInstance("list")->width());
        Shell_->SetLayout(
            Stacked({Pair("list", "terminal", list + now - width)}));
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(SlotByInstance("terminal")->width() - width) < 2, TIMEOUT_MS);
    }

    // The list is `width` wide.
    void ListWidthTo(int width) {
        Shell_->SetLayout(Stacked({Pair("list", "terminal", width)}));
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(SlotByInstance("list")->width() - width) < 2 &&
                SlotByInstance("terminal")->width() > 1,
            TIMEOUT_MS);
    }

    // The layout of the window is done for what changed before: two frames
    // are drawn (the first may have been under way), and the polish of the
    // items comes before each.
    void WaitForFrames() {
        for (int i = 0; i < 2; ++i) {
            QSignalSpy frame(Window(), &QQuickWindow::frameSwapped);
            Window()->update();
            QVERIFY(frame.wait(TIMEOUT_MS) || !frame.isEmpty());
        }
    }

    // Writes the document `settings`, as the settings window does.
    void WriteSettings(const QJsonObject& settings) {
        const QFuture<rpc::Result> writing =
            Api_->Store().Set(QStringLiteral("settings"), settings);
        QTRY_VERIFY_WITH_TIMEOUT(writing.isFinished(), TIMEOUT_MS);
        QVERIFY(writing.result());
    }

    // Where an item of the panel is, in the coordinates of the panel.
    QRectF InPanel(const QString& instance, const char* name) {
        const QQuickItem* item = LabelOf(instance, name);
        const QQuickItem* panel = PanelByInstance(instance);
        return item == nullptr || panel == nullptr
                   ? QRectF()
                   : QRectF(item->mapToItem(panel, QPointF(0, 0)),
                            QSizeF(item->width(), item->height()));
    }

    QString WordsOf(const QString& instance, const char* name) {
        const QQuickItem* text = LabelOf(instance, name);
        return text == nullptr ? QString() : text->property("text").toString();
    }

    // The keyboard goes to the panel, as the user's click does.
    void Focus(const QString& instance) {
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance(instance) != nullptr &&
                                     SlotByInstance(instance)->isVisible() &&
                                     SlotByInstance(instance)->width() > 0,
                                 TIMEOUT_MS);
        QQuickItem* slot = SlotByInstance(instance);
        slot->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(slot->hasActiveFocus(), TIMEOUT_MS);
    }

    QObject* Palette() { return Root()->findChild<QObject*>("commandPalette"); }

    void BootPair() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
    }

private slots:
    void init() {
        Prepare();
        QVERIFY(QDir().mkpath(Dir_->filePath("home")));
        Home_.emplace("HOME", Dir_->filePath("home").toUtf8());
        Socket_.emplace("ROOST_SOCKET", SocketPath().toUtf8());
        Config_.emplace("ROOST_CONFIG", Dir_->filePath("config").toUtf8());
    }

    void cleanup() {
        Teardown();
        Config_.reset();
        Socket_.reset();
        Home_.reset();
    }

    // The label is in the head of the list while the list has the keyboard,
    // in the head of the terminal while the terminal has it, and nowhere while
    // the palette has it; the palette says so itself.
    void TheLabelFollowsTheKeyboard() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        Focus("list");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list", "sessionsFocusLabel"),
                                 TIMEOUT_MS);
        QCOMPARE(WordsOf("list", "sessionsFocusLabelText"),
                 QStringLiteral("Input in list"));
        QVERIFY(!Shown("terminal", "terminalFocusLabel"));

        Focus("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("terminal", "terminalFocusLabel"),
                                 TIMEOUT_MS);
        QCOMPARE(WordsOf("terminal", "terminalFocusLabelText"),
                 QStringLiteral("Input in terminal"));
        QVERIFY(!Shown("list", "sessionsFocusLabel"));

        Focus("list");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list", "sessionsFocusLabel"),
                                 TIMEOUT_MS);
        QVERIFY(!Shown("terminal", "terminalFocusLabel"));

        // The palette has the keyboard: no panel shows the label, the palette
        // shows its own, which the panels do not repeat.
        QVERIFY(QMetaObject::invokeMethod(Root(), "openPalette"));
        QTRY_VERIFY_WITH_TIMEOUT(Palette()->property("visible").toBool(),
                                 TIMEOUT_MS);
        QQuickItem* hint = Palette()->findChild<QQuickItem*>("paletteHint");
        QVERIFY(hint != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            hint->isVisible() && hint->property("text").toString().startsWith(
                                     QStringLiteral("Input in commands")),
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!Shown("list", "sessionsFocusLabel"),
                                 TIMEOUT_MS);
        QVERIFY(!Shown("terminal", "terminalFocusLabel"));
    }

    // Of two terminals only the one with the keyboard has the label.
    void OnlyThePanelWithTheKeyboardHasTheLabel() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        LayoutNode second;
        second.Instance = "terminal-2";
        second.Panel = TERMINAL;
        second.Links.insert(
            "session", {.From = "list", .Output = "selected", .Problem = {}});
        Layout layout = Stacked({Pair("list", "terminal")});
        layout.Root.Children.push_back(std::move(second));
        layout.Root.Sizes.push_back(LayoutSize::Rest());
        Shell_->SetLayout(layout);
        for (const QString& instance : {"list", "terminal", "terminal-2"}) {
            QTRY_VERIFY_WITH_TIMEOUT(
                SlotByInstance(instance) != nullptr &&
                    SlotByInstance(instance)->isVisible() &&
                    SlotByInstance(instance)->width() > 0,
                TIMEOUT_MS);
        }
        Focus("terminal-2");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("terminal-2", "terminalFocusLabel"),
                                 TIMEOUT_MS);
        QVERIFY(!Shown("terminal", "terminalFocusLabel"));
        QVERIFY(!Shown("list", "sessionsFocusLabel"));
        Focus("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("terminal", "terminalFocusLabel"),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!Shown("terminal-2", "terminalFocusLabel"),
                                 TIMEOUT_MS);
    }

    // The dot and the words are in the `focus` token, the dot is 8 px, and the
    // words are 11 px semibold (the type scale's `metaFont`).
    void TheLabelIsDrawnInTheFocusToken() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        // Another colour than Base, so that a colour written into the QML
        // instead of the token would show.
        Theme* theme = Singleton_->theme();
        const QColor other("#6a1b9a");
        QVERIFY(theme->focus() != other);
        theme->load({{"focus", other.name()}});
        QCOMPARE(theme->focus(), other);
        Focus("list");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list", "sessionsFocusLabel"),
                                 TIMEOUT_MS);
        for (const auto& [instance, label, words] :
             {std::tuple{"list", "sessionsFocusLabel",
                         "sessionsFocusLabelText"},
              std::tuple{"terminal", "terminalFocusLabel",
                         "terminalFocusLabelText"}}) {
            Focus(instance);
            QTRY_VERIFY_WITH_TIMEOUT(Shown(instance, label), TIMEOUT_MS);
            const QQuickItem* row = LabelOf(instance, label);
            const QQuickItem* dot = row->childItems().value(0);
            QVERIFY(dot != nullptr);
            QCOMPARE(dot->property("color").value<QColor>(), other);
            QCOMPARE(dot->width(), 8.0);
            QCOMPARE(dot->height(), 8.0);
            const QQuickItem* text = LabelOf(instance, words);
            QCOMPARE(text->property("color").value<QColor>(), other);
            const QFont font = text->property("font").value<QFont>();
            QCOMPARE(font.weight(), QFont::DemiBold);
            QCOMPARE(font.pointSizeF(), theme->metaFont().pointSizeF());
        }
    }

    // The title of the list stays where it is whether the label is there or
    // not: the label keeps its place.
    void TheTitleOfTheListDoesNotMoveWithTheLabel() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        Focus("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(!Shown("list", "sessionsFocusLabel"),
                                 TIMEOUT_MS);
        WaitForFrames();
        const QRectF without = InPanel("list", "sessionsTitle");
        QVERIFY(without.height() > 0);

        Focus("list");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list", "sessionsFocusLabel"),
                                 TIMEOUT_MS);
        WaitForFrames();
        QCOMPARE(InPanel("list", "sessionsTitle"), without);
        // The label is under the title, inside the head.
        QVERIFY(InPanel("list", "sessionsFocusLabel").top() >=
                without.bottom());
        QVERIFY(InPanel("list", "sessionsFocusLabel").bottom() <=
                InPanel("list", "sessionsSearch").top());
    }

    // The label of the list goes when it does not fit left of "+ New", and
    // never runs under the button: the list is as narrow as its minimum lets
    // it be (200), and the widths in between are checked too.
    void TheLabelOfTheListGoesInANarrowList() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        bool wasShown = false;
        bool wasHidden = false;
        // The list cannot be narrower than 200, and at the default font the
        // label still fits there: a larger font of the interface makes the
        // label and the button wider, so that the label goes at 240 and fits
        // at 280.
        WriteSettings({{"uiFontSize", 20}});
        QTRY_VERIFY_WITH_TIMEOUT(
            LabelOf("list", "sessionsTitleColumn")->parentItem()->height() > 76,
            TIMEOUT_MS);
        for (const int width : {320, 280, 240, 200}) {
            ListWidthTo(width);
            Focus("list");
            QTRY_VERIFY_WITH_TIMEOUT(
                InPanel("list", "sessionsNewButton").width() > 0, TIMEOUT_MS);
            WaitForFrames();
            const QRectF button = InPanel("list", "sessionsNewButton");
            const QRectF label = InPanel("list", "sessionsFocusLabel");
            const QRectF title = InPanel("list", "sessionsTitle");
            // The label is faded in or out with the width: when it is there it
            // is whole, left of the button.
            if (Shown("list", "sessionsFocusLabel")) {
                wasShown = true;
                QVERIFY2(label.right() <= button.left(),
                         qPrintable(QString::number(width)));
            } else {
                wasHidden = true;
            }
            QVERIFY2(title.right() <= button.left(),
                     qPrintable(QString::number(width)));
            // Its place is kept, shown or not.
            QVERIFY2(label.top() >= title.bottom(),
                     qPrintable(QString::number(width)));
        }
        QVERIFY(wasShown);
        QVERIFY(wasHidden);
        // Narrow: no label; wide again: it is back.
        ListWidthTo(200);
        Focus("list");
        QTRY_VERIFY_WITH_TIMEOUT(!Shown("list", "sessionsFocusLabel"),
                                 TIMEOUT_MS);
        ListWidthTo(320);
        Focus("list");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list", "sessionsFocusLabel"),
                                 TIMEOUT_MS);
    }

    // A large font of the interface: the head of the list grows with its two
    // lines and the padding, and the search field starts below it; with the
    // normal font it is 76 again.
    void TheHeadOfTheListGrowsWithALargeFont() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        Focus("list");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list", "sessionsFocusLabel"),
                                 TIMEOUT_MS);
        const QQuickItem* column = LabelOf("list", "sessionsTitleColumn");
        const QQuickItem* top = column->parentItem();
        QCOMPARE(top->height(), 76.0);

        WriteSettings({{"uiFontSize", 36}});
        QTRY_VERIFY_WITH_TIMEOUT(top->height() > 76, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            column->height() + 2 * Theme::panelPadding() <= top->height() &&
                column->y() >= Theme::panelPadding() - 1 &&
                InPanel("list", "sessionsSearch").top() >=
                    InPanel("list", "sessionsFocusLabel").bottom() +
                        Theme::panelPadding() - 1,
            TIMEOUT_MS);

        WriteSettings({});
        QTRY_COMPARE_WITH_TIMEOUT(top->height(), 76.0, TIMEOUT_MS);
    }

    // The head of the terminal in a narrow panel: the label goes before a
    // button does, and the title keeps its 80 px.
    void TheLabelOfTheTerminalGoesBeforeTheButtons() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        Focus("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("terminal", "terminalFocusLabel") &&
                                     Shown("terminal", "listsButton") &&
                                     Shown("terminal", "commandsButton"),
                                 TIMEOUT_MS);
        // The two buttons and the room the head keeps for the title (80),
        // the margins and the padding: the width at which the buttons still
        // fit, with a little to spare. It is too narrow for the label, which
        // needs its own width and a gap of 28 more.
        const qreal buttons = LabelOf("terminal", "listsButton")->width() + 8 +
                              LabelOf("terminal", "commandsButton")->width();
        const int narrow = static_cast<int>(20 + 2 * Theme::panelPadding() +
                                            80 + buttons + 24);
        NarrowTerminalTo(narrow);
        Focus("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(!Shown("terminal", "terminalFocusLabel"),
                                 TIMEOUT_MS);
        QVERIFY(Shown("terminal", "listsButton"));
        QVERIFY(Shown("terminal", "commandsButton"));
        QVERIFY(LabelOf("terminal", "terminalTitle")->width() >= 80);

        // Narrower: a button goes, still without the label.
        NarrowTerminalTo(narrow - 70);
        Focus("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(!Shown("terminal", "commandsButton"),
                                 TIMEOUT_MS);
        QVERIFY(!Shown("terminal", "terminalFocusLabel"));
        QVERIFY(LabelOf("terminal", "terminalTitle")->width() >= 80);

        // Wide: all of them.
        NarrowTerminalTo(640);
        Focus("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("terminal", "terminalFocusLabel") &&
                                     Shown("terminal", "commandsButton"),
                                 TIMEOUT_MS);
    }
};

QTEST_MAIN(FocusLabelTest)
#include "focus_label_test.moc"
