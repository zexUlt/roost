#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

namespace {

QJsonObject Thread(const QString& id, const QString& preview) {
    return QJsonObject{{"id", id},
                       {"preview", preview},
                       {"createdAt", 1'730'831'111},
                       {"cwd", qEnvironmentVariable("ROOST_TEST_PROJECT")}};
}

void Write(const QJsonObject& message) {
    const QByteArray line =
        QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    std::fflush(stdout);
}

void WritePage(const QJsonValue& id, const QJsonArray& rows,
               const QJsonValue& next = QJsonValue::Null) {
    Write({{"id", id},
           {"result", QJsonObject{{"data", rows}, {"nextCursor", next}}}});
}

bool Touch(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly);
}

QJsonArray OddThreads(const QString& project) {
    QJsonObject omitted = Thread("no_cwd", "No working directory");
    omitted.remove("cwd");
    QJsonObject empty = Thread("empty_cwd", "Empty working directory");
    empty.insert("cwd", "");
    QJsonObject gone = Thread("gone_cwd", QString(400, u'a'));
    gone.insert("cwd", QDir(project).filePath("gone"));
    return {omitted, Thread("--invalid", "Skip me"), empty, gone};
}

// The test executable doubles as the Codex CLI. The default fixture first
// lists two pages, then changes a title, removes a thread and adds another.
int AppServer() {
    int polls = 0;
    const QString scenario = qEnvironmentVariable("ROOST_TEST_SCENARIO");
    if (scenario == "signals") {
        for (const int signal : {SIGPIPE, SIGUSR1}) {
            struct sigaction disposition{};
            if (::sigaction(signal, nullptr, &disposition) != 0 ||
                disposition.sa_handler != SIG_DFL) {
                return 1;
            }
        }
    }
    const QDir project(qEnvironmentVariable("ROOST_TEST_PROJECT"));
    const QString failed = project.filePath("bad-page-sent");
    const QString recover = project.filePath("recover");
    const QString childPid = project.filePath("child.pid");
    char input[4096];
    bool initialized = false;
    while (std::fgets(input, sizeof input, stdin) != nullptr) {
        const QJsonObject request = QJsonDocument::fromJson(input).object();
        const QString method = request.value("method").toString();
        const QJsonValue id = request.value("id");
        if (method == "initialize") {
            Write({{"id", id}, {"result", QJsonObject{}}});
            continue;
        }
        if (method == "initialized") {
            initialized = true;
            continue;
        }
        const QJsonObject params = request.value("params").toObject();
        if (!initialized || method != "thread/list" ||
            !params.value("modelProviders").isArray() ||
            !params.value("sourceKinds").toArray().contains("appServer")) {
            return 1;
        }
        if (scenario == "timeout") {
            if (!QFile::exists(childPid)) {
                const pid_t child = ::fork();
                if (child == 0) {
                    for (;;) {
                        ::pause();
                    }
                }
                QFile file(childPid);
                if (child < 0 || !file.open(QIODevice::WriteOnly) ||
                    file.write(QByteArray::number(child)) < 0) {
                    if (child > 0) {
                        ::kill(child, SIGKILL);
                    }
                    return 1;
                }
            }
            continue;
        }
        if (scenario == "server-error") {
            Write({{"id", id},
                   {"error",
                    QJsonObject{{"code", -32000},
                                {"message", "thread database unavailable"}}}});
            continue;
        }
        if (scenario == "oversized") {
            const QByteArray bytes =
                QByteArray(16 * 1024 * 1024 + 1, 'x') + '\n';
            std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()),
                        stdout);
            std::fflush(stdout);
            continue;
        }
        if (scenario == "odd-threads") {
            Write({{"id", id},
                   {"result",
                    QJsonObject{{"data", OddThreads(project.path())}}}});
            continue;
        }
        if (scenario == "mixed-panel") {
            WritePage(id, {Thread("codex_fixture", "Saved Codex task")});
            continue;
        }
        const QString cursor = params.value("cursor").toString();
        if (scenario == "endless-pages") {
            WritePage(id, {}, QString::number(id.toInteger()));
            continue;
        }
        if (scenario == "slow-pages") {
            QTest::qSleep(300);
            if (cursor == "page+3") {
                WritePage(id, {Thread("last", "Last page")});
            } else {
                WritePage(id, {}, cursor.isEmpty() ? "page+2" : "page+3");
            }
            continue;
        }
        if (!cursor.isEmpty()) {
            if (cursor != "page+2") {
                return 1;
            }
            if (scenario == "bad-page" && !QFile::exists(recover) &&
                (polls > 1 || QFile::exists(failed))) {
                if (!Touch(failed)) {
                    return 1;
                }
                Write({{"id", id}, {"result", QJsonObject{{"data", false}}}});
                continue;
            }
            WritePage(id, {Thread("old", "Older")});
            continue;
        }
        ++polls;
        QJsonObject first = Thread("thr_first", "First prompt");
        if (polls > 1) {
            first.insert("name", "Renamed");
        }
        QJsonArray data{first};
        if (scenario == "bad-page") {
            // Hold the fault until the test observes it. Recovery republishes
            // the original snapshot, including the row on the second page.
            if (polls == 1 || QFile::exists(recover)) {
                data = {Thread("thr_first", "First prompt")};
            }
            WritePage(id, data, "page+2");
        } else if (polls == 1) {
            WritePage(id, data, "page+2");
        } else {
            data.append(Thread("new", "New prompt"));
            WritePage(id, data);
        }
    }
    return 0;
}

class CodexAdapterTest : public QObject {
    Q_OBJECT

private:
    bool Launch(const QString& scenario, const QString& program,
                const QString& pollMs = "500",
                const QString& timeoutMs = "4000") {
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_TEST_PROJECT", WorkDirectory());
        environment.insert("ROOST_TEST_SCENARIO", scenario);
        environment.insert("ROOST_CODEX_EXECUTABLE", program);
        Process_.setProcessEnvironment(environment);
        Process_.start(CODEX_ADAPTER_PATH,
                       {"--poll-ms", pollMs, "--timeout-ms", timeoutMs});
        return Process_.waitForStarted();
    }

    QString WorkDirectory() const {
        return Project_.filePath("project with spaces");
    }

    QJsonObject Read() {
        QElapsedTimer timer;
        timer.start();
        while (!Process_.canReadLine()) {
            const auto remaining = 5000 - timer.elapsed();
            if (remaining <= 0 ||
                !Process_.waitForReadyRead(static_cast<int>(remaining))) {
                return {};
            }
        }
        return QJsonDocument::fromJson(Process_.readLine()).object();
    }

    QJsonObject Call(const QString& method, QJsonObject params = {}) {
        params.insert("agent", "codex");
        const int id = ++NextId_;
        const QByteArray request =
            QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"method", method},
                                      {"params", params}})
                .toJson(QJsonDocument::Compact) +
            '\n';
        Process_.write(request);
        for (QJsonObject answer = Read(); !answer.isEmpty(); answer = Read()) {
            if (answer.value("id") == id) {
                return answer;
            }
            Events_.append(answer);
        }
        return {};
    }

    QTemporaryDir Project_;
    pid_t ChildPid_ = 0;
    QProcess Process_;
    QList<QJsonObject> Events_;
    int NextId_ = 0;

private slots:
    void init() {
        QVERIFY(Project_.isValid());
        QVERIFY(QDir(Project_.path()).mkpath("project with spaces"));
        Events_.clear();
    }

    void cleanup() {
        Process_.closeWriteChannel();
        QVERIFY2(Process_.waitForFinished(5000),
                 Process_.readAllStandardError().constData());
        QCOMPARE(Process_.exitCode(), 0);
        Process_.readAll();
        if (ChildPid_ > 0) {
            ::kill(ChildPid_, SIGKILL);
            ChildPid_ = 0;
        }
    }

    void ListsPagesAndResumesById() {
        const QString cli = QCoreApplication::applicationFilePath();
        QVERIFY(Launch({}, cli));
        const QJsonObject listed = Call("agent.listSessions");
        const QJsonArray rows = listed.value("result").toArray();
        QCOMPARE(rows.size(), 2);
        const QJsonObject first = rows.first().toObject();
        QCOMPARE(first.value("id"), QJsonValue("thr_first"));
        QCOMPARE(first.value("title"), QJsonValue("First prompt"));
        QCOMPARE(first.value("cwd"), QJsonValue(WorkDirectory()));
        QCOMPARE(first.value("state"), QJsonValue("idle"));
        QCOMPARE(first.value("startedAt").toInteger(), 1'730'831'111'000LL);
        QCOMPARE(rows.last().toObject().value("id"), QJsonValue("old"));

        const auto attached =
            Call("agent.attachCommand", {{"session", "thr_first"}})
                .value("result")
                .toObject();
        QCOMPARE(QDir::cleanPath(attached.value("program").toString()),
                 QDir::cleanPath(cli));
        QCOMPARE(attached.value("args").toArray(),
                 QJsonArray({"resume", "thr_first"}));
        QCOMPARE(attached.value("cwd"), QJsonValue(WorkDirectory()));
        QVERIFY(Call("agent.attachCommand", {{"session", "missing"}})
                    .value("result")
                    .isNull());
        QCOMPARE(Call("agent.attachCommand", {{"session", "--last"}})
                     .value("error")
                     .toObject()
                     .value("code")
                     .toInt(),
                 -32602);

        while (Events_.size() < 3) {
            const QJsonObject event = Read();
            QVERIFY(!event.isEmpty());
            Events_.append(event);
        }
        QCOMPARE(Events_[0].value("method"),
                 QJsonValue("session.stateChanged"));
        const auto changed =
            Events_[0].value("params").toObject().value("session").toObject();
        QCOMPARE(changed.value("title"), QJsonValue("Renamed"));
        const auto added =
            Events_[1].value("params").toObject().value("session").toObject();
        QCOMPARE(added.value("id"), QJsonValue("new"));
        QCOMPARE(Events_[2].value("method"), QJsonValue("session.removed"));
        QCOMPARE(Events_[2].value("params").toObject().value("session"),
                 QJsonValue("old"));
    }

    void MissingCliIsOptional() {
        QVERIFY(Launch({}, Project_.filePath("missing-codex")));
        QCOMPARE(Call("agent.listSessions").value("result"),
                 QJsonValue(QJsonArray{}));
    }

    void AFailedSecondPageDoesNotPublishAPartialSnapshot() {
        QVERIFY(Launch("bad-page", QCoreApplication::applicationFilePath()));
        const QJsonArray rows =
            Call("agent.listSessions").value("result").toArray();
        QCOMPARE(rows.size(), 2);
        QJsonObject response;
        QTRY_VERIFY_WITH_TIMEOUT(
            (response = Call("agent.listSessions")).contains("error"), 5000);
        QVERIFY(response.value("error")
                    .toObject()
                    .value("message")
                    .toString()
                    .contains("invalid thread/list page"));
        QVERIFY(Events_.isEmpty());
        const QJsonObject known =
            Call("agent.attachCommand", {{"session", "old"}})
                .value("result")
                .toObject();
        QCOMPARE(known.value("args"), QJsonValue(QJsonArray{"resume", "old"}));
        QCOMPARE(known.value("cwd").toString(), WorkDirectory());
        QCOMPARE(Call("agent.attachCommand", {{"session", "unknown"}})
                     .value("error"),
                 response.value("error"));
        QVERIFY(Touch(QDir(WorkDirectory()).filePath("recover")));
        QTRY_COMPARE_WITH_TIMEOUT(
            Call("agent.listSessions").value("result").toArray(), rows, 5000);
        QVERIFY(Events_.isEmpty());
    }

    void EachPageGetsItsOwnTimeout() {
        QVERIFY(Launch("slow-pages", QCoreApplication::applicationFilePath(),
                       "10000", "750"));
        const QJsonArray rows =
            Call("agent.listSessions").value("result").toArray();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toObject().value("id"), QJsonValue("last"));
    }

    void AChainOfDistinctCursorsCannotKeepAPollOpen() {
        QVERIFY(Launch("endless-pages", QCoreApplication::applicationFilePath(),
                       "10000"));
        const QJsonObject error =
            Call("agent.listSessions").value("error").toObject();
        QCOMPARE(error.value("code").toInt(), -32603);
        QVERIFY(
            error.value("message").toString().contains("exceeded 1000 pages"));
    }

    void TheAppServerRestoresDefaultSignalHandlers() {
        // Qt itself resets SIGPIPE on exec. SIGUSR1 verifies the explicit
        // ResetSignalHandlers flag reaches the App Server as well.
        const auto oldPipe = ::signal(SIGPIPE, SIG_IGN);
        QVERIFY(oldPipe != SIG_ERR);
        const auto restorePipe =
            qScopeGuard([oldPipe] { ::signal(SIGPIPE, oldPipe); });
        const auto oldUser = ::signal(SIGUSR1, SIG_IGN);
        QVERIFY(oldUser != SIG_ERR);
        const auto restoreUser =
            qScopeGuard([oldUser] { ::signal(SIGUSR1, oldUser); });

        QVERIFY(Launch("signals", QCoreApplication::applicationFilePath()));
        QCOMPARE(Call("agent.listSessions").value("result").toArray().size(),
                 2);
    }

    void BadIdsAreSkippedAndAWorkingDirectoryIsOptional() {
        QVERIFY(Launch("odd-threads", QCoreApplication::applicationFilePath()));
        const QJsonArray rows =
            Call("agent.listSessions").value("result").toArray();
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows.last().toObject().value("title").toString(),
                 QString(200, u'a'));
        for (const QString& id : {QString("no_cwd"), QString("empty_cwd")}) {
            const QJsonObject attached =
                Call("agent.attachCommand", {{"session", id}})
                    .value("result")
                    .toObject();
            QCOMPARE(attached.value("args").toArray(),
                     QJsonArray({"resume", id}));
            QVERIFY(!attached.contains("cwd"));
        }
        const auto error =
            Call("agent.attachCommand", {{"session", "gone_cwd"}})
                .value("error")
                .toObject();
        QVERIFY(error.value("message").toString().contains(
            QDir(WorkDirectory()).filePath("gone")));
        QVERIFY(Process_.readAllStandardError().contains("invalid id"));
    }

    void ReportsAnAppServerError() {
        QVERIFY(
            Launch("server-error", QCoreApplication::applicationFilePath()));
        const auto error = Call("agent.listSessions").value("error").toObject();
        QCOMPARE(error.value("code").toInt(), -32603);
        QVERIFY(error.value("message").toString().contains(
            "thread database unavailable"));
    }

    void RejectsOversizedAppServerMessages() {
        QVERIFY(Launch("oversized", QCoreApplication::applicationFilePath()));
        const auto error = Call("agent.listSessions").value("error").toObject();
        QVERIFY(
            error.value("message").toString().contains("oversized message"));
    }

    void ATimeoutReleasesWaitingRequestsAndKillsTheProcessGroup() {
        QVERIFY(Launch("timeout", QCoreApplication::applicationFilePath(),
                       "10000", "750"));
        const QJsonObject error =
            Call("agent.listSessions").value("error").toObject();
        QCOMPARE(error.value("code").toInt(), -32603);
        QVERIFY(error.value("message").toString().contains("timed out"));

        QFile pidFile(QDir(WorkDirectory()).filePath("child.pid"));
        QVERIFY(pidFile.open(QIODevice::ReadOnly));
        ChildPid_ = pidFile.readAll().toInt();
        QVERIFY(ChildPid_ > 0);
        QTRY_VERIFY_WITH_TIMEOUT(::kill(ChildPid_, 0) == -1 && errno == ESRCH,
                                 5000);
        ChildPid_ = 0;
    }
};

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "app-server") {
        return AppServer();
    }
    CodexAdapterTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "codex_adapter_test.moc"
