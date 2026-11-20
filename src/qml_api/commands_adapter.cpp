#include "qml_api/commands_adapter.hpp"

#include <QJSValueList>
#include <QJsonObject>
#include <QPointer>
#include <QQuickItem>
#include <QtLogging>

#include "qml_api/js_callback.hpp"

namespace roost {

CommandsAdapter::CommandsAdapter(api::Api& api, QJSEngine& engine,
                                 const CallOwners& owners, QObject* parent)
    : QObject(parent), Api_(api), Engine_(engine), Owners_(owners) {}

void CommandsAdapter::run(const QString& command, const QJSValue& args,
                          const QJSValue& callback) {
    QObject* const context =
        Owners_.contextFor(Engine_, this, QStringLiteral("commands.run"));
    if (context == nullptr) {
        return;
    }
    QJsonObject params{{QStringLiteral("command"), command}};
    if (!args.isUndefined()) {
        params.insert(QStringLiteral("args"), toJson(Engine_, args));
    }
    callMethod(Api_, Engine_, context, QStringLiteral("commands.run"), params,
               callback);
}

bool CommandsAdapter::registerCommand(const QString& command,
                                      const QJSValue& handler) {
    if (!handler.isCallable()) {
        Engine_.throwError(QJSValue::TypeError,
                           QStringLiteral("Roost.commands.registerCommand: the "
                                          "handler must be a function"));
        return false;
    }
    QObject* const owner = Owners_.contextFor(
        Engine_, this, QStringLiteral("commands.registerCommand"));
    if (owner == nullptr) {
        return false;
    }
    // The panel that registers has the focus when it, or an item in it, has.
    // The probe is weak on both sides: a panel that is gone has no focus.
    const auto focused = [owners = QPointer<const CallOwners>(&Owners_),
                          owner = QPointer<const QObject>(owner)] {
        if (!owners || !owner) {
            return false;
        }
        const auto* panel =
            qobject_cast<const QQuickItem*>(owners->panelOf(owner));
        return panel != nullptr && panel->hasActiveFocus();
    };
    // The panel is told to the service when it gets the focus, and now if it
    // has it, so that a command run while the focus is outside of every panel
    // goes to the panel that had it last. Once per panel, whatever number of
    // commands it registers.
    if (auto* const panel = qobject_cast<QQuickItem*>(Owners_.panelOf(owner));
        panel != nullptr && !FocusWatched_.contains(owner)) {
        FocusWatched_.insert(owner);
        connect(owner, &QObject::destroyed, this,
                [this, owner] { FocusWatched_.remove(owner); });
        const auto note = [commands =
                               QPointer<api::CommandService>(&Api_.Commands()),
                           owner = QPointer<const QObject>(owner)] {
            if (commands && owner) {
                commands->NoteFocus(owner);
            }
        };
        connect(panel, &QQuickItem::activeFocusChanged, owner,
                [note](bool focused) {
                    if (focused) {
                        note();
                    }
                });
        if (panel->hasActiveFocus()) {
            note();
        }
    }
    const bool added = Api_.Commands().Register(
        command, owner,
        [engine = QPointer<QJSEngine>(&Engine_), handler,
         command](const QJsonValue& args) -> rpc::Result {
            if (!engine) {
                return std::unexpected(rpc::Error::FromCode(
                    rpc::EErrorCode::InternalError,
                    QStringLiteral("the engine of the handler of %1 is gone")
                        .arg(command)));
            }
            const QJSValue returned =
                handler.call({engine->toScriptValue(args)});
            if (returned.isError()) {
                return std::unexpected(rpc::Error::FromCode(
                    rpc::EErrorCode::InternalError,
                    QStringLiteral("the handler of %1 threw: %2")
                        .arg(command, returned.toString())));
            }
            const QJsonValue value = toJson(*engine, returned);
            return value.isUndefined() ? QJsonValue(QJsonValue::Null) : value;
        },
        focused);
    if (!added) {
        qWarning("roost: no handler set for the command %s: the id is empty",
                 qUtf8Printable(command));
    }
    return added;
}

}  // namespace roost
