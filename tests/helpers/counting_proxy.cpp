#include "helpers/counting_proxy.hpp"

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
        connect(client, &QLocalSocket::readyRead, client,
                [this, client, upstream, buffer] {
                    const QByteArray bytes = client->readAll();
                    upstream->write(bytes);
                    buffer->append(bytes);
                    Count(*buffer);
                });
        connect(upstream, &QLocalSocket::readyRead, client,
                [client, upstream] { client->write(upstream->readAll()); });
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

void CountingProxy::Count(QByteArray& buffer) {
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
