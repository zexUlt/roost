#include "roost/plugin_loader.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJSEngine>
#include <QJsonArray>
#include <QMetaObject>
#include <QSet>
#include <QUrl>
#include <QtLogging>

#include <algorithm>
#include <map>

namespace roost {

namespace {

// The id of a panel entry if it has a usable one.
QString PanelIdOf(const QJsonValue& entry) {
    if (!entry.isObject()) {
        return {};
    }
    const QJsonValue id = entry.toObject().value("id");
    return id.isString() ? id.toString() : QString();
}

// The path a manifest names, resolved, if it stays inside the plugin
// directory. The same rule as roostd's check of backend paths: on the text,
// symlinks are not followed.
QString InsidePlugin(const QString& directory, const QString& path) {
    if (directory.isEmpty() || !QDir::isAbsolutePath(directory)) {
        return {};
    }
    const QString resolved = QDir::cleanPath(directory + u'/' + path);
    if (QDir::isAbsolutePath(path) || !resolved.startsWith(directory + u'/')) {
        return {};
    }
    return resolved;
}

}  // namespace

PluginLoader::PluginLoader(QQmlEngine& engine, api::PluginService& plugins,
                           RoostSingleton& roost, QObject* parent)
    : QObject(parent), Engine_(engine), Plugins_(plugins), Roost_(roost) {
    connect(&Plugins_, &api::PluginService::Changed, this,
            &PluginLoader::OnPluginsChanged);
    if (Plugins_.IsLoaded()) {
        OnPluginsChanged();
    }
}

PluginLoader::~PluginLoader() {
    for (const auto& plugin : Loaded_) {
        plugin->Unloading = true;
    }
    for (const auto& plugin : Loaded_) {
        Unload(*plugin);
    }
}

QStringList PluginLoader::PanelTypes() const {
    QStringList types;
    for (const auto& plugin : Loaded_) {
        for (const PanelType& type : plugin->Panels) {
            if (!type.Disabled) {
                types.append(type.Id);
            }
        }
    }
    return types;
}

std::optional<PluginLoader::PanelInfo> PluginLoader::Info(
    const QString& panelId) const {
    for (const auto& plugin : Loaded_) {
        for (const PanelType& type : plugin->Panels) {
            if (type.Id == panelId && !type.Disabled) {
                return PanelInfo{plugin->Id, type.Inputs, type.Outputs};
            }
        }
    }
    return std::nullopt;
}

std::vector<PluginLoader::Declared> PluginLoader::ReadDeclared() const {
    std::vector<Declared> declared;
    QSet<QString> seen;
    // Panel id -> the plugin that declared it first.
    QMap<QString, QString> claims;
    const QJsonArray list = Plugins_.Plugins().value("plugins").toArray();
    for (const QJsonValue& entry : list) {
        const QJsonObject object = entry.toObject();
        const QString id = object.value("id").toString();
        if (id.isEmpty() || seen.contains(id)) {
            qWarning("roost: plugins.list has an entry without a usable id");
            continue;
        }
        seen.insert(id);
        Declared plugin{id,
                        object.value("version").toString(),
                        object.value("directory").toString(),
                        object.value("contributes").toObject(),
                        {}};
        for (const QJsonValue& panel :
             plugin.Contributes.value("panels").toArray()) {
            const QString panelId = PanelIdOf(panel);
            if (panelId.isEmpty()) {
                continue;
            }
            const auto claim = claims.constFind(panelId);
            if (claim == claims.constEnd()) {
                claims.insert(panelId, id);
            } else if (claim.value() != id) {
                plugin.Rejected.insert(panelId, claim.value());
            }
        }
        declared.push_back(std::move(plugin));
    }
    return declared;
}

void PluginLoader::OnPluginsChanged() {
    const std::vector<Declared> declared = ReadDeclared();
    const QStringList typesBefore = PanelTypes();
    bool reloaded = false;

    // Mark what is gone or changed before tearing anything down: a slot of
    // PanelAboutToBeDestroyed may call CreatePanel.
    std::vector<Plugin*> doomed;
    for (const auto& plugin : Loaded_) {
        const auto same = std::ranges::find_if(
            declared, [&](const Declared& d) { return d.Id == plugin->Id; });
        if (same != declared.end() && same->Version == plugin->Version &&
            same->Directory == plugin->Directory &&
            same->Contributes == plugin->Contributes &&
            same->Rejected == plugin->Rejected) {
            continue;
        }
        reloaded = reloaded || same != declared.end();
        plugin->Unloading = true;
        doomed.push_back(plugin.get());
    }
    // An unloaded plugin stays in Loaded_, empty, until the list is rebuilt.
    for (Plugin* plugin : doomed) {
        Unload(*plugin);
    }
    EverUnloaded_ = EverUnloaded_ || !doomed.empty();
    std::erase_if(Loaded_,
                  [](const auto& plugin) { return plugin->Unloading; });

    // Loaded_ holds only the plugins that stay, none moved from, while Load
    // runs. The new ones wait here until the list is put together.
    QSet<QString> kept;
    for (const auto& plugin : Loaded_) {
        kept.insert(plugin->Id);
    }
    std::map<QString, std::unique_ptr<Plugin>> fresh;
    for (const Declared& d : declared) {
        if (kept.contains(d.Id)) {
            continue;
        }
        // Once per diff, so that edited QML is read from disk again.
        if (EverUnloaded_ && fresh.empty()) {
            Engine_.clearComponentCache();
        }
        fresh[d.Id] = Load(d);
    }

    std::vector<std::unique_ptr<Plugin>> ordered;
    for (const Declared& d : declared) {
        // Nothing runs from here on, so the moved-from entries are harmless.
        const auto old = std::ranges::find_if(Loaded_, [&](const auto& plugin) {
            return plugin && plugin->Id == d.Id;
        });
        ordered.push_back(old != Loaded_.end() ? std::move(*old)
                                               : std::move(fresh.at(d.Id)));
    }
    Loaded_ = std::move(ordered);

    if (ProblemsDirty_) {
        ProblemsDirty_ = false;
        emit ProblemsChanged();
    }
    if (reloaded || PanelTypes() != typesBefore) {
        emit PanelTypesChanged();
    }
}

std::unique_ptr<PluginLoader::Plugin> PluginLoader::Load(
    const Declared& declared) {
    auto plugin = std::make_unique<Plugin>();
    plugin->Id = declared.Id;
    plugin->Version = declared.Version;
    plugin->Directory = declared.Directory;
    plugin->Contributes = declared.Contributes;
    plugin->Rejected = declared.Rejected;
    plugin->Context = std::make_unique<QQmlContext>(Engine_.rootContext());
    Plugin& ref = *plugin;

    const QJsonValue panels = ref.Contributes.value("panels");
    if (!panels.isUndefined() && !panels.isArray()) {
        AddProblem(ref.Id, {}, QStringLiteral("\"panels\" must be an array"));
    }
    for (const QJsonValue& entry : panels.toArray()) {
        LoadPanel(ref, entry);
    }
    return plugin;
}

void PluginLoader::LoadPanel(Plugin& plugin, const QJsonValue& entry) {
    if (!entry.isObject()) {
        AddProblem(plugin.Id, {},
                   QStringLiteral("a panel entry must be an object"));
        return;
    }
    const QJsonObject panel = entry.toObject();
    const QJsonValue idValue = panel.value("id");
    if (!idValue.isString() || idValue.toString().isEmpty()) {
        AddProblem(plugin.Id, {},
                   QStringLiteral("\"id\" must be a non-empty string"));
        return;
    }
    const QString id = idValue.toString();
    const auto taken = plugin.Rejected.constFind(id);
    if (taken != plugin.Rejected.constEnd()) {
        AddProblem(plugin.Id, id,
                   QStringLiteral("panel id is already taken by plugin \"%1\"")
                       .arg(taken.value()));
        return;
    }
    if (plugin.Claimed.contains(id)) {
        AddProblem(plugin.Id, id,
                   QStringLiteral("panel id is declared twice in the plugin"));
        return;
    }
    plugin.Claimed.insert(id);
    const QJsonValue qml = panel.value("qml");
    if (!qml.isString() || qml.toString().isEmpty()) {
        AddProblem(plugin.Id, id,
                   QStringLiteral("\"qml\" must be a non-empty string"));
        return;
    }
    for (const char* key : {"inputs", "outputs"}) {
        const QJsonValue value = panel.value(QLatin1String(key));
        if (!value.isUndefined() && !value.isObject()) {
            AddProblem(plugin.Id, id,
                       QStringLiteral("\"%1\" must be an object").arg(key));
            return;
        }
    }
    const QString path = InsidePlugin(plugin.Directory, qml.toString());
    if (path.isEmpty()) {
        AddProblem(plugin.Id, id,
                   QStringLiteral("\"qml\" must be a path inside the plugin "
                                  "directory"));
        return;
    }
    if (!QFileInfo(path).isFile()) {
        AddProblem(plugin.Id, id,
                   QStringLiteral("file %1 does not exist").arg(path));
        return;
    }
    auto component = std::make_unique<QQmlComponent>(
        &Engine_, QUrl::fromLocalFile(path), QQmlComponent::PreferSynchronous);
    if (component->status() != QQmlComponent::Ready) {
        QString message = component->errorString().trimmed();
        if (message.isEmpty()) {
            message = QStringLiteral("the component is not ready");
        }
        AddProblem(plugin.Id, id, message);
        return;
    }
    PanelType type;
    type.Id = id;
    type.Inputs = panel.value("inputs").toObject().toVariantMap();
    type.Outputs = panel.value("outputs").toObject().toVariantMap();
    type.Component = std::move(component);
    plugin.Panels.push_back(std::move(type));
}

QQuickItem* PluginLoader::CreatePanel(const QString& panelId) {
    for (const auto& plugin : Loaded_) {
        if (plugin->Unloading) {
            continue;
        }
        for (PanelType& type : plugin->Panels) {
            if (type.Id != panelId || type.Disabled) {
                continue;
            }
            const auto fail = [&](const QString& message) -> QQuickItem* {
                type.Disabled = true;
                AddProblem(plugin->Id, panelId, message);
                ProblemsDirty_ = false;
                emit ProblemsChanged();
                emit PanelTypesChanged();
                return nullptr;
            };
            auto* context = new QQmlContext(plugin->Context.get());
            QObject* owner = Roost_.registerCallOwner(context);
            QObject* object = type.Component->beginCreate(context);
            if (object == nullptr) {
                const QString message = type.Component->errorString().trimmed();
                delete context;
                return fail(message.isEmpty()
                                ? QStringLiteral("the panel could not be "
                                                 "created")
                                : message);
            }
            QJSEngine::setObjectOwnership(object, QJSEngine::CppOwnership);
            if (object->metaObject()->indexOfProperty("pluginId") >= 0) {
                // An initial property, so that `required` is satisfied.
                type.Component->setInitialProperties(
                    object, {{QStringLiteral("pluginId"), plugin->Id}});
            }
            type.Component->completeCreate();
            auto* item = qobject_cast<QQuickItem*>(object);
            if (item == nullptr) {
                delete object;
                delete context;
                return fail(
                    QStringLiteral("the root of the panel must be an "
                                   "Item"));
            }
            plugin->Instances.push_back({item, owner, context});
            CheckDeclared(*plugin, type, *item);
            return item;
        }
    }
    return nullptr;
}

void PluginLoader::DestroyPanel(QQuickItem* panel) {
    if (panel == nullptr) {
        return;
    }
    for (const auto& plugin : Loaded_) {
        auto& instances = plugin->Instances;
        const auto found = std::ranges::find_if(
            instances, [&](const Instance& i) { return i.Item == panel; });
        if (found == instances.end()) {
            continue;
        }
        // Out of the list first: a slot of the signal may come back here.
        Instance instance = *found;
        instances.erase(found);
        DestroyInstance(instance);
        return;
    }
}

void PluginLoader::DestroyInstance(Instance& instance) {
    if (instance.Item) {
        emit PanelAboutToBeDestroyed(instance.Item);
    }
    delete instance.Owner.data();
    delete instance.Item.data();
    delete instance.Context.data();
}

void PluginLoader::Unload(Plugin& plugin) {
    plugin.Unloading = true;
    // A slot of the signal may come back; CreatePanel skips the plugin, so the
    // loop ends, but nothing is left in Instances when the context goes.
    while (!plugin.Instances.empty()) {
        std::vector<Instance> instances = std::move(plugin.Instances);
        plugin.Instances.clear();
        for (Instance& instance : instances) {
            DestroyInstance(instance);
        }
    }
    plugin.Panels.clear();
    plugin.Context.reset();
    DropProblems(plugin.Id);
}

void PluginLoader::AddProblem(const QString& plugin, const QString& panel,
                              const QString& message) {
    qWarning("roost: plugin %s, panel %s: %s", qPrintable(plugin),
             qPrintable(panel.isEmpty() ? QStringLiteral("(no id)") : panel),
             qPrintable(message));
    Problems_.append({plugin, panel, message});
    ProblemsDirty_ = true;
}

void PluginLoader::CheckDeclared(const Plugin& plugin, const PanelType& type,
                                 const QQuickItem& item) {
    bool added = false;
    const auto check = [&](const QVariantMap& declared, const char* kind) {
        for (auto it = declared.begin(); it != declared.end(); ++it) {
            if (item.metaObject()->indexOfProperty(qPrintable(it.key())) >= 0) {
                continue;
            }
            const QString message =
                QStringLiteral(
                    "declared %1 \"%2\" is not a property of the "
                    "root")
                    .arg(QLatin1String(kind), it.key());
            const bool known = std::ranges::any_of(
                Problems_, [&](const PanelProblem& problem) {
                    return problem.Plugin == plugin.Id &&
                           problem.Panel == type.Id &&
                           problem.Message == message;
                });
            if (!known) {
                AddProblem(plugin.Id, type.Id, message);
                added = true;
            }
        }
    };
    check(type.Inputs, "input");
    check(type.Outputs, "output");
    if (added) {
        ProblemsDirty_ = false;
        emit ProblemsChanged();
    }
}

void PluginLoader::DropProblems(const QString& plugin) {
    const qsizetype before = Problems_.size();
    Problems_.removeIf(
        [&](const PanelProblem& problem) { return problem.Plugin == plugin; });
    if (Problems_.size() != before) {
        ProblemsDirty_ = true;
    }
}

}  // namespace roost
