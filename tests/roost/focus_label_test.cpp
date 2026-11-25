// The label of the keyboard in the head of a panel (SessionsPanel.qml,
// TerminalPanel.qml, CommandPalette.qml): "Input in list" and "Input in
// terminal", a dot of `focus` and the words, only in the head of the panel that
// has the keyboard. In the real window against a real roostd.

#include <QDir>
#include <QQuickItem>
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

    // Whether the label is drawn: made and not hidden.
    bool Shown(const QString& instance, const char* name) {
        const QQuickItem* label = LabelOf(instance, name);
        return label != nullptr && label->isVisible();
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
        Focus("list");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list", "sessionsFocusLabel"),
                                 TIMEOUT_MS);
        const Theme* theme = Singleton_->theme();
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
            QCOMPARE(dot->property("color").value<QColor>(), theme->focus());
            QCOMPARE(dot->width(), 8.0);
            QCOMPARE(dot->height(), 8.0);
            const QQuickItem* text = LabelOf(instance, words);
            QCOMPARE(text->property("color").value<QColor>(), theme->focus());
            const QFont font = text->property("font").value<QFont>();
            QCOMPARE(font.weight(), QFont::DemiBold);
            QCOMPARE(font.pointSizeF(), theme->metaFont().pointSizeF());
        }
    }
};

QTEST_MAIN(FocusLabelTest)
#include "focus_label_test.moc"
