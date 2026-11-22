#include "qml_api/updates.hpp"

#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QtLogging>

#include <string>

#include "api/api.hpp"
#include "core/version.hpp"

namespace roost {

namespace {

constexpr auto RELEASES_URL =
    "https://api.github.com/repos/zexUlt/roost/releases/latest";
// For tests only: the URL of the check (design/17-updates.md).
constexpr auto TEST_URL_VARIABLE = "ROOST_TEST_UPDATES_URL";
constexpr std::chrono::hours CHECK_INTERVAL{24};
constexpr int TRANSFER_TIMEOUT_MS = 30000;
// The answer of the GitHub API is a few kilobytes; a reply that is far
// beyond that is not read.
constexpr qint64 MAX_ANSWER_BYTES = 1 << 20;

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

}  // namespace

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
    : QObject(parent), Manager_(this), Timer_(this), Interval_(CHECK_INTERVAL) {
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
    // A draft or a prerelease is not offered; neither is a tag that is not a
    // version. Not failures: the answer is fine, there is nothing to offer.
    const auto version = parseReleaseVersion(tag.toString(), true);
    const bool offerable = version && !release.value("draft").toBool() &&
                           !release.value("prerelease").toBool();
    succeeded(offerable ? versionText(*version) : QString(), page.toString());
}

void Updates::failed(const QString& reason) {
    if (reason != LastFailure_) {
        qWarning("roost: the check for a newer release failed: %s",
                 qUtf8Printable(reason));
        LastFailure_ = reason;
    }
}

void Updates::succeeded(const QString& version, const QString& url) {
    LastFailure_.clear();
    const auto running = runningVersion();
    const bool newer = !version.isEmpty() && running &&
                       isNewer(version, versionText(*running));
    const QString latest = newer ? version : QString();
    const QString page = newer ? url : QString();
    if (latest == Latest_ && page == Url_) {
        return;
    }
    Latest_ = latest;
    Url_ = page;
    emit changed();
}

}  // namespace roost
