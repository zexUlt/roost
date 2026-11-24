// The signs of a row of the sessions panel (docs/design/18-style.md): open,
// cursor, unread and waiting are independent and combine on one row. The
// window, the daemon and the fake `claude` are helpers/sessions_window.hpp; the
// panel and the model are the real ones, and what is checked is the items of
// the row's delegate.
//
// With ROOST_TEST_SNAPSHOTS set to a directory, the cases save a picture of the
// window there, for a person to look at.

#include <QColor>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTest>

#include <functional>

#include "helpers/sessions_window.hpp"
#include "qml_api/roost_singleton.hpp"
#include "qml_api/theme.hpp"

using namespace roost;
using namespace roost::testing;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

class SessionsSignsTest : public SessionsWindow {
    Q_OBJECT

private:
    // The document `sessions` as the daemon finds it at the start.
    bool Seed(const QJsonObject& document) {
        if (!QDir().mkpath(Dir_->filePath("config"))) {
            return false;
        }
        QSaveFile file(Dir_->filePath("config/sessions.json"));
        return file.open(QIODevice::WriteOnly) &&
               file.write(QJsonDocument(document).toJson()) >= 0 &&
               file.commit();
    }

    // BETA waits (the fake claude says `blocked`) and is unread, in a
    // category with a colour of its own; ALPHA works; GAMMA and DELTA are done.
    void BootWithUnreadBeta() {
        QVERIFY(Seed(
            QJsonObject{{"version", 1},
                        {"unread", QJsonArray{"claude/" + BETA}},
                        {"categories", QJsonObject{{"claude/" + BETA, "Ops"}}},
                        {"colors", QJsonObject{{"Ops", 2}}}}));
        Boot();
    }

    int Number(const QString& id) {
        return static_cast<int>(Ids().indexOf(id));
    }

    // The delegate of the session, made.
    QQuickItem* Row(const QString& id) {
        QQuickItem* item = nullptr;
        WaitFor([&] {
            item = RowItem(Number(id));
            return item != nullptr;
        });
        return item;
    }

    static bool WaitFor(const std::function<bool()>& predicate) {
        return QTest::qWaitFor(predicate, TIMEOUT_MS);
    }

    // The Item of the row's delegate with the object name.
    static QQuickItem* In(QQuickItem* row, const char* name) {
        return row->findChild<QQuickItem*>(name);
    }

    // The row draws each sign with an item of its own.
    static bool HasParts(QQuickItem* row) {
        for (const char* name :
             {"openBar", "rowBackground", "cursorRing", "unreadDot",
              "sessionTitle", "sessionDetail"}) {
            if (row == nullptr || In(row, name) == nullptr) {
                return false;
            }
        }
        return true;
    }

    static QColor ColorOf(const QObject* item) {
        return item->property("color").value<QColor>();
    }

    // Waits until the list has the keyboard, or has not.
    void WaitForListFocus(bool focused) {
        QTRY_COMPARE_WITH_TIMEOUT(ListView()->hasActiveFocus(), focused,
                                  TIMEOUT_MS);
    }

    // Saves a picture of the window as <case>-<step>.png when asked to.
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

private slots:
    void init() { Prepare(); }

    void cleanup() { Teardown(); }

    // One row is open, under the cursor of the focused list, unread and
    // waiting: each sign is drawn by its own item, and the rows with no sign
    // draw none.
    void AllFourSignsShowOnOneRow() {
        FailOnWarnings();
        BootWithUnreadBeta();
        if (QTest::currentTestFailed()) {
            return;
        }
        const Theme* theme = Singleton_->theme();
        // The model never shows the open session as unread (ListState:
        // IsShownUnread), so the two signs meet only when the panel's
        // `selected` is set without the model's `opened` following it, which
        // `restored` = false does. The list keeps the keyboard.
        while (CursorId() != BETA) {
            Press(Qt::Key_Down);
            QTRY_VERIFY_WITH_TIMEOUT(!CursorId().isEmpty(), TIMEOUT_MS);
        }
        QQuickItem* const panel = PanelOf(LIST);
        QVERIFY(panel->setProperty("restored", false));
        QVERIFY(panel->setProperty(
            "selected", QVariantMap{{"agent", "claude"}, {"id", BETA}}));
        QVERIFY(ListView()->hasActiveFocus());
        QCOMPARE(CursorId(), BETA);
        QQuickItem* row = Row(BETA);
        QVERIFY(HasParts(row));
        QTRY_VERIFY_WITH_TIMEOUT(row->property("isOpen").toBool(), TIMEOUT_MS);

        // Open: the ground, the bar, the title in the accent colour.
        QVERIFY(row->property("isOpen").toBool());
        QVERIFY(In(row, "openBar")->isVisible());
        QCOMPARE(ColorOf(In(row, "openBar")), theme->accent());
        QCOMPARE(In(row, "openBar")->width(), 3.0);
        QCOMPARE(ColorOf(In(row, "rowBackground")),
                 theme->selectedBackground());
        QCOMPARE(ColorOf(In(row, "sessionTitle")), theme->accent());
        QCOMPARE(
            In(row, "sessionTitle")->property("font").value<QFont>().weight(),
            QFont::DemiBold);
        // Cursor: the ring of the focus inside the row; the bar is beside it.
        QVERIFY(row->property("isCursor").toBool());
        QQuickItem* ring = In(row, "cursorRing");
        QVERIFY(ring->isVisible());
        QCOMPARE(ring->property("border").value<QObject*>()->property("width"),
                 QVariant(theme->cursorRingWidth()));
        QCOMPARE(ring->property("border").value<QObject*>()->property("color"),
                 QVariant(theme->focus()));
        QCOMPARE(In(row, "openBar")->x(), 4.0);
        // Unread: a dot of 6 px at the right edge.
        QVERIFY(row->property("isUnread").toBool());
        QQuickItem* dot = In(row, "unreadDot");
        QVERIFY(dot->isVisible());
        QCOMPARE(dot->width(), 6.0);
        QCOMPARE(dot->height(), 6.0);
        QCOMPARE(ColorOf(dot), theme->accent());
        QVERIFY(dot->x() + dot->width() <= row->width());
        QVERIFY(dot->x() > row->width() / 2);
        // Waiting: the second line in the colour of attention.
        QVERIFY(row->property("isWaiting").toBool());
        QCOMPARE(ColorOf(In(row, "sessionDetail")), theme->attention());
        QVERIFY(In(row, "sessionDetail")
                    ->property("text")
                    .toString()
                    .startsWith("waiting"));
        Snapshot("1-focused");

        // A row with none of the signs draws none of them.
        QQuickItem* plain = Row(GAMMA);
        QVERIFY(HasParts(plain));
        QVERIFY(!In(plain, "openBar")->isVisible());
        QVERIFY(!In(plain, "cursorRing")->isVisible());
        QVERIFY(!In(plain, "unreadDot")->isVisible());
        QCOMPARE(ColorOf(In(plain, "rowBackground")).alpha(), 0);
        QCOMPARE(ColorOf(In(plain, "sessionTitle")), theme->text());
        QCOMPARE(ColorOf(In(plain, "sessionDetail")), theme->textSecondary());

        // The same row with the keyboard in the terminal: no ring, the rest
        // stays.
        Terminal()->forceActiveFocus();
        WaitForListFocus(false);
        QVERIFY(!In(row, "cursorRing")->isVisible());
        QVERIFY(In(row, "openBar")->isVisible());
        QVERIFY(In(row, "unreadDot")->isVisible());
        QCOMPARE(ColorOf(In(row, "rowBackground")),
                 theme->selectedBackground());
        QCOMPARE(ColorOf(In(row, "sessionDetail")), theme->attention());
        Snapshot("2-unfocused");
    }

    // The list loses the keyboard: the ring goes, the open row keeps its
    // ground, and a row under the cursor that is not open has the ground of
    // an unfocused cursor.
    void TheRingIsDrawnOnlyWhileTheListHasTheKeyboard() {
        FailOnWarnings();
        BootWithUnreadBeta();
        if (QTest::currentTestFailed()) {
            return;
        }
        const Theme* theme = Singleton_->theme();
        OpenRow(Number(BETA));
        // The keyboard is in the terminal.
        WaitForListFocus(false);
        QQuickItem* row = Row(BETA);
        QVERIFY(HasParts(row));
        QVERIFY(row->property("isCursor").toBool());
        QVERIFY(!In(row, "cursorRing")->isVisible());
        QVERIFY(In(row, "openBar")->isVisible());
        QCOMPARE(ColorOf(In(row, "rowBackground")),
                 theme->selectedBackground());
        // An open session is not unread.
        QVERIFY(!In(row, "unreadDot")->isVisible());
        Snapshot("1-unfocused");

        BackToList();
        QVERIFY(In(row, "cursorRing")->isVisible());

        // The cursor leaves the open row to a row that is not open.
        const int beta = Number(BETA);
        Press(Qt::Key_Down);
        QTRY_VERIFY_WITH_TIMEOUT(Cursor() != beta, TIMEOUT_MS);
        QQuickItem* other = RowItem(Cursor());
        QVERIFY(HasParts(other));
        QVERIFY(In(other, "cursorRing")->isVisible());
        QCOMPARE(ColorOf(In(other, "rowBackground")), theme->cursorFocused());
        QVERIFY(!In(row, "cursorRing")->isVisible());
        QVERIFY(In(row, "openBar")->isVisible());
        QCOMPARE(In(row, "openBar")->x(), 1.0);
        Snapshot("2-cursor-away");

        Terminal()->forceActiveFocus();
        WaitForListFocus(false);
        QVERIFY(!In(other, "cursorRing")->isVisible());
        QCOMPARE(ColorOf(In(other, "rowBackground")), theme->cursorUnfocused());
        Snapshot("3-cursor-away-unfocused");
    }
};

QTEST_MAIN(SessionsSignsTest)
#include "sessions_signs_test.moc"
