// SessionsList, the QML-facing model of the sessions list (module
// RoostSessions): the cursor, folds and pins, the store document and the
// changes of the rows, over a source model that behaves as Roost.sessions. The
// cases are the ported fold-cursor, pin, tree and title tests, and those
// of design 06-panels. A QAbstractItemModelTester watches the
// model through every case.

#include <QAbstractItemModelTester>
#include <QAbstractListModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <memory>
#include <vector>

#include "api/api.hpp"
#include "api/session_service.hpp"
#include "qml_api/roost_singleton.hpp"
#include "qml_api/sessions_model.hpp"
#include "sessions/model/row_diff.hpp"
#include "sessions/qml_types/sessions_list.hpp"

using namespace roost;
using namespace roost::sessions;

// The static plugin of module Roost.Api, linked into this executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)

namespace {

const QString AGENT = QStringLiteral("claude");

struct Item {
    QString Id;
    QString State = QStringLiteral("working");
    QString Title = QString();
};

// A source with the roles and the changes of Roost.sessions: an insert at the
// end, removes, a changed row, a reset.
class FakeSource : public QAbstractListModel {
public:
    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : static_cast<int>(Items_.size());
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) {
            return {};
        }
        const Item& item = Items_[static_cast<std::size_t>(index.row())];
        switch (role - Qt::UserRole) {
            case 0:
                return AGENT;
            case 1:
                return item.Id;
            case 2:
                return item.Title.isEmpty() ? item.Id : item.Title;
            case 3:
                return QStringLiteral("/tmp");
            case 4:
                return item.State;
            default:
                return {};
        }
    }

    QHash<int, QByteArray> roleNames() const override {
        QHash<int, QByteArray> names;
        const QStringList wanted = SessionsList::sourceRoleNames();
        for (int i = 0; i < wanted.size(); ++i) {
            names.insert(Qt::UserRole + i, wanted[i].toUtf8());
        }
        return names;
    }

    void Add(const Item& item) {
        beginInsertRows({}, static_cast<int>(Items_.size()),
                        static_cast<int>(Items_.size()));
        Items_.push_back(item);
        endInsertRows();
    }

    void Remove(int row) {
        beginRemoveRows({}, row, row);
        Items_.erase(Items_.begin() + row);
        endRemoveRows();
    }

    void SetState(int row, const QString& state) {
        Items_[static_cast<std::size_t>(row)].State = state;
        emit dataChanged(index(row), index(row));
    }

    void MoveRow(int from, int to) {
        beginMoveRows({}, from, from, {}, to > from ? to + 1 : to);
        Item item = std::move(Items_[static_cast<std::size_t>(from)]);
        Items_.erase(Items_.begin() + from);
        Items_.insert(Items_.begin() + to, std::move(item));
        endMoveRows();
    }

    // A sort of a proxy: the rows change places and only layoutChanged says so.
    void Reverse() {
        emit layoutAboutToBeChanged();
        std::ranges::reverse(Items_);
        emit layoutChanged();
    }

    void ResetTo(std::vector<Item> items) {
        beginResetModel();
        Items_ = std::move(items);
        endResetModel();
    }

    const std::vector<Item>& Items() const { return Items_; }

private:
    std::vector<Item> Items_;
};

QVariantMap Ref(const QString& id) { return {{"agent", AGENT}, {"id", id}}; }

QString Key(const QString& id) { return AGENT + "/" + id; }

void Settle() {
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

int RoleOf(const QAbstractItemModel& model, const char* name) {
    return model.roleNames().key(name, -1);
}

QVariant Role(const QAbstractItemModel& model, int row, const char* name) {
    return model.data(model.index(row, 0), RoleOf(model, name));
}

// The keys of the rows: "header:<category>" and "claude/<id>".
QStringList Rows(const QAbstractItemModel& model) {
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row) {
        out << Role(model, row, "key").toString();
    }
    return out;
}

// The key of the row under the cursor, empty if none.
QString Cur(const SessionsList& list) {
    return list.cursorRow() < 0
               ? QString()
               : Role(list, list.cursorRow(), "key").toString();
}

bool IsNone(const QVariant& value) {
    return value.metaType() == QMetaType::fromType<std::nullptr_t>();
}

QJsonArray Strings(const QStringList& list) {
    QJsonArray out;
    for (const QString& item : list) {
        out.append(item);
    }
    return out;
}

// A document of the given categories {id: category}, folds and pins.
QVariantMap Doc(const QMap<QString, QString>& categories,
                const QStringList& folded = {},
                const QStringList& pinned = {}) {
    QJsonObject cats;
    for (auto it = categories.begin(); it != categories.end(); ++it) {
        cats.insert(Key(it.key()), it.value());
    }
    return QJsonObject{{"version", 1},
                       {"categories", cats},
                       {"folded", Strings(folded)},
                       {"pinned", Strings(pinned)}}
        .toVariantMap();
}

// A list over a source, with a tester, the documents it asked to write and an
// acknowledgement of each of them unless told otherwise.
struct Env {
    FakeSource Source;
    SessionsList List;
    QAbstractItemModelTester Tester{
        &List, QAbstractItemModelTester::FailureReportingMode::QtTest};
    QList<QVariantMap> Saved;
    bool AutoAck = true;

    Env() {
        QObject::connect(&List, &SessionsList::saveRequested, &List,
                         [this](const QVariantMap& document) {
                             Saved.append(document);
                             if (AutoAck) {
                                 QMetaObject::invokeMethod(
                                     &List, [this] { List.saveFinished({}); },
                                     Qt::QueuedConnection);
                             }
                         });
    }

    // The sessions listed and loaded, the document read; writes settled.
    void Open(std::vector<Item> items, const QVariantMap& document = {}) {
        Source.ResetTo(std::move(items));
        List.setSource(&Source);
        List.setSourceLoaded(true);
        List.restore(document);
        Settle();
        Saved.clear();
    }

    void Reset(std::vector<Item> items, bool loaded) {
        Source.ResetTo(std::move(items));
        List.setSourceLoaded(loaded);
        Settle();
    }

    void CursorTo(const QString& key) {
        QVERIFY(List.setCursorRow(static_cast<int>(Rows(List).indexOf(key))));
    }
};

// The model as a view that applies only the signals sees it.
struct Mirror {
    explicit Mirror(const SessionsList& list) : List(list) {
        Rows = All();
        QObject::connect(&list, &QAbstractItemModel::rowsInserted, &list,
                         [this](const QModelIndex&, int first, int last) {
                             for (int row = first; row <= last; ++row) {
                                 Rows.insert(row, RowData(row));
                             }
                         });
        QObject::connect(&list, &QAbstractItemModel::rowsRemoved, &list,
                         [this](const QModelIndex&, int first, int last) {
                             Rows.remove(first, last - first + 1);
                         });
        QObject::connect(&list, &QAbstractItemModel::rowsMoved, &list,
                         [this](const QModelIndex&, int start, int end,
                                const QModelIndex&, int row) {
                             const int count = end - start + 1;
                             const QList<QVariantMap> moved =
                                 Rows.mid(start, count);
                             Rows.remove(start, count);
                             const int to = row > start ? row - count : row;
                             for (int i = 0; i < count; ++i) {
                                 Rows.insert(to + i, moved[i]);
                             }
                         });
        QObject::connect(
            &list, &QAbstractItemModel::dataChanged, &list,
            [this](const QModelIndex& from, const QModelIndex& to) {
                for (int row = from.row(); row <= to.row(); ++row) {
                    Rows[row] = RowData(row);
                }
            });
        QObject::connect(&list, &QAbstractItemModel::modelReset, &list, [this] {
            ++Resets;
            Rows = All();
        });
    }

    QVariantMap RowData(int row) const {
        QVariantMap out;
        const QHash<int, QByteArray> names = List.roleNames();
        for (auto it = names.begin(); it != names.end(); ++it) {
            out.insert(QString::fromUtf8(it.value()),
                       List.data(List.index(row, 0), it.key()));
        }
        return out;
    }

    QList<QVariantMap> All() const {
        QList<QVariantMap> out;
        for (int row = 0; row < List.rowCount(); ++row) {
            out.append(RowData(row));
        }
        return out;
    }

    const SessionsList& List;
    QList<QVariantMap> Rows;
    int Resets = 0;
};

const QMap<QString, QString> WORK_AND_ALPHA = {
    {"a", "work"}, {"b", "work"}, {"z", "alpha"}};

}  // namespace

class SessionsListTest : public QObject {
    Q_OBJECT

private slots:
    void TheCursorStopsOnAFoldedHeader();
    void UnfoldingTheHeaderUnderTheCursorGoesIn();
    void UnfoldingAllTakesTheCursorIntoItsCategory();
    void FoldingTheCursorsCategoryKeepsTheCursorOnItsHeader();
    void OnAFoldedHeaderThereIsNoCursorSession();
    void PinningFromAHeaderMovesItFirstWithTheCursor();
    void TheCursorCategoryOfAHeaderIsItsCategory();
    void ACategorySetBeforeTheSessionIsListedApplies();
    void ACategoryOutOfSightHasNoCursor();
    void AFoldedHeaderCountsItsUnread();
    void TheTabCountsCountWhatTheyShow();
    void APinIsSavedRestoredAndUndone();
    void AFoldIsSavedAndRestored();
    void ARemovedSessionMovesTheCursorToTheNextStop();
    void AResetPutsTheCursorBackElseOnTheOpenedElseFirst();
    void RowsInAnEmptyListPutTheCursorOnTheFirst();
    void NothingIsWrittenBeforeTheDocumentIsRead();
    void AFailedReadLeavesTheListReadOnly();
    void OneWriteInFlightAndTheLastEditWins();
    void TheEchoOfOurWriteChangesNothing();
    void AWriteFromAnotherWindowIsTakenIn();
    void ChangesAreMovesInsertsAndRemovesNotResets();
    void TheSourceRolesAreRoostSessionsRoles();
    void TheModuleLoadsFromThePluginDirectory();
    void RevealClearsTheQueryUnfoldsAndPutsTheCursorOnTheSession();
    void NoEchoOutlivesItsWrite();
    void AForeignDocumentInTheMiddleOfOurWriteEndsOnTheLastWriter();
    void AForeignDocumentTakesTheViewAndTheOpenedSessionAlong();
    void AFailedWriteIsWrittenAgainWithTheNextChangeOfTheSource();
    void AFailedWriteLeavesNoEchoBehind();
    void ADestroyedSourceEmptiesTheListAndUnloadsIt();
    void TheOpenedSessionSetBeforeAFailedReadIsKept();
    void ADocumentEqualToOurWriteEndsAFailedReread();
    void ARemovedStopMovesTheCursorToTheStopThatTookItsPlace();
    void AColourThatChangesIsAChangeOfTheRow();
    void RowsShownAfterTheSourceIsLoadedWithNoOtherSignal();
    void MovedAndRelaidSourceRowsChangeTheOrderInAGroup();
};

// test_fold_cursor.py:18
void SessionsListTest::TheCursorStopsOnAFoldedHeader() {
    Env env;
    env.Open({{"a"}, {"b"}, {"c"}},
             Doc({{"a", "work"}, {"b", "work"}}, {"work"}));
    // work folded; c uncategorized, its header is not a stop.
    QCOMPARE(Rows(env.List), (QStringList{"header:work", "header:", Key("c")}));
    QCOMPARE(Cur(env.List), QString("header:work"));
    env.List.move(1);
    QCOMPARE(Cur(env.List), Key("c"));
    env.List.move(1);
    QCOMPARE(Cur(env.List), Key("c"));
    env.List.move(-1);
    QCOMPARE(Cur(env.List), QString("header:work"));
    env.List.move(-1);
    QCOMPARE(Cur(env.List), QString("header:work"));
    // A tap on a header that is no stop does nothing.
    QVERIFY(!env.List.setCursorRow(1));
    QCOMPARE(Cur(env.List), QString("header:work"));
}

// test_fold_cursor.py:29, :44
void SessionsListTest::UnfoldingTheHeaderUnderTheCursorGoesIn() {
    Env env;
    env.Open({{"a"}, {"b"}, {"z"}}, Doc(WORK_AND_ALPHA, {"work"}));
    QCOMPARE(Rows(env.List),
             (QStringList{"header:alpha", Key("z"), "header:work"}));
    env.CursorTo("header:work");
    env.List.toggleFold("work");
    QCOMPARE(Cur(env.List), Key("a"));

    // A tap on the folded header, then the fold.
    env.List.toggleFold("work");
    QCOMPARE(Cur(env.List), QString("header:work"));
    QVERIFY(env.List.setCursorRow(
        static_cast<int>(Rows(env.List).indexOf("header:work"))));
    env.List.toggleFold("work");
    QCOMPARE(Cur(env.List), Key("a"));
}

// test_fold_cursor.py:37
void SessionsListTest::UnfoldingAllTakesTheCursorIntoItsCategory() {
    Env env;
    env.Open({{"a"}, {"b"}, {"z"}}, Doc(WORK_AND_ALPHA, {"work", "alpha"}));
    QCOMPARE(Rows(env.List), (QStringList{"header:alpha", "header:work"}));
    env.CursorTo("header:work");
    env.List.unfoldAll();
    QCOMPARE(Rows(env.List), (QStringList{"header:alpha", Key("z"),
                                          "header:work", Key("a"), Key("b")}));
    QCOMPARE(Cur(env.List), Key("a"));
}

// test_tree.py:72
void SessionsListTest::FoldingTheCursorsCategoryKeepsTheCursorOnItsHeader() {
    Env env;
    env.Open({{"a"}, {"b"}, {"z"}}, Doc(WORK_AND_ALPHA));
    env.CursorTo(Key("b"));
    env.List.toggleFold("work");
    QCOMPARE(Rows(env.List),
             (QStringList{"header:alpha", Key("z"), "header:work"}));
    QCOMPARE(Cur(env.List), QString("header:work"));
}

// test_fold_cursor.py:51
void SessionsListTest::OnAFoldedHeaderThereIsNoCursorSession() {
    Env env;
    env.Open({{"a"}, {"b"}}, Doc({{"a", "work"}, {"b", "work"}}, {"work"}));
    QCOMPARE(Cur(env.List), QString("header:work"));
    QVERIFY(IsNone(env.List.cursorSession()));
    QVERIFY(IsNone(env.List.refAt(env.List.cursorRow())));
    QCOMPARE(env.List.rowOf(Ref("a")), -1);

    env.List.toggleFold("work");
    QCOMPARE(env.List.cursorSession().toMap(), Ref("a"));
}

// test_fold_cursor.py:56
void SessionsListTest::PinningFromAHeaderMovesItFirstWithTheCursor() {
    Env env;
    env.Open({{"a"}, {"b"}, {"z"}}, Doc(WORK_AND_ALPHA, {"work"}));
    env.CursorTo("header:work");
    QCOMPARE(env.List.cursorRow(), 2);
    env.List.togglePin("work");
    QCOMPARE(Rows(env.List),
             (QStringList{"header:work", "header:alpha", Key("z")}));
    QCOMPARE(env.List.cursorRow(), 0);
    QCOMPARE(Cur(env.List), QString("header:work"));
}

// test_fold_cursor.py:63
void SessionsListTest::TheCursorCategoryOfAHeaderIsItsCategory() {
    Env env;
    env.Open({{"a"}, {"b"}, {"c"}, {"z"}},
             Doc({{"a", "work"}, {"b", "work"}, {"z", "alpha"}}, {"work"}));
    env.CursorTo("header:work");
    QCOMPARE(env.List.cursorCategory().toString(), QString("work"));
    env.CursorTo(Key("z"));
    QCOMPARE(env.List.cursorCategory().toString(), QString("alpha"));
    // The uncategorized is "", not nothing.
    env.CursorTo(Key("c"));
    QVERIFY(!IsNone(env.List.cursorCategory()));
    QCOMPARE(env.List.cursorCategory().toString(), QString());
}

// test_fold_cursor.py:63, step 11: a session started from a header.
void SessionsListTest::ACategorySetBeforeTheSessionIsListedApplies() {
    Env env;
    env.Open({});
    env.List.setCategory(Ref("new"), "work");
    QVERIFY(!env.Saved.isEmpty());
    QCOMPARE(env.Saved.last()["categories"].toMap()[Key("new")].toString(),
             QString("work"));
    env.Source.Add({"new"});
    Settle();
    QCOMPARE(Rows(env.List), (QStringList{"header:work", Key("new")}));
    QCOMPARE(Role(env.List, 1, "category").toString(), QString("work"));
}

// test_fold_cursor.py:68
void SessionsListTest::ACategoryOutOfSightHasNoCursor() {
    Env env;
    env.Open({{"a"}, {"b"}, {"c"}},
             Doc({{"a", "work"}, {"b", "work"}}, {"work"}));
    QCOMPARE(Cur(env.List), QString("header:work"));

    // Another category is all that is left in sight: the cursor goes to it.
    env.List.setQuery("c");
    QCOMPARE(Rows(env.List), (QStringList{"header:", Key("c")}));
    QCOMPARE(Cur(env.List), Key("c"));

    // Nothing in sight: no cursor at all.
    env.CursorTo(Key("c"));
    env.List.setQuery("nothing matches this");
    QCOMPARE(env.List.rowCount(), 0);
    QCOMPARE(env.List.cursorRow(), -1);
    QVERIFY(IsNone(env.List.cursorSession()));
    QVERIFY(IsNone(env.List.cursorCategory()));

    // A header gone by the query is not kept as the cursor.
    env.List.setQuery("");
    env.CursorTo("header:work");
    env.List.setQuery("c");
    QCOMPARE(Cur(env.List), Key("c"));
}

// test_tree.py:99
void SessionsListTest::AFoldedHeaderCountsItsUnread() {
    Env env;
    env.Open({{"a"}, {"b"}, {"c"}},
             Doc({{"a", "work"}, {"b", "work"}}, {"work"}));
    env.Source.SetState(0, "done");
    env.Source.SetState(1, "done");
    Settle();
    QCOMPARE(Rows(env.List), (QStringList{"header:work", "header:", Key("c")}));
    QCOMPARE(Role(env.List, 0, "count").toInt(), 2);
    QCOMPARE(Role(env.List, 0, "unreadCount").toInt(), 2);
    QVERIFY(Role(env.List, 0, "folded").toBool());
    QCOMPARE(Role(env.List, 2, "unreadCount"), QVariant());
}

// test_title.py:10
void SessionsListTest::TheTabCountsCountWhatTheyShow() {
    Env env;
    env.Open({{"a"}, {"b"}, {"c"}});
    const auto counts = [&] {
        const QVariantMap map = env.List.counts();
        return QList<int>{map["all"].toInt(), map["active"].toInt(),
                          map["unread"].toInt()};
    };
    QCOMPARE(counts(), (QList<int>{3, 3, 0}));
    // Working to done moves one from Active to Unread.
    env.Source.SetState(0, "done");
    Settle();
    QCOMPARE(counts(), (QList<int>{3, 2, 1}));
    // The counts are of the archive view, before the query and the tab.
    env.List.setQuery("b");
    QCOMPARE(counts(), (QList<int>{3, 2, 1}));
    env.List.setQuery("");
    env.List.setTab("unread");
    QCOMPARE(Rows(env.List), (QStringList{"header:", Key("a")}));
    QCOMPARE(counts(), (QList<int>{3, 2, 1}));
    env.List.setTab("all");
    env.List.setArchived(Ref("c"), true);
    QCOMPARE(counts(), (QList<int>{2, 1, 1}));
}

// test_pins.py:29
void SessionsListTest::APinIsSavedRestoredAndUndone() {
    Env env;
    env.Open({{"a"}, {"z"}}, Doc({{"a", "work"}, {"z", "alpha"}}));
    QCOMPARE(Rows(env.List),
             (QStringList{"header:alpha", Key("z"), "header:work", Key("a")}));
    env.List.togglePin("work");
    Settle();
    QCOMPARE(env.Saved.size(), 1);
    QCOMPARE(env.Saved.last()["pinned"].toStringList(), QStringList{"work"});

    Env again;
    again.Open({{"a"}, {"z"}}, env.Saved.last());
    QCOMPARE(Rows(again.List),
             (QStringList{"header:work", Key("a"), "header:alpha", Key("z")}));

    env.List.togglePin("work");
    Settle();
    QCOMPARE(env.Saved.size(), 2);
    QCOMPARE(env.Saved.last()["pinned"].toStringList(), QStringList{});
    QCOMPARE(Rows(env.List),
             (QStringList{"header:alpha", Key("z"), "header:work", Key("a")}));
}

// test_tree.py:86
void SessionsListTest::AFoldIsSavedAndRestored() {
    Env env;
    env.Open({{"a"}, {"b"}}, Doc({{"a", "work"}, {"b", "work"}}));
    env.List.toggleFold("work");
    Settle();
    QCOMPARE(env.Saved.size(), 1);
    QCOMPARE(env.Saved.last()["folded"].toStringList(), QStringList{"work"});

    Env again;
    again.Open({{"a"}, {"b"}}, env.Saved.last());
    QCOMPARE(Rows(again.List), (QStringList{"header:work"}));
    QCOMPARE(Role(again.List, 0, "count").toInt(), 2);
}

// 06-panels.md:15
void SessionsListTest::ARemovedSessionMovesTheCursorToTheNextStop() {
    Env env;
    env.Open({{"a"}, {"b"}, {"c"}});
    env.CursorTo(Key("b"));
    env.Source.Remove(1);
    Settle();
    QCOMPARE(Cur(env.List), Key("c"));
    // The last one: the previous.
    env.Source.Remove(1);
    Settle();
    QCOMPARE(Cur(env.List), Key("a"));
    env.Source.Remove(0);
    Settle();
    QCOMPARE(env.List.cursorRow(), -1);
}

// 06-panels.md:16
void SessionsListTest::AResetPutsTheCursorBackElseOnTheOpenedElseFirst() {
    Env env;
    env.Open({{"a"}, {"b"}, {"c"}});
    env.CursorTo(Key("b"));

    // The connection is gone: the list is empty and not loaded. Nothing is
    // taken until it is, and the cursor is not lost.
    env.Reset({}, false);
    QCOMPARE(env.List.rowCount(), 0);
    QCOMPARE(env.List.cursorRow(), -1);
    env.Reset({{"a"}, {"b"}, {"c"}}, true);
    QCOMPARE(Cur(env.List), Key("b"));

    // The same session gone: the opened one.
    env.List.setOpened(Ref("c"));
    env.Reset({}, false);
    env.Reset({{"a"}, {"c"}}, true);
    QCOMPARE(Cur(env.List), Key("c"));

    // Neither: the first.
    env.Reset({}, false);
    env.Reset({{"d"}, {"a"}}, true);
    QCOMPARE(Cur(env.List), Key("d"));
}

// 06-panels.md:17
void SessionsListTest::RowsInAnEmptyListPutTheCursorOnTheFirst() {
    Env env;
    env.Open({});
    QCOMPARE(env.List.cursorRow(), -1);
    env.Source.Add({"a"});
    Settle();
    QCOMPARE(env.List.cursorRow(), 1);
    QCOMPARE(env.List.cursorSession().toMap(), Ref("a"));
}

void SessionsListTest::NothingIsWrittenBeforeTheDocumentIsRead() {
    Env env;
    env.Source.ResetTo({{"a"}});
    env.List.setSource(&env.Source);
    env.List.setSourceLoaded(true);
    Settle();
    QVERIFY(!env.List.ready());
    env.List.togglePin("work");
    env.List.setQuery("x");
    env.List.setCategory(Ref("a"), "work");
    env.List.toggleFold("work");
    Settle();
    QVERIFY(env.Saved.isEmpty());
    QCOMPARE(env.List.query(), QString());
    QCOMPARE(env.List.rowCount(), 0);

    // The first read: what was done before it is not in the document.
    env.List.restore({});
    Settle();
    QVERIFY(env.List.ready());
    QCOMPARE(env.List.query(), QString());
    for (const QVariantMap& document : env.Saved) {
        QCOMPARE(document["pinned"].toStringList(), QStringList{});
        QCOMPARE(document["categories"].toMap().size(), 0);
    }
}

void SessionsListTest::AFailedReadLeavesTheListReadOnly() {
    Env env;
    env.Source.ResetTo({{"a"}, {"b"}});
    env.List.setSource(&env.Source);
    env.List.setSourceLoaded(true);
    env.List.readFailed("store: -32004");
    Settle();
    QVERIFY(env.List.ready());
    QVERIFY(env.List.readOnly());
    QVERIFY(env.List.problem().contains("-32004"));
    // The list shows; edits work in memory; nothing is written.
    QCOMPARE(Rows(env.List), (QStringList{"header:", Key("a"), Key("b")}));
    env.List.togglePin("work");
    env.List.setCategory(Ref("a"), "work");
    env.Source.SetState(0, "done");
    Settle();
    QCOMPARE(Rows(env.List).first(), QString("header:work"));
    QVERIFY(env.Saved.isEmpty());

    // A good document later ends it.
    env.List.restore({});
    Settle();
    QVERIFY(!env.List.readOnly());
    QVERIFY(env.List.problem().isEmpty());
}

void SessionsListTest::OneWriteInFlightAndTheLastEditWins() {
    Env env;
    env.Open({{"a"}, {"z"}}, Doc({{"a", "work"}, {"z", "alpha"}}));
    env.AutoAck = false;
    env.List.togglePin("work");
    QCOMPARE(env.Saved.size(), 1);
    env.List.togglePin("alpha");
    env.List.setQuery("a");
    env.List.setQuery("z");
    Settle();
    QCOMPARE(env.Saved.size(), 1);
    QCOMPARE(env.Saved[0]["pinned"].toStringList(), QStringList{"work"});

    env.List.saveFinished({});
    QCOMPARE(env.Saved.size(), 2);
    QCOMPARE(env.Saved[1]["pinned"].toStringList(),
             (QStringList{"work", "alpha"}));
    QCOMPARE(env.Saved[1]["view"].toMap()["query"].toString(), QString("z"));
    env.List.saveFinished({});
    QCOMPARE(env.Saved.size(), 2);

    // A failed write is reported and does not stop the next one.
    env.List.togglePin("work");
    QCOMPARE(env.Saved.size(), 3);
    env.List.saveFinished("disk full");
    QCOMPARE(env.List.problem(), QString("disk full"));
    QVERIFY(!env.List.readOnly());
    env.List.togglePin("work");
    QCOMPARE(env.Saved.size(), 4);
    env.List.saveFinished({});
    QVERIFY(env.List.problem().isEmpty());
}

void SessionsListTest::TheEchoOfOurWriteChangesNothing() {
    Env env;
    env.Open({{"a"}, {"z"}}, Doc({{"a", "work"}, {"z", "alpha"}}));
    env.AutoAck = false;
    env.CursorTo(Key("z"));
    env.List.togglePin("work");
    const QVariantMap first = env.Saved.last();
    env.List.togglePin("alpha");
    QCOMPARE(env.Saved.size(), 1);
    QCOMPARE(Rows(env.List),
             (QStringList{"header:work", Key("a"), "header:alpha", Key("z")}));

    QSignalSpy moved(&env.List, &QAbstractItemModel::rowsMoved);
    QSignalSpy inserted(&env.List, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&env.List, &QAbstractItemModel::rowsRemoved);
    QSignalSpy changed(&env.List, &QAbstractItemModel::dataChanged);
    QSignalSpy reset(&env.List, &QAbstractItemModel::modelReset);
    QSignalSpy cursor(&env.List, &SessionsList::cursorChanged);
    // The echo of the first write comes while the second edit waits.
    env.List.restore(first);
    Settle();
    QCOMPARE(moved.size() + inserted.size() + removed.size() + changed.size() +
                 reset.size() + cursor.size(),
             0);
    QCOMPARE(Rows(env.List),
             (QStringList{"header:work", Key("a"), "header:alpha", Key("z")}));
    // And it is not written back.
    env.List.saveFinished({});
    QCOMPARE(env.Saved.size(), 2);
    QCOMPARE(env.Saved[1]["pinned"].toStringList(),
             (QStringList{"work", "alpha"}));
}

void SessionsListTest::AWriteFromAnotherWindowIsTakenIn() {
    Env env;
    env.Open({{"a"}, {"z"}}, Doc({{"a", "work"}, {"z", "alpha"}}));
    env.CursorTo(Key("a"));
    QSignalSpy queryChanged(&env.List, &SessionsList::queryChanged);

    QVariantMap other =
        Doc({{"a", "work"}, {"z", "alpha"}}, {}, {"work", "alpha"});
    other["view"] = QVariantMap{{"query", "alpha"}, {"tab", "all"}};
    env.List.restore(other);
    Settle();
    QCOMPARE(Rows(env.List), (QStringList{"header:alpha", Key("z")}));
    QCOMPARE(env.List.query(), QString("alpha"));
    QCOMPARE(queryChanged.size(), 1);
    // The cursor's session is out of sight: the first stop.
    QCOMPARE(Cur(env.List), Key("z"));
    for (const QVariantMap& document : env.Saved) {
        QCOMPARE(document["view"].toMap()["query"].toString(),
                 QString("alpha"));
    }
}

void SessionsListTest::ChangesAreMovesInsertsAndRemovesNotResets() {
    // The steps alone, replayed on plain rows, give the rows they aim at.
    {
        QRandomGenerator random(5);
        const auto makeRows = [&random]() {
            std::vector<Row> rows;
            for (int i = 0; i < 8; ++i) {
                if (random.bounded(2) == 0) {
                    Row row;
                    row.Key = QStringLiteral("k%1").arg(i);
                    row.Title = QString::number(random.bounded(2));
                    rows.push_back(row);
                }
            }
            std::shuffle(rows.begin(), rows.end(), random);
            return rows;
        };
        for (int round = 0; round < 300; ++round) {
            const std::vector<Row> before = makeRows();
            const std::vector<Row> after = makeRows();
            std::vector<Row> rows = before;
            for (const RowStep& step : DiffRows(before, after)) {
                const auto to = static_cast<std::size_t>(step.To);
                switch (step.Kind) {
                    case ERowStep::Remove:
                        rows.erase(rows.begin() + step.From);
                        break;
                    case ERowStep::Move:
                        std::rotate(rows.begin() + step.To,
                                    rows.begin() + step.From,
                                    rows.begin() + step.From + 1);
                        break;
                    case ERowStep::Insert:
                        rows.insert(rows.begin() + step.To, after[to]);
                        break;
                    case ERowStep::Change:
                        rows[to] = after[to];
                        break;
                }
            }
            QVERIFY(rows == after);
        }
    }
    const QStringList states = {"working", "idle", "done"};
    const QStringList categories = {"", "work", "play", "x"};
    for (const quint32 seed : {1u, 7u, 42u}) {
        QRandomGenerator random(seed);
        Env env;
        env.Open({{"a0"}, {"a1"}, {"a2"}});
        Mirror mirror(env.List);
        QStringList absent = {"b0", "b1", "b2", "b3", "b4", "b5"};
        const auto pick = [&random](const QStringList& from) {
            return from[random.bounded(static_cast<int>(from.size()))];
        };
        for (int step = 0; step < 150; ++step) {
            const std::vector<Item>& items = env.Source.Items();
            const auto anyRow = [&]() {
                return static_cast<std::size_t>(
                    random.bounded(static_cast<int>(items.size())));
            };
            switch (random.bounded(12)) {
                case 0:
                    if (!absent.isEmpty()) {
                        env.Source.Add({absent.takeFirst(), pick(states)});
                    }
                    break;
                case 1:
                    if (!items.empty()) {
                        const std::size_t row = anyRow();
                        absent.append(items[row].Id);
                        env.Source.Remove(static_cast<int>(row));
                    }
                    break;
                case 2:
                    if (!items.empty()) {
                        env.Source.SetState(static_cast<int>(anyRow()),
                                            pick(states));
                    }
                    break;
                case 3:
                    env.List.togglePin(pick(categories));
                    break;
                case 4:
                    env.List.toggleFold(pick(categories));
                    break;
                case 5:
                    if (!items.empty()) {
                        env.List.setCategory(Ref(items[anyRow()].Id),
                                             pick(categories));
                    }
                    break;
                case 6:
                    if (!items.empty()) {
                        env.List.rename(Ref(items[anyRow()].Id),
                                        pick({"", "n1", "n2"}));
                    }
                    break;
                case 7:
                    if (!items.empty()) {
                        env.List.setArchived(Ref(items[anyRow()].Id),
                                             random.bounded(2) == 0);
                    }
                    break;
                case 8:
                    env.List.setQuery(pick({"", "a", "work", "n1", "done"}));
                    break;
                case 9:
                    env.List.setTab(pick({"all", "active", "unread"}));
                    break;
                case 10:
                    env.List.setViewArchive(random.bounded(2) == 0);
                    break;
                default:
                    if (random.bounded(3) == 0) {
                        env.List.unfoldAll();
                    } else {
                        env.List.move(random.bounded(2) == 0 ? 1 : -1);
                    }
                    break;
            }
            Settle();
            QCOMPARE(mirror.Rows, mirror.All());
            QCOMPARE(mirror.Resets, 0);
        }
    }
}

void SessionsListTest::TheSourceRolesAreRoostSessionsRoles() {
    QQmlEngine engine;
    auto* singleton =
        engine.singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
    QVERIFY(singleton != nullptr);
    api::Api api;
    singleton->setApi(api);
    SessionsModel* real = singleton->sessions();
    QVERIFY(real != nullptr);

    QStringList roost;
    for (const QByteArray& name : real->roleNames()) {
        roost << QString::fromUtf8(name);
    }
    roost.sort();
    QStringList wanted = SessionsList::sourceRoleNames();
    wanted.sort();
    QCOMPARE(wanted, roost);
    QStringList fields = api::SessionService::FIELDS;
    fields.sort();
    QCOMPARE(wanted, fields);

    // The real model is a source as it is.
    SessionsList list;
    QAbstractItemModelTester tester(
        &list, QAbstractItemModelTester::FailureReportingMode::QtTest);
    list.setSource(real);
    list.setSourceLoaded(real->loaded());
    list.restore({});
    Settle();
    QVERIFY(list.ready());
    QCOMPARE(list.rowCount(), 0);
}

void SessionsListTest::TheModuleLoadsFromThePluginDirectory() {
    QQmlEngine engine;
    engine.addImportPath(SESSIONS_QML_IMPORT_PATH);
    QQmlComponent component(&engine);
    component.setData(R"(
import QtQml
import RoostSessions

SessionsList {
}
)",
                      QUrl());
    std::unique_ptr<QObject> list(component.create());
    QVERIFY2(list != nullptr, qPrintable(component.errorString()));
    QCOMPARE(list->property("tab").toString(), QString("all"));
    QVERIFY(QMetaObject::invokeMethod(list.get(), "restore",
                                      Q_ARG(QVariantMap, QVariantMap())));
    QVERIFY(list->setProperty("tab", "unread"));
    QCOMPARE(list->property("tab").toString(), QString("unread"));
    QVERIFY(list->metaObject()->indexOfMethod("toggleFold(QString)") >= 0);
    QVERIFY(list->metaObject()->indexOfProperty("cursorSession") >= 0);
    QVERIFY(qobject_cast<QAbstractListModel*>(list.get()) != nullptr);
}

void SessionsListTest::
    RevealClearsTheQueryUnfoldsAndPutsTheCursorOnTheSession() {
    Env env;
    env.Open({{"a"}, {"b"}, {"z"}}, Doc(WORK_AND_ALPHA, {"work"}));
    env.List.setArchived(Ref("b"), true);
    env.List.setQuery("z");
    QVERIFY(env.List.reveal(Ref("b")));
    QCOMPARE(env.List.query(), QString());
    QVERIFY(env.List.viewArchive());
    QCOMPARE(Cur(env.List), Key("b"));
    QVERIFY(!env.List.reveal(Ref("missing")));
}

// 06-panels.md, 3.6: roostd announces every successful write, equal or not,
// before it answers (store_service.cpp:94-95); an echo belongs to its write.
void SessionsListTest::NoEchoOutlivesItsWrite() {
    Env env;
    env.Open({{"a"}, {"z"}}, Doc({{"a", "work"}, {"z", "alpha"}}));
    env.AutoAck = false;
    env.List.togglePin("work");
    const QVariantMap first = env.Saved.last();
    env.List.restore(first);  // its echo
    env.List.saveFinished({});
    for (const QString& pin : {QString("alpha"), QString("work")}) {
        env.List.togglePin(pin);
        env.List.restore(env.Saved.last());
        env.List.saveFinished({});
    }
    QCOMPARE(env.Saved.size(), 3);
    QCOMPARE(Rows(env.List).first(), QString("header:alpha"));

    // Another window writes something else, then a document equal to our
    // first write: that one is a write of its own and is taken in, however
    // long ago we wrote it.
    env.List.restore(Doc({{"a", "work"}, {"z", "alpha"}}, {}, {"alpha"}));
    QCOMPARE(Rows(env.List).first(), QString("header:alpha"));
    env.List.restore(first);
    QCOMPARE(Rows(env.List),
             (QStringList{"header:work", Key("a"), "header:alpha", Key("z")}));
}

// restore(foreign), restore(echo of ours), saveFinished: roostd saw the
// foreign write first and ours after it, so the store ends on ours and so does
// the list; nothing is written back.
void SessionsListTest::
    AForeignDocumentInTheMiddleOfOurWriteEndsOnTheLastWriter() {
    Env env;
    env.Open({{"a"}, {"z"}}, Doc({{"a", "work"}, {"z", "alpha"}}));
    env.AutoAck = false;
    env.List.togglePin("work");
    const QVariantMap ours = env.Saved.last();
    QCOMPARE(Rows(env.List).first(), QString("header:work"));

    env.List.restore(Doc({{"a", "work"}, {"z", "alpha"}}, {}, {"alpha"}));
    QCOMPARE(Rows(env.List).first(), QString("header:alpha"));
    env.List.restore(ours);
    QCOMPARE(Rows(env.List).first(), QString("header:work"));
    env.List.saveFinished({});
    Settle();
    QCOMPARE(Rows(env.List).first(), QString("header:work"));
    QCOMPARE(env.Saved.size(), 1);
}

// 06-panels.md, [APPROVAL 3]: query, tab, archive, compact and opened are in
// the shared document; the last whole-document writer wins, the view's state
// with it (two list panels are M6).
void SessionsListTest::AForeignDocumentTakesTheViewAndTheOpenedSessionAlong() {
    Env env;
    env.Open({{"a"}, {"z"}}, Doc({{"a", "work"}, {"z", "alpha"}}));
    env.List.setOpened(Ref("a"));
    Settle();
    QSignalSpy openedChanged(&env.List, &SessionsList::openedChanged);
    QSignalSpy tabChanged(&env.List, &SessionsList::tabChanged);
    QSignalSpy compactChanged(&env.List, &SessionsList::compactChanged);

    QVariantMap other = Doc({{"a", "work"}, {"z", "alpha"}});
    other["view"] = QVariantMap{{"query", ""},
                                {"tab", "active"},
                                {"compact", true},
                                {"opened", Key("z")}};
    env.List.restore(other);
    Settle();
    QCOMPARE(env.List.opened().toMap(), Ref("z"));
    QCOMPARE(env.List.tab(), QString("active"));
    QVERIFY(env.List.compact());
    QCOMPARE(openedChanged.size(), 1);
    QCOMPARE(tabChanged.size(), 1);
    QCOMPARE(compactChanged.size(), 1);
    QVERIFY(Role(env.List, env.List.rowOf(Ref("z")), "isOpen").toBool());
}

void SessionsListTest::
    AFailedWriteIsWrittenAgainWithTheNextChangeOfTheSource() {
    Env env;
    env.Open({{"a"}, {"z"}}, Doc({{"a", "work"}, {"z", "alpha"}}));
    env.AutoAck = false;
    env.List.togglePin("work");
    const QVariantMap failed = env.Saved.last();
    env.List.saveFinished("disk full");
    QCOMPARE(env.List.problem(), QString("disk full"));
    Settle();
    QCOMPARE(env.Saved.size(), 1);

    // A change of the source that changes nothing of the document.
    env.Source.SetState(0, "working");
    Settle();
    QCOMPARE(env.Saved.size(), 2);
    QCOMPARE(env.Saved.last(), failed);
    env.List.saveFinished({});
    QVERIFY(env.List.problem().isEmpty());
    // Done: nothing more is written.
    env.Source.SetState(0, "working");
    Settle();
    QCOMPARE(env.Saved.size(), 2);

    // An edit made while the write that failed was in flight goes out with the
    // retry as well, once, in one document.
    env.List.togglePin("alpha");
    env.List.setQuery("a");
    env.List.saveFinished("again");
    QCOMPARE(env.Saved.size(), 3);
    env.Source.SetState(0, "working");
    Settle();
    QCOMPARE(env.Saved.size(), 4);
    QCOMPARE(env.Saved.last()["view"].toMap()["query"].toString(),
             QString("a"));
}

void SessionsListTest::AFailedWriteLeavesNoEchoBehind() {
    Env env;
    env.Open({{"a"}, {"z"}}, Doc({{"a", "work"}, {"z", "alpha"}}));
    env.AutoAck = false;
    env.List.togglePin("work");
    const QVariantMap failed = env.Saved.last();
    env.List.setQuery("z");  // an edit while it was in flight
    env.List.saveFinished("disk full");
    QCOMPARE(env.Saved.size(), 1);
    QCOMPARE(env.List.query(), QString("z"));

    // Another window writes what we failed to: nothing of ours came back from
    // it, so it is a write of its own and is taken in.
    env.List.restore(failed);
    QCOMPARE(env.List.query(), QString());
    QCOMPARE(Rows(env.List).first(), QString("header:work"));
}

void SessionsListTest::ADestroyedSourceEmptiesTheListAndUnloadsIt() {
    Env env;
    env.Open({});
    auto* source = new FakeSource;
    source->ResetTo({{"a"}, {"b"}});
    env.List.setSource(source);
    env.List.setSourceLoaded(true);
    Settle();
    QCOMPARE(env.List.rowCount(), 3);
    QSignalSpy loaded(&env.List, &SessionsList::sourceLoadedChanged);

    delete source;
    Settle();
    QCOMPARE(env.List.rowCount(), 0);
    QCOMPARE(env.List.cursorRow(), -1);
    QVERIFY(!env.List.sourceLoaded());
    QCOMPARE(loaded.size(), 1);

    // A new source is taken once it says it is loaded.
    env.Source.ResetTo({{"c"}});
    env.List.setSource(&env.Source);
    Settle();
    QCOMPARE(env.List.rowCount(), 0);
    env.List.setSourceLoaded(true);
    Settle();
    QCOMPARE(Rows(env.List), (QStringList{"header:", Key("c")}));
}

void SessionsListTest::TheOpenedSessionSetBeforeAFailedReadIsKept() {
    Env env;
    env.Source.ResetTo({{"a"}, {"b"}});
    env.List.setSource(&env.Source);
    env.List.setSourceLoaded(true);
    env.List.setOpened(Ref("b"));
    env.List.readFailed("store: -32004");
    Settle();
    QCOMPARE(env.List.opened().toMap(), Ref("b"));
    QVERIFY(Role(env.List, env.List.rowOf(Ref("b")), "isOpen").toBool());
    QVERIFY(env.Saved.isEmpty());
}

// A read fails after we wrote; the good document that follows is equal to
// what we wrote. It is no echo: it ends the read-only state.
void SessionsListTest::ADocumentEqualToOurWriteEndsAFailedReread() {
    Env env;
    env.Open({{"a"}, {"z"}}, Doc({{"a", "work"}, {"z", "alpha"}}));
    env.AutoAck = false;
    env.List.togglePin("work");
    const QVariantMap ours = env.Saved.last();
    env.List.readFailed("store: -32004");
    QVERIFY(env.List.readOnly());
    env.List.restore(ours);
    QVERIFY(!env.List.readOnly());
    QVERIFY(env.List.problem().isEmpty());
}

void SessionsListTest::ARemovedStopMovesTheCursorToTheStopThatTookItsPlace() {
    // A session in the middle: the next one, not the last.
    {
        Env env;
        env.Open({{"a"}, {"b"}, {"c"}, {"d"}});
        env.CursorTo(Key("b"));
        env.Source.Remove(1);
        Settle();
        QCOMPARE(Cur(env.List), Key("c"));
    }
    // A folded header whose category is gone: the stop after it.
    {
        Env env;
        env.Open({{"a"}, {"b"}, {"c"}, {"d"}, {"e"}},
                 Doc({{"a", "work"}, {"b", "work"}, {"c", "alpha"}}, {"work"}));
        QCOMPARE(Rows(env.List),
                 (QStringList{"header:alpha", Key("c"), "header:work",
                              "header:", Key("d"), Key("e")}));
        env.CursorTo("header:work");
        env.Source.Remove(1);
        env.Source.Remove(0);
        Settle();
        QCOMPARE(Cur(env.List), Key("d"));
    }
}

void SessionsListTest::AColourThatChangesIsAChangeOfTheRow() {
    Env env;
    QVariantMap first = Doc({{"a", "work"}, {"z", "alpha"}});
    first["colors"] = QVariantMap{{"work", 1}, {"alpha", 2}};
    env.Open({{"a"}, {"z"}}, first);
    const int work = env.List.rowOf(Ref("a")) - 1;
    QCOMPARE(Role(env.List, work, "colorIndex").toInt(), 1);

    QSignalSpy changed(&env.List, &QAbstractItemModel::dataChanged);
    QSignalSpy reset(&env.List, &QAbstractItemModel::modelReset);
    QVariantMap second = first;
    second["colors"] = QVariantMap{{"work", 5}, {"alpha", 2}};
    env.List.restore(second);
    Settle();
    QCOMPARE(Role(env.List, work, "colorIndex").toInt(), 5);
    QCOMPARE(reset.size(), 0);
    QVERIFY(!changed.isEmpty());
    bool announced = false;
    for (const QList<QVariant>& call : changed) {
        const QList<int> roles = call.at(2).value<QList<int>>();
        announced |= roles.contains(RoleOf(env.List, "colorIndex"));
    }
    QVERIFY(announced);
}

// The rows of a source that is not loaded wait; the flag alone releases them.
void SessionsListTest::RowsShownAfterTheSourceIsLoadedWithNoOtherSignal() {
    Env env;
    env.Open({});
    env.Reset({{"a"}, {"b"}}, false);
    QCOMPARE(env.List.rowCount(), 0);
    env.List.setSourceLoaded(true);
    Settle();
    QCOMPARE(Rows(env.List), (QStringList{"header:", Key("a"), Key("b")}));
}

void SessionsListTest::MovedAndRelaidSourceRowsChangeTheOrderInAGroup() {
    Env env;
    env.Open({{"a"}, {"b"}, {"c"}});
    env.Source.MoveRow(2, 0);
    Settle();
    QCOMPARE(Rows(env.List),
             (QStringList{"header:", Key("c"), Key("a"), Key("b")}));
    env.Source.Reverse();
    Settle();
    QCOMPARE(Rows(env.List),
             (QStringList{"header:", Key("b"), Key("a"), Key("c")}));
}

QTEST_MAIN(SessionsListTest)
#include "sessions_list_test.moc"
