#include "helpers/roostd_process.hpp"

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QTest>

#include "core/rpc_client.hpp"

namespace roost::testing {

namespace {

constexpr int TIMEOUT_MS = 10000;

// Whether a roostd, perhaps still starting, answers hello at the path.
bool Greets(const QString& socketPath) {
    const QDeadlineTimer deadline(TIMEOUT_MS);
    while (!deadline.hasExpired()) {
        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(socketPath);
        if (QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS) &&
            hello.result()) {
            return true;
        }
        QTest::qWait(20);
    }
    return false;
}

}  // namespace

RoostdProcess::RoostdProcess() = default;

RoostdProcess::~RoostdProcess() { Stop(); }

bool RoostdProcess::AddFixturePlugin(const QString& id,
                                     const QString& fixtureBackendPath) {
    const QDir directory(PluginsPath() + u'/' + id);
    if (!directory.mkpath(".") ||
        !QFile::link(fixtureBackendPath, directory.filePath("backend"))) {
        return false;
    }
    QJsonArray commands;
    for (const char* command : {"fixture.echo", "fixture.pid", "fixture.crash",
                                "fixture.hang", "fixture.notify"}) {
        commands.append(QJsonObject{{"id", command}, {"title", command}});
    }
    const QJsonObject manifest{
        {"id", id},
        {"version", "1"},
        {"api", "0"},
        {"backend", QJsonObject{{"command", QJsonArray{"backend"}}}},
        {"contributes",
         QJsonObject{
             {"commands", commands},
             {"agents", QJsonArray{QJsonObject{
                            {"id", id + "-agent"},
                            {"capabilities", QJsonArray{"nativeTuiAttach"}}}}},
         }},
    };
    // What the backend answers to agent.listSessions; see fixture_backend.cpp.
    const QJsonObject answers{
        {"agent.listSessions",
         QJsonObject{{id + "-agent",
                      QJsonObject{{"result",
                                   QJsonArray{QJsonObject{
                                       {"id", "s1"}, {"state", "idle"}}}}}}}}};
    QFile manifestFile(directory.filePath("plugin.json"));
    QFile answersFile(directory.filePath("agent.json"));
    return manifestFile.open(QIODevice::WriteOnly) &&
           manifestFile.write(QJsonDocument(manifest).toJson()) > 0 &&
           answersFile.open(QIODevice::WriteOnly) &&
           answersFile.write(QJsonDocument(answers).toJson()) > 0;
}

bool RoostdProcess::AddQmlPlugin(const QString& id, const QJsonArray& panels,
                                 const QMap<QString, QByteArray>& files,
                                 const QString& version,
                                 const QJsonValue& native) {
    const QDir directory(PluginsPath() + u'/' + id);
    if (!directory.mkpath(".")) {
        return false;
    }
    for (auto it = files.begin(); it != files.end(); ++it) {
        QFile file(directory.filePath(it.key()));
        if (!QDir().mkpath(QFileInfo(file).absolutePath()) ||
            !file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(it.value()) != it.value().size()) {
            return false;
        }
    }
    QJsonObject manifest{
        {"id", id},
        {"version", version},
        {"api", "0"},
        {"contributes", QJsonObject{{"panels", panels}}},
    };
    if (!native.isUndefined()) {
        manifest.insert("native", native);
    }
    QFile manifestFile(directory.filePath("plugin.json"));
    return manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           manifestFile.write(QJsonDocument(manifest).toJson()) > 0;
}

bool RoostdProcess::Start(const QString& roostdPath) {
    if (!IsValid() || IsRunning()) {
        return false;
    }
    Process_ = std::make_unique<QProcess>();
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("ROOST_SOCKET", SocketPath());
    environment.insert("ROOST_CONFIG", ConfigPath());
    environment.insert("ROOST_PLUGINS_PATH", PluginsPath());
    Process_->setProcessEnvironment(environment);
    Process_->start(roostdPath, {});
    return Process_->waitForStarted(TIMEOUT_MS) && Greets(SocketPath());
}

bool RoostdProcess::Stop() {
    if (!Process_ || Process_->state() == QProcess::NotRunning) {
        return true;
    }
    Process_->terminate();
    if (Process_->waitForFinished(TIMEOUT_MS)) {
        return true;
    }
    Process_->kill();
    Process_->waitForFinished(TIMEOUT_MS);
    return false;
}

bool RoostdProcess::IsRunning() const {
    return Process_ && Process_->state() != QProcess::NotRunning;
}

}  // namespace roost::testing
