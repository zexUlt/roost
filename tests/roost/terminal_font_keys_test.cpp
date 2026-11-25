// The keys of the terminal font size: Cmd+= / Cmd+- / Cmd+0 on macOS,
// Ctrl+Shift+= / Ctrl+Shift+- / Ctrl+Shift+0 elsewhere, the commands
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
#include <QTest>

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

    void WriteSettings(const QJsonObject& settings) {
        const QFuture<rpc::Result> writing =
            Api_->Store().Set(QStringLiteral("settings"), settings);
        QTRY_VERIFY_WITH_TIMEOUT(writing.isFinished(), TIMEOUT_MS);
        QVERIFY(writing.result());
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

            Press(static_cast<Qt::Key>(key), FONT_MODIFIERS);

            QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), expected, TIMEOUT_MS);
            QTRY_COMPARE_WITH_TIMEOUT(SizeOf(other.Singleton), expected,
                                      TIMEOUT_MS);
            QCOMPARE(Stored(), TerminalFont({{"size", expected}}));
            QVERIFY(Shown().isEmpty());
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
    // when the size changes.
    void TheLimitsDoNothingAndSayWhy_data() {
        QTest::addColumn<int>("key");
        QTest::addColumn<double>("limit");
        QTest::addColumn<QString>("reason");
        QTest::addColumn<int>("opposite");
        QTest::newRow("maximum")
            << int(Qt::Key_Equal) << MAX_FONT_SIZE
            << QStringLiteral(
                   "Terminal font size is already at its maximum, 72")
            << int(Qt::Key_Minus);
        QTest::newRow("minimum")
            << int(Qt::Key_Minus) << MIN_FONT_SIZE
            << QStringLiteral("Terminal font size is already at its minimum, 6")
            << int(Qt::Key_Equal);
    }

    void TheLimitsDoNothingAndSayWhy() {
        QFETCH(int, key);
        QFETCH(double, limit);
        QFETCH(QString, reason);
        QFETCH(int, opposite);
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WriteSettings(TerminalFont({{"size", limit}}));
        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), limit, TIMEOUT_MS);
        QVERIFY(Shown().isEmpty());

        Press(static_cast<Qt::Key>(key), FONT_MODIFIERS);

        // The refusal is the barrier: the key has been run when it is shown.
        QTRY_COMPARE_WITH_TIMEOUT(Shown(), QStringList{reason}, TIMEOUT_MS);
        WAIT_FOR_EFFECTS();
        QCOMPARE(Stored(), TerminalFont({{"size", limit}}));
        QCOMPARE(SizeOf(Singleton_), limit);

        // A second press says the same, once.
        Press(static_cast<Qt::Key>(key), FONT_MODIFIERS);
        WAIT_FOR_EFFECTS();
        QCOMPARE(Shown(), QStringList{reason});

        // The other way is free, and the reason goes with the change.
        Press(static_cast<Qt::Key>(opposite), FONT_MODIFIERS);
        QTRY_VERIFY_WITH_TIMEOUT(SizeOf(Singleton_) != limit, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Shown().isEmpty(), TIMEOUT_MS);
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

        Press(Qt::Key_0, FONT_MODIFIERS);
        Press(Qt::Key_Equal, FONT_MODIFIERS);

        QTRY_COMPARE_WITH_TIMEOUT(SizeOf(Singleton_), standard + 1, TIMEOUT_MS);
        QCOMPARE(Stored(),
                 (QJsonObject{
                     {"uiFontSize", 14},
                     {"terminalFont", QJsonObject{{"size", standard + 1}}}}));
        QVERIFY(Shown().isEmpty());
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
