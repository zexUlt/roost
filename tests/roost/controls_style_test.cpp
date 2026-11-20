// The Qt Quick Controls style of the window is Fusion, and the palette of the
// window comes from Roost.theme: a Button of the connection banner (Main.qml)
// and the Report a Bug dialog take their colours from the tokens, and a token
// that changes recolours them, painted. The window is the real Main.qml,
// offscreen; no roostd is started (ROOST_SOCKET and ROOST_CONFIG are
// temporary).

#include <QColor>
#include <QDir>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlExtensionPlugin>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "helpers/scoped_env.hpp"
#include "qml_api/roost_singleton.hpp"
#include "qml_api/theme.hpp"
#include "roost/controls_style.hpp"

using namespace roost;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

class ControlsStyleTest : public QObject {
    Q_OBJECT

private:
    // The palette of a Control or a Popup is a QPalette; that of a Window is a
    // QQuickPalette (read through the QML property).
    static QColor PaletteOf(QObject* object, QPalette::ColorRole role) {
        static const QHash<QPalette::ColorRole, const char*> NAMES{
            {QPalette::Window, "window"},
            {QPalette::WindowText, "windowText"},
            {QPalette::Base, "base"},
            {QPalette::Button, "button"},
            {QPalette::ButtonText, "buttonText"},
            {QPalette::Highlight, "highlight"},
            {QPalette::HighlightedText, "highlightedText"},
        };
        return QQmlProperty(object, QStringLiteral("palette.") +
                                        QLatin1String(NAMES.value(role)))
            .read()
            .value<QColor>();
    }

    QQuickItem* Item(const char* name) {
        return Window_->findChild<QQuickItem*>(name);
    }

    QTemporaryDir Dir_;
    std::unique_ptr<testing::ScopedEnv> Socket_;
    std::unique_ptr<testing::ScopedEnv> Config_;
    std::unique_ptr<testing::ScopedEnv> Style_;
    std::unique_ptr<QQmlApplicationEngine> Engine_;
    QQuickWindow* Window_ = nullptr;
    Theme* Theme_ = nullptr;

private slots:
    void initTestCase() {
        // A style asked for by the user's environment does not replace it.
        Style_ = std::make_unique<testing::ScopedEnv>("QT_QUICK_CONTROLS_STYLE",
                                                      "Basic");
        SelectControlsStyle();
    }

    void cleanupTestCase() { Style_.reset(); }

    void init() {
        QVERIFY(Dir_.isValid());
        QVERIFY(QDir().mkpath(Dir_.filePath("run")));
        QVERIFY(QDir().mkpath(Dir_.filePath("config")));
        Socket_ = std::make_unique<testing::ScopedEnv>(
            "ROOST_SOCKET", Dir_.filePath("run/roostd.sock").toUtf8());
        Config_ = std::make_unique<testing::ScopedEnv>(
            "ROOST_CONFIG", Dir_.filePath("config").toUtf8());
        Engine_ = std::make_unique<QQmlApplicationEngine>();
        Engine_->loadFromModule("Roost.Shell", "Main");
        QVERIFY(!Engine_->rootObjects().isEmpty());
        Window_ = qobject_cast<QQuickWindow*>(Engine_->rootObjects().first());
        QVERIFY(Window_ != nullptr);
        auto* singleton =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(singleton != nullptr);
        Theme_ = singleton->theme();
        // The banner and its Retry button show on a refusal.
        Window_->setProperty("connection", "refused");
        QVERIFY(QTest::qWaitForWindowExposed(Window_));
    }

    void cleanup() {
        Engine_.reset();
        Window_ = nullptr;
        Theme_ = nullptr;
        Config_.reset();
        Socket_.reset();
    }

    void TheStyleInEffectIsFusion() {
        QCOMPARE(QQuickStyle::name(), QStringLiteral("Fusion"));
        // The Button really is the Fusion one: its background is the
        // ButtonPanel of QtQuick.Controls.Fusion.impl, which Basic has not.
        QQuickItem* button = Item("retryButton");
        QVERIFY(button != nullptr);
        const auto* background =
            button->property("background").value<QObject*>();
        QVERIFY(background != nullptr);
        QVERIFY2(QByteArray(background->metaObject()->className())
                     .contains("ButtonPanel"),
                 background->metaObject()->className());
    }

    void TheWindowPaletteIsTheTokens() {
        QCOMPARE(PaletteOf(Window_, QPalette::Window),
                 Theme_->windowBackground());
        QCOMPARE(PaletteOf(Window_, QPalette::WindowText),
                 Theme_->windowText());
        QCOMPARE(PaletteOf(Window_, QPalette::Base), Theme_->inputBackground());
        QCOMPARE(PaletteOf(Window_, QPalette::Button),
                 Theme_->controlBackground());
        QCOMPARE(PaletteOf(Window_, QPalette::ButtonText),
                 Theme_->windowText());
        QCOMPARE(PaletteOf(Window_, QPalette::Highlight), Theme_->selection());
        QCOMPARE(PaletteOf(Window_, QPalette::HighlightedText),
                 Theme_->textStrong());
    }

    // The Retry button, the dialog and the dialog's own button inherit the
    // palette of the window; a token that changes changes them.
    void AControlTakesThePaletteOfTheTokens() {
        QQuickItem* button = Item("retryButton");
        QObject* dialog = Window_->findChild<QObject*>("bugReportDialog");
        QObject* create = Window_->findChild<QObject*>("createButton");
        QVERIFY(button != nullptr && dialog != nullptr && create != nullptr);
        QCOMPARE(PaletteOf(button, QPalette::Button),
                 Theme_->controlBackground());
        QCOMPARE(PaletteOf(button, QPalette::ButtonText), Theme_->windowText());
        QCOMPARE(PaletteOf(dialog, QPalette::Window),
                 Theme_->windowBackground());
        QCOMPARE(PaletteOf(create, QPalette::Button),
                 Theme_->controlBackground());

        // A popup takes the palette of the window while it is open.
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QJsonObject changed;
        changed.insert("controlBackground", "#336699");
        changed.insert("windowText", "#996633");
        changed.insert("windowBackground", "#669933");
        QVERIFY(Theme_->load(changed));
        QCOMPARE(PaletteOf(button, QPalette::Button), QColor("#336699"));
        QCOMPARE(PaletteOf(button, QPalette::ButtonText), QColor("#996633"));
        QCOMPARE(PaletteOf(dialog, QPalette::Window), QColor("#669933"));
        QCOMPARE(PaletteOf(dialog, QPalette::WindowText), QColor("#996633"));
        QCOMPARE(PaletteOf(create, QPalette::Button), QColor("#336699"));
    }

    // The palette is painted, not only set: the middle of the button is the
    // colour of the token, whichever the token is (the native style of macOS
    // would draw nothing there).
    void TheButtonIsPaintedWithTheToken() {
        QQuickItem* button = Item("retryButton");
        QVERIFY(button != nullptr);
        const auto middleOfButton = [&] {
            const QPointF at = button->mapToScene(
                QPointF(button->width() / 2, button->height() / 2));
            QTest::qWait(100);
            const QImage frame = Window_->grabWindow();
            // Left of the label: the face of the button, free of the text.
            return frame.pixelColor(
                QPoint(static_cast<int>(button->mapToScene(QPointF(8, 0)).x()),
                       static_cast<int>(at.y())));
        };
        QJsonObject red;
        red.insert("controlBackground", "#ff0000");
        QVERIFY(Theme_->load(red));
        const QColor redFace = middleOfButton();
        QVERIFY2(redFace.red() > 200 && redFace.green() < 120 &&
                     redFace.blue() < 120,
                 qPrintable(redFace.name()));
        QJsonObject blue;
        blue.insert("controlBackground", "#0000ff");
        QVERIFY(Theme_->load(blue));
        const QColor blueFace = middleOfButton();
        QVERIFY2(blueFace.blue() > 200 && blueFace.red() < 150 &&
                     blueFace.green() < 150,
                 qPrintable(blueFace.name()));
    }
};

QTEST_MAIN(ControlsStyleTest)
#include "controls_style_test.moc"
