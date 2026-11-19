// A plugin backend for tests: speaks JSON-RPC over stdio one line at a time
// and misbehaves on request. It answers commands.run {command, args}:
//   fixture.echo     the args, null if there are none
//   fixture.pid      its process id
//   fixture.cwd      its working directory
//   fixture.fds      the pipes and sockets it has open besides stdio
//   fixture.stderr   writes the args, a string, to its stderr, answers null
//   fixture.crash    exits with code 3 without answering
//   fixture.garbage  prints a line that is not JSON
//   fixture.stray    answers a request id nobody sent
//   fixture.flood    prints more than a line may hold, without a newline
//   fixture.split    sends the answer in two writes 50 ms apart, after a
//                    notification
//   fixture.ask      sends roostd a request and returns its answer
//   fixture.hang     never answers
//   fixture.badError answers with an error that is not an object
//   fixture.empty    answers with neither a result nor an error
//   fixture.lastWord answers only when its stdin closes, on the way out
//   fixture.notify   sends each message of the args as it is, then answers
// An agent method such as agent.listSessions {agent, ...} is answered as
// agent.json in the working directory says, read anew for each request:
//   {"<method>": {"<agent>": {"result": ...} | {"error": {...}} | "crash" |
//                            "hang"}}
// An answer under "<agent>/<session>" comes first for a request with a
// session. A "result" or an "error" may come with "delayMs": N, and then it is
// sent after N milliseconds, the backend answering nothing else meanwhile; or
// with "holdUntilFile": path, and then it is sent once that file exists (at
// most 30 s), likewise.
// With --crash-on-start it exits with code 3 at once. It exits with code 0 when
// its stdin closes, or after --linger-ms N more milliseconds.

#include <QByteArray>
#include <QDir>
#include <QFile>
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
        // Absent args come back as null: a result cannot be absent.
        Answer(id, args.isUndefined() ? QJsonValue() : args);
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
    } else if (command == "fixture.stderr") {
        std::cerr << args.toString().toStdString() << std::endl;
        Answer(id, QJsonValue());
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
    } else if (command == "fixture.empty") {
        Send({{"jsonrpc", "2.0"}, {"id", id}});
    } else if (command == "fixture.lastWord") {
        lastWord = id;
    } else if (command == "fixture.notify") {
        for (const QJsonValue& message : args.toArray()) {
            Send(message.toObject());
        }
        Answer(id, true);
    } else {
        Fail(id, -32602, QString("unknown command %1").arg(command));
    }
}

void Delay(const QJsonObject& answer) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(answer.value("delayMs").toInt()));
    const QString gate = answer.value("holdUntilFile").toString();
    if (gate.isEmpty()) {
        return;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!QFile::exists(gate) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void AnswerAgent(const QJsonValue& id, const QString& method,
                 const QJsonObject& params) {
    QFile file("agent.json");
    const QJsonObject answers =
        file.open(QIODevice::ReadOnly)
            ? QJsonDocument::fromJson(file.readAll()).object()
            : QJsonObject{};
    const QJsonObject forMethod = answers.value(method).toObject();
    const QString agent = params.value("agent").toString();
    const QString session = agent + u'/' + params.value("session").toString();
    const QJsonValue answer = forMethod.contains(session)
                                  ? forMethod.value(session)
                                  : forMethod.value(agent);
    if (answer == QJsonValue("crash")) {
        std::_Exit(3);
    } else if (answer == QJsonValue("hang")) {
        // No answer.
    } else if (answer.toObject().contains("error")) {
        Delay(answer.toObject());
        Send({{"jsonrpc", "2.0"},
              {"id", id},
              {"error", answer.toObject().value("error")}});
    } else if (answer.toObject().contains("result")) {
        Delay(answer.toObject());
        Answer(id, answer.toObject().value("result"));
    } else {
        Fail(id, -32602, QString("agent.json has no answer to %1").arg(method));
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
        const QString method = message.value("method").toString();
        if (method.startsWith("agent.")) {
            AnswerAgent(id, method, message.value("params").toObject());
            continue;
        }
        if (method != "commands.run") {
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
