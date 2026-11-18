// What the sessions list remembers and derives: the store document v1,
// colours, names, forgetting, unread and its ranking. The cases are ported from
// the predecessor: its colours, names and seen state, and its title tests.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTest>

#include <optional>
#include <vector>

#include "sessions/model/list_state.hpp"

using namespace roost::sessions;

namespace {

const QString A = QStringLiteral("claude/a");
const QString B = QStringLiteral("claude/b");
const QString C = QStringLiteral("claude/c");

SessionEntry Listed(const QString& id, const QString& state,
                    const QString& title = {}) {
    SessionEntry entry;
    entry.Agent = QStringLiteral("claude");
    entry.Id = id;
    entry.AgentTitle = title.isEmpty() ? id : title;
    entry.State = state;
    return entry;
}

QStringList List(const QJsonValue& value) {
    QStringList out;
    for (const QJsonValue& item : value.toArray()) {
        out << item.toString();
    }
    return out;
}

// Keys of the sessions shown unread.
QStringList UnreadOf(const ListState& state) {
    QStringList out;
    for (const SessionEntry& entry : state.Entries()) {
        if (entry.Unread) {
            out << SessionKey(entry.Agent, entry.Id);
        }
    }
    out.sort();
    return out;
}

QStringList RankedOf(const ListState& state) {
    QStringList out;
    for (const SessionEntry& entry : state.Entries()) {
        if (entry.Ranked) {
            out << SessionKey(entry.Agent, entry.Id);
        }
    }
    out.sort();
    return out;
}

// The document of a state that is not read-only.
QJsonObject Doc(const ListState& state) {
    const std::optional<QJsonObject> document = state.ToDocument();
    if (!document.has_value()) {
        qFatal("the state is read-only");
    }
    return *document;
}

const SessionEntry& EntryOf(const std::vector<SessionEntry>& entries,
                            const QString& id) {
    for (const SessionEntry& entry : entries) {
        if (entry.Id == id) {
            return entry;
        }
    }
    qFatal("no entry %s", qPrintable(id));
}

// A state that has had its first Update: later changes are transitions.
ListState Seeded(const std::vector<SessionEntry>& sessions) {
    ListState state;
    state.Restore({});
    state.Update(sessions);
    return state;
}

}  // namespace

class SessionsStateTest : public QObject {
    Q_OBJECT

private slots:
    void TheDocumentRoundTrips();
    void ANewerVersionIsReadOnly();
    void MalformedEntriesAreDroppedOneByOne();
    void ColoursAreHandedOutInTurnThenTheLeastUsedRepeats();
    void UncategorizedHasNoColour();
    void ARenameToTheAgentsTitleOrEmptyDropsTheName();
    void ForgetDropsEverythingOfTheSession();
    void UnreadComesWhenAStateLeavesWork();
    void TheOpenedSessionIsNeverUnreadAndOpeningReadsIt();
    void TheFirstRunStartsWithNothingUnread();
    void ASessionThatFinishedWhileNoWindowRanIsUnread();
    void ANewSessionThatAppearsFinishedIsUnread();
    void TheRankingIsRenewedOnlyByANewUnread();
    void ARankedSessionThatIsUnreadAgainRenewsTheRanking();
    void OnlyAnIntegerOneIsOurVersion();
    void AReadOnlyStateSavesNothing();
    void ForgetDropsTheListedSessionToo();
    void RestoreSaysWhetherTheStateDiffersAndKeepsTheRanking();
    void StatesOfOnlyJunkIsNotSeeded();
    void AKeyThatIsNotASessionIsRefused();
    void RepeatedPinsKeepTheLastPosition();
    void ViewEditsSayWhetherTheyChanged();
    void ColourIndexBoundary();
    void UpdateIsTrueWhenOnlyAStateChanges();
};

void SessionsStateTest::TheDocumentRoundTrips() {
    ListState state = Seeded({Listed(QStringLiteral("a"), "working"),
                              Listed(QStringLiteral("b"), "working")});
    state.SetCategory(A, QStringLiteral("work"));
    state.Rename(B, QStringLiteral("mine"));
    state.SetArchived(B, true);
    state.TogglePin(QStringLiteral("work"));
    state.TogglePin(QString());
    state.ToggleFold(QString());
    state.SetQuery(QStringLiteral("q"));
    state.SetTab(ETab::Unread);
    state.SetViewArchive(true);
    state.SetCompact(true);
    state.SetOpened(B);
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "working")});

    const QJsonObject written = Doc(state);
    QCOMPARE(written.value("version").toInt(), 1);
    QCOMPARE(written.value("categories").toObject().value(A).toString(),
             QStringLiteral("work"));
    QCOMPARE(written.value("names").toObject().value(B).toString(),
             QStringLiteral("mine"));
    QCOMPARE(List(written.value("archived")), QStringList{B});
    QCOMPARE(written.value("colors").toObject().value("work").toInt(-1), 0);
    QCOMPARE(List(written.value("pinned")),
             (QStringList{QStringLiteral("work"), QString()}));
    QCOMPARE(List(written.value("folded")), QStringList{QString()});
    QCOMPARE(List(written.value("unread")), QStringList{A});
    QCOMPARE(written.value("states").toObject().value(A).toString(),
             QStringLiteral("done"));
    const QJsonObject view = written.value("view").toObject();
    QCOMPARE(view.value("query").toString(), QStringLiteral("q"));
    QCOMPARE(view.value("tab").toString(), QStringLiteral("unread"));
    QCOMPARE(view.value("archive").toBool(), true);
    QCOMPARE(view.value("compact").toBool(), true);
    QCOMPARE(view.value("opened").toString(), B);

    ListState restored;
    restored.Restore(written);
    QCOMPARE(Doc(restored), written);
    QVERIFY(!restored.ReadOnly());
    QCOMPARE(restored.View().Opened, B);
    QCOMPARE(restored.View().Pinned,
             (QStringList{QStringLiteral("work"), QString()}));
    QVERIFY(restored.Compact());
}

void SessionsStateTest::ANewerVersionIsReadOnly() {
    QJsonObject newer;
    newer.insert("version", 2);
    newer.insert("names", QJsonObject{{B, QStringLiteral("mine")}});
    ListState state;
    state.Restore(newer);
    QVERIFY(state.ReadOnly());
    QVERIFY(!state.Problem().isEmpty());
    // Still readable for the list.
    state.Update({Listed(QStringLiteral("b"), "idle")});
    QCOMPARE(state.Entries().front().Name, QStringLiteral("mine"));

    // Our own and a missing version are read and written.
    state.Restore(QJsonObject{{"version", 1}});
    QVERIFY(!state.ReadOnly());
    QVERIFY(state.Problem().isEmpty());
    state.Restore({});
    QVERIFY(!state.ReadOnly());
    // A later document that is ours lifts the mark.
    state.Restore(newer);
    QVERIFY(state.ReadOnly());
    state.Restore(QJsonObject{{"version", 1}});
    QVERIFY(!state.ReadOnly());
}

void SessionsStateTest::MalformedEntriesAreDroppedOneByOne() {
    const QJsonObject document{
        {"version", 1},
        {"categories",
         QJsonObject{{A, "work"}, {"nokey", "x"}, {B, 5}, {C, ""}}},
        {"names", QJsonObject{{A, "n"}, {B, QJsonArray{}}, {"/x", "y"}}},
        {"archived", QJsonArray{A, 3, "nokey", QJsonValue::Null, B}},
        {"colors",
         QJsonObject{
             {"work", 3}, {"big", 99}, {"neg", -1}, {"s", "x"}, {"", 2}}},
        {"pinned", QJsonArray{"work", 5, QJsonValue::Null, ""}},
        {"folded", QJsonArray{"", false}},
        {"unread", QJsonArray{B, 7, "x/"}},
        {"states", QJsonObject{{A, "done"}, {B, 1}}},
        {"view", QJsonObject{{"query", 4},
                             {"tab", "weird"},
                             {"archive", "yes"},
                             {"compact", true},
                             {"opened", "nokey"}}}};
    ListState state;
    state.Restore(document);
    const QJsonObject out = Doc(state);

    QCOMPARE(out.value("categories").toObject(), (QJsonObject{{A, "work"}}));
    QCOMPARE(out.value("names").toObject(), (QJsonObject{{A, "n"}}));
    QCOMPARE(List(out.value("archived")), (QStringList{A, B}));
    QCOMPARE(out.value("colors").toObject(), (QJsonObject{{"work", 3}}));
    QCOMPARE(List(out.value("pinned")),
             (QStringList{QStringLiteral("work"), QString()}));
    QCOMPARE(List(out.value("folded")), QStringList{QString()});
    QCOMPARE(List(out.value("unread")), QStringList{B});
    QCOMPARE(out.value("states").toObject(), (QJsonObject{{A, "done"}}));
    const QJsonObject view = out.value("view").toObject();
    QCOMPARE(view.value("query").toString(), QString());
    QCOMPARE(view.value("tab").toString(), QStringLiteral("all"));
    QCOMPARE(view.value("archive").toBool(), false);
    QCOMPARE(view.value("compact").toBool(), true);
    QCOMPARE(view.value("opened").toString(), QString());

    // A section of the wrong type is an empty section.
    ListState other;
    other.Restore(QJsonObject{{"categories", QJsonArray{A}},
                              {"pinned", QStringLiteral("work")},
                              {"names", QJsonObject{{A, "kept"}}}});
    QVERIFY(!other.ReadOnly());
    QVERIFY(Doc(other).value("categories").toObject().isEmpty());
    QCOMPARE(Doc(other).value("names").toObject(), (QJsonObject{{A, "kept"}}));
}

void SessionsStateTest::ColoursAreHandedOutInTurnThenTheLeastUsedRepeats() {
    ListState state;
    for (int i = 0; i < COLOR_COUNT; ++i) {
        state.SetCategory(QStringLiteral("claude/s%1").arg(i),
                          QStringLiteral("cat%1").arg(i));
        QCOMPARE(state.ColorIndex(QStringLiteral("cat%1").arg(i)), i);
    }
    // All eight used once: the first of the least used repeats, then the next.
    state.SetCategory(QStringLiteral("claude/x"), QStringLiteral("nine"));
    QCOMPARE(state.ColorIndex(QStringLiteral("nine")), 0);
    state.SetCategory(QStringLiteral("claude/y"), QStringLiteral("ten"));
    QCOMPARE(state.ColorIndex(QStringLiteral("ten")), 1);
    // A second session of a category keeps its colour.
    state.SetCategory(QStringLiteral("claude/z"), QStringLiteral("cat3"));
    QCOMPARE(state.ColorIndex(QStringLiteral("cat3")), 3);

    // Not by position: colour 1 is used twice, 0 once, nine categories.
    ListState uneven;
    QJsonObject colors;
    for (int i = 0; i < COLOR_COUNT; ++i) {
        colors.insert(QStringLiteral("cat%1").arg(i), i);
    }
    colors.insert(QStringLiteral("cat1b"), 1);
    uneven.Restore(QJsonObject{{"colors", colors}});
    uneven.SetCategory(A, QStringLiteral("new"));
    QCOMPARE(uneven.ColorIndex(QStringLiteral("new")), 0);

    // A free colour is taken before any repeats.
    ListState gap;
    gap.Restore(QJsonObject{{"colors", QJsonObject{{"p", 0}, {"q", 2}}}});
    gap.SetCategory(A, QStringLiteral("r"));
    QCOMPARE(gap.ColorIndex(QStringLiteral("r")), 1);

    // Handed out and saved with the document.
    QCOMPARE(Doc(state).value("colors").toObject().value("ten").toInt(), 1);
}

void SessionsStateTest::UncategorizedHasNoColour() {
    ListState state;
    state.SetCategory(A, QStringLiteral("work"));
    state.SetCategory(A, QString());
    QCOMPARE(state.ColorIndex(QString()), -1);
    QVERIFY(!Doc(state).value("colors").toObject().contains(""));
    // The category stays coloured: its other sessions keep the look.
    QCOMPARE(state.ColorIndex(QStringLiteral("work")), 0);
    QCOMPARE(state.ColorIndex(QStringLiteral("unseen")), -1);

    ListState restored;
    restored.Restore(QJsonObject{{"colors", QJsonObject{{"", 3}}}});
    QCOMPARE(restored.ColorIndex(QString()), -1);
}

void SessionsStateTest::ARenameToTheAgentsTitleOrEmptyDropsTheName() {
    ListState state = Seeded({Listed(QStringLiteral("a"), "idle", "Agent A")});
    QVERIFY(state.Rename(A, QStringLiteral("  mine ")));
    QCOMPARE(state.Entries().front().Name, QStringLiteral("mine"));
    QVERIFY(!state.Rename(A, QStringLiteral("mine")));

    QVERIFY(state.Rename(A, QStringLiteral("Agent A")));
    QCOMPARE(state.Entries().front().Name, QString());
    QVERIFY(!Doc(state).value("names").toObject().contains(A));

    state.Rename(A, QStringLiteral("mine"));
    QVERIFY(state.Rename(A, QString()));
    QVERIFY(!Doc(state).value("names").toObject().contains(A));
    state.Rename(A, QStringLiteral("mine"));
    QVERIFY(state.Rename(A, QStringLiteral("   ")));
    QVERIFY(!Doc(state).value("names").toObject().contains(A));
}

void SessionsStateTest::ForgetDropsEverythingOfTheSession() {
    ListState state = Seeded({Listed(QStringLiteral("a"), "working"),
                              Listed(QStringLiteral("b"), "working")});
    for (const QString& key : {A, B}) {
        state.SetCategory(key, QStringLiteral("work"));
        state.Rename(key, QStringLiteral("name"));
        state.SetArchived(key, true);
    }
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "done")});
    state.SetOpened(A);
    QVERIFY(state.Forget(A));

    const QJsonObject document = Doc(state);
    const QString text = QString::fromUtf8(
        QJsonDocument(document).toJson(QJsonDocument::Compact));
    QVERIFY2(!text.contains(A), qPrintable(text));
    // The other session and the category stay.
    QVERIFY(text.contains(B));
    QCOMPARE(document.value("categories").toObject().value(B).toString(),
             QStringLiteral("work"));
    QCOMPARE(state.ColorIndex(QStringLiteral("work")), 0);
    QCOMPARE(state.View().Opened, QString());
    QVERIFY(!state.Forget(A));
}

void SessionsStateTest::UnreadComesWhenAStateLeavesWork() {
    ListState state = Seeded({Listed(QStringLiteral("a"), "working"),
                              Listed(QStringLiteral("b"), "idle")});
    QCOMPARE(UnreadOf(state), QStringList{});

    // A finished turn is unread; a session that starts working is not.
    QVERIFY(state.Update({Listed(QStringLiteral("a"), "done"),
                          Listed(QStringLiteral("b"), "working")}));
    QCOMPARE(UnreadOf(state), QStringList{A});
    QCOMPARE(List(Doc(state).value("unread")), QStringList{A});

    // The same state again changes nothing.
    QVERIFY(!state.Update({Listed(QStringLiteral("a"), "done"),
                           Listed(QStringLiteral("b"), "working")}));

    // The tab counts follow: one unread, one active.
    const TabCounts counts = BuildRows(state.Entries(), state.View()).Counts;
    QCOMPARE(counts.Unread, 1);
    QCOMPARE(counts.Active, 1);

    // Unread while it works again is not shown, and shows again after.
    state.Update({Listed(QStringLiteral("a"), "working"),
                  Listed(QStringLiteral("b"), "working")});
    QCOMPARE(UnreadOf(state), QStringList{});
    state.Update({Listed(QStringLiteral("a"), "blocked"),
                  Listed(QStringLiteral("b"), "working")});
    QCOMPARE(UnreadOf(state), QStringList{A});
}

void SessionsStateTest::TheOpenedSessionIsNeverUnreadAndOpeningReadsIt() {
    ListState state = Seeded({Listed(QStringLiteral("a"), "working"),
                              Listed(QStringLiteral("b"), "working")});
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "done")});
    QCOMPARE(UnreadOf(state), (QStringList{A, B}));

    // Opening reads it, in the display and in the document.
    QVERIFY(state.SetOpened(A));
    QCOMPARE(UnreadOf(state), QStringList{B});
    QCOMPARE(List(Doc(state).value("unread")), QStringList{B});

    // What the opened session does is never unread.
    state.Update({Listed(QStringLiteral("a"), "working"),
                  Listed(QStringLiteral("b"), "done")});
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "done")});
    QCOMPARE(UnreadOf(state), QStringList{B});
    QCOMPARE(List(Doc(state).value("unread")), QStringList{B});

    // Once another is opened it is an ordinary session again.
    state.SetOpened(B);
    state.Update({Listed(QStringLiteral("a"), "working"),
                  Listed(QStringLiteral("b"), "done")});
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "done")});
    QCOMPARE(UnreadOf(state), QStringList{A});

    // A document restored with an unread opened session shows it read.
    ListState restored;
    restored.Restore(QJsonObject{{"states", QJsonObject{{A, "done"}}},
                                 {"unread", QJsonArray{A}},
                                 {"view", QJsonObject{{"opened", A}}}});
    restored.Update({Listed(QStringLiteral("a"), "done")});
    QCOMPARE(UnreadOf(restored), QStringList{});
}

void SessionsStateTest::TheFirstRunStartsWithNothingUnread() {
    ListState state;
    state.Restore({});
    QVERIFY(state.Update({Listed(QStringLiteral("a"), "done"),
                          Listed(QStringLiteral("b"), "idle"),
                          Listed(QStringLiteral("c"), "working")}));
    QCOMPARE(UnreadOf(state), QStringList{});
    QVERIFY(Doc(state).value("unread").toArray().isEmpty());
    QCOMPARE(Doc(state).value("states").toObject().size(), 3);
    QCOMPARE(RankedOf(state), QStringList{});

    // From then on the transitions count.
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "idle"),
                  Listed(QStringLiteral("c"), "done")});
    QCOMPARE(UnreadOf(state), QStringList{C});

    // A first run with nothing listed yet is a first run all the same.
    ListState empty;
    empty.Restore({});
    empty.Update({});
    empty.Update({Listed(QStringLiteral("a"), "working")});
    empty.Update({Listed(QStringLiteral("a"), "done")});
    QCOMPARE(UnreadOf(empty), QStringList{A});
}

void SessionsStateTest::ASessionThatFinishedWhileNoWindowRanIsUnread() {
    ListState state;
    state.Restore(QJsonObject{
        {"states", QJsonObject{{A, "working"}, {B, "working"}, {C, "idle"}}}});
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "working"),
                  Listed(QStringLiteral("c"), "idle")});
    QCOMPARE(UnreadOf(state), QStringList{A});

    // The order of the two calls does not matter.
    ListState late;
    late.Update({Listed(QStringLiteral("a"), "done")});
    late.Restore(QJsonObject{{"states", QJsonObject{{A, "working"}}}});
    QCOMPARE(UnreadOf(late), QStringList{A});
}

void SessionsStateTest::ANewSessionThatAppearsFinishedIsUnread() {
    ListState state;
    state.Restore(QJsonObject{{"states", QJsonObject{{A, "done"}}}});
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "done"),
                  Listed(QStringLiteral("c"), "working")});
    QCOMPARE(UnreadOf(state), QStringList{B});
    QCOMPARE(Doc(state).value("states").toObject().size(), 3);

    // A session skipped by one listing and back is the same session.
    state.SetOpened(B);
    state.Update({Listed(QStringLiteral("a"), "done")});
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("c"), "working")});
    QCOMPARE(UnreadOf(state), QStringList{});
}

void SessionsStateTest::TheRankingIsRenewedOnlyByANewUnread() {
    ListState state = Seeded({Listed(QStringLiteral("a"), "working"),
                              Listed(QStringLiteral("b"), "working"),
                              Listed(QStringLiteral("c"), "working")});
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "done"),
                  Listed(QStringLiteral("c"), "working")});
    QCOMPARE(RankedOf(state), (QStringList{A, B}));

    // Reading one leaves the order alone: it is read, but still ranked.
    state.SetOpened(A);
    QCOMPARE(UnreadOf(state), QStringList{B});
    QCOMPARE(RankedOf(state), (QStringList{A, B}));
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "done"),
                  Listed(QStringLiteral("c"), "working")});
    QCOMPARE(RankedOf(state), (QStringList{A, B}));

    // A new unread renews it to what is unread now.
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "done"),
                  Listed(QStringLiteral("c"), "done")});
    QCOMPARE(UnreadOf(state), (QStringList{B, C}));
    QCOMPARE(RankedOf(state), (QStringList{B, C}));

    const std::vector<SessionEntry> entries = state.Entries();
    QVERIFY(EntryOf(entries, "b").Ranked);
    QVERIFY(!EntryOf(entries, "a").Ranked);
}

void SessionsStateTest::ARankedSessionThatIsUnreadAgainRenewsTheRanking() {
    ListState state = Seeded({Listed(QStringLiteral("a"), "working"),
                              Listed(QStringLiteral("b"), "working")});
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "done")});
    QCOMPARE(RankedOf(state), (QStringList{A, B}));
    state.SetOpened(A);
    state.SetOpened(B);
    QCOMPARE(UnreadOf(state), QStringList{});

    state.Update({Listed(QStringLiteral("a"), "working"),
                  Listed(QStringLiteral("b"), "done")});
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "done")});
    QCOMPARE(UnreadOf(state), QStringList{A});
    QCOMPARE(RankedOf(state), QStringList{A});
}

void SessionsStateTest::OnlyAnIntegerOneIsOurVersion() {
    ListState state;
    state.Restore({});
    QVERIFY(!state.ReadOnly());
    state.Restore(QJsonObject{{"version", 1}});
    QVERIFY(!state.ReadOnly());
    state.Restore(QJsonObject{{"version", 1.0}});
    QVERIFY(!state.ReadOnly());

    const QList<QJsonValue> unknown{2,
                                    2.5,
                                    1e30,
                                    1.5,
                                    0,
                                    -5,
                                    QStringLiteral("2"),
                                    QStringLiteral("1"),
                                    QJsonValue::Null,
                                    true,
                                    QJsonArray{1}};
    for (const QJsonValue& version : unknown) {
        state.Restore(QJsonObject{{"version", version}});
        QVERIFY2(state.ReadOnly(),
                 qPrintable(QJsonDocument(QJsonArray{version}).toJson()));
        QVERIFY(!state.Problem().isEmpty());
        // The mark is lifted by the next document that is ours.
        state.Restore(QJsonObject{{"version", 1}});
        QVERIFY(!state.ReadOnly());
    }
}

void SessionsStateTest::AReadOnlyStateSavesNothing() {
    ListState state;
    QVERIFY(
        !state.Restore(QJsonObject{{"version", 2}, {"states", QJsonObject{}}}));
    QVERIFY(state.ReadOnly());
    QVERIFY(!state.ToDocument().has_value());

    // Everything works in memory and nothing asks for a save.
    QVERIFY(!state.Update({Listed(QStringLiteral("a"), "working")}));
    QVERIFY(!state.Update({Listed(QStringLiteral("a"), "done")}));
    QCOMPARE(UnreadOf(state), QStringList{A});
    QVERIFY(!state.SetCategory(A, QStringLiteral("work")));
    QCOMPARE(state.Entries().front().Category, QStringLiteral("work"));
    QVERIFY(!state.Rename(A, QStringLiteral("mine")));
    QVERIFY(!state.SetArchived(A, true));
    QVERIFY(!state.TogglePin(QStringLiteral("work")));
    QVERIFY(!state.ToggleFold(QStringLiteral("work")));
    QVERIFY(!state.UnfoldAll());
    QVERIFY(!state.SetQuery(QStringLiteral("q")));
    QVERIFY(!state.SetTab(ETab::Unread));
    QVERIFY(!state.SetViewArchive(true));
    QVERIFY(!state.SetCompact(true));
    QVERIFY(!state.SetOpened(A));
    QVERIFY(!state.Forget(A));
    QCOMPARE(state.View().Query, QStringLiteral("q"));
}

void SessionsStateTest::ForgetDropsTheListedSessionToo() {
    ListState state = Seeded({Listed(QStringLiteral("a"), "working"),
                              Listed(QStringLiteral("b"), "working")});
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "working")});
    QVERIFY(state.Forget(A));
    QCOMPARE(state.Entries().size(), size_t(1));
    QCOMPARE(UnreadOf(state), QStringList{});

    // The same stale list again: the session is new, first seen, unread.
    QVERIFY(state.Update({Listed(QStringLiteral("a"), "done"),
                          Listed(QStringLiteral("b"), "working")}));
    QCOMPARE(state.Entries().size(), size_t(2));
    QCOMPARE(UnreadOf(state), QStringList{A});
    QVERIFY(!state.Update({Listed(QStringLiteral("a"), "done"),
                           Listed(QStringLiteral("b"), "working")}));
}

void SessionsStateTest::RestoreSaysWhetherTheStateDiffersAndKeepsTheRanking() {
    ListState state = Seeded({Listed(QStringLiteral("a"), "working"),
                              Listed(QStringLiteral("b"), "working")});
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "done")});
    const QJsonObject saved = Doc(state);
    QVERIFY(!state.Restore(saved));

    // The document says a is working; the list says done: reconciled, differs.
    QJsonObject stale = saved;
    stale.insert("unread", QJsonArray{});
    stale.insert("states", QJsonObject{{A, "working"}, {B, "done"}});
    QVERIFY(state.Restore(stale));
    QCOMPARE(UnreadOf(state), QStringList{A});

    // The ranking survives a Restore that brings no new unread.
    ListState ranked = Seeded({Listed(QStringLiteral("a"), "working"),
                               Listed(QStringLiteral("b"), "working")});
    ranked.Update({Listed(QStringLiteral("a"), "done"),
                   Listed(QStringLiteral("b"), "done")});
    QCOMPARE(RankedOf(ranked), (QStringList{A, B}));
    ranked.SetOpened(A);
    ranked.Restore(Doc(ranked));
    QCOMPARE(RankedOf(ranked), (QStringList{A, B}));

    // Malformed entries dropped: the state differs from what was given.
    ListState junk;
    QVERIFY(junk.Restore(QJsonObject{
        {"version", 1}, {"categories", QJsonObject{{"nokey", "x"}}}}));
}

void SessionsStateTest::StatesOfOnlyJunkIsNotSeeded() {
    ListState state;
    state.Restore(
        QJsonObject{{"states", QJsonObject{{"nokey", "done"}, {A, 5}}}});
    state.Update({Listed(QStringLiteral("a"), "done"),
                  Listed(QStringLiteral("b"), "idle")});
    QCOMPARE(UnreadOf(state), QStringList{});

    // An empty object is seeded: everything listed is new.
    ListState empty;
    empty.Restore(QJsonObject{{"states", QJsonObject{}}});
    empty.Update({Listed(QStringLiteral("a"), "done")});
    QCOMPARE(UnreadOf(empty), QStringList{A});
}

void SessionsStateTest::AKeyThatIsNotASessionIsRefused() {
    ListState state = Seeded({Listed(QStringLiteral("a"), "working")});
    const QJsonObject before = Doc(state);
    for (const QString& key : {QStringLiteral("nokey"), QStringLiteral("/x"),
                               QStringLiteral("x/")}) {
        QVERIFY(!state.SetCategory(key, QStringLiteral("work")));
        QVERIFY(!state.Rename(key, QStringLiteral("name")));
        QVERIFY(!state.SetArchived(key, true));
        QVERIFY(!state.SetOpened(key));
        QVERIFY(!state.Forget(key));
    }
    QCOMPARE(Doc(state), before);
    QCOMPARE(state.ColorIndex(QStringLiteral("work")), -1);
    // An empty key still means nothing is opened.
    QVERIFY(state.SetOpened(A));
    QVERIFY(state.SetOpened(QString()));
}

void SessionsStateTest::RepeatedPinsKeepTheLastPosition() {
    ListState state;
    state.Restore(
        QJsonObject{{"pinned", QJsonArray{"x", "y", "x", "", "y", ""}}});
    QCOMPARE(
        state.View().Pinned,
        (QStringList{QStringLiteral("x"), QStringLiteral("y"), QString()}));
    QVERIFY(state.TogglePin(QStringLiteral("x")));
    QVERIFY(!state.View().Pinned.contains(QStringLiteral("x")));
}

void SessionsStateTest::ViewEditsSayWhetherTheyChanged() {
    ListState state = Seeded({Listed(QStringLiteral("a"), "working")});
    QVERIFY(!state.UnfoldAll());
    QVERIFY(state.ToggleFold(QStringLiteral("work")));
    QVERIFY(state.UnfoldAll());
    QVERIFY(!state.UnfoldAll());
    QVERIFY(state.ToggleFold(QStringLiteral("work")));
    QVERIFY(state.ToggleFold(QStringLiteral("work")));

    QVERIFY(state.SetOpened(A));
    QVERIFY(!state.SetOpened(A));
    QVERIFY(state.SetViewArchive(true));
    QVERIFY(!state.SetViewArchive(true));
    QVERIFY(state.SetViewArchive(false));
}

void SessionsStateTest::ColourIndexBoundary() {
    ListState state;
    state.Restore(QJsonObject{{"colors", QJsonObject{{"top", COLOR_COUNT - 1},
                                                     {"over", COLOR_COUNT}}}});
    QCOMPARE(state.ColorIndex(QStringLiteral("top")), COLOR_COUNT - 1);
    QCOMPARE(state.ColorIndex(QStringLiteral("over")), -1);
}

void SessionsStateTest::UpdateIsTrueWhenOnlyAStateChanges() {
    ListState state = Seeded({Listed(QStringLiteral("a"), "idle")});
    QVERIFY(state.Update({Listed(QStringLiteral("a"), "working")}));
    QCOMPARE(UnreadOf(state), QStringList{});
    QVERIFY(!state.Update({Listed(QStringLiteral("a"), "working")}));
}

QTEST_GUILESS_MAIN(SessionsStateTest)
#include "sessions_state_test.moc"
