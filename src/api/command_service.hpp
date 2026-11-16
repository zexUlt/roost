#pragma once

#include <QFuture>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include "core/rpc.hpp"
#include "terminal/io_thread.hpp"

namespace roost::api {

// Runs plugin commands (commands.run, design.md §4). No cache: every call is
// a request to roostd. Use it from the GUI thread; the future finishes in the
// IO thread, so attach a continuation with a context.
class CommandService : public QObject {
    Q_OBJECT

public:
    explicit CommandService(IoThread& io, QObject* parent = nullptr);

    // The answer is the one of the backend of the plugin that owns the
    // command; roostd's errors pass unchanged. Absent args are left out.
    QFuture<rpc::Result> Run(const QString& command,
                             const QJsonValue& args = QJsonValue::Undefined);

private:
    IoThread& Io_;
};

}  // namespace roost::api
