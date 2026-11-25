// The colour of the focus ring (settings.focusColor, SettingsWindow.qml,
// Theme::follow): Base, Contrast and Custom change the `focus` token of every
// window, a custom colour with less than 3:1 against `surface` or `canvas` is
// refused with its reason and the token keeps the colour it had, and the
// choice is still there after roostd restarts. Settings windows of several
// engines against one real roostd; ROOST_CONFIG is the directory of roostd's
// own temporary tree, so the settings of the user are never read.

#include <QColor>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <memory>

#include "api/api.hpp"
#include "api/api_test_support.hpp"
#include "helpers/roostd_process.hpp"
#include "helpers/scoped_env.hpp"
#include "qml_api/roost_singleton.hpp"
#include "qml_api/theme.hpp"
#include "roost/controls_style.hpp"
#include "roost/plugin_loader.hpp"

using namespace roost;
using namespace roost::api_test;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

const QColor BASE("#225fc2");
const QColor CONTRAST("#1b2430");
const QColor PURPLE("#6a1b9a");
const QColor CANVAS("#f3f6f8");

constexpr int PRESET_BASE = 0;
constexpr int PRESET_CONTRAST = 1;
constexpr int PRESET_CUSTOM = 2;

QQuickItem* FindItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name) {
        return root;
    }
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = FindItem(child, name)) {
            return found;
        }
    }
    return nullptr;
}

double Luminance(const QColor& color) {
    const auto channel = [](double value) {
        return value <= 0.04045 ? value / 12.92
                                : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(static_cast<double>(color.redF())) +
           0.7152 * channel(static_cast<double>(color.greenF())) +
           0.0722 * channel(static_cast<double>(color.blueF()));
}

double Contrast(const QColor& first, const QColor& second) {
    const double a = Luminance(first);
    const double b = Luminance(second);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

}  // namespace

// One window of roost: its own Api (a connection to roostd), engine, loader and
// the settings window.
struct FocusWindow {
    std::unique_ptr<api::Api> Api = std::make_unique<api::Api>();
    std::unique_ptr<QQmlEngine> Engine = std::make_unique<QQmlEngine>();
    RoostSingleton* Singleton = nullptr;
    std::unique_ptr<PluginLoader> Loader;
    std::unique_ptr<QObject> Object;

    ~FocusWindow() {
        Object.reset();
        Loader.reset();
        Engine.reset();
        Api.reset();
    }

    QQuickWindow* Window() const {
        return qobject_cast<QQuickWindow*>(Object.get());
    }

    QQuickItem* Find(const QString& name) const {
        return FindItem(Window()->contentItem(), name);
    }

    QColor Focus() const { return Singleton->theme()->focus(); }

    QStringList Problems() const { return Singleton->theme()->fontProblems(); }

    // What the user does to the box: chooses an entry of the list.
    bool Choose(int index) const {
        QQuickItem* box = Find("focusPresetBox");
        return box != nullptr &&
               QMetaObject::invokeMethod(box, "activated", Q_ARG(int, index));
    }

    // What the user does to a text field: types, then leaves it.
    bool Edit(const QString& field, const QString& text) const {
        QQuickItem* item = Find(field);
        return item != nullptr && item->setProperty("text", text) &&
               QMetaObject::invokeMethod(item, "editingFinished");
    }
};

class FocusColorSettingsTest : public QObject {
    Q_OBJECT

private:
    // A window against roostd; null if it could not be made.
    std::unique_ptr<FocusWindow> OpenWindow() {
        auto window = std::make_unique<FocusWindow>();
        QSignalSpy connected(window->Api.get(), &api::Api::Connected);
        window->Singleton = window->Engine->singletonInstance<RoostSingleton*>(
            "Roost.Api", "Roost");
        if (window->Singleton == nullptr) {
            return nullptr;
        }
        window->Singleton->setApi(*window->Api);
        window->Api->Connect(Daemon_->SocketPath());
        if (connected.isEmpty() && !connected.wait(TIMEOUT_MS)) {
            return nullptr;
        }
        if (!QTest::qWaitFor([&] { return window->Api->Plugins().IsLoaded(); },
                             TIMEOUT_MS)) {
            return nullptr;
        }
        window->Loader = std::make_unique<PluginLoader>(
            *window->Engine, window->Api->Plugins(), *window->Singleton);
        QQmlComponent component(window->Engine.get(), "Roost.Shell",
                                "SettingsWindow");
        window->Object.reset(component.create());
        if (window->Object == nullptr || window->Window() == nullptr) {
            qWarning("%s", qPrintable(component.errorString()));
            return nullptr;
        }
        // No file of the user's is opened by a test.
        window->Object->setProperty("opensFiles", false);
        QMetaObject::invokeMethod(window->Object.get(), "open");
        return window;
    }

    // A window that has read the document: its controls are what is stored.
    std::unique_ptr<FocusWindow> OpenReadWindow() {
        auto window = OpenWindow();
        if (window == nullptr) {
            return nullptr;
        }
        return QTest::qWaitFor(
                   [&] { return window->Find("focusPresetBox") != nullptr; },
                   TIMEOUT_MS)
                   ? std::move(window)
                   : nullptr;
    }

    QJsonObject Settings() const {
        QFile file(Daemon_->ConfigPath() + "/settings.json");
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    // A hand-edited document, as roostd keeps it.
    bool WriteSettings(const QJsonObject& settings) {
        QFile file(Daemon_->ConfigPath() + "/settings.json");
        return QDir().mkpath(Daemon_->ConfigPath()) &&
               file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(QJsonDocument(settings).toJson()) > 0;
    }

    std::unique_ptr<testing::RoostdProcess> Daemon_;
    std::unique_ptr<testing::ScopedEnv> Config_;

private slots:
    // The style of the window, as main() selects it, before the first engine.
    void initTestCase() { roost::SelectControlsStyle(); }

    void init() {
        Daemon_ = std::make_unique<testing::RoostdProcess>();
        QVERIFY(Daemon_->IsValid());
        Config_ = std::make_unique<testing::ScopedEnv>(
            "ROOST_CONFIG", Daemon_->ConfigPath().toUtf8());
    }

    void cleanup() {
        Daemon_.reset();
        Config_.reset();
    }

    // Without the key the ring has the colour of the theme.
    void WithoutTheSettingTheRingIsBase() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto window = OpenReadWindow();
        QVERIFY(window);
        QCOMPARE(window->Focus(), BASE);
        QVERIFY(window->Problems().isEmpty());
        QCOMPARE(window->Find("focusPresetBox")->property("currentIndex"),
                 QVariant(PRESET_BASE));
        QVERIFY(!window->Find("focusCustomField")->isVisible());
    }

    // A choice in one window changes the token in both, and is in the document.
    void ThePresetsChangeTheTokenInEveryWindow() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto first = OpenReadWindow();
        auto second = OpenReadWindow();
        QVERIFY(first);
        QVERIFY(second);

        QVERIFY(first->Choose(PRESET_CONTRAST));
        QTRY_COMPARE_WITH_TIMEOUT(first->Focus(), CONTRAST, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), CONTRAST, TIMEOUT_MS);
        QCOMPARE(Settings().value("focusColor").toObject(),
                 (QJsonObject{{"preset", "contrast"}}));

        // Custom starts from the colour the ring has, and the field shows it.
        QVERIFY(second->Choose(PRESET_CUSTOM));
        QTRY_COMPARE_WITH_TIMEOUT(
            Settings().value("focusColor").toObject(),
            (QJsonObject{{"preset", "custom"}, {"custom", "#1b2430"}}),
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(second->Find("focusCustomField")->isVisible(),
                                 TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            second->Find("focusCustomField")->property("text"),
            QVariant("#1b2430"), TIMEOUT_MS);

        QVERIFY(second->Edit("focusCustomField", "#6A1B9A"));
        QTRY_COMPARE_WITH_TIMEOUT(first->Focus(), PURPLE, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), PURPLE, TIMEOUT_MS);
        QVERIFY(first->Problems().isEmpty());
        QVERIFY(second->Problems().isEmpty());

        // Base again: the custom colour stays in the document.
        QVERIFY(first->Choose(PRESET_BASE));
        QTRY_COMPARE_WITH_TIMEOUT(first->Focus(), BASE, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), BASE, TIMEOUT_MS);
        QCOMPARE(Settings().value("focusColor").toObject(),
                 (QJsonObject{{"preset", "base"}, {"custom", "#6A1B9A"}}));
    }

    void ACustomColourTooLightForTheCardsIsRefused_data() {
        QTest::addColumn<QString>("custom");
        QTest::addColumn<bool>("taken");
        QTest::newRow("almost white") << "#f0f0f0" << false;
        QTest::newRow("white") << "#ffffff" << false;
        // 3.03:1 against the surface, below 3:1 against the canvas.
        QTest::newRow("passes the surface, fails the canvas")
            << "#949494" << false;
        QTest::newRow("dark enough") << "#767676" << true;
    }

    // Refused: the token keeps the colour it had, the reason is shown in the
    // settings window and in the problems of the theme, in every window; the
    // document keeps what the user wrote, and a new choice lifts the refusal.
    void ACustomColourTooLightForTheCardsIsRefused() {
        QFETCH(QString, custom);
        QFETCH(bool, taken);
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto first = OpenReadWindow();
        auto second = OpenReadWindow();
        QVERIFY(first);
        QVERIFY(second);
        QVERIFY(first->Choose(PRESET_CONTRAST));
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), CONTRAST, TIMEOUT_MS);

        QVERIFY(first->Choose(PRESET_CUSTOM));
        QTRY_VERIFY_WITH_TIMEOUT(first->Find("focusCustomField")->isVisible(),
                                 TIMEOUT_MS);
        QVERIFY(first->Edit("focusCustomField", custom));
        if (taken) {
            QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), QColor(custom),
                                      TIMEOUT_MS);
            QVERIFY(first->Problems().isEmpty());
            return;
        }
        // The line comes in the same step as the token, so that when it is
        // there the token has had its say.
        QTRY_VERIFY_WITH_TIMEOUT(
            !first->Problems().isEmpty() && !second->Problems().isEmpty(),
            TIMEOUT_MS);
        QCOMPARE(first->Focus(), CONTRAST);
        QCOMPARE(second->Focus(), CONTRAST);
        QCOMPARE(first->Problems().size(), 1);
        QVERIFY2(first->Problems().first().contains(
                     "settings.focusColor.custom " + custom),
                 qPrintable(first->Problems().first()));
        QVERIFY2(first->Problems().first().contains("less than 3:1"),
                 qPrintable(first->Problems().first()));
        QVERIFY2(first->Problems().first().contains("focus colour is kept"),
                 qPrintable(first->Problems().first()));
        // Where the window shows a refusal: under the fields.
        QQuickItem* shown = first->Find("fontProblems");
        QVERIFY(shown != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(shown->isVisible(), TIMEOUT_MS);
        QCOMPARE(shown->property("text").toString(), first->Problems().first());
        QCOMPARE(Settings().value("focusColor").toObject().value("custom"),
                 QJsonValue(custom));

        // A colour that is readable lifts the refusal.
        QVERIFY(first->Edit("focusCustomField", "#6a1b9a"));
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), PURPLE, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            first->Problems().isEmpty() && second->Problems().isEmpty(),
            TIMEOUT_MS);
    }

    // The test's own numbers: the colours of the rows above are what the names
    // say, against the surface (#ffffff) and the canvas of the theme.
    void TheColoursOfTheRowsAreWhatTheirNamesSay() {
        QVERIFY(Contrast(QColor("#949494"), Qt::white) >= 3);
        QVERIFY(Contrast(QColor("#949494"), CANVAS) < 3);
        QVERIFY(Contrast(QColor("#f0f0f0"), Qt::white) < 3);
        QVERIFY(Contrast(QColor("#767676"), CANVAS) >= 3);
        QVERIFY(Contrast(PURPLE, CANVAS) >= 3);
    }

    // A document that is not what it should be, as a hand edit leaves it: the
    // colour is the one the theme has, with a reason; the reason goes with the
    // fix.
    void ADocumentThatIsWrongIsRefusedWithAReason_data() {
        QTest::addColumn<QJsonValue>("focusColor");
        QTest::addColumn<QString>("reason");
        QTest::newRow("not an object")
            << QJsonValue("red") << "settings.focusColor must be an object";
        QTest::newRow("unknown preset")
            << QJsonValue(QJsonObject{{"preset", "neon"}})
            << "settings.focusColor.preset must be base, contrast or custom";
        QTest::newRow("custom without a colour")
            << QJsonValue(QJsonObject{{"preset", "custom"}})
            << "settings.focusColor.custom must be a colour #rrggbb";
        QTest::newRow("custom not a colour")
            << QJsonValue(QJsonObject{{"preset", "custom"}, {"custom", "red"}})
            << "settings.focusColor.custom must be a colour #rrggbb";
        QTest::newRow("custom not a string")
            << QJsonValue(QJsonObject{{"preset", "custom"}, {"custom", 7}})
            << "settings.focusColor.custom must be a colour #rrggbb";
    }

    void ADocumentThatIsWrongIsRefusedWithAReason() {
        QFETCH(QJsonValue, focusColor);
        QFETCH(QString, reason);
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto window = OpenReadWindow();
        QVERIFY(window);
        QVERIFY(window->Choose(PRESET_CONTRAST));
        QTRY_COMPARE_WITH_TIMEOUT(window->Focus(), CONTRAST, TIMEOUT_MS);

        // A write by another connection, as a hand edit reaches the window.
        api::Api other;
        QSignalSpy connected(&other, &api::Api::Connected);
        other.Connect(Daemon_->SocketPath());
        QVERIFY(connected.wait(TIMEOUT_MS) || !connected.isEmpty());
        const QFuture<rpc::Result> writing = other.Store().Set(
            "settings", QJsonObject{{"focusColor", focusColor}});
        QVERIFY(WaitFor(writing));
        QVERIFY(writing.result());

        QTRY_VERIFY_WITH_TIMEOUT(!window->Problems().isEmpty(), TIMEOUT_MS);
        QCOMPARE(window->Focus(), CONTRAST);
        QCOMPARE(window->Problems().size(), 1);
        QVERIFY2(window->Problems().first().contains(reason),
                 qPrintable(window->Problems().first()));

        QVERIFY(window->Choose(PRESET_BASE));
        QTRY_COMPARE_WITH_TIMEOUT(window->Focus(), BASE, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(window->Problems().isEmpty(), TIMEOUT_MS);
    }

    // The choice is in settings.json: a new roostd and a new window have it.
    void TheChoiceSurvivesARestart() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        {
            auto window = OpenReadWindow();
            QVERIFY(window);
            QVERIFY(window->Choose(PRESET_CUSTOM));
            QTRY_VERIFY_WITH_TIMEOUT(
                window->Find("focusCustomField")->isVisible(), TIMEOUT_MS);
            QVERIFY(window->Edit("focusCustomField", "#6a1b9a"));
            QTRY_COMPARE_WITH_TIMEOUT(window->Focus(), PURPLE, TIMEOUT_MS);
            QTRY_COMPARE_WITH_TIMEOUT(
                Settings().value("focusColor").toObject(),
                (QJsonObject{{"preset", "custom"}, {"custom", "#6a1b9a"}}),
                TIMEOUT_MS);
        }
        QVERIFY(Daemon_->Stop());
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto window = OpenReadWindow();
        QVERIFY(window);
        QTRY_COMPARE_WITH_TIMEOUT(window->Focus(), PURPLE, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            window->Find("focusPresetBox")->property("currentIndex"),
            QVariant(PRESET_CUSTOM), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            window->Find("focusCustomField")->property("text"),
            QVariant("#6a1b9a"), TIMEOUT_MS);
    }

    // A colour that was refused when it was written is refused at the start
    // too: the ring has the colour of the theme, with the reason.
    void ARefusedColourInTheDocumentIsBaseAtTheStart() {
        QVERIFY(WriteSettings(
            QJsonObject{{"focusColor", QJsonObject{{"preset", "custom"},
                                                   {"custom", "#f0f0f0"}}}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto window = OpenReadWindow();
        QVERIFY(window);
        QTRY_VERIFY_WITH_TIMEOUT(!window->Problems().isEmpty(), TIMEOUT_MS);
        QCOMPARE(window->Focus(), BASE);
        QVERIFY(window->Problems().first().contains("less than 3:1"));
    }
};

QTEST_MAIN(FocusColorSettingsTest)
#include "focus_color_settings_test.moc"
