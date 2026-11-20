// The "Report a Bug…" dialog in the window of the shell (Main.qml), offscreen:
// the toolbar button (the Help menu on macOS) and the shortcut open it, it
// takes a description and writes the zip into a temporary directory. Show and
// Open are never pressed. The logs and the settings are the ones of temporary
// directories (ROOST_SOCKET, ROOST_CONFIG); HOME and Downloads are not touched.

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "helpers/scoped_env.hpp"
#include "qml_api/terminal_item.hpp"

using namespace roost;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

class BugReportDialogTest : public QObject {
    Q_OBJECT

private:
    // The item is read-only text that selects with the mouse and keyboard,
    // and selectAll() takes `expected`.
    static void ExpectSelectable(QQuickItem* item, const QString& expected) {
        QVERIFY(item->property("readOnly").toBool());
        QVERIFY(item->property("selectByMouse").toBool());
        QVERIFY(item->property("selectByKeyboard").toBool());
        QVERIFY(QMetaObject::invokeMethod(item, "selectAll"));
        QVERIFY2(item->property("selectedText").toString().contains(expected),
                 qPrintable(item->property("selectedText").toString()));
    }

    // The Repeater's delegates are children of the item, not of the QObject
    // tree findChild() walks.
    static QQuickItem* VisualChild(QQuickItem* parent, const QString& name) {
        for (QQuickItem* child : parent->childItems()) {
            if (child->objectName() == name) {
                return child;
            }
            if (QQuickItem* found = VisualChild(child, name)) {
                return found;
            }
        }
        return nullptr;
    }

    QObject* Dialog() {
        return Window_->findChild<QObject*>("bugReportDialog");
    }

    QTemporaryDir Dir_;
    std::unique_ptr<testing::ScopedEnv> Socket_;
    std::unique_ptr<testing::ScopedEnv> Config_;
    std::unique_ptr<QQmlApplicationEngine> Engine_;
    QQuickWindow* Window_ = nullptr;

private slots:
    void init() {
        QVERIFY(Dir_.isValid());
        QVERIFY(QDir().mkpath(Dir_.filePath("run")));
        QVERIFY(QDir().mkpath(Dir_.filePath("config")));
        Socket_ = std::make_unique<testing::ScopedEnv>(
            "ROOST_SOCKET", Dir_.filePath("run/roostd.sock").toUtf8());
        Config_ = std::make_unique<testing::ScopedEnv>(
            "ROOST_CONFIG", Dir_.filePath("config").toUtf8());
        QFile log(Dir_.filePath("run/roostd.log"));
        QVERIFY(log.open(QIODevice::WriteOnly));
        log.write("roostd says hello\n");
        log.close();
        Engine_ = std::make_unique<QQmlApplicationEngine>();
        Engine_->loadFromModule("Roost.Shell", "Main");
        QVERIFY(!Engine_->rootObjects().isEmpty());
        Window_ = qobject_cast<QQuickWindow*>(Engine_->rootObjects().first());
        QVERIFY(Window_ != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(Window_));
        QVERIFY(Dialog() != nullptr);
        QVERIFY(!Dialog()->property("visible").toBool());
    }

    void cleanup() {
        Engine_.reset();
        Window_ = nullptr;
        Config_.reset();
        Socket_.reset();
    }

    // Linux: the toolbar button. macOS: no toolbar, the Help menu item.
    void TheToolbarOrTheHelpMenuOpensTheDialog() {
#ifdef Q_OS_MACOS
        auto* toolBar = Window_->findChild<QQuickItem*>("toolBar");
        QVERIFY(toolBar != nullptr);
        QVERIFY(!toolBar->isVisible());
        QObject* item = Engine_->rootObjects().first()->findChild<QObject*>(
            "reportBugMenuItem");
        QVERIFY(item != nullptr);
        QCOMPARE(item->property("text").toString(),
                 QString::fromUtf8("Report a Bug…"));
        QVERIFY(QMetaObject::invokeMethod(item, "triggered"));
        QTRY_VERIFY(Dialog()->property("visible").toBool());
#else
        auto* button = Window_->findChild<QQuickItem*>("reportBugButton");
        QVERIFY(button != nullptr);
        QCOMPARE(button->property("text").toString(),
                 QString::fromUtf8("Report a Bug…"));
        const QPoint center =
            button
                ->mapToScene(QPointF(button->width() / 2, button->height() / 2))
                .toPoint();
        QTest::mouseClick(Window_, Qt::LeftButton, Qt::NoModifier, center);
        QTRY_VERIFY(Dialog()->property("visible").toBool());
#endif
    }

    // macOS only (Cmd+Shift+B); elsewhere the key is the terminal program's.
    // With the keyboard in a terminal: TerminalItem does not take the key.
    void TheShortcutIsMacOnly() {
        auto* terminal = new TerminalItem(Window_->contentItem());
        terminal->setSize(QSizeF(100, 100));
        terminal->forceActiveFocus();
        QVERIFY(terminal->hasActiveFocus());
        QTest::keyClick(Window_, Qt::Key_B,
                        Qt::ControlModifier | Qt::ShiftModifier);
#ifdef Q_OS_MACOS
        QTRY_VERIFY(Dialog()->property("visible").toBool());
#else
        QTest::qWait(200);
        QVERIFY(!Dialog()->property("visible").toBool());
#endif
    }

    // The texts of the dialog can be selected and copied: read-only text
    // with the mouse selection, not Labels.
    void TheShownTextsAreSelectable() {
        QMetaObject::invokeMethod(Window_, "reportBug");
        QTRY_VERIFY(Dialog()->property("visible").toBool());

        QQuickItem* entry = nullptr;
        QTRY_VERIFY((entry = VisualChild(
                         Dialog()->property("contentItem").value<QQuickItem*>(),
                         "fileEntry")));
        ExpectSelectable(entry, entry->property("text").toString());

        Dialog()->setProperty("outputDirectory", Dir_.filePath("selectable"));
        QVERIFY(QMetaObject::invokeMethod(
            Window_->findChild<QObject*>("createButton"), "clicked"));
        QVERIFY(Dialog()->property("done").toBool());
        const QString zip = Dialog()
                                ->property("report")
                                .value<QObject*>()
                                ->property("zipPath")
                                .toString();
        QVERIFY(!zip.isEmpty());
        auto* saved = Window_->findChild<QQuickItem*>("zipPathText");
        QVERIFY(saved != nullptr);
        ExpectSelectable(saved, zip);
    }

    void TheDialogListsTheFilesAndWritesTheZip() {
        QMetaObject::invokeMethod(Window_, "reportBug");
        QTRY_VERIFY(Dialog()->property("visible").toBool());
        // about.txt and description.txt are always there; the log is listed.
        const QVariantList files = Dialog()->property("fileList").toList();
        QCOMPARE(files.size(), 1);
        QVERIFY(files.first().toMap().value("text").toString().startsWith(
            "logs/roostd.log ("));

        const QString output = Dir_.filePath("out");
        Dialog()->setProperty("outputDirectory", output);
        const QString description =
            QString::fromUtf8("Не открывается сессия ✓");
        Dialog()->setProperty("description", description);
        QVERIFY(QMetaObject::invokeMethod(
            Window_->findChild<QObject*>("createButton"), "clicked"));
        QVERIFY(Dialog()->property("done").toBool());

        auto* report = Dialog()->property("report").value<QObject*>();
        QVERIFY(report != nullptr);
        const QString zip = report->property("zipPath").toString();
        QVERIFY2(zip.startsWith(output + "/roost-bug-") && zip.endsWith(".zip"),
                 qPrintable(zip));
        QVERIFY(QFile::exists(zip));
        QCOMPARE(Window_->findChild<QQuickItem*>("zipPathText")
                     ->property("text")
                     .toString(),
                 "Saved: " + zip);
        QVERIFY(Window_->findChild<QQuickItem*>("showButton")->isVisible());
        QVERIFY(Window_->findChild<QQuickItem*>("issueButton")->isVisible());

        const QString unzip = QStandardPaths::findExecutable("unzip");
        if (unzip.isEmpty()) {
            QSKIP(
                "unzip is not installed: the zip is written, its content is "
                "not checked");
        }
        QProcess process;
        process.start(unzip, {"-p", zip, "*/description.txt"});
        QVERIFY(process.waitForFinished(30000));
        QCOMPARE(QString::fromUtf8(process.readAllStandardOutput()),
                 description);
        process.start(unzip, {"-p", zip, "*/logs/roostd.log"});
        QVERIFY(process.waitForFinished(30000));
        QCOMPARE(process.readAllStandardOutput(),
                 QByteArray("roostd says hello\n"));
        process.start(unzip, {"-t", zip});
        QVERIFY(process.waitForFinished(30000));
        QCOMPARE(process.exitCode(), 0);
        QStringList left = QDir(output).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        QCOMPARE(left.size(), 1);
    }

    void TheSettingsBoxAddsTheStoreFiles() {
        QFile store(Dir_.filePath("config/sessions.json"));
        QVERIFY(store.open(QIODevice::WriteOnly));
        store.write("{}");
        store.close();
        QMetaObject::invokeMethod(Window_, "reportBug");
        QTRY_VERIFY(Dialog()->property("visible").toBool());
        QCOMPARE(Dialog()->property("fileList").toList().size(), 1);
        Dialog()->setProperty("includeSettings", true);
        QVERIFY(QMetaObject::invokeMethod(
            Window_->findChild<QObject*>("includeSettingsBox"), "toggled"));
        QCOMPARE(Dialog()->property("fileList").toList().size(), 2);
    }

    void AFailedCreateShowsTheErrorAndNoZip() {
        QMetaObject::invokeMethod(Window_, "reportBug");
        QTRY_VERIFY(Dialog()->property("visible").toBool());
        QFile blocker(Dir_.filePath("blocker"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        Dialog()->setProperty("outputDirectory", Dir_.filePath("blocker/out"));
        QVERIFY(QMetaObject::invokeMethod(
            Window_->findChild<QObject*>("createButton"), "clicked"));
        QVERIFY(!Dialog()->property("done").toBool());
        QVERIFY(!Window_->findChild<QQuickItem*>("errorText")
                     ->property("text")
                     .toString()
                     .isEmpty());
    }

    void AReopenedDialogShowsNoOldError() {
        QMetaObject::invokeMethod(Window_, "reportBug");
        QTRY_VERIFY(Dialog()->property("visible").toBool());
        QFile blocker(Dir_.filePath("blocker"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        Dialog()->setProperty("outputDirectory", Dir_.filePath("blocker/out"));
        QVERIFY(QMetaObject::invokeMethod(
            Window_->findChild<QObject*>("createButton"), "clicked"));
        auto* errorText = Window_->findChild<QQuickItem*>("errorText");
        QVERIFY(errorText->isVisible());
        QVERIFY(!errorText->property("text").toString().isEmpty());

        QVERIFY(QMetaObject::invokeMethod(Dialog(), "close"));
        QTRY_VERIFY(!Dialog()->property("visible").toBool());
        QMetaObject::invokeMethod(Window_, "reportBug");
        QTRY_VERIFY(Dialog()->property("visible").toBool());
        QVERIFY(errorText->property("text").toString().isEmpty());
        QVERIFY(!errorText->isVisible());
    }
};

QTEST_MAIN(BugReportDialogTest)
#include "bug_report_dialog_test.moc"
