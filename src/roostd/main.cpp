#include <QCommandLineParser>
#include <QCoreApplication>

#include "core/protocol.hpp"
#include "core/version.hpp"
#include "roostd/server.hpp"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("roostd"));
    QCoreApplication::setApplicationVersion(
        QString::fromStdString(roost::VersionString()));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("roost daemon: agent sessions, PTYs, plugin backends"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);

    roost::Server server;
    const QString socketPath = roost::protocol::SocketPath();
    if (auto listening = server.Listen(socketPath); !listening) {
        qCritical("roostd: %s", qUtf8Printable(listening.error()));
        return 1;
    }
    qInfo("roostd: listening on %s", qUtf8Printable(socketPath));

    return QCoreApplication::exec();
}
