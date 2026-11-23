#include "qml_api/update_download.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QNetworkRequest>
#include <QtLogging>

#include <algorithm>

namespace roost {

namespace {

constexpr int TRANSFER_TIMEOUT_MS = 30000;
// SHA256SUMS lists a few packages and the signature file is four lines; far
// beyond that is not read.
constexpr qint64 MAX_SUMS_BYTES = 1 << 20;
constexpr qint64 MAX_SIGNATURE_BYTES = 16 << 10;
// A package is tens of megabytes; this is what the app takes at most.
constexpr qint64 MAX_PACKAGE_BYTES = qint64{1} << 30;
constexpr int SUMS = 0;
constexpr int SIGNATURE = 1;
constexpr int PACKAGE = 2;
// An attempt that left its directory (a crash) is swept when a later one
// starts, once it is this old: a younger one may be a package of another
// window that is being installed.
constexpr qint64 STALE_SECONDS = 24 * 60 * 60;
const QString DIRECTORY_PATTERN = QStringLiteral("download-XXXXXX");

void sweepStale(const QString& root) {
    const QDateTime limit =
        QDateTime::currentDateTime().addSecs(-STALE_SECONDS);
    const QFileInfoList stale = QDir(root).entryInfoList(
        {QStringLiteral("download-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry : stale) {
        if (entry.lastModified() < limit) {
            QDir(entry.absoluteFilePath()).removeRecursively();
        }
    }
}

}  // namespace

UpdateDownload::UpdateDownload(QNetworkAccessManager& manager, QObject* parent)
    : QObject(parent), Manager_(manager) {}

UpdateDownload::~UpdateDownload() { dropAttempt(); }

QString UpdateDownload::packagePath() const {
    return State_ == EState::Ready ? pathOf(Request_.PackageName) : QString();
}

QString UpdateDownload::pathOf(const QString& name) const {
    return Directory_ ? Directory_->filePath(name) : QString();
}

void UpdateDownload::setState(EState state, const QString& error) {
    if (state == State_ && error == Error_) {
        return;
    }
    State_ = state;
    Error_ = error;
    emit changed();
}

void UpdateDownload::dropAttempt() {
    if (Reply_ != nullptr) {
        QNetworkReply* const reply = Reply_;
        Reply_ = nullptr;
        // The abort finishes the reply; nothing is to be done with it.
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    File_.reset();
    // The directory is deleted with the object.
    Directory_.reset();
    Written_ = 0;
    Progress_ = 0;
}

void UpdateDownload::cancel() {
    dropAttempt();
    setState(EState::None);
}

void UpdateDownload::fail(const QString& reason) {
    dropAttempt();
    qWarning("roost: the download of the update failed: %s",
             qUtf8Printable(reason));
    setState(EState::Failed, reason);
}

void UpdateDownload::start(const Request& request) {
    dropAttempt();
    Request_ = request;
    if (!QDir().mkpath(request.Root)) {
        fail(QStringLiteral("cannot make the directory %1").arg(request.Root));
        return;
    }
    sweepStale(request.Root);
    Directory_ = std::make_unique<QTemporaryDir>(
        QDir(request.Root).filePath(DIRECTORY_PATTERN));
    if (!Directory_->isValid()) {
        const QString reason = Directory_->errorString();
        fail(QStringLiteral("cannot make a directory in %1: %2")
                 .arg(request.Root, reason));
        return;
    }
    Steps_ = {
        {.Name = QStringLiteral("SHA256SUMS"),
         .Url = request.SumsUrl,
         .Limit = MAX_SUMS_BYTES},
        {.Name = QStringLiteral("SHA256SUMS.minisig"),
         .Url = request.SignatureUrl,
         .Limit = MAX_SIGNATURE_BYTES},
        {.Name = request.PackageName,
         .Url = request.PackageUrl,
         .Limit = MAX_PACKAGE_BYTES},
    };
    Progress_ = 0;
    setState(EState::Downloading);
    fetch(SUMS);
}

void UpdateDownload::fetch(int index) {
    Index_ = index;
    const Step& step = Steps_.at(index);
    auto file = std::make_unique<QFile>(pathOf(step.Name));
    if (!file->open(QIODevice::WriteOnly)) {
        fail(QStringLiteral("cannot write %1: %2")
                 .arg(step.Name, file->errorString()));
        return;
    }
    File_ = std::move(file);
    Written_ = 0;
    QNetworkRequest request(step.Url);
    request.setTransferTimeout(TRANSFER_TIMEOUT_MS);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "roost");
    QNetworkReply* const reply = Manager_.get(request);
    Reply_ = reply;
    connect(reply, &QNetworkReply::readyRead, this,
            [this, reply] { onReadyRead(reply); });
    connect(reply, &QNetworkReply::downloadProgress, this,
            &UpdateDownload::onProgress);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply] { onFinished(reply); });
}

bool UpdateDownload::drain(QNetworkReply& reply) {
    // A body that comes with an error status is not the file.
    if (reply.error() != QNetworkReply::NoError ||
        reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() !=
            200) {
        return true;
    }
    const Step& step = Steps_.at(Index_);
    while (reply.bytesAvailable() > 0) {
        const QByteArray chunk = reply.read(1 << 16);
        if (Written_ + chunk.size() > step.Limit) {
            fail(QStringLiteral("%1 is larger than %2 bytes")
                     .arg(step.Name)
                     .arg(step.Limit));
            return false;
        }
        if (File_->write(chunk) != chunk.size()) {
            fail(QStringLiteral("cannot write %1: %2")
                     .arg(step.Name, File_->errorString()));
            return false;
        }
        Written_ += chunk.size();
    }
    return true;
}

void UpdateDownload::onReadyRead(QNetworkReply* reply) {
    if (reply == Reply_) {
        drain(*reply);
    }
}

void UpdateDownload::onProgress(qint64 received, qint64 total) {
    if (Index_ != PACKAGE || total <= 0) {
        return;
    }
    if (total > MAX_PACKAGE_BYTES) {
        fail(QStringLiteral("%1 is larger than %2 bytes")
                 .arg(Request_.PackageName)
                 .arg(MAX_PACKAGE_BYTES));
        return;
    }
    const double progress = std::clamp(
        static_cast<double>(received) / static_cast<double>(total), 0.0, 1.0);
    if (progress != Progress_) {
        Progress_ = progress;
        emit changed();
    }
}

void UpdateDownload::onFinished(QNetworkReply* reply) {
    reply->deleteLater();
    if (reply != Reply_) {
        return;
    }
    const QString name = Steps_.at(Index_).Name;
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        fail(status != 0 ? QStringLiteral("cannot download %1: HTTP %2")
                               .arg(name)
                               .arg(status)
                         : QStringLiteral("cannot download %1: %2")
                               .arg(name, reply->errorString()));
        return;
    }
    if (status != 200) {
        fail(QStringLiteral("cannot download %1: HTTP %2")
                 .arg(name)
                 .arg(status));
        return;
    }
    if (!drain(*reply)) {
        return;
    }
    Reply_ = nullptr;
    if (!File_->flush()) {
        fail(QStringLiteral("cannot write %1: %2")
                 .arg(name, File_->errorString()));
        return;
    }
    File_.reset();
    stepDone();
}

std::optional<QByteArray> UpdateDownload::readSmall(const QString& name) {
    QFile file(pathOf(name));
    if (!file.open(QIODevice::ReadOnly)) {
        fail(
            QStringLiteral("cannot read %1: %2").arg(name, file.errorString()));
        return std::nullopt;
    }
    return file.readAll();
}

void UpdateDownload::stepDone() {
    if (Index_ == SUMS) {
        fetch(SIGNATURE);
        return;
    }
    if (Index_ == SIGNATURE) {
        // Before the package is fetched: the signature of the sums, and the
        // line the package has to match.
        const auto sums = readSmall(Steps_.at(SUMS).Name);
        const auto signature = readSmall(Steps_.at(SIGNATURE).Name);
        if (!sums || !signature) {
            return;
        }
        const auto signedSums =
            VerifySums(Request_.Key, *sums, *signature,
                       QStringLiteral("roost %1 SHA256SUMS").arg(Request_.Tag));
        if (!signedSums) {
            fail(signedSums.error());
            return;
        }
        const auto digest = SumsDigest(*sums, Request_.PackageName);
        if (!digest) {
            fail(digest.error());
            return;
        }
        Digest_ = *digest;
        fetch(PACKAGE);
        return;
    }
    const auto verified =
        VerifyFileDigest(pathOf(Request_.PackageName), Digest_);
    if (!verified) {
        fail(verified.error());
        return;
    }
    Progress_ = 1;
    setState(EState::Ready);
    emit ready(pathOf(Request_.PackageName));
}

}  // namespace roost
