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
#include <cmath>

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

namespace {

// The relative luminance and the contrast ratio of WCAG 2.x.
double Luminance(const QColor& color) {
    const auto channel = [](float value) {
        const auto v = static_cast<double>(value);
        return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) +
           0.0722 * channel(color.blueF());
}

double Contrast(const QColor& a, const QColor& b) {
    const double la = Luminance(a);
    const double lb = Luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// `top` over `bottom`, as the scene graph composes two rectangles.
QColor Over(const QColor& top, const QColor& bottom) {
    const float alpha = top.alphaF();
    return QColor::fromRgbF(
        top.redF() * alpha + bottom.redF() * (1 - alpha),
        top.greenF() * alpha + bottom.greenF() * (1 - alpha),
        top.blueF() * alpha + bottom.blueF() * (1 - alpha));
}

}  // namespace

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

    // The list is one line a row, as the elide of a long title says: a newline
    // or a tab of a title (or of a working directory) is a space.
    void ATitleWithANewlineOrATabIsOneLineLikeTheOthers() {
        FailOnWarnings();
        QVERIFY(WriteFakeClaude(FAKE_CLAUDE));
        const QString title = "line1\nline2\ttabbed\rcr\u2028ls\u007fdel";
        const QString cwd = "/work/al\npha";
        QJsonArray rows;
        for (const auto& [id, name] :
             {std::pair{ALPHA, QString("plain")}, std::pair{BETA, title}}) {
            rows.append(QJsonObject{{"id", id},
                                    {"name", name},
                                    {"cwd", id == BETA ? cwd : "/work/alpha"},
                                    {"kind", "background"},
                                    {"state", "working"},
                                    {"status", "busy"}});
        }
        QSaveFile agents(AgentsPath());
        QVERIFY(agents.open(QIODevice::WriteOnly));
        QVERIFY(agents.write(QJsonDocument(rows).toJson()) > 0);
        QVERIFY(agents.commit());
        QVERIFY(StartDaemon());
        OpenWindow();
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 2, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(RowItem(0) != nullptr && RowItem(1) != nullptr,
                                 TIMEOUT_MS);

        QCOMPARE(RowText(1, "sessionTitle"),
                 QString("line1 line2 tabbed cr ls del"));
        QCOMPARE(RowText(1, "sessionDetail"),
                 QString("working · /work/al pha"));
        for (const char* name : {"sessionTitle", "sessionDetail"}) {
            const QObject* plain = RowItem(0)->findChild<QObject*>(name);
            const QObject* odd = RowItem(1)->findChild<QObject*>(name);
            QVERIFY(plain != nullptr && odd != nullptr);
            QCOMPARE(odd->property("lineCount").toInt(), 1);
            QCOMPARE(odd->property("height").toDouble(),
                     plain->property("height").toDouble());
        }
        QCOMPARE(RowItem(1)->height(), RowItem(0)->height());
    }

    // The title of a row that is open or under the cursor reads (WCAG AA,
    // 4.5:1) on what is really behind it: the panel, the highlight of the
    // list's cursor (focused or not), and for the open row the tint over that.
    // The other rows keep the colour of their state.
    void TheTitleOfTheOpenOrCursorRowReadsOnTheRealBackground() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QQuickItem* background =
            PanelOf(LIST)->findChild<QQuickItem*>("sessionsBackground");
        QVERIFY(background != nullptr);
        const QColor panel = background->property("color").value<QColor>();

        // The colour of the title of the session and what it lies on, with the
        // cursor on `cursor`.
        const auto check = [&](int session, int cursor, bool open,
                               bool focused) {
            QCOMPARE(ListView()->hasActiveFocus(), focused);
            QCOMPARE(Cursor(), cursor);
            QQuickItem* item = RowItem(session);
            QVERIFY(item != nullptr);
            const QQuickItem* bar = item->findChild<QQuickItem*>("openBar");
            const QQuickItem* rowGround =
                item->findChild<QQuickItem*>("rowBackground");
            QVERIFY(bar != nullptr && rowGround != nullptr);
            QCOMPARE(bar->isVisible(), open);
            // One rectangle is the ground of the row, whatever signs it has.
            const QColor ground =
                Over(rowGround->property("color").value<QColor>(), panel);
            const QColor titleColor =
                item->findChild<QQuickItem*>("sessionTitle")
                    ->property("color")
                    .value<QColor>();
            const double ratio = Contrast(titleColor, ground);
            QVERIFY2(ratio >= 4.5,
                     qPrintable(QStringLiteral("%1 open=%2 focused=%3 %4 on "
                                               "%5: %6")
                                    .arg(ORDER[session])
                                    .arg(open)
                                    .arg(focused)
                                    .arg(titleColor.name(), ground.name())
                                    .arg(ratio)));
        };

        // ALPHA works, BETA is blocked (blue), GAMMA and DELTA are done.
        for (const int session : {2, 0, 1}) {
            // Open: the keyboard goes to the terminal, the list is unfocused.
            OpenRow(session);
            if (QTest::currentTestFailed()) {
                return;
            }
            check(session, session, true, false);
            if (QTest::currentTestFailed()) {
                return;
            }
            BackToList();
            if (QTest::currentTestFailed()) {
                return;
            }
            check(session, session, true, true);
            if (QTest::currentTestFailed()) {
                return;
            }
            // The cursor leaves the open row: it is read on its tint alone,
            // and the row under the cursor, not open, on the focused cursor.
            const int other = session + 1;
            Press(Qt::Key_Down);
            QTRY_COMPARE_WITH_TIMEOUT(Cursor(), session + 1, TIMEOUT_MS);
            check(other, other, false, true);
            if (QTest::currentTestFailed()) {
                return;
            }
            check(session, other, true, true);
            if (QTest::currentTestFailed()) {
                return;
            }
            // The state does not colour the title: a row with neither the
            // open sign nor the cursor has the text colour, and the state is
            // a word in the second line.
            const int plain = session == 0 ? 2 : 0;
            const QColor titleColor =
                RowItem(plain)
                    ->findChild<QQuickItem*>("sessionTitle")
                    ->property("color")
                    .value<QColor>();
            QCOMPARE(titleColor, QColor("#18232e"));
            Press(Qt::Key_Up);
            QTRY_COMPARE_WITH_TIMEOUT(Cursor(), session, TIMEOUT_MS);
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
        PressWhenSettled(Qt::Key_Return);

        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), BETA, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(BETA)),
                                 TIMEOUT_MS);
        BackToList();

        // Enter of the keypad does the same, for the row above.
        Press(Qt::Key_Up);
        PressWhenSettled(Qt::Key_Enter, Qt::KeypadModifier);

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

        PressWhenSettled(Qt::Key_Return);
        PressWhenSettled(Qt::Key_Enter, Qt::KeypadModifier);
        WaitForEffects();

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
        WaitForEffects();

        QVERIFY(SelectedId().isEmpty());
        QVERIFY2(!Opened(), qPrintable(Changes_.join(", ")));
        QVERIFY(ListView()->hasActiveFocus());
        QVERIFY(!Terminal()->hasActiveFocus());
        // The same key pressed once opens the row.
        PressWhenSettled(Qt::Key_Return);
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

        PressWhenSettled(Qt::Key_Return);

        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), ALPHA, TIMEOUT_MS);
        WaitForEffects();
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

        PressWhenSettled(Qt::Key_Return);
        PressWhenSettled(Qt::Key_Enter, Qt::KeypadModifier);
        WaitForEffects();

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
        // A later list, answered after the first: the terminal is not touched
        // by it either.
        QVERIFY(ListWithout({ALPHA, DELTA}));
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 2, TIMEOUT_MS);
        WaitForEffects();
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
        PressWhenSettled(Qt::Key_Return);
        PressWhenSettled(Qt::Key_Enter, Qt::KeypadModifier);
        Press(Qt::Key_Down);
        Press(Qt::Key_J);
        WaitForEffects();
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
        // The reset of the disconnect has settled, so the panel's reconcile has
        // run, and a wrong one has cleared the selection.
        WaitUntilSettled();
        FlushDeferredCalls();
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
        WaitUntilSettled();
        WaitForEffects();

        QVERIFY(SelectedId().isEmpty());
        QCOMPARE(Status(), QString("none"));
        // The cursor is where the user left it, and Return works again.
        QCOMPARE(CursorId(), BETA);
        PressWhenSettled(Qt::Key_Return);
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
        WaitUntilSettled();
        WaitForEffects();

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
        PressWhenSettled(Qt::Key_Return);
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
        PressWhenSettled(Qt::Key_Return);

        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(!ListView()->hasActiveFocus());
        Settle();
        int typed = 0;
        for (const char key : {'w', 'o', 'r', 'l', 'd'}) {
            QTest::keyClick(Window(), key);
            // Each key in a read of its own: the split a slow runner makes.
            // The next key goes once this one is shown, so each is a read of
            // its own.
            ++typed;
            const QRegularExpression shown(
                QStringLiteral("total=%1(?!\\d)").arg(typed));
            QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(shown), TIMEOUT_MS);
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
        // The keys go to the list now.
        Press(Qt::Key_Down);
        QCOMPARE(Cursor(), 2);
        Press(Qt::Key_Up);
        QCOMPARE(Cursor(), 1);

        // The byte 0x1c did not reach the TUI. What the terminal gets goes in
        // the order of its pty, so a "w" typed now is counted after any byte
        // that was on its way: total=2 is the "w" of before and this one.
        PressWhenSettled(Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasActiveFocus(), TIMEOUT_MS);
        QTest::keyClick(Window(), 'w');
        QTRY_VERIFY_WITH_TIMEOUT(
            !ScreenNow().contains(QRegularExpression("total=1(?!\\d)")),
            TIMEOUT_MS);
        const QString screen = ScreenNow();
        QVERIFY2(screen.contains("input w total=2"), qPrintable(screen));
        QVERIFY2(!screen.contains("x1c"), qPrintable(screen));
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

        PressWhenSettled(Qt::Key_Return);

        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(!ListView()->hasActiveFocus());
        WaitForEffects();
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
            WaitForEffects();
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
                 QString("▾ Uncategorized"));
        auto* count = header->findChild<QQuickItem*>("headerCount");
        QVERIFY(count != nullptr);
        QCOMPARE(count->property("text").toString(), QString("4"));
    }

    // The header is one line like a row: a newline of a category is a space.
    void ACategoryWithANewlineIsOneLineHeader() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForThePanelToStopWriting();
        if (QTest::currentTestFailed()) {
            return;
        }
        StoreSet(QStringLiteral(
                     R"({version: 1, categories: {"claude/%1": "wo\nrk"}})")
                     .arg(BETA));
        QTRY_COMPARE_WITH_TIMEOUT(
            Kinds(),
            (QStringList{"header", "session", "header", "session", "session",
                         "session"}),
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            ViewItem(0) != nullptr && ViewItem(2) != nullptr, TIMEOUT_MS);

        const auto* odd = ViewItem(0)->findChild<QQuickItem*>("headerTitle");
        const auto* plain = ViewItem(2)->findChild<QQuickItem*>("headerTitle");
        QVERIFY(odd != nullptr && plain != nullptr);
        QCOMPARE(odd->property("text").toString(), QString("▾ wo rk"));
        QCOMPARE(odd->property("lineCount").toInt(), 1);
        QCOMPARE(odd->property("height").toDouble(),
                 plain->property("height").toDouble());
        QCOMPARE(ViewItem(0)->height(), ViewItem(2)->height());
        QCOMPARE(odd->property("textFormat").toInt(),
                 static_cast<int>(Qt::PlainText));
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
                                      .value("list")
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
                                      .value("list")
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
        // The panel has read the document and settled: what it would do with
        // the session that is gone, it has done.
        WaitForThePanelToStopWriting();
        WaitUntilSettled();
        WaitForEffects();
        // `selected` never held the session that is gone, not for a moment.
        QVERIFY(SelectedSpy_ != nullptr);
        QCOMPARE(SelectedSpy_->count(), 0);
        QVERIFY(SelectedId().isEmpty());
        QCOMPARE(Status(), QString("none"));
        // The document does not keep it as the open one.
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument()
                                      .value("view")
                                      .toObject()
                                      .value("list")
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
        WaitForEffects();

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

    // The list keeps what it changed while roostd was away over what the
    // document of the next connection says, and takes the rest of it.
    void AnEditMadeWhileRoostdIsDownSurvivesTheReread() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(Kinds().count("header"), 1);

        StopDaemon();
        QTRY_COMPARE_WITH_TIMEOUT(Root()->property("connection").toString(),
                                  QString("disconnected"), TIMEOUT_MS);
        // Nowhere to write: the fold waits (the list shows nothing meanwhile).
        Invoke("toggleFold", QString());
        FlushDeferredCalls();
        // The document meanwhile: a category for BETA, written by someone else.
        QVERIFY(WriteStoreFile(
            QStringLiteral(
                R"({"version": 1, "categories": {"claude/%1": "work"}})")
                .arg(BETA)
                .toUtf8()));
        QVERIFY(StartDaemon());

        QTRY_COMPARE_WITH_TIMEOUT(Root()->property("connection").toString(),
                                  QString("connected"), 30000);
        // Ours: the fold of the uncategorized. Theirs: the category.
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument().value("folded").toArray(),
                                  QJsonArray{QString()}, 30000);
        QCOMPARE(StoredDocument()
                     .value("categories")
                     .toObject()
                     .value("claude/" + BETA)
                     .toString(),
                 QString("work"));
        // Its fold is not the fold of "work": the session is shown.
        QTRY_VERIFY_WITH_TIMEOUT(Ids().contains(BETA), 30000);
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

        PressWhenSettled(Qt::Key_Return);

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 4, TIMEOUT_MS);
        QVERIFY(!Role(0, "folded").toBool());
        // The cursor went in, to the first session of the category.
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), 0, TIMEOUT_MS);
        WaitForEffects();
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
