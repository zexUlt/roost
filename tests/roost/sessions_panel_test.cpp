// The built-in plugin `sessions`: its panel sessions.list in the shell's window
// (Main.qml of module Roost.Shell), against the real roostd with the real
// Claude plugin behind a fake `claude` whose `agents` prints a file and whose
// `attach` runs fake_tui, and with all the built-in plugins. What the user
// sees is read from the panels: the list's cursor, its rows, the status text,
// and the status and screen of the terminal; keys go through the window, to
// whatever has the focus. The window is helpers/sessions_window.hpp.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQmlExtensionPlugin>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>

#include "helpers/scoped_env.hpp"
#include "helpers/sessions_window.hpp"
#include "qml_api/sessions_model.hpp"
#include "terminal/session_pane.hpp"

using namespace roost;
using namespace roost::testing;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

class SessionsPanelTest : public SessionsWindow {
    Q_OBJECT

private slots:
    void init() { Prepare(); }

    void cleanup() { Teardown(); }

    void TheListPanelIsLoadedFromTheBuiltInPluginAndItsOutputIsSelected() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        QCOMPARE(PanelOf(LIST)->parentItem(), Slot(LIST));
        QCOMPARE(Slot(LIST)->property("linkOutput").toString(),
                 QString("selected"));
        QCOMPARE(Slot(TERMINAL)->property("linkInput").toString(),
                 QString("session"));
        QVERIFY(Problems().isEmpty());
        QVERIFY(ListView() != nullptr);
    }

    void TheRowsShowTheTitleAndTheStateInTheOrderOfRoostd() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        QCOMPARE(Ids(), ORDER);
        // The title is the name, or the id of a session without one.
        const QStringList titles = {"Fix the flaky test", "Review the PR",
                                    GAMMA, "Answer a question"};
        for (int row = 0; row < titles.size(); ++row) {
            QCOMPARE(RowText(row, "sessionTitle"), titles[row]);
        }
        QCOMPARE(RowText(0, "sessionDetail"), QString("working · /work/alpha"));
        QCOMPARE(RowText(1, "sessionDetail"), QString("waiting · /work/beta"));
        QCOMPARE(RowText(2, "sessionDetail"), QString("done · /work/gamma"));
        QVERIFY(ListStatusNow().isEmpty());
        QVERIFY(!ListStatus()->isVisible());
    }

    void SessionLabelsAndMessagesUsePlainText() {
        FailOnWarnings();
        QVERIFY(WriteFakeClaude(FAKE_CLAUDE));
        const QString title = "<b>x</b>";
        const QString cwd = "/work/<b>alpha</b>";
        const QJsonArray rows{QJsonObject{{"id", ALPHA},
                                          {"name", title},
                                          {"cwd", cwd},
                                          {"kind", "background"},
                                          {"state", "working"},
                                          {"status", "busy"}}};
        QSaveFile agents(AgentsPath());
        QVERIFY(agents.open(QIODevice::WriteOnly));
        QVERIFY(agents.write(QJsonDocument(rows).toJson()) > 0);
        QVERIFY(agents.commit());
        QVERIFY(StartDaemon());
        OpenWindow();
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 1, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(RowItem(0) != nullptr, TIMEOUT_MS);

        QCOMPARE(RowText(0, "sessionTitle"), title);
        QCOMPARE(RowText(0, "sessionDetail"), QString("working · ") + cwd);
        for (const char* name : {"sessionTitle", "sessionDetail"}) {
            const QObject* label = RowItem(0)->findChild<QObject*>(name);
            QVERIFY(label != nullptr);
            QCOMPARE(label->property("textFormat").toInt(),
                     static_cast<int>(Qt::PlainText));
        }
        for (const char* name :
             {"sessionsUnavailable", "sessionsConfirm", "sessionsNotice"}) {
            const QObject* label = PanelOf(LIST)->findChild<QObject*>(name);
            QVERIFY(label != nullptr);
            QCOMPARE(label->property("textFormat").toInt(),
                     static_cast<int>(Qt::PlainText));
        }
    }

    // The image loads for real: the svg of the module's resources needs the
    // image format plugin of Qt Svg, and a row that shows no icon has none.
    void ASessionRowShowsTheIconOfItsAgent() {
        // Not the icons of the real user's folder.
        const ScopedEnv config("ROOST_CONFIG",
                               Dir_->filePath("config").toUtf8());
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        for (int row = 0; row < ORDER.size(); ++row) {
            QQuickItem* item = RowItem(row);
            QVERIFY(item != nullptr);
            QQuickItem* icon = item->findChild<QQuickItem*>("agentIcon");
            QVERIFY2(icon != nullptr, qPrintable(QString::number(row)));
            QVERIFY(icon->isVisible());
            QVERIFY2(icon->property("source").toUrl().toString().endsWith(
                         "/agent-icons/claude.svg"),
                     qPrintable(icon->property("source").toString()));
            // Image.Ready.
            QTRY_COMPARE(icon->property("status").toInt(), 1);
            QCOMPARE(icon->width(), 16.0);
            QCOMPARE(icon->height(), 16.0);
            // The icon does not overlap the title: its right edge, in the
            // coordinates of the row, is not right of the left edge of the
            // text of the title.
            QQuickItem* title = item->findChild<QQuickItem*>("sessionTitle");
            QVERIFY(title != nullptr);
            const qreal iconRight =
                item->mapFromItem(icon, QPointF(icon->width(), 0)).x();
            const qreal titleLeft = item->mapFromItem(title, QPointF(0, 0)).x();
            QVERIFY2(iconRight <= titleLeft,
                     qPrintable(QStringLiteral("%1 > %2").arg(iconRight).arg(
                         titleLeft)));
        }
    }

    // A file of the user that exists but does not load leaves no gap: the row
    // falls back to the bundled icon of its agent.
    void ABrokenUserIconFallsBackToTheBundledOne() {
        const ScopedEnv config("ROOST_CONFIG",
                               Dir_->filePath("config").toUtf8());
        QVERIFY(QDir().mkpath(Dir_->filePath("config/agent-icons")));
        QFile broken(Dir_->filePath("config/agent-icons/claude.svg"));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        QVERIFY(broken.write("this is not an svg") > 0);
        broken.close();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        for (int row = 0; row < ORDER.size(); ++row) {
            QQuickItem* item = RowItem(row);
            QVERIFY(item != nullptr);
            QQuickItem* icon = item->findChild<QQuickItem*>("agentIcon");
            QVERIFY(icon != nullptr);
            // Image.Ready, from the bundled source.
            QTRY_COMPARE(icon->property("status").toInt(), 1);
            QVERIFY2(icon->property("source").toString().startsWith(
                         "qrc:/qt/qml/RoostSessions/agent-icons/claude.svg"),
                     qPrintable(icon->property("source").toString()));
        }
    }

    void TheKeysMoveTheCursorAndChangeNeitherTheSelectionNorTheTerminal() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(ListView()->hasActiveFocus());

        Press(Qt::Key_Down);
        QCOMPARE(Cursor(), 1);
        Press(Qt::Key_J);
        QCOMPARE(Cursor(), 2);
        Press(Qt::Key_Up);
        QCOMPARE(Cursor(), 1);
        Press(Qt::Key_K);
        QCOMPARE(Cursor(), 0);
        // The top and the bottom do not wrap.
        Press(Qt::Key_Up);
        QCOMPARE(Cursor(), 0);
        Press(Qt::Key_K);
        QCOMPARE(Cursor(), 0);
        for (int i = 0; i < 6; ++i) {
            Press(Qt::Key_Down);
        }
        QCOMPARE(Cursor(), 3);
        Press(Qt::Key_J);
        QCOMPARE(Cursor(), 3);
        // j with a modifier is not j.
        Press(Qt::Key_K, Qt::ControlModifier);
        Press(Qt::Key_K, Qt::ShiftModifier);
        QCOMPARE(Cursor(), 3);
        Press(Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(Cursor(), 3);
        Press(Qt::Key_Up);
        Press(Qt::Key_J, Qt::AltModifier);
        QCOMPARE(Cursor(), 2);

        // Moving opens nothing.
        QVERIFY(SelectedId().isEmpty());
        QCOMPARE(Status(), QString("none"));
        QCOMPARE(StatusText()->property("text").toString(), EMPTY_TEXT);
    }

    void JAndKMoveTheCursorOnTheRussianLayout() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(ListView()->hasActiveFocus());

        // Cyrillic "о" and "к" sit on the J and K keys: kVK_ANSI_J 0x26 and
        // KEY_J + 8, kVK_ANSI_K 0x28 and KEY_K + 8.
        PressNative(0x041E, QStringLiteral("о"), 0x26, 44);
        QCOMPARE(Cursor(), 1);
        PressNative(0x041E, QStringLiteral("о"), 0x26, 44);
        QCOMPARE(Cursor(), 2);
        PressNative(0x041A, QStringLiteral("к"), 0x28, 45);
        QCOMPARE(Cursor(), 1);
        // A Cyrillic letter that is not on a key of the list moves nothing: ч
        // sits on X (kVK_ANSI_X 0x07, KEY_X + 8).
        PressNative(0x0427, QStringLiteral("ч"), 0x07, 53);
        QCOMPARE(Cursor(), 1);
        QVERIFY(SelectedId().isEmpty());
    }

    void ReturnOpensTheSessionUnderTheCursorInTheTerminal() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        Press(Qt::Key_Down);
        Press(Qt::Key_Return);

        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), BETA, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(BETA)),
                                 TIMEOUT_MS);
        BackToList();

        // Enter of the keypad does the same, for the row above.
        Press(Qt::Key_Up);
        Press(Qt::Key_Enter, Qt::KeypadModifier);

        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), ALPHA, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(ALPHA)),
                                 TIMEOUT_MS);
        QVERIFY(!ScreenNow().contains(Greeting(BETA)));
        QCOMPARE(Status(), QString("open"));
    }

    void ATapOnARowOpensItLikeReturn() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(Cursor(), 0);

        TapRow(2);

        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), GAMMA, TIMEOUT_MS);
        QCOMPARE(Cursor(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(GAMMA)),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasActiveFocus(), TIMEOUT_MS);
    }

    void ReturnOnAnEmptyListKeepsTheKeyboardInTheList() {
        FailOnWarnings();
        Boot(0, FAKE_CLAUDE, true);
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(ListView()->hasActiveFocus());

        Press(Qt::Key_Return);
        Press(Qt::Key_Enter, Qt::KeypadModifier);
        QTest::qWait(200);

        QVERIFY(ListView()->hasActiveFocus());
        QVERIFY(!Terminal()->hasActiveFocus());
    }

    void AHeldReturnNeitherOpensNorMovesTheKeyboard() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Watch();

        PressRepeated(Qt::Key_Return);
        QTest::qWait(300);

        QVERIFY(SelectedId().isEmpty());
        QVERIFY2(!Opened(), qPrintable(Changes_.join(", ")));
        QVERIFY(ListView()->hasActiveFocus());
        QVERIFY(!Terminal()->hasActiveFocus());
        // The same key pressed once opens the row.
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), ALPHA, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasActiveFocus(), TIMEOUT_MS);
    }

    void WithoutTheTerminalPanelReturnKeepsTheKeyboardInTheList() {
        FailOnWarnings();
        WithoutTerminal_ = true;
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(PanelOf(TERMINAL) == nullptr);

        Press(Qt::Key_Return);

        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), ALPHA, TIMEOUT_MS);
        QTest::qWait(200);
        QVERIFY(ListView()->hasActiveFocus());
    }

    void ReturnOnTheOpenRowNeitherReopensNorRewritesTheSelection() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        BackToList();
        Watch();
        QSignalSpy changed(PanelOf(LIST), SIGNAL(selectedChanged()));

        Press(Qt::Key_Return);
        Press(Qt::Key_Enter, Qt::KeypadModifier);
        QTest::qWait(300);

        QCOMPARE(changed.count(), 0);
        QVERIFY2(!Opened(), qPrintable(Changes_.join(", ")));
        QCOMPARE(SelectedId(), BETA);
        QCOMPARE(Status(), QString("open"));
    }

    void ARemovedOpenSessionIsClearedAndTheCursorGoesToTheNextRow() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }

        QVERIFY(ListWithout({BETA}));

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 3, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(SelectedId().isEmpty(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("none"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(StatusText()->isVisible(), TIMEOUT_MS);
        QCOMPARE(StatusText()->property("text").toString(), EMPTY_TEXT);
        QCOMPARE(Ids(), (QStringList{ALPHA, GAMMA, DELTA}));
        QCOMPARE(Cursor(), 1);
        QCOMPARE(CursorId(), GAMMA);
    }

    void ARemovedOtherSessionKeepsTheSelectionAndTheCursorOnItsSession() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        Watch();

        QVERIFY(ListWithout({ALPHA}));

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 3, TIMEOUT_MS);
        // The cursor was on BETA, now the first row.
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), 0, TIMEOUT_MS);
        QCOMPARE(CursorId(), BETA);
        QCOMPARE(SelectedId(), BETA);
        // The terminal is not touched for some polls more.
        QTest::qWait(500);
        QCOMPARE(SelectedId(), BETA);
        QCOMPARE(Status(), QString("open"));
        QVERIFY2(!Opened(), qPrintable(Changes_.join(", ")));
        QVERIFY(ScreenNow().contains(Greeting(BETA)));
    }

    void NoSessionsLeftSaysSoAndReturnDoesNothing() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        BackToList();

        QVERIFY(ListWithout(ORDER));

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 0, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(ListStatusNow(), QString("No sessions."),
                                  TIMEOUT_MS);
        QVERIFY(ListStatus()->isVisible());
        QCOMPARE(Cursor(), NO_CURSOR);
        QTRY_VERIFY_WITH_TIMEOUT(SelectedId().isEmpty(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("none"), TIMEOUT_MS);

        // A TypeError would be a warning, which fails the test.
        Press(Qt::Key_Return);
        Press(Qt::Key_Enter, Qt::KeypadModifier);
        Press(Qt::Key_Down);
        Press(Qt::Key_J);
        QTest::qWait(100);
        QCOMPARE(Cursor(), NO_CURSOR);
        // `selected` is still null, not a ref of nothing.
        QVERIFY(PanelOf(LIST)->property("selected").isNull());
    }

    void ARefreshKeepsTheCursorAndTheOpenSessionAndOpensNothingAgain() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        BackToList();
        Press(Qt::Key_Down);
        Press(Qt::Key_Down);
        QCOMPARE(CursorId(), DELTA);
        Watch();

        // As a plugin asks: through the singleton, from the panel's context.
        QSignalSpy reset(Singleton_->sessions(),
                         &QAbstractItemModel::modelReset);
        QQmlExpression refresh(QQmlEngine::contextForObject(PanelOf(LIST)),
                               PanelOf(LIST), "Roost.sessions.refresh()");
        refresh.evaluate();
        QVERIFY2(!refresh.hasError(), qPrintable(refresh.error().toString()));
        QTRY_COMPARE_WITH_TIMEOUT(reset.count(), 1, TIMEOUT_MS);

        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), 3, TIMEOUT_MS);
        QCOMPARE(CursorId(), DELTA);
        QCOMPARE(SelectedId(), BETA);
        QCOMPARE(Status(), QString("open"));
        QVERIFY2(!Opened(), qPrintable(Changes_.join(", ")));
    }

    void ARestartedRoostdKeepsTheSelectionAndTheTerminalReopensTheSession() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        BackToList();
        Press(Qt::Key_Down);
        QCOMPARE(CursorId(), GAMMA);

        StopDaemon();

        QTRY_COMPARE_WITH_TIMEOUT(Root()->property("connection").toString(),
                                  QString("disconnected"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("disconnected"),
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(ListStatusNow(), QString("Loading sessions…"),
                                  TIMEOUT_MS);
        // Give a reconcile that is wrong the time to clear it.
        QTest::qWait(300);
        QCOMPARE(SelectedId(), BETA);

        QVERIFY(StartDaemon());

        QTRY_COMPARE_WITH_TIMEOUT(Root()->property("connection").toString(),
                                  QString("connected"), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 4, TIMEOUT_MS);
        QCOMPARE(SelectedId(), BETA);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(BETA)),
                                 TIMEOUT_MS);
        // The cursor is where the user left it.
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), 2, TIMEOUT_MS);
    }

    void ARestartedRoostdThatLacksTheOpenSessionClearsTheSelection() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }

        StopDaemon();
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("disconnected"),
                                  TIMEOUT_MS);
        QVERIFY(ListWithout({BETA}));
        QVERIFY(StartDaemon());

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 3, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(SelectedId().isEmpty(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("none"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(StatusText()->property("text").toString(),
                                  EMPTY_TEXT, TIMEOUT_MS);
        QCOMPARE(Cursor(), 0);
    }

    void ARestartedRoostdThatLacksTheCursorsSessionPutsTheCursorOnTheOpenRow() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        BackToList();
        Press(Qt::Key_Down);
        QCOMPARE(CursorId(), GAMMA);

        StopDaemon();
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("disconnected"),
                                  TIMEOUT_MS);
        QVERIFY(ListWithout({GAMMA}));
        QVERIFY(StartDaemon());

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 3, 30000);
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), 1, TIMEOUT_MS);
        QCOMPARE(CursorId(), BETA);
        QCOMPARE(SelectedId(), BETA);
    }

    void AReturnBetweenTheResetAndTheReconcileOpensNothing() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Down);
        QCOMPARE(CursorId(), BETA);
        // Connected after the panel's own, so it runs once the panel has seen
        // the reset and before the panel's reconcile, which waits for the
        // event loop.
        int pressed = 0;
        connect(Singleton_->sessions(), &QAbstractItemModel::modelReset, this,
                [this, &pressed] {
                    ++pressed;
                    Press(Qt::Key_Return);
                });

        QQmlExpression refresh(QQmlEngine::contextForObject(PanelOf(LIST)),
                               PanelOf(LIST), "Roost.sessions.refresh()");
        refresh.evaluate();
        QVERIFY2(!refresh.hasError(), qPrintable(refresh.error().toString()));
        QTRY_COMPARE_WITH_TIMEOUT(pressed, 1, TIMEOUT_MS);
        QTest::qWait(300);

        QVERIFY(SelectedId().isEmpty());
        QCOMPARE(Status(), QString("none"));
        // The cursor is where the user left it, and Return works again.
        QCOMPARE(CursorId(), BETA);
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), BETA, TIMEOUT_MS);
    }

    void ACursorMovedByARemovalIsRememberedAcrossAReset() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Down);
        Press(Qt::Key_Down);
        QCOMPARE(CursorId(), GAMMA);

        QVERIFY(ListWithout({GAMMA}));
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 3, TIMEOUT_MS);
        QCOMPARE(Cursor(), 2);
        QCOMPARE(CursorId(), DELTA);

        // The panel remembers the session the cursor is on now.
        QSignalSpy reset(Singleton_->sessions(),
                         &QAbstractItemModel::modelReset);
        QQmlExpression refresh(QQmlEngine::contextForObject(PanelOf(LIST)),
                               PanelOf(LIST), "Roost.sessions.refresh()");
        refresh.evaluate();
        QVERIFY2(!refresh.hasError(), qPrintable(refresh.error().toString()));
        QTRY_COMPARE_WITH_TIMEOUT(reset.count(), 1, TIMEOUT_MS);
        QTest::qWait(100);

        QCOMPARE(Cursor(), 2);
        QCOMPARE(CursorId(), DELTA);
    }

    void SessionsThatAppearInAnEmptyListPutTheCursorOnTheFirstRow() {
        FailOnWarnings();
        Boot(0, FAKE_CLAUDE, true);
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(ListStatusNow(), QString("No sessions."));

        QVERIFY(ListWithout({}));

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 4, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), 0, TIMEOUT_MS);
        QVERIFY(ListStatusNow().isEmpty());
        Press(Qt::Key_Return);
        // The first session of the list: those that came in a state but
        // `working` are unread, so they are listed before ALPHA.
        const QString first = IdAt(0);
        QCOMPARE(first, BETA);
        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), first, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(first)),
                                 TIMEOUT_MS);
    }

    void BeforeTheFirstListThePanelSaysItIsLoading() {
        FailOnWarnings();
        // An API that is not connected: the model never loads.
        api::Api api;
        QQmlEngine engine;
        auto* singleton =
            engine.singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(singleton != nullptr);
        singleton->setApi(api);
        // As the window does for a plugin with a native module.
        engine.addImportPath(QStringLiteral(ROOST_PLUGINS_DIR "/sessions/qml"));
        QQmlComponent component(
            &engine, QUrl::fromLocalFile(QStringLiteral(
                         SESSIONS_PLUGIN_DIR "/qml/SessionsPanel.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        const std::unique_ptr<QObject> panel(component.create());
        QVERIFY(panel != nullptr);
        auto* text = panel->findChild<QQuickItem*>("sessionsStatus");
        QVERIFY(text != nullptr);

        QCOMPARE(text->property("text").toString(),
                 QString("Loading sessions…"));
        QVERIFY(text->isVisible());
    }

    void AnAgentThatCannotListIsSaidSoInsteadOfNoSessions() {
        FailOnWarnings();
        Boot(0, BROKEN_CLAUDE);
        if (QTest::currentTestFailed()) {
            return;
        }

        QTRY_VERIFY_WITH_TIMEOUT(
            UnavailableNow().startsWith("claude: claude agents exited with "
                                        "code 2"),
            TIMEOUT_MS);
        QVERIFY2(UnavailableNow().contains("claude is broken"),
                 qPrintable(UnavailableNow()));
        // Not "No sessions.": the list is empty because claude cannot list.
        QVERIFY(ListStatusNow().isEmpty());
        QVERIFY(!ListStatus()->isVisible());
        // The rows end above the text, so that it hides none of them.
        auto* unavailable =
            PanelOf(LIST)->findChild<QQuickItem*>("sessionsUnavailable");
        QVERIFY(unavailable->isVisible());
        QVERIFY(ListView()->y() + ListView()->height() <= unavailable->y());
    }

    void ReturnMovesTheKeyboardToTheTerminalAndTheKeysReachTheTui() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(ListView()->hasActiveFocus());

        Press(Qt::Key_Down);
        Press(Qt::Key_Return);

        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(!ListView()->hasActiveFocus());
        Settle();
        for (const char key : {'w', 'o', 'r', 'l', 'd'}) {
            QTest::keyClick(Window(), key);
            // Each key in a read of its own: the split a slow runner makes.
            QTest::qWait(50);
        }
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(TYPED_WORLD), TIMEOUT_MS);
        // The typed letters did not move the list.
        QCOMPARE(Cursor(), 1);
    }

    void ControlBackslashMovesTheKeyboardToTheListAndSendsNoByteToTheTui() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasActiveFocus(), TIMEOUT_MS);
        QTest::keyClick(Window(), 'w');
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains("input w total=1"),
                                 TIMEOUT_MS);

        Press(Qt::Key_Backslash, CONTROL_KEY);

        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(!Terminal()->hasActiveFocus());
        QCOMPARE(Cursor(), 1);
        QCOMPARE(SelectedId(), BETA);
        // The byte 0x1c did not reach the TUI: its last input is still "w".
        QTest::qWait(300);
        const QString screen = ScreenNow();
        QVERIFY2(screen.contains("input w total=1"), qPrintable(screen));
        QVERIFY2(!screen.contains("x1c"), qPrintable(screen));
        // The keys go to the list now.
        Press(Qt::Key_Down);
        QCOMPARE(Cursor(), 2);
    }

    void ReturnOnTheOpenRowAfterControlBackslashRefocusesTheTerminalOnly() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Backslash, CONTROL_KEY);
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
        Watch();
        QSignalSpy changed(PanelOf(LIST), SIGNAL(selectedChanged()));

        Press(Qt::Key_Return);

        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(!ListView()->hasActiveFocus());
        QTest::qWait(300);
        QCOMPARE(changed.count(), 0);
        QVERIFY2(!Opened(), qPrintable(Changes_.join(", ")));
        QCOMPARE(SelectedId(), BETA);
        QCOMPARE(Status(), QString("open"));
    }

    void ControlBackslashInTheListKeepsTheKeyboardInTheList() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Down);
        QVERIFY(ListView()->hasActiveFocus());

        // After each press: a toggle is back in the list after the second.
        for (int i = 0; i < 2; ++i) {
            Press(Qt::Key_Backslash, CONTROL_KEY);
            QTest::qWait(100);
            QVERIFY(ListView()->hasActiveFocus());
            QVERIFY(!Terminal()->hasActiveFocus());
        }
        QCOMPARE(Cursor(), 1);
        QCOMPARE(Status(), QString("none"));
        QVERIFY(SelectedId().isEmpty());
    }

    // From the panel's context, as a plugin calls the store.
    void StoreSet(const QString& document) {
        QQmlExpression set(
            QQmlEngine::contextForObject(PanelOf(LIST)), PanelOf(LIST),
            QStringLiteral("Roost.store.set(\"sessions\", %1)").arg(document));
        set.evaluate();
        QVERIFY2(!set.hasError(), qPrintable(set.error().toString()));
    }

    void TheSessionsAreUnderTheHeaderOfTheirCategory() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        QCOMPARE(Kinds(), (QStringList{"header", "session", "session",
                                       "session", "session"}));
        QCOMPARE(Role(0, "category").toString(), QString());
        QCOMPARE(Role(0, "count").toInt(), 4);
        QQuickItem* header = ViewItem(0);
        QVERIFY(header != nullptr);
        auto* title = header->findChild<QQuickItem*>("headerTitle");
        QVERIFY(title != nullptr);
        QCOMPARE(title->property("text").toString(),
                 QString("▾ Uncategorized · 4"));
    }

    void ADocumentWrittenToTheStoreRegroupsTheList() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        WaitForThePanelToStopWriting();
        if (QTest::currentTestFailed()) {
            return;
        }
        StoreSet(
            QStringLiteral(R"({version: 1, categories: {"claude/%1": "work"}})")
                .arg(BETA));

        QTRY_COMPARE_WITH_TIMEOUT(
            Kinds(),
            (QStringList{"header", "session", "header", "session", "session",
                         "session"}),
            TIMEOUT_MS);
        QCOMPARE(Role(0, "category").toString(), QString("work"));
        QCOMPARE(Ids(), (QStringList{BETA, ALPHA, GAMMA, DELTA}));
    }

    void TheListStateSurvivesAWindowRestart() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        // Folding the only category leaves its header as the one stop.
        Invoke("toggleFold", QString());
        QTRY_COMPARE_WITH_TIMEOUT(Kinds(), (QStringList{"header"}), TIMEOUT_MS);
        QCOMPARE(CursorRow(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument().value("folded").toArray(),
                                  QJsonArray{QString()}, TIMEOUT_MS);

        CloseWindow();
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }

        QTRY_COMPARE_WITH_TIMEOUT(Kinds(), (QStringList{"header"}), TIMEOUT_MS);
        QVERIFY(Role(0, "folded").toBool());
        QCOMPARE(Role(0, "count").toInt(), 4);
        QTRY_COMPARE_WITH_TIMEOUT(CursorRow(), 0, TIMEOUT_MS);
    }

    void TheOpenedSessionIsOpenedAgainAfterAWindowRestart() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument()
                                      .value("view")
                                      .toObject()
                                      .value("opened")
                                      .toString(),
                                  QString("claude/" + BETA), TIMEOUT_MS);

        CloseWindow();
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }

        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), BETA, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(BETA)),
                                 TIMEOUT_MS);
    }

    void AnOpenSessionThatIsGoneIsNotOpenedAfterAWindowRestart() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument()
                                      .value("view")
                                      .toObject()
                                      .value("opened")
                                      .toString(),
                                  QString("claude/" + BETA), TIMEOUT_MS);
        CloseWindow();
        // A new daemon: the old one would answer with the list it has.
        StopDaemon();
        QVERIFY(ListWithout({BETA}));
        QVERIFY(StartDaemon());

        SpySelected_ = true;
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 3, TIMEOUT_MS);
        QTest::qWait(300);
        // `selected` never held the session that is gone, not for a moment.
        QVERIFY(SelectedSpy_ != nullptr);
        QCOMPARE(SelectedSpy_->count(), 0);
        QVERIFY(SelectedId().isEmpty());
        QCOMPARE(Status(), QString("none"));
        // The document does not keep it as the open one.
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument()
                                      .value("view")
                                      .toObject()
                                      .value("opened")
                                      .toString(),
                                  QString(), TIMEOUT_MS);
    }

    // A file the store cannot parse stands for a document that cannot be read.
    bool WriteStoreFile(const QByteArray& content) const {
        const QDir config(Dir_->filePath("config"));
        QSaveFile file(config.filePath("sessions.json"));
        return config.mkpath(".") && file.open(QIODevice::WriteOnly) &&
               file.write(content) >= 0 && file.commit();
    }

    void ADocumentThatCannotBeReadMakesTheListReadOnlyAndIsNotOverwritten() {
        FailOnWarnings();
        const QByteArray broken = "{not json";
        QVERIFY(WriteStoreFile(broken));
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        QTRY_VERIFY_WITH_TIMEOUT(List()->property("readOnly").toBool(),
                                 TIMEOUT_MS);
        QVERIFY(!List()->property("problem").toString().isEmpty());
        // Edits that would be written: a fold, and an open session.
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        Invoke("toggleFold", QString());
        QTest::qWait(500);

        QCOMPARE(ReadFile(Dir_->filePath("config/sessions.json")), broken);
    }

    void ARestartedRoostdsDocumentIsReadAgainAtTheReconnect() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(Kinds().count("header"), 1);

        StopDaemon();
        QTRY_COMPARE_WITH_TIMEOUT(Root()->property("connection").toString(),
                                  QString("disconnected"), TIMEOUT_MS);
        QVERIFY(WriteStoreFile(
            QStringLiteral(
                R"({"version": 1, "categories": {"claude/%1": "work"}})")
                .arg(BETA)
                .toUtf8()));
        QVERIFY(StartDaemon());

        QTRY_COMPARE_WITH_TIMEOUT(Root()->property("connection").toString(),
                                  QString("connected"), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(
            Kinds(),
            (QStringList{"header", "session", "header", "session", "session",
                         "session"}),
            30000);
        QCOMPARE(Ids(), (QStringList{BETA, ALPHA, GAMMA, DELTA}));
    }

    void EnterOnAFoldedHeaderUnfoldsAndOpensNothing() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Invoke("toggleFold", QString());
        QTRY_COMPARE_WITH_TIMEOUT(Kinds(), (QStringList{"header"}), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(CursorRow(), 0, TIMEOUT_MS);
        QVERIFY(ListView()->hasActiveFocus());
        Watch();
        QSignalSpy activated(PanelOf(LIST), SIGNAL(activated()));
        QSignalSpy selected(PanelOf(LIST), SIGNAL(selectedChanged()));

        Press(Qt::Key_Return);

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 4, TIMEOUT_MS);
        QVERIFY(!Role(0, "folded").toBool());
        // The cursor went in, to the first session of the category.
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), 0, TIMEOUT_MS);
        QTest::qWait(300);
        QCOMPARE(activated.count(), 0);
        QCOMPARE(selected.count(), 0);
        QVERIFY(SelectedId().isEmpty());
        QVERIFY2(!Opened(), qPrintable(Changes_.join(", ")));
        QVERIFY(ListView()->hasActiveFocus());
        QCOMPARE(Status(), QString("none"));
    }

    // The build holds copies of the plugin's files (configure_file); a copy
    // that is older than the source is a panel the window does not show.
    void TheBuildCopiesOfThePluginAreTheSourceFiles() {
        for (const QString& file : {QStringLiteral("plugin.json"),
                                    QStringLiteral("qml/SessionsPanel.qml")}) {
            const QByteArray source =
                ReadFile(QStringLiteral(SESSIONS_PLUGIN_DIR "/") + file);
            const QByteArray copy =
                ReadFile(QStringLiteral(ROOST_PLUGINS_DIR "/sessions/") + file);
            QVERIFY2(!source.isEmpty(), qPrintable(file));
            QVERIFY2(source == copy,
                     qPrintable(file + ": the build copy is stale"));
        }
    }
};

QTEST_MAIN(SessionsPanelTest)
#include "sessions_panel_test.moc"
