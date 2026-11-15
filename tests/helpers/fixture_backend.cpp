// A plugin backend for tests: speaks JSON-RPC over stdio one line at a time
// and misbehaves on request. It answers commands.run {command, args}:
//   fixture.echo     the args
//   fixture.pid      its process id
//   fixture.cwd      its working directory
//   fixture.fds      the pipes and sockets it has open besides stdio
//   fixture.crash    exits with code 3 without answering
//   fixture.garbage  prints a line that is not JSON
//   fixture.stray    answers a request id nobody sent
//   fixture.flood    prints more than a line may hold, without a newline
//   fixture.split    sends the answer in two writes 50 ms apart, after a
//                    notification
//   fixture.ask      sends roostd a request and returns its answer
//   fixture.hang     never answers
//   fixture.badError answers with an error that is not an object
//   fixture.lastWord answers only when its stdin closes, on the way out
// With --crash-on-start it exits with code 3 at once. It exits with code 0 when
// its stdin closes, or after --linger-ms N more milliseconds.

#include <QByteArray>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

void Write(const QByteArray& bytes) {
    std::cout.write(bytes.constData(), bytes.size());
    std::cout.flush();
}

void Send(const QJsonObject& message) {
    Write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void Answer(const QJsonValue& id, const QJsonValue& result) {
    Send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

void Fail(const QJsonValue& id, int code, const QString& message) {
    Send({{"jsonrpc", "2.0"},
          {"id", id},
          {"error", QJsonObject{{"code", code}, {"message", message}}}});
}

QJsonObject ReadMessage() {
    std::string line;
    while (std::getline(std::cin, line)) {
        const QJsonDocument document =
            QJsonDocument::fromJson(QByteArray::fromStdString(line));
        if (document.isObject()) {
            return document.object();
        }
    }
    return {};
}

void Run(const QJsonValue& id, const QString& command, const QJsonValue& args,
         QJsonValue& lastWord) {
    if (command == "fixture.echo") {
        Answer(id, args);
    } else if (command == "fixture.pid") {
        Answer(id, static_cast<qint64>(getpid()));
    } else if (command == "fixture.cwd") {
        Answer(id, QDir::currentPath());
    } else if (command == "fixture.fds") {
        QJsonArray open;
        for (int fd = 3; fd < 1024; ++fd) {
            struct stat status{};
            if (::fstat(fd, &status) == 0 &&
                (S_ISFIFO(status.st_mode) || S_ISSOCK(status.st_mode))) {
                open.append(fd);
            }
        }
        Answer(id, open);
    } else if (command == "fixture.crash") {
        std::_Exit(3);
    } else if (command == "fixture.garbage") {
        Write("this is not JSON\n");
    } else if (command == "fixture.stray") {
        Answer(id.toInteger() + 1000, true);
    } else if (command == "fixture.flood") {
        const QByteArray chunk(1024 * 1024, 'x');
        for (int i = 0; i < 17; ++i) {
            Write(chunk);
        }
    } else if (command == "fixture.split") {
        Send({{"jsonrpc", "2.0"}, {"method", "fixture.progress"}});
        const QByteArray line = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                                          {"id", id},
                                                          {"result", "split"}})
                                    .toJson(QJsonDocument::Compact) +
                                '\n';
        Write(line.first(line.size() / 2));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Write(line.sliced(line.size() / 2));
    } else if (command == "fixture.ask") {
        Send(
            {{"jsonrpc", "2.0"}, {"id", "ask"}, {"method", "roostd.anything"}});
        const QJsonObject reply = ReadMessage();
        Answer(id, reply);
    } else if (command == "fixture.hang") {
        // No answer.
    } else if (command == "fixture.badError") {
        Send({{"jsonrpc", "2.0"}, {"id", id}, {"error", "boom"}});
    } else if (command == "fixture.lastWord") {
        lastWord = id;
    } else {
        Fail(id, -32602, QString("unknown command %1").arg(command));
    }
}

}  // namespace

int main(int argc, char** argv) {
    int lingerMs = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--crash-on-start") {
            return 3;
        }
        if (arg == "--linger-ms" && i + 1 < argc) {
            lingerMs = std::atoi(argv[++i]);
        }
    }
    QJsonValue lastWord = QJsonValue::Undefined;
    for (;;) {
        const QJsonObject message = ReadMessage();
        if (message.isEmpty()) {
            break;
        }
        const QJsonValue id = message.value("id");
        if (message.value("method").toString() != "commands.run") {
            Fail(id, -32601, "Method not found");
            continue;
        }
        const QJsonObject params = message.value("params").toObject();
        Run(id, params.value("command").toString(), params.value("args"),
            lastWord);
    }
    if (!lastWord.isUndefined()) {
        Answer(lastWord, "last word");
    }
    if (lingerMs >= 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(lingerMs));
    }
    return 0;
}
