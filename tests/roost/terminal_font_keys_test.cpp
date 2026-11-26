// The keys of the terminal font size: Cmd+= / Cmd+- / Cmd+0 on macOS,
// Ctrl+Shift+= / Ctrl+Shift+- / Ctrl+Shift+0 elsewhere (and Cmd+Shift+= for
// the bigger one on macOS), the commands
// `terminal.fontBigger`, `terminal.fontSmaller` and `terminal.fontDefault` of
// the plugin `terminal`. They write the document `settings` as the field of the
// Settings window does (one `store.patch` of {terminalFont: {size}}), so every
// window follows and the size survives a restart of roostd. The window is the
// real one, over the real roostd (the helpers of sessions_window.hpp); HOME,
// ROOST_SOCKET and ROOST_CONFIG are temporary.

#include <QDir>
#include <QJsonObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <optional>

#include "core/theme_source.hpp"
#include "helpers/scoped_env.hpp"
#include "helpers/sessions_window.hpp"
#include "qml_api/theme.hpp"

using namespace roost;
using namespace roost::testing;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

// The keys as the manifest of `terminal` has them: Qt's Control is Command on
// macOS, where the keys have no Shift.
#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifiers FONT_MODIFIERS = Qt::ControlModifier;
#else
constexpr Qt::KeyboardModifiers FONT_MODIFIERS =
    Qt::ControlModifier | Qt::ShiftModifier;
#endif

// A size that is not the default one.
constexpr double START_SIZE = 20;

QJsonObject TerminalFont(const QJsonObject& font) {
    return {{"terminalFont", font}};
}

}  // namespace

class TerminalFontKeysTest : public SessionsWindow {
    Q_OBJECT

private:
    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

    // Returns when the window's API has also heard the write announced, so
    // that a count of writes started after it does not take this one in.
    void WriteSettings(const QJsonObject& settings) {
        QSignalSpy announced(&Api_->Store(), &api::StoreService::Changed);
        const QFuture<rpc::Result> writing =
            Api_->Store().Set(QStringLiteral("settings"), settings);
        QTRY_VERIFY_WITH_TIMEOUT(writing.isFinished(), TIMEOUT_MS);
        QVERIFY(writing.result());
        QTRY_VERIFY_WITH_TIMEOUT(SettingsWrites(announced) >= 1, TIMEOUT_MS);
    }

    // The document `settings` as the daemon keeps it.
    QJsonObject Stored() {
        const QFuture<rpc::Result> reading =
            Api_->Store().Get(QStringLiteral("settings"));
        if (!QTest::qWaitFor([&] { return reading.isFinished(); },
                             TIMEOUT_MS)) {
            QTest::qFail("settings were not read", __FILE__, __LINE__);
            return {};
        }
        const rpc::Result answer = reading.result();
        if (!answer) {
            QTest::qFail("settings were not read", __FILE__, __LINE__);
            return {};
        }
        return answer->toObject().value(QStringLiteral("document")).toObject();
    }

    double SizeOf(const RoostSingleton* singleton) {
        return singleton->theme()->terminalFont().pointSizeF();
    }

    // The window and a second one over its own API and engine: both read the
    // size from the theme.
    struct Other {
        api::Api Api;
        QQmlEngine Engine;
        RoostSingleton* Singleton = nullptr;
    };

    void Connect(Other& other) {
        other.Singleton = other.Engine.singletonInstance<RoostSingleton*>(
            "Roost.Api", "Roost");
        QVERIFY(other.Singleton != nullptr);
        other.Singleton->setApi(other.Api);
        other.Api.Connect(SocketPath());
        QTRY_VERIFY_WITH_TIMEOUT(other.Api.IsConnected(), TIMEOUT_MS);
    }

    // Records what roostd announces from now on; `SettingsWrites` counts the
    // announcements of the document `settings`: every `store.patch` is
    // announced once, even one that changes nothing, so the count is the
    // number of writes. Other documents (the layout the window settles after
    // it starts) are announced too and are not counted.
    std::unique_ptr<QSignalSpy> CountWrites() {
        return std::make_unique<QSignalSpy>(&Api_->Store(),
                                            &api::StoreService::Changed);
    }

    static qsizetype SettingsWrites(const QSignalSpy& spy) {
        return std::count_if(spy.begin(), spy.end(), [](const auto& args) {
            return args.at(0).toString() == QStringLiteral("settings");
        });
    }

    QStringList Shown() {
        return Root()->property("shownProblems").toStringList();
    }

    // The window and the daemon go and come again over the same state
    // directory.
    void Restart() {
        CloseWindow();
        StopDaemon();
        QVERIFY(StartDaemon());
        OpenWindow();
    }

    // The field of the Settings window, which is open.
    QQuickItem* SizeField() {
        QQuickItem* const content = Root()
                                        ->findChild<QObject*>("settingsWindow")
                                        ->property("contentItem")
                                        .value<QQuickItem*>();
        return Find(content, "terminalSizeField");
    }

    static QQuickItem* Find(QQuickItem* parent, const QString& name) {
        if (parent == nullptr) {
            return nullptr;
        }
        for (QQuickItem* child : parent->childItems()) {
            if (child->objectName() == name) {
                return child;
            }
            if (QQuickItem* found = Find(child, name)) {
                return found;
            }
        }
        return nullptr;
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

    // The window has a handler for each command, and the limits it holds are
    // the ones of the reader of the settings.
    void TheWindowHandlesTheCommandsWithTheLimitsOfTheSettings() {
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        for (const char* command :
             {"terminal.fontBigger", "terminal.fontSmaller",
              "terminal.fontDefault"}) {
            QVERIFY2(Api_->Commands().HasLocal(command), command);
        }
        QCOMPARE(Root()->property("minFontSize").toDouble(), MIN_FONT_SIZE);
        QCOMPARE(Root()->property("maxFontSize").toDouble(), MAX_FONT_SIZE);
    }

    // The key changes the size by 1 in the window and in another one, writes
    // the one patch to the document, and the size is there after a restart.
    void TheKeyChangesTheSizeInTwoWindowsAndAfterARestart_data() {
        QTest::addColumn<int>("key");
        QTest::addColumn<double>("step");
        QTest::newRow("bigger") << int(Qt::Key_Equal) << 1.0;
        QTest::newRow("smaller") << int(Qt::Key_Minus) << -1.0;
    }

    void TheKeyChangesTheSizeInTwoWindowsAndAfterARestart() {
        QFETCH(int, key);
        QFETCH(double, step);
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WriteSettings(TerminalFont({{"size", START_SIZE}}));
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), START_SIZE, TIMEOUT_MS);
        const double expected = START_SIZE + step;
        {
            Other other;
            Connect(other);
            if (QTest::currentTestFailed()) {
                return;
            }
            QTRY_COMPARE_WITH_TIMEOUT(SizeOf(other.Singleton), START_SIZE,
                                      TIMEOUT_MS);

            const auto writes = CountWrites();

            Press(static_cast<Qt::Key>(key), FONT_MODIFIERS);

            QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), expected, TIMEOUT_MS);
            QTRY_COMPARE_WITH_TIMEOUT(SizeOf(other.Singleton), expected,
                                      TIMEOUT_MS);
            QCOMPARE(Stored(), TerminalFont({{"size", expected}}));
            QVERIFY(Shown().isEmpty());
            // Stored() answers from the window's cache, which only the
            // announcements update, in the order roostd sent them: once it
            // shows the key's size, every write announced before is counted.
            QCOMPARE(SettingsWrites(*writes), 1);
        }

        Restart();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), expected, TIMEOUT_MS);
    }

    // The family stays: the patch names the size only.
    void TheKeyKeepsTheFamily() {
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WriteSettings(TerminalFont({{"family", "monospace"}, {"size", 14}}));
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), 14.0, TIMEOUT_MS);

        Press(Qt::Key_Equal, FONT_MODIFIERS);

        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), 15.0, TIMEOUT_MS);
        QCOMPARE(Stored(),
                 TerminalFont({{"family", "monospace"}, {"size", 15}}));
    }

    // At the limit the key writes nothing, and the strip says why; it goes
    // when the size changes. The key the other way is the barrier: it is the
    // one write, and it is answered after the refused presses.
    void TheLimitsDoNothingAndSayWhy_data() {
        QTest::addColumn<int>("key");
        QTest::addColumn<double>("limit");
        QTest::addColumn<double>("other");
        QTest::addColumn<QString>("reason");
        QTest::addColumn<int>("opposite");
        QTest::newRow("maximum")
            << int(Qt::Key_Equal) << MAX_FONT_SIZE << MAX_FONT_SIZE - 1
            << QStringLiteral(
                   "Terminal font size is already at the maximum (72)")
            << int(Qt::Key_Minus);
        QTest::newRow("minimum")
            << int(Qt::Key_Minus) << MIN_FONT_SIZE << MIN_FONT_SIZE + 1
            << QStringLiteral(
                   "Terminal font size is already at the minimum (6)")
            << int(Qt::Key_Equal);
    }

    void TheLimitsDoNothingAndSayWhy() {
        QFETCH(int, key);
        QFETCH(double, limit);
        QFETCH(double, other);
        QFETCH(QString, reason);
        QFETCH(int, opposite);
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WriteSettings(TerminalFont({{"size", limit}}));
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), limit, TIMEOUT_MS);
        QVERIFY(Shown().isEmpty());
        const auto writes = CountWrites();

        Press(static_cast<Qt::Key>(key), FONT_MODIFIERS);

        QTRY_COMPARE_WITH_TIMEOUT(Shown(), QStringList{reason}, TIMEOUT_MS);
        QCOMPARE(SizeOf(Singleton_), limit);

        // A second press says the same, once.
        Press(static_cast<Qt::Key>(key), FONT_MODIFIERS);
        QCOMPARE(Shown(), QStringList{reason});

        // The other way is free, and the reason goes with the change.
        Press(static_cast<Qt::Key>(opposite), FONT_MODIFIERS);
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), other, TIMEOUT_MS);
        QVERIFY(Shown().isEmpty());
        QCOMPARE(Stored(), TerminalFont({{"size", other}}));
        // The two refusals wrote nothing: the one write is the barrier's.
        QCOMPARE(SettingsWrites(*writes), 1);
    }

    // Keys that come faster than the size makes its way through roostd and
    // back each step from the size the last key wrote: two in a row are two
    // steps, not the same write twice.
    void KeysInARowWithNoWaitAreStepsFromEachOther() {
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WriteSettings(TerminalFont({{"size", 14}}));
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), 14.0, TIMEOUT_MS);
        const auto writes = CountWrites();

        Press(Qt::Key_Equal, FONT_MODIFIERS);
        Press(Qt::Key_Equal, FONT_MODIFIERS);

        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), 16.0, TIMEOUT_MS);
        QCOMPARE(Stored(), TerminalFont({{"size", 16}}));
        QCOMPARE(SettingsWrites(*writes), 2);

        // And the other way, with the key between them that does the reverse.
        Press(Qt::Key_Minus, FONT_MODIFIERS);
        Press(Qt::Key_Minus, FONT_MODIFIERS);
        Press(Qt::Key_Equal, FONT_MODIFIERS);
        // The size does not tell the last write: 15 is the first write's too
        // (15, 14, 15), and the window shows it as soon as that one is
        // announced. The announcements come in the order roostd sent them, so
        // the fifth is the last key's.
        QTRY_COMPARE_WITH_TIMEOUT(SettingsWrites(*writes), 5, TIMEOUT_MS);
        QCOMPARE(SizeOf(Singleton_), 15.0);
        QCOMPARE(Stored(), TerminalFont({{"size", 15}}));
    }

    // One below the maximum, two keys in a row: one write, then the refusal,
    // and the refusal stays when the write arrives (the size is the one it is
    // about). The key the other way is the barrier.
    void TwoKeysBelowTheMaximumWriteOnceAndRefuse() {
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WriteSettings(TerminalFont({{"size", MAX_FONT_SIZE - 1}}));
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), MAX_FONT_SIZE - 1,
                                  TIMEOUT_MS);
        const auto writes = CountWrites();

        Press(Qt::Key_Equal, FONT_MODIFIERS);
        Press(Qt::Key_Equal, FONT_MODIFIERS);

        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), MAX_FONT_SIZE,
                                  TIMEOUT_MS);
        QCOMPARE(Shown(),
                 QStringList{"Terminal font size is already at the maximum "
                             "(72)"});

        Press(Qt::Key_Minus, FONT_MODIFIERS);
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), MAX_FONT_SIZE - 1,
                                  TIMEOUT_MS);
        QVERIFY(Shown().isEmpty());
        QCOMPARE(Stored(), TerminalFont({{"size", MAX_FONT_SIZE - 1}}));
        QCOMPARE(SettingsWrites(*writes), 2);
    }

    // The reason is the size's: a change from the Settings field takes it away
    // too.
    void ASizeWrittenElsewhereTakesTheReasonAway() {
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WriteSettings(TerminalFont({{"size", MAX_FONT_SIZE}}));
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), MAX_FONT_SIZE,
                                  TIMEOUT_MS);
        Press(Qt::Key_Equal, FONT_MODIFIERS);
        QTRY_COMPARE_WITH_TIMEOUT(Shown().size(), 1, TIMEOUT_MS);

        WriteSettings(TerminalFont({{"size", 30}}));

        QTRY_VERIFY_WITH_TIMEOUT(Shown().isEmpty(), TIMEOUT_MS);

        // It does not come back when the size does, with no key pressed.
        WriteSettings(TerminalFont({{"size", MAX_FONT_SIZE}}));
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), MAX_FONT_SIZE,
                                  TIMEOUT_MS);
        QVERIFY(Shown().isEmpty());
    }

    // Default removes the size, and the object with it when the size was its
    // last key, in the window, in another one and after a restart; the family
    // stays when there is one.
    void TheDefaultKeyRemovesTheSize_data() {
        QTest::addColumn<QJsonObject>("font");
        QTest::addColumn<QJsonObject>("left");
        QTest::newRow("size only")
            << QJsonObject{{"size", START_SIZE}} << QJsonObject{};
        QTest::newRow("with the family")
            << QJsonObject{{"family", "monospace"}, {"size", START_SIZE}}
            << TerminalFont({{"family", "monospace"}});
    }

    void TheDefaultKeyRemovesTheSize() {
        QFETCH(QJsonObject, font);
        QFETCH(QJsonObject, left);
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        const double standard = SizeOf(Singleton_);
        WriteSettings(TerminalFont(font));
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), START_SIZE, TIMEOUT_MS);
        {
            Other other;
            Connect(other);
            if (QTest::currentTestFailed()) {
                return;
            }
            QTRY_COMPARE_WITH_TIMEOUT(SizeOf(other.Singleton), START_SIZE,
                                      TIMEOUT_MS);

            Press(Qt::Key_0, FONT_MODIFIERS);

            QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), standard, TIMEOUT_MS);
            QTRY_COMPARE_WITH_TIMEOUT(SizeOf(other.Singleton), standard,
                                      TIMEOUT_MS);
            QCOMPARE(Stored(), left);
            QVERIFY(Shown().isEmpty());
        }

        Restart();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), standard, TIMEOUT_MS);
        QCOMPARE(Stored(), left);
    }

    // Without a stored size the default key has nothing to remove and writes
    // nothing: the next key, which is answered after it, finds the document as
    // it was.
    void TheDefaultKeyWithoutASizeWritesNothing() {
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        const double standard = SizeOf(Singleton_);
        WriteSettings({{"uiFontSize", 14}});
        const auto writes = CountWrites();

        Press(Qt::Key_0, FONT_MODIFIERS);
        Press(Qt::Key_Equal, FONT_MODIFIERS);

        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), standard + 1, TIMEOUT_MS);
        // The default key wrote nothing: the one write is the next key's.
        QCOMPARE(SettingsWrites(*writes), 1);
        QCOMPARE(Stored(),
                 (QJsonObject{
                     {"uiFontSize", 14},
                     {"terminalFont", QJsonObject{{"size", standard + 1}}}}));
        QVERIFY(Shown().isEmpty());
    }

    // The keys as the US layout sends the shifted symbols: Ctrl+Shift+= is a
    // press of "+" with Shift, Ctrl+Shift+- one of "_". On macOS Cmd+Shift+=
    // (people press Cmd++) is the second key of the bigger command, Cmd+Shift+-
    // the second key of the smaller one.
    void TheShiftedSymbolKeysRunTheCommands_data() {
        QTest::addColumn<int>("key");
        QTest::addColumn<double>("step");
#ifdef Q_OS_MACOS
        QTest::newRow("Cmd+Shift+=") << int(Qt::Key_Plus) << 1.0;
        QTest::newRow("Cmd+Shift+-") << int(Qt::Key_Underscore) << -1.0;
#else
        QTest::newRow("Ctrl+Shift+= is Plus") << int(Qt::Key_Plus) << 1.0;
        QTest::newRow("Ctrl+Shift+- is Underscore")
            << int(Qt::Key_Underscore) << -1.0;
#endif
    }

    void TheShiftedSymbolKeysRunTheCommands() {
        QFETCH(int, key);
        QFETCH(double, step);
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WriteSettings(TerminalFont({{"size", START_SIZE}}));
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), START_SIZE, TIMEOUT_MS);
        const auto writes = CountWrites();

        Press(static_cast<Qt::Key>(key), FONT_MODIFIERS | Qt::ShiftModifier);

        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), START_SIZE + step,
                                  TIMEOUT_MS);
        QCOMPARE(Stored(), TerminalFont({{"size", START_SIZE + step}}));
        QCOMPARE(SettingsWrites(*writes), 1);
    }

    // The keys are the window's: the program in the terminal gets no byte of
    // them. A letter typed after them is the second the program has seen, not
    // the third; the size that changed shows the keys ran.
    void TheKeysSendNoByteToTheProgramInTheTerminal() {
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasActiveFocus(), TIMEOUT_MS);
        WriteSettings(TerminalFont({{"size", START_SIZE}}));
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), START_SIZE, TIMEOUT_MS);
        QTest::keyClick(Window(), 'w');
        QTRY_VERIFY2_WITH_TIMEOUT(ScreenNow().contains("input w total=1"),
                                  qPrintable(ScreenNow()), TIMEOUT_MS);

        Press(Qt::Key_Equal, FONT_MODIFIERS);
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), START_SIZE + 1,
                                  TIMEOUT_MS);
        Press(Qt::Key_Minus, FONT_MODIFIERS);
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), START_SIZE, TIMEOUT_MS);
        Press(Qt::Key_0, FONT_MODIFIERS);
        QTRY_COMPARE_WITH_TIMEOUT(Stored(), QJsonObject{}, TIMEOUT_MS);

        QVERIFY(Terminal()->hasActiveFocus());
        QTest::keyClick(Window(), 'o');
        QTRY_VERIFY2_WITH_TIMEOUT(ScreenNow().contains("input o total=2"),
                                  qPrintable(ScreenNow()), TIMEOUT_MS);
    }

    // The field of the Settings window shows the size the key wrote, and the
    // one the default key took away.
    void TheSettingsFieldShowsTheNewSize() {
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WriteSettings(TerminalFont({{"size", START_SIZE}}));
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), START_SIZE, TIMEOUT_MS);
        QMetaObject::invokeMethod(Root(), "openSettings");
        QTRY_VERIFY_WITH_TIMEOUT(SizeField() != nullptr, TIMEOUT_MS);
        QQuickItem* const field = SizeField();
        QTRY_COMPARE_WITH_TIMEOUT(field->property("text").toString(),
                                  QStringLiteral("20"), TIMEOUT_MS);

        Press(Qt::Key_Equal, FONT_MODIFIERS);
        QTRY_COMPARE_WITH_TIMEOUT(field->property("text").toString(),
                                  QStringLiteral("21"), TIMEOUT_MS);

        Press(Qt::Key_0, FONT_MODIFIERS);
        QTRY_COMPARE_WITH_TIMEOUT(field->property("text").toString(), QString(),
                                  TIMEOUT_MS);
    }
};

QTEST_MAIN(TerminalFontKeysTest)
#include "terminal_font_keys_test.moc"
