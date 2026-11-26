// The "About roost" window (AboutWindow.qml), in the window of the shell
// (Main.qml), offscreen and without roostd: the command shell.about, the
// toolbar button (the first item of the roost menu on macOS) open the one
// window, which shows the version of this build (the lines of about.txt: one
// source, VersionLines()), Copy puts those lines on the clipboard, Escape and
// the Close button close it. The link is never opened: opensUrls is off.

#include <QClipboard>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>

#include <memory>

#include "api/api.hpp"
#include "api/api_test_support.hpp"
#include "core/version.hpp"
#include "helpers/warning_filter.hpp"
#include "qml_api/roost_singleton.hpp"
#include "roost/bug_report.hpp"
#include "roost/controls_style.hpp"

using namespace roost;
using namespace roost::api_test;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

// Image.Ready of QtQuick, whose enum is in a private header.
constexpr int IMAGE_READY = 1;

}  // namespace

class AboutWindowTest : public QObject {
    Q_OBJECT

private:
    QQuickItem* Item(const char* name) {
        return About_->findChild<QQuickItem*>(name);
    }

    QString Text(const char* name) {
        QQuickItem* item = Item(name);
        return item != nullptr ? item->property("text").toString() : QString();
    }

    // Opens the window by the function of Main.qml and waits until it is
    // exposed.
    void Open() {
        QVERIFY(QMetaObject::invokeMethod(Main_, "openAbout"));
        QVERIFY(QTest::qWaitForWindowExposed(About_));
    }

    // The engine goes before the api it uses.
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<QQmlApplicationEngine> Engine_;
    QQuickWindow* Main_ = nullptr;
    QQuickWindow* About_ = nullptr;

private slots:
    // The style of the window, as main() selects it, before the first engine.
    void initTestCase() { roost::SelectControlsStyle(); }

    void init() {
        Api_ = std::make_unique<api::Api>();
        Engine_ = std::make_unique<QQmlApplicationEngine>();
        auto* singleton =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(singleton != nullptr);
        singleton->setApi(*Api_);
        Engine_->loadFromModule("Roost.Shell", "Main");
        QVERIFY(!Engine_->rootObjects().isEmpty());
        Main_ = qobject_cast<QQuickWindow*>(Engine_->rootObjects().first());
        QVERIFY(Main_ != nullptr);
        About_ = Main_->findChild<QQuickWindow*>("aboutWindow");
        QVERIFY(About_ != nullptr);
        About_->setProperty("opensUrls", false);
        QVERIFY(!About_->isVisible());
    }

    void cleanup() {
        Engine_.reset();
        Api_.reset();
        Main_ = nullptr;
        About_ = nullptr;
    }

    // The command, and the toolbar button (the menu item on macOS), show the
    // one window; a second open shows the same one, not another.
    void TheCommandAndTheButtonOpenTheOneWindow() {
        QTest::failOnWarning(testing::WarningFilter());
        const QFuture<rpc::Result> run = Api_->Commands().Run("shell.about");
        QVERIFY(WaitFor(run));
        QVERIFY2(run.result(), qPrintable(Why(run.result())));
        QVERIFY(About_->isVisible());

        // The toolbar button is in the window on every platform (it shows
        // off macOS only); the menu item is macOS's.
        auto* button = Main_->findChild<QQuickItem*>("aboutButton");
        QVERIFY(button != nullptr);
        QCOMPARE(button->property("text").toString(),
                 QString::fromUtf8("About…"));
        About_->hide();
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QVERIFY(About_->isVisible());
#ifdef Q_OS_MACOS
        About_->hide();
        QObject* item = Main_->findChild<QObject*>("aboutMenuItem");
        QVERIFY(item != nullptr);
        QCOMPARE(item->property("text").toString(), QString("About roost"));
        QVERIFY(QMetaObject::invokeMethod(item, "triggered"));
#endif
        QVERIFY(About_->isVisible());
        // Open again while it is shown: the same window. The offscreen
        // platform has no raise(), and says so.
        QTest::ignoreMessage(QtWarningMsg,
                             "This plugin does not support raise()");
        QVERIFY(QMetaObject::invokeMethod(Main_, "openAbout"));
        QCOMPARE(Main_->findChildren<QQuickWindow*>("aboutWindow").size(), 1);
        QCOMPARE(Main_->findChild<QQuickWindow*>("aboutWindow"), About_);
        QVERIFY(About_->isVisible());
    }

    // The name, the version of this build, the libraries on a muted line, the
    // licence and the link; the icon is the one of the application.
    void TheWindowShowsTheVersionOfThisBuild() {
        QTest::failOnWarning(testing::WarningFilter());
        Open();
        QCOMPARE(About_->property("title").toString(), QString("About roost"));
        QCOMPARE(Text("aboutName"), QString("roost"));
        QCOMPARE(Text("aboutVersion"), QString::fromStdString(RoostVersion()));
        QCOMPARE(Text("aboutLibraries"),
                 QString("libghostty-vt %1, Qt %2")
                     .arg(QString::fromStdString(GhosttyVtVersion()),
                          QString::fromLatin1(qVersion())));
        QCOMPARE(Text("aboutLicense"), QString("Apache-2.0"));
        QVERIFY(Text("aboutLink").contains("https://github.com/zexUlt/roost"));
        QTRY_COMPARE_WITH_TIMEOUT(Item("aboutIcon")->property("status").toInt(),
                                  IMAGE_READY, TIMEOUT_MS);
    }

    // "Project page" opens the project page, by a click and by the keyboard;
    // the test does not open the browser.
    void TheProjectPageButtonOpensTheProjectPage() {
        QTest::failOnWarning(testing::WarningFilter());
        Open();
        QVERIFY(QTest::qWaitForWindowActive(About_));
        QSignalSpy opened(About_, SIGNAL(linkOpened(QUrl)));
        QQuickItem* button = Item("aboutPageButton");
        QVERIFY(button != nullptr);
        QCOMPARE(button->property("text").toString(), QString("Project page"));
        QTest::mouseClick(
            About_, Qt::LeftButton, {},
            button
                ->mapToScene(QPointF(button->width() / 2, button->height() / 2))
                .toPoint());
        QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, TIMEOUT_MS);
        QCOMPARE(opened.first().at(0).toUrl(),
                 QUrl("https://github.com/zexUlt/roost"));
        // Tab reaches the button, Space presses it.
        for (int i = 0; i < 4 && !button->hasActiveFocus(); ++i) {
            QTest::keyClick(About_, Qt::Key_Tab);
        }
        QVERIFY(button->hasActiveFocus());
        QTest::keyClick(About_, Qt::Key_Space);
        QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 2, TIMEOUT_MS);
    }

    // At a large window font the lines wrap inside the window, none is wider.
    void TheLinesWrapAtALargeFont() {
        QTest::failOnWarning(testing::WarningFilter());
        Open();
        QFont font = About_->property("font").value<QFont>();
        font.setPointSizeF(36);
        About_->setProperty("font", font);
        for (const char* name :
             {"aboutVersion", "aboutLibraries", "aboutLicense", "aboutLink"}) {
            QQuickItem* line = Item(name);
            QVERIFY(line != nullptr);
            line->setProperty("font", font);
            QTRY_VERIFY2_WITH_TIMEOUT(
                line->mapToScene(QPointF(line->width(), 0)).x() <=
                        About_->width() &&
                    line->property("contentWidth").toReal() <= line->width(),
                name, TIMEOUT_MS);
        }
        // The buttons do not wrap: the window widens for their row.
        for (const char* name :
             {"aboutPageButton", "aboutCopyButton", "aboutCloseButton"}) {
            Item(name)->setProperty("font", font);
        }
        QQuickItem* close = Item("aboutCloseButton");
        QTRY_VERIFY_WITH_TIMEOUT(
            close->mapToScene(QPointF(close->width(), 0)).x() <=
                About_->width(),
            TIMEOUT_MS);
    }

    // Copy puts the lines of about.txt that name the versions, and only them,
    // on the clipboard: the same text as the report starts with.
    void CopyPutsTheVersionLinesOnTheClipboard() {
        QTest::failOnWarning(testing::WarningFilter());
        Open();
        QClipboard* clipboard = QGuiApplication::clipboard();
        clipboard->setText("kept");
        QQuickItem* button = Item("aboutCopyButton");
        QVERIFY(button != nullptr);
        QCOMPARE(button->property("text").toString(), QString("Copy"));
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        const QString copied = clipboard->text();
        QCOMPARE(copied, VersionLines());
        QVERIFY(copied.startsWith(
            "roost: " + QString::fromStdString(VersionString()) + "\n"));
        QVERIFY(copied.contains("\nQt (runtime): "));
        QCOMPARE(copied.count('\n'), 2);
        QVERIFY(AboutText("/s", {}).startsWith(copied));
    }

    // Escape and the Close button close the window.
    void EscapeAndTheCloseButtonCloseTheWindow() {
        QTest::failOnWarning(testing::WarningFilter());
        Open();
        QVERIFY(QTest::qWaitForWindowActive(About_));
        QTest::keyClick(About_, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!About_->isVisible(), TIMEOUT_MS);

        Open();
        QQuickItem* button = Item("aboutCloseButton");
        QVERIFY(button != nullptr);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QVERIFY(!About_->isVisible());
    }
};

QTEST_MAIN(AboutWindowTest)
#include "about_window_test.moc"
