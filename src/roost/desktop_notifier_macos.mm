#include "roost/desktop_notifier_macos.hpp"

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QtLogging>

#include <deque>
#include <utility>

namespace {

// The key of the window's id in the userInfo of a notification.
NSString* const ID_KEY = @"roostId";

QString ToQString(NSString* string) {
    return string == nil ? QString() : QString::fromUtf8(string.UTF8String);
}

NSString* ToNSString(const QString& string) {
    return [NSString stringWithUTF8String:string.toUtf8().constData()];
}

}  // namespace

// Called by the notification center on a thread of its own: it only hands the
// click to the GUI thread (the callback is never given the notifier itself).
@interface RoostNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@property(nonatomic, copy) void (^onActivated)(NSString* identifier, NSString* userInfoId);
@end

@implementation RoostNotificationDelegate

// roost may be the frontmost application: the window controller has already
// decided that this one is shown, so the system must not hide it.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    (void)center;
    (void)notification;
    completionHandler(UNNotificationPresentationOptionBanner |
                      UNNotificationPresentationOptionList);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
    (void)center;
    if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier] &&
        self.onActivated != nil) {
        id roostId = response.notification.request.content.userInfo[ID_KEY];
        self.onActivated(response.notification.request.identifier,
                         [roostId isKindOfClass:[NSString class]] ? roostId : @"");
    }
    completionHandler();
}

@end

namespace roost {

namespace {

enum class Authorization { Unknown, Requesting, Granted, Denied };

struct Pending {
    QString Id;
    QString Title;
    QString Body;
};

}  // namespace

struct MacDesktopNotifier::Impl {
    MacDesktopNotifier* Owner = nullptr;
    bool Bundled = false;
    bool Warned = false;
    Authorization State = Authorization::Unknown;
    std::deque<Pending> Queue;
    RoostNotificationDelegate* Delegate = nil;

    void Warn(const QString& message) {
        if (!std::exchange(Warned, true)) {
            qWarning("roost: %s", qUtf8Printable(message));
        }
    }

    // Runs `work` on the GUI thread, if the notifier is still there by then;
    // callable from any thread.
    template <typename Work>
    static void OnGuiThread(QPointer<MacDesktopNotifier> owner, Work work) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [owner, work = std::move(work)]() mutable {
                if (owner) {
                    work(*owner);
                }
            },
            Qt::QueuedConnection);
    }

    void Post(const Pending& item) {
        UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
        content.title = ToNSString(item.Title);
        content.body = ToNSString(item.Body);
        content.sound = nil;
        content.userInfo = @{ID_KEY : ToNSString(item.Id)};
        UNNotificationRequest* request = [UNNotificationRequest
            requestWithIdentifier:ToNSString(NotificationIdentifier(
                                      QCoreApplication::applicationPid(), item.Id))
                          content:content
                          trigger:nil];
        const QPointer<MacDesktopNotifier> owner(Owner);
        [[UNUserNotificationCenter currentNotificationCenter]
            addNotificationRequest:request
             withCompletionHandler:^(NSError* error) {
               if (error != nil) {
                   const QString text = ToQString(error.localizedDescription);
                   OnGuiThread(owner, [text](MacDesktopNotifier& self) {
                       self.Impl_->Warn(QStringLiteral("no desktop notifications: %1").arg(text));
                   });
               }
             }];
    }

    void Authorize() {
        State = Authorization::Requesting;
        const QPointer<MacDesktopNotifier> owner(Owner);
        [[UNUserNotificationCenter currentNotificationCenter]
            requestAuthorizationWithOptions:(UNAuthorizationOptionAlert |
                                             UNAuthorizationOptionSound)
                          completionHandler:^(BOOL granted, NSError* error) {
                            const QString text =
                                error != nil ? ToQString(error.localizedDescription) : QString();
                            OnGuiThread(owner, [granted, text](MacDesktopNotifier& self) {
                                self.Impl_->Authorized(granted, text);
                            });
                          }];
    }

    void Authorized(bool granted, const QString& error) {
        if (!granted) {
            State = Authorization::Denied;
            Queue.clear();
            Warn(error.isEmpty() ? QStringLiteral("no desktop notifications: not allowed (System "
                                                  "Settings > Notifications > roost)")
                                 : QStringLiteral("no desktop notifications: %1").arg(error));
            return;
        }
        State = Authorization::Granted;
        for (const Pending& item : std::exchange(Queue, {})) {
            Post(item);
        }
    }
};

MacDesktopNotifier::MacDesktopNotifier() : Impl_(std::make_unique<Impl>()) {
    Impl_->Owner = this;
    // A process with no bundle (build/<preset>/bin/roost) has no identifier,
    // and UNUserNotificationCenter throws for it: it is never asked.
    Impl_->Bundled = [NSBundle mainBundle].bundleIdentifier != nil;
    if (!Impl_->Bundled) {
        return;
    }
    const QPointer<MacDesktopNotifier> owner(this);
    const qint64 pid = QCoreApplication::applicationPid();
    Impl_->Delegate = [[RoostNotificationDelegate alloc] init];
    Impl_->Delegate.onActivated = ^(NSString* identifier, NSString* userInfoId) {
      const auto id = ActivatedNotificationId(ToQString(identifier), ToQString(userInfoId), pid);
      if (id) {
          Impl::OnGuiThread(owner,
                            [id = *id](MacDesktopNotifier& self) { emit self.Activated(id); });
      }
    };
    [UNUserNotificationCenter currentNotificationCenter].delegate = Impl_->Delegate;
}

MacDesktopNotifier::~MacDesktopNotifier() {
    if (Impl_->Bundled) {
        // The center holds its delegate weakly; the callback must not outlive
        // the notifier all the same.
        Impl_->Delegate.onActivated = nil;
    }
}

void MacDesktopNotifier::Show(const QString& id, const QString& title, const QString& body) {
    if (!Impl_->Bundled) {
        Impl_->Warn(QStringLiteral("desktop notifications need roost.app"));
        return;
    }
    switch (Impl_->State) {
        case Authorization::Denied:
            return;
        case Authorization::Granted:
            Impl_->Post({id, title, body});
            return;
        case Authorization::Unknown:
            Impl_->Queue.push_back({id, title, body});
            Impl_->Authorize();
            return;
        case Authorization::Requesting:
            Impl_->Queue.push_back({id, title, body});
            return;
    }
}

}  // namespace roost
