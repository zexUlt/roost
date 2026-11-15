#include "roostd/dump.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QtEndian>

#include <cstdio>

#include "core/protocol.hpp"
#include "core/version.hpp"

namespace roost {

namespace {

constexpr int CONNECT_TIMEOUT_MS = 5000;

// How much of a channel frame is shown.
constexpr qsizetype PREVIEW_BYTES = 64;

QString KindName(EFrameKind kind) {
    switch (kind) {
        case EFrameKind::Rpc:
            return QStringLiteral("rpc");
        case EFrameKind::Output:
            return QStringLiteral("output");
        case EFrameKind::Input:
            return QStringLiteral("input");
        case EFrameKind::Snapshot:
            return QStringLiteral("snapshot");
    }
    return QStringLiteral("kind %1").arg(static_cast<int>(kind));
}

QString Escape(QByteArrayView bytes) {
    QString text;
    for (const char c : bytes) {
        switch (c) {
            case '\n':
                text += QStringLiteral("\\n");
                break;
            case '\r':
                text += QStringLiteral("\\r");
                break;
            case '\t':
                text += QStringLiteral("\\t");
                break;
            case '\\':
                text += QStringLiteral("\\\\");
                break;
            case '"':
                text += QStringLiteral("\\\"");
                break;
            default:
                if (c >= ' ' && c <= '~') {
                    text += QLatin1Char(c);
                } else {
                    text += QStringLiteral("\\x%1").arg(
                        static_cast<unsigned char>(c), 2, 16, QLatin1Char('0'));
                }
        }
    }
    return text;
}

QByteArray Request(int id, const QString& method, const QJsonObject& params) {
    const QJsonObject request{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    };
    // Small enough to always fit a frame.
    return EncodeFrame(EFrameKind::Rpc,
                       QJsonDocument(request).toJson(QJsonDocument::Compact))
        .value();
}

void PrintLine(const QString& line) {
    std::fputs(qPrintable(line + u'\n'), stdout);
    // Read through a pipe, line by line.
    std::fflush(stdout);
}

}  // namespace

QString DescribeFrame(const Frame& frame) {
    const QString kind = KindName(frame.Kind);
    if (frame.Kind == EFrameKind::Rpc) {
        const QJsonDocument document = QJsonDocument::fromJson(frame.Body);
        if (document.isNull()) {
            return QStringLiteral("%1 (not JSON) \"%2\"")
                .arg(kind, Escape(frame.Body));
        }
        return kind + u' ' +
               QString::fromUtf8(document.toJson(QJsonDocument::Compact));
    }
    if (frame.Body.size() < 4) {
        return QStringLiteral("%1 (no channel) %2 bytes")
            .arg(kind)
            .arg(frame.Body.size());
    }
    const quint32 channel = qFromBigEndian<quint32>(frame.Body.constData());
    const QByteArrayView data = QByteArrayView(frame.Body).sliced(4);
    return QStringLiteral("%1 channel %2, %3 bytes \"%4\"%5")
        .arg(kind)
        .arg(channel)
        .arg(data.size())
        .arg(Escape(data.first(qMin(data.size(), PREVIEW_BYTES))),
             data.size() > PREVIEW_BYTES ? QStringLiteral("...") : QString());
}

int Dump(const QString& socketPath) {
    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(CONNECT_TIMEOUT_MS)) {
        std::fprintf(stderr, "roostd --dump: cannot connect to %s: %s\n",
                     qUtf8Printable(socketPath),
                     qUtf8Printable(socket.errorString()));
        return 1;
    }
    socket.write(Request(1, QStringLiteral("hello"),
                         QJsonObject{
                             {QStringLiteral("protocol"), protocol::VERSION},
                             {QStringLiteral("ghosttyVt"),
                              QString::fromStdString(GhosttyVtVersion())},
                         }));
    socket.write(
        Request(2, QStringLiteral("bus.subscribe"),
                QJsonObject{{QStringLiteral("pattern"), QStringLiteral("*")}}));

    FrameDecoder decoder;
    for (;;) {
        while (std::optional<Frame> frame = decoder.Next()) {
            PrintLine(DescribeFrame(*frame));
        }
        if (decoder.Failed()) {
            std::fprintf(stderr, "roostd --dump: %s\n",
                         qUtf8Printable(decoder.Error()));
            return 1;
        }
        if (socket.state() != QLocalSocket::ConnectedState &&
            socket.bytesAvailable() == 0) {
            return 0;
        }
        socket.waitForReadyRead(-1);
        decoder.Feed(socket.readAll());
    }
}

}  // namespace roost
