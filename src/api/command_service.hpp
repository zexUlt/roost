#pragma once

#include <QFuture>
#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPointer>
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
    // Whether the registrant has the keyboard focus now (a panel with focus in
    // it).
    using FocusProbe = std::function<bool()>;

    explicit CommandService(IoThread& io, QObject* parent = nullptr);

    // Gives `command` a handler in the window; False for an empty id or no
    // handler. A command may have several: they are kept in the order they
    // came, Run uses the most recent one that is still alive, and the end of
    // an owner takes away only its own. So a panel that is created again
    // before the old one is destroyed does not lose the command, and the
    // second of two panels keeps it when the first is closed. Run goes
    // from the most recent registration to the first that gave no `focused`
    // or whose `focused` says the registrant has the focus; when none, it
    // takes the registrant that NoteFocus named last (the panel that had the
    // focus before it went out of the panels), and when there is none, the
    // most recent one. With
    // an `owner`, the handler goes when the owner is destroyed; without one,
    // with the service. The id is declared by a plugin manifest
    // (`contributes.commands`): that is what gives it a key and a place in the
    // palette, this only runs it.
    [[nodiscard]] bool Register(const QString& command, const QObject* owner,
                                LocalHandler handler, FocusProbe focused = {});

    // Says that `owner` has the focus now, or had it last: the registrations
    // it has are what Run takes for their commands when no registrant has the
    // focus. Weak: an owner that is gone is forgotten. No-op for an owner
    // with no registration.
    void NoteFocus(const QObject* owner);

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
        FocusProbe Focused;
    };

    IoThread& Io_;
    // By command: the registrations in the order they came, the last first to
    // run.
    QHash<QString, QList<Local>> Locals_;
    // By command: the owner that had the focus last among its registrants.
    QHash<QString, QPointer<const QObject>> LastFocused_;
};

}  // namespace roost::api
