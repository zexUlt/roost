// templates/plugin-qml: a copy of the template with another id, put into the
// plugins directory of the real roostd, loads without problems, and its panel
// opens in a window with the real PanelSlot, the Shell and the PluginLoader.
// What the user sees is read from the panel: the id of the plugin as the
// window gave it and the state of the connection.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <memory>

#include "api/api.hpp"
#include "api/api_test_support.hpp"
#include "helpers/roostd_process.hpp"
#include "qml_api/roost_singleton.hpp"
#include "roost/plugin_loader.hpp"
#include "roost/shell.hpp"

using namespace roost;
using namespace roost::api_test;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

// The id of the copy, not the one the template has.
const QString COPY_ID = "my-notes";
// The id of the panel of the copy: the id of the plugin and the name the
// template gives the panel.
const QString PANEL = COPY_ID + ".hello";

// A window of the shell's kind with one slot, for the panel under test. The
// slot is the real PanelSlot; the properties are the ones Shell writes.
QByteArray WindowFor(const QString& panel) {
    return QByteArray(R"(
import QtQuick
import Roost.Shell
Window {
    property string connection: "connecting"
    property string connectionMessage: ""
    property var problems: []
    signal retryRequested
    width: 400
    height: 300
    visible: true
    PanelSlot {
        objectName: "panelSlot"
        panelType: "@PANEL@"
        anchors.fill: parent
    }
}
)")
        .replace("@PANEL@", panel.toUtf8());
}

bool CopyTree(const QString& from, const QString& to) {
    QDirIterator files(from, QDir::Files, QDirIterator::Subdirectories);
    const QDir source(from);
    while (files.hasNext()) {
        const QString path = files.next();
        const QString target = to + u'/' + source.relativeFilePath(path);
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) ||
            !QFile::copy(path, target)) {
            return false;
        }
    }
    return true;
}

QByteArray ReadFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool WriteFile(const QString& path, const QByteArray& content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(content) == content.size();
}

}  // namespace

class PluginTemplateTest : public QObject {
    Q_OBJECT

private:
    // Copies the template into the plugins directory of roostd, as the README
    // says: only the id of the plugin is rewritten in plugin.json; the panel
    // keeps its name, relative to the plugin.
    bool InstallCopy(const QString& id) {
        const QString target = Daemon_->PluginsPath() + "/copy";
        if (!CopyTree(QStringLiteral(PLUGIN_TEMPLATE_DIR), target)) {
            return false;
        }
        const QString manifestPath = target + "/plugin.json";
        QJsonObject manifest =
            QJsonDocument::fromJson(ReadFile(manifestPath)).object();
        // The template must have the id to be rewritten, not get one here.
        if (!manifest.contains("id")) {
            return false;
        }
        manifest["id"] = id;
        return WriteFile(manifestPath, QJsonDocument(manifest).toJson());
    }

    bool Boot() {
        if (!Daemon_->IsValid() || !Daemon_->Start(ROOSTD_PATH)) {
            return false;
        }
        Api_ = std::make_unique<api::Api>();
        Engine_ = std::make_unique<QQmlApplicationEngine>();
        Singleton_ =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        if (Singleton_ == nullptr) {
            return false;
        }
        Singleton_->setApi(*Api_);
        QQmlComponent window(Engine_.get());
        window.setData(WindowFor(PANEL), QUrl());
        Window_.reset(qobject_cast<QQuickWindow*>(window.create()));
        if (!Window_) {
            qWarning("%s", qPrintable(window.errorString()));
            return false;
        }
        Loader_ = std::make_unique<PluginLoader>(*Engine_, Api_->Plugins(),
                                                 *Singleton_);
        Shell_ = std::make_unique<Shell>(*Window_, *Loader_, *Api_,
                                         Daemon_->SocketPath());
        Api_->Connect(Daemon_->SocketPath());
        return true;
    }

    QQuickItem* Slot() { return Window_->findChild<QQuickItem*>("panelSlot"); }

    QQuickItem* Panel() {
        return Slot()->property("panel").value<QQuickItem*>();
    }

    QString TextOf(const char* name) {
        auto* text = Panel() == nullptr ? nullptr
                                        : Panel()->findChild<QQuickItem*>(name);
        return text == nullptr ? QStringLiteral("(none)")
                               : text->property("text").toString();
    }

    // The plugins of roostd's answer to plugins.list, as the window has it.
    QJsonArray Listed() {
        return Api_->Plugins().Plugins()["plugins"].toArray();
    }

private slots:
    void init() { Daemon_ = std::make_unique<roost::testing::RoostdProcess>(); }

    void cleanup() {
        // Before the engine, as documented; the Api outlives the engine.
        Shell_.reset();
        Loader_.reset();
        Window_.reset();
        Engine_.reset();
        Singleton_ = nullptr;
        Api_.reset();
        Daemon_.reset();
    }

    void CopyWithAnotherIdLoadsAndItsPanelOpens() {
        QVERIFY(Daemon_->IsValid());
        QVERIFY(InstallCopy(COPY_ID));
        QVERIFY(Boot());
        QTRY_VERIFY_WITH_TIMEOUT(Api_->Plugins().IsLoaded(), TIMEOUT_MS);

        // roostd: the copy is there under its id, alone, and nothing is wrong.
        QCOMPARE(Listed().size(), 1);
        const QJsonObject listed = Listed().first().toObject();
        QCOMPARE(listed["id"].toString(), COPY_ID);
        QCOMPARE(listed["contributes"]
                     .toObject()["panels"]
                     .toArray()
                     .first()
                     .toObject()["id"]
                     .toString(),
                 QString("hello"));
        QVERIFY2(Api_->Plugins().Plugins()["problems"].toArray().isEmpty(),
                 qPrintable(QJsonDocument(
                                Api_->Plugins().Plugins()["problems"].toArray())
                                .toJson()));

        // The window: the panel is in the slot and shows what the window gave
        // it.
        QTRY_VERIFY_WITH_TIMEOUT(Panel() != nullptr, TIMEOUT_MS);
        QVERIFY(Loader_->Problems().isEmpty());
        QCOMPARE(Slot()->property("error").toString(), QString());
        QVERIFY(QTest::qWaitForWindowExposed(Window_.get()));
        QVERIFY(Panel()->isVisible());
        QVERIFY(Panel()->width() > 0 && Panel()->height() > 0);
        QCOMPARE(TextOf("pluginIdText"), "Plugin " + COPY_ID);
        QTRY_COMPARE_WITH_TIMEOUT(TextOf("connectionText"),
                                  QString("connected to roostd"), TIMEOUT_MS);

        // The text follows the connection: it is not a constant.
        QVERIFY(Daemon_->Stop());
        QTRY_COMPARE_WITH_TIMEOUT(TextOf("connectionText"),
                                  QString("not connected"), TIMEOUT_MS);
    }

private:
    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<QQmlApplicationEngine> Engine_;
    RoostSingleton* Singleton_ = nullptr;
    std::unique_ptr<QQuickWindow> Window_;
    std::unique_ptr<PluginLoader> Loader_;
    std::unique_ptr<Shell> Shell_;
};

QTEST_MAIN(PluginTemplateTest)
#include "plugin_template_test.moc"
