// Roost.bindingKey(event): the Qt key a binding matches for a QML KeyEvent.
// An inline QML handler in a real QQuickWindow on the offscreen platform stores
// what the call returns for a QKeyEvent with explicit native codes. Both
// platforms' codes are given, so a row passes on every runner; the macOS-only
// facts are under Q_OS_MACOS. No roostd: the call has nothing behind it.

#include <QKeyEvent>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <memory>

#include "qml_api/roost_singleton.hpp"

using namespace roost;

// The static plugin of module Roost.Api, linked into this executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)

namespace {

constexpr int CYR_O = 0x041E;
constexpr int CYR_BE = 0x0411;
constexpr int CYR_YA = 0x042F;

// The J key: kVK_ANSI_J, and KEY_J + 8.
constexpr quint32 MAC_J = 0x26;
constexpr quint32 XKB_J = 44;
// The comma key: kVK_ANSI_Comma, and KEY_COMMA + 8.
constexpr quint32 MAC_COMMA = 0x2B;
constexpr quint32 XKB_COMMA = 59;
// The slash key: kVK_ANSI_Slash, and KEY_SLASH + 8.
constexpr quint32 MAC_SLASH = 0x2C;
constexpr quint32 XKB_SLASH = 61;

// What bindingKey gives for the event of the last key press.
const char* const QML = R"(
import QtQuick
import Roost.Api

Item {
    property int got: -1
    property int presses: 0
    focus: true
    Keys.onPressed: event => {
        got = Roost.bindingKey(event)
        presses += 1
        event.accepted = true
    }
}
)";

}  // namespace

class RoostBindingKeyTest : public QObject {
    Q_OBJECT

private:
    // A press with the codes of both platforms, as the event of a real
    // keyboard carries those of its own.
    int Press(int key, Qt::KeyboardModifiers modifiers, quint32 xkb,
              quint32 mac, quint32 nativeModifiers, const QString& text) {
        const int before = Item_->property("presses").toInt();
        QKeyEvent event(QEvent::KeyPress, key, modifiers, xkb, mac,
                        nativeModifiers, text);
        event.setAccepted(false);
        QCoreApplication::sendEvent(&Window_, &event);
        if (Item_->property("presses").toInt() != before + 1) {
            return -2;
        }
        return Item_->property("got").toInt();
    }

    // A release with the codes of both platforms; no handler sees it.
    void Release(int key, Qt::KeyboardModifiers modifiers, quint32 xkb,
                 quint32 mac, quint32 nativeModifiers, const QString& text) {
        QKeyEvent event(QEvent::KeyRelease, key, modifiers, xkb, mac,
                        nativeModifiers, text);
        QCoreApplication::sendEvent(&Window_, &event);
    }

private slots:
    void init() {
        // QML creates a singleton at its first use, which would be this very
        // key handler, after the application filter was to see the event. The
        // window of roost uses Roost from its start.
        Singleton_ =
            Engine_.singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(Singleton_ != nullptr);
        QQmlComponent component(&Engine_);
        component.setData(QML, QUrl());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        Item_.reset(qobject_cast<QQuickItem*>(component.create()));
        QVERIFY(Item_ != nullptr);
        Item_->setParentItem(Window_.contentItem());
        Item_->setSize(QSizeF(100, 100));
        Window_.resize(100, 100);
        Window_.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Window_));
        QVERIFY(QTest::qWaitForWindowActive(&Window_));
        Item_->forceActiveFocus();
        QVERIFY(QTest::qWaitFor([&] { return Item_->hasActiveFocus(); }));
    }

    void cleanup() {
        Window_.hide();
        Item_.reset();
    }

    void RussianJIsJ() {
        QCOMPARE(Press(CYR_O, Qt::NoModifier, XKB_J, MAC_J, 0x100,
                       QStringLiteral("о")),
                 static_cast<int>(Qt::Key_J));
    }

    void LatinIsKept() {
        // A Dvorak "z" sits at the position of the US slash.
        QCOMPARE(Press(Qt::Key_Z, Qt::NoModifier, XKB_SLASH, MAC_SLASH, 0x100,
                       QStringLiteral("z")),
                 static_cast<int>(Qt::Key_Z));
    }

    void ShiftedSymbol() {
        QCOMPARE(Press(CYR_BE, Qt::ShiftModifier, XKB_COMMA, MAC_COMMA, 0x102,
                       QStringLiteral("Б")),
                 static_cast<int>(Qt::Key_Less));
        QCOMPARE(Press(CYR_BE, Qt::NoModifier, XKB_COMMA, MAC_COMMA, 0x100,
                       QStringLiteral("б")),
                 static_cast<int>(Qt::Key_Comma));
    }

    void NoCodes() {
        // A synthetic event carries no codes: the Qt key stays.
        QCOMPARE(Press(CYR_O, Qt::NoModifier, 0, 0, 0, QStringLiteral("о")),
                 CYR_O);
#ifdef Q_OS_MACOS
        // kVK_ANSI_A is 0: a real macOS event of the A key carries it with
        // its modifier flags, which a synthetic event does not.
        QCOMPARE(
            Press(CYR_YA, Qt::NoModifier, 0, 0, 0x100, QStringLiteral("ф")),
            static_cast<int>(Qt::Key_A));
#else
        // xkb codes are the scan code: 0 is no key.
        QCOMPARE(
            Press(CYR_YA, Qt::NoModifier, 0, 0, 0x100, QStringLiteral("ф")),
            CYR_YA);
#endif
    }

    void ReleaseThenPressOfAnotherKey() {
        // The release of the previous key ("о" at J) was the last event seen;
        // the press of the next key brings its own codes.
        QCOMPARE(Press(CYR_O, Qt::NoModifier, XKB_J, MAC_J, 0x100,
                       QStringLiteral("о")),
                 static_cast<int>(Qt::Key_J));
        Release(CYR_O, Qt::NoModifier, XKB_J, MAC_J, 0x100,
                QStringLiteral("о"));
        QCOMPARE(Press(CYR_BE, Qt::NoModifier, XKB_COMMA, MAC_COMMA, 0x100,
                       QStringLiteral("б")),
                 static_cast<int>(Qt::Key_Comma));
    }

    void RecordedReleaseDoesNotLeakToAnotherKey() {
        Release(CYR_O, Qt::NoModifier, XKB_J, MAC_J, 0x100,
                QStringLiteral("о"));
        // The recorded release is of the key "о"; an event of another key
        // keeps its Qt key.
        QObject otherKey;
        otherKey.setProperty("key", CYR_BE);
        otherKey.setProperty("modifiers", 0);
        QCOMPARE(Singleton_->bindingKey(&otherKey), CYR_BE);
        // A QML press of a Latin key after the release of "о" is not J.
        QCOMPARE(Press(Qt::Key_Z, Qt::NoModifier, XKB_SLASH, MAC_SLASH, 0x100,
                       QStringLiteral("z")),
                 static_cast<int>(Qt::Key_Z));
    }

    void CodesOfAnotherEventAreNotUsed() {
        // The recorded event (Cyrillic "о" at J) is not the KeyEvent that the
        // call gets: the call has the Qt key and modifiers of another one.
        QKeyEvent event(QEvent::KeyPress, CYR_O, Qt::NoModifier, XKB_J, MAC_J,
                        0x100, QStringLiteral("о"));
        QCoreApplication::sendEvent(&Window_, &event);

        QObject same;
        same.setProperty("key", CYR_O);
        same.setProperty("modifiers", 0);
        QCOMPARE(Singleton_->bindingKey(&same), static_cast<int>(Qt::Key_J));

        QObject otherKey;
        otherKey.setProperty("key", CYR_BE);
        otherKey.setProperty("modifiers", 0);
        QCOMPARE(Singleton_->bindingKey(&otherKey), CYR_BE);

        QObject otherModifiers;
        otherModifiers.setProperty("key", CYR_O);
        otherModifiers.setProperty("modifiers",
                                   static_cast<int>(Qt::ShiftModifier));
        QCOMPARE(Singleton_->bindingKey(&otherModifiers), CYR_O);
    }

private:
    QQmlEngine Engine_;
    RoostSingleton* Singleton_ = nullptr;
    QQuickWindow Window_;
    std::unique_ptr<QQuickItem> Item_;
};

QTEST_MAIN(RoostBindingKeyTest)
#include "roost_binding_key_test.moc"
