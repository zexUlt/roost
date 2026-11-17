#include "qml_api/commands_adapter.hpp"

#include <QJsonObject>

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

}  // namespace roost
