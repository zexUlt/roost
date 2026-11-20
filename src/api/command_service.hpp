#pragma once

#include <QFuture>
#include <QHash>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <functional>

#include "core/rpc.hpp"
#include "terminal/io_thread.hpp"

namespace roost::api {

// Runs commands (commands.run, design.md §4). A command that the window has a
// handler for (Register) runs in the window and never reaches roostd; any
// other is a request to roostd, which gives it to the backend of the plugin
// that owns it. No cache. Use it from the GUI thread; a future of a command of
// roostd finishes in the IO thread, so attach a continuation with a context.
class CommandService : public QObject {
    Q_OBJECT

public:
    // The handler of a command of the window: runs in the GUI thread, inside
    // Run, and answers at once. `args` is as given to Run.
    using LocalHandler = std::function<rpc::Result(const QJsonValue& args)>;

    explicit CommandService(IoThread& io, QObject* parent = nullptr);

    // Gives `command` a handler in the window. False if the command already
    // has one (a handler is not replaced). With an `owner`, the handler goes
    // when the owner is destroyed; without one, with the service. The id is
    // declared by a plugin manifest (`contributes.commands`): that is what
    // gives it a key and a place in the palette, this only runs it.
    [[nodiscard]] bool Register(const QString& command, const QObject* owner,
                                LocalHandler handler);

    // Whether `command` has a handler in the window.
    [[nodiscard]] bool HasLocal(const QString& command) const;

    // The answer of the handler of the window, if the command has one, else
    // the one of the backend of the plugin that owns the command; roostd's
    // errors pass unchanged. Absent args are left out of the request.
    QFuture<rpc::Result> Run(const QString& command,
                             const QJsonValue& args = QJsonValue::Undefined);

private:
    struct Local {
        LocalHandler Handler;
        const QObject* Owner = nullptr;
    };

    IoThread& Io_;
    QHash<QString, Local> Locals_;
};

}  // namespace roost::api
