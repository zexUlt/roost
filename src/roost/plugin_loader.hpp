#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

#include "api/plugin_service.hpp"
#include "qml_api/roost_singleton.hpp"

namespace roost {

// The window's loader of the QML panels of plugins (design.md §5): from the
// list roostd gives (PluginService) it makes, for each plugin, a QQmlContext
// and, for each panel type that the plugin contributes, a QQmlComponent, and
// it creates and destroys panel instances on request. Plain C++, not a QML
// type. GUI thread only.
//
// The loader validates `contributes.panels` itself: roostd checks only that
// it is an array. A broken panel disables only itself. The failure is logged
// (qWarning) and goes to Problems(); other panels of the plugin and other
// plugins load.
//
// On each Changed of PluginService the list is compared per plugin id on
// (directory, version, contributes, panel ids the plugin lost to an earlier
// plugin): an unchanged plugin is not touched, its components and live panels
// stay; a removed plugin is unloaded; a changed one is unloaded and loaded
// again; a new one is loaded. The first plugin in load order that declares a
// panel id keeps it, whether or not its panel loads.
//
// The loader owns the instances it creates (C++ ownership); the caller only
// shows the item (setParentItem) and gives it back with DestroyPanel. The
// caller must not delete a panel item itself, and must not call DestroyPanel
// from inside a JS call or an event handler of the panel being destroyed.
//
// When a diff loads a plugin after anything was unloaded in the life of the
// loader, the component cache of the engine is cleared once, so that edited
// QML is read from disk again. Qt clears only what nothing references: the
// components of the plugins that stay keep their cache entries.
//
// It must be destroyed before the engine: its contexts and components belong
// to the engine, and deleting them after it is undefined.
class PluginLoader : public QObject {
    Q_OBJECT

public:
    // A panel that cannot be offered. Panel is empty when the entry has no
    // usable id.
    struct PanelProblem {
        QString Plugin;
        QString Panel;
        QString Message;
    };

    PluginLoader(QQmlEngine& engine, api::PluginService& plugins,
                 RoostSingleton& roost, QObject* parent = nullptr);
    ~PluginLoader() override;

    // The ids of the panel types that can be created, in load order of their
    // plugins, then in the order of the manifest.
    QStringList PanelTypes() const;

    const QList<PanelProblem>& Problems() const { return Problems_; }

    // A new instance of the panel type, or nullptr: the type is unknown or
    // disabled, or the creation failed (the root is not a QQuickItem, or the
    // component could not make it). A failure is a problem and disables the
    // type until its plugin is loaded again. The panel gets its own
    // QQmlContext, registered as the owner of its one-shot calls; if the root
    // has a property `pluginId` (also a `required` one), it is set to the id
    // of the plugin before Component.onCompleted. A plugin that is being
    // unloaded gives nothing: a slot of PanelAboutToBeDestroyed gets nullptr.
    QQuickItem* CreatePanel(const QString& panelId);

    // Tears the instance down: PanelAboutToBeDestroyed (the item is alive, a
    // slot drops it and its bindings), then the owner of its calls, the item,
    // its context. Not through deleteLater. Does nothing for an item that is
    // not an instance of this loader. The caller must not call it from inside
    // a JS call or an event handler of the panel being destroyed (its frames
    // would run on a deleted object); a handler of the shell may.
    void DestroyPanel(QQuickItem* panel);

signals:
    // The set of types that can be created changed, or a plugin was loaded
    // again (its instances are gone). Once per list that PluginService gave,
    // after all loads.
    void PanelTypesChanged();
    void ProblemsChanged();
    // Before an instance is deleted, with the item alive. Also while a plugin
    // is unloaded. The slot must be direct: after the signal the item is gone.
    void PanelAboutToBeDestroyed(QQuickItem* panel);

private:
    struct PanelType {
        QString Id;
        // Null when the component could not be made.
        std::unique_ptr<QQmlComponent> Component;
        // The creation failed once; until the plugin is loaded again.
        bool Disabled = false;
    };

    struct Instance {
        QPointer<QQuickItem> Item;
        QPointer<QObject> Owner;
        QPointer<QQmlContext> Context;
    };

    struct Plugin {
        QString Id;
        QString Version;
        QString Directory;
        QJsonObject Contributes;
        // Panel id -> the plugin that has it.
        QMap<QString, QString> Rejected;
        std::unique_ptr<QQmlContext> Context;
        std::vector<PanelType> Panels;
        std::vector<Instance> Instances;
        // The ids this plugin has taken so far while loading.
        QSet<QString> Claimed;
        // Being torn down: CreatePanel skips the plugin.
        bool Unloading = false;
    };

    struct Declared {
        QString Id;
        QString Version;
        QString Directory;
        QJsonObject Contributes;
        QMap<QString, QString> Rejected;
    };

    void OnPluginsChanged();
    std::vector<Declared> ReadDeclared() const;
    std::unique_ptr<Plugin> Load(const Declared& declared);
    void LoadPanel(Plugin& plugin, const QJsonValue& entry);
    // Destroys the instances, the components, the context and the problems.
    // Marks the plugin Unloading first.
    void Unload(Plugin& plugin);
    void DestroyInstance(Instance& instance);
    void AddProblem(const QString& plugin, const QString& panel,
                    const QString& message);
    void DropProblems(const QString& plugin);

    QQmlEngine& Engine_;
    api::PluginService& Plugins_;
    RoostSingleton& Roost_;
    // In load order.
    std::vector<std::unique_ptr<Plugin>> Loaded_;
    QList<PanelProblem> Problems_;
    bool ProblemsDirty_ = false;
    // Something was unloaded earlier: a load clears the component cache.
    bool EverUnloaded_ = false;
};

}  // namespace roost
