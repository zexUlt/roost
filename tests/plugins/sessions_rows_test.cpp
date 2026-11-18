// The rows of the sessions list: grouping, pins, folding, order, tabs, search.
// The cases are those of cc-fleet's listview.py, test_pins.py and test_tree.py.

#include <QStringList>
#include <QTest>

#include <vector>

#include "sessions/model/session_rows.hpp"

using namespace roost::sessions;

namespace {

SessionEntry Make(const QString& id, const QString& category = {},
                  const QString& state = QStringLiteral("idle")) {
    SessionEntry entry;
    entry.Agent = QStringLiteral("claude");
    entry.Id = id;
    entry.AgentTitle = id;
    entry.State = state;
    entry.Category = category;
    return entry;
}

// Unread and ranked, as a session that has just become unread.
SessionEntry Unread(SessionEntry entry) {
    entry.Unread = true;
    entry.Ranked = true;
    return entry;
}

QStringList Headers(const RowsResult& result) {
    QStringList out;
    for (const Row& row : result.Rows) {
        if (row.Kind == ERowKind::Header) {
            out << row.Category;
        }
    }
    return out;
}

QStringList Ids(const RowsResult& result) {
    QStringList out;
    for (const Row& row : result.Rows) {
        if (row.Kind == ERowKind::Session) {
            out << row.Id;
        }
    }
    return out;
}

}  // namespace

class SessionsRowsTest : public QObject {
    Q_OBJECT

private slots:
    void PinnedGoFirstInPinOrderAboveUnread();
    void APinnedCategoryWithoutSessionsHasNoHeader();
    void AFoldedCategoryIsAHeaderThatCountsItsSessions();
    void UncategorizedIsLastAmongEquals();
    void UnreadComeFirstInACategoryTheRestKeepRoostdsOrder();
    void TheArchiveViewShowsOnlyTheArchived();
    void TheActiveTabHidesTheDoneButNotTheOpened();
    void TheUnreadTabKeepsTheUnreadAndTheOpened();
    void SearchNeedsEveryWordAnywhereIgnoringCase();
    void SearchFindsTheGivenNameTheStateAndUncategorized();
    void TheOrderFollowsRankedNotUnread();
    void ARepeatedPinTakesItsLastPlace();
    void SearchSplitsOnUnicodeWhitespace();
    void CountsIgnoreTheTabAndTheQuery();
    void AnOpenedArchivedSessionIsHiddenOutsideTheArchive();
};

void SessionsRowsTest::PinnedGoFirstInPinOrderAboveUnread() {
    const std::vector<SessionEntry> sessions = {Unread(Make("a", "alpha")),
                                                Make("b", "beta"),
                                                Make("c", "gamma"), Make("d")};
    ViewState view;
    view.Pinned = {QStringLiteral("gamma"), QString()};
    QCOMPARE(Headers(BuildRows(sessions, view)),
             (QStringList{QStringLiteral("gamma"), QString(),
                          QStringLiteral("alpha"), QStringLiteral("beta")}));
    QVERIFY(BuildRows(sessions, view).Rows.front().Pinned);
}

void SessionsRowsTest::APinnedCategoryWithoutSessionsHasNoHeader() {
    ViewState view;
    view.Pinned = {QStringLiteral("gone")};
    QCOMPARE(Headers(BuildRows({Make("a", "alpha")}, view)),
             QStringList{QStringLiteral("alpha")});
}

void SessionsRowsTest::AFoldedCategoryIsAHeaderThatCountsItsSessions() {
    const std::vector<SessionEntry> sessions = {
        Make("a", "alpha"), Unread(Make("b", "alpha")), Make("c", "beta")};
    ViewState view;
    view.Folded = {QStringLiteral("alpha")};
    const RowsResult result = BuildRows(sessions, view);
    QCOMPARE(result.Rows.size(), size_t{3});  // header alpha, header beta, c
    QCOMPARE(result.Rows[0].Category, QStringLiteral("alpha"));
    QVERIFY(result.Rows[0].Folded);
    QCOMPARE(result.Rows[0].Count, 2);
    QCOMPARE(result.Rows[0].UnreadCount, 1);
    QCOMPARE(Ids(result), QStringList{QStringLiteral("c")});
}

void SessionsRowsTest::UncategorizedIsLastAmongEquals() {
    // "" would sort first by name: only its own key puts it last.
    const std::vector<SessionEntry> sessions = {Make("a"), Make("b", "beta"),
                                                Make("c", "zeta")};
    QCOMPARE(Headers(BuildRows(sessions, {})),
             (QStringList{QStringLiteral("beta"), QStringLiteral("zeta"),
                          QString()}));
    // More unread beats being last.
    const std::vector<SessionEntry> fresh = {Unread(Make("a")),
                                             Make("b", "beta")};
    QCOMPARE(Headers(BuildRows(fresh, {})),
             (QStringList{QString(), QStringLiteral("beta")}));
}

void SessionsRowsTest::UnreadComeFirstInACategoryTheRestKeepRoostdsOrder() {
    // Neither the unread nor the rest are in alphabetical order.
    const std::vector<SessionEntry> sessions = {
        Make("e", "x"), Make("c", "x"), Unread(Make("d", "x")), Make("b", "x"),
        Unread(Make("a", "x"))};
    QCOMPARE(Ids(BuildRows(sessions, {})),
             (QStringList{QStringLiteral("d"), QStringLiteral("a"),
                          QStringLiteral("e"), QStringLiteral("c"),
                          QStringLiteral("b")}));
}

void SessionsRowsTest::TheArchiveViewShowsOnlyTheArchived() {
    SessionEntry archived = Make("old");
    archived.Archived = true;
    const std::vector<SessionEntry> sessions = {Make("new"), archived};
    ViewState view;
    QCOMPARE(Ids(BuildRows(sessions, view)),
             QStringList{QStringLiteral("new")});
    view.ViewArchive = true;
    const RowsResult result = BuildRows(sessions, view);
    QCOMPARE(Ids(result), QStringList{QStringLiteral("old")});
    QVERIFY(result.Rows.back().Archived);
    QCOMPARE(result.Counts.All, 1);
}

void SessionsRowsTest::TheActiveTabHidesTheDoneButNotTheOpened() {
    const std::vector<SessionEntry> sessions = {Make("run", {}, "working"),
                                                Make("fin", {}, "done"),
                                                Make("shown", {}, "done")};
    ViewState view;
    view.Tab = ETab::Active;
    QCOMPARE(Ids(BuildRows(sessions, view)),
             QStringList{QStringLiteral("run")});
    view.Opened = QStringLiteral("claude/shown");
    const RowsResult result = BuildRows(sessions, view);
    QCOMPARE(Ids(result),
             (QStringList{QStringLiteral("run"), QStringLiteral("shown")}));
    QVERIFY(result.Rows.back().IsOpen);
    // The counts are those of the archive view, not of the tab.
    QCOMPARE(result.Counts, (TabCounts{3, 1, 0}));
}

void SessionsRowsTest::TheUnreadTabKeepsTheUnreadAndTheOpened() {
    const std::vector<SessionEntry> sessions = {Unread(Make("new", {}, "done")),
                                                Make("read", {}, "done"),
                                                Make("shown", {}, "done")};
    ViewState view;
    view.Tab = ETab::Unread;
    QCOMPARE(Ids(BuildRows(sessions, view)),
             QStringList{QStringLiteral("new")});
    view.Opened = QStringLiteral("claude/shown");
    QCOMPARE(Ids(BuildRows(sessions, view)),
             (QStringList{QStringLiteral("new"), QStringLiteral("shown")}));
}

void SessionsRowsTest::SearchNeedsEveryWordAnywhereIgnoringCase() {
    SessionEntry one = Make("one", "Work", "working");
    one.Name = QStringLiteral("Fix the Parser");
    SessionEntry two = Make("two", "Work", "idle");
    two.Name = QStringLiteral("Fix the lexer");
    const std::vector<SessionEntry> sessions = {one, two};
    ViewState view;
    view.Query = QStringLiteral("  PARSER fix ");
    QCOMPARE(Ids(BuildRows(sessions, view)),
             QStringList{QStringLiteral("one")});
    view.Query =
        QStringLiteral("fix work");  // a word of the name, one of the category
    QCOMPARE(Ids(BuildRows(sessions, view)),
             (QStringList{QStringLiteral("one"), QStringLiteral("two")}));
    view.Query = QStringLiteral(
        "parser lexer");  // each found in a different session only
    QVERIFY(Ids(BuildRows(sessions, view)).isEmpty());
    // Counts are taken before the query.
    QCOMPARE(BuildRows(sessions, view).Counts.All, 2);
}

void SessionsRowsTest::SearchFindsTheGivenNameTheStateAndUncategorized() {
    SessionEntry named = Make("bg-1", {}, "blocked");
    named.AgentTitle = QStringLiteral("adapter title");
    named.Name = QStringLiteral("my name");
    const std::vector<SessionEntry> sessions = {named, Make("other", "work")};
    ViewState view;
    view.Query = QStringLiteral("my");
    QCOMPARE(Ids(BuildRows(sessions, view)),
             QStringList{QStringLiteral("bg-1")});
    view.Query = QStringLiteral(
        "adapter");  // the adapter's title is hidden by the given name
    QVERIFY(Ids(BuildRows(sessions, view)).isEmpty());
    view.Query = QStringLiteral("blocked");
    QCOMPARE(Ids(BuildRows(sessions, view)),
             QStringList{QStringLiteral("bg-1")});
    view.Query = QStringLiteral("uncategorized");
    QCOMPARE(Ids(BuildRows(sessions, view)),
             QStringList{QStringLiteral("bg-1")});
    // Without a given name the adapter's title is searched.
    named.Name.clear();
    view.Query = QStringLiteral("adapter");
    QCOMPARE(Ids(BuildRows({named}, view)),
             QStringList{QStringLiteral("bg-1")});
    QCOMPARE(BuildRows({named}, view).Rows.back().Title,
             QStringLiteral("adapter title"));
}

void SessionsRowsTest::TheOrderFollowsRankedNotUnread() {
    SessionEntry justRead = Make("read", "x");
    justRead.Ranked = true;  // was unread when the order was taken
    SessionEntry fresh = Make("fresh", "x");
    fresh.Unread = true;  // not ranked yet
    const std::vector<SessionEntry> sessions = {Make("a", "x"), fresh,
                                                justRead};
    QCOMPARE(Ids(BuildRows(sessions, {})),
             (QStringList{QStringLiteral("read"), QStringLiteral("a"),
                          QStringLiteral("fresh")}));
    // The Unread tab and the counts take the live flag.
    ViewState view;
    view.Tab = ETab::Unread;
    const RowsResult result = BuildRows(sessions, view);
    QCOMPARE(Ids(result), QStringList{QStringLiteral("fresh")});
    QCOMPARE(result.Counts.Unread, 1);
    // Ranked moves a group up, too.
    SessionEntry other = Make("o", "y");
    other.Ranked = true;
    QCOMPARE(Headers(BuildRows({Make("a", "x"), other}, {})),
             (QStringList{QStringLiteral("y"), QStringLiteral("x")}));
}

void SessionsRowsTest::ARepeatedPinTakesItsLastPlace() {
    // As cc-fleet's {cat: i for i, cat in enumerate(pins)}.
    const std::vector<SessionEntry> sessions = {Make("a", "x"), Make("b", "y")};
    ViewState view;
    view.Pinned = {QStringLiteral("x"), QStringLiteral("y"),
                   QStringLiteral("x")};
    QCOMPARE(Headers(BuildRows(sessions, view)),
             (QStringList{QStringLiteral("y"), QStringLiteral("x")}));
}

void SessionsRowsTest::SearchSplitsOnUnicodeWhitespace() {
    SessionEntry one = Make("one");
    one.Name = QStringLiteral("fix parser");
    const std::vector<SessionEntry> sessions = {one, Make("two")};
    ViewState view;
    for (const QChar space : {QChar(0x00A0), QChar(0x3000)}) {
        view.Query = QStringLiteral("fix") + space + QStringLiteral("parser");
        QCOMPARE(Ids(BuildRows(sessions, view)),
                 QStringList{QStringLiteral("one")});
    }
}

void SessionsRowsTest::CountsIgnoreTheTabAndTheQuery() {
    const std::vector<SessionEntry> sessions = {
        Unread(Make("run", {}, "working")), Unread(Make("fin", {}, "done")),
        Make("idle"), Make("old", {}, "done")};
    const TabCounts all{4, 2, 2};
    QCOMPARE(BuildRows(sessions, {}).Counts, all);
    ViewState view;
    view.Tab = ETab::Unread;
    QCOMPARE(BuildRows(sessions, view).Counts, all);
    view.Query = QStringLiteral("run");
    QCOMPARE(Ids(BuildRows(sessions, view)),
             QStringList{QStringLiteral("run")});
    QCOMPARE(BuildRows(sessions, view).Counts, all);
    view.Tab = ETab::Active;
    QCOMPARE(BuildRows(sessions, view).Counts, all);
}

void SessionsRowsTest::AnOpenedArchivedSessionIsHiddenOutsideTheArchive() {
    SessionEntry archived = Make("old");
    archived.Archived = true;
    ViewState view;
    view.Opened = QStringLiteral("claude/old");
    QVERIFY(BuildRows({archived, Make("new")}, view).Rows.size() == 2);
    QCOMPARE(Ids(BuildRows({archived, Make("new")}, view)),
             QStringList{QStringLiteral("new")});
    view.Tab = ETab::Active;
    QCOMPARE(Ids(BuildRows({archived, Make("new")}, view)),
             QStringList{QStringLiteral("new")});
}

QTEST_GUILESS_MAIN(SessionsRowsTest)
#include "sessions_rows_test.moc"
