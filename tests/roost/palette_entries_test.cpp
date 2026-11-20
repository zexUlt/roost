// PaletteModel::BuildEntries on hand-written plugins.list and keymap tables:
// the mark "captures input" for both platforms, whatever the host, and the
// text of the conflicts. Pure data: no window, no daemon.

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

#include <vector>

#include "roost/palette_model.hpp"

using namespace roost;

namespace {

QJsonObject Plugin(const QString& id, const QStringList& commands,
                   const QJsonArray& keybindings = {}) {
    QJsonArray declared;
    for (const QString& command : commands) {
        declared.append(QJsonObject{{"id", command}, {"title", command}});
    }
    return {{"id", id},
            {"contributes", QJsonObject{{"commands", declared},
                                        {"keybindings", keybindings}}}};
}

QJsonObject Contribution(const QString& command, const QString& key) {
    return {{"command", command}, {"key", key}};
}

QJsonObject Row(const QString& key, const QString& command,
                const QString& source, const QString& when = {}) {
    QJsonObject row{{"key", key}, {"command", command}, {"source", source}};
    if (!when.isEmpty()) {
        row.insert("when", when);
    }
    return row;
}

QJsonObject Plugins(const QJsonArray& plugins) {
    return {{"plugins", plugins}};
}

QJsonObject Table(const QJsonArray& bindings) {
    return {{"bindings", bindings}};
}

const PaletteModel::Entry* Find(const std::vector<PaletteModel::Entry>& entries,
                                const QString& command) {
    for (const PaletteModel::Entry& entry : entries) {
        if (entry.CommandId == command) {
            return &entry;
        }
    }
    return nullptr;
}

int Count(const QString& text, const QString& part) {
    return static_cast<int>(text.count(part));
}

}  // namespace

class PaletteEntriesTest : public QObject {
    Q_OBJECT

private slots:
    // Ctrl with a letter and, on macOS, Alt with a letter are what the
    // terminal needs, if the key is active in the terminal.
    void CapturesInput_data() {
        QTest::addColumn<int>("platform");
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("when");
        QTest::addColumn<bool>("captures");

        const int mac = static_cast<int>(EPlatform::Mac);
        const int linux = static_cast<int>(EPlatform::Linux);
        QTest::newRow("mac Alt+K") << mac << "Alt+K" << "" << true;
        QTest::newRow("linux Alt+K") << linux << "Alt+K" << "" << false;
        QTest::newRow("linux Ctrl+K") << linux << "Ctrl+K" << "" << true;
        QTest::newRow("mac Ctrl+K") << mac << "Ctrl+K" << "" << true;
        QTest::newRow("linux Ctrl+K in the terminal")
            << linux << "Ctrl+K" << "terminal.view" << true;
        QTest::newRow("mac Alt+K in the terminal")
            << mac << "Alt+K" << "terminal.view" << true;
        QTest::newRow("linux Ctrl+K in another panel")
            << linux << "Ctrl+K" << "sessions.list" << false;
        QTest::newRow("mac Alt+K in another panel")
            << mac << "Alt+K" << "sessions.list" << false;
        QTest::newRow("linux Ctrl+Backslash")
            << linux << "Ctrl+\\" << "" << false;
        QTest::newRow("mac Alt+Backslash") << mac << "Alt+\\" << "" << false;
        QTest::newRow("mac Cmd+K") << mac << "Cmd+K" << "" << false;
    }

    void CapturesInput() {
        QFETCH(int, platform);
        QFETCH(QString, key);
        QFETCH(QString, when);
        QFETCH(bool, captures);
        const auto entries =
            PaletteModel::BuildEntries(Plugins({Plugin("pa", {"pa.run"})}),
                                       Table({Row(key, "pa.run", "pa", when)}),
                                       static_cast<EPlatform>(platform));
        const PaletteModel::Entry* entry = Find(entries, "pa.run");
        QVERIFY(entry != nullptr);
        QCOMPARE(entry->CapturesInput, captures);
    }

    // A command with a key that captures and one that does not is marked.
    void OneKeyOfTwoIsEnoughToCapture() {
        const auto entries = PaletteModel::BuildEntries(
            Plugins({Plugin("pa", {"pa.run"})}),
            Table({Row("Ctrl+K", "pa.run", "pa", "sessions.list"),
                   Row("Ctrl+J", "pa.run", "pa")}),
            EPlatform::Linux);
        QCOMPARE(Find(entries, "pa.run")->CapturesInput, true);
    }

    // Three plugins bind one key: the one loaded first keeps it. Each row of
    // the conflict says it once, and names every plugin once.
    void AConflictOfThreeNamesEveryLoserOnce() {
        const QString key = "Ctrl+Alt+K";
        const auto entries = PaletteModel::BuildEntries(
            Plugins(
                {Plugin("pa", {"pa.one"}, {Contribution("pa.one", key)}),
                 Plugin("pb", {"pb.two"}, {Contribution("pb.two", key)}),
                 Plugin("pc", {"pc.three"}, {Contribution("pc.three", key)})}),
            Table({Row(key, "pa.one", "pa")}), EPlatform::Linux);
        const QString text =
            "Ctrl+Alt+K is bound by pa, pb and pc: pa keeps it, pb and pc "
            "are ignored";
        for (const QString& command : {"pa.one", "pb.two", "pc.three"}) {
            const PaletteModel::Entry* entry = Find(entries, command);
            QVERIFY(entry != nullptr);
            QCOMPARE(entry->Conflict, text);
        }
    }

    // A loser with two commands on the key is named once, and both of its
    // rows have the sentence.
    void ALoserOfTwoCommandsIsNamedOnce() {
        const QString key = "Ctrl+Alt+K";
        const auto entries = PaletteModel::BuildEntries(
            Plugins({Plugin("pa", {"pa.one"}, {Contribution("pa.one", key)}),
                     Plugin("pb", {"pb.two", "pb.three"},
                            {Contribution("pb.two", key),
                             Contribution("pb.three", key)})}),
            Table({Row(key, "pa.one", "pa")}), EPlatform::Linux);
        for (const QString& command : {"pa.one", "pb.two", "pb.three"}) {
            const QString text = Find(entries, command)->Conflict;
            QCOMPARE(Count(text, "pb is ignored"), 1);
            QCOMPARE(Count(text, "is bound by"), 1);
        }
    }

    // The key is the same but the panel differs: two conflicts, two
    // sentences.
    void TwoKeysTwoSentences() {
        const auto entries = PaletteModel::BuildEntries(
            Plugins({Plugin("pa", {"pa.one"},
                            {Contribution("pa.one", "Ctrl+Alt+K"),
                             Contribution("pa.one", "Ctrl+Alt+J")}),
                     Plugin("pb", {"pb.two"},
                            {Contribution("pb.two", "Ctrl+Alt+K"),
                             Contribution("pb.two", "Ctrl+Alt+J")})}),
            Table({Row("Ctrl+Alt+K", "pa.one", "pa"),
                   Row("Ctrl+Alt+J", "pa.one", "pa")}),
            EPlatform::Linux);
        const QString text = Find(entries, "pb.two")->Conflict;
        QCOMPARE(Count(text, "is bound by"), 2);
        QVERIFY(text.contains("Ctrl+Alt+K is bound by pa and pb"));
        QVERIFY(text.contains("Ctrl+Alt+J is bound by pa and pb"));
    }

    // Each key of a command is shown with its panel: the same key in the list
    // and in any panel are two items, not "Ctrl+Alt+L, Ctrl+Alt+L".
    void EveryKeyIsShownWithItsPanel() {
        const auto entries = PaletteModel::BuildEntries(
            Plugins({Plugin("pa", {"pa.one"})}),
            Table({Row("Ctrl+Alt+L", "pa.one", "pa", "sessions.list"),
                   Row("Ctrl+Alt+L", "pa.one", "pa"),
                   Row("Ctrl+Alt+J", "pa.one", "user", "terminal.view")}),
            EPlatform::Linux);
        QCOMPARE(Find(entries, "pa.one")->Key,
                 "Ctrl+Alt+L (sessions.list), Ctrl+Alt+L, "
                 "Ctrl+Alt+J (terminal.view)");
    }
};

QTEST_MAIN(PaletteEntriesTest)
#include "palette_entries_test.moc"
