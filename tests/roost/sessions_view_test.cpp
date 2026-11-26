// The view of the sessions panel in the shell's window: the head with the
// title and the tabs, the marks and colours of the rows, and the keys that act
// on the view (a, v, m, p, z, Z, ?, < and >), on the US and the Russian
// layouts. The window, the daemon and the fake `claude` are
// helpers/sessions_window.hpp; the panel and the model are the real ones.

#include <QColor>
#include <QDir>
#include <QFont>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExtensionPlugin>
#include <QQuickItemGrabResult>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTest>

#include <functional>

#include "helpers/sessions_window.hpp"
#include "qml_api/roost_singleton.hpp"
#include "qml_api/theme.hpp"
#include "terminal/physical_keys.hpp"

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

// The sessions of a fake `claude` that lists COUNT_MANY of them, all waiting
// (active, and unread when the document says so): the counts of the chips have
// two digits.
constexpr int COUNT_MANY = 99;
const QString MANY_CLAUDE = QStringLiteral(R"sh(#!/bin/sh
case "$1" in
agents)
    i=0
    printf '['
    while [ $i -lt COUNT ]; do
        [ $i -gt 0 ] && printf ','
        printf '{"id":"bg-%08x","cwd":"/work/s%d","kind":"background","startedAt":1790150000000,"sessionId":"00000000-0000-4000-8000-%012d","name":"Session %d","state":"blocked","pid":4242,"status":"busy"}' $i $i $i $i
        i=$((i + 1))
    done
    printf ']'
    ;;
*) exit 2 ;;
esac
)sh")
                                .replace("COUNT", QString::number(COUNT_MANY));

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
        return StoredDocument()
            .value("view")
            .toObject()
            .value("list")
            .toObject();
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

    // The font of the application, which the theme takes as its base, is a
    // state of the process: a case that changes it puts it back.
    void SetAppFont(const std::function<void(QFont&)>& change) {
        QFont font = QGuiApplication::font();
        change(font);
        QGuiApplication::setFont(font);
    }

    // The font of an item.
    static QFont FontOf(const QQuickItem* item) {
        return item->property("font").value<QFont>();
    }

    // Where the part of the row's delegate lies inside the row.
    static QRectF InsideRow(QQuickItem* row, QQuickItem* part) {
        return QRectF(part->mapToItem(row, QPointF(0, 0)),
                      QSizeF(part->width(), part->height()));
    }

    // Saves a picture of the window as <case>-<step>.png when
    // ROOST_TEST_SNAPSHOTS names a directory, for a person to look at.
    void Snapshot(const QString& step) {
        const QString directory = qEnvironmentVariable("ROOST_TEST_SNAPSHOTS");
        if (directory.isEmpty()) {
            return;
        }
        QVERIFY(QDir().mkpath(directory));
        const QString name =
            QStringLiteral("%1-%2.png")
                .arg(QString::fromLatin1(QTest::currentTestFunction()), step);
        // The content item, not the window: grabWindow distorts the terminal
        // in the offscreen platform.
        QSharedPointer<QQuickItemGrabResult> result =
            Window()->contentItem()->grabToImage();
        QVERIFY(!result.isNull());
        QSignalSpy ready(result.data(), &QQuickItemGrabResult::ready);
        QVERIFY(ready.wait(TIMEOUT_MS) || !ready.isEmpty());
        QVERIFY(result->saveToFile(QDir(directory).filePath(name)));
    }

    // The number of the first header in the list.
    int FirstHeader() { return static_cast<int>(Kinds().indexOf("header")); }

    QFont AppFont_;

private slots:
    void init() {
        // The layout is a state of the process: each test starts on a Latin
        // one and tells its own.
        roost::SetNonLatinLayout(false);
        AppFont_ = QGuiApplication::font();
        Prepare();
    }

    void cleanup() {
        Teardown();
        QGuiApplication::setFont(AppFont_);
    }

    void TheHeadShowsTitleAndTabCounts() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        QCOMPARE(Title(), QString("Sessions"));
        // Only GAMMA is done.
        QCOMPARE(Tabs(), (QStringList{"All 4", "Active 3", "Unread 0"}));
        QCOMPARE(Part("sessionsSearchInput")->property("text").toString(),
                 QString());
        QVERIFY(Part("sessionsSearchPlaceholder")->isVisible());
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

    void ALineSeparatesThePinnedCategoriesFromTheRest() {
        FailOnWarnings();
        // Ops (BETA), Dev (DELTA), then the uncategorized ALPHA and GAMMA.
        BootWith(
            Document({{"categories", QJsonObject{{KeyOf(BETA), "Ops"},
                                                 {KeyOf(DELTA), "Dev"}}}}));
        if (QTest::currentTestFailed()) {
            return;
        }
        // The header of a session's category is the row above the session.
        auto ruleShown = [&](const QString& id) {
            QQuickItem* rule = InRow(ViewRowOf(id) - 1, "pinnedRule");
            return rule != nullptr && rule->isVisible();
        };
        // Nothing is pinned: no line.
        for (const QString& id : {BETA, DELTA, ALPHA}) {
            QVERIFY(!ruleShown(id));
        }
        MoveTo(BETA);

        // One pinned, the rest not: the line is above the first unpinned.
        Press(Qt::Key_P);
        QTRY_VERIFY_WITH_TIMEOUT(ruleShown(DELTA), TIMEOUT_MS);
        QVERIFY(!ruleShown(BETA));
        QVERIFY(!ruleShown(ALPHA));
        // It takes no height: the header is as high as without it.
        QCOMPARE(InRow(ViewRowOf(DELTA) - 1, "pinnedRule")->height(), 1.0);

        // Pinning another category moves the line down.
        MoveTo(DELTA);
        Press(Qt::Key_P);
        QTRY_VERIFY_WITH_TIMEOUT(ruleShown(ALPHA), TIMEOUT_MS);
        QVERIFY(!ruleShown(BETA));
        QVERIFY(!ruleShown(DELTA));

        // The last one pinned: nothing is left below, no line.
        MoveTo(ALPHA);
        Press(Qt::Key_P);
        QTRY_VERIFY_WITH_TIMEOUT(Role(ViewRowOf(ALPHA) - 1, "pinned").toBool(),
                                 TIMEOUT_MS);
        for (const QString& id : {BETA, DELTA, ALPHA}) {
            QVERIFY(!ruleShown(id));
        }
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

        // Russian: a symbol key is the key of the US layout at its position, so
        // "?" is Shift on the US slash key, which types "," there. The layout
        // is told by a letter (Ctrl+о, which the list does not take).
        PressNative(0x041E, QStringLiteral("о"), 0x26, 44, Qt::ControlModifier);
        PressShifted(Qt::Key_Comma, ",", 0x2C, 61);
        QVERIFY(HelpOpen());
        PressShifted(Qt::Key_Comma, ",", 0x2C, 61);
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
        WAIT_FOR_EFFECTS();
        QCOMPARE(ListWidth(), WIDTH_MIN);
        for (int i = 0; i < 30; ++i) {
            Press(Qt::Key_Period);
        }
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), WIDTH_MAX, TIMEOUT_MS);
        Press(Qt::Key_Period);
        WAIT_FOR_EFFECTS();
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
        WAIT_FOR_EFFECTS();
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
        // The state does not colour the title: every row has the text colour
        // (none is open), and a session that waits has its second line in the
        // colour of attention, the others in the secondary colour.
        const auto titleColor = [this](const QString& id) {
            return InRow(ViewRowOf(id), "sessionTitle")
                ->property("color")
                .value<QColor>();
        };
        QCOMPARE(CursorId(), BETA);
        QCOMPARE(titleColor(ALPHA), QColor("#18232e"));
        QCOMPARE(titleColor(BETA), QColor("#18232e"));
        QCOMPARE(titleColor(GAMMA), QColor("#18232e"));
        const auto detailColor = [this](const QString& id) {
            return InRow(ViewRowOf(id), "sessionDetail")
                ->property("color")
                .value<QColor>();
        };
        QCOMPARE(detailColor(BETA), QColor("#8b500b"));
        QCOMPARE(detailColor(ALPHA), QColor("#5a6a7a"));
        QCOMPARE(detailColor(GAMMA), QColor("#5a6a7a"));
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
        // The name is secondary; the colour of the category is the dot.
        QCOMPARE(InRow(ops, "headerTitle")->property("color").value<QColor>(),
                 QColor("#5a6a7a"));
        QVERIFY(InRow(ops, "categoryMark")->isVisible());
        QCOMPARE(InRow(ops, "categoryMark")->property("color").value<QColor>(),
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
                 QColor("#5a6a7a"));
        QVERIFY(!InRow(none, "categoryMark")->isVisible());
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

    // The cursor row stays in the viewport of a list that holds fewer rows
    // than there are sessions. The window is sized from the measured heights
    // (the head, the search field, the row and the header depend on the font),
    // not from a number: the list gets room for a header and two rows.
    void CheckTheCursorStaysInView() {
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            ViewItem(0) != nullptr && ViewItem(1) != nullptr, TIMEOUT_MS);
        const qreal header = ViewItem(0)->height();
        const qreal row = ViewItem(1)->height();
        // What the window has besides the list does not depend on its height.
        const qreal chrome = Window()->height() - ListView()->height();
        const qreal room = header + 2 * row;
        Window()->setHeight(static_cast<int>(chrome + room));
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(ListView()->height() - room) <= 1,
                                 TIMEOUT_MS);
        // The case means something only if a row and a header fit in the list
        // and the whole content does not.
        QVERIFY(ListView()->height() >= header + row);
        QVERIFY(ListView()->height() <
                ListView()->property("contentHeight").toReal());

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

    void TheCursorStaysInView() {
        FailOnWarnings();
        CheckTheCursorStaysInView();
    }

    void TheCursorStaysInViewWithALargeInterfaceFont() {
        FailOnWarnings();
        SetAppFont(
            [](QFont& font) { font.setPointSizeF(font.pointSizeF() * 2.5); });
        CheckTheCursorStaysInView();
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
        WAIT_FOR_EFFECTS();

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
        PressWhenSettled(Qt::Key_Return);
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
        WAIT_FOR_EFFECTS();

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
        WAIT_FOR_EFFECTS();
        QVERIFY(Role(0, "folded").toBool());
        Press(Qt::Key_Z, Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 4, TIMEOUT_MS);
        PressShifted(Qt::Key_Question, "?", 0x2C, 61);
        QVERIFY(HelpOpen());
        held(Qt::Key_Question, "?", Qt::ShiftModifier);
        held(Qt::Key_Escape, QString());
        WAIT_FOR_EFFECTS();
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
        WAIT_FOR_EFFECTS();

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

    // The counts of two digits: the three chips stay inside the head of the
    // narrowest panel, with the padding of the panel at the right.
    void TheChipsFitTheNarrowestPanelWithTwoDigitCounts() {
        FailOnWarnings();
        QStringList unread;
        for (int i = 0; i < COUNT_MANY; ++i) {
            unread << KeyOf(QStringLiteral("bg-%1").arg(i, 8, 16, QChar('0')));
        }
        QVERIFY(
            Seed(Document({{"unread", QJsonArray::fromStringList(unread)}})));
        Boot(COUNT_MANY, MANY_CLAUDE.toUtf8().constData());
        if (QTest::currentTestFailed()) {
            return;
        }
        for (int i = 0; i < 30; ++i) {
            Press(Qt::Key_Comma);
        }
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), WIDTH_MIN, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            Tabs(),
            (QStringList{QStringLiteral("All %1").arg(COUNT_MANY),
                         QStringLiteral("Active %1").arg(COUNT_MANY),
                         QStringLiteral("Unread %1").arg(COUNT_MANY)}),
            TIMEOUT_MS);

        const QQuickItem* head = Part("sessionsHead");
        const qreal padding = Singleton_->theme()->panelPadding();
        qreal previousRight = 0;
        for (QQuickItem* tab : TabItems()) {
            const QRectF box = InsideRow(Part("sessionsHead"), tab);
            QVERIFY2(box.right() <= head->width() - padding,
                     qPrintable(QStringLiteral("%1: %2 > %3 - %4")
                                    .arg(tab->property("text").toString())
                                    .arg(box.right())
                                    .arg(head->width())
                                    .arg(padding)));
            // The chips follow one another, a row gap apart.
            QVERIFY(box.left() >= previousRight);
            previousRight = box.right();
        }
        Snapshot("chips-99");
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
        QTRY_VERIFY_WITH_TIMEOUT(
            !Part("sessionsSearchPlaceholder")->isVisible(), TIMEOUT_MS);

        // Whatever is drawn in the head stays inside the panel: the query is
        // clipped by its field and the tabs fit.
        const qreal panel = PanelOf(LIST)->width();
        QCOMPARE(panel, static_cast<qreal>(WIDTH_MIN));
        const auto right = [&](QQuickItem* item) {
            return item->mapToItem(PanelOf(LIST), QPointF(item->width(), 0))
                .x();
        };
        QVERIFY(right(Part("sessionsSearch")) <= panel);
        QVERIFY(right(Part("sessionsSearchInput")) <=
                right(Part("sessionsSearch")));
        QVERIFY(right(Part("sessionsNewButton")) <= panel);
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

    // The text of the panel is in the fonts of the type scale whatever the
    // base font is made of: with a base in pixels (no point size) the chip, the
    // count of a header and the title of a row have the pixel size of their
    // theme font, not the default one.
    void ThePanelKeepsTheSizeOfAThemeFontInPixels() {
        FailOnWarnings();
        SetAppFont([](QFont& font) { font.setPixelSize(17); });
        BootWith(Document({{"categories", QJsonObject{{KeyOf(BETA), "Ops"}}}}));
        if (QTest::currentTestFailed()) {
            return;
        }
        const Theme* theme = Singleton_->theme();
        QCOMPARE(theme->uiFont().pointSizeF(), -1.0);
        QVERIFY(theme->metaFont().pixelSize() > 0);
        QVERIFY(theme->rowFont().pixelSize() > 0);
        QVERIFY(theme->metaFont().pixelSize() != theme->rowFont().pixelSize());

        // The current chip (DemiBold) and one that is not (the theme's weight).
        const QList<QQuickItem*> chips = TabItems();
        QCOMPARE(chips.size(), 3);
        QVERIFY(chips[0]->property("current").toBool());
        QCOMPARE(FontOf(chips[0]).pixelSize(), theme->metaFont().pixelSize());
        QCOMPARE(FontOf(chips[0]).weight(), QFont::DemiBold);
        QCOMPARE(FontOf(chips[1]).pixelSize(), theme->metaFont().pixelSize());
        QCOMPARE(FontOf(chips[1]), theme->metaFont());

        const QQuickItem* count =
            InRow(FirstHeader(), QStringLiteral("headerCount"));
        QVERIFY(count != nullptr);
        QCOMPARE(FontOf(count).pixelSize(), theme->metaFont().pixelSize());
        QCOMPARE(FontOf(count).weight(), theme->metaFont().weight());
        QVERIFY(FontOf(count).family() == theme->metaFont().family());

        const QQuickItem* title =
            InRow(ViewRow(0), QStringLiteral("sessionTitle"));
        QVERIFY(title != nullptr);
        QCOMPARE(FontOf(title), theme->rowFont());
    }

    // A large interface font makes the rows taller: the text of each row is
    // inside it, and a row ends above the next one.
    void RowsGrowWithALargeInterfaceFont() {
        FailOnWarnings();
        SetAppFont(
            [](QFont& font) { font.setPointSizeF(font.pointSizeF() * 2.5); });
        BootWith(Document({{"categories", QJsonObject{{KeyOf(BETA), "Ops"}}}}));
        if (QTest::currentTestFailed()) {
            return;
        }
        const Theme* theme = Singleton_->theme();
        const int rows = static_cast<int>(Kinds().size());
        QTRY_VERIFY_WITH_TIMEOUT(ViewItem(rows - 1) != nullptr, TIMEOUT_MS);
        // The case means something only if two lines do not fit in a row of
        // the style.
        QQuickItem* first = RowItem(0);
        const qreal text =
            first->findChild<QQuickItem*>("sessionTitle")->implicitHeight() +
            first->findChild<QQuickItem*>("sessionDetail")->implicitHeight();
        QVERIFY2(text + 12 > theme->rowHeight(),
                 qPrintable(QString::number(text)));

        for (int row = 0; row < rows; ++row) {
            QQuickItem* item = ViewItem(row);
            QVERIFY(item != nullptr);
            if (Kinds().at(row) == "header") {
                QVERIFY(item->height() >= 34);
                QVERIFY(InsideRow(item, InRow(row, "headerTitle")).bottom() <=
                        item->height());
            } else {
                QVERIFY(item->height() > theme->rowHeight());
                for (const char* name : {"sessionTitle", "sessionDetail"}) {
                    const QRectF box = InsideRow(item, InRow(row, name));
                    QVERIFY2(box.top() >= 0 && box.bottom() <= item->height(),
                             name);
                }
            }
            if (row + 1 < rows) {
                QVERIFY(item->y() + item->height() <= ViewItem(row + 1)->y());
            }
        }
        Snapshot("large-font");
    }

    // The compact view: a row is as high as its text and the padding, the
    // title alone, or with the second line under the cursor; the title keeps
    // its place in the row when the cursor comes to it.
    void CompactRowsAreAsHighAsTheirText() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        const qreal rowHeight = Singleton_->theme()->rowHeight();
        const auto height = [&](int session) {
            return RowItem(session)->height();
        };
        const auto titleTop = [&](int session) {
            return InsideRow(RowItem(session),
                             InRow(ViewRow(session), "sessionTitle"))
                .top();
        };
        for (int session = 0; session < 4; ++session) {
            QCOMPARE(height(session), rowHeight);
        }

        Press(Qt::Key_M);
        QTRY_VERIFY_WITH_TIMEOUT(
            !InRow(ViewRow(1), "sessionDetail")->isVisible(), TIMEOUT_MS);
        const auto textHeight = [&](int session, bool detail) {
            return InRow(ViewRow(session), "sessionTitle")->implicitHeight() +
                   (detail ? InRow(ViewRow(session), "sessionDetail")
                                 ->implicitHeight()
                           : 0);
        };
        // The cursor row has both lines; the others are lower, by the second
        // line, and none is lower than its title needs.
        QTRY_COMPARE_WITH_TIMEOUT(height(0), textHeight(0, true) + 12,
                                  TIMEOUT_MS);
        for (int session = 1; session < 4; ++session) {
            QTRY_COMPARE_WITH_TIMEOUT(
                height(session), textHeight(session, false) + 12, TIMEOUT_MS);
            QVERIFY(height(session) < height(0));
            QVERIFY(height(session) < rowHeight);
        }
        const qreal top = titleTop(0);
        QCOMPARE(titleTop(1), top);
        QCOMPARE(titleTop(2), top);
        Snapshot("compact-cursor-0");

        // The cursor moves: the same for the next row, the title does not jump.
        Press(Qt::Key_Down);
        QTRY_COMPARE_WITH_TIMEOUT(height(1), textHeight(1, true) + 12,
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(height(0), textHeight(0, false) + 12,
                                  TIMEOUT_MS);
        QCOMPARE(titleTop(0), top);
        QCOMPARE(titleTop(1), top);
        QTRY_COMPARE_WITH_TIMEOUT(height(2), textHeight(2, false) + 12,
                                  TIMEOUT_MS);

        // The full view again: every row has the height of the style.
        Press(Qt::Key_M);
        for (int session = 0; session < 4; ++session) {
            QTRY_COMPARE_WITH_TIMEOUT(height(session), rowHeight, TIMEOUT_MS);
        }
    }
};

QTEST_MAIN(SessionsViewTest)
#include "sessions_view_test.moc"
