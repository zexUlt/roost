#pragma once

#include <QtQml/qqmlregistration.h>
#include <QJSEngine>
#include <QJSValue>
#include <QObject>
#include <QString>

#include "api/api.hpp"
#include "qml_api/calling_context.hpp"

namespace roost {

// Roost.commands: commands.run of the dispatcher, and the handlers of commands
// that the window runs itself (registerCommand).
class CommandsAdapter : public QObject {
    Q_OBJECT
    QML_ANONYMOUS

public:
    CommandsAdapter(api::Api& api, QJSEngine& engine, const CallOwners& owners,
                    QObject* parent);

    // callback(error, result): the answer of the backend of the command's
    // plugin. args is any JSON value, or absent.
    Q_INVOKABLE void run(const QString& command,
                         const QJSValue& args = QJSValue(),
                         const QJSValue& callback = QJSValue());

    // Gives the command a handler written in QML: `handler(args)` runs in the
    // window when the command is run (by a key, the palette or `run`), without
    // a request to roostd; its return value is the answer (undefined is null),
    // a throw is an error. The handler lives as long as the caller does, as a
    // call of `run` does (the calling panel's owner, 05-plugins.md): it is
    // removed when the panel is torn down; in the shell it lives as long as
    // the window. A command may have handlers of several panels: the one
    // whose panel has the keyboard focus runs; if none of them has it, or a
    // handler was set outside a panel, the most recent live one runs. Returns
    // false, and the handler is not set, if the id is empty or the caller is
    // being torn down. A handler that is not a function throws TypeError. The
    // name is not `register`: that is a C++ keyword, and moc names the method
    // as it is written.
    Q_INVOKABLE bool registerCommand(const QString& command,
                                     const QJSValue& handler);

private:
    api::Api& Api_;
    QJSEngine& Engine_;
    const CallOwners& Owners_;
};

}  // namespace roost
