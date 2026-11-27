#include "roost/desktop_notifier.hpp"

namespace roost {

namespace {

// For a platform that has no implementation yet: shows nothing.
class NoDesktopNotifier : public DesktopNotifier {
public:
    void Show(const QString& /*id*/, const QString& /*title*/,
              const QString& /*body*/) override {}
};

}  // namespace

std::unique_ptr<DesktopNotifier> MakeDesktopNotifier() {
    return std::make_unique<NoDesktopNotifier>();
}

}  // namespace roost
