#pragma once

#include <QtQml/qqmlregistration.h>
#include <QObject>

#include "sessions/qml_types/sessions_list.hpp"

namespace roost::sessions {

// Registers SessionsList in module RoostSessions without making the library
// of the model depend on QtQml.
struct SessionsListForeign {
    Q_GADGET
    QML_FOREIGN(SessionsList)
    QML_NAMED_ELEMENT(SessionsList)
};

}  // namespace roost::sessions
