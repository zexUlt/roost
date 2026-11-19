#include "helpers/counting_proxy.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QtEndian>

#include <memory>
#include <utility>

#include "core/frame.hpp"

namespace roost::testing {

namespace {

constexpr int CONNECT_TIMEOUT_MS = 10000;

}  // namespace

CountingProxy::CountingProxy(QString listenPath, QString upstreamPath)
    : Listen_(std::move(listenPath)), Upstream_(std::move(upstreamPath)) {
    connect(&Server_, &QLocalServer::newConnection, this,
            &CountingProxy::Accept);
}

void CountingProxy::Accept() {
    while (QLocalSocket* const client = Server_.nextPendingConnection()) {
        auto* const upstream = new QLocalSocket(client);
        upstream->connectToServer(Upstream_);
        if (!upstream->waitForConnected(CONNECT_TIMEOUT_MS)) {
            client->disconnectFromServer();
            continue;
        }
        Clients_.append(client);
        const auto buffer = std::make_shared<QByteArray>();
        const auto connection = std::make_shared<Connection>();
        Connections_.append({client, connection});
        connect(client, &QLocalSocket::readyRead, client,
                [this, client, upstream, buffer, connection] {
                    const QByteArray bytes = client->readAll();
                    upstream->write(bytes);
                    buffer->append(bytes);
                    Count(*buffer, *connection);
                });
        connect(upstream, &QLocalSocket::readyRead, client,
                [this, client, upstream, connection] {
                    Forward(*client, *connection, upstream->readAll());
                });
        connect(client, &QLocalSocket::disconnected, upstream,
                [upstream] { upstream->disconnectFromServer(); });
        connect(upstream, &QLocalSocket::disconnected, client,
                [client] { client->disconnectFromServer(); });
    }
}

void CountingProxy::DropClients() {
    for (const QPointer<QLocalSocket>& client : std::exchange(Clients_, {})) {
        if (client) {
            client->disconnectFromServer();
        }
    }
}

void CountingProxy::HoldAnswers(QString method) {
    HeldMethod_ = std::move(method);
}

void CountingProxy::ReleaseAnswers() {
    HeldMethod_.clear();
    for (auto& [client, connection] : std::exchange(Connections_, {})) {
        if (!client) {
            continue;
        }
        for (const QByteArray& frame : std::exchange(connection->Held, {})) {
            client->write(frame);
        }
        connection->HeldIds.clear();
        Connections_.append({client, connection});
    }
}

void CountingProxy::Forward(QLocalSocket& client, Connection& connection,
                            const QByteArray& bytes) {
    constexpr qsizetype lengthSize = 4;
    connection.FromDaemon.append(bytes);
    QByteArray& buffer = connection.FromDaemon;
    while (buffer.size() > lengthSize) {
        const auto length = qFromBigEndian<quint32>(buffer.constData());
        const qsizetype size = lengthSize + qsizetype{length};
        if (length == 0 || buffer.size() < size) {
            return;
        }
        const QByteArray frame = buffer.first(size);
        buffer.remove(0, size);
        bool hold = !connection.Held.isEmpty();
        if (!hold && !connection.HeldIds.isEmpty() &&
            static_cast<EFrameKind>(frame[lengthSize]) == EFrameKind::Rpc) {
            const QJsonObject message =
                QJsonDocument::fromJson(frame.sliced(lengthSize + 1)).object();
            hold =
                !message.contains("method") &&
                connection.HeldIds.contains(message.value("id").toInteger(-1));
        }
        if (hold) {
            connection.Held.append(frame);
        } else {
            client.write(frame);
        }
    }
}

void CountingProxy::Count(QByteArray& buffer, Connection& connection) {
    constexpr qsizetype lengthSize = 4;
    while (buffer.size() > lengthSize) {
        const auto length = qFromBigEndian<quint32>(buffer.constData());
        const qsizetype frame = lengthSize + qsizetype{length};
        if (length == 0 || buffer.size() < frame) {
            return;
        }
        const auto kind = static_cast<EFrameKind>(buffer[lengthSize]);
        if (kind == EFrameKind::Input) {
            ++InputFrames_;
        } else if (kind == EFrameKind::Rpc) {
            const QByteArrayView body =
                QByteArrayView(buffer).sliced(lengthSize, length);
            if (!HeldMethod_.isEmpty() &&
                body.contains("\"" + HeldMethod_.toUtf8() + "\"")) {
                const QJsonObject request =
                    QJsonDocument::fromJson(body.sliced(1).toByteArray())
                        .object();
                if (request.value("method").toString() == HeldMethod_ &&
                    request.contains("id")) {
                    connection.HeldIds.insert(
                        request.value("id").toInteger(-1));
                }
            }
            if (body.contains("\"session.resize\"")) {
                ++ResizeRequests_;
            } else if (body.contains("\"session.open\"")) {
                ++OpenRequests_;
                if (body.contains("\"columns\"")) {
                    ++SizedOpenRequests_;
                }
            } else if (body.contains("\"session.view\"")) {
                ++ViewRequests_;
            }
        }
        buffer.remove(0, frame);
    }
}

}  // namespace roost::testing
