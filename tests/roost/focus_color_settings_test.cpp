// The colour of the focus ring (settings.focusColor, SettingsWindow.qml,
// Theme::follow): Base, Contrast and Custom change the `focus` token of every
// window, any "#rrggbb" is applied, and a colour below the contrast of
// 18-style.md only adds a warning to the problems of the theme; what is not a
// colour is refused with its reason and the token is the theme's own. The
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
};

QTEST_MAIN(FocusColorSettingsTest)
#include "focus_color_settings_test.moc"
