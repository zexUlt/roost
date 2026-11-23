#include "qml_api/updates.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QtLogging>

#include <string>

#include "api/api.hpp"
#include "core/update_verify.hpp"
#include "core/version.hpp"

namespace roost {

namespace {

constexpr auto RELEASES_URL =
    "https://api.github.com/repos/zexUlt/roost/releases/latest";
// The release page is offered only from here: the answer is data from the
// network, and the link is opened by the system.
constexpr auto RELEASE_PAGE_PREFIX =
    "https://github.com/zexUlt/roost/releases/";
// For tests only: the URL of the check (design/17-updates.md).
constexpr auto TEST_URL_VARIABLE = "ROOST_TEST_UPDATES_URL";
// For tests only: the public key (the text of a minisign.pub) that checks the
// signature of SHA256SUMS instead of the built-in one. Read only while
// ROOST_TEST_UPDATES_URL is set and not empty.
constexpr auto TEST_KEY_VARIABLE = "ROOST_TEST_UPDATES_PUBLIC_KEY";
// For tests only: where a downloaded package goes instead of the cache
// directory of the user.
constexpr auto CACHE_VARIABLE = "ROOST_CACHE";
// The public key of the releases: the key line of packaging/minisign.pub,
// which CMake reads (src/qml_api/CMakeLists.txt).
constexpr auto RELEASE_KEY = ROOST_UPDATE_PUBLIC_KEY;
constexpr std::chrono::hours CHECK_INTERVAL{24};
constexpr int TRANSFER_TIMEOUT_MS = 30000;
// The answer of the GitHub API is a few kilobytes; a reply that is far
// beyond that is not read.
constexpr qint64 MAX_ANSWER_BYTES = 1 << 20;

// Whether a test has pointed the check at a server of its own.
bool testServer() { return !qEnvironmentVariableIsEmpty(TEST_URL_VARIABLE); }

QString versionText(const ReleaseVersion& version) {
    return QStringLiteral("%1.%2.%3")
        .arg(version.Major)
        .arg(version.Minor)
        .arg(version.Patch);
}

// The version of this build: the start of VersionString(), "0.3.1 (...)".
std::optional<ReleaseVersion> runningVersion() {
    const QString text = QString::fromStdString(VersionString());
    return parseReleaseVersion(text.section(u' ', 0, 0));
}

bool isNewer(const QString& candidate, const QString& than) {
    const auto first = parseReleaseVersion(candidate);
    if (!first) {
        return false;
    }
    const auto second = parseReleaseVersion(than);
    return !second || *first > *second;
}

std::expected<UpdateKey, QString> releaseKey() {
    if (testServer() && !qEnvironmentVariableIsEmpty(TEST_KEY_VARIABLE)) {
        return ParseUpdateKey(qgetenv(TEST_KEY_VARIABLE));
    }
    return ParseUpdateKey(QByteArray(RELEASE_KEY));
}

// Where the packages of this window are downloaded: a directory of its own
// in the cache of the user (~/Library/Caches/roost, ~/.cache/roost).
QString downloadRoot() {
    QString base = qEnvironmentVariable(CACHE_VARIABLE);
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    }
    return base.isEmpty() ? QString() : base + QStringLiteral("/updates");
}

// The name of an asset is a file name: no part of a path or of a URL.
bool isAssetName(const QString& name) {
    return !name.isEmpty() && name != u"." && name != u".." &&
           !name.contains(QRegularExpression(QStringLiteral(R"([/\\?#%])")));
}

}  // namespace

QString packageAssetName(const QString& version) {
    const QString arch = QSysInfo::buildCpuArchitecture();
#if defined(Q_OS_MACOS)
    // tools/package_macos.sh: roost-<version>-macos-$(uname -m).dmg.
    return QStringLiteral("roost-%1-macos-%2.dmg").arg(version, arch);
#elif defined(Q_OS_LINUX)
    // tools/package_linux.sh: the AppImage names carry no version.
    Q_UNUSED(version);
    if (arch == u"x86_64") {
        return QStringLiteral("roost-x86_64.AppImage");
    }
    if (arch == u"arm64") {
        return QStringLiteral("roost-aarch64.AppImage");
    }
    return {};
#else
    Q_UNUSED(version);
    Q_UNUSED(arch);
    return {};
#endif
}

QString assetUrlPrefix(const QString& tag) {
    if (testServer()) {
        QUrl base(qEnvironmentVariable(TEST_URL_VARIABLE));
        base.setPath(QStringLiteral("/releases/download/%1/").arg(tag));
        base.setQuery(QString());
        return base.toString();
    }
    return QStringLiteral(
               "https://github.com/zexUlt/roost/releases/download/%1/")
        .arg(tag);
}

std::optional<ReleaseVersion> parseReleaseVersion(const QString& text,
                                                  bool tagged) {
    static const QRegularExpression PLAIN_VERSION(
        QStringLiteral(R"(^(\d{1,9})\.(\d{1,9})\.(\d{1,9})$)"));
    static const QRegularExpression TAGGED_VERSION(
        QStringLiteral(R"(^v(\d{1,9})\.(\d{1,9})\.(\d{1,9})$)"));
    const QRegularExpressionMatch match =
        (tagged ? TAGGED_VERSION : PLAIN_VERSION).match(text);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    return ReleaseVersion{.Major = match.captured(1).toInt(),
                          .Minor = match.captured(2).toInt(),
                          .Patch = match.captured(3).toInt()};
}

Updates::Updates(QObject* parent)
    : QObject(parent),
      Manager_(this),
      Download_(Manager_, this),
      Timer_(this),
      Interval_(CHECK_INTERVAL) {
    connect(&Download_, &UpdateDownload::changed, this, &Updates::changed);
    connect(&Download_, &UpdateDownload::ready, this, &Updates::packageReady);
    Timer_.setSingleShot(false);
    connect(&Timer_, &QTimer::timeout, this, &Updates::startCheck);
}

QString Updates::available() const {
    if (!Enabled_ || !StateKnown_ || Latest_.isEmpty() ||
        !isNewer(Latest_, Dismissed_)) {
        return {};
    }
    return Latest_;
}

void Updates::follow(const QJsonObject& settings) {
    bool check = true;
    QStringList problems;
    const QJsonValue section = settings.value(QStringLiteral("updates"));
    if (!section.isUndefined()) {
        const QJsonValue value =
            section.toObject().value(QStringLiteral("check"));
        if (!section.isObject()) {
            problems.append(
                QStringLiteral("settings.updates must be an object with "
                               "check; the check is on"));
        } else if (value.isBool()) {
            check = value.toBool();
        } else if (!value.isUndefined()) {
            problems.append(
                QStringLiteral("settings.updates.check must be true or "
                               "false; the check is on"));
        }
    }
    const bool problemsDiffer = problems != Problems_;
    if (problemsDiffer) {
        for (const QString& problem : problems) {
            qWarning("roost: %s", qUtf8Printable(problem));
        }
    }
    Problems_ = problems;
    const bool changedCheck = check != Enabled_;
    Enabled_ = check;
    if (changedCheck) {
        if (check) {
            Timer_.start(Interval_);
            startCheck();
        } else {
            Timer_.stop();
            cancelCheck();
            Download_.cancel();
        }
    }
    if (changedCheck || problemsDiffer) {
        emit changed();
    }
}

void Updates::followState(const QJsonObject& document) {
    const QString dismissed =
        document.value(QStringLiteral("dismissed")).toString();
    if (StateKnown_ && dismissed == Dismissed_) {
        return;
    }
    StateKnown_ = true;
    Dismissed_ = dismissed;
    emit changed();
}

void Updates::dismiss() {
    const QString version = available();
    if (version.isEmpty()) {
        return;
    }
    Dismissed_ = version;
    emit changed();
    if (Api_ == nullptr) {
        return;
    }
    Api_->Store()
        .Patch(QStringLiteral("updates"),
               QJsonObject{{QStringLiteral("dismissed"), version}})
        .then(this, [](const rpc::Result& answer) {
            if (!answer) {
                qWarning("roost: the dismissal of the update was not saved: %s",
                         qUtf8Printable(answer.error().Message));
            }
        });
}

bool Updates::canDownload() const {
    return !available().isEmpty() && !packageAssetName(Latest_).isEmpty();
}

QString Updates::downloadState() const {
    switch (Download_.state()) {
        case UpdateDownload::EState::None:
            return {};
        case UpdateDownload::EState::Downloading:
            return QStringLiteral("downloading");
        case UpdateDownload::EState::Ready:
            return QStringLiteral("ready");
        case UpdateDownload::EState::Failed:
            return QStringLiteral("failed");
    }
    return {};
}

// The three files of a release and where they come from, every URL checked:
// the answer is data from the network, and the files are fetched from what it
// says.
std::optional<UpdateDownload::Request> Updates::downloadRequest() {
    const QString tag = QStringLiteral("v") + Latest_;
    const QString packageName = packageAssetName(Latest_);
    const QString prefix = assetUrlPrefix(tag);
    const auto urlOf = [&](const QString& name) -> std::optional<QUrl> {
        for (const UpdateAsset& asset : Assets_) {
            if (asset.Name != name) {
                continue;
            }
            // The prefix and a file name: no path up, no query, no escape.
            if (!asset.Url.startsWith(prefix) ||
                !isAssetName(asset.Url.mid(prefix.size()))) {
                Download_.fail(QStringLiteral("the release has %1 at an "
                                              "address outside the "
                                              "repository")
                                   .arg(name));
                return std::nullopt;
            }
            return QUrl(asset.Url);
        }
        Download_.fail(QStringLiteral("the release has no file %1").arg(name));
        return std::nullopt;
    };
    const auto key = releaseKey();
    if (!key) {
        Download_.fail(QStringLiteral("the public key of the releases is not "
                                      "valid: %1")
                           .arg(key.error()));
        return std::nullopt;
    }
    const auto package = urlOf(packageName);
    const auto sums = package ? urlOf(QStringLiteral("SHA256SUMS")) : package;
    const auto signature =
        sums ? urlOf(QStringLiteral("SHA256SUMS.minisig")) : sums;
    if (!signature) {
        return std::nullopt;
    }
    return UpdateDownload::Request{.Root = downloadRoot(),
                                   .Tag = tag,
                                   .PackageName = packageName,
                                   .PackageUrl = *package,
                                   .SumsUrl = *sums,
                                   .SignatureUrl = *signature,
                                   .Key = *key};
}

void Updates::download() {
    // A package that is downloading or ready is not fetched again.
    if (!canDownload() ||
        Download_.state() == UpdateDownload::EState::Downloading ||
        Download_.state() == UpdateDownload::EState::Ready) {
        return;
    }
    if (const auto request = downloadRequest()) {
        Download_.start(*request);
    }
}

void Updates::startCheck() {
    if (!Enabled_ || Reply_ != nullptr) {
        return;
    }
    // Set and empty: no request at all (the test binaries set it so, and no
    // test reaches GitHub).
    if (qEnvironmentVariableIsSet(TEST_URL_VARIABLE) &&
        qEnvironmentVariableIsEmpty(TEST_URL_VARIABLE)) {
        return;
    }
    const QString override = qEnvironmentVariable(TEST_URL_VARIABLE);
    QNetworkRequest request(QUrl(
        override.isEmpty() ? QString::fromLatin1(RELEASES_URL) : override));
    request.setTransferTimeout(TRANSFER_TIMEOUT_MS);
    request.setRawHeader("Accept", "application/vnd.github+json");
    // The GitHub API refuses a request without one.
    request.setRawHeader("User-Agent",
                         ("roost/" + runningVersion()
                                         .transform(versionText)
                                         .value_or(QStringLiteral("unknown")))
                             .toUtf8());
    QNetworkReply* const reply = Manager_.get(request);
    Reply_ = reply;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply] { onReply(reply); });
}

void Updates::cancelCheck() {
    if (Reply_ == nullptr) {
        return;
    }
    QNetworkReply* const reply = Reply_;
    Reply_ = nullptr;
    // The abort finishes the reply; nothing is to be done with the answer.
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void Updates::onReply(QNetworkReply* reply) {
    reply->deleteLater();
    if (reply != Reply_) {
        return;
    }
    Reply_ = nullptr;
    readAnswer(*reply);
    emit checked();
}

void Updates::readAnswer(QNetworkReply& reply) {
    const int status =
        reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply.error() != QNetworkReply::NoError) {
        failed(
            status != 0
                ? QStringLiteral("HTTP %1").arg(status)
                : QStringLiteral("network error: %1").arg(reply.errorString()));
        return;
    }
    if (status != 200) {
        failed(QStringLiteral("HTTP %1").arg(status));
        return;
    }
    const QByteArray body = reply.read(MAX_ANSWER_BYTES);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        failed(QStringLiteral("the answer is not a JSON object"));
        return;
    }
    const QJsonObject release = document.object();
    const QJsonValue tag = release.value(QStringLiteral("tag_name"));
    const QJsonValue page = release.value(QStringLiteral("html_url"));
    if (!tag.isString() || !page.isString()) {
        failed(QStringLiteral("the answer has no tag_name and html_url"));
        return;
    }
    if (!page.toString().startsWith(QLatin1StringView(RELEASE_PAGE_PREFIX))) {
        failed(QStringLiteral(
            "the answer has a release page outside the repository"));
        return;
    }
    // A draft or a prerelease is not offered; neither is a tag that is not a
    // version. Not failures: the answer is fine, there is nothing to offer.
    const auto version = parseReleaseVersion(tag.toString(), true);
    const bool offerable = version && !release.value("draft").toBool() &&
                           !release.value("prerelease").toBool();
    QList<UpdateAsset> assets;
    for (const QJsonValue& entry : release.value("assets").toArray()) {
        const QJsonObject asset = entry.toObject();
        const QJsonValue name = asset.value("name");
        const QJsonValue url = asset.value("browser_download_url");
        if (name.isString() && url.isString()) {
            assets.append({.Name = name.toString(), .Url = url.toString()});
        }
    }
    succeeded(offerable ? versionText(*version) : QString(), page.toString(),
              assets);
}

void Updates::failed(const QString& reason) {
    if (reason != LastFailure_) {
        qWarning("roost: the check for a newer release failed: %s",
                 qUtf8Printable(reason));
        LastFailure_ = reason;
    }
}

void Updates::succeeded(const QString& version, const QString& url,
                        const QList<UpdateAsset>& assets) {
    LastFailure_.clear();
    const auto running = runningVersion();
    const bool newer = !version.isEmpty() && running &&
                       isNewer(version, versionText(*running));
    const QString latest = newer ? version : QString();
    const QString page = newer ? url : QString();
    Assets_ = newer ? assets : QList<UpdateAsset>();
    if (latest == Latest_ && page == Url_) {
        return;
    }
    // A download is of the release that was offered.
    Download_.cancel();
    Latest_ = latest;
    Url_ = page;
    emit changed();
}

}  // namespace roost
