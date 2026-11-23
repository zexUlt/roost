#pragma once

#include <QtQml/qqmlregistration.h>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <optional>

namespace roost::api {
class Api;
}  // namespace roost::api

namespace roost {

// A release version, numeric X.Y.Z (design/17-updates.md).
struct ReleaseVersion {
    int Major = 0;
    int Minor = 0;
    int Patch = 0;

    friend auto operator<=>(const ReleaseVersion&,
                            const ReleaseVersion&) = default;
};

// "X.Y.Z" or, with `tagged`, "vX.Y.Z"; anything else (a suffix, a missing
// part, a sign, a number too big for an int) is not a version.
std::optional<ReleaseVersion> parseReleaseVersion(const QString& text,
                                                  bool tagged = false);

// Roost.updates: whether a newer release than this one is out (design/17).
// The window asks the GitHub API once it has read the settings, then every 24
// hours; it installs nothing.
//
// `available` is the version ("0.4.0", no "v") of the newest release that is
// newer than this one and than the one the user dismissed; empty when there is
// none, while the state of dismissals is not read, and while the check is off.
// `url` is the page of that release. Both are read-only to QML; `dismiss()`
// hides the offer until a newer version comes.
//
// follow() takes the document `settings` (key updates.check, default true:
// false cancels a request in flight and makes no more), followState() the
// document `updates` ({dismissed}); the singleton calls both as it does for
// the theme (RoostSingleton::setApi). Both are C++ only.
class Updates : public QObject {
    Q_OBJECT
    QML_ANONYMOUS

    Q_PROPERTY(QString available READ available NOTIFY changed)
    Q_PROPERTY(QString url READ url NOTIFY changed)
    // Whether the check is on: settings.updates.check.
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    // The settings of this feature that are not valid, one text each; the
    // window shows them in the strip of problems like the font ones.
    Q_PROPERTY(QStringList problems READ problems NOTIFY changed)

public:
    explicit Updates(QObject* parent = nullptr);

    // The window's API, for the write of a dismissal. Without one dismiss()
    // hides the offer for this run only. The Api must outlive this object.
    void attach(api::Api& api) { Api_ = &api; }

    // The time between two checks; 24 hours unless a test says otherwise.
    // Takes effect at the next follow() that turns the check on.
    void setInterval(std::chrono::milliseconds interval) {
        Interval_ = interval;
    }

    void follow(const QJsonObject& settings);
    void followState(const QJsonObject& document);

    Q_INVOKABLE void dismiss();

    QString available() const;
    QString url() const { return available().isEmpty() ? QString() : Url_; }
    bool enabled() const { return Enabled_; }
    const QStringList& problems() const { return Problems_; }

signals:
    void changed();
    // A request has ended with an answer, good or not (not when it was
    // cancelled). For tests.
    void checked();

private:
    void startCheck();
    void cancelCheck();
    void onReply(QNetworkReply* reply);
    void readAnswer(QNetworkReply& reply);
    // Writes a warning for a failure that the last one was not.
    void failed(const QString& reason);
    void succeeded(const QString& version, const QString& url);

    QNetworkAccessManager Manager_;
    QTimer Timer_;
    QPointer<QNetworkReply> Reply_;
    api::Api* Api_ = nullptr;
    std::chrono::milliseconds Interval_;
    bool Enabled_ = false;
    // Whether the document `updates` is read: an offer waits for it, or a
    // dismissed version would flash at the start.
    bool StateKnown_ = false;
    QStringList Problems_;
    // The newest release the last answer offered, if it was newer than this
    // one; "X.Y.Z" and its page.
    QString Latest_;
    QString Url_;
    QString Dismissed_;
    QString LastFailure_;
};

}  // namespace roost
