#include "qml_api/calling_context.hpp"

#include <QFileInfo>
#include <QUrl>
#include <QtLogging>

// The only file that includes private Qt headers. Qt has no public way to ask
// which QQmlContext the calling JS code belongs to, and the owner of a call
// cannot be told without it. The price is a dependency on the Qt version:
// QT_VERSION in .github/workflows/ci.yml pins it, and the test
// roost_call_owner_test breaks if a Qt update changes this behaviour.
#include <private/qqmlcontextdata_p.h>
#include <private/qv4engine_p.h>
#include <private/qv4stackframe_p.h>

namespace roost {

QQmlContext* callingContext(QJSEngine& engine) {
    QV4::ExecutionEngine* v4 = engine.handle();
    if (v4 == nullptr) {
        return nullptr;
    }
    const auto data = v4->callingQmlContext();
    return data ? data->asQQmlContext() : nullptr;
}

QList<QQmlContext*> callingContexts(QJSEngine& engine) {
    QList<QQmlContext*> contexts;
    QV4::ExecutionEngine* v4 = engine.handle();
    if (v4 == nullptr) {
        return contexts;
    }
    // callingQmlContext reads the frame the engine is at: step it up the
    // chain of the JS frames, and put it back whatever happens.
    struct Restore {
        QV4::ExecutionEngine* Engine;
        QV4::CppStackFrame* Frame;
        ~Restore() { Engine->currentStackFrame = Frame; }
    } restore{v4, v4->currentStackFrame};
    for (QV4::CppStackFrame* frame = restore.Frame; frame != nullptr;
         frame = frame->parentFrame()) {
        v4->currentStackFrame = frame;
        const auto data = v4->callingQmlContext();
        QQmlContext* context = data ? data->asQQmlContext() : nullptr;
        if (context != nullptr && !contexts.contains(context)) {
            contexts.append(context);
        }
    }
    return contexts;
}

CallOwners::CallOwners(QObject* parent) : QObject(parent) {}

void CallOwners::bindPanel(const QObject* owner, QObject* panel) {
    if (owner == nullptr) {
        return;
    }
    if (panel == nullptr) {
        Panels_.remove(owner);
        return;
    }
    Panels_.insert(owner, panel);
    // The owner is a child of the context: it is gone with the panel.
    connect(owner, &QObject::destroyed, this,
            [this, owner] { Panels_.remove(owner); });
}

QObject* CallOwners::panelOf(const QObject* owner) const {
    return Panels_.value(owner).data();
}

QString CallOwners::pluginOf(const QObject* owner) const {
    return Plugins_.value(owner);
}

QObject* CallOwners::add(QQmlContext* context, const QString& plugin) {
    if (context == nullptr) {
        qWarning("roost: registerCallOwner needs a context");
        return nullptr;
    }
    if (!Owners_.contains(context)) {
        // The key only: by the time `destroyed` fires the object is no longer
        // a QQmlContext, so it is not cast back to one.
        connect(context, &QObject::destroyed, this,
                [this, context] { Owners_.remove(context); });
    }
    auto* owner = new QObject(context);
    Owners_.insert(context, owner);
    if (!plugin.isEmpty()) {
        Plugins_.insert(owner, plugin);
        // The key only, as above: the owner is gone with the context.
        connect(owner, &QObject::destroyed, this,
                [this, owner] { Plugins_.remove(owner); });
    }
    return owner;
}

QString CallOwners::pluginAbove(QQmlContext* context) const {
    for (QQmlContext* above = context; above != nullptr;
         above = above->parentContext()) {
        const auto found = Owners_.constFind(above);
        if (found != Owners_.constEnd() && !found.value().isNull()) {
            const QString plugin = Plugins_.value(found.value().data());
            if (!plugin.isEmpty()) {
                return plugin;
            }
        }
    }
    // The file the code is in: baseUrl is the file of the nearest context
    // that has one.
    if (context == nullptr || Directories_.isEmpty()) {
        return {};
    }
    const QUrl url = context->baseUrl();
    if (!url.isLocalFile()) {
        return {};
    }
    const QString path = QFileInfo(url.toLocalFile()).canonicalFilePath();
    for (auto it = Directories_.constBegin(); it != Directories_.constEnd();
         ++it) {
        if (path.startsWith(it.value())) {
            return it.key();
        }
    }
    return {};
}

void CallOwners::addPluginDirectory(const QString& plugin,
                                    const QString& directory) {
    const QString canonical = QFileInfo(directory).canonicalFilePath();
    if (canonical.isEmpty()) {
        return;
    }
    Directories_.insert(plugin, canonical + u'/');
}

void CallOwners::dropPluginDirectory(const QString& plugin) {
    Directories_.remove(plugin);
}

QString CallOwners::callingPlugin(QJSEngine& engine) const {
    for (QQmlContext* context : callingContexts(engine)) {
        const QString plugin = pluginAbove(context);
        if (!plugin.isEmpty()) {
            return plugin;
        }
    }
    return {};
}

QObject* CallOwners::contextFor(QJSEngine& engine, QObject* fallback,
                                const QString& what) const {
    return contextFor(callingContext(engine), fallback, what);
}

QObject* CallOwners::contextFor(QQmlContext* calling, QObject* fallback,
                                const QString& what) const {
    for (QQmlContext* context = calling; context != nullptr;
         context = context->parentContext()) {
        const auto found = Owners_.constFind(context);
        if (found == Owners_.constEnd()) {
            continue;
        }
        if (found.value().isNull()) {
            qDebug("roost: %s dropped: its panel is being unloaded",
                   qPrintable(what));
            return nullptr;
        }
        return found.value().data();
    }
    return fallback;
}

}  // namespace roost
