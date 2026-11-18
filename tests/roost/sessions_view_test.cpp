// The view of the sessions panel in the shell's window: the head with the
// title and the tabs, the marks and colours of the rows, and the keys that act
// on the view (a, v, m, p, z, Z, ?, < and >), on the US and the Russian
// layouts. The window, the daemon and the fake `claude` are
// helpers/sessions_window.hpp; the panel and the model are the real ones.

#include <QColor>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExtensionPlugin>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTest>

#include "helpers/sessions_window.hpp"

using namespace roost;
using namespace roost::testing;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

// The keys of the sessions in the document `sessions`.
QString KeyOf(const QString& id) { return "claude/" + id; }

// The width of the list slot before a step, and what a step and the bounds are
// in src/roost/Main.qml.
constexpr int LIST_WIDTH = 320;
constexpr int WIDTH_STEP = 20;
constexpr int WIDTH_MIN = 200;
constexpr int WIDTH_MAX = 560;

}  // namespace

class SessionsViewTest : public SessionsWindow {
    Q_OBJECT

private:
    // The document `sessions` as the daemon finds it at the start, so that the
    // panel reads it with the first list and writes nothing before the case.
    bool Seed(const QJsonObject& document) {
        if (!QDir().mkpath(Dir_->filePath("config"))) {
            return false;
        }
        QSaveFile file(Dir_->filePath("config/sessions.json"));
        return file.open(QIODevice::WriteOnly) &&
               file.write(QJsonDocument(document).toJson()) >= 0 &&
               file.commit();
    }

    // A document of version 1 with the given sections.
    static QJsonObject Document(const QJsonObject& sections) {
        QJsonObject document = sections;
        document.insert("version", 1);
        return document;
    }

    QQuickItem* Part(const QString& name) {
        return PanelOf(LIST)->findChild<QQuickItem*>(name);
    }

    QString Title() {
        return Part("sessionsTitle")->property("text").toString();
    }

    // The items below `item` with the object name, by their visual parents:
    // what a Repeater makes is not a QObject child of anything findChildren
    // reaches.
    static void Collect(QQuickItem* item, const QString& name,
                        QList<QQuickItem*>& found) {
        for (QQuickItem* child : item->childItems()) {
            if (child->objectName() == name) {
                found << child;
            }
            Collect(child, name, found);
        }
    }

    QList<QQuickItem*> TabItems() {
        QList<QQuickItem*> found;
        Collect(Part("sessionsHead"), "sessionsTab", found);
        return found;
    }

    // The texts of the tabs, in order.
    QStringList Tabs() {
        QStringList texts;
        for (QQuickItem* tab : TabItems()) {
            texts << tab->property("text").toString();
        }
        return texts;
    }

    QString TabNow() { return List()->property("tab").toString(); }

    bool HelpOpen() { return PanelOf(LIST)->property("helpOpen").toBool(); }

    QJsonObject StoredView() {
        return StoredDocument().value("view").toObject();
    }

    int ListWidth() { return static_cast<int>(Slot(LIST)->width()); }

    // The part of the row's delegate, whatever the row; null if it is not made.
    QQuickItem* InRow(int viewRow, const QString& name) {
        QQuickItem* item = ViewItem(viewRow);
        return item == nullptr ? nullptr : item->findChild<QQuickItem*>(name);
    }

    // A tap at the centre of the view's row, a header included.
    void TapViewRow(int viewRow) {
        QQuickItem* item = ViewItem(viewRow);
        QVERIFY(item != nullptr);
        QTest::mouseClick(
            Window(), Qt::LeftButton, Qt::NoModifier,
            item->mapToScene(QPointF(item->width() / 2, item->height() / 2))
                .toPoint());
    }

    // The keys of the symbols: the Qt key and the text of the layout, with the
    // native codes of the physical key (kVK_ANSI_*, KEY_* + 8).
    void PressShifted(int key, const QString& text, quint32 mac, quint32 xkb) {
        PressNative(key, text, mac, xkb, Qt::ShiftModifier);
    }

    // Moves the cursor down to the session.
    void MoveTo(const QString& id) {
        for (int step = 0; step < 8 && CursorId() != id; ++step) {
            Press(Qt::Key_Down);
        }
        QCOMPARE(CursorId(), id);
    }

    void BootWith(const QJsonObject& document, int rows = 4) {
        QVERIFY(Seed(document));
        Boot(rows);
    }

private slots:
    void init() { Prepare(); }

    void cleanup() { Teardown(); }

    void TheHeadShowsTitleAndTabCounts() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        QCOMPARE(Title(), QString("Sessions"));
        // Only GAMMA is done.
        QCOMPARE(Tabs(), (QStringList{"All 4", "Active 3", "Unread 0"}));
        QVERIFY(!Part("sessionsQuery")->isVisible());
        QVERIFY(!Part("sessionsProblem")->isVisible());
        QVERIFY(!HelpOpen());
    }

    void TappingATabSelectsIt() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        const auto tabs = TabItems();
        QCOMPARE(tabs.size(), 3);
        QTest::mouseClick(Window(), Qt::LeftButton, Qt::NoModifier,
                          tabs[1]
                              ->mapToScene(QPointF(tabs[1]->width() / 2,
                                                   tabs[1]->height() / 2))
                              .toPoint());

        QTRY_COMPARE_WITH_TIMEOUT(TabNow(), QString("active"), TIMEOUT_MS);
        QCOMPARE(Ids(), (QStringList{ALPHA, BETA, DELTA}));
        QVERIFY(tabs[1]->property("current").toBool());
        QVERIFY(ListView()->hasActiveFocus());
    }

    void ACyclesTabsAndSavesThem() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        Press(Qt::Key_A);
        QTRY_COMPARE_WITH_TIMEOUT(TabNow(), QString("active"), TIMEOUT_MS);
        QCOMPARE(Ids(), (QStringList{ALPHA, BETA, DELTA}));
        QTRY_COMPARE_WITH_TIMEOUT(StoredView().value("tab").toString(),
                                  QString("active"), TIMEOUT_MS);

        Press(Qt::Key_A);
        QTRY_COMPARE_WITH_TIMEOUT(TabNow(), QString("unread"), TIMEOUT_MS);
        QCOMPARE(Count(), 0);

        Press(Qt::Key_A);
        QTRY_COMPARE_WITH_TIMEOUT(TabNow(), QString("all"), TIMEOUT_MS);
        QCOMPARE(Ids(), ORDER);
        QTRY_COMPARE_WITH_TIMEOUT(StoredView().value("tab").toString(),
                                  QString("all"), TIMEOUT_MS);
        // Nothing was opened on the way.
        QVERIFY(SelectedId().isEmpty());
    }

    void RussianFCyclesTabs() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        // Cyrillic "ф" sits on the A key: kVK_ANSI_A 0x00 and KEY_A + 8.
        PressNative(0x0424, QStringLiteral("ф"), 0x00, 38);

        QTRY_COMPARE_WITH_TIMEOUT(TabNow(), QString("active"), TIMEOUT_MS);
        QCOMPARE(Ids(), (QStringList{ALPHA, BETA, DELTA}));
    }

    void VShowsTheArchive() {
        FailOnWarnings();
        BootWith(Document({{"archived", QJsonArray{KeyOf(GAMMA)}}}), 3);
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(Ids(), (QStringList{ALPHA, BETA, DELTA}));
        QCOMPARE(Tabs(), (QStringList{"All 3", "Active 3", "Unread 0"}));

        Press(Qt::Key_V);
        QTRY_COMPARE_WITH_TIMEOUT(Title(), QString("Archive"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), (QStringList{GAMMA}), TIMEOUT_MS);
        QCOMPARE(Tabs(), (QStringList{"All 1", "Active 0", "Unread 0"}));
        QTRY_VERIFY_WITH_TIMEOUT(StoredView().value("archive").toBool(),
                                 TIMEOUT_MS);

        Press(Qt::Key_V);
        QTRY_COMPARE_WITH_TIMEOUT(Title(), QString("Sessions"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), (QStringList{ALPHA, BETA, DELTA}),
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!StoredView().value("archive").toBool(),
                                 TIMEOUT_MS);
    }

    void MTogglesCompact() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        for (int row = 0; row < 4; ++row) {
            QVERIFY(RowItem(row)
                        ->findChild<QQuickItem*>("sessionDetail")
                        ->isVisible());
        }

        Press(Qt::Key_M);

        // Only the cursor's row keeps its second line.
        QTRY_VERIFY_WITH_TIMEOUT(
            RowItem(0)->findChild<QQuickItem*>("sessionDetail")->isVisible() &&
                !RowItem(1)
                     ->findChild<QQuickItem*>("sessionDetail")
                     ->isVisible() &&
                !RowItem(2)
                     ->findChild<QQuickItem*>("sessionDetail")
                     ->isVisible() &&
                !RowItem(3)
                     ->findChild<QQuickItem*>("sessionDetail")
                     ->isVisible(),
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(StoredView().value("compact").toBool(),
                                 TIMEOUT_MS);
        Press(Qt::Key_Down);
        QTRY_VERIFY_WITH_TIMEOUT(
            !RowItem(0)->findChild<QQuickItem*>("sessionDetail")->isVisible() &&
                RowItem(1)
                    ->findChild<QQuickItem*>("sessionDetail")
                    ->isVisible(),
            TIMEOUT_MS);

        Press(Qt::Key_M);
        QTRY_VERIFY_WITH_TIMEOUT(
            RowItem(0)->findChild<QQuickItem*>("sessionDetail")->isVisible() &&
                RowItem(3)
                    ->findChild<QQuickItem*>("sessionDetail")
                    ->isVisible(),
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!StoredView().value("compact").toBool(),
                                 TIMEOUT_MS);
    }

    void PPinsTheCategoryUnderTheCursor() {
        FailOnWarnings();
        // The categories are in alphabetical order, the uncategorized last.
        BootWith(
            Document({{"categories", QJsonObject{{KeyOf(BETA), "Ops"},
                                                 {KeyOf(DELTA), "Dev"}}}}));
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(Ids(), (QStringList{DELTA, BETA, ALPHA, GAMMA}));
        QVERIFY(!InRow(0, "pinMark")->isVisible());
        MoveTo(BETA);

        Press(Qt::Key_P);

        QTRY_COMPARE_WITH_TIMEOUT(
            Ids(), (QStringList{BETA, DELTA, ALPHA, GAMMA}), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            InRow(0, "pinMark") != nullptr && InRow(0, "pinMark")->isVisible(),
            TIMEOUT_MS);
        QCOMPARE(Role(0, "category").toString(), QString("Ops"));
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument().value("pinned").toArray(),
                                  QJsonArray{"Ops"}, TIMEOUT_MS);
        // The cursor stays on the session.
        QCOMPARE(CursorId(), BETA);

        Press(Qt::Key_P);
        QTRY_COMPARE_WITH_TIMEOUT(
            Ids(), (QStringList{DELTA, BETA, ALPHA, GAMMA}), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument().value("pinned").toArray(),
                                  QJsonArray{}, TIMEOUT_MS);
    }

    void ZFoldsAndShiftZUnfolds() {
        FailOnWarnings();
        // Rows: Ops, DELTA, "" (uncategorized), ALPHA, BETA, GAMMA.
        BootWith(
            Document({{"categories", QJsonObject{{KeyOf(DELTA), "Ops"}}}}));
        if (QTest::currentTestFailed()) {
            return;
        }
        MoveTo(BETA);

        Press(Qt::Key_Z);

        // The category of the cursor is folded and the cursor is on its header.
        QTRY_COMPARE_WITH_TIMEOUT(
            Kinds(), (QStringList{"header", "session", "header"}), TIMEOUT_MS);
        QVERIFY(!Role(0, "folded").toBool());
        QVERIFY(Role(2, "folded").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(CursorRow(), 2, TIMEOUT_MS);
        QCOMPARE(Cursor(), ON_HEADER);

        Press(Qt::Key_Z, Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 4, TIMEOUT_MS);
        QVERIFY(!Role(2, "folded").toBool());

        // Fold, then unfold with Shift+я of the Russian layout: kVK_ANSI_Z 0x06
        // and KEY_Z + 8.
        MoveTo(BETA);
        Press(Qt::Key_Z);
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 1, TIMEOUT_MS);
        PressShifted(0x042F, QStringLiteral("Я"), 0x06, 52);
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 4, TIMEOUT_MS);
        // Without Shift, я folds.
        MoveTo(BETA);
        PressNative(0x044F, QStringLiteral("я"), 0x06, 52);
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 1, TIMEOUT_MS);
    }

    void ZOnAnUncategorizedSessionFoldsUncategorized() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(Role(0, "category").toString(), QString());

        Press(Qt::Key_Z);

        // The category "" is a category: it folds, not nothing.
        QTRY_COMPARE_WITH_TIMEOUT(Kinds(), (QStringList{"header"}), TIMEOUT_MS);
        QVERIFY(Role(0, "folded").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument().value("folded").toArray(),
                                  QJsonArray{QString()}, TIMEOUT_MS);
    }

    void TappingAHeaderFoldsAndOpensNothing() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QSignalSpy activated(PanelOf(LIST), SIGNAL(activated()));
        QSignalSpy selected(PanelOf(LIST), SIGNAL(selectedChanged()));
        // The keyboard is elsewhere: a tap on the header takes it to the list.
        QVERIFY(QMetaObject::invokeMethod(Slot(TERMINAL), "forceActiveFocus"));
        QTRY_VERIFY_WITH_TIMEOUT(!ListView()->hasActiveFocus(), TIMEOUT_MS);

        TapViewRow(0);

        QTRY_COMPARE_WITH_TIMEOUT(Kinds(), (QStringList{"header"}), TIMEOUT_MS);
        QVERIFY(Role(0, "folded").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), ON_HEADER, TIMEOUT_MS);
        QVERIFY(ListView()->hasActiveFocus());
        QVERIFY(SelectedId().isEmpty());
        QCOMPARE(activated.count(), 0);
        QCOMPARE(selected.count(), 0);

        TapViewRow(0);

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 4, TIMEOUT_MS);
        QVERIFY(!Role(0, "folded").toBool());
        QVERIFY(SelectedId().isEmpty());
        QCOMPARE(activated.count(), 0);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(Status(), QString("none"));
    }

    void QuestionMarkOpensHelpOnBothLayouts() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(!Part("sessionsHelp")->isVisible());

        // US: Shift+/ is "?" (kVK_ANSI_Slash 0x2C, KEY_SLASH + 8).
        PressShifted(Qt::Key_Question, "?", 0x2C, 61);
        QVERIFY(HelpOpen());
        QVERIFY(Part("sessionsHelp")->isVisible());
        // The help takes the keys: j moves nothing, z folds nothing.
        Press(Qt::Key_J);
        Press(Qt::Key_Z);
        QCOMPARE(Cursor(), 0);
        QCOMPARE(Count(), 4);
        QVERIFY(HelpOpen());
        Press(Qt::Key_Escape);
        QVERIFY(!HelpOpen());
        QVERIFY(!Part("sessionsHelp")->isVisible());
        // And it gives them back.
        Press(Qt::Key_J);
        QCOMPARE(Cursor(), 1);

        // Russian: Shift+7 is "?" (kVK_ANSI_7 0x1A, KEY_7 + 8); the same key
        // closes it.
        PressShifted(Qt::Key_Question, "?", 0x1A, 16);
        QVERIFY(HelpOpen());
        PressShifted(Qt::Key_Question, "?", 0x1A, 16);
        QVERIFY(!HelpOpen());
        // Without Shift the key is a "7": no help.
        PressNative(Qt::Key_7, "7", 0x1A, 16);
        QVERIFY(!HelpOpen());
    }

    void AngleBracketsResizeTheList() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), LIST_WIDTH, TIMEOUT_MS);

        // US: Shift+, and Shift+. (kVK_ANSI_Comma 0x2B, KEY_COMMA + 8; period
        // 0x2F, KEY_DOT + 8).
        PressShifted(Qt::Key_Less, "<", 0x2B, 59);
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), LIST_WIDTH - WIDTH_STEP,
                                  TIMEOUT_MS);
        PressShifted(Qt::Key_Greater, ">", 0x2F, 60);
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), LIST_WIDTH, TIMEOUT_MS);

        // Russian: Shift+б and Shift+ю sit on the same keys.
        PressShifted(0x0411, QStringLiteral("Б"), 0x2B, 59);
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), LIST_WIDTH - WIDTH_STEP,
                                  TIMEOUT_MS);
        PressShifted(0x042E, QStringLiteral("Ю"), 0x2F, 60);
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), LIST_WIDTH, TIMEOUT_MS);

        // The keys without Shift do the same: , . б ю.
        Press(Qt::Key_Comma);
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), LIST_WIDTH - WIDTH_STEP,
                                  TIMEOUT_MS);
        Press(Qt::Key_Period);
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), LIST_WIDTH, TIMEOUT_MS);
        PressNative(0x0411, QStringLiteral("б"), 0x2B, 59);
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), LIST_WIDTH - WIDTH_STEP,
                                  TIMEOUT_MS);
        PressNative(0x042E, QStringLiteral("ю"), 0x2F, 60);
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), LIST_WIDTH, TIMEOUT_MS);

        // The bounds.
        for (int i = 0; i < 30; ++i) {
            Press(Qt::Key_Comma);
        }
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), WIDTH_MIN, TIMEOUT_MS);
        Press(Qt::Key_Comma);
        QTest::qWait(50);
        QCOMPARE(ListWidth(), WIDTH_MIN);
        for (int i = 0; i < 30; ++i) {
            Press(Qt::Key_Period);
        }
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), WIDTH_MAX, TIMEOUT_MS);
        Press(Qt::Key_Period);
        QTest::qWait(50);
        QCOMPARE(ListWidth(), WIDTH_MAX);
    }

    void ModifiedLettersAreNotListKeys() {
        FailOnWarnings();
        BootWith(
            Document({{"categories", QJsonObject{{KeyOf(DELTA), "Ops"}}}}));
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Down);
        const QStringList kinds = Kinds();
        const QString cursor = CursorId();

        // The physical Control key (CONTROL_KEY), the other one, and Alt: none
        // of them is a list key, with Shift or without.
        const QList<Qt::KeyboardModifiers> held = {
            CONTROL_KEY, CONTROL_KEY | Qt::ShiftModifier, Qt::AltModifier,
            Qt::AltModifier | Qt::ShiftModifier,
            Qt::ControlModifier | Qt::MetaModifier};
        for (const Qt::KeyboardModifiers modifiers : held) {
            for (const Qt::Key key :
                 {Qt::Key_A, Qt::Key_V, Qt::Key_M, Qt::Key_P, Qt::Key_Z,
                  Qt::Key_Question, Qt::Key_Less, Qt::Key_Greater}) {
                Press(key, modifiers);
            }
        }
        // Control+Shift+/ also: the "?" of a layout with a modifier.
        QTest::qWait(50);
        QVERIFY(!HelpOpen());
        QCOMPARE(TabNow(), QString("all"));
        QCOMPARE(Kinds(), kinds);
        QCOMPARE(CursorId(), cursor);
        QCOMPARE(Title(), QString("Sessions"));
        QCOMPARE(ListWidth(), LIST_WIDTH);
        QVERIFY(
            RowItem(2)->findChild<QQuickItem*>("sessionDetail")->isVisible());
        QVERIFY(StoredDocument().value("pinned").toArray().isEmpty());
    }

    // The colour of the title of the session.
    QColor TitleColor(const QString& id) {
        return RowItem(static_cast<int>(Ids().indexOf(id)))
            ->findChild<QQuickItem*>("sessionTitle")
            ->property("color")
            .value<QColor>();
    }

    // The row of the view of the session with the id.
    int ViewRowOf(const QString& id) {
        return ViewRow(static_cast<int>(Ids().indexOf(id)));
    }

    void RowsShowStateAndUnread() {
        FailOnWarnings();
        // GAMMA finished while the window was closed: it is unread. ALPHA is
        // working as it was.
        BootWith(
            Document({{"unread", QJsonArray{KeyOf(GAMMA)}},
                      {"states", QJsonObject{{KeyOf(ALPHA), "working"},
                                             {KeyOf(GAMMA), "working"}}}}));
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(Role(ViewRowOf(GAMMA), "unread").toBool(),
                                 TIMEOUT_MS);

        QVERIFY(InRow(ViewRowOf(GAMMA), "unreadDot")->isVisible());
        QVERIFY(!Role(ViewRowOf(ALPHA), "unread").toBool());
        QVERIFY(!InRow(ViewRowOf(ALPHA), "unreadDot")->isVisible());
        // The title takes the colour of the state; a session that is done keeps
        // the text colour.
        const auto titleColor = [this](const QString& id) {
            return InRow(ViewRowOf(id), "sessionTitle")
                ->property("color")
                .value<QColor>();
        };
        QCOMPARE(titleColor(ALPHA), QColor("#d78700"));
        QCOMPARE(titleColor(BETA), QColor("#005fd7"));
        QCOMPARE(titleColor(GAMMA), QColor("#e0e0e0"));
        // A blocked session says it waits.
        QCOMPARE(InRow(ViewRowOf(BETA), "sessionDetail")
                     ->property("text")
                     .toString(),
                 QString("waiting · /work/beta"));
        QCOMPARE(InRow(ViewRowOf(ALPHA), "sessionDetail")
                     ->property("text")
                     .toString(),
                 QString("working · /work/alpha"));
    }

    void ACategoryHeaderIsInItsColourAndAFoldedOneCountsItsUnread() {
        FailOnWarnings();
        BootWith(
            Document({{"categories", QJsonObject{{KeyOf(GAMMA), "Ops"}}},
                      {"colors", QJsonObject{{"Ops", 2}}},
                      {"unread", QJsonArray{KeyOf(GAMMA)}},
                      {"states", QJsonObject{{KeyOf(ALPHA), "working"},
                                             {KeyOf(GAMMA), "working"}}}}));
        if (QTest::currentTestFailed()) {
            return;
        }
        // The header of the category is the row before its session.
        const int ops = ViewRowOf(GAMMA) - 1;
        QCOMPARE(Role(ops, "category").toString(), QString("Ops"));
        QCOMPARE(InRow(ops, "headerTitle")->property("color").value<QColor>(),
                 QColor("#008700"));
        // An unfolded header shows no count of unread.
        QVERIFY(!InRow(ops, "headerUnread")->isVisible());

        MoveTo(GAMMA);
        Press(Qt::Key_Z);

        QTRY_VERIFY_WITH_TIMEOUT(Role(ops, "folded").toBool(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(InRow(ops, "headerUnread") != nullptr &&
                                     InRow(ops, "headerUnread")->isVisible(),
                                 TIMEOUT_MS);
        QCOMPARE(InRow(ops, "headerUnread")->property("text").toString(),
                 QString("● 1"));
        // The uncategorized header has no colour of a category.
        const int none = ViewRowOf(ALPHA) - 1;
        QCOMPARE(Role(none, "category").toString(), QString());
        QCOMPARE(InRow(none, "headerTitle")->property("color").value<QColor>(),
                 QColor("#a0a0a0"));
    }

    void AListThatCannotBeWrittenShowsItsProblem() {
        FailOnWarnings();
        // A document of a newer version: the list works and is read-only.
        QVERIFY(Seed(QJsonObject{{"version", 2}}));
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        QTRY_VERIFY_WITH_TIMEOUT(Part("sessionsProblem")->isVisible(),
                                 TIMEOUT_MS);
        QVERIFY(List()->property("readOnly").toBool());
        QVERIFY(
            !Part("sessionsProblem")->property("text").toString().isEmpty());
        QCOMPARE(Part("sessionsProblem")->property("text").toString(),
                 List()->property("problem").toString());
    }

    void TheCursorStaysInView() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        // A window that shows about two rows.
        Window()->setHeight(130);
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->height() < 100, TIMEOUT_MS);

        for (int i = 0; i < 3; ++i) {
            Press(Qt::Key_J);
        }
        QCOMPARE(CursorId(), DELTA);

        // The row of the last session is inside the viewport.
        QTRY_VERIFY_WITH_TIMEOUT(
            ViewItem(4) != nullptr &&
                ViewItem(4)->y() >= ListView()->property("contentY").toReal() &&
                ViewItem(4)->y() + ViewItem(4)->height() <=
                    ListView()->property("contentY").toReal() +
                        ListView()->height(),
            TIMEOUT_MS);
        // And back to the first.
        Press(Qt::Key_K);
        Press(Qt::Key_K);
        Press(Qt::Key_K);
        QTRY_VERIFY_WITH_TIMEOUT(
            ListView()->property("contentY").toReal() <= ViewItem(1)->y(),
            TIMEOUT_MS);
    }

    void TheHelpTakesEnterAndTab() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QSignalSpy activated(PanelOf(LIST), SIGNAL(activated()));
        QSignalSpy selected(PanelOf(LIST), SIGNAL(selectedChanged()));
        PressShifted(Qt::Key_Question, "?", 0x2C, 61);
        QVERIFY(HelpOpen());

        // Every key is taken, not only passed over: the event is accepted, so
        // it does not go on to the focus chain or the shell.
        for (const Qt::Key key :
             {Qt::Key_Return, Qt::Key_Enter, Qt::Key_Tab, Qt::Key_Backtab,
              Qt::Key_J, Qt::Key_Z, Qt::Key_Left}) {
            QVERIFY2(PressAccepted(key), qPrintable(QString::number(key)));
        }
        Press(Qt::Key_Return);
        Press(Qt::Key_Enter, Qt::KeypadModifier);
        Press(Qt::Key_Tab);
        Press(Qt::Key_Backtab, Qt::ShiftModifier);
        QTest::qWait(50);

        QVERIFY(HelpOpen());
        QVERIFY(SelectedId().isEmpty());
        QCOMPARE(activated.count(), 0);
        QCOMPARE(selected.count(), 0);
        QVERIFY(ListView()->hasActiveFocus());
        QVERIFY(!Terminal()->hasActiveFocus());
        QCOMPARE(Cursor(), 0);

        // Closed, the same Enter opens the row.
        Press(Qt::Key_Escape);
        QVERIFY(!HelpOpen());
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), ALPHA, TIMEOUT_MS);
        QCOMPARE(activated.count(), 1);
    }

    void AHeldToggleKeyDoesNothing() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        const auto held = [this](Qt::Key key, const QString& text,
                                 Qt::KeyboardModifiers modifiers =
                                     Qt::NoModifier) {
            PressRepeated(key, modifiers, text);
        };

        held(Qt::Key_A, "a");
        held(Qt::Key_V, "v");
        held(Qt::Key_M, "m");
        held(Qt::Key_P, "p");
        held(Qt::Key_Z, "z");
        held(Qt::Key_Question, "?", Qt::ShiftModifier);
        QTest::qWait(100);

        QVERIFY(!HelpOpen());
        QCOMPARE(TabNow(), QString("all"));
        QCOMPARE(Title(), QString("Sessions"));
        QCOMPARE(Count(), 4);
        QVERIFY(!Role(0, "pinned").toBool());
        QVERIFY(
            RowItem(3)->findChild<QQuickItem*>("sessionDetail")->isVisible());

        // A held Shift+z unfolds nothing; a held `?` does not close the help.
        Press(Qt::Key_Z);
        QTRY_VERIFY_WITH_TIMEOUT(Role(0, "folded").toBool(), TIMEOUT_MS);
        held(Qt::Key_Z, "Z", Qt::ShiftModifier);
        QTest::qWait(100);
        QVERIFY(Role(0, "folded").toBool());
        Press(Qt::Key_Z, Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 4, TIMEOUT_MS);
        PressShifted(Qt::Key_Question, "?", 0x2C, 61);
        QVERIFY(HelpOpen());
        held(Qt::Key_Question, "?", Qt::ShiftModifier);
        held(Qt::Key_Escape, QString());
        QTest::qWait(50);
        QVERIFY(HelpOpen());
        Press(Qt::Key_Escape);
        QVERIFY(!HelpOpen());

        // `<` and `>` keep repeating: a held key resizes step by step.
        held(Qt::Key_Comma, ",");
        held(Qt::Key_Comma, ",");
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), LIST_WIDTH - 2 * WIDTH_STEP,
                                  TIMEOUT_MS);
        held(Qt::Key_Period, ".");
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), LIST_WIDTH - WIDTH_STEP,
                                  TIMEOUT_MS);
    }

    void ShiftedLettersAreNotViewKeys() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        for (const Qt::Key key : {Qt::Key_A, Qt::Key_V, Qt::Key_M, Qt::Key_P}) {
            Press(key, Qt::ShiftModifier);
        }
        QTest::qWait(100);

        QCOMPARE(TabNow(), QString("all"));
        QCOMPARE(Title(), QString("Sessions"));
        QVERIFY(!Role(0, "pinned").toBool());
        QVERIFY(
            RowItem(3)->findChild<QQuickItem*>("sessionDetail")->isVisible());
        QVERIFY(StoredDocument().value("pinned").toArray().isEmpty());
        QVERIFY(!StoredView().value("compact").toBool());
        QCOMPARE(Count(), 4);
    }

    void TheListEndsAboveTheFooterAndTheUnavailableAgents() {
        FailOnWarnings();
        // A document of a newer version fills the footer with the problem; a
        // broken claude fills the list of unavailable agents.
        QVERIFY(Seed(QJsonObject{{"version", 2}}));
        Boot(0, BROKEN_CLAUDE);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(Part("sessionsProblem")->isVisible() &&
                                     Part("sessionsUnavailable")->isVisible(),
                                 TIMEOUT_MS);
        auto* footer = Part("footer");
        auto* unavailable = Part("sessionsUnavailable");
        // The Column lays itself out on the next polish, not when a line in
        // it shows.
        QTRY_VERIFY_WITH_TIMEOUT(footer->height() > 0, TIMEOUT_MS);
        const qreal bottom = ListView()->y() + ListView()->height();

        QVERIFY(bottom <= unavailable->y());
        QVERIFY(unavailable->y() + unavailable->height() <= footer->y());
        QVERIFY(bottom <= footer->y());
    }

    void TheFooterWithoutTheUnavailableAgentsBoundsTheList() {
        FailOnWarnings();
        QVERIFY(Seed(QJsonObject{{"version", 2}}));
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(Part("sessionsProblem")->isVisible(),
                                 TIMEOUT_MS);
        QVERIFY(!Part("sessionsUnavailable")->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(Part("footer")->height() > 0, TIMEOUT_MS);
        QVERIFY(ListView()->y() + ListView()->height() <= Part("footer")->y());
    }

    void TheHeadFitsTheNarrowestPanel() {
        FailOnWarnings();
        const QString query =
            QString("a long query that cannot fit ").repeated(4);
        BootWith(Document({{"view", QJsonObject{{"query", query}}}}), 0);
        if (QTest::currentTestFailed()) {
            return;
        }
        for (int i = 0; i < 30; ++i) {
            Press(Qt::Key_Comma);
        }
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), WIDTH_MIN, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Part("sessionsQuery")->isVisible(),
                                 TIMEOUT_MS);

        // Whatever is drawn in the head stays inside the panel: the query is
        // elided and the tabs fit.
        const qreal panel = PanelOf(LIST)->width();
        QCOMPARE(panel, static_cast<qreal>(WIDTH_MIN));
        const auto right = [&](QQuickItem* item) {
            return item->mapToItem(PanelOf(LIST), QPointF(item->width(), 0))
                .x();
        };
        QVERIFY(right(Part("sessionsQuery")) <= panel);
        QVERIFY(Part("sessionsQuery")->property("truncated").toBool());
        QVERIFY(Part("sessionsHead")->clip());
        QVERIFY(right(Part("sessionsTitle")) <= panel);
        QCOMPARE(TabItems().size(), 3);
        for (QQuickItem* tab : TabItems()) {
            QVERIFY2(right(tab) <= panel,
                     qPrintable(tab->property("text").toString()));
        }
        QVERIFY(Part("sessionsHead")->property("implicitWidth").toReal() <=
                panel);
    }
};

QTEST_MAIN(SessionsViewTest)
#include "sessions_view_test.moc"
