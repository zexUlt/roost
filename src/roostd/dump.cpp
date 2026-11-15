#include "roostd/dump.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QtEndian>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <optional>

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

// Whether the bytes are exactly one JSON value, a scalar included.
bool IsJson(const QByteArray& bytes) {
    QJsonParseError error{};
    const QJsonDocument wrapped =
        QJsonDocument::fromJson('[' + bytes + ']', &error);
    return error.error == QJsonParseError::NoError &&
           wrapped.array().size() == 1;
}

// JSON on one line: outside strings a control byte can only be whitespace.
QString OneLine(const QByteArray& json) {
    QByteArray line = json;
    for (char& c : line) {
        if (static_cast<unsigned char>(c) < ' ') {
            c = ' ';
        }
    }
    return QString::fromUtf8(line);
}

constexpr int HELLO_ID = 1;
constexpr int SUBSCRIBE_ID = 2;

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

// False if the line could not be written.
bool PrintLine(std::FILE* out, const QString& line) {
    // Flushed: read through a pipe, line by line.
    return std::fputs(qUtf8Printable(line + u'\n'), out) >= 0 &&
           std::fflush(out) == 0;
}

// The error roostd answered one of the dump's own requests with, if any.
std::optional<QString> Refusal(const Frame& frame) {
    if (frame.Kind != EFrameKind::Rpc) {
        return std::nullopt;
    }
    const QJsonObject message = QJsonDocument::fromJson(frame.Body).object();
    const QJsonValue id = message.value(QStringLiteral("id"));
    const QJsonValue error = message.value(QStringLiteral("error"));
    if (error.isUndefined() ||
        (id != QJsonValue(HELLO_ID) && id != QJsonValue(SUBSCRIBE_ID))) {
        return std::nullopt;
    }
    return QStringLiteral("roostd refused %1: %2")
        .arg(id == QJsonValue(HELLO_ID) ? QStringLiteral("hello")
                                        : QStringLiteral("bus.subscribe"),
             error.toObject().value(QStringLiteral("message")).toString());
}

}  // namespace

QString DescribeFrame(const Frame& frame) {
    const QString kind = KindName(frame.Kind);
    if (frame.Kind == EFrameKind::Rpc) {
        if (!IsJson(frame.Body)) {
            return QStringLiteral("%1 (not JSON) \"%2\"")
                .arg(kind, Escape(frame.Body));
        }
        return kind + u' ' + OneLine(frame.Body);
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

int Dump(const QString& socketPath, std::FILE* out) {
    // A reader gone away shows up as EPIPE. Qt may ignore SIGPIPE already;
    // the dump does not count on it.
    std::signal(SIGPIPE, SIG_IGN);
    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(CONNECT_TIMEOUT_MS)) {
        std::fprintf(stderr, "roostd --dump: cannot connect to %s: %s\n",
                     qUtf8Printable(socketPath),
                     qUtf8Printable(socket.errorString()));
        return 1;
    }
    socket.write(Request(HELLO_ID, QStringLiteral("hello"),
                         QJsonObject{
                             {QStringLiteral("protocol"), protocol::VERSION},
                             {QStringLiteral("ghosttyVt"),
                              QString::fromStdString(GhosttyVtVersion())},
                         }));
    socket.write(
        Request(SUBSCRIBE_ID, QStringLiteral("bus.subscribe"),
                QJsonObject{{QStringLiteral("pattern"), QStringLiteral("*")}}));

    FrameDecoder decoder;
    for (;;) {
        while (std::optional<Frame> frame = decoder.Next()) {
            if (!PrintLine(out, DescribeFrame(*frame))) {
                if (errno == EPIPE) {
                    return 0;
                }
                std::fprintf(stderr, "roostd --dump: cannot print: %s\n",
                             std::strerror(errno));
                return 1;
            }
            if (const std::optional<QString> refusal = Refusal(*frame)) {
                std::fprintf(stderr, "roostd --dump: %s\n",
                             qUtf8Printable(*refusal));
                return 1;
            }
        }
        if (decoder.Failed()) {
            std::fprintf(stderr, "roostd --dump: %s\n",
                         qUtf8Printable(decoder.Error()));
            return 1;
        }
        if (socket.state() != QLocalSocket::ConnectedState &&
            socket.bytesAvailable() == 0) {
            if (decoder.Unread() > 0) {
                std::fprintf(stderr,
                             "roostd --dump: roostd closed the connection "
                             "inside a frame\n");
                return 1;
            }
            return 0;
        }
        socket.waitForReadyRead(-1);
        decoder.Feed(socket.readAll());
    }
}

}  // namespace roost
