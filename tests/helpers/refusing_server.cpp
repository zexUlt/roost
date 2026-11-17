#include "helpers/refusing_server.hpp"

#include <QJsonDocument>
#include <QJsonObject>

#include "core/protocol.hpp"

namespace roost::testing {

RefusingServer::RefusingServer(QObject* parent) : QObject(parent) {
    connect(&Server_, &QLocalServer::newConnection, this,
            &RefusingServer::OnConnection);
}

bool RefusingServer::Listen(const QString& path) {
    return Server_.listen(path);
}

void RefusingServer::OnConnection() {
    while (QLocalSocket* socket = Server_.nextPendingConnection()) {
        socket->setParent(this);
        Decoders_.try_emplace(socket);
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket] { OnReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            Decoders_.erase(socket);
            socket->deleteLater();
        });
    }
}

void RefusingServer::OnReadyRead(QLocalSocket* socket) {
    FrameDecoder& decoder = Decoders_.at(socket);
    decoder.Feed(socket->readAll());
    while (const auto frame = decoder.Next()) {
        const QJsonObject request =
            QJsonDocument::fromJson(frame->Body).object();
        ++Hellos_;
        const rpc::Error error = protocol::MakeError(
            protocol::EErrorCode::VersionMismatch,
            QStringLiteral("roostd speaks protocol %1 with libghostty-vt %2; "
                           "restart roostd")
                .arg(PROTOCOL)
                .arg(QLatin1String(GHOSTTY_VT)),
            QJsonObject{{"protocol", PROTOCOL},
                        {"ghosttyVt", QLatin1String(GHOSTTY_VT)}});
        QJsonObject errorObject{{"code", error.Code},
                                {"message", error.Message}};
        if (!OmitData_) {
            errorObject.insert("data", error.Data);
        }
        const QJsonObject answer{{"jsonrpc", "2.0"},
                                 {"id", request.value("id")},
                                 {"error", errorObject}};
        socket->write(
            *EncodeFrame(EFrameKind::Rpc,
                         QJsonDocument(answer).toJson(QJsonDocument::Compact)));
    }
}

}  // namespace roost::testing
