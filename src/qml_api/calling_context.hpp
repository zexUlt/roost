#pragma once

#include <QHash>
#include <QJSEngine>
#include <QObject>
#include <QPointer>
#include <QQmlContext>
#include <QString>

namespace roost {

// The context of the JS code that is calling into C++ now, or nullptr when no
// JS code is (a C++ caller, or an event that came from outside). Call it inside
// a Q_INVOKABLE, before the JS call returns.
QQmlContext* callingContext(QJSEngine& engine);

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
    // `context` is null.
    QObject* add(QQmlContext* context);

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
};

}  // namespace roost
