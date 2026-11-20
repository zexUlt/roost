#include "roost/window_icon.hpp"

#include <QGuiApplication>
#include <QString>

#include <array>

namespace roost {

namespace {

constexpr std::array<int, 7> SIZES{16, 32, 48, 64, 128, 256, 512};

}  // namespace

QIcon WindowIcon() {
    QIcon icon;
    for (const int size : SIZES) {
        icon.addFile(QStringLiteral(":/roost/icons/%1.png").arg(size),
                     QSize(size, size));
    }
    return icon;
}

void InstallWindowIcon() { QGuiApplication::setWindowIcon(WindowIcon()); }

}  // namespace roost
