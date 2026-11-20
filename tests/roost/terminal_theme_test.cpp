// The colours of the terminals follow settings.theme (design.md §5), against
// the real roostd with the real Claude plugin behind a fake `claude` whose
// `attach` runs fake_tui. The window is its API and the Roost singleton, which
// follows the document `settings` and gives its terminals the colours; the TUI
// asks its terminal for the colours (fake_tui, key Q) and what roostd answers
// is read from the screen. A theme with the id "test" is the file named by
// ROOST_TEST_THEME_FILE, which both processes read.

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

#include <signal.h>

#include <memory>

#include "api/api.hpp"
#include "core/rpc_client.hpp"
#include "helpers/process_lifeline.hpp"
#include "qml_api/roost_singleton.hpp"
#include "terminal/io_thread.hpp"
#include "terminal/terminal_session.hpp"

using namespace roost;
using roost::testing::ProcessLifeline;
using roost::testing::RelayStandardError;

// The static plugin of the module Roost.Api, linked into this executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)

namespace {

constexpr int TIMEOUT_MS = 10000;
// How long one asking waits for its reply before asking again.
constexpr int ASK_RETRY_MS = 300;
const QString SESSION = "bg-1b2c3d4e";
// Each query answered once; a digit after the 1 would be more than once.
const QRegularExpression ANSWERED_ONCE(
    QStringLiteral(R"(answers da1=1 cpr=1(?!\d))"));

// The background of the default theme and of the test theme, and the OSC 11
// answer to each, as fake_tui shows it (escaped).
const QString TEST_BACKGROUND = "#102030";
const QString ASKED_DEFAULT = R"(reply \x1b]11;rgb:0000/0000/0000\x07)";
const QString ASKED_TEST = R"(reply \x1b]11;rgb:1010/2020/3030\x07)";

constexpr const char* FAKE_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
attach) exec "$dir/fake_tui" ;;
*) exit 2 ;;
esac
)sh";

bool Greets(const QString& socketPath) {
    const QDeadlineTimer deadline(TIMEOUT_MS);
    while (!deadline.hasExpired()) {
        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(socketPath);
        if (QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS) &&
            hello.result()) {
            return true;
        }
        QTest::qWait(20);
    }
    return false;
}

// The test theme: its background and nothing else; the rest is the default's.
bool WriteThemeFile(const QString& path, bool broken = false) {
    QJsonObject tokens{{"terminalBackground", TEST_BACKGROUND},
                       {"terminalForeground", "#c0d0e0"}};
    if (broken) {
        // Fifteen colours instead of sixteen: the terminal tokens do not parse.
        QJsonArray ansi;
        for (int i = 0; i < 15; ++i) {
            ansi.append("#000000");
        }
        tokens.insert("terminalAnsi", ansi);
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(QJsonDocument(tokens).toJson()) > 0;
}

// Written to disk before it is run: an open file that is exec'd is not a
// program yet.
bool FlushAndClose(QFile& file) {
    const bool flushed = file.flush();
    file.close();
    return flushed;
}

QString Screen(const OpenedSession& opened) {
    const auto lock = opened.Session->Lock();
    return QString::fromUtf8(
        opened.Session->Dump(lock).value_or("dump failed"));
}

}  // namespace

class TerminalThemeTest : public QObject {
    Q_OBJECT

private:
    QString SocketPath() const { return Dir_->filePath("roostd.sock"); }

    bool LaunchDaemon() {
        const QDir bin(Dir_->filePath("bin"));
        QFile script(bin.filePath("claude"));
        if (!bin.mkpath(".") || !script.open(QIODevice::WriteOnly) ||
            script.write(FAKE_CLAUDE) < 0 || !FlushAndClose(script) ||
            !script.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                                   QFile::ExeOwner) ||
            !QFile::copy(QStringLiteral(FIXTURES_DIR "/claude_agents.json"),
                         bin.filePath("agents.json")) ||
            !QFile::link(FAKE_TUI_PATH, bin.filePath("fake_tui"))) {
            return false;
        }
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_SOCKET", SocketPath());
        environment.insert("ROOST_CONFIG", Dir_->filePath("config"));
        environment.insert("ROOST_PLUGINS_PATH", ROOST_PLUGINS_DIR);
        environment.insert("ROOST_TEST_THEME_FILE",
                           Dir_->filePath("theme.json"));
        environment.insert(
            "PATH", Dir_->filePath("bin") + u':' + environment.value("PATH"));
        Daemon_ = std::make_unique<QProcess>();
        Daemon_->setProcessEnvironment(environment);
        RelayStandardError(*Daemon_);
        if (!Lifeline_.Attach(*Daemon_)) {
            return false;
        }
        Daemon_->start(ROOSTD_PATH, {});
        return Daemon_->waitForStarted(TIMEOUT_MS) && Greets(SocketPath());
    }

    // The window: its API, connected, and the singleton that follows
    // settings.
    bool Boot() {
        Api_ = std::make_unique<api::Api>();
        Engine_ = std::make_unique<QQmlEngine>();
        auto* singleton =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        if (singleton == nullptr) {
            return false;
        }
        Roost_ = singleton;
        singleton->setApi(*Api_);
        Api_->Connect(SocketPath());
        return QTest::qWaitFor([&] { return Api_->IsConnected(); }, TIMEOUT_MS);
    }

    // Opens the session and waits until the TUI has had its queries answered.
    bool OpenAndSettle() {
        const QFuture<OpenResult> opening =
            Api_->Io().Open("claude", SESSION, 80, 24);
        if (!QTest::qWaitFor([&] { return opening.isFinished(); },
                             TIMEOUT_MS) ||
            !opening.result()) {
            return false;
        }
        Opened_ = *opening.result();
        return QTest::qWaitFor(
            [&] { return Screen(Opened_).contains(ANSWERED_ONCE); },
            TIMEOUT_MS);
    }

    // The TUI asks the terminal; the answer is on its line 7.
    void Ask(const QByteArray& query) {
        Api_->Io().SendInput(Opened_.Channel, "Q" + query);
    }

    // Asks again until the screen shows the reply: roostd takes a changed
    // document in its own time, and a query asked before that is answered
    // with the old colours.
    bool AskUntilAnswered(const QByteArray& query, const QString& reply) {
        const QDeadlineTimer deadline(TIMEOUT_MS);
        while (!deadline.hasExpired()) {
            Ask(query);
            if (QTest::qWaitFor([&] { return Screen(Opened_).contains(reply); },
                                ASK_RETRY_MS)) {
                return true;
            }
        }
        return false;
    }

    // Writes the document `settings` of the daemon, as the window does.
    bool WriteSettings(const QJsonObject& settings) {
        const QFuture<rpc::Result> writing =
            Api_->Store().Set(QStringLiteral("settings"), settings);
        return QTest::qWaitFor([&] { return writing.isFinished(); },
                               TIMEOUT_MS) &&
               writing.result();
    }

    // What the window's own terminal takes for the background of its cells.
    QString WindowBackground() {
        {
            const auto lock = Opened_.Session->Lock();
            if (!Opened_.Session->BeginRenderUpdate(lock)) {
                return "begin failed";
            }
        }
        Opened_.Session->EndRenderUpdate();
        const auto rgb = Opened_.Session->RenderState().DefaultBackground();
        return QStringLiteral("#%1%2%3")
            .arg(rgb.R, 2, 16, QLatin1Char('0'))
            .arg(rgb.G, 2, 16, QLatin1Char('0'))
            .arg(rgb.B, 2, 16, QLatin1Char('0'));
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    ProcessLifeline Lifeline_;
    std::unique_ptr<QProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<QQmlEngine> Engine_;
    OpenedSession Opened_;
    RoostSingleton* Roost_ = nullptr;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
        QVERIFY(WriteThemeFile(Dir_->filePath("theme.json")));
        // The window reads the same file as the daemon.
        qputenv("ROOST_TEST_THEME_FILE",
                Dir_->filePath("theme.json").toLocal8Bit());
        Opened_ = {};
        QVERIFY(LaunchDaemon());
        QVERIFY(Boot());
    }

    void cleanup() {
        Opened_ = {};
        Roost_ = nullptr;
        Api_.reset();
        Engine_.reset();
        if (Daemon_ && Daemon_->state() != QProcess::NotRunning) {
            QCOMPARE(::kill(static_cast<pid_t>(Daemon_->processId()), SIGTERM),
                     0);
            QVERIFY(Daemon_->waitForFinished(TIMEOUT_MS));
        }
        Daemon_.reset();
        qunsetenv("ROOST_TEST_THEME_FILE");
    }

    // Without settings the colours are the default theme's: the background is
    // black, and roostd, which did not answer at all before, says so.
    void DefaultThemeIsAnswered() {
        QVERIFY(OpenAndSettle());
        Ask("\x1b]11;?\x07");
        QTRY_VERIFY_WITH_TIMEOUT(Screen(Opened_).contains(ASKED_DEFAULT),
                                 TIMEOUT_MS);
        QCOMPARE(WindowBackground(), QStringLiteral("#000000"));
    }

    // The next answer, and the window's own default background, change with
    // settings.theme, in a terminal that is open.
    void ChangingTheThemeChangesTheNextAnswerAndTheWindow() {
        QVERIFY(OpenAndSettle());
        Ask("\x1b]11;?\x07");
        QTRY_VERIFY_WITH_TIMEOUT(Screen(Opened_).contains(ASKED_DEFAULT),
                                 TIMEOUT_MS);

        QVERIFY(WriteSettings({{"theme", "test"}}));
        QTRY_COMPARE_WITH_TIMEOUT(WindowBackground(), TEST_BACKGROUND,
                                  TIMEOUT_MS);
        // roostd takes the document in its own time: ask until it answers
        // with the new colour.
        AskUntilAnswered("\x1b]11;?\x07", ASKED_TEST);

        // The foreground, the cursor and a palette entry of the same theme:
        // the test theme has a foreground of its own, and the default
        // cursor and palette.
        Ask("\x1b]10;?\x07");
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Opened_).contains(R"(reply \x1b]10;rgb:c0c0/d0d0/e0e0\x07)"),
            TIMEOUT_MS);
        Ask("\x1b]4;1;?\x07");
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Opened_).contains(
                R"(reply \x1b]4;1;rgb:cccc/6666/6666\x07)"),
            TIMEOUT_MS);
    }

    // A terminal opened after the change has the colours from the start.
    void ANewTerminalHasTheThemeAtOnce() {
        QVERIFY(WriteSettings({{"theme", "test"}}));
        // The window's Theme has the settings before the terminal opens.
        QTRY_VERIFY_WITH_TIMEOUT(
            Roost_->theme()->terminalColors().Background.R == 0x10, TIMEOUT_MS);
        QVERIFY(OpenAndSettle());
        QCOMPARE(WindowBackground(), TEST_BACKGROUND);
        AskUntilAnswered("\x1b]11;?\x07", ASKED_TEST);
    }

    // An id that no theme has is the default theme, and the daemon stays up.
    void UnknownThemeIsTheDefault() {
        QVERIFY(WriteSettings({{"theme", "test"}}));
        QVERIFY(OpenAndSettle());
        QTRY_COMPARE_WITH_TIMEOUT(WindowBackground(), TEST_BACKGROUND,
                                  TIMEOUT_MS);
        QVERIFY(WriteSettings({{"theme", "nonesuch"}}));
        QTRY_COMPARE_WITH_TIMEOUT(WindowBackground(), QStringLiteral("#000000"),
                                  TIMEOUT_MS);
        AskUntilAnswered("\x1b]11;?\x07", ASKED_DEFAULT);
        QCOMPARE(Daemon_->state(), QProcess::Running);
    }

    // A theme whose terminal tokens do not parse is the default for both
    // processes, not the previous theme for the window and the default for
    // roostd.
    void BrokenTerminalTokensAreTheDefaultInBothProcesses() {
        QVERIFY(WriteSettings({{"theme", "test"}}));
        QVERIFY(OpenAndSettle());
        QTRY_COMPARE_WITH_TIMEOUT(WindowBackground(), TEST_BACKGROUND,
                                  TIMEOUT_MS);
        QVERIFY(AskUntilAnswered("\x1b]11;?\x07", ASKED_TEST));

        // The same id, the file now broken; the document changes so that
        // both processes read it again.
        QVERIFY(WriteThemeFile(Dir_->filePath("theme.json"), true));
        QVERIFY(WriteSettings({{"theme", "test"}, {"again", 1}}));
        QTRY_COMPARE_WITH_TIMEOUT(WindowBackground(), QStringLiteral("#000000"),
                                  TIMEOUT_MS);
        QVERIFY(AskUntilAnswered("\x1b]11;?\x07", ASKED_DEFAULT));
    }
};

QTEST_MAIN(TerminalThemeTest)
#include "terminal_theme_test.moc"
