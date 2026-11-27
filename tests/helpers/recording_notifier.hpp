// A DesktopNotifier for the tests of the window's notifications: it records
// what it is asked to show, in place of the operating system, and Click is the
// click of the user on one of them.

#pragma once

#include <QString>

#include <vector>

#include "roost/desktop_notifier.hpp"

namespace roost::testing {

// What a notification was asked to show.
struct Shown {
    QString Id;
    QString Title;
    QString Body;
    bool operator==(const Shown&) const = default;
};

class RecordingNotifier : public DesktopNotifier {
public:
    void Show(const QString& id, const QString& title,
              const QString& body) override {
        Calls.push_back({id, title, body});
    }

    void Click(const QString& id) { emit Activated(id); }

    std::vector<Shown> Calls;
};

}  // namespace roost::testing
