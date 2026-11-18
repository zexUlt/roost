// The prompt of the sessions panel's footer in the shell's window: the search
// (/), the category (c) and the name (e) of the session under the cursor, on
// the US and the Russian layouts. The window, the daemon and the fake `claude`
// are helpers/sessions_window.hpp; the panel and the model are the real ones.

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

// The title of ALPHA in the fixture.
const QString ALPHA_TITLE = QStringLiteral("Fix the flaky test");

// The width of the list slot before a step (src/roost/Main.qml).
constexpr int LIST_WIDTH = 320;

}  // namespace

class SessionsPromptTest : public SessionsWindow {
    Q_OBJECT

private:
    // The document `sessions` as the daemon finds it at the start, so that the
    // panel reads it with the first list and writes nothing before the case.
    bool Seed(const QJsonObject& document) {
        if (!QDir().mkpath(Dir_->filePath("config"))) {
            return false;
        }
        QJsonObject versioned = document;
        versioned.insert("version", 1);
        QSaveFile file(Dir_->filePath("config/sessions.json"));
        return file.open(QIODevice::WriteOnly) &&
               file.write(QJsonDocument(versioned).toJson()) >= 0 &&
               file.commit();
    }

    void BootWith(const QJsonObject& document, int rows = 4) {
        QVERIFY(Seed(document));
        Boot(rows);
    }

    QQuickItem* Part(const QString& name) {
        return PanelOf(LIST)->findChild<QQuickItem*>(name);
    }

    // A role of the session with the number, whatever the headers.
    QVariant SessionRole(int session, const QByteArray& name) {
        return Role(ViewRow(session), name);
    }

    QString PromptKind() {
        return PanelOf(LIST)->property("promptKind").toString();
    }

    QQuickItem* Field() { return Part("sessionsPromptInput"); }

    // What the field holds.
    QString Typed() { return Field()->property("text").toString(); }

    QString Query() { return List()->property("query").toString(); }

    QString TabNow() { return List()->property("tab").toString(); }

    QString Title() {
        return Part("sessionsTitle")->property("text").toString();
    }

    bool HelpOpen() { return PanelOf(LIST)->property("helpOpen").toBool(); }

    int ListWidth() { return static_cast<int>(Slot(LIST)->width()); }

    // Typed text, as the keys of the US layout; no native codes.
    void Type(const QByteArray& text) {
        for (const char letter : text) {
            QTest::keyClick(Window(), letter);
        }
    }

    // Moves the cursor down to the session.
    void MoveTo(const QString& id) {
        for (int step = 0; step < 8 && CursorId() != id; ++step) {
            Press(Qt::Key_Down);
        }
        QCOMPARE(CursorId(), id);
    }

    // The Cyrillic letters ф and я of the Russian layout, on the A and Z keys.
    void TypeRussianFYa() {
        PressNative(0x0424, QStringLiteral("ф"), 0x00, 38);
        PressNative(0x042F, QStringLiteral("я"), 0x06, 52);
    }

    // Empties the field, whatever it holds.
    void ClearField() {
        const auto length = Typed().size();
        for (qsizetype i = 0; i < length; ++i) {
            Press(Qt::Key_Backspace);
        }
        QCOMPARE(Typed(), QString());
    }

    QJsonObject StoredSection(const char* name) {
        return StoredDocument().value(name).toObject();
    }

    // The prompt is closed and the list has the keyboard.
    void VerifyPromptClosed() {
        QCOMPARE(PromptKind(), QString());
        QVERIFY(!Part("sessionsPrompt")->isVisible());
        QVERIFY(ListView()->hasActiveFocus());
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

    // The texts of everything below `item`, in the order of the tree.
    static void Texts(QQuickItem* item, QStringList& found) {
        const QString text = item->property("text").toString();
        if (!text.isEmpty()) {
            found << text;
        }
        for (QQuickItem* child : item->childItems()) {
            Texts(child, found);
        }
    }

private slots:
    void init() { Prepare(); }

    void cleanup() { Teardown(); }

    void SlashFiltersLiveEnterKeepsEscClears() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(!Part("sessionsPrompt")->isVisible());

        Press(Qt::Key_Slash);

        QCOMPARE(PromptKind(), QString("search"));
        QVERIFY(Part("sessionsPrompt")->isVisible());
        QVERIFY(Field()->hasActiveFocus());
        QVERIFY(!ListView()->hasActiveFocus());
        // The "/" that opened it is not in the field.
        QCOMPARE(Typed(), QString());

        // Live: the list follows every edit, before any Enter.
        Type("flaky");
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), (QStringList{ALPHA}), TIMEOUT_MS);
        QCOMPARE(Query(), QString("flaky"));
        QVERIFY(Part("sessionsQuery")->isVisible());
        QCOMPARE(Part("sessionsQuery")->property("text").toString(),
                 QString("/ flaky"));
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredSection("view").value("query").toString(), QString("flaky"),
            TIMEOUT_MS);

        // Enter keeps the query, gives the list the keyboard and opens nothing.
        Press(Qt::Key_Return);
        VerifyPromptClosed();
        QCOMPARE(Query(), QString("flaky"));
        QCOMPARE(Ids(), (QStringList{ALPHA}));
        QVERIFY(SelectedId().isEmpty());

        // Esc in the list clears it.
        Press(Qt::Key_Escape);
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), ORDER, TIMEOUT_MS);
        QCOMPARE(Query(), QString());
        QVERIFY(!Part("sessionsQuery")->isVisible());
        QVERIFY(ListView()->hasActiveFocus());
        // With no query, Esc is not taken by the list.
        QVERIFY(!PressAccepted(Qt::Key_Escape));
    }

    void SlashAgainEditsTheQueryAndEscInTheFieldClearsIt() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Slash);
        Type("flaky");
        Press(Qt::Key_Return);
        QCOMPARE(Ids(), (QStringList{ALPHA}));

        // The field starts with the query that is.
        Press(Qt::Key_Slash);
        QCOMPARE(PromptKind(), QString("search"));
        QCOMPARE(Typed(), QString("flaky"));
        Press(Qt::Key_Backspace);
        QTRY_COMPARE_WITH_TIMEOUT(Query(), QString("flak"), TIMEOUT_MS);

        // Esc in the field drops the query and closes it.
        Press(Qt::Key_Escape);
        VerifyPromptClosed();
        QCOMPARE(Query(), QString());
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), ORDER, TIMEOUT_MS);
    }

    void RussianSlashOpensSearch() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        // Shift+\ of the Russian PC layout types "/": kVK_ANSI_Backslash 0x2A,
        // KEY_BACKSLASH + 8.
        PressNative(Qt::Key_Slash, QStringLiteral("/"), 0x2A, 51,
                    Qt::ShiftModifier);

        QCOMPARE(PromptKind(), QString("search"));
        QVERIFY(Field()->hasActiveFocus());
        QCOMPARE(Typed(), QString());
    }

    void RussianPeriodDoesNotOpenSearch() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        // The "." of the Russian layout sits on the US slash key
        // (kVK_ANSI_Slash 0x2C, KEY_SLASH + 8) and is not a "/".
        PressNative(Qt::Key_Period, QStringLiteral("."), 0x2C, 61);

        VerifyPromptClosed();
    }

    void TypingInTheFieldTriggersNoListKeys() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QStringList kinds = Kinds();
        Press(Qt::Key_Slash);

        // z folds, d deletes, a changes the tab on the list; here they are
        // text, and so are the Russian letters, which stay Russian.
        Type("zda");
        TypeRussianFYa();

        QCOMPARE(Typed(), QStringLiteral("zdaфя"));
        QTRY_COMPARE_WITH_TIMEOUT(Query(), QStringLiteral("zdaфя"), TIMEOUT_MS);
        QCOMPARE(TabNow(), QString("all"));
        QCOMPARE(Title(), QString("Sessions"));
        QCOMPARE(PromptKind(), QString("search"));
        QVERIFY(Field()->hasActiveFocus());
        QVERIFY(!HelpOpen());

        // Nothing matches; what was typed goes away with the query, and the
        // list is as it was.
        Press(Qt::Key_Escape);
        VerifyPromptClosed();
        QTRY_COMPARE_WITH_TIMEOUT(Kinds(), kinds, TIMEOUT_MS);
        QCOMPARE(TabNow(), QString("all"));
    }

    void ListKeysDoNotFireWhileThePromptHasFocus() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QStringList kinds = Kinds();
        QCOMPARE(Cursor(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(ListWidth(), LIST_WIDTH, TIMEOUT_MS);
        // The slot changes its width on the next polish, so the signal is the
        // check, the width only a second one.
        QSignalSpy widthSteps(PanelOf(LIST), SIGNAL(widthStepRequested(int)));

        // The category prompt, so that the typed text is not a query that
        // hides the rows.
        Press(Qt::Key_C);
        QCOMPARE(PromptKind(), QString("category"));

        // j and k move, a changes the tab, v shows the archive, z folds, ?
        // opens the help and < narrows the list: on the list. Here they are
        // text.
        Type("jkavz");
        PressNative(Qt::Key_Question, QStringLiteral("?"), 0x2C, 61,
                    Qt::ShiftModifier);
        PressNative(Qt::Key_Less, QStringLiteral("<"), 0x2B, 59,
                    Qt::ShiftModifier);
        TypeRussianFYa();
        // The Cyrillic letters j and k stand for in the Russian layout: о and
        // л.
        PressNative(0x041E, QStringLiteral("о"), 0x26, 44);
        PressNative(0x041B, QStringLiteral("л"), 0x28, 45);
        const QString expected = QStringLiteral("jkavz?<фяол");
        QCOMPARE(Typed(), expected);

        QCOMPARE(Cursor(), 0);
        QCOMPARE(TabNow(), QString("all"));
        QCOMPARE(Title(), QString("Sessions"));
        QVERIFY(!HelpOpen());
        QCOMPARE(Kinds(), kinds);
        QCOMPARE(widthSteps.count(), 0);
        QTest::qWait(100);
        QCOMPARE(widthSteps.count(), 0);
        QCOMPARE(ListWidth(), LIST_WIDTH);
        QCOMPARE(PromptKind(), QString("category"));
        QVERIFY(Field()->hasActiveFocus());

        // Enter on the field is the prompt's: it opens nothing on the list.
        QSignalSpy activated(PanelOf(LIST), SIGNAL(activated()));
        QSignalSpy selected(PanelOf(LIST), SIGNAL(selectedChanged()));
        Press(Qt::Key_Return);
        VerifyPromptClosed();
        QCOMPARE(activated.count(), 0);
        QCOMPARE(selected.count(), 0);
        QVERIFY(SelectedId().isEmpty());

        // The category is the exact text, Cyrillic kept.
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredSection("categories").value(KeyOf(ALPHA)).toString(),
            expected, TIMEOUT_MS);
    }

    void CSetsAndClearsACategory() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(Role(0, "category").toString(), QString());

        Press(Qt::Key_C);
        QCOMPARE(PromptKind(), QString("category"));
        QVERIFY(Field()->hasActiveFocus());
        QCOMPARE(Typed(), QString());
        Type("Ops");
        Press(Qt::Key_Return);

        VerifyPromptClosed();
        // ALPHA is under a header "Ops" now, the others are uncategorized.
        QTRY_COMPARE_WITH_TIMEOUT(Role(0, "kind").toString(), QString("header"),
                                  TIMEOUT_MS);
        QCOMPARE(Role(0, "category").toString(), QString("Ops"));
        QCOMPARE(Role(1, "id").toString(), ALPHA);
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredSection("categories").value(KeyOf(ALPHA)).toString(),
            QString("Ops"), TIMEOUT_MS);

        // The field starts with the category; emptied, Enter clears it.
        QTRY_COMPARE_WITH_TIMEOUT(CursorId(), ALPHA, TIMEOUT_MS);
        Press(Qt::Key_C);
        QCOMPARE(Typed(), QString("Ops"));
        ClearField();
        Press(Qt::Key_Return);

        VerifyPromptClosed();
        QTRY_COMPARE_WITH_TIMEOUT(Kinds().count("header"), 1, TIMEOUT_MS);
        QCOMPARE(Role(0, "category").toString(), QString());
        QTRY_VERIFY_WITH_TIMEOUT(
            !StoredSection("categories").contains(KeyOf(ALPHA)), TIMEOUT_MS);
    }

    void CPicksAKnownCategoryByNumber() {
        FailOnWarnings();
        // Known: Dev, Ops, in that order.
        BootWith(
            QJsonObject{{"categories", QJsonObject{{KeyOf(BETA), "Ops"},
                                                   {KeyOf(DELTA), "Dev"}}}});
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(List()->property("knownCategories").toStringList(),
                 (QStringList{"Dev", "Ops"}));
        // ALPHA is the first uncategorized one.
        MoveTo(ALPHA);

        Press(Qt::Key_C);
        QCOMPARE(PromptKind(), QString("category"));
        QVERIFY(Part("sessionsPromptHint")->isVisible());
        QCOMPARE(Part("sessionsPromptHint")->property("text").toString(),
                 QString("1 Dev   2 Ops"));
        Press(Qt::Key_2);

        VerifyPromptClosed();
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredSection("categories").value(KeyOf(ALPHA)).toString(),
            QString("Ops"), TIMEOUT_MS);
        QCOMPARE(Query(), QString());
    }

    void ADigitInsideANameIsText() {
        FailOnWarnings();
        // Two categories are known: 2 picks one, but not after a letter.
        BootWith(
            QJsonObject{{"categories", QJsonObject{{KeyOf(BETA), "Ops"},
                                                   {KeyOf(DELTA), "Dev"}}}});
        if (QTest::currentTestFailed()) {
            return;
        }
        MoveTo(ALPHA);

        // A digit after a letter is text, so is one that picks nothing.
        Press(Qt::Key_C);
        Type("v2");
        QCOMPARE(Typed(), QString("v2"));
        Press(Qt::Key_Return);
        VerifyPromptClosed();
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredSection("categories").value(KeyOf(ALPHA)).toString(),
            QString("v2"), TIMEOUT_MS);

        // Three categories are known now (Dev, Ops, v2), so 4 is a digit, not
        // a pick.
        QTRY_COMPARE_WITH_TIMEOUT(CursorId(), ALPHA, TIMEOUT_MS);
        Press(Qt::Key_C);
        ClearField();
        Press(Qt::Key_4);
        QCOMPARE(Typed(), QString("4"));
        Press(Qt::Key_Escape);
        VerifyPromptClosed();
    }

    void ERenamesAndEmptyRestores() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(SessionRole(0, "title").toString(), ALPHA_TITLE);

        Press(Qt::Key_E);
        QCOMPARE(PromptKind(), QString("rename"));
        QVERIFY(Field()->hasActiveFocus());
        QCOMPARE(Typed(), ALPHA_TITLE);
        ClearField();
        Type("Renamed");
        Press(Qt::Key_Return);

        VerifyPromptClosed();
        QTRY_COMPARE_WITH_TIMEOUT(SessionRole(0, "title").toString(),
                                  QString("Renamed"), TIMEOUT_MS);
        QCOMPARE(SessionRole(0, "agentTitle").toString(), ALPHA_TITLE);
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredSection("names").value(KeyOf(ALPHA)).toString(),
            QString("Renamed"), TIMEOUT_MS);

        // The field starts with the name; an empty one restores the agent's.
        Press(Qt::Key_E);
        QCOMPARE(Typed(), QString("Renamed"));
        ClearField();
        Press(Qt::Key_Return);

        VerifyPromptClosed();
        QTRY_COMPARE_WITH_TIMEOUT(SessionRole(0, "title").toString(),
                                  ALPHA_TITLE, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!StoredSection("names").contains(KeyOf(ALPHA)),
                                 TIMEOUT_MS);
    }

    void CAndEOnAHeaderDoNothing() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        // z folds the uncategorized: the cursor is on its header.
        Press(Qt::Key_Z);
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), ON_HEADER, TIMEOUT_MS);

        Press(Qt::Key_C);
        VerifyPromptClosed();
        Press(Qt::Key_E);
        VerifyPromptClosed();

        // The keys were taken, not left to a field: the list is as it was and
        // a list key still works.
        QCOMPARE(Kinds(), (QStringList{"header"}));
        Press(Qt::Key_Z);
        QTRY_VERIFY_WITH_TIMEOUT(Kinds().size() > 1, TIMEOUT_MS);
        QVERIFY(StoredSection("categories").isEmpty());
        QVERIFY(StoredSection("names").isEmpty());
    }

    void EscCancelsWithoutChange() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QStringList kinds = Kinds();

        Press(Qt::Key_C);
        Type("Ops");
        Press(Qt::Key_Escape);
        VerifyPromptClosed();
        QCOMPARE(Kinds(), kinds);

        Press(Qt::Key_E);
        ClearField();
        Type("Other");
        Press(Qt::Key_Escape);
        VerifyPromptClosed();
        QCOMPARE(SessionRole(0, "title").toString(), ALPHA_TITLE);

        // Nothing was written for either.
        WaitForThePanelToStopWriting();
        QVERIFY(StoredSection("categories").isEmpty());
        QVERIFY(StoredSection("names").isEmpty());
        // The keyboard is back on the list: j moves.
        Press(Qt::Key_J);
        QCOMPARE(Cursor(), 1);
    }

    void ThePromptKeepsItsTargetAcrossAPoll() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_C);
        QCOMPARE(CursorId(), ALPHA);
        Type("Ops");

        // A poll takes the session under the cursor away while the user types:
        // the cursor moves to another one, the target of the prompt does not.
        QVERIFY(ListWithout({ALPHA}));
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 3, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(CursorId() != ALPHA, TIMEOUT_MS);
        const QString under = CursorId();
        QCOMPARE(PromptKind(), QString("category"));

        Press(Qt::Key_Return);

        VerifyPromptClosed();
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredSection("categories").value(KeyOf(ALPHA)).toString(),
            QString("Ops"), TIMEOUT_MS);
        // What the cursor stands on now got nothing.
        QVERIFY(!StoredSection("categories").contains(KeyOf(under)));
        QCOMPARE(Role(0, "category").toString(), QString());
    }

    void ModifiedAndShiftedKeysDoNotOpenThePrompt() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        // Shift+c and Shift+e are other letters; Ctrl, Alt and a held key are
        // not list keys.
        Press(Qt::Key_C, Qt::ShiftModifier);
        Press(Qt::Key_E, Qt::ShiftModifier);
        Press(Qt::Key_C, CONTROL_KEY);
        Press(Qt::Key_E, Qt::AltModifier);
        Press(Qt::Key_Slash, CONTROL_KEY);
        Press(Qt::Key_Slash, Qt::AltModifier);
        PressRepeated(Qt::Key_C, Qt::NoModifier, QStringLiteral("c"));
        PressRepeated(Qt::Key_E, Qt::NoModifier, QStringLiteral("e"));
        PressRepeated(Qt::Key_Slash, Qt::NoModifier, QStringLiteral("/"));

        VerifyPromptClosed();
    }

    void TheHelpTakesSlashAndListsThePromptKeys() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        PressNative(Qt::Key_Question, QStringLiteral("?"), 0x2C, 61,
                    Qt::ShiftModifier);
        QVERIFY(HelpOpen());

        // The help takes every key: no prompt behind it.
        Press(Qt::Key_Slash);
        Press(Qt::Key_C);
        Press(Qt::Key_E);
        QCOMPARE(PromptKind(), QString());
        QVERIFY(HelpOpen());

        QStringList texts;
        Texts(Part("sessionsHelp"), texts);
        const QString help = texts.join('\n');
        QVERIFY2(help.contains(QRegularExpression("(^|\n)/ {3}search")),
                 qPrintable(help));
        QVERIFY2(help.contains(QRegularExpression("\nc {3}category")),
                 qPrintable(help));
        QVERIFY2(help.contains(QRegularExpression("\ne {3}rename")),
                 qPrintable(help));
    }

    void TabInTheFieldDoesNothing() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_C);
        Type("Ops");

        QVERIFY(PressAccepted(Qt::Key_Tab));
        QVERIFY(PressAccepted(Qt::Key_Backtab, Qt::ShiftModifier));
        QTest::qWait(100);

        QCOMPARE(PromptKind(), QString("category"));
        QVERIFY(Field()->hasActiveFocus());
        QCOMPARE(Typed(), QString("Ops"));

        Press(Qt::Key_Return);
        VerifyPromptClosed();
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredSection("categories").value(KeyOf(ALPHA)).toString(),
            QString("Ops"), TIMEOUT_MS);
    }

    void CAndEOpenOnTheRussianLayout() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        // с and у sit on the C and E keys: mac 0x08 / xkb 54 and 0x0E / 26.
        PressNative(0x0421, QStringLiteral("с"), 0x08, 54);
        QCOMPARE(PromptKind(), QString("category"));
        QVERIFY(Field()->hasActiveFocus());
        Press(Qt::Key_Escape);
        VerifyPromptClosed();

        PressNative(0x0423, QStringLiteral("у"), 0x0E, 26);
        QCOMPARE(PromptKind(), QString("rename"));
        QVERIFY(Field()->hasActiveFocus());
        QCOMPARE(Typed(), ALPHA_TITLE);
    }

    void ACyrillicNameStaysCyrillic() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        PressNative(0x0423, QStringLiteral("у"), 0x0E, 26);
        QCOMPARE(PromptKind(), QString("rename"));
        ClearField();
        TypeRussianFYa();
        Press(Qt::Key_Return);

        VerifyPromptClosed();
        const QString name = QStringLiteral("фя");
        QTRY_COMPARE_WITH_TIMEOUT(SessionRole(0, "title").toString(), name,
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredSection("names").value(KeyOf(ALPHA)).toString(), name,
            TIMEOUT_MS);
    }

    void ATapOnTheListEndsThePrompt() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        // A tap on a tab takes the keyboard to the list and opens nothing.
        const auto tapTab = [&](int index) {
            QList<QQuickItem*> tabs;
            Collect(Part("sessionsHead"), "sessionsTab", tabs);
            QVERIFY(tabs.size() > index);
            QQuickItem* tab = tabs[index];
            QTest::mouseClick(
                Window(), Qt::LeftButton, Qt::NoModifier,
                tab->mapToScene(QPointF(tab->width() / 2, tab->height() / 2))
                    .toPoint());
        };

        // A name that is half typed is dropped.
        Press(Qt::Key_E);
        ClearField();
        Type("Half");
        tapTab(1);
        QTRY_VERIFY_WITH_TIMEOUT(PromptKind().isEmpty(), TIMEOUT_MS);
        QVERIFY(ListView()->hasActiveFocus());
        QCOMPARE(SessionRole(0, "title").toString(), ALPHA_TITLE);
        WaitForThePanelToStopWriting();
        QVERIFY(StoredSection("names").isEmpty());

        // A search keeps its query.
        Press(Qt::Key_Slash);
        Type("flaky");
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), (QStringList{ALPHA}), TIMEOUT_MS);
        tapTab(0);
        QTRY_VERIFY_WITH_TIMEOUT(PromptKind().isEmpty(), TIMEOUT_MS);
        QVERIFY(ListView()->hasActiveFocus());
        QCOMPARE(Query(), QString("flaky"));
        QCOMPARE(Ids(), (QStringList{ALPHA}));
    }
};

QTEST_MAIN(SessionsPromptTest)
#include "sessions_prompt_test.moc"
