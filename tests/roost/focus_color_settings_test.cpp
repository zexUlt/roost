// The colour of the focus ring (settings.focusColor, SettingsWindow.qml,
// Theme::follow): Base, Contrast and Custom change the `focus` token of every
// window, any "#rrggbb" is applied, and a colour below the contrast of
// 18-style.md only adds a warning to the problems of the theme; what is not a
// colour is refused with its reason and the token is the theme's own. The
// choice is still there after roostd restarts. Settings windows of several
// engines against one real roostd; ROOST_CONFIG is the directory of roostd's
// own temporary tree, so the settings of the user are never read.

#include <QAccessible>
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
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
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
    // Counts the documents that the settings window has taken (the property
    // settingsDocument is set anew by each one).
    std::unique_ptr<QSignalSpy> Read;

    ~FocusWindow() {
        Read.reset();
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

    // The texts of the problems of the theme.
    QStringList Problems() const {
        QStringList texts;
        for (const QVariant& problem : Singleton->theme()->settingsProblems()) {
            texts.append(problem.toMap().value("text").toString());
        }
        return texts;
    }

    // The keys of the settings the problems are about.
    QStringList ProblemSettings() const {
        QStringList settings;
        for (const QVariant& problem : Singleton->theme()->settingsProblems()) {
            settings.append(problem.toMap().value("setting").toString());
        }
        return settings;
    }

    // What the user does to the box: chooses an entry of the list.
    bool Choose(int index) const {
        QQuickItem* box = Find("focusPresetBox");
        return box != nullptr &&
               QMetaObject::invokeMethod(box, "activated", Q_ARG(int, index));
    }

    // The swatch of the ring colour at `index` (null if there is none), and
    // whether it is the one marked as the colour in force.
    QQuickItem* Swatch(int index) const {
        return Find(QStringLiteral("focusSwatch%1").arg(index));
    }
    bool InForce(int index) const {
        QQuickItem* swatch = Swatch(index);
        return swatch != nullptr && swatch->property("inForce").toBool();
    }

    // The index of the swatch that has the keyboard, -1 if none has.
    int ActiveSwatch() const {
        for (int i = 0; i < 64; ++i) {
            QQuickItem* swatch = Swatch(i);
            if (swatch == nullptr) {
                break;
            }
            if (swatch->hasActiveFocus()) {
                return i;
            }
        }
        return -1;
    }

    // The name of the item that has the keyboard.
    QString ActiveName() const {
        QQuickItem* item = Window()->activeFocusItem();
        return item == nullptr ? QString() : item->objectName();
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
        // No file of the user's is opened by a test. The spy is there before
        // open(), so the answer of roostd cannot come ahead of it.
        window->Object->setProperty("opensFiles", false);
        window->Read = std::make_unique<QSignalSpy>(
            window->Object.get(), SIGNAL(settingsDocumentChanged()));
        if (!window->Read->isValid()) {
            return nullptr;
        }
        QMetaObject::invokeMethod(window->Object.get(), "open");
        return window;
    }

    // A window that has read the document: the settings window has taken the
    // answer of its settings read (settingsDocument is set by it, and by
    // nothing else before), so its controls are what is stored. The theme has
    // its own read of the document, which this does not wait for: what the
    // theme shows is awaited by the case.
    std::unique_ptr<FocusWindow> OpenReadWindow() {
        auto window = OpenWindow();
        if (window == nullptr) {
            return nullptr;
        }
        return QTest::qWaitFor([&] { return !window->Read->isEmpty(); },
                               TIMEOUT_MS)
                   ? std::move(window)
                   : nullptr;
    }

    // The middle of a swatch in the window, once the form has been scrolled to
    // it by the keyboard focus (the swatches are below the visible part at the
    // default size).
    static QPoint Reveal(const FocusWindow& window, QQuickItem* item) {
        item->forceActiveFocus(Qt::TabFocusReason);
        QQuickItem* scroll = window.Find("settingsScroll");
        const auto middle = [&] {
            return item->mapToScene(
                QPointF(item->width() / 2, item->height() / 2));
        };
        const auto visible = [&] {
            return item->width() > 0 && item->height() > 0 &&
                   scroll
                       ->mapRectToScene(
                           QRectF(0, 0, scroll->width(), scroll->height()))
                       .contains(middle());
        };
        if (!QTest::qWaitFor(visible, TIMEOUT_MS)) {
            return {-1, -1};
        }
        return middle().toPoint();
    }

    // A click on the swatch at `index`.
    static bool Click(const FocusWindow& window, int index) {
        QQuickItem* swatch = window.Swatch(index);
        if (swatch == nullptr) {
            return false;
        }
        const QPoint at = Reveal(window, swatch);
        if (at.x() < 0) {
            return false;
        }
        QTest::mouseClick(window.Window(), Qt::LeftButton, Qt::NoModifier, at);
        return true;
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
        // Contrast holds every pair on the default theme: no warning (the
        // line comes in the same step as the token).
        QVERIFY(first->Problems().isEmpty());
        QVERIFY(second->Problems().isEmpty());

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
        QVERIFY(first->Problems().isEmpty());
        QVERIFY(second->Problems().isEmpty());
        QCOMPARE(Settings().value("focusColor").toObject(),
                 (QJsonObject{{"preset", "base"}, {"custom", "#6A1B9A"}}));
    }

    void ACustomColourBelowTheContrastIsAppliedWithAWarning_data() {
        QTest::addColumn<QString>("custom");
        // The worst pair of the colour on the default theme, as the warning
        // names it; empty for a colour that holds every pair.
        QTest::addColumn<QString>("warning");
        QTest::newRow("almost white")
            << "#f0f0f0"
            << "focus colour #f0f0f0 is 1.0:1 on selectedBackground, below "
               "4.5:1";
        QTest::newRow("white")
            << "#ffffff"
            << "focus colour #ffffff is 1.0:1 on surface, below 4.5:1";
        // 3.03:1 on the surface, below 3:1 on the canvas, 2.67:1 on the
        // selected background.
        QTest::newRow("passes the ring on the surface only")
            << "#949494"
            << "focus colour #949494 is 2.6:1 on selectedBackground, below "
               "4.5:1";
        // A ring, but not text on the filter chip: 3.99:1 is not 4.0.
        QTest::newRow("a ring, not text")
            << "#767676"
            << "focus colour #767676 is 3.9:1 on selectedBackground, below "
               "4.5:1";
        QTest::newRow("dark enough") << "#5a5a5a" << "";
        QTest::newRow("purple") << "#6a1b9a" << "";
    }

    // Applied in every window, with the warning in the problems of the theme
    // and under the field of the setting; the document keeps what the user
    // wrote, and a colour that holds every pair clears the warning.
    void ACustomColourBelowTheContrastIsAppliedWithAWarning() {
        QFETCH(QString, custom);
        QFETCH(QString, warning);
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
        QTRY_COMPARE_WITH_TIMEOUT(first->Focus(), QColor(custom), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), QColor(custom), TIMEOUT_MS);
        // The line comes in the same step as the token.
        const QStringList expected =
            warning.isEmpty() ? QStringList{} : QStringList{warning};
        QCOMPARE(first->Problems(), expected);
        QCOMPARE(second->Problems(), expected);
        QCOMPARE(Settings().value("focusColor").toObject().value("custom"),
                 QJsonValue(custom));
        if (warning.isEmpty()) {
            return;
        }
        QCOMPARE(first->ProblemSettings(), QStringList{"focusColor"});
        QQuickItem* shown = first->Find("focusColorProblems");
        QVERIFY(shown != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(shown->isVisible(), TIMEOUT_MS);
        QCOMPARE(shown->property("text").toString(), warning);

        // A colour that holds every pair clears the warning.
        QVERIFY(first->Edit("focusCustomField", "#6a1b9a"));
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), PURPLE, TIMEOUT_MS);
        QVERIFY(first->Problems().isEmpty());
        QVERIFY(second->Problems().isEmpty());
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
    // colour is the theme's own (Base), with a reason; the reason goes with
    // the fix.
    void ANonColourIsBaseWithAReason_data() {
        QTest::addColumn<QJsonValue>("focusColor");
        QTest::addColumn<QString>("reason");
        // Whether the box has no preset to show, so that it shows Base.
        QTest::addColumn<bool>("boxIsBase");
        QTest::newRow("not an object")
            << QJsonValue("red") << "settings.focusColor must be an object"
            << true;
        QTest::newRow("unknown preset")
            << QJsonValue(QJsonObject{{"preset", "neon"}})
            << "settings.focusColor.preset must be base, contrast or custom"
            << true;
        QTest::newRow("custom without a colour")
            << QJsonValue(QJsonObject{{"preset", "custom"}})
            << "settings.focusColor.custom must be a colour #rrggbb" << false;
        QTest::newRow("custom not a colour")
            << QJsonValue(QJsonObject{{"preset", "custom"}, {"custom", "red"}})
            << "settings.focusColor.custom must be a colour #rrggbb" << false;
        QTest::newRow("custom not a string")
            << QJsonValue(QJsonObject{{"preset", "custom"}, {"custom", 7}})
            << "settings.focusColor.custom must be a colour #rrggbb" << false;
        QTest::newRow("custom a short colour")
            << QJsonValue(QJsonObject{{"preset", "custom"}, {"custom", "#12g"}})
            << "settings.focusColor.custom must be a colour #rrggbb" << false;
        QTest::newRow("custom empty")
            << QJsonValue(QJsonObject{{"preset", "custom"}, {"custom", ""}})
            << "settings.focusColor.custom must be a colour #rrggbb" << false;
    }

    void ANonColourIsBaseWithAReason() {
        QFETCH(QJsonValue, focusColor);
        QFETCH(QString, reason);
        QFETCH(bool, boxIsBase);
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
        QCOMPARE(window->Focus(), BASE);
        QCOMPARE(window->Problems().size(), 1);
        QVERIFY2(window->Problems().first().contains(reason),
                 qPrintable(window->Problems().first()));
        QCOMPARE(window->ProblemSettings(), QStringList{"focusColor"});
        // A document with no preset the box knows shows Base, not nothing;
        // the reason is under the box.
        if (boxIsBase) {
            QTRY_COMPARE_WITH_TIMEOUT(
                window->Find("focusPresetBox")->property("currentIndex"),
                QVariant(PRESET_BASE), TIMEOUT_MS);
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            window->Find("focusColorProblems")->isVisible(), TIMEOUT_MS);

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

    // A colour below the contrast in the document is applied at the start, in
    // every window and after the restart of roostd, with its warning.
    void ALowContrastColourInTheDocumentIsAppliedAtTheStart() {
        const QString warning =
            "focus colour #f0f0f0 is 1.0:1 on selectedBackground, below 4.5:1";
        QVERIFY(WriteSettings(
            QJsonObject{{"focusColor", QJsonObject{{"preset", "custom"},
                                                   {"custom", "#f0f0f0"}}}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        {
            auto first = OpenReadWindow();
            auto second = OpenReadWindow();
            QVERIFY(first);
            QVERIFY(second);
            QTRY_COMPARE_WITH_TIMEOUT(first->Focus(), QColor("#f0f0f0"),
                                      TIMEOUT_MS);
            QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), QColor("#f0f0f0"),
                                      TIMEOUT_MS);
            QCOMPARE(first->Problems(), QStringList{warning});
            QCOMPARE(second->Problems(), QStringList{warning});
        }
        QVERIFY(Daemon_->Stop());
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto window = OpenReadWindow();
        QVERIFY(window);
        QTRY_COMPARE_WITH_TIMEOUT(window->Focus(), QColor("#f0f0f0"),
                                  TIMEOUT_MS);
        QCOMPARE(window->Problems(), QStringList{warning});
    }

    // The warning is about the current theme: a theme with a dark surface
    // makes Base a warning, and a light colour on it holds every pair. The
    // theme "test" takes its tokens from ROOST_TEST_THEME_FILE.
    void AThemeChangeReevaluatesTheWarning() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile file(dir.filePath("theme.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QJsonDocument(
                               QJsonObject{{"surface", "#10151b"},
                                           {"canvas", "#0b0f14"},
                                           {"cursorFocused", "#161c24"},
                                           {"selectedBackground", "#1a2230"}})
                               .toJson()) > 0);
        file.close();
        const testing::ScopedEnv themeFile("ROOST_TEST_THEME_FILE",
                                           file.fileName().toUtf8());
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto first = OpenReadWindow();
        auto second = OpenReadWindow();
        QVERIFY(first);
        QVERIFY(second);
        QCOMPARE(first->Focus(), BASE);
        QVERIFY(first->Problems().isEmpty());

        api::Api other;
        QSignalSpy connected(&other, &api::Api::Connected);
        other.Connect(Daemon_->SocketPath());
        QVERIFY(connected.wait(TIMEOUT_MS) || !connected.isEmpty());
        const auto write = [&](const QJsonObject& settings) {
            const QFuture<rpc::Result> writing =
                other.Store().Set("settings", settings);
            return WaitFor(writing) && writing.result();
        };

        // Base on a dark surface: the same colour, a warning.
        QVERIFY(write({{"theme", "test"}}));
        QTRY_VERIFY_WITH_TIMEOUT(
            first->Problems().size() == 1 && second->Problems().size() == 1,
            TIMEOUT_MS);
        QCOMPARE(first->Focus(), BASE);
        QVERIFY2(
            first->Problems().first().startsWith("focus colour #225fc2 is "),
            qPrintable(first->Problems().first()));
        QVERIFY2(first->Problems().first().contains(" below "),
                 qPrintable(first->Problems().first()));
        QCOMPARE(first->ProblemSettings(), QStringList{"focusColor"});

        // A light custom colour holds every pair of the dark theme.
        QVERIFY(write({{"theme", "test"},
                       {"focusColor", QJsonObject{{"preset", "custom"},
                                                  {"custom", "#ffffff"}}}}));
        QTRY_COMPARE_WITH_TIMEOUT(first->Focus(), QColor("#ffffff"),
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), QColor("#ffffff"),
                                  TIMEOUT_MS);
        QVERIFY(first->Problems().isEmpty());
        QVERIFY(second->Problems().isEmpty());

        // Back to the default theme: white is the warning there.
        QVERIFY(write({{"focusColor", QJsonObject{{"preset", "custom"},
                                                  {"custom", "#ffffff"}}}}));
        QTRY_COMPARE_WITH_TIMEOUT(
            first->Problems(),
            QStringList{"focus colour #ffffff is 1.0:1 on surface, below "
                        "4.5:1"},
            TIMEOUT_MS);
        QCOMPARE(first->Focus(), QColor("#ffffff"));
    }

    // Each problem of the settings the theme reads is under the field of its
    // own setting, and only there.
    void AProblemIsShownUnderTheFieldOfItsSetting() {
        QVERIFY(WriteSettings(
            QJsonObject{{"terminalFont", QJsonObject{{"size", -1}}},
                        {"uiFontSize", "big"},
                        {"focusColor", QJsonObject{{"preset", "neon"}}}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto window = OpenReadWindow();
        QVERIFY(window);
        QTRY_COMPARE_WITH_TIMEOUT(window->Problems().size(), 3, TIMEOUT_MS);
        QCOMPARE(window->ProblemSettings(),
                 (QStringList{"terminalFont", "uiFontSize", "focusColor"}));
        const std::pair<const char*, const char*> shown[] = {
            {"terminalFontProblems", "settings.terminalFont.size"},
            {"uiFontSizeProblems", "settings.uiFontSize"},
            {"focusColorProblems", "settings.focusColor.preset"}};
        for (const auto& [field, text] : shown) {
            QQuickItem* label = window->Find(field);
            QVERIFY2(label != nullptr, field);
            QTRY_VERIFY_WITH_TIMEOUT(label->isVisible(), TIMEOUT_MS);
            const QString words = label->property("text").toString();
            QVERIFY2(words.startsWith(text), qPrintable(words));
            QVERIFY2(!words.contains('\n'), qPrintable(words));
        }

        // Fixed: the lines go from under the fields.
        QVERIFY(window->Choose(PRESET_BASE));
        QVERIFY(window->Edit("uiSizeField", "14"));
        QVERIFY(window->Edit("terminalSizeField", "14"));
        for (const auto& [field, text] : shown) {
            QQuickItem* label = window->Find(field);
            QTRY_VERIFY_WITH_TIMEOUT(!label->isVisible(), TIMEOUT_MS);
        }
        QVERIFY(window->Problems().isEmpty());
    }

    // Only the problems of the focus colour change (the token stays Base, the
    // fonts stay): the theme still says so, so that the line under the field
    // goes.
    void AFixedFocusColourTakesItsProblemAwayAlone() {
        QVERIFY(WriteSettings(
            QJsonObject{{"focusColor", QJsonObject{{"preset", "neon"}}}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto window = OpenReadWindow();
        QVERIFY(window);
        QTRY_COMPARE_WITH_TIMEOUT(window->Problems().size(), 1, TIMEOUT_MS);
        QCOMPARE(window->Focus(), BASE);
        QQuickItem* label = window->Find("focusColorProblems");
        QVERIFY(label != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(label->isVisible(), TIMEOUT_MS);

        QSignalSpy changed(window->Singleton->theme(), &Theme::changed);
        QSignalSpy colors(window->Singleton->theme(),
                          &Theme::terminalColorsChanged);
        api::Api other;
        QSignalSpy connected(&other, &api::Api::Connected);
        other.Connect(Daemon_->SocketPath());
        QVERIFY(connected.wait(TIMEOUT_MS) || !connected.isEmpty());
        const QFuture<rpc::Result> writing = other.Store().Set(
            "settings",
            QJsonObject{{"focusColor", QJsonObject{{"preset", "base"}}}});
        QVERIFY(WaitFor(writing));
        QVERIFY(writing.result());

        // What QML sees goes with the problems of C++.
        QTRY_VERIFY_WITH_TIMEOUT(!label->isVisible(), TIMEOUT_MS);
        QVERIFY(window->Problems().isEmpty());
        QCOMPARE(window->Focus(), BASE);
        // The one emit of the write is the one of the problems: the token,
        // the colours of the terminal and the fonts did not change.
        QCOMPARE(changed.count(), 1);
        QCOMPARE(colors.count(), 0);
    }

    // The swatches are the documented set (FOCUS_SWATCHES): the two presets
    // first, every one a colour, every one holds every pair of the focus
    // contrast on the default theme (so a swatch alone never brings the
    // warning; the pairs are read from the tokens of the theme), the one marked
    // at Base is the colour of the ring, and the window shows one for each,
    // named by the hex (the presets with their name before it), for every
    // preset of the box.
    void TheSwatchesAreTheDocumentedSetAndHoldTheContrast() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto window = OpenReadWindow();
        QVERIFY(window);
        const Theme* theme = window->Singleton->theme();
        const QStringList swatches = theme->focusSwatches();
        QCOMPARE(swatches.size(), 10);
        QCOMPARE(swatches.at(0), theme->focusBaseText());
        QCOMPARE(swatches.at(1), theme->focusContrastText());
        QCOMPARE(QSet<QString>(swatches.begin(), swatches.end()).size(),
                 swatches.size());
        const std::pair<QColor, double> pairs[] = {
            {theme->surface(), 4.5},
            {theme->canvas(), 3.0},
            {theme->cursorFocused(), 3.0},
            {theme->selectedBackground(), 4.5}};
        for (const QString& text : swatches) {
            const QColor color(text);
            QVERIFY2(color.isValid() && text == text.toLower(),
                     qPrintable(text));
            for (const auto& [back, least] : pairs) {
                QVERIFY2(Contrast(color, back) >= least, qPrintable(text));
            }
        }
        // At Base the mark is on the swatch of the colour of the ring.
        int marked = -1;
        for (int i = 0; i < swatches.size(); ++i) {
            if (window->InForce(i)) {
                QCOMPARE(marked, -1);
                marked = i;
            }
        }
        QVERIFY(marked >= 0);
        QCOMPARE(QColor(swatches.at(marked)), theme->focus());
        for (int preset : {PRESET_BASE, PRESET_CONTRAST, PRESET_CUSTOM}) {
            QVERIFY(window->Choose(preset));
            for (int i = 0; i < swatches.size(); ++i) {
                QQuickItem* swatch = window->Swatch(i);
                QVERIFY(swatch != nullptr);
                QTRY_VERIFY_WITH_TIMEOUT(swatch->isVisible(), TIMEOUT_MS);
                QCOMPARE(swatch->property("modelData").toString(),
                         swatches.at(i));
                // The name for a screen reader is the hex, with the name of the
                // preset for the two colours that are presets.
                const QString name =
                    i == 0 ? QStringLiteral("Base (%1)").arg(swatches.at(i))
                    : i == 1
                        ? QStringLiteral("Contrast (%1)").arg(swatches.at(i))
                        : swatches.at(i);
                QAccessibleInterface* accessible =
                    QAccessible::queryAccessibleInterface(swatch);
                QVERIFY(accessible != nullptr);
                QCOMPARE(accessible->text(QAccessible::Name), name);
                QCOMPARE(accessible->text(QAccessible::Description),
                         QStringLiteral("Picking it makes the preset Custom"));
            }
        }
        QVERIFY(window->Swatch(static_cast<int>(swatches.size())) == nullptr);
    }

    // A click on a swatch while the preset is not Custom is one patch: the
    // preset and the colour together. The token changes in every window, the
    // swatch is marked in each, and the field takes the colour.
    void AClickOnASwatchWritesTheCustomColourInOnePatch() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto first = OpenReadWindow();
        auto second = OpenReadWindow();
        QVERIFY(first);
        QVERIFY(second);
        QVERIFY(first->Choose(PRESET_CONTRAST));
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), CONTRAST, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(first->InForce(1), TIMEOUT_MS);
        QVERIFY(!first->InForce(5));
        const qsizetype before = second->Read->count();

        QQuickItem* swatch = first->Swatch(5);
        QVERIFY(swatch != nullptr);
        const QString color = swatch->property("modelData").toString();
        const QPoint at = Reveal(*first, swatch);
        QVERIFY(at.x() >= 0);
        QTest::mouseClick(first->Window(), Qt::LeftButton, Qt::NoModifier, at);

        QTRY_COMPARE_WITH_TIMEOUT(first->Focus(), QColor(color), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), QColor(color), TIMEOUT_MS);
        QCOMPARE(Settings().value("focusColor").toObject(),
                 (QJsonObject{{"preset", "custom"}, {"custom", color}}));
        QTRY_VERIFY_WITH_TIMEOUT(first->InForce(5) && second->InForce(5),
                                 TIMEOUT_MS);
        QVERIFY(!first->InForce(1));
        QTRY_COMPARE_WITH_TIMEOUT(
            first->Find("focusPresetBox")->property("currentIndex"),
            QVariant(PRESET_CUSTOM), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            first->Find("focusCustomField")->property("text"), QVariant(color),
            TIMEOUT_MS);
        QVERIFY(first->Problems().isEmpty());

        // A write of the field after it is the barrier: the window of the
        // second engine has seen the pick as one document and this one.
        QVERIFY(first->Edit("focusCustomField", "#6a1b9a"));
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), PURPLE, TIMEOUT_MS);
        QCOMPARE(second->Read->count() - before, qsizetype{2});
        QVERIFY(!second->InForce(5));
    }

    // The keyboard: Right moves along the row, Space and Enter pick. With the
    // preset on Custom a pick writes the colour alone.
    void TheKeyboardPicksASwatch() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto first = OpenReadWindow();
        auto second = OpenReadWindow();
        QVERIFY(first);
        QVERIFY(second);
        QVERIFY(first->Choose(PRESET_CUSTOM));
        QTRY_VERIFY_WITH_TIMEOUT(first->Find("focusCustomField")->isVisible(),
                                 TIMEOUT_MS);
        QVERIFY(first->Edit("focusCustomField", "#6a1b9a"));
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), PURPLE, TIMEOUT_MS);
        const qsizetype before = second->Read->count();

        // The window with the keyboard is the active one (two are open).
        first->Window()->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(first->Window(), TIMEOUT_MS));
        QQuickItem* start = first->Swatch(2);
        QVERIFY(start != nullptr);
        QVERIFY(Reveal(*first, start).x() >= 0);
        QTRY_VERIFY_WITH_TIMEOUT(start->property("visualFocus").toBool(),
                                 TIMEOUT_MS);
        QTest::keyClick(first->Window(), Qt::Key_Right);
        QQuickItem* next = first->Swatch(3);
        QTRY_VERIFY_WITH_TIMEOUT(next->property("visualFocus").toBool(),
                                 TIMEOUT_MS);
        QTest::keyClick(first->Window(), Qt::Key_Space);
        const QString space = next->property("modelData").toString();
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), QColor(space), TIMEOUT_MS);
        QCOMPARE(Settings().value("focusColor").toObject(),
                 (QJsonObject{{"preset", "custom"}, {"custom", space}}));

        QTest::keyClick(first->Window(), Qt::Key_Left);
        QTest::keyClick(first->Window(), Qt::Key_Left);
        QQuickItem* before2 = first->Swatch(1);
        QTRY_VERIFY_WITH_TIMEOUT(before2->property("visualFocus").toBool(),
                                 TIMEOUT_MS);
        QTest::keyClick(first->Window(), Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), CONTRAST, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(second->InForce(1), TIMEOUT_MS);
        // Two picks, two documents: no preset write of its own.
        QVERIFY(first->Edit("focusCustomField", "#6a1b9a"));
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), PURPLE, TIMEOUT_MS);
        QCOMPARE(second->Read->count() - before, qsizetype{3});
    }

    // The set has no colour below the contrast, so the warning for a low
    // colour is the one of the field (ACustomColourBelowTheContrast...); what a
    // swatch does with it is to clear it, the colour of a swatch holds every
    // pair.
    void ASwatchClearsTheWarningOfALowColour() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto window = OpenReadWindow();
        QVERIFY(window);
        QVERIFY(window->Choose(PRESET_CUSTOM));
        QTRY_VERIFY_WITH_TIMEOUT(window->Find("focusCustomField")->isVisible(),
                                 TIMEOUT_MS);
        QVERIFY(window->Edit("focusCustomField", "#f0f0f0"));
        QTRY_COMPARE_WITH_TIMEOUT(window->Problems().size(), 1, TIMEOUT_MS);
        QCOMPARE(window->Focus(), QColor("#f0f0f0"));

        QQuickItem* swatch = window->Swatch(7);
        QVERIFY(swatch != nullptr);
        const QString color = swatch->property("modelData").toString();
        const QPoint at = Reveal(*window, swatch);
        QVERIFY(at.x() >= 0);
        QTest::mouseClick(window->Window(), Qt::LeftButton, Qt::NoModifier, at);
        QTRY_COMPARE_WITH_TIMEOUT(window->Focus(), QColor(color), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(window->Problems().isEmpty(), TIMEOUT_MS);
    }

    // The field and the marked swatch show the same colour after roostd
    // restarts.
    void TheFieldAndTheSwatchAgreeAfterARestart() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QString color;
        {
            auto window = OpenReadWindow();
            QVERIFY(window);
            QQuickItem* swatch = window->Swatch(8);
            QVERIFY(swatch != nullptr);
            color = swatch->property("modelData").toString();
            const QPoint at = Reveal(*window, swatch);
            QVERIFY(at.x() >= 0);
            QTest::mouseClick(window->Window(), Qt::LeftButton, Qt::NoModifier,
                              at);
            QTRY_COMPARE_WITH_TIMEOUT(
                Settings().value("focusColor").toObject(),
                (QJsonObject{{"preset", "custom"}, {"custom", color}}),
                TIMEOUT_MS);
        }
        QVERIFY(Daemon_->Stop());
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto window = OpenReadWindow();
        QVERIFY(window);
        QTRY_COMPARE_WITH_TIMEOUT(window->Focus(), QColor(color), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            window->Find("focusCustomField")->property("text"), QVariant(color),
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(window->InForce(8), TIMEOUT_MS);
        for (int i = 0; i < 10; ++i) {
            QCOMPARE(window->InForce(i), i == 8);
        }
    }

    // The same colour picked again with the preset on Custom writes nothing:
    // the barrier is a write of another key, which the second window reads
    // after the pick, so exactly the pick and the barrier are documents.
    void ThePickOfTheStoredColourAgainWritesNothing() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto first = OpenReadWindow();
        auto second = OpenReadWindow();
        QVERIFY(first);
        QVERIFY(second);
        const QString color =
            first->Swatch(5)->property("modelData").toString();
        QVERIFY(Click(*first, 5));
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), QColor(color), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(first->InForce(5), TIMEOUT_MS);
        const qsizetype before = second->Read->count();

        QVERIFY(Click(*first, 5));
        QVERIFY(first->Edit("uiSizeField", "14"));
        QTRY_COMPARE_WITH_TIMEOUT(second->Read->count() - before, qsizetype{1},
                                  TIMEOUT_MS);
        QCOMPARE(Settings().value("focusColor").toObject(),
                 (QJsonObject{{"preset", "custom"}, {"custom", color}}));
        QCOMPARE(Settings().value("uiFontSize"), QJsonValue(14));
    }

    // The theme reads the colour in lower case, so a hand-written one in upper
    // case is the colour that is picked: nothing is written, and the text of
    // the document stays as it was.
    void TheStoredColourInUpperCaseIsTheSameColour() {
        QVERIFY(WriteSettings(
            QJsonObject{{"focusColor", QJsonObject{{"preset", "custom"},
                                                   {"custom", "#B42318"}}}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto first = OpenReadWindow();
        auto second = OpenReadWindow();
        QVERIFY(first);
        QVERIFY(second);
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), QColor("#b42318"),
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(first->InForce(7), TIMEOUT_MS);
        QCOMPARE(first->Swatch(7)->property("modelData").toString(),
                 QString("#b42318"));
        const qsizetype before = second->Read->count();

        QVERIFY(Click(*first, 7));
        QVERIFY(first->Edit("uiSizeField", "14"));
        QTRY_COMPARE_WITH_TIMEOUT(second->Read->count() - before, qsizetype{1},
                                  TIMEOUT_MS);
        QCOMPARE(Settings().value("focusColor").toObject(),
                 (QJsonObject{{"preset", "custom"}, {"custom", "#B42318"}}));
    }

    // A stored custom colour that is not a colour (not a string, or not a
    // hex) is replaced by a pick: one patch, the ring is the colour again and
    // the warning of the theme goes.
    void APickReplacesACustomColourThatIsNotAColour_data() {
        QTest::addColumn<QJsonValue>("custom");
        QTest::newRow("not a string") << QJsonValue(7);
        QTest::newRow("not a hex") << QJsonValue("red");
        QTest::newRow("a short hex") << QJsonValue("#12g");
    }

    void APickReplacesACustomColourThatIsNotAColour() {
        QFETCH(QJsonValue, custom);
        QVERIFY(WriteSettings(
            QJsonObject{{"focusColor", QJsonObject{{"preset", "custom"},
                                                   {"custom", custom}}}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto first = OpenReadWindow();
        auto second = OpenReadWindow();
        QVERIFY(first);
        QVERIFY(second);
        QTRY_COMPARE_WITH_TIMEOUT(first->Problems().size(), 1, TIMEOUT_MS);
        QCOMPARE(first->Focus(), BASE);
        const qsizetype before = second->Read->count();

        const QString color =
            first->Swatch(5)->property("modelData").toString();
        QVERIFY(Click(*first, 5));
        QTRY_COMPARE_WITH_TIMEOUT(second->Focus(), QColor(color), TIMEOUT_MS);
        QCOMPARE(Settings().value("focusColor").toObject(),
                 (QJsonObject{{"preset", "custom"}, {"custom", color}}));
        QTRY_VERIFY_WITH_TIMEOUT(first->Problems().isEmpty(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(first->InForce(5), TIMEOUT_MS);
        // One document for the pick.
        QVERIFY(first->Edit("uiSizeField", "14"));
        QTRY_COMPARE_WITH_TIMEOUT(second->Read->count() - before, qsizetype{2},
                                  TIMEOUT_MS);
    }

    // Reset after a pick takes the key out, and the mark goes to the colour
    // that is in force then: Base.
    void ResetAfterAPickTakesTheKeyOutAndTheMarkFollows() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto window = OpenReadWindow();
        QVERIFY(window);
        QVERIFY(Click(*window, 5));
        QTRY_VERIFY_WITH_TIMEOUT(window->InForce(5), TIMEOUT_MS);
        QCOMPARE(Settings().value("focusColor").toObject(),
                 (QJsonObject{
                     {"preset", "custom"},
                     {"custom",
                      window->Swatch(5)->property("modelData").toString()}}));

        QQuickItem* reset = window->Find("focusColorReset");
        QVERIFY(reset != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(reset->isVisible(), TIMEOUT_MS);
        QVERIFY(QMetaObject::invokeMethod(reset, "clicked"));
        QTRY_COMPARE_WITH_TIMEOUT(window->Focus(), BASE, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(window->InForce(0), TIMEOUT_MS);
        QVERIFY(!window->InForce(5));
        QVERIFY(!Settings().contains("focusColor"));
        QTRY_VERIFY_WITH_TIMEOUT(!reset->isVisible(), TIMEOUT_MS);
    }

    // The row is one stop of Tab: Tab enters on the swatch in force, one more
    // Tab leaves the row, Shift+Tab enters on the same one and leaves the row
    // backwards in one press.
    void TheRowIsOneStopOfTab() {
        QVERIFY(WriteSettings(
            QJsonObject{{"focusColor", QJsonObject{{"preset", "custom"},
                                                   {"custom", "#b42318"}}}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto window = OpenReadWindow();
        QVERIFY(window);
        QTRY_VERIFY_WITH_TIMEOUT(window->InForce(7), TIMEOUT_MS);
        window->Window()->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window->Window(), TIMEOUT_MS));
        QQuickItem* reset = window->Find("focusColorReset");
        QVERIFY(reset != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(reset->isVisible(), TIMEOUT_MS);
        QVERIFY(Reveal(*window, reset).x() >= 0);
        QTRY_COMPARE_WITH_TIMEOUT(window->ActiveName(),
                                  QString("focusColorReset"), TIMEOUT_MS);

        // In: the swatch in force, not the first.
        QTest::keyClick(window->Window(), Qt::Key_Tab);
        QTRY_COMPARE_WITH_TIMEOUT(window->ActiveSwatch(), 7, TIMEOUT_MS);
        QVERIFY(window->Swatch(7)->property("visualFocus").toBool());
        // Out: one more Tab, and the keyboard is not on a swatch.
        QTest::keyClick(window->Window(), Qt::Key_Tab);
        QTRY_COMPARE_WITH_TIMEOUT(window->ActiveSwatch(), -1, TIMEOUT_MS);
        QVERIFY(!window->ActiveName().isEmpty());

        // Back from after the row: the same swatch again.
        QTest::keyClick(window->Window(), Qt::Key_Tab, Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(window->ActiveSwatch(), 7, TIMEOUT_MS);
        // Moved inside, then Shift+Tab leaves backwards in one press, to what
        // is before the row.
        QTest::keyClick(window->Window(), Qt::Key_Right);
        QTRY_COMPARE_WITH_TIMEOUT(window->ActiveSwatch(), 8, TIMEOUT_MS);
        QTest::keyClick(window->Window(), Qt::Key_Tab, Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(window->ActiveName(),
                                  QString("focusColorReset"), TIMEOUT_MS);
        // Tab enters on the one in force, not on the one that was left.
        QTest::keyClick(window->Window(), Qt::Key_Tab);
        QTRY_COMPARE_WITH_TIMEOUT(window->ActiveSwatch(), 7, TIMEOUT_MS);
    }

    // Without a swatch in force Tab enters on the first.
    void TabEntersOnTheFirstSwatchWhenNoneIsInForce() {
        QVERIFY(WriteSettings(
            QJsonObject{{"focusColor", QJsonObject{{"preset", "custom"},
                                                   {"custom", "#123456"}}}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto window = OpenReadWindow();
        QVERIFY(window);
        QTRY_COMPARE_WITH_TIMEOUT(window->Focus(), QColor("#123456"),
                                  TIMEOUT_MS);
        for (int i = 0; i < 10; ++i) {
            QVERIFY(!window->InForce(i));
        }
        window->Window()->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window->Window(), TIMEOUT_MS));
        QQuickItem* reset = window->Find("focusColorReset");
        QVERIFY(reset != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(reset->isVisible(), TIMEOUT_MS);
        QVERIFY(Reveal(*window, reset).x() >= 0);
        QTest::keyClick(window->Window(), Qt::Key_Tab);
        QTRY_COMPARE_WITH_TIMEOUT(window->ActiveSwatch(), 0, TIMEOUT_MS);
    }

    // Up and Down move by the rows of the Flow when it wraps, to the swatch
    // nearest in x; the arrows stop at the ends.
    void UpAndDownMoveByTheRowsOfTheFlow() {
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        auto window = OpenReadWindow();
        QVERIFY(window);
        // Narrow enough for the swatches to wrap.
        window->Window()->setWidth(420);
        window->Window()->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window->Window(), TIMEOUT_MS));
        const auto y = [&](int i) { return window->Swatch(i)->y(); };
        QTRY_VERIFY_WITH_TIMEOUT(y(9) > y(0), TIMEOUT_MS);
        int rowEnd = 0;
        while (y(rowEnd + 1) == y(0)) {
            ++rowEnd;
        }
        QVERIFY(rowEnd >= 1);
        QVERIFY(Reveal(*window, window->Swatch(1)).x() >= 0);
        QTRY_COMPARE_WITH_TIMEOUT(window->ActiveSwatch(), 1, TIMEOUT_MS);

        // Down: a swatch of the second row, the nearest in x to swatch 1.
        QTest::keyClick(window->Window(), Qt::Key_Down);
        QTRY_VERIFY_WITH_TIMEOUT(window->ActiveSwatch() > rowEnd, TIMEOUT_MS);
        const int landed = window->ActiveSwatch();
        QCOMPARE(y(landed), y(rowEnd + 1));
        for (int i = rowEnd + 1; i < 10; ++i) {
            if (y(i) == y(landed)) {
                QVERIFY(
                    std::abs(window->Swatch(landed)->x() -
                             window->Swatch(1)->x()) <=
                    std::abs(window->Swatch(i)->x() - window->Swatch(1)->x()));
            }
        }
        // Up: the first row again.
        QTest::keyClick(window->Window(), Qt::Key_Up);
        QTRY_VERIFY_WITH_TIMEOUT(
            window->ActiveSwatch() >= 0 && window->ActiveSwatch() <= rowEnd,
            TIMEOUT_MS);
        // Up from the first row has nowhere to go; Left at the first swatch
        // too.
        QTest::keyClick(window->Window(), Qt::Key_Up);
        QVERIFY(window->ActiveSwatch() >= 0);
        window->Swatch(0)->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window->Window(), Qt::Key_Left);
        QCOMPARE(window->ActiveSwatch(), 0);
    }
};

QTEST_MAIN(FocusColorSettingsTest)
#include "focus_color_settings_test.moc"
