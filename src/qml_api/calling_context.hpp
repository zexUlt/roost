#pragma once

#include <QHash>
#include <QJSEngine>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQmlContext>
#include <QString>

namespace roost {

// The context of the JS code that is calling into C++ now, or nullptr when no
// JS code is (a C++ caller, or an event that came from outside). Call it inside
// a Q_INVOKABLE, before the JS call returns.
QQmlContext* callingContext(QJSEngine& engine);

// The contexts of every JS frame that is running now, the innermost first, each
// once: the function that is calling into C++, and the functions that called
// it. Empty when no JS code is running.
QList<QQmlContext*> callingContexts(QJSEngine& engine);

// Who owns the calls that QML code makes. A panel of a plugin has its own
// QQmlContext; registering it creates an owner object that belongs to the
// context (its child). A one-shot call from the panel (and from any object
// created in it) belongs to the owner, so that the answer does not outlive the
// panel. A call from anywhere else (the shell QML, C++) belongs to the fallback
// the adapter gives. GUI thread only.
class CallOwners : public QObject {
public:
    explicit CallOwners(QObject* parent = nullptr);

    // Makes the owner of the calls made from `context` or a context below it:
    // a child of `context`, so destroying the context destroys it too. The
    // owner may be deleted earlier to start unloading; calls made after that
    // are dropped. The entry goes when the context is destroyed. nullptr when
    // `context` is null. `plugin` is the id of the plugin whose panel it is
    // (empty for code that is not a plugin's): pluginOf says it back.
    QObject* add(QQmlContext* context, const QString& plugin = {});

    // The plugin whose panel `owner` serves, empty for the window and for an
    // owner made with no plugin.
    QString pluginOf(const QObject* owner) const;

    // The plugin whose code is running: the nearest registered context above
    // `context`, or above any context of a function that called the running
    // one (a plugin's page that calls a function of the window is still the
    // plugin's call). Empty when the window's own code is running alone. It
    // is a guard against a plugin's mistake: code in the same engine is no
    // sandbox, and an answer that comes later has no calling frames.
    QString pluginAbove(QQmlContext* context) const;

    // The files under `directory` are the plugin's: QML that no panel made
    // (a singleton of the plugin's own module, made in the root context of the
    // engine) is the plugin's by the file it is written in. Dropped when the
    // plugin is unloaded.
    void addPluginDirectory(const QString& plugin, const QString& directory);
    void dropPluginDirectory(const QString& plugin);
    QString callingPlugin(QJSEngine& engine) const;

    // Tells that `owner` serves the panel `panel` (the item made in the
    // context of `owner`). Weak: the panel may go first.
    void bindPanel(const QObject* owner, QObject* panel);

    // The panel that `owner` serves, or nullptr.
    QObject* panelOf(const QObject* owner) const;

    // The object to use as the `context` of a call made by the JS code that is
    // calling now: the owner of the nearest registered context, `fallback` when
    // no registered context is on the chain, nullptr when the call must be
    // dropped (the context is registered but its owner is gone: the panel is
    // being torn down). `what` names the call in the log.
    QObject* contextFor(QJSEngine& engine, QObject* fallback,
                        const QString& what) const;

    // The same for a known calling context.
    QObject* contextFor(QQmlContext* calling, QObject* fallback,
                        const QString& what) const;

private:
    QHash<QQmlContext*, QPointer<QObject>> Owners_;
    QHash<const QObject*, QPointer<QObject>> Panels_;
    QHash<const QObject*, QString> Plugins_;
    // Plugin id to the canonical path of its directory, with a trailing slash.
    QHash<QString, QString> Directories_;
};

}  // namespace roost
