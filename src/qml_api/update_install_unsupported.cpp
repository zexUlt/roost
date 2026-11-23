// The platforms that have no installer: the update line only links to the
// release (design/17-updates.md).

#include "qml_api/update_install.hpp"

namespace roost {

QString installTarget() { return {}; }

std::expected<InstallDone, QString> installPackage(const InstallRequest&) {
    return std::unexpected(
        QStringLiteral("this platform cannot install an update"));
}

std::expected<void, QString> relaunch(const QString&) {
    return std::unexpected(QStringLiteral("this platform cannot relaunch"));
}

}  // namespace roost
