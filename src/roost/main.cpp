#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "api/api.hpp"
#include "core/protocol.hpp"
#include "core/version.hpp"
#include "qml_api/roost_singleton.hpp"
#include "qml_api/terminal_item.hpp"
#include "roost/daemon_launcher.hpp"
#include "roost/plugin_loader.hpp"
#include "roost/plugin_watcher.hpp"
#include "roost/shell.hpp"
#include "roost/single_session.hpp"

namespace {

// Long enough for a cold start of the software renderer on a loaded CI runner.
constexpr std::chrono::seconds SMOKE_TIMEOUT{10};
// Longer: roostd answers, the plugins load and the sessions are listed first.
constexpr std::chrono::seconds SMOKE_SESSIONS_TIMEOUT{20};
constexpr std::chrono::milliseconds SMOKE_SESSIONS_POLL{50};

// --smoke: exit with 0 once the window has shown its first frame, with 1 if it
// has not within SMOKE_TIMEOUT. Proves that Qt, the QML module and the scene
// graph load and render, which is all M0 has to prove. With a session, the
// frame counts only once the session is open, so a failed open cannot pass.
void ExitAfterFirstFrame(QQuickWindow* window, roost::SingleSession* session) {
    auto opened = std::make_shared<bool>(session == nullptr);
    if (session != nullptr) {
        QObject::connect(session, &roost::SingleSession::Opened, qApp,
                         [opened] { *opened = true; });
    }
    // frameSwapped comes from the render thread; queued, so exit runs on ours.
    QObject::connect(
        window, &QQuickWindow::frameSwapped, qApp,
        [opened] {
            if (*opened) {
                QCoreApplication::exit(0);
            }
        },
        Qt::QueuedConnection);
    QTimer::singleShot(SMOKE_TIMEOUT, qApp, [] {
        qCritical("roost --smoke: no frame within %lld s",
                  static_cast<long long>(SMOKE_TIMEOUT.count()));
        QCoreApplication::exit(1);
    });
}

// --smoke-sessions: exit with 0 once the shell is ready (connected, both
// panels created without a problem, sessions loaded and not empty, and the
// session the window opens at start, if any, open in the terminal, so that
// roostd has started its attach) and a frame was swapped after that; with 1
// and the reason as soon as the shell cannot become ready (a problem of the
// loader, such as a QML import a panel lacks, or roostd refusing to open the
// session), or with the reason of the last check if it is not ready within
// SMOKE_SESSIONS_TIMEOUT. It opens no session itself: the one the window
// opens is the one the store marks as opened, and nothing is changed beyond
// what the window writes when it shows the list.
void ExitWhenSessionsShown(QQuickWindow* window, const roost::Shell* shell) {
    struct State {
        bool Ready = false;
        // Frames swapped so far, and the number that ends the run once Ready.
        int Frames = 0;
        int FramesNeeded = 0;
        QString Reason;
    };
    auto state = std::make_shared<State>();
    auto* poll = new QTimer(qApp);
    poll->setInterval(SMOKE_SESSIONS_POLL);
    QObject::connect(poll, &QTimer::timeout, qApp, [state, window, shell] {
        if (state->Ready) {
            return;
        }
        const roost::Shell::Readiness readiness = shell->CheckReadiness();
        state->Reason = readiness.Reason;
        if (readiness.Failed) {
            qCritical("roost --smoke-sessions: %s", qPrintable(state->Reason));
            QCoreApplication::exit(1);
        } else if (readiness.Ready) {
            state->Ready = true;
            // A frame after the shell is ready, even if nothing else changes.
            state->FramesNeeded = state->Frames + 1;
            state->Reason =
                QStringLiteral("no frame after the shell was ready");
            window->update();
        }
    });
    poll->start();
    // frameSwapped comes from the render thread; queued, so exit runs on ours.
    QObject::connect(
        window, &QQuickWindow::frameSwapped, qApp,
        [state] {
            ++state->Frames;
            if (state->Ready && state->Frames >= state->FramesNeeded) {
                QCoreApplication::exit(0);
            }
        },
        Qt::QueuedConnection);
    QTimer::singleShot(SMOKE_SESSIONS_TIMEOUT, qApp, [state] {
        qCritical("roost --smoke-sessions: not ready within %lld s: %s",
                  static_cast<long long>(SMOKE_SESSIONS_TIMEOUT.count()),
                  qPrintable(state->Reason));
        QCoreApplication::exit(1);
    });
}

// QGuiApplication takes "-session <id>" and "--session <id>" for itself, as
// the id of an X11 session manager, and hides both from the parser. The form
// with "=" it leaves alone, so both are rewritten to "--session=<id>" first.
class SessionArguments {
public:
    SessionArguments(int argc, char** argv) {
        for (int i = 0; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            if ((argument == "--session" || argument == "-session") &&
                i + 1 < argc) {
                Joined_.push_back(std::string("--session=") + argv[++i]);
            } else {
                Joined_.emplace_back(argv[i]);
            }
        }
        for (std::string& argument : Joined_) {
            Pointers_.push_back(argument.data());
        }
        Pointers_.push_back(nullptr);
        Count_ = static_cast<int>(Joined_.size());
    }

    int& Count() { return Count_; }
    char** Values() { return Pointers_.data(); }

private:
    std::vector<std::string> Joined_;
    std::vector<char*> Pointers_;
    int Count_ = 0;
};

}  // namespace

int main(int argc, char* argv[]) {
    SessionArguments arguments(argc, argv);
    QGuiApplication app(arguments.Count(), arguments.Values());
    QCoreApplication::setApplicationName(QStringLiteral("roost"));
    QCoreApplication::setApplicationVersion(
        QString::fromStdString(roost::VersionString()));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Background coding-agent sessions on the desktop"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption smoke(
        QStringLiteral("smoke"),
        QStringLiteral("Exit after the first frame is shown (CI check)."));
    parser.addOption(smoke);
    const QCommandLineOption smokeSessions(
        QStringLiteral("smoke-sessions"),
        QStringLiteral("Exit 0 once connected to roostd, both panels are "
                       "created without a problem, the sessions are listed "
                       "and a frame is shown; 1 with the reason on a timeout "
                       "(check of the packages)."));
    parser.addOption(smokeSessions);
    const QCommandLineOption sessionOption(
        QStringLiteral("session"),
        QStringLiteral("Developer mode: show this one session in the whole "
                       "window, without plugins."),
        QStringLiteral("id"));
    parser.addOption(sessionOption);
    const QCommandLineOption agentOption(
        QStringLiteral("agent"),
        QStringLiteral("The agent of --session (default: claude)."),
        QStringLiteral("name"), QStringLiteral("claude"));
    parser.addOption(agentOption);
    const QCommandLineOption devOption(
        QStringLiteral("dev"),
        QStringLiteral("Developer mode: reload a plugin's QML when its files "
                       "change."));
    parser.addOption(devOption);
    const QCommandLineOption noSpawnOption(
        QStringLiteral("no-spawn"),
        QStringLiteral("Do not start roostd when nobody listens on the "
                       "socket; wait for one."));
    parser.addOption(noSpawnOption);
    parser.process(app);

    const bool sessionMode = parser.isSet(sessionOption);
    if (parser.isSet(smoke) && parser.isSet(smokeSessions)) {
        qCritical("roost: --smoke does not go with --smoke-sessions");
        return 1;
    }
    if (sessionMode && parser.isSet(smokeSessions)) {
        qCritical("roost: --smoke-sessions does not go with --session");
        return 1;
    }
    // Declared before the engine, so that it outlives the singleton Roost,
    // which the engine owns. Only the window of the shell has one: --session
    // mode has no use of its IO thread.
    std::optional<roost::api::Api> api;
    // Starts roostd on a socket nobody listens on; outlives the shell, which
    // reads it. None with --no-spawn.
    std::optional<roost::DaemonLauncher> launcher;
    if (!sessionMode) {
        api.emplace();
        if (!parser.isSet(noSpawnOption)) {
            launcher.emplace(
                *api,
                roost::DaemonLauncher::DefaultCommand(
                    roost::protocol::SocketPath()),
                roost::DaemonLauncher::DEFAULT_START_TIMEOUT,
                &roost::DaemonLauncher::StartDetached,
                roost::LoginEnvironment::Settings::OfThisProcess());
        }
    }
    QQmlApplicationEngine engine;
    // Declared after the engine, so that they go before it: the panels and
    // the contexts of the loader belong to the engine. The shell goes before
    // the loader. The watcher goes first of all.
    std::optional<roost::PluginLoader> loader;
    std::optional<roost::Shell> shell;
    std::optional<roost::PluginWatcher> watcher;
    roost::RoostSingleton* roostSingleton = nullptr;
    if (!sessionMode) {
        // Created here, before any QML: the singleton installs the key filter
        // that Roost.bindingKey needs in `create`, so the first key press must
        // not be the one that creates it.
        roostSingleton = engine.singletonInstance<roost::RoostSingleton*>(
            "Roost.Api", "Roost");
        if (roostSingleton == nullptr) {
            qCritical("roost: the singleton Roost.Api.Roost is not registered");
            return 1;
        }
        roostSingleton->setApi(*api);
        api->Connect(roost::protocol::SocketPath());
    }
    engine.loadFromModule("Roost.Shell", sessionMode ? "SessionMain" : "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    if (!sessionMode) {
        loader.emplace(engine, api->Plugins(), *roostSingleton);
        shell.emplace(*engine.rootObjects().first(), *loader, *api,
                      roost::protocol::SocketPath(),
                      launcher ? &*launcher : nullptr);
        if (parser.isSet(devOption)) {
            watcher.emplace(*loader);
        }
    }

    std::unique_ptr<roost::SingleSession> session;
    if (sessionMode) {
        auto* item =
            engine.rootObjects().first()->findChild<roost::TerminalItem*>(
                QStringLiteral("terminal"));
        if (item == nullptr) {
            qCritical("roost --session: no terminal in the window");
            return 1;
        }
        session = std::make_unique<roost::SingleSession>(
            item, parser.value(agentOption), parser.value(sessionOption));
        QObject::connect(session.get(), &roost::SingleSession::Failed, &app,
                         [] { QCoreApplication::exit(1); });
        QObject::connect(session.get(), &roost::SingleSession::Ended, &app,
                         [](const QString& reason) {
                             qInfo("roost: session ended: %s",
                                   qPrintable(reason));
                             QCoreApplication::exit(0);
                         });
        session->Start(roost::protocol::SocketPath());
    }

    if (parser.isSet(smoke)) {
        auto* window =
            qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        if (window == nullptr) {
            qCritical("roost --smoke: the root object is not a window");
            return 1;
        }
        ExitAfterFirstFrame(window, session.get());
    }

    if (parser.isSet(smokeSessions)) {
        auto* window =
            qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        if (window == nullptr) {
            qCritical(
                "roost --smoke-sessions: the root object is not a window");
            return 1;
        }
        ExitWhenSessionsShown(window, &*shell);
    }

    return QGuiApplication::exec();
}
