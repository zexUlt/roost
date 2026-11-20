#include "roost/bug_report_controller.hpp"

#include <QDesktopServices>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QSGRendererInterface>
#include <QUrl>
#include <QVariantMap>

#include "core/protocol.hpp"
#include "core/store.hpp"
#include "roost/bug_report.hpp"
#include "roost/daemon_launcher.hpp"

namespace roost {

namespace {

QString GraphicsApiOf(const QQuickWindow* window) {
    if (window == nullptr) {
        return {};
    }
    const QSGRendererInterface* renderer = window->rendererInterface();
    if (renderer == nullptr) {
        return {};
    }
    switch (renderer->graphicsApi()) {
        case QSGRendererInterface::Software:
            return QStringLiteral("Software");
        case QSGRendererInterface::OpenVG:
            return QStringLiteral("OpenVG");
        case QSGRendererInterface::OpenGL:
            return QStringLiteral("OpenGL");
        case QSGRendererInterface::Direct3D11:
            return QStringLiteral("Direct3D 11");
        case QSGRendererInterface::Vulkan:
            return QStringLiteral("Vulkan");
        case QSGRendererInterface::Metal:
            return QStringLiteral("Metal");
        case QSGRendererInterface::Direct3D12:
            return QStringLiteral("Direct3D 12");
        case QSGRendererInterface::Null:
            return QStringLiteral("Null");
        case QSGRendererInterface::Unknown:
            break;
    }
    return {};
}

QString ConfigDirectoryOrEmpty() {
    const auto directory = ConfigDirectory();
    return directory ? *directory : QString();
}

QString LogDirectory() {
    return DaemonLauncher::LogDirectory(protocol::SocketPath());
}

}  // namespace

BugReport::BugReport(QObject* parent) : QObject(parent) {}

QVariantList BugReport::files(bool includeSettings) const {
    QList<BugReportFile> list = LogFiles(LogDirectory());
    if (includeSettings) {
        list += SettingsFiles(ConfigDirectoryOrEmpty());
    }
    QVariantList result;
    const QLocale locale;
    for (const BugReportFile& file : list) {
        result.push_back(
            QVariantMap{{QStringLiteral("name"), file.Name},
                        {QStringLiteral("size"), file.Size},
                        {QStringLiteral("text"),
                         QStringLiteral("%1 (%2)").arg(
                             file.Name, locale.formattedDataSize(file.Size))}});
    }
    return result;
}

bool BugReport::create(const QString& description, bool includeSettings) {
    BugReportRequest request;
    request.Description = description;
    request.About = AboutText(protocol::SocketPath(), GraphicsApiOf(Window_));
    request.LogDirectory = LogDirectory();
    request.ConfigDirectory = ConfigDirectoryOrEmpty();
    request.IncludeSettings = includeSettings;
    request.OutputDirectory = OutputDirectory_;
    const auto path = CreateBugReport(request);
    const QString newPath = path ? *path : QString();
    const QString newError = path ? QString() : path.error();
    if (newPath != ZipPath_) {
        ZipPath_ = newPath;
        emit zipPathChanged();
    }
    if (newError != Error_) {
        Error_ = newError;
        emit errorChanged();
    }
    return path.has_value();
}

void BugReport::clearError() {
    if (!Error_.isEmpty()) {
        Error_.clear();
        emit errorChanged();
    }
}

void BugReport::showInFolder() const {
    if (ZipPath_.isEmpty()) {
        return;
    }
#ifdef Q_OS_MACOS
    QProcess::startDetached(QStringLiteral("open"),
                            {QStringLiteral("-R"), ZipPath_});
#else
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QFileInfo(ZipPath_).absolutePath()));
#endif
}

void BugReport::openIssue(const QString& description) const {
    QDesktopServices::openUrl(QUrl(issueUrl(description)));
}

QString BugReport::issueUrl(const QString& description) const {
    return IssueUrl(description, ZipPath_.isEmpty()
                                     ? QString()
                                     : QFileInfo(ZipPath_).fileName());
}

}  // namespace roost
