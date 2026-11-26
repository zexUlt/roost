#include "roost/about_info.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QtGlobal>

#include "core/version.hpp"
#include "roost/bug_report.hpp"

namespace roost {

AboutInfo::AboutInfo(QObject* parent) : QObject(parent) {}

QString AboutInfo::version() const {
    return QString::fromStdString(RoostVersion());
}

QString AboutInfo::ghosttyVt() const {
    return QString::fromStdString(GhosttyVtVersion());
}

QString AboutInfo::qtVersion() const { return QString::fromLatin1(qVersion()); }

QString AboutInfo::versionLines() const { return VersionLines(); }

void AboutInfo::copyVersion() const {
    QGuiApplication::clipboard()->setText(VersionLines());
}

}  // namespace roost
