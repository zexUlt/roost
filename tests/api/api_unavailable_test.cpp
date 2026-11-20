// The window lists the sessions again while an agent is unavailable: the wait
// before the next list, how it grows and where it stops, what puts it back,
// and what ends the retry (an answer with no agent unavailable, a lost
// connection, a Refresh from outside). Against a daemon of the test's own,
// which holds every sessions.list until the test answers it, so that the test
// decides what each answer says and when.

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include "api/api.hpp"
#include "api_test_support.hpp"
#include "core/frame.hpp"
#include "core/version.hpp"

using namespace roost;
using namespace roost::api_test;

namespace {

const QString AGENT = QStringLiteral("scripted-agent");

// The waits are small so that the tests are quick. The wait the service chose
// is read from its timer; the wall clock only shows that the next list does not
// come earlier than that wait, give or take the coarseness of a timer. There is
// no upper bound: a slow runner stalls for as long as it likes.
constexpr int EARLY_MS = 5;

QJsonObject Row(const QString& id, const QString& state) {
    return {{"agent", AGENT},
            {"id", id},
            {"title", ""},
            {"cwd", ""},
            {"state", state}};
}

QJsonArray Down() {
    return {QJsonObject{{"agent", AGENT}, {"reason", "claude agents failed"}}};
}

// A roostd that is not one: it answers hello and bus.subscribe, keeps every
// sessions.list unanswered until the test answers it, and notes when each list
// came and when it was answered.
class ScriptedDaemon : public QObject {
public:
    bool Listen(const QString& path) {
        connect(&Server_, &QLocalServer::newConnection, this, [this] {
            Socket_ = Server_.nextPendingConnection();
            Decoder_ = FrameDecoder();
            Lists_.clear();
            ListedAt_.clear();
            AnsweredAt_.clear();
            connect(Socket_, &QLocalSocket::readyRead, this, [this] {
                Decoder_.Feed(Socket_->readAll());
                while (const auto frame = Decoder_.Next()) {
                    OnRequest(QJsonDocument::fromJson(frame->Body).object());
                }
            });
        });
        Clock_.start();
        return Server_.listen(path);
    }

    void Drop() {
        if (Socket_ != nullptr) {
            Socket_->abort();
        }
    }

    // The sessions.list requests of the present connection.
    int Lists() const { return static_cast<int>(Lists_.size()); }

    // Answers the index-th list of the connection.
    void AnswerList(int index, const QJsonArray& sessions,
                    const QJsonArray& unavailable) {
        AnsweredAt_.resize(Lists_.size(), -1);
        AnsweredAt_[static_cast<std::size_t>(index)] = Clock_.elapsed();
        Send({{"jsonrpc", "2.0"},
              {"id", Lists_.at(static_cast<std::size_t>(index))},
              {"result", QJsonObject{{"sessions", sessions},
                                     {"unavailable", unavailable}}}});
    }

    // From the answer to the index-th list to the arrival of the next, in ms.
    qint64 WaitAfter(int index) const {
        return ListedAt_.at(static_cast<std::size_t>(index) + 1) -
               AnsweredAt_.at(static_cast<std::size_t>(index));
    }

private:
    void Send(const QJsonObject& message) {
        Socket_->write(*EncodeFrame(
            EFrameKind::Rpc,
            QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }

    void OnRequest(const QJsonObject& request) {
        const QString method = request.value("method").toString();
        const QJsonValue id = request.value("id");
        if (method == "hello") {
            Send({{"jsonrpc", "2.0"},
                  {"id", id},
                  {"result", QJsonObject{{"roostd", QString::fromStdString(
                                                        VersionString())}}}});
        } else if (method == "bus.subscribe") {
            Send({{"jsonrpc", "2.0"},
                  {"id", id},
                  {"result", QJsonObject{{"subscription", ++Subscriptions_}}}});
        } else if (method == "sessions.list") {
            Lists_.push_back(id);
            ListedAt_.push_back(Clock_.elapsed());
        }
    }

    QLocalServer Server_;
    QLocalSocket* Socket_ = nullptr;
    FrameDecoder Decoder_;
    QElapsedTimer Clock_;
    std::vector<QJsonValue> Lists_;
    std::vector<qint64> ListedAt_;
    std::vector<qint64> AnsweredAt_;
    int Subscriptions_ = 0;
};

}  // namespace

class ApiUnavailableTest : public QObject {
    Q_OBJECT

private:
    // The window is connected to the daemon with these waits, and the list
    // that the connection asked for has not been answered yet.
    bool ConnectApi(std::chrono::milliseconds firstRelist,
                    std::chrono::milliseconds maxRelist,
                    std::chrono::milliseconds firstReconnect =
                        std::chrono::milliseconds{40}) {
        if (!Dir_->isValid() || !Daemon_->Listen(Dir_->filePath("fake.sock"))) {
            return false;
        }
        Api_ = std::make_unique<api::Api>(
            api::ApiOptions{.FirstReconnectDelay = firstReconnect,
                            .MaxReconnectDelay = firstReconnect,
                            .FirstRelistDelay = firstRelist,
                            .MaxRelistDelay = maxRelist});
        Api_->Connect(Dir_->filePath("fake.sock"));
        return QTest::qWaitFor([this] { return Daemon_->Lists() == 1; },
                               TIMEOUT_MS);
    }

    // Answers the index-th list with the agent unavailable, waits until the
    // next list comes and checks the wait that the service chose for it. The
    // timer keeps its interval after it has fired and nothing restarts it
    // before the next answer, so the interval is read without a race.
    bool AnswerDownAndWaitForNext(int index, int expectedMs) {
        Daemon_->AnswerList(index, {}, Down());
        if (!QTest::qWaitFor(
                [this, index] { return Daemon_->Lists() == index + 2; },
                TIMEOUT_MS)) {
            return false;
        }
        auto* retry = Api_->Sessions().findChild<QTimer*>();
        if (retry == nullptr) {
            return false;
        }
        const qint64 waited = Daemon_->WaitAfter(index);
        [&] {
            QVERIFY2(retry->interval() == expectedMs,
                     qPrintable(QString("wait chosen after answer %1: %2 ms, "
                                        "expected %3")
                                    .arg(index)
                                    .arg(retry->interval())
                                    .arg(expectedMs)));
            QVERIFY2(waited >= expectedMs - EARLY_MS,
                     qPrintable(QString("next list came %1 ms after answer "
                                        "%2, expected at least %3")
                                    .arg(waited)
                                    .arg(index)
                                    .arg(expectedMs)));
        }();
        return !QTest::currentTestFailed();
    }

    std::unique_ptr<ScriptedDaemon> Daemon_;
    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<api::Api> Api_;

private slots:
    void init() {
        Daemon_ = std::make_unique<ScriptedDaemon>();
        Dir_ = std::make_unique<QTemporaryDir>();
    }

    void cleanup() {
        Api_.reset();
        Daemon_.reset();
        Dir_.reset();
    }

    void AnAnswerWithAnUnavailableAgentIsAskedAgainAndAGoodOneEndsTheRetry() {
        QVERIFY(ConnectApi(std::chrono::milliseconds{40},
                           std::chrono::milliseconds{160}));
        Daemon_->AnswerList(0, {Row("s1", "idle")}, Down());
        QTRY_COMPARE_WITH_TIMEOUT(Api_->Sessions().Unavailable().size(), 1,
                                  TIMEOUT_MS);
        // Not asked at once, but after the first wait.
        QCOMPARE(Daemon_->Lists(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(Daemon_->Lists(), 2, TIMEOUT_MS);
        auto* retry = Api_->Sessions().findChild<QTimer*>();
        QVERIFY(retry != nullptr);
        QCOMPARE(retry->interval(), 40);
        QVERIFY(Daemon_->WaitAfter(0) >= 40 - EARLY_MS);

        Daemon_->AnswerList(1, {Row("s2", "working")}, {});
        QTRY_VERIFY_WITH_TIMEOUT(Api_->Sessions().Unavailable().isEmpty(),
                                 TIMEOUT_MS);
        QCOMPARE(Api_->Sessions().Rows(),
                 (QList<QJsonObject>{Row("s2", "working")}));
        // Long past the retry that a still unavailable agent would have had.
        QTest::qWait(500);
        QCOMPARE(Daemon_->Lists(), 2);
    }

    void TheWaitDoublesWhileTheAgentStaysUnavailableAndStopsAtTheCap() {
        QVERIFY(ConnectApi(std::chrono::milliseconds{40},
                           std::chrono::milliseconds{160}));
        const int expected[] = {40, 80, 160, 160, 160};
        for (int index = 0; index < 5; ++index) {
            QVERIFY(AnswerDownAndWaitForNext(index, expected[index]));
        }
    }

    void AfterAGoodAnswerTheWaitIsTheFirstAgain() {
        QVERIFY(ConnectApi(std::chrono::milliseconds{40},
                           std::chrono::milliseconds{640}));
        // 40, 80 and 160 ms are waited; the next would be 320.
        const int grown[] = {40, 80, 160, 320};
        for (int index = 0; index < 4; ++index) {
            QVERIFY(AnswerDownAndWaitForNext(index, grown[index]));
        }
        Daemon_->AnswerList(4, {Row("s1", "idle")}, {});
        QTRY_VERIFY_WITH_TIMEOUT(Api_->Sessions().Unavailable().isEmpty(),
                                 TIMEOUT_MS);

        // The agent goes away again, as it is found by a list from outside.
        Api_->Sessions().Refresh();
        QTRY_COMPARE_WITH_TIMEOUT(Daemon_->Lists(), 6, TIMEOUT_MS);
        QVERIFY(AnswerDownAndWaitForNext(5, 40));
        QVERIFY(AnswerDownAndWaitForNext(6, 80));
    }

    void ALostConnectionEndsTheRetryAndTheReconnectStartsItAnew() {
        // The reconnect is slower than the retry would be, and the wait has
        // grown to its cap when the connection goes.
        QVERIFY(ConnectApi(std::chrono::milliseconds{40},
                           std::chrono::milliseconds{160},
                           std::chrono::milliseconds{600}));
        const int grown[] = {40, 80, 160};
        for (int index = 0; index < 3; ++index) {
            QVERIFY(AnswerDownAndWaitForNext(index, grown[index]));
        }
        Daemon_->AnswerList(3, {}, Down());
        auto* retry = Api_->Sessions().findChild<QTimer*>();
        QVERIFY(retry != nullptr);
        // The wait is the cap by now, so there is time to see the timer run.
        QTRY_VERIFY_WITH_TIMEOUT(retry->isActive(), TIMEOUT_MS);

        Daemon_->Drop();
        QTRY_VERIFY_WITH_TIMEOUT(!Api_->Sessions().IsLoaded(), TIMEOUT_MS);
        QVERIFY(!retry->isActive());
        // Through the waits that the retry would have had.
        QTest::qWait(300);
        QVERIFY(!retry->isActive());

        // The reconnect asks for the list once, and nothing besides.
        QTRY_COMPARE_WITH_TIMEOUT(Daemon_->Lists(), 1, TIMEOUT_MS);
        QTest::qWait(250);
        QCOMPARE(Daemon_->Lists(), 1);
        // An answer that is not unavailable ends it there.
        Daemon_->AnswerList(0, {Row("s1", "idle")}, {});
        QTRY_VERIFY_WITH_TIMEOUT(Api_->Sessions().IsLoaded(), TIMEOUT_MS);
        QTest::qWait(300);
        QCOMPARE(Daemon_->Lists(), 1);
    }

    void TheReconnectListStartsTheRetryAtTheFirstWaitIfAnAgentIsUnavailable() {
        QVERIFY(ConnectApi(std::chrono::milliseconds{40},
                           std::chrono::milliseconds{640},
                           std::chrono::milliseconds{200}));
        // The wait grows to 640 ms by the drop: far out of the check below.
        const int grown[] = {40, 80, 160, 320};
        for (int index = 0; index < 4; ++index) {
            QVERIFY(AnswerDownAndWaitForNext(index, grown[index]));
        }
        Daemon_->Drop();
        QTRY_COMPARE_WITH_TIMEOUT(Daemon_->Lists(), 1, TIMEOUT_MS);
        QVERIFY(AnswerDownAndWaitForNext(0, 40));
    }

    void ARefreshWithoutAConnectionLeavesNoRetryScheduled() {
        QVERIFY(ConnectApi(std::chrono::milliseconds{500},
                           std::chrono::milliseconds{1000},
                           std::chrono::milliseconds{2000}));
        Daemon_->AnswerList(0, {Row("s1", "idle")}, {});
        QTRY_VERIFY_WITH_TIMEOUT(Api_->Sessions().IsLoaded(), TIMEOUT_MS);
        Daemon_->Drop();
        QTRY_VERIFY_WITH_TIMEOUT(!Api_->Sessions().IsLoaded(), TIMEOUT_MS);

        // The list fails with no connection; the one that the reconnect asks
        // for is the next, so no timer is left. There is no other sign of it.
        auto* retry = Api_->Sessions().findChild<QTimer*>();
        QVERIFY(retry != nullptr);
        QVERIFY(WaitFor(Api_->Sessions().Refresh()));
        QVERIFY(!retry->isActive());
    }

    void AListAskedByAResetSlotIsTheOneThatDecides() {
        QVERIFY(ConnectApi(std::chrono::milliseconds{500},
                           std::chrono::milliseconds{1000}));
        bool asked = false;
        connect(&Api_->Sessions(), &api::SessionService::Reset, this, [&] {
            if (!std::exchange(asked, true)) {
                Api_->Sessions().Refresh();
            }
        });
        Daemon_->AnswerList(0, {}, Down());
        QTRY_COMPARE_WITH_TIMEOUT(Daemon_->Lists(), 2, TIMEOUT_MS);
        // That list is out: the answer of the first schedules no retry.
        auto* retry = Api_->Sessions().findChild<QTimer*>();
        QVERIFY(retry != nullptr);
        QVERIFY(!retry->isActive());
        QTest::qWait(700);
        QCOMPARE(Daemon_->Lists(), 2);
    }

    void ARefreshFromOutsideWhileARetryIsScheduledLeavesOneListOut() {
        QVERIFY(ConnectApi(std::chrono::milliseconds{200},
                           std::chrono::milliseconds{400}));
        Daemon_->AnswerList(0, {}, Down());
        QTRY_COMPARE_WITH_TIMEOUT(Api_->Sessions().Unavailable().size(), 1,
                                  TIMEOUT_MS);
        // The retry is scheduled; a list is asked for before it comes.
        Api_->Sessions().Refresh();
        QTRY_COMPARE_WITH_TIMEOUT(Daemon_->Lists(), 2, TIMEOUT_MS);
        // The retry would have come in 200 ms; this list is still out.
        QTest::qWait(500);
        QCOMPARE(Daemon_->Lists(), 2);

        // Its answer decides: another retry, one only.
        Daemon_->AnswerList(1, {}, Down());
        QTRY_COMPARE_WITH_TIMEOUT(Daemon_->Lists(), 3, TIMEOUT_MS);
        QTest::qWait(900);
        QCOMPARE(Daemon_->Lists(), 3);

        Daemon_->AnswerList(2, {}, {});
        QTRY_VERIFY_WITH_TIMEOUT(Api_->Sessions().Unavailable().isEmpty(),
                                 TIMEOUT_MS);
        QTest::qWait(900);
        QCOMPARE(Daemon_->Lists(), 3);
    }
};

QTEST_GUILESS_MAIN(ApiUnavailableTest)
#include "api_unavailable_test.moc"
