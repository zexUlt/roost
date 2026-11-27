#include "roost/desktop_notifier.hpp"
#include "roost/desktop_notifier_linux.hpp"

namespace roost {

std::unique_ptr<DesktopNotifier> MakeDesktopNotifier() {
    return std::make_unique<LinuxDesktopNotifier>();
}

}  // namespace roost
