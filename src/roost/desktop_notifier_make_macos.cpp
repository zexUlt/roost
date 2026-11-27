#include "roost/desktop_notifier.hpp"
#include "roost/desktop_notifier_macos.hpp"

namespace roost {

std::unique_ptr<DesktopNotifier> MakeDesktopNotifier() {
    return std::make_unique<MacDesktopNotifier>();
}

}  // namespace roost
