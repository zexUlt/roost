#include "qml_api/calling_context.hpp"

#include <QtLogging>

// The only file that includes private Qt headers. Qt has no public way to ask
// which QQmlContext the calling JS code belongs to, and the owner of a call
// cannot be told without it. The price is a dependency on the Qt version:
// QT_VERSION in .github/workflows/ci.yml pins it, and the test
// roost_call_owner_test breaks if a Qt update changes this behaviour.
#include <private/qqmlcontextdata_p.h>
#include <private/qv4engine_p.h>

namespace roost {

QQmlContext* callingContext(QJSEngine& engine) {
    QV4::ExecutionEngine* v4 = engine.handle();
    if (v4 == nullptr) {
        return nullptr;
    }
    const auto data = v4->callingQmlContext();
    return data ? data->asQQmlContext() : nullptr;
}

CallOwners::CallOwners(QObject* parent) : QObject(parent) {}

QObject* CallOwners::add(QQmlContext* context) {
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
    return owner;
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
