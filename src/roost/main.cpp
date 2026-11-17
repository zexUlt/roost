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
#include "roost/single_session.hpp"

namespace {

// Long enough for a cold start of the software renderer on a loaded CI runner.
constexpr std::chrono::seconds SMOKE_TIMEOUT{10};

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
    parser.process(app);

    const bool sessionMode = parser.isSet(sessionOption);
    // Declared before the engine, so that it outlives the singleton Roost,
    // which the engine owns. Only the window of the shell has one: --session
    // mode has no use of its IO thread.
    std::optional<roost::api::Api> api;
    if (!sessionMode) {
        api.emplace();
    }
    QQmlApplicationEngine engine;
    if (!sessionMode) {
        auto* roostSingleton = engine.singletonInstance<roost::RoostSingleton*>(
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

    return QGuiApplication::exec();
}
