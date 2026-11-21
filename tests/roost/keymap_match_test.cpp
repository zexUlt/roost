// KeyMatcher in a real QQuickWindow on the offscreen platform, against the
// real roostd with a plugin whose backend is the fixture backend and a
// keymap.json. A key of the table runs the command `fixture.notify`, which
// has the backend say that a session appeared; so the rows of the window's
// session cache tell which entry ran (the ids are k<N>: the fixture backend
// lists s1 on its own). A sentinel run after a press (roostd handles one
// socket in order) tells that no entry ran. What reaches the item in focus is
// counted by a filter on the item. Native codes are given for both platforms,
// as in roost_binding_key_test.cpp.

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

#include <memory>

#include "api/api.hpp"
#include "api/api_test_support.hpp"
#include "helpers/roostd_process.hpp"
#include "roost/key_matcher.hpp"
#include "terminal/physical_keys.hpp"

using namespace roost;
using namespace roost::api_test;

// The static plugin of module Roost.Api, linked into this executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)

namespace {

const QString AGENT = QStringLiteral("fixture-agent");

constexpr int CYR_O = 0x041E;

// The J key: kVK_ANSI_J, and KEY_J + 8.
constexpr quint32 MAC_J = 0x26;
constexpr quint32 XKB_J = 44;
// The C key.
constexpr quint32 MAC_C = 0x08;
constexpr quint32 XKB_C = 54;
// The slash key.
constexpr quint32 MAC_SLASH = 0x2C;
constexpr quint32 XKB_SLASH = 61;
// The equal key.
constexpr quint32 MAC_EQUAL = 0x18;
constexpr quint32 XKB_EQUAL = 21;
// The 4 key, and the minus key.
constexpr quint32 MAC_4 = 0x15;
constexpr quint32 XKB_4 = 13;
constexpr quint32 MAC_MINUS = 0x1B;
constexpr quint32 XKB_MINUS = 20;
// F5 and K carry codes too: only Qt's key matters for them.
constexpr quint32 MAC_K = 0x28;
constexpr quint32 XKB_K = 45;

// The physical Control key, written out per platform on purpose: on macOS Qt
// names the Control key Meta and the Command key Control.
#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifier CONTROL_KEY = Qt::MetaModifier;
#else
constexpr Qt::KeyboardModifier CONTROL_KEY = Qt::ControlModifier;
#endif

QJsonObject Row(const QString& id) {
    return {{"agent", AGENT},
            {"id", id},
            {"title", ""},
            {"cwd", ""},
            {"state", "idle"}};
}

// The command that makes session `id` appear.
QJsonArray Appears(const QString& id) {
    return {QJsonObject{
        {"jsonrpc", "2.0"},
        {"method", "session.stateChanged"},
        {"params", QJsonObject{{"agent", AGENT}, {"session", Row(id)}}}}};
}

// An entry of keymap.json that makes session `id` appear. The same text for
// Linux and macOS: Ctrl is the Control key on both.
QJsonObject Entry(const QString& key, const QString& id,
                  const QString& when = {}) {
    QJsonObject entry{{"key", key},
                      {"mac", key},
                      {"command", "fixture.notify"},
                      {"args", Appears(id)}};
    if (!when.isEmpty()) {
        entry.insert("when", when);
    }
    return entry;
}

// Counts the key presses that reach an item.
class PressCounter : public QObject {
public:
    int Presses = 0;

protected:
    bool eventFilter(QObject* /*watched*/, QEvent* event) override {
        if (event->type() == QEvent::KeyPress) {
            ++Presses;
        }
        return false;
    }
};

// Counts the key releases that reach an item.
class ReleaseCounter : public QObject {
public:
    int* Count = nullptr;

protected:
    bool eventFilter(QObject* /*watched*/, QEvent* event) override {
        if (event->type() == QEvent::KeyRelease) {
            ++*Count;
        }
        return false;
    }
};

// Three panels, as the shell has them: a slot is a FocusScope with the
// objectName "panelSlot" and a panelType.
const char* const QML = R"(
import QtQuick
import QtQuick.Controls
import Roost.Api

Item {
    width: 300
    height: 300
    FocusScope {
        objectName: "panelSlot"
        property string panelType: "sessions.list"
        Item { objectName: "listItem" }
        TextInput { objectName: "listField" }
    }
    FocusScope {
        objectName: "panelSlot"
        property string panelType: "terminal.view"
        TerminalItem { objectName: "terminal" }
    }
    FocusScope {
        objectName: "panelSlot"
        property string panelType: "other.panel"
        Item { objectName: "otherItem" }
    }
    Item { objectName: "outsideItem" }
    Popup {
        objectName: "popup"
        parent: Overlay.overlay
        modal: true
        width: 100
        height: 100
        contentItem: Item {
            objectName: "popupItem"
            focus: true
        }
    }
}
)";

}  // namespace

class KeymapMatchTest : public QObject {
    Q_OBJECT

private:
    bool WriteKeymap(const QJsonArray& entries) {
        if (!QDir().mkpath(Daemon_->ConfigPath())) {
            return false;
        }
        QFile file(Daemon_->ConfigPath() + "/keymap.json");
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(QJsonDocument(entries).toJson()) > 0;
    }

    // The daemon with the entries, the API connected with the table loaded,
    // the matcher on, the window shown.
    bool Boot(const QJsonArray& entries) {
        if (!Daemon_->IsValid() ||
            !Daemon_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH) ||
            !WriteKeymap(entries) || !Daemon_->Start(ROOSTD_PATH)) {
            return false;
        }
        Api_ = std::make_unique<api::Api>();
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Api_->Connect(Daemon_->SocketPath());
        if (!(!connected.isEmpty() || connected.wait(TIMEOUT_MS)) ||
            !QTest::qWaitFor([this] { return Api_->Keymap().IsLoaded(); },
                             TIMEOUT_MS)) {
            return false;
        }
        Matcher_ =
            std::make_unique<KeyMatcher>(Api_->Keymap(), Api_->Commands());
        QQmlComponent component(&Engine_);
        component.setData(QML, QUrl());
        if (!component.isReady()) {
            qWarning("%s", qPrintable(component.errorString()));
            return false;
        }
        Root_.reset(qobject_cast<QQuickItem*>(component.create()));
        if (Root_ == nullptr) {
            return false;
        }
        Root_->setParentItem(Window_.contentItem());
        Window_.resize(300, 300);
        Window_.show();
        return QTest::qWaitForWindowExposed(&Window_) &&
               QTest::qWaitForWindowActive(&Window_);
    }

    QQuickItem* Focus(const char* name) {
        auto* item = Root_->findChild<QQuickItem*>(name);
        if (item == nullptr) {
            return nullptr;
        }
        item->setProperty("focus", true);
        item->forceActiveFocus();
        Counter_.Presses = 0;
        item->installEventFilter(&Counter_);
        return Window_.activeFocusItem() == item ? item : nullptr;
    }

    // A press with the codes of both platforms.
    void Press(int key, Qt::KeyboardModifiers modifiers, quint32 xkb,
               quint32 mac, const QString& text, bool autoRepeat = false) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, xkb, mac, 0x100, text,
                        autoRepeat);
        QCoreApplication::sendEvent(&Window_, &event);
    }

    void Release(int key, Qt::KeyboardModifiers modifiers, quint32 xkb,
                 quint32 mac, const QString& text) {
        QKeyEvent event(QEvent::KeyRelease, key, modifiers, xkb, mac, 0x100,
                        text);
        QCoreApplication::sendEvent(&Window_, &event);
    }

    // Waits until everything that was sent to roostd before has been done and
    // its events have come: a sentinel row is the last of them.
    bool Flush() {
        const QString id = QStringLiteral("flush%1").arg(++Flushes_);
        const QFuture<rpc::Result> sent =
            Api_->Commands().Run("fixture.notify", Appears(id));
        return WaitFor(sent) && sent.result() &&
               QTest::qWaitFor(
                   [&] { return Api_->Sessions().IndexOf(AGENT, id) >= 0; },
                   TIMEOUT_MS);
    }

    bool Ran(const QString& id) {
        return Api_->Sessions().IndexOf(AGENT, id) >= 0;
    }

    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<KeyMatcher> Matcher_;
    QQmlEngine Engine_;
    QQuickWindow Window_;
    std::unique_ptr<QQuickItem> Root_;
    PressCounter Counter_;
    int Flushes_ = 0;

private slots:
    void init() {
        // The layout is a state of the process: each test starts on a Latin
        // one and tells its own.
        SetNonLatinLayout(false);
        Daemon_ = std::make_unique<roost::testing::RoostdProcess>();
    }

    void cleanup() {
        Window_.hide();
        Matcher_.reset();
        Root_.reset();
        Api_.reset();
        Daemon_.reset();
    }

    // A key of the table pressed in the window runs the command, and the item
    // in focus does not see it.
    void AKeyOfTheTableRunsItsCommand() {
        QVERIFY(Boot({Entry("Ctrl+J", "k1")}));
        QVERIFY(Focus("otherItem"));
        Press(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QString(QChar(0x0A)));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("k1"), TIMEOUT_MS);
        QCOMPARE(Counter_.Presses, 0);
        // A key that the table does not have goes through.
        Press(Qt::Key_K, CONTROL_KEY, XKB_K, MAC_K, QString(QChar(0x0B)));
        QCOMPARE(Counter_.Presses, 1);
    }

    void CtrlJFiresOnTheRussianLayout() {
        QVERIFY(Boot({Entry("Ctrl+J", "k1")}));
        QVERIFY(Focus("otherItem"));
        // The Russian "о" sits at the J key.
        Press(CYR_O, CONTROL_KEY, XKB_J, MAC_J, QStringLiteral("о"));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("k1"), TIMEOUT_MS);
        QCOMPARE(Counter_.Presses, 0);
    }

    // On Dvorak the letter on the key is the letter of the layout: Ctrl+J is
    // the key that types j, wherever it is; the US "j" position types "c".
    void DvorakLettersMatchByTheLayout() {
        QVERIFY(Boot({Entry("Ctrl+J", "sj"), Entry("Ctrl+C", "sc"),
                      Entry("Ctrl+Z", "sz")}));
        QVERIFY(Focus("otherItem"));
        // The Dvorak "j" is at the US "c" position.
        Press(Qt::Key_J, CONTROL_KEY, XKB_C, MAC_C, QStringLiteral("j"));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("sj"), TIMEOUT_MS);
        // The Dvorak "z" is at the US slash position.
        Press(Qt::Key_Z, CONTROL_KEY, XKB_SLASH, MAC_SLASH,
              QStringLiteral("z"));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("sz"), TIMEOUT_MS);
        QVERIFY(Flush());
        QVERIFY(!Ran("sc"));
        QCOMPARE(Counter_.Presses, 0);
    }

    // The Russian layout is told by a letter that was typed: "о" at the J key.
    // After it a symbol key matches by its US position, as a letter does.
    void CtrlSlashFiresOnTheRussianLayout() {
        QVERIFY(Boot({Entry("Ctrl+/", "sslash")}));
        QVERIFY(Focus("otherItem"));
        Press(CYR_O, Qt::NoModifier, XKB_J, MAC_J, QStringLiteral("о"));
        QCOMPARE(Counter_.Presses, 1);
        // The US slash key types "." on the Russian layout.
        Press(Qt::Key_Period, CONTROL_KEY, XKB_SLASH, MAC_SLASH,
              QStringLiteral("."));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("sslash"), TIMEOUT_MS);
        QCOMPARE(Counter_.Presses, 1);
    }

    // Some platforms report the Latin letter of another layout group with
    // Ctrl: such a press does not make the layout Latin, so a symbol key after
    // it still matches by its US position.
    void ACtrlLatinLetterOnTheRussianLayoutKeepsTheLayout() {
        QVERIFY(Boot({Entry("Ctrl+/", "sslash")}));
        QVERIFY(Focus("otherItem"));
        Press(CYR_O, Qt::NoModifier, XKB_J, MAC_J, QStringLiteral("о"));
        Press(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QStringLiteral("j"));
        QCOMPARE(Counter_.Presses, 2);
        Press(Qt::Key_Period, CONTROL_KEY, XKB_SLASH, MAC_SLASH,
              QStringLiteral("."));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("sslash"), TIMEOUT_MS);
        QCOMPARE(Counter_.Presses, 2);
    }

    // Shift+4 types ";" on the Russian layout: the press is Ctrl+Shift+4, not
    // Ctrl+;.
    void CtrlShiftFourOnTheRussianLayoutIsNotCtrlSemicolon() {
        QVERIFY(Boot({Entry("Ctrl+Shift+4", "s4"), Entry("Ctrl+;", "ssemi")}));
        QVERIFY(Focus("otherItem"));
        Press(CYR_O, Qt::NoModifier, XKB_J, MAC_J, QStringLiteral("о"));
        Press(Qt::Key_Semicolon, CONTROL_KEY | Qt::ShiftModifier, XKB_4, MAC_4,
              QStringLiteral(";"));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("s4"), TIMEOUT_MS);
        QVERIFY(Flush());
        QVERIFY(!Ran("ssemi"));
        QCOMPARE(Counter_.Presses, 1);
    }

    // On Dvorak the "[" is at the US minus key: a Latin layout keeps the
    // character of a symbol key. The layout is Latin again after a Latin
    // letter, whatever the one before it was.
    void ASymbolKeyOnALatinLayoutMatchesByItsCharacter() {
        QVERIFY(Boot({Entry("Ctrl+[", "sbracket"), Entry("Ctrl+-", "sminus")}));
        QVERIFY(Focus("otherItem"));
        Press(CYR_O, Qt::NoModifier, XKB_J, MAC_J, QStringLiteral("о"));
        Press(Qt::Key_J, Qt::NoModifier, XKB_C, MAC_C, QStringLiteral("j"));
        QCOMPARE(Counter_.Presses, 2);
        Press(Qt::Key_BracketLeft, CONTROL_KEY, XKB_MINUS, MAC_MINUS,
              QStringLiteral("["));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("sbracket"), TIMEOUT_MS);
        QVERIFY(Flush());
        QVERIFY(!Ran("sminus"));
        QCOMPARE(Counter_.Presses, 2);
    }

    // The letter of the Russian layout still matches by its US position after
    // symbol keys were pressed.
    void ALetterOnTheRussianLayoutStillMatchesByPosition() {
        QVERIFY(Boot({Entry("Ctrl+J", "k1"), Entry("Ctrl+/", "sslash")}));
        QVERIFY(Focus("otherItem"));
        Press(CYR_O, Qt::NoModifier, XKB_J, MAC_J, QStringLiteral("о"));
        Press(Qt::Key_Period, CONTROL_KEY, XKB_SLASH, MAC_SLASH,
              QStringLiteral("."));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("sslash"), TIMEOUT_MS);
        Press(CYR_O, CONTROL_KEY, XKB_J, MAC_J, QStringLiteral("о"));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("k1"), TIMEOUT_MS);
    }

    // A letter with no Cmd, Ctrl or Alt is text where there is text: in the
    // terminal and in a field it goes to them, even when the table has it for
    // the panel; in a panel with no input it fires.
    void ABareLetterInTheTerminalGoesToTheTerminal() {
        QVERIFY(Boot({Entry("J", "sterm", "terminal.view"),
                      Entry("J", "slist", "sessions.list")}));
        QVERIFY(Focus("terminal"));
        Press(Qt::Key_J, Qt::NoModifier, XKB_J, MAC_J, QStringLiteral("j"));
        QCOMPARE(Counter_.Presses, 1);
        Press(Qt::Key_J, Qt::ShiftModifier, XKB_J, MAC_J, QStringLiteral("J"));
        QCOMPARE(Counter_.Presses, 2);
        QVERIFY(Flush());
        QVERIFY(!Ran("sterm"));

        QVERIFY(Focus("listField"));
        Press(Qt::Key_J, Qt::NoModifier, XKB_J, MAC_J, QStringLiteral("j"));
        QCOMPARE(Counter_.Presses, 1);
        QVERIFY(Flush());
        QVERIFY(!Ran("slist"));

        // The list without a field: the key is the list's.
        QVERIFY(Focus("listItem"));
        Press(Qt::Key_J, Qt::NoModifier, XKB_J, MAC_J, QStringLiteral("j"));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("slist"), TIMEOUT_MS);
        QCOMPARE(Counter_.Presses, 0);
        QVERIFY(!Ran("sterm"));
    }

    void AFunctionKeyFiresInTheTerminalToo() {
        QVERIFY(Boot({Entry("F5", "s5")}));
        QVERIFY(Focus("terminal"));
        Press(Qt::Key_F5, Qt::NoModifier, 0, 0, QString());
        QTRY_VERIFY_WITH_TIMEOUT(Ran("s5"), TIMEOUT_MS);
        QCOMPARE(Counter_.Presses, 0);
    }

    void ACtrlLetterFiresInTheTerminalAndInAField() {
        QVERIFY(Boot({Entry("Ctrl+J", "k1")}));
        QVERIFY(Focus("terminal"));
        Press(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QString(QChar(0x0A)));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("k1"), TIMEOUT_MS);
        QCOMPARE(Counter_.Presses, 0);
    }

    // The pair (key, when) of the panel in focus first, then (key, no when).
    void ANonMatchingWhenFallsThroughToTheEntryWithoutWhen() {
        QVERIFY(Boot({Entry("Ctrl+K", "swhen", "sessions.list"),
                      Entry("Ctrl+K", "sany")}));
        const auto press = [&] {
            Press(Qt::Key_K, CONTROL_KEY, XKB_K, MAC_K, QString(QChar(0x0B)));
        };
        QVERIFY(Focus("terminal"));
        press();
        QTRY_VERIFY_WITH_TIMEOUT(Ran("sany"), TIMEOUT_MS);
        QVERIFY(Flush());
        QVERIFY(!Ran("swhen"));
        // Another panel and no panel at all: the same.
        QVERIFY(Focus("otherItem"));
        press();
        QVERIFY(Focus("outsideItem"));
        press();
        QVERIFY(Flush());
        QVERIFY(!Ran("swhen"));
        // The panel the entry is for.
        QVERIFY(Focus("listItem"));
        press();
        QTRY_VERIFY_WITH_TIMEOUT(Ran("swhen"), TIMEOUT_MS);
        QCOMPARE(Counter_.Presses, 0);
    }

    void NothingIsMatchedWhileAnImeComposes() {
        QVERIFY(Boot({Entry("Ctrl+J", "k1")}));
        QVERIFY(Focus("otherItem"));
        QInputMethodEvent composing(QStringLiteral("に"), {});
        QCoreApplication::sendEvent(Window_.activeFocusItem(), &composing);
        Press(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QString(QChar(0x0A)));
        QCOMPARE(Counter_.Presses, 1);
        QVERIFY(Flush());
        QVERIFY(!Ran("k1"));
        // The composition is committed: the table is looked at again.
        QInputMethodEvent committed;
        committed.setCommitString(QStringLiteral("に"));
        QCoreApplication::sendEvent(Window_.activeFocusItem(), &committed);
        Press(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QString(QChar(0x0A)));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("k1"), TIMEOUT_MS);
        QCOMPARE(Counter_.Presses, 1);
    }

    // Ctrl++ and Ctrl+Shift+= are one physical key on the US layout: either
    // spelling in the table fires on the press of Shift+= with Ctrl, and
    // neither fires on the key without Shift.
    void BothSpellingsOfCtrlPlusFire_data() {
        QTest::addColumn<QString>("spelling");
        QTest::newRow("Plus") << "Ctrl++";
        QTest::newRow("ShiftEqual") << "Ctrl+Shift+=";
        QTest::newRow("ShiftPlus") << "Ctrl+Shift++";
    }

    void BothSpellingsOfCtrlPlusFire() {
        QFETCH(QString, spelling);
        QVERIFY(Boot({Entry(spelling, "splus")}));
        QVERIFY(Focus("otherItem"));
        // Without Shift the key is "=": another key.
        Press(Qt::Key_Equal, CONTROL_KEY, XKB_EQUAL, MAC_EQUAL,
              QStringLiteral("="));
        QCOMPARE(Counter_.Presses, 1);
        QVERIFY(Flush());
        QVERIFY(!Ran("splus"));
        Press(Qt::Key_Plus, CONTROL_KEY | Qt::ShiftModifier, XKB_EQUAL,
              MAC_EQUAL, QStringLiteral("+"));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("splus"), TIMEOUT_MS);
        QCOMPARE(Counter_.Presses, 1);
    }

    void PlainCtrlEqualIsNotCtrlPlus() {
        QVERIFY(Boot({Entry("Ctrl+=", "seq")}));
        QVERIFY(Focus("otherItem"));
        Press(Qt::Key_Plus, CONTROL_KEY | Qt::ShiftModifier, XKB_EQUAL,
              MAC_EQUAL, QStringLiteral("+"));
        QCOMPARE(Counter_.Presses, 1);
        Press(Qt::Key_Equal, CONTROL_KEY, XKB_EQUAL, MAC_EQUAL,
              QStringLiteral("="));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("seq"), TIMEOUT_MS);
        QCOMPARE(Counter_.Presses, 1);
    }

    // A repeat of a caught key does not run the command again, and neither it
    // nor the release reaches the item.
    void ARepeatAndTheReleaseOfACaughtKeyAreEaten() {
        QVERIFY(Boot({Entry("Ctrl+J", "k1")}));
        QVERIFY(Focus("otherItem"));
        QSignalSpy rows(&Api_->Sessions(), &api::SessionService::RowChanged);
        QSignalSpy inserted(&Api_->Sessions(),
                            &api::SessionService::RowInserted);
        Press(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QString(QChar(0x0A)));
        Press(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QString(QChar(0x0A)), true);
        Release(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QString(QChar(0x0A)));
        QVERIFY(Flush());
        QCOMPARE(inserted.size(), 2);  // k1 and the sentinel
        QCOMPARE(rows.size(), 0);      // k1 was not said twice
        QCOMPARE(Counter_.Presses, 0);
        // The release of a key that was not caught is not eaten.
        int releases = 0;
        ReleaseCounter probe;
        probe.Count = &releases;
        Window_.activeFocusItem()->installEventFilter(&probe);
        Release(Qt::Key_K, CONTROL_KEY, XKB_K, MAC_K, QString(QChar(0x0B)));
        QCOMPARE(releases, 1);
    }

    // The keys of a Popup or a Dialog are its own: an entry without `when`
    // does not fire under it, and fires again when it is closed.
    void NothingIsMatchedWhileTheFocusIsInAPopup() {
        QVERIFY(Boot({Entry("Ctrl+J", "k1")}));
        auto* popup = Root_->findChild<QObject*>("popup");
        QVERIFY(popup != nullptr);
        QVERIFY(QMetaObject::invokeMethod(popup, "open"));
        QTRY_VERIFY(popup->property("opened").toBool());
        QQuickItem* inside = Root_->findChild<QQuickItem*>("popupItem");
        QVERIFY(inside != nullptr);
        inside->forceActiveFocus();
        QTRY_COMPARE(Window_.activeFocusItem(), inside);
        Counter_.Presses = 0;
        inside->installEventFilter(&Counter_);
        Press(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QString(QChar(0x0A)));
        QCOMPARE(Counter_.Presses, 1);
        QVERIFY(Flush());
        QVERIFY(!Ran("k1"));
        QVERIFY(QMetaObject::invokeMethod(popup, "close"));
        QTRY_VERIFY(!popup->property("visible").toBool());
        QVERIFY(Focus("otherItem"));
        Press(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QString(QChar(0x0A)));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("k1"), TIMEOUT_MS);
    }

    // A key caught in one focus span is not held in the next: when the window
    // loses the focus between the press and the release, the release of the
    // same key in the terminal reaches the terminal.
    void AReleaseAfterTheFocusWindowChangedIsNotEaten() {
        QVERIFY(Boot({Entry("Ctrl+J", "k1")}));
        QVERIFY(Focus("terminal"));
        Press(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QString(QChar(0x0A)));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("k1"), TIMEOUT_MS);
        QQuickWindow other;
        other.resize(50, 50);
        other.show();
        QVERIFY(QTest::qWaitForWindowActive(&other));
        QTRY_VERIFY(QGuiApplication::focusWindow() != &Window_);
        Window_.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&Window_));
        QVERIFY(Focus("terminal"));
        int releases = 0;
        ReleaseCounter probe;
        probe.Count = &releases;
        Window_.activeFocusItem()->installEventFilter(&probe);
        Release(Qt::Key_J, Qt::NoModifier, XKB_J, MAC_J, QStringLiteral("j"));
        QCOMPARE(releases, 1);
    }

    // The release of a key caught in the same focus span is still eaten after
    // the focus moves to another item of the window (a command that moves it).
    void AReleaseAfterTheFocusItemChangedIsEaten() {
        QVERIFY(Boot({Entry("Ctrl+J", "k1")}));
        QVERIFY(Focus("otherItem"));
        Press(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QString(QChar(0x0A)));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("k1"), TIMEOUT_MS);
        QVERIFY(Focus("terminal"));
        int releases = 0;
        ReleaseCounter probe;
        probe.Count = &releases;
        Window_.activeFocusItem()->installEventFilter(&probe);
        Release(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QString(QChar(0x0A)));
        QCOMPARE(releases, 0);
    }

    // An input method cancelled by a move of the focus says nothing more: the
    // table is looked at again.
    void ACompositionEndedByAFocusChangeDoesNotSwitchTheTableOff() {
        QVERIFY(Boot({Entry("Ctrl+J", "k1")}));
        QVERIFY(Focus("otherItem"));
        QInputMethodEvent composing(QStringLiteral("に"), {});
        QCoreApplication::sendEvent(Window_.activeFocusItem(), &composing);
        QVERIFY(Focus("listItem"));
        Press(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QString(QChar(0x0A)));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("k1"), TIMEOUT_MS);
        QCOMPARE(Counter_.Presses, 0);
    }

    void NothingIsMatchedBeforeTheTableIsLoaded() {
        QVERIFY(Boot({Entry("Ctrl+J", "k1")}));
        QVERIFY(Focus("otherItem"));
        QVERIFY(Daemon_->Stop());
        QTRY_VERIFY_WITH_TIMEOUT(!Api_->Keymap().IsLoaded(), TIMEOUT_MS);
        Press(Qt::Key_J, CONTROL_KEY, XKB_J, MAC_J, QString(QChar(0x0A)));
        QCOMPARE(Counter_.Presses, 1);
    }

#ifdef Q_OS_MACOS
    void CommandIsTheCommandKeyOnMacOS() {
        QVERIFY(Boot({QJsonObject {
            {"key", "Ctrl+Shift+Y"},
            {"mac", "Cmd+J"},
            {"command", "fixture.notify"},
            { "args",
              Appears("k1") }
        }}));
        QVERIFY(Focus("otherItem"));
        // The Control key is not Command.
        Press(Qt::Key_J, Qt::MetaModifier, XKB_J, MAC_J, QStringLiteral("j"));
        QCOMPARE(Counter_.Presses, 1);
        QVERIFY(Flush());
        QVERIFY(!Ran("k1"));
        // Qt calls the Command key Control.
        Press(Qt::Key_J, Qt::ControlModifier, XKB_J, MAC_J,
              QStringLiteral("j"));
        QTRY_VERIFY_WITH_TIMEOUT(Ran("k1"), TIMEOUT_MS);
        QCOMPARE(Counter_.Presses, 1);
    }
#else
    void SuperIsNotMatchedOnLinux() {
        QVERIFY(Boot({Entry("Ctrl+J", "k1")}));
        QVERIFY(Focus("otherItem"));
        Press(Qt::Key_J, Qt::ControlModifier | Qt::MetaModifier, XKB_J, MAC_J,
              QStringLiteral("j"));
        QCOMPARE(Counter_.Presses, 1);
        QVERIFY(Flush());
        QVERIFY(!Ran("k1"));
    }
#endif
};

QTEST_MAIN(KeymapMatchTest)
#include "keymap_match_test.moc"
