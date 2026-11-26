// The keymap table of the window (api::KeymapService) against the real roostd
// with a plugin whose backend is the fixture backend and a keymap.json: the
// table is loaded on connect, keymap.changed replaces it, an event of another
// platform is not ours, and a roostd that comes back is asked again. And
// MatchForm, the form in which a chord is compared with a key press.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <memory>

#include "api/api.hpp"
#include "api/keymap_service.hpp"
#include "api_test_support.hpp"
#include "core/key_chord.hpp"
#include "helpers/roostd_process.hpp"

using namespace roost;
using namespace roost::api_test;

namespace {

// A chord written for Linux, as this platform has it: Command takes the place
// of Ctrl on macOS.
QString Spelled(QString text) {
    return HostPlatform() == EPlatform::Mac ? text.replace("Ctrl", "Cmd")
                                            : text;
}

// The chord as the table has it for this platform.
KeyChord Form(const QString& text) {
    const auto chord = ParseKeyChord(Spelled(text), HostPlatform());
    return chord ? api::MatchForm(*chord) : KeyChord{};
}

// An entry of keymap.json, `key` as written for Linux.
QJsonObject Entry(const QString& key, const QString& command = "fixture.echo") {
    return {{"key", key}, {"mac", Spelled(key)}, {"command", command}};
}

}  // namespace

class ApiKeymapTest : public QObject {
    Q_OBJECT

private:
    bool WriteKeymap(const QJsonArray& entries) {
        if (!QDir().mkpath(Daemon_->ConfigPath())) {
            return false;
        }
        QFile file(Daemon_->ConfigPath() + "/keymap.json");
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(QJsonDocument(entries).toJson()) > 0;
    }

    QJsonArray ReadKeymap() {
        QFile file(Daemon_->ConfigPath() + "/keymap.json");
        return file.open(QIODevice::ReadOnly)
                   ? QJsonDocument::fromJson(file.readAll()).array()
                   : QJsonArray();
    }

    bool StartDaemon(const QJsonArray& entries) {
        return Daemon_->IsValid() &&
               Daemon_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH) &&
               WriteKeymap(entries) && Daemon_->Start(ROOSTD_PATH);
    }

    bool ConnectApi() {
        Api_ = std::make_unique<api::Api>(api::ApiOptions{
            .FirstReconnectDelay = std::chrono::milliseconds(20),
            .MaxReconnectDelay = std::chrono::milliseconds(80),
            .FirstRelistDelay = std::chrono::milliseconds(500),
            .MaxRelistDelay = std::chrono::milliseconds(30'000)});
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Api_->Connect(Daemon_->SocketPath());
        return (!connected.isEmpty() || connected.wait(TIMEOUT_MS)) &&
               QTest::qWaitFor([this] { return Api_->Keymap().IsLoaded(); },
                               TIMEOUT_MS);
    }

    const api::KeyBinding* Find(const QString& chord,
                                const QString& when = {}) {
        return Api_->Keymap().Find(Form(chord), when);
    }

    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;

private slots:
    void init() { Daemon_ = std::make_unique<roost::testing::RoostdProcess>(); }

    void cleanup() {
        Api_.reset();
        Daemon_.reset();
    }

    void MatchFormMakesShiftPartOfASymbol_data() {
        QTest::addColumn<QString>("chord");
        QTest::addColumn<QString>("form");
        QTest::newRow("PlusIsShiftEqual") << "Ctrl+Shift+=" << "Ctrl++";
        QTest::newRow("PlusAsWritten") << "Ctrl++" << "Ctrl++";
        QTest::newRow("PlusWithAnExtraShift") << "Ctrl+Shift++" << "Ctrl++";
        QTest::newRow("Digit") << "Ctrl+Shift+4" << "Ctrl+$";
        QTest::newRow("Comma") << "Alt+Shift+," << "Alt+<";
        QTest::newRow("PlainEqualStays") << "Ctrl+=" << "Ctrl+=";
        QTest::newRow("PlainDigitStays") << "Ctrl+4" << "Ctrl+4";
        QTest::newRow("LetterKeepsShift") << "Ctrl+Shift+J" << "Ctrl+Shift+J";
        QTest::newRow("NameKeepsShift") << "Ctrl+Shift+Left"
                                        << "Ctrl+Shift+Left";
        QTest::newRow("FunctionKeepsShift") << "Shift+F5" << "Shift+F5";
        QTest::newRow("SymbolOutsideTheTableKeepsShift")
            << "Ctrl+Shift+É" << "Ctrl+Shift+É";
    }

    void MatchFormMakesShiftPartOfASymbol() {
        QFETCH(QString, chord);
        QFETCH(QString, form);
        QCOMPARE(WriteKeyChord(
                     api::MatchForm(*ParseKeyChord(chord, EPlatform::Linux))),
                 WriteKeyChord(*ParseKeyChord(form, EPlatform::Linux)));
    }

    void TheTableIsLoadedOnConnect() {
        QVERIFY(StartDaemon(
            {Entry("Ctrl+Shift+Y"), QJsonObject{{"key", "J"},
                                                {"mac", "J"},
                                                {"when", "sessions.list"},
                                                {"command", "fixture.echo"},
                                                {"args", QJsonArray{1}}}}));
        QVERIFY(ConnectApi());
        const api::KeyBinding* any = Find("Ctrl+Shift+Y");
        QVERIFY(any != nullptr);
        QCOMPARE(any->Command, QString("fixture.echo"));
        QCOMPARE(any->Source, QString("user"));
        QVERIFY(any->When.isEmpty());
        const api::KeyBinding* inList = Find("J", "sessions.list");
        QVERIFY(inList != nullptr);
        QCOMPARE(inList->Args, QJsonValue(QJsonArray{1}));
        // The pair (key, when): the same key in another panel, or with no
        // panel, is not bound.
        QVERIFY(Find("J", "other.panel") == nullptr);
        QVERIFY(Find("J") == nullptr);
        QVERIFY(Find("Ctrl+Shift+Y", "sessions.list") == nullptr);
    }

    void TwoSpellingsOfAKeyAreOnePair() {
        QVERIFY(StartDaemon({Entry("Ctrl+Shift+=", "fixture.pid"),
                             Entry("Ctrl+=", "fixture.echo")}));
        QVERIFY(ConnectApi());
        for (const char* spelling : {"Ctrl++", "Ctrl+Shift+="}) {
            const api::KeyBinding* binding = Find(spelling);
            QVERIFY2(binding != nullptr, spelling);
            QCOMPARE(binding->Command, QString("fixture.pid"));
        }
        // Without Shift it is another key.
        QCOMPARE(Find("Ctrl+=")->Command, QString("fixture.echo"));
    }

    void KeymapChangedReplacesTheTable() {
        QVERIFY(StartDaemon({Entry("Ctrl+Shift+Y")}));
        QVERIFY(ConnectApi());
        QSignalSpy changed(&Api_->Keymap(), &api::KeymapService::Changed);
        QVERIFY(WriteKeymap({Entry("Ctrl+Shift+U", "fixture.pid")}));
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), TIMEOUT_MS);
        QVERIFY(Find("Ctrl+Shift+Y") == nullptr);
        const api::KeyBinding* now = Find("Ctrl+Shift+U");
        QVERIFY(now != nullptr);
        QCOMPARE(now->Command, QString("fixture.pid"));
    }

    void AnEventOfAnotherPlatformIsNotOurs() {
        QVERIFY(StartDaemon({Entry("Ctrl+Shift+Y")}));
        QVERIFY(ConnectApi());
        const QString own = api::KeymapService::PlatformName();
        const QString other = own == "mac" ? "linux" : "mac";
        const QJsonArray bindings{QJsonObject{{"key", Spelled("Ctrl+Shift+U")},
                                              {"command", "fixture.pid"},
                                              {"source", "user"}}};
        QSignalSpy changed(&Api_->Keymap(), &api::KeymapService::Changed);
        QVERIFY(WaitFor(Api_->Bus().Publish(
            "keymap.changed", QJsonObject{{"platform", other},
                                          {"bindings", bindings},
                                          {"problems", QJsonArray{}}})));
        QVERIFY(WaitFor(Api_->Bus().Publish(
            "keymap.changed", QJsonObject{{"platform", own},
                                          {"bindings", bindings},
                                          {"problems", QJsonArray{}}})));
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, TIMEOUT_MS);
        // Events are in order: the first, which came before, changed nothing.
        QVERIFY(Find("Ctrl+Shift+Y") == nullptr);
        QVERIFY(Find("Ctrl+Shift+U") != nullptr);
    }

    // The window asks roostd to write the file; the table comes back by the
    // event that a hand edit also gives, once, and the call touches nothing
    // itself.
    void SetCommandWritesTheFileAndTheTableFollowsOnce() {
        QVERIFY(StartDaemon({}));
        QVERIFY(ConnectApi());
        QVERIFY(Find("Ctrl+Shift+Y") == nullptr);
        QSignalSpy changed(&Api_->Keymap(), &api::KeymapService::Changed);

        auto done = Api_->Keymap().SetCommand(
            "fixture.echo",
            QJsonArray{QJsonObject{{"key", "Ctrl+Shift+Y"},
                                   {"mac", Spelled("Ctrl+Shift+Y")}}});
        QVERIFY(WaitFor(done));
        QVERIFY2(done.result().has_value(), qPrintable(Why(done.result())));

        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, TIMEOUT_MS);
        const api::KeyBinding* now = Find("Ctrl+Shift+Y");
        QVERIFY(now != nullptr);
        QCOMPARE(now->Command, QString("fixture.echo"));
        QCOMPARE(now->Source, QString("user"));
        QCOMPARE(ReadKeymap().size(), 1);

        // A barrier for "once": a later, different edit, as an editor saves
        // it. Its event comes after any second event of the write above, so
        // when the table has it, the count is final.
        QVERIFY(WriteKeymap({Entry("Ctrl+Shift+U", "fixture.pid")}));
        QTRY_VERIFY_WITH_TIMEOUT(Find("Ctrl+Shift+U") != nullptr, TIMEOUT_MS);
        QCOMPARE(changed.size(), 2);
    }

    void ResetCommandTakesTheKeyAwayAgain() {
        QVERIFY(StartDaemon(
            {Entry("Ctrl+Shift+Y"), Entry("Ctrl+Shift+U", "fixture.pid")}));
        QVERIFY(ConnectApi());
        QVERIFY(Find("Ctrl+Shift+Y") != nullptr);

        auto done = Api_->Keymap().ResetCommand("fixture.echo");
        QVERIFY(WaitFor(done));
        QVERIFY2(done.result().has_value(), qPrintable(Why(done.result())));

        QTRY_VERIFY_WITH_TIMEOUT(Find("Ctrl+Shift+Y") == nullptr, TIMEOUT_MS);
        // The entry of the other command stays.
        QVERIFY(Find("Ctrl+Shift+U") != nullptr);
    }

    // roostd's refusals pass unchanged, and the file stays.
    void SetCommandPassesTheErrorsOfRoostd() {
        QVERIFY(StartDaemon({}));
        QVERIFY(ConnectApi());
        auto badKey = Api_->Keymap().SetCommand(
            "fixture.echo", QJsonArray{QJsonObject{{"key", "Ctrl+Nonsense"}}});
        QVERIFY(WaitFor(badKey));
        QVERIFY(!badKey.result());
        QCOMPARE(badKey.result().error().Code,
                 static_cast<int>(rpc::EErrorCode::InvalidParams));

        QFile file(Daemon_->ConfigPath() + "/keymap.json");
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write("[ {") > 0);
        file.close();
        auto broken = Api_->Keymap().SetCommand(
            "fixture.echo", QJsonArray{QJsonObject{{"key", "Ctrl+Shift+Y"}}});
        QVERIFY(WaitFor(broken));
        QVERIFY(!broken.result());
        QCOMPARE(broken.result().error().Code, -32004);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("[ {"));
    }

    void ARoostdThatComesBackIsAskedAgain() {
        QVERIFY(StartDaemon({Entry("Ctrl+Shift+Y")}));
        QVERIFY(ConnectApi());
        QSignalSpy disconnected(Api_.get(), &api::Api::Disconnected);
        QVERIFY(Daemon_->Stop());
        QVERIFY(disconnected.size() > 0 || disconnected.wait(TIMEOUT_MS));
        // No key runs a command of a roostd that is gone.
        QVERIFY(!Api_->Keymap().IsLoaded());
        QVERIFY(Find("Ctrl+Shift+Y") == nullptr);
        // The file changed while it was gone.
        QVERIFY(WriteKeymap({Entry("Ctrl+Shift+U", "fixture.pid")}));
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(connected.size() > 0 || connected.wait(TIMEOUT_MS));
        QTRY_VERIFY_WITH_TIMEOUT(Api_->Keymap().IsLoaded(), TIMEOUT_MS);
        QVERIFY(Find("Ctrl+Shift+Y") == nullptr);
        QVERIFY(Find("Ctrl+Shift+U") != nullptr);
        // Still kept fresh after the reconnect: one subscription, one event.
        QSignalSpy changed(&Api_->Keymap(), &api::KeymapService::Changed);
        QVERIFY(WriteKeymap({Entry("Ctrl+Shift+I", "fixture.pid")}));
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, TIMEOUT_MS);
        QTest::qWait(300);
        QCOMPARE(changed.size(), 1);
        QVERIFY(Find("Ctrl+Shift+I") != nullptr);
    }
};

QTEST_GUILESS_MAIN(ApiKeymapTest)
#include "api_keymap_test.moc"
