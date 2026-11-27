// The runner of the commands of the notifications plugin: the real bash, in
// the limits the plugin promises (docs/design/05-plugins.md, M8).

#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <vector>

#include "helpers/scoped_env.hpp"
#include "notifications/backend/command_runner.hpp"

using namespace roost::notifications;
using roost::testing::ScopedEnv;

namespace {

constexpr int TIMEOUT_MS = 10000;

CommandJob Job(const QString& name, const QString& run) {
    return CommandJob{.Name = name,
                      .Run = run,
                      .Event = QStringLiteral("blocked"),
                      .Environment = QProcessEnvironment::systemEnvironment(),
                      .Input = {}};
}

}  // namespace

class NotificationsRunnerTest : public QObject {
    Q_OBJECT

private:
    std::vector<CommandResult> Results_;

    CommandRunner::Done Collect() {
        return
            [this](const CommandResult& result) { Results_.push_back(result); };
    }

private slots:
    void init() { Results_.clear(); }

    void OnlyTheLimitRunAtOnceTheRestWaitAndTheOverflowIsDropped() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString go = dir.filePath("go");
        const QString wait =
            QStringLiteral("while [ ! -e '%1' ]; do sleep 0.05; done").arg(go);
        CommandRunner runner(RunnerOptions{.MaxRunning = 2, .MaxQueued = 1},
                             Collect());

        QVERIFY(runner.Submit(Job("j1", wait)));
        QVERIFY(runner.Submit(Job("j2", wait)));
        QVERIFY(runner.Submit(Job("j3", wait)));
        // Two run, one waits, and the fourth has no place: its run is dropped.
        QVERIFY(!runner.Submit(Job("j4", wait)));
        QCOMPARE(runner.RunningCount(), 2);
        QCOMPARE(runner.QueuedCount(), 1);

        QFile release(go);
        QVERIFY(release.open(QIODevice::WriteOnly));
        release.close();
        QTRY_COMPARE_WITH_TIMEOUT(Results_.size(), std::size_t{3}, TIMEOUT_MS);
        QSet<QString> names;
        for (const CommandResult& result : Results_) {
            names.insert(result.Name);
            QCOMPARE(result.ExitCode, std::optional<int>(0));
            QVERIFY(result.Error.isEmpty());
        }
        QCOMPARE(names, (QSet<QString>{"j1", "j2", "j3"}));
        QCOMPARE(runner.RunningCount(), 0);
        QCOMPARE(runner.QueuedCount(), 0);
    }

    void OnlyTheEndOfTheStderrIsKept() {
        CommandRunner runner(RunnerOptions{}, Collect());
        QVERIFY(runner.Submit(Job(
            "noisy",
            "head -c 5000 /dev/zero | tr '\\0' a >&2; echo END >&2; exit 7")));
        QTRY_COMPARE_WITH_TIMEOUT(Results_.size(), std::size_t{1}, TIMEOUT_MS);
        QCOMPARE(Results_[0].ExitCode, std::optional<int>(7));
        QCOMPARE(Results_[0].Stderr.size(), 2000);
        QVERIFY(Results_[0].Stderr.endsWith("aaaEND\n"));
    }

    void ACommandThatCannotStartIsAResultNotASilence() {
        const ScopedEnv path("PATH", "/nonexistent");
        CommandRunner runner(RunnerOptions{}, Collect());
        QVERIFY(runner.Submit(Job("lost", "true")));
        QTRY_COMPARE_WITH_TIMEOUT(Results_.size(), std::size_t{1}, TIMEOUT_MS);
        QCOMPARE(Results_[0].Error, "failed to start");
        QVERIFY(!Results_[0].ExitCode);
        QCOMPARE(runner.RunningCount(), 0);
    }
};

QTEST_GUILESS_MAIN(NotificationsRunnerTest)
#include "notifications_runner_test.moc"
