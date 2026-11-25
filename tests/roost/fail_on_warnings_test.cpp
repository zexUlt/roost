// `FailOnWarnings` of the window tests lets through the warning that Qt's own
// animation timer prints when a pause animation (every ListView has one)
// comes right after the timer of the QML animations was emptied. The sequence
// is made here with three pause animations and the order of the queued
// events, with no window and no timing.

#include <QCoreApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QTest>

#include <memory>

#include "helpers/sessions_window.hpp"
#include "helpers/warning_filter.hpp"

namespace {

QObject* Animation(QObject* root, const char* name) {
    return root->property(name).value<QObject*>();
}

}  // namespace

class FailOnWarningsTest : public QObject {
    Q_OBJECT

    // The events that Qt's timers queue, in the order of the calls below:
    //   1. a0, the only running animation, stops: the timer of the QML
    //      animations is empty and posts its stop;
    //   2. a2 will start from a queued call, after that stop;
    //   3. a1 starts and stops at once: it posts the start of the animations
    //      of the timer, and the list of that start is empty again, so the
    //      stop of the timer goes on and posts the stop of the driver.
    // The queued start of a2 runs between them and joins the list of the
    // pending start without posting. That start finds a pause alone (a2) and
    // stops the driver, and the stop of the driver posted before finds it
    // stopped.
    static bool Replay(QObject* root) {
        QObject* a0 = Animation(root, "a0");
        QObject* a1 = Animation(root, "a1");
        QObject* a2 = Animation(root, "a2");
        if (a0 == nullptr || a1 == nullptr || a2 == nullptr) {
            return false;
        }
        QMetaObject::invokeMethod(a0, "start");
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QMetaObject::invokeMethod(a0, "stop");
        QMetaObject::invokeMethod(a2, "start", Qt::QueuedConnection);
        QMetaObject::invokeMethod(a1, "start");
        QMetaObject::invokeMethod(a1, "stop");
        // The queue drains with no waiting: every event is already posted.
        for (int i = 0; i < 4; ++i) {
            QCoreApplication::processEvents();
        }
        QMetaObject::invokeMethod(a2, "stop");
        for (int i = 0; i < 4; ++i) {
            QCoreApplication::processEvents();
        }
        return true;
    }

    std::unique_ptr<QObject> Make() {
        Engine_ = std::make_unique<QQmlEngine>();
        QQmlComponent component(Engine_.get());
        component.setData(R"(
            import QtQuick
            Item {
                property var a0: PauseAnimation { duration: 600000 }
                property var a1: PauseAnimation { duration: 600000 }
                property var a2: PauseAnimation { duration: 600000 }
            })",
                          QUrl());
        std::unique_ptr<QObject> root(component.create());
        if (!root) {
            qWarning() << component.errorString();
        }
        return root;
    }

    std::unique_ptr<QQmlEngine> Engine_;

private slots:
    void cleanup() { Engine_.reset(); }

    void WarningFilterMatchesWhatFailsTheTest_data() {
        QTest::addColumn<QString>("message");
        QTest::addColumn<bool>("fails");
        QTest::newRow("font database with its tail")
            << "Populating font family aliases took 12 ms." << false;
        QTest::newRow("animation driver, the whole message")
            << "QUnifiedTimer::stopAnimationDriver: driver is not running"
            << false;
        QTest::newRow("animation driver with a tail")
            << "QUnifiedTimer::stopAnimationDriver: driver is not running "
               "(id=1)"
            << true;
        QTest::newRow("another animation driver message")
            << "QUnifiedTimer::stopAnimationDriver: driver is already running"
            << true;
        QTest::newRow("anything else") << "qml: something" << true;
        QTest::newRow("allowed message on the second line")
            << "qml: something\n"
               "QUnifiedTimer::stopAnimationDriver: driver is not running"
            << true;
    }

    void WarningFilterMatchesWhatFailsTheTest() {
        QFETCH(QString, message);
        QFETCH(bool, fails);
        QCOMPARE(message.contains(roost::testing::WarningFilter()), fails);
    }

    // Qt does warn: when a Qt release stops it, the filter can go.
    void QtWarnsOnTheSequence() {
        const std::unique_ptr<QObject> root = Make();
        QVERIFY(root);
        QTest::ignoreMessage(
            QtWarningMsg,
            "QUnifiedTimer::stopAnimationDriver: driver is not running");
        QVERIFY(Replay(root.get()));
    }

    void FailOnWarningsLetsTheWarningThrough() {
        const std::unique_ptr<QObject> root = Make();
        QVERIFY(root);
        roost::testing::FailOnWarnings();
        QVERIFY(Replay(root.get()));
    }
};

QTEST_MAIN(FailOnWarningsTest)
#include "fail_on_warnings_test.moc"
