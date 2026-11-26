#pragma once

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QString>

namespace roost {

// The "About roost" window's side in C++ (AboutWindow.qml), a type of the
// module Roost.Shell, as BugReport is for the bug report dialog. The versions
// are this process's own; the lines that Copy puts on the clipboard are the
// top of about.txt (VersionLines()). GUI thread only.
//
// The names of the methods are QML's (camelCase), as in src/qml_api.
// NOLINTBEGIN(readability-identifier-naming)
class AboutInfo : public QObject {
    Q_OBJECT
    QML_ELEMENT
    // "0.6.1": the version of roost alone.
    Q_PROPERTY(QString version READ version CONSTANT)
    // "1.0.1": the libghostty-vt that roost is linked with.
    Q_PROPERTY(QString ghosttyVt READ ghosttyVt CONSTANT)
    // The Qt that the process runs with, not the one it was built against.
    Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT)
    // The lines of about.txt that name the versions.
    Q_PROPERTY(QString versionLines READ versionLines CONSTANT)

public:
    explicit AboutInfo(QObject* parent = nullptr);

    QString version() const;
    QString ghosttyVt() const;
    QString qtVersion() const;
    QString versionLines() const;

    // Puts versionLines on the system clipboard.
    Q_INVOKABLE void copyVersion() const;
};
// NOLINTEND(readability-identifier-naming)

}  // namespace roost
