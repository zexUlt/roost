#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTest>

#include <memory>

#include "core/protocol.hpp"
#include "core/rpc_client.hpp"
#include "helpers/roostd_process.hpp"

using namespace roost;

namespace {

constexpr int TIMEOUT_MS = 10000;
// For the checks that nothing more comes, after an event that shows the daemon
// has handled the change: longer than the delay of the service (100 ms) by a
// good margin, so what would come after the first event is in by then. A state
// that is expected is awaited with QTRY, not with this.
constexpr int SETTLE_MS = 600;
// The three writes of a chunked file are within the delay of the service (100
// ms) when they took less than this.
constexpr qint64 CHUNKS_WITHIN_MS = 40;
const QJsonValue UNBIND{QJsonValue::Null};

QJsonObject Binding(const QString& key, const QString& command,
                    const QString& source, const QString& when = {},
                    const QJsonValue& args = QJsonValue::Undefined) {
    QJsonObject binding{{"key", key}, {"command", command}, {"source", source}};
    if (!when.isEmpty()) {
        binding.insert("when", when);
    }
    if (!args.isUndefined()) {
        binding.insert("args", args);
    }
    return binding;
}

QJsonObject Plugin(const QString& id, const QJsonArray& commands,
                   const QJsonArray& keybindings) {
    QJsonArray declared;
    for (const QJsonValue& command : commands) {
        declared.append(QJsonObject{{"id", command}, {"title", command}});
    }
    return {{"id", id},
            {"version", "1"},
            {"api", "0"},
            {"contributes", QJsonObject{{"commands", declared},
                                        {"keybindings", keybindings}}}};
}

}  // namespace

// The real roostd, with two plugins that bind keys, over a temporary
// ROOST_CONFIG.
class KeymapServiceTest : public QObject {
    Q_OBJECT

private:
    QString KeymapPath() const {
        return Daemon_->ConfigPath() + "/keymap.json";
    }

    bool WritePlugin(const QJsonObject& manifest) {
        const QDir directory(Daemon_->PluginsPath() + '/' +
                             manifest.value("id").toString());
        QFile file(directory.filePath("plugin.json"));
        return directory.mkpath(".") && file.open(QIODevice::WriteOnly) &&
               file.write(QJsonDocument(manifest).toJson()) > 0;
    }

    // As an editor saves: a temporary file in the directory, renamed over it.
    bool SaveFile(const QByteArray& bytes) {
        QSaveFile file(KeymapPath());
        return file.open(QIODevice::WriteOnly) && file.write(bytes) > 0 &&
               file.commit();
    }
    bool SaveFile(const QJsonArray& entries) {
        return SaveFile(QJsonDocument(entries).toJson());
    }

    // "alpha" is first in load order, "beta" second.
    bool WritePlugins() {
        QDir().mkpath(Daemon_->ConfigPath());
        return WritePlugin(Plugin("alpha",
                                  {"alpha.new", "alpha.next", "alpha.save"},
                                  {QJsonObject{{"command", "alpha.new"},
                                               {"key", "Ctrl+Shift+N"},
                                               {"mac", "Cmd+N"}},
                                   QJsonObject{{"command", "alpha.next"},
                                               {"key", "J"},
                                               {"when", "sessions.list"}},
                                   QJsonObject{{"command", "alpha.save"},
                                               {"key", "Ctrl+S"}}})) &&
               WritePlugin(Plugin(
                   "beta", {"beta.new", "beta.next", "beta.mac"},
                   {QJsonObject{{"command", "beta.new"},
                                {"key", "Shift+Ctrl+N"}},
                    QJsonObject{{"command", "beta.next"},
                                {"key", "J"},
                                {"when", "beta.transcript"}},
                    QJsonObject{{"command", "beta.mac"}, {"mac", "Cmd+B"}}}));
    }

    bool Connect() {
        Client_ = std::make_unique<RpcClient>();
        QFuture<rpc::Result> hello = Client_->Connect(Daemon_->SocketPath());
        if (!QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS) ||
            !hello.result()) {
            return false;
        }
        Events_ = std::make_unique<QSignalSpy>(
            Client_.get(), &RpcClient::NotificationReceived);
        return Call("bus.subscribe", {{"pattern", "keymap.*"}}).has_value();
    }

    rpc::Result Call(const QString& method, const QJsonObject& params) {
        QFuture<rpc::Result> future = Client_->Request(method, params);
        if (!QTest::qWaitFor([&] { return future.isFinished(); }, TIMEOUT_MS)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return future.result();
    }

    // What keymap.get answers for the platform.
    QJsonObject Get(const QString& platform) {
        const rpc::Result answer = Call("keymap.get", {{"platform", platform}});
        if (!answer) {
            qWarning().noquote() << "keymap.get:" << answer.error().Message;
            return {};
        }
        return answer->toObject();
    }

    // The payloads of the keymap.changed events so far.
    QList<QJsonObject> Changes() const {
        QList<QJsonObject> changes;
        for (const QList<QVariant>& notification : *Events_) {
            const QJsonObject event =
                notification[1].value<QJsonValue>().toObject();
            if (notification[0] == "bus.event" &&
                event.value("name") == "keymap.changed") {
                changes.append(event.value("payload").toObject());
            }
        }
        return changes;
    }

    // The problems that mention `text`.
    static QStringList ProblemsWith(const QJsonObject& table,
                                    const QString& text) {
        QStringList found;
        for (const QJsonValue& problem : table.value("problems").toArray()) {
            if (problem.toString().contains(text)) {
                found.append(problem.toString());
            }
        }
        return found;
    }

    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;
    std::unique_ptr<RpcClient> Client_;
    std::unique_ptr<QSignalSpy> Events_;

private slots:
    void init() {
        Daemon_ = std::make_unique<roost::testing::RoostdProcess>();
        QVERIFY(Daemon_->IsValid());
        QVERIFY(WritePlugins());
    }

    void cleanup() {
        Events_.reset();
        Client_.reset();
        Daemon_.reset();
    }

    // No file: the keys of the plugins, chosen for the platform that asks.
    void NoFileHasThePluginsKeysOnly() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());

        const QJsonObject linux = Get("linux");
        QCOMPARE(linux.value("bindings").toArray(),
                 (QJsonArray{
                     Binding("Ctrl+Shift+N", "alpha.new", "alpha"),
                     Binding("J", "alpha.next", "alpha", "sessions.list"),
                     Binding("Ctrl+S", "alpha.save", "alpha"),
                     Binding("J", "beta.next", "beta", "beta.transcript"),
                 }));
        // beta.new loses the pair to alpha.new; beta.mac is for the Mac only.
        QCOMPARE(linux.value("problems").toArray().size(), 1);
        QVERIFY(ProblemsWith(linux, "Ctrl+Shift+N of plugin beta is ignored")
                    .size() == 1);

        const QJsonObject mac = Get("mac");
        QCOMPARE(mac.value("bindings").toArray(),
                 (QJsonArray{
                     Binding("Cmd+N", "alpha.new", "alpha"),
                     Binding("J", "alpha.next", "alpha", "sessions.list"),
                     Binding("Ctrl+S", "alpha.save", "alpha"),
                     Binding("Ctrl+Shift+N", "beta.new", "beta"),
                     Binding("J", "beta.next", "beta", "beta.transcript"),
                     Binding("Cmd+B", "beta.mac", "beta"),
                 }));
        QCOMPARE(mac.value("problems").toArray().size(), 0);
        QVERIFY(Changes().isEmpty());
    }

    void PlatformMustBeMacOrLinux() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());
        for (const QJsonObject& params :
             {QJsonObject{}, QJsonObject{{"platform", "windows"}},
              QJsonObject{{"platform", 1}}}) {
            const rpc::Result answer = Call("keymap.get", params);
            QVERIFY(!answer);
            QCOMPARE(answer.error().Code,
                     static_cast<int>(rpc::EErrorCode::InvalidParams));
        }
    }

    // The pair (key, when) has one entry: the file over the plugins, and of
    // two entries of the file the lower.
    void FileEntryOverridesAContributionAndTheLowerEntryWins() {
        QVERIFY(SaveFile(QJsonArray{
            QJsonObject{{"key", "Ctrl+S"}, {"command", "beta.next"}},
            // The same key as Ctrl+S, written another way: the same pair.
            QJsonObject{{"key", "ctrl+s"},
                        {"command", "alpha.new"},
                        {"args", QJsonObject{{"category", "work"}}}},
            // Another panel: its own pair, next to the plugins' J.
            QJsonObject{
                {"key", "J"}, {"command", "alpha.next"}, {"when", "other"}},
        }));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());

        const QJsonObject table = Get("linux");
        QCOMPARE(table.value("bindings").toArray(),
                 (QJsonArray{
                     Binding("Ctrl+Shift+N", "alpha.new", "alpha"),
                     Binding("J", "alpha.next", "alpha", "sessions.list"),
                     Binding("Ctrl+S", "alpha.new", "user", {},
                             QJsonObject{{"category", "work"}}),
                     Binding("J", "beta.next", "beta", "beta.transcript"),
                     Binding("J", "alpha.next", "user", "other"),
                 }));
        // A contribution that the file covers is no problem.
        QCOMPARE(table.value("problems").toArray().size(), 1);
    }

    // `command: null` removes the pair with the same `when`, at that point of
    // the file: the entries above go, the ones below stay.
    void NullRemovesTheSamePairOnly() {
        QVERIFY(SaveFile(QJsonArray{
            // Another `when` than the key has: nothing to remove.
            QJsonObject{{"key", "Ctrl+S"},
                        {"when", "sessions.list"},
                        {"command", UNBIND}},
            // For the Mac only.
            QJsonObject{{"mac", "Cmd+N"}, {"command", UNBIND}},
            // The J of alpha: the J of beta is in another panel.
            QJsonObject{
                {"key", "J"}, {"when", "sessions.list"}, {"command", UNBIND}},
            // An entry of the file above the one that removes it, and one
            // below that binds the key again.
            QJsonObject{{"key", "Ctrl+K"}, {"command", "alpha.new"}},
            QJsonObject{{"key", "Ctrl+K"}, {"command", UNBIND}},
            QJsonObject{{"key", "Ctrl+L"}, {"command", "alpha.new"}},
            QJsonObject{{"key", "Ctrl+L"}, {"command", UNBIND}},
            QJsonObject{{"key", "Ctrl+L"}, {"command", "alpha.save"}},
        }));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());

        QCOMPARE(Get("linux").value("bindings").toArray(),
                 (QJsonArray{
                     Binding("Ctrl+Shift+N", "alpha.new", "alpha"),
                     Binding("Ctrl+S", "alpha.save", "alpha"),
                     Binding("J", "beta.next", "beta", "beta.transcript"),
                     Binding("Ctrl+L", "alpha.save", "user"),
                 }));
        const QJsonObject mac = Get("mac");
        QCOMPARE(mac.value("bindings").toArray(),
                 (QJsonArray{
                     Binding("Ctrl+S", "alpha.save", "alpha"),
                     Binding("Ctrl+Shift+N", "beta.new", "beta"),
                     Binding("J", "beta.next", "beta", "beta.transcript"),
                     Binding("Cmd+B", "beta.mac", "beta"),
                     Binding("Ctrl+L", "alpha.save", "user"),
                 }));
        QCOMPARE(mac.value("problems").toArray().size(), 0);
    }

    // A removal binds nothing: a bare key is no problem for it, and it takes
    // away the contribution that has the same pair.
    void RemovalOfABareKeyIsNoProblem() {
        QVERIFY(SaveFile(QJsonArray{
            QJsonObject{{"key", "J"}, {"command", UNBIND}},
            QJsonObject{
                {"key", "J"}, {"when", "sessions.list"}, {"command", UNBIND}},
        }));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());

        const QJsonObject table = Get("linux");
        QCOMPARE(ProblemsWith(table, "keymap.json entry").size(), 0);
        QCOMPARE(table.value("bindings").toArray(),
                 (QJsonArray{
                     Binding("Ctrl+Shift+N", "alpha.new", "alpha"),
                     Binding("Ctrl+S", "alpha.save", "alpha"),
                     Binding("J", "beta.next", "beta", "beta.transcript"),
                 }));
    }

    void BrokenEntriesAreSkippedWithAProblem() {
        QVERIFY(SaveFile(QJsonArray{
            "text",
            QJsonObject{{"command", "alpha.new"}},
            QJsonObject{{"key", 5}, {"command", "alpha.new"}},
            QJsonObject{{"key", "Ctrl+Shift+Q"}},
            QJsonObject{{"key", "Ctrl+Shift+Q"}, {"command", "nobody.has"}},
            QJsonObject{{"key", "Ctrl+Hyper"}, {"command", "alpha.new"}},
            QJsonObject{{"key", "Cmd+Q"}, {"command", "alpha.new"}},
            QJsonObject{
                {"key", "Ctrl+Q"}, {"when", 3}, {"command", "alpha.new"}},
            QJsonObject{{"key", "Q"}, {"command", "alpha.new"}},
            // For the Mac only: nothing on Linux, and no problem.
            QJsonObject{{"mac", "Cmd+Q"}, {"command", "alpha.new"}},
            // Good, after the broken ones.
            QJsonObject{{"key", "F5"}, {"command", "alpha.save"}},
            QJsonObject{{"key", "Q"},
                        {"when", "sessions.list"},
                        {"command", "alpha.save"}},
        }));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());

        const QJsonObject linux = Get("linux");
        const QJsonArray bindings = linux.value("bindings").toArray();
        QCOMPARE(bindings.size(), 6);
        QCOMPARE(bindings[4], QJsonValue(Binding("F5", "alpha.save", "user")));
        QCOMPARE(bindings[5], QJsonValue(Binding("Q", "alpha.save", "user",
                                                 "sessions.list")));
        // Entries 0-8 are broken, 9 is for the Mac, 10 and 11 are good.
        QCOMPARE(ProblemsWith(linux, "keymap.json entry").size(), 9);
        const QStringList expected{
            "entry 0 is ignored: it is not an object",
            "entry 1 is ignored: it has neither \"key\" nor \"mac\"",
            "entry 2 is ignored: \"key\" is not a string",
            "entry 3 is ignored: \"command\" is neither a string nor null",
            "entry 4 is ignored: no plugin has the command nobody.has",
            "entry 5 is ignored: \"key\":",
            "entry 6 is ignored: \"key\":",
            "entry 7 is ignored: \"when\" is not a string",
            "entry 8 is ignored: Q has no Cmd, Ctrl or Alt",
        };
        for (const QString& text : expected) {
            QCOMPARE(ProblemsWith(linux, text).size(), 1);
        }
        // The Mac has the same broken entries, and the entry 9 is good there.
        const QJsonObject mac = Get("mac");
        QCOMPARE(ProblemsWith(mac, "keymap.json entry").size(), 9);
        QCOMPARE(mac.value("bindings").toArray().size(), 9);
        QCOMPARE(mac.value("bindings").toArray()[6],
                 QJsonValue(Binding("Cmd+Q", "alpha.new", "user")));
    }

    // After a save by rename, the editor's way, one event after the delay,
    // with the table of each platform that a window asked for.
    void SavingByRenameGivesOneEventForEachPlatform() {
        QVERIFY(SaveFile(QJsonArray{
            QJsonObject{{"key", "Ctrl+S"}, {"command", "alpha.new"}}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());
        const QJsonObject before = Get("linux");
        QCOMPARE(Get("mac").value("bindings").toArray().size(), 6);

        QVERIFY(SaveFile(QJsonArray{
            QJsonObject{{"key", "Ctrl+S"}, {"command", "beta.new"}}}));
        QTRY_COMPARE_WITH_TIMEOUT(Changes().size(), 2, TIMEOUT_MS);
        QTest::qWait(SETTLE_MS);
        QCOMPARE(Changes().size(), 2);
        QStringList platforms;
        for (const QJsonObject& change : Changes()) {
            platforms.append(change.value("platform").toString());
        }
        platforms.sort();
        QCOMPARE(platforms, (QStringList{"linux", "mac"}));
        for (QJsonObject change : Changes()) {
            const QString platform = change.value("platform").toString();
            change.remove("platform");
            QCOMPARE(change, Get(platform));
            QVERIFY(change.value("bindings")
                        .toArray()
                        .contains(Binding("Ctrl+S", "beta.new", "user")));
        }
        QVERIFY(Get("linux") != before);

        // A second save is seen too: the file that replaced the old one is
        // watched again.
        QVERIFY(SaveFile(QJsonArray{
            QJsonObject{{"key", "Ctrl+S"}, {"command", "alpha.next"}}}));
        QTRY_COMPARE_WITH_TIMEOUT(Changes().size(), 4, TIMEOUT_MS);
        QTest::qWait(SETTLE_MS);
        QCOMPARE(Changes().size(), 4);
    }

    // A writer that writes in place and not by rename, in chunks that come
    // within the delay of the service: there is one event, for the whole file.
    // Whether the chunks came within the delay depends on the writer being on
    // the CPU, so the writer times itself: a round in which it took long is not
    // the case under test and is done again, until the deadline.
    void ChunksWithinTheDelayGiveOneEvent() {
        QVERIFY(SaveFile(QJsonArray{}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());
        Get("linux");

        const QByteArray whole =
            R"([{"key": "Ctrl+S", "command": "beta.new"}])";
        const qsizetype third = whole.size() / 3;
        const QJsonObject wholeBinding = Binding("Ctrl+S", "beta.new", "user");
        const QJsonObject closing = Binding("Ctrl+S", "alpha.next", "user");
        QElapsedTimer deadline;
        deadline.start();
        while (!deadline.hasExpired(TIMEOUT_MS)) {
            const qsizetype before = Changes().size();
            QElapsedTimer writing;
            writing.start();
            QFile file(KeymapPath());
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            for (const QByteArray& chunk :
                 {whole.left(third), whole.mid(third, third),
                  whole.mid(2 * third)}) {
                QCOMPARE(file.write(chunk), qsizetype(chunk.size()));
                QVERIFY(file.flush());
            }
            file.close();
            const bool withinTheDelay = writing.elapsed() < CHUNKS_WITHIN_MS;

            // The event of the writes, then the event of a later save by
            // rename: it comes after anything the writes would still give, so
            // when it is in, the writes are done with.
            QTRY_VERIFY_WITH_TIMEOUT(Changes().size() > before, TIMEOUT_MS);
            QVERIFY(SaveFile(QJsonArray{
                QJsonObject{{"key", "Ctrl+S"}, {"command", "alpha.next"}}}));
            QTRY_VERIFY_WITH_TIMEOUT(
                Changes().size() > before + 1 && Changes()
                                                     .last()
                                                     .value("bindings")
                                                     .toArray()
                                                     .contains(closing),
                TIMEOUT_MS);
            if (!withinTheDelay) {
                continue;
            }
            QCOMPARE(Changes().size(), before + 2);
            QVERIFY(Changes()[before]
                        .value("bindings")
                        .toArray()
                        .contains(wholeBinding));
            QCOMPARE(ProblemsWith(Changes()[before], "keymap.json is ignored")
                         .size(),
                     0);
            return;
        }
        QFAIL("the writer was never fast enough to keep within the delay");
    }

    // A writer that pauses between the chunks for longer than the delay may
    // give an event for the half of the file; the half keeps the version
    // before it, and the last event has the whole file.
    void ASlowWriteEndsWithTheWholeFile() {
        QVERIFY(SaveFile(QJsonArray{
            QJsonObject{{"key", "Ctrl+L"}, {"command", "alpha.save"}}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());
        Get("linux");

        QFile file(KeymapPath());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray whole =
            R"([{"key": "Ctrl+S", "command": "beta.new"}])";
        for (qsizetype from = 0; from < whole.size(); from += 10) {
            QCOMPARE(file.write(whole.mid(from, 10)),
                     qsizetype(whole.mid(from, 10).size()));
            QVERIFY(file.flush());
            QTest::qWait(30);
        }
        file.close();
        const auto hasWhole = [this] {
            return !Changes().isEmpty() &&
                   Changes()
                       .last()
                       .value("bindings")
                       .toArray()
                       .contains(Binding("Ctrl+S", "beta.new", "user"));
        };
        QTRY_VERIFY_WITH_TIMEOUT(hasWhole(), TIMEOUT_MS);
        QTest::qWait(SETTLE_MS);
        QVERIFY(hasWhole());
        QCOMPARE(
            ProblemsWith(Changes().last(), "keymap.json is ignored").size(), 0);
        // An event before it is the half of the file: the old table stays.
        for (const QJsonObject& change :
             Changes().first(Changes().size() - 1)) {
            QVERIFY(change.value("bindings")
                        .toArray()
                        .contains(Binding("Ctrl+L", "alpha.save", "user")));
            QCOMPARE(ProblemsWith(change, "the version before it stays").size(),
                     1);
        }
    }

    // The table of a platform that no window asked for is not published.
    void NoEventForAPlatformNobodyAskedFor() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());
        QCOMPARE(Get("linux").value("problems").toArray().size(), 1);
        QVERIFY(SaveFile(QJsonArray{
            QJsonObject{{"key", "Ctrl+S"}, {"command", "beta.new"}}}));
        QTRY_COMPARE_WITH_TIMEOUT(Changes().size(), 1, TIMEOUT_MS);
        QTest::qWait(SETTLE_MS);
        QCOMPARE(Changes().size(), 1);
        QCOMPARE(Changes()[0].value("platform"), QJsonValue("linux"));
    }

    // Rewriting the file with what it had says nothing, and neither do the
    // other documents of the directory.
    void ChangesThatLeaveTheFileAsItWasAreNotPublished() {
        const QJsonArray entries{
            QJsonObject{{"key", "Ctrl+S"}, {"command", "beta.new"}}};
        QVERIFY(SaveFile(entries));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());
        Get("linux");
        QVERIFY(SaveFile(entries));
        QVERIFY(Call("store.set", {{"plugin", "alpha"},
                                   {"document", QJsonObject{{"a", 1}}}}));
        QTest::qWait(SETTLE_MS);
        QVERIFY(Changes().isEmpty());
        // And the watch is alive.
        QVERIFY(SaveFile(QJsonArray{}));
        QTRY_COMPARE_WITH_TIMEOUT(Changes().size(), 1, TIMEOUT_MS);
    }

    // A file that cannot be used leaves the table of the version before it, and
    // the problem goes with the next good one.
    void BrokenFileKeepsThePreviousTableAndReportsAProblem_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<QString>("problem");
        QTest::newRow("not JSON")
            << QByteArray("[{\"key\": ") << "it is not JSON";
        QTest::newRow("not an array")
            << QByteArray("{\"key\": \"J\"}") << "it is not an array";
        // Valid JSON, one byte over the limit.
        QByteArray big("[");
        big.append(QByteArray(1024 * 1024 - 1, ' ')).append(']');
        QTest::newRow("over 1 MiB") << big << "it is over 1 MiB";
        QByteArray limit("[");
        limit.append(QByteArray(1024 * 1024 - 2, ' ')).append(']');
        QTest::newRow("exactly 1 MiB is read") << limit << QString();
    }

    void BrokenFileKeepsThePreviousTableAndReportsAProblem() {
        QFETCH(QByteArray, bytes);
        QFETCH(QString, problem);
        QVERIFY(SaveFile(QJsonArray{
            QJsonObject{{"key", "Ctrl+S"}, {"command", "beta.new"}}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());
        const QJsonObject good = Get("linux");
        QVERIFY(good.value("bindings")
                    .toArray()
                    .contains(Binding("Ctrl+S", "beta.new", "user")));

        QVERIFY(SaveFile(bytes));
        QTRY_COMPARE_WITH_TIMEOUT(Changes().size(), 1, TIMEOUT_MS);
        const QJsonObject changed = Changes()[0];
        if (problem.isEmpty()) {
            // Whitespace only around an empty array: an empty file.
            QVERIFY(ProblemsWith(changed, "keymap.json is ignored").isEmpty());
            QVERIFY(!changed.value("bindings")
                         .toArray()
                         .contains(Binding("Ctrl+S", "beta.new", "user")));
            return;
        }
        QCOMPARE(changed.value("bindings"), good.value("bindings"));
        const QStringList found = ProblemsWith(changed, problem);
        QCOMPARE(found.size(), 1);
        QVERIFY(found[0].startsWith("keymap.json is ignored: "));
        QVERIFY(found[0].endsWith("; the version before it stays"));
        QJsonObject withoutPlatform = changed;
        withoutPlatform.remove("platform");
        QCOMPARE(Get("linux"), withoutPlatform);

        // The next good file brings back the table and clears the problem.
        QVERIFY(SaveFile(QJsonArray{}));
        QTRY_COMPARE_WITH_TIMEOUT(Changes().size(), 2, TIMEOUT_MS);
        QCOMPARE(ProblemsWith(Changes()[1], "keymap.json is ignored").size(),
                 0);
        QVERIFY(!Changes()[1]
                     .value("bindings")
                     .toArray()
                     .contains(Binding("Ctrl+S", "beta.new", "user")));
    }

    // At start there is no version before: the plugins' keys stand alone.
    void BrokenFileAtStartLeavesThePluginsKeys() {
        QVERIFY(SaveFile(QByteArray("not json")));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());
        const QJsonObject table = Get("linux");
        QCOMPARE(table.value("bindings").toArray().size(), 4);
        const QStringList found = ProblemsWith(table, "keymap.json is ignored");
        QCOMPARE(found.size(), 1);
        QVERIFY(!found[0].contains("stays"));
    }

    // The file made after the start, in a directory made after it, and
    // deleted: the plugins' keys stand alone again.
    void FileComesAndGoes() {
        QVERIFY(QDir(Daemon_->ConfigPath()).removeRecursively());
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());
        QCOMPARE(Get("linux").value("bindings").toArray().size(), 4);

        QVERIFY(QDir().mkpath(Daemon_->ConfigPath()));
        QVERIFY(SaveFile(QJsonArray{
            QJsonObject{{"key", "Ctrl+S"}, {"command", "beta.new"}}}));
        QTRY_COMPARE_WITH_TIMEOUT(Changes().size(), 1, TIMEOUT_MS);
        QVERIFY(Changes()[0]
                    .value("bindings")
                    .toArray()
                    .contains(Binding("Ctrl+S", "beta.new", "user")));

        QVERIFY(QFile::remove(KeymapPath()));
        QTRY_COMPARE_WITH_TIMEOUT(Changes().size(), 2, TIMEOUT_MS);
        QCOMPARE(Changes()[1].value("bindings").toArray().size(), 4);
        QTest::qWait(SETTLE_MS);
        QCOMPARE(Changes().size(), 2);
    }

    // A file under the limit whose table does not fit a frame (a problem for
    // each of its entries): the answer is InternalError, and the daemon lives.
    void TableThatDoesNotFitAFrameIsInternalError() {
        QByteArray bytes("[");
        while (bytes.size() < 1024 * 1024 - 8) {
            bytes.append("{},");
        }
        bytes.append("{}]");
        QVERIFY(bytes.size() <= 1024 * 1024);
        QVERIFY(SaveFile(bytes));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());
        const rpc::Result answer = Call("keymap.get", {{"platform", "linux"}});
        QVERIFY(!answer);
        QCOMPARE(answer.error().Code,
                 static_cast<int>(rpc::EErrorCode::InternalError));

        // Nothing is published for it either; the next good file is.
        QCOMPARE(Changes().size(), 0);
        QVERIFY(SaveFile(QJsonArray{}));
        QTRY_VERIFY_WITH_TIMEOUT(Call("keymap.get", {{"platform", "linux"}}),
                                 TIMEOUT_MS);
    }

    // The platform is remembered from a good answer; then the file grows to a
    // table that does not fit an event. It is not published, the daemon keeps
    // answering, and the next good file is published.
    void EventThatDoesNotFitAFrameIsNotPublished() {
        QVERIFY(SaveFile(QJsonArray{}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());
        QVERIFY(!Get("linux").isEmpty());

        QByteArray bytes("[");
        while (bytes.size() < 1024 * 1024 - 8) {
            bytes.append("{},");
        }
        bytes.append("{}]");
        QVERIFY(bytes.size() <= 1024 * 1024);
        QVERIFY(SaveFile(bytes));
        // The daemon has read the file once keymap.get says the table is too
        // big; any other failure (a timeout, a lost connection) is not that.
        // An event of the change would go out in the same step, on the same
        // connection, before this answer.
        rpc::Result answer;
        QTRY_VERIFY_WITH_TIMEOUT(
            !(answer = Call("keymap.get", {{"platform", "linux"}})),
            TIMEOUT_MS);
        QCOMPARE(answer.error().Code,
                 static_cast<int>(rpc::EErrorCode::InternalError));
        QCOMPARE(answer.error().Message,
                 QString("the keymap is too big for a frame"));
        QCOMPARE(Changes().size(), 0);

        QVERIFY(SaveFile(QJsonArray{}));
        QTRY_COMPARE_WITH_TIMEOUT(Changes().size(), 1, TIMEOUT_MS);
        QCOMPARE(Changes()[0].value("platform"), QJsonValue("linux"));
    }

    // What a connection asked for is forgotten when it closes: the platform
    // that nobody asks for any more is not published.
    void ClosedConnectionIsForgotten() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Connect());
        QVERIFY(!Get("mac").isEmpty());
        Events_.reset();
        Client_.reset();

        // A second connection asks for the other platform. roostd says nothing
        // when it has seen the first connection go, so each change is a probe:
        // the daemon publishes the platforms in the order mac, linux, in one
        // step, so the event for linux, which this connection asked for, is the
        // last of the change, and a mac event before it shows that the daemon
        // still remembers the first connection. Then the next change tries
        // again, until the deadline.
        QVERIFY(Connect());
        QVERIFY(!Get("linux").isEmpty());
        QElapsedTimer deadline;
        deadline.start();
        for (int change = 0; !deadline.hasExpired(TIMEOUT_MS); ++change) {
            const qsizetype before = Changes().size();
            // Each file is not the one before it, or nothing is published.
            QVERIFY(SaveFile(change % 2 == 0 ? QJsonArray{QJsonObject{
                                                   {"key", "Ctrl+S"},
                                                   {"command", "beta.new"}}}
                                             : QJsonArray{}));
            QTRY_VERIFY_WITH_TIMEOUT(
                Changes().size() > before &&
                    Changes().last().value("platform") == QJsonValue("linux"),
                TIMEOUT_MS);
            if (Changes().size() == before + 1) {
                return;
            }
        }
        QFAIL("the daemon still publishes for the closed connection");
    }
};

QTEST_GUILESS_MAIN(KeymapServiceTest)
#include "keymap_service_test.moc"
