// The icon of an agent in the sessions list (SessionsList::agentIcon, module
// RoostSessions): the user's folder agent-icons first (svg, then png), then
// the bundled icon, else the default; an id that is not a plain file name gets
// the default. The module is loaded from the plugin directory as the window
// does, so that the bundled icons are the real ones of its resources.

#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <memory>

#include "helpers/scoped_env.hpp"

using namespace roost::testing;

namespace {

const QString BUNDLED =
    QStringLiteral("qrc:/qt/qml/RoostSessions/agent-icons/");

}  // namespace

class AgentIconTest : public QObject {
    Q_OBJECT

private:
    // A list of module RoostSessions: a new one is a new panel, with no icon
    // remembered.
    std::unique_ptr<QObject> NewList() {
        Engines_.push_back(std::make_unique<QQmlEngine>());
        Engines_.back()->addImportPath(SESSIONS_QML_IMPORT_PATH);
        QQmlComponent component(Engines_.back().get());
        component.setData(
            "import QtQml\nimport RoostSessions\nSessionsList {}\n", QUrl());
        std::unique_ptr<QObject> list(component.create());
        if (list == nullptr) {
            qWarning() << component.errorString();
        }
        return list;
    }

    static QUrl Icon(QObject* list, const QString& agent) {
        QUrl url;
        const bool called = QMetaObject::invokeMethod(
            list, "agentIcon", Q_RETURN_ARG(QUrl, url), Q_ARG(QString, agent));
        return called ? url : QUrl("not-called");
    }

    static QUrl Bundled(QObject* list, const QString& agent) {
        QUrl url;
        const bool called = QMetaObject::invokeMethod(list, "bundledAgentIcon",
                                                      Q_RETURN_ARG(QUrl, url),
                                                      Q_ARG(QString, agent));
        return called ? url : QUrl("not-called");
    }

    bool Touch(const QString& name) const {
        QDir().mkpath(Config_->filePath("agent-icons"));
        QFile file(Config_->filePath("agent-icons/" + name));
        return file.open(QIODevice::WriteOnly) && file.write("x") == 1;
    }

    std::vector<std::unique_ptr<QQmlEngine>> Engines_;
    std::unique_ptr<QTemporaryDir> Config_;
    std::unique_ptr<ScopedEnv> Env_;

private slots:
    void init() {
        Config_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Config_->isValid());
        Env_ = std::make_unique<ScopedEnv>("ROOST_CONFIG",
                                           Config_->path().toUtf8());
    }

    void cleanup() {
        Engines_.clear();
        Env_.reset();
        Config_.reset();
    }

    void TheBundledIconsAreThoseOfTheAgentsAndTheDefaultIsForTheRest() {
        const auto list = NewList();
        QVERIFY(list != nullptr);
        QCOMPARE(Icon(list.get(), "claude"), QUrl(BUNDLED + "claude.svg"));
        QCOMPARE(Icon(list.get(), "codex"), QUrl(BUNDLED + "codex.svg"));
        QCOMPARE(Icon(list.get(), "aider"), QUrl(BUNDLED + "default.svg"));
        QCOMPARE(Icon(list.get(), ""), QUrl(BUNDLED + "default.svg"));
        QVERIFY(
            QFile::exists(":/qt/qml/RoostSessions/agent-icons/default.svg"));
    }

    void AUserSvgWinsOverTheBundledIcon() {
        QVERIFY(Touch("claude.svg"));
        QVERIFY(Touch("claude.png"));
        QVERIFY(Touch("aider.svg"));
        const auto list = NewList();
        QCOMPARE(
            Icon(list.get(), "claude"),
            QUrl::fromLocalFile(Config_->filePath("agent-icons/claude.svg")));
        QCOMPARE(
            Icon(list.get(), "aider"),
            QUrl::fromLocalFile(Config_->filePath("agent-icons/aider.svg")));
        QCOMPARE(Icon(list.get(), "codex"), QUrl(BUNDLED + "codex.svg"));
    }

    void AUserPngIsUsedWhenThereIsNoSvg() {
        QVERIFY(Touch("codex.png"));
        const auto list = NewList();
        QCOMPARE(
            Icon(list.get(), "codex"),
            QUrl::fromLocalFile(Config_->filePath("agent-icons/codex.png")));
    }

    void AUserDefaultReplacesTheBundledDefaultNotTheBundledIcons() {
        QVERIFY(Touch("default.png"));
        const auto png = NewList();
        const QUrl userPng =
            QUrl::fromLocalFile(Config_->filePath("agent-icons/default.png"));
        QCOMPARE(Icon(png.get(), "aider"), userPng);
        QCOMPARE(Icon(png.get(), "a b"), userPng);
        QCOMPARE(Icon(png.get(), "claude"), QUrl(BUNDLED + "claude.svg"));

        QVERIFY(Touch("default.svg"));
        QVERIFY(Touch("aider.svg"));
        const auto svg = NewList();
        QCOMPARE(Icon(svg.get(), "codex"), QUrl(BUNDLED + "codex.svg"));
        QCOMPARE(
            Icon(svg.get(), "aider"),
            QUrl::fromLocalFile(Config_->filePath("agent-icons/aider.svg")));
        QCOMPARE(
            Icon(svg.get(), "other"),
            QUrl::fromLocalFile(Config_->filePath("agent-icons/default.svg")));
        // The bundled icon is the one to fall back to, whatever the user has.
        QCOMPARE(Bundled(svg.get(), "other"), QUrl(BUNDLED + "default.svg"));
        QCOMPARE(Bundled(svg.get(), "claude"), QUrl(BUNDLED + "claude.svg"));
        QCOMPARE(Bundled(svg.get(), "../x"), QUrl(BUNDLED + "default.svg"));
    }

    void WithoutAHomeDirectoryOnlyTheBundledIconsAreUsed() {
        ScopedEnv roost("ROOST_CONFIG", std::nullopt);
        ScopedEnv xdg("XDG_CONFIG_HOME", std::nullopt);
        for (const char* home : {"", "relative/home", "/"}) {
            ScopedEnv env("HOME", QByteArray(home));
            const auto list = NewList();
            QCOMPARE(Icon(list.get(), "claude"), QUrl(BUNDLED + "claude.svg"));
            QCOMPARE(Icon(list.get(), "aider"), QUrl(BUNDLED + "default.svg"));
        }
        // A good HOME gives ~/.config/roost/agent-icons.
        ScopedEnv home("HOME", Config_->path().toUtf8());
        QVERIFY(QDir().mkpath(Config_->filePath(".config/roost/agent-icons")));
        QFile file(Config_->filePath(".config/roost/agent-icons/claude.svg"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto list = NewList();
        QCOMPARE(Icon(list.get(), "claude"),
                 QUrl::fromLocalFile(Config_->filePath(
                     ".config/roost/agent-icons/claude.svg")));
    }

    void ADirectoryOfTheNameIsNotAnIcon() {
        QVERIFY(QDir().mkpath(Config_->filePath("agent-icons/claude.svg")));
        const auto list = NewList();
        QCOMPARE(Icon(list.get(), "claude"), QUrl(BUNDLED + "claude.svg"));
    }

    void AnIdThatIsNotAPlainFileNameGetsTheDefault() {
        // Files a path trick would reach: next to the folder, and in it.
        QVERIFY(Touch("x.svg"));
        QFile outside(Config_->filePath("x.svg"));
        QVERIFY(outside.open(QIODevice::WriteOnly));
        QVERIFY(QDir().mkpath(Config_->filePath("agent-icons/a")));
        QFile nested(Config_->filePath("agent-icons/a/b.svg"));
        QVERIFY(nested.open(QIODevice::WriteOnly));
        const auto list = NewList();
        for (const QString& agent :
             {QStringLiteral("../x"), QStringLiteral("a/b"),
              QStringLiteral("a\\b"), QStringLiteral(".."), QStringLiteral("."),
              QStringLiteral("a b"), QStringLiteral("claude\n"),
              QStringLiteral("é")}) {
            QCOMPARE(Icon(list.get(), agent), QUrl(BUNDLED + "default.svg"));
        }
    }

    void TheFolderIsReadAgainByANewListNotByTheOldOne() {
        const auto before = NewList();
        QCOMPARE(Icon(before.get(), "claude"), QUrl(BUNDLED + "claude.svg"));
        QVERIFY(Touch("claude.svg"));
        QCOMPARE(Icon(before.get(), "claude"), QUrl(BUNDLED + "claude.svg"));
        const auto after = NewList();
        QCOMPARE(
            Icon(after.get(), "claude"),
            QUrl::fromLocalFile(Config_->filePath("agent-icons/claude.svg")));
    }

    void TheFolderFollowsTheXdgRuleWithoutRoostConfig() {
        ScopedEnv roost("ROOST_CONFIG", std::nullopt);
        ScopedEnv xdg("XDG_CONFIG_HOME", Config_->path().toUtf8());
        QDir().mkpath(Config_->filePath("roost/agent-icons"));
        QFile file(Config_->filePath("roost/agent-icons/codex.svg"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto list = NewList();
        QCOMPARE(Icon(list.get(), "codex"),
                 QUrl::fromLocalFile(
                     Config_->filePath("roost/agent-icons/codex.svg")));
    }
};

QTEST_MAIN(AgentIconTest)
#include "agent_icon_test.moc"
