#pragma once

#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>

#include <memory>
#include <optional>

#include "core/update_verify.hpp"

namespace roost {

// The download of one release package and the check of it
// (design/17-updates.md), owned by Updates: SHA256SUMS and its minisign
// signature first, the package after them, each streamed to a file of a
// directory made for this attempt. The signature and the package's line are
// checked before the package is fetched, the package's digest after. A failure
// or a cancel deletes the directory; nothing is kept.
class UpdateDownload : public QObject {
    Q_OBJECT

public:
    enum class EState { None, Downloading, Ready, Failed };

    // What to fetch and check against: `PackageName` is the line of
    // SHA256SUMS to find and the name of the file; the three URLs have been
    // checked by the caller. The directory of the attempt is made under `Root`.
    struct Request {
        QString Root;
        QString Tag;
        QString PackageName;
        QUrl PackageUrl;
        QUrl SumsUrl;
        QUrl SignatureUrl;
        UpdateKey Key;
    };

    explicit UpdateDownload(QNetworkAccessManager& manager,
                            QObject* parent = nullptr);
    ~UpdateDownload() override;

    // Starts an attempt; one that is running is cancelled first.
    void start(const Request& request);
    // Ends the attempt and its directory, whatever its state: None again.
    void cancel();
    // Ends the attempt, deletes its directory, and is Failed with the reason;
    // also for an attempt that could not start.
    void fail(const QString& reason);

    EState state() const { return State_; }
    // The part of the package received, 0 to 1; 0 while it is unknown.
    double progress() const { return Progress_; }
    const QString& error() const { return Error_; }
    // The verified package, when Ready; empty otherwise.
    QString packagePath() const;

signals:
    void changed();
    void ready(const QString& path);

private:
    struct Step {
        QString Name;
        QUrl Url;
        qint64 Limit = 0;
    };

    void fetch(int index);
    void onReadyRead(QNetworkReply* reply);
    void onFinished(QNetworkReply* reply);
    void onProgress(qint64 received, qint64 total);
    // Writes what the reply has; false (the attempt failed) if it cannot.
    bool drain(QNetworkReply& reply);
    void stepDone();
    void dropAttempt();
    void setState(EState state, const QString& error = {});
    QString pathOf(const QString& name) const;
    std::optional<QByteArray> readSmall(const QString& name);

    QNetworkAccessManager& Manager_;
    EState State_ = EState::None;
    double Progress_ = 0;
    QString Error_;
    Request Request_;
    QList<Step> Steps_;
    int Index_ = 0;
    std::unique_ptr<QTemporaryDir> Directory_;
    std::unique_ptr<QFile> File_;
    qint64 Written_ = 0;
    QPointer<QNetworkReply> Reply_;
    QByteArray Digest_;
};

}  // namespace roost
