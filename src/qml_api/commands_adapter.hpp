#pragma once

#include <QtQml/qqmlregistration.h>
#include <QJSEngine>
#include <QJSValue>
#include <QObject>
#include <QString>

#include "api/api.hpp"

namespace roost {

// Roost.commands: commands.run of the dispatcher, and nothing else.
class CommandsAdapter : public QObject {
    Q_OBJECT
    QML_ANONYMOUS

public:
    CommandsAdapter(api::Api& api, QJSEngine& engine, QObject* parent);

    // callback(error, result): the answer of the backend of the command's
    // plugin. args is any JSON value, or absent.
    Q_INVOKABLE void run(const QString& command,
                         const QJSValue& args = QJSValue(),
                         const QJSValue& callback = QJSValue());

private:
    api::Api& Api_;
    QJSEngine& Engine_;
};

}  // namespace roost
