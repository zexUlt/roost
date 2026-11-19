#include "roost/login_environment.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QUuid>
#include <QtLogging>

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

namespace roost {

namespace {

// More than any environment: a shell that prints without end is killed.
constexpr qsizetype MAX_OUTPUT = 8 * 1024 * 1024;

}  // namespace

const QStringList& LoginEnvironment::BundleVariables() {
    static const QStringList VARIABLES{
        QStringLiteral("APPDIR"), QStringLiteral("APPIMAGE"),
        QStringLiteral("ARGV0"), QStringLiteral("OWD")};
    return VARIABLES;
}

const QStringList& LoginEnvironment::AppImageLibraryVariables() {
    static const QStringList VARIABLES{QStringLiteral("LD_LIBRARY_PATH"),
                                       QStringLiteral("QT_PLUGIN_PATH"),
                                       QStringLiteral("QML2_IMPORT_PATH")};
    return VARIABLES;
}

bool LoginEnvironment::InAppImage(const QString& appDir,
                                  const QString& executableDir) {
    if (appDir.isEmpty()) {
        return false;
    }
    const QString root = QDir::cleanPath(appDir);
    const QString executable = QDir::cleanPath(executableDir);
    return executable == root || executable.startsWith(root + QLatin1Char('/'));
}

QProcessEnvironment LoginEnvironment::Strip(QProcessEnvironment environment,
                                            bool inAppImage) {
    for (const QString& name : BundleVariables()) {
        environment.remove(name);
    }
    if (inAppImage) {
        for (const QString& name : AppImageLibraryVariables()) {
            environment.remove(name);
        }
    }
    return environment;
}

std::optional<QProcessEnvironment> LoginEnvironment::Parse(
    const QByteArray& output, const QByteArray& begin, const QByteArray& end) {
    const qsizetype from = output.indexOf(begin);
    if (from < 0) {
        return std::nullopt;
    }
    const qsizetype start = from + begin.size();
    const qsizetype to = output.indexOf(end, start);
    if (to < 0) {
        return std::nullopt;
    }
    QProcessEnvironment environment;
    const QByteArray body = output.mid(start, to - start);
    for (const QByteArray& entry : body.split('\0')) {
        const qsizetype equals = entry.indexOf('=');
        if (equals <= 0) {
            continue;
        }
        environment.insert(QString::fromLocal8Bit(entry.left(equals)),
                           QString::fromLocal8Bit(entry.mid(equals + 1)));
    }
    return environment;
}

LoginEnvironment::Settings LoginEnvironment::Settings::OfThisProcess() {
    Settings settings;
    settings.Base = QProcessEnvironment::systemEnvironment();
    settings.HasTerminal =
        !settings.Base.value(QStringLiteral("TERM")).isEmpty() &&
        ::isatty(0) != 0;
    settings.Shell = settings.Base.value(QStringLiteral("SHELL"));
    settings.InAppImage = LoginEnvironment::InAppImage(
        settings.Base.value(QStringLiteral("APPDIR")),
        QCoreApplication::applicationDirPath());
    return settings;
}

LoginEnvironment::LoginEnvironment(Settings settings, QObject* parent)
    : QObject(parent), Settings_(std::move(settings)) {
    const QByteArray id = QUuid::createUuid().toByteArray(QUuid::Id128);
    Begin_ = "ROOST_ENV_BEGIN_" + id;
    End_ = "ROOST_ENV_END_" + id;
    Timeout_.setSingleShot(true);
    Timeout_.setInterval(Settings_.Timeout);
    connect(&Timeout_, &QTimer::timeout, this, &LoginEnvironment::OnTimeout);
}

LoginEnvironment::~LoginEnvironment() {
    if (Shell_ != nullptr) {
        KillShell();
    }
}

void LoginEnvironment::Start() {
    if (std::exchange(Started_, true)) {
        return;
    }
    if (Settings_.HasTerminal) {
        Finish(Settings_.Base, false);
        return;
    }
    if (Settings_.Shell.isEmpty()) {
        Fail(QStringLiteral("SHELL is not set"));
        return;
    }
    // printf with no newline, so that the entries are the only thing between
    // the markers; single quotes are the same in every shell.
    const QString script =
        QStringLiteral("printf %s '%1'; env -0; printf %s '%2'")
            .arg(QString::fromLatin1(Begin_), QString::fromLatin1(End_));
    Shell_ = std::make_unique<QProcess>();
    // The shell gets the window's environment without the bundle's variables:
    // the libraries of an AppImage are not the ones of the shell.
    Shell_->setProcessEnvironment(Strip(Settings_.Base, Settings_.InAppImage));
    Shell_->setProgram(Settings_.Shell);
    Shell_->setArguments({QStringLiteral("-l"), QStringLiteral("-i"),
                          QStringLiteral("-c"), script});
    Shell_->setWorkingDirectory(QDir::homePath());
    Shell_->setStandardInputFile(QProcess::nullDevice());
    Shell_->setProcessChannelMode(QProcess::SeparateChannels);
    // What the rc files say on stderr is of no use.
    Shell_->setStandardErrorFile(QProcess::nullDevice());
    // Its own session: no controlling terminal for an interactive shell to
    // take, and one process group to kill.
    QProcess::UnixProcessParameters parameters;
    parameters.flags = QProcess::UnixProcessFlag::CreateNewSession |
                       QProcess::UnixProcessFlag::CloseFileDescriptors |
                       QProcess::UnixProcessFlag::ResetSignalHandlers;
    Shell_->setUnixProcessParameters(parameters);
    connect(Shell_.get(), &QProcess::readyReadStandardOutput, this,
            &LoginEnvironment::OnOutput);
    connect(Shell_.get(), &QProcess::finished, this,
            &LoginEnvironment::OnProcessFinished);
    connect(Shell_.get(), &QProcess::errorOccurred, this,
            &LoginEnvironment::OnError);
    Timeout_.start();
    Shell_->start();
}

void LoginEnvironment::OnOutput() {
    if (Shell_ == nullptr) {
        return;
    }
    // The end marker may be split between two reads.
    const qsizetype scanFrom =
        std::max<qsizetype>(0, Output_.size() - End_.size() + 1);
    Output_ += Shell_->readAllStandardOutput();
    if (Output_.indexOf(End_, scanFrom) >= 0) {
        // All there is to read: a shell whose rc hangs on exit is not waited
        // for. A marker in the echo of the script parses to nothing; then the
        // read goes on.
        if (const auto parsed = Parse(Output_, Begin_, End_)) {
            KillShell();
            Succeed(*parsed);
            return;
        }
    }
    if (Output_.size() > MAX_OUTPUT) {
        Fail(QStringLiteral("the shell printed too much"));
    }
}

void LoginEnvironment::OnProcessFinished() {
    if (Shell_ == nullptr) {
        return;
    }
    Output_ += Shell_->readAllStandardOutput();
    if (const auto parsed = Parse(Output_, Begin_, End_)) {
        Succeed(*parsed);
        return;
    }
    Fail(QStringLiteral("the output of %1 has no environment between the "
                        "markers (exit code %2)")
             .arg(Settings_.Shell)
             .arg(Shell_->exitCode()));
}

void LoginEnvironment::OnError(QProcess::ProcessError error) {
    // The others end in finished(), or in the timeout.
    if (error == QProcess::FailedToStart && Shell_ != nullptr) {
        Fail(QStringLiteral("cannot start %1: %2")
                 .arg(Settings_.Shell, Shell_->errorString()));
    }
}

void LoginEnvironment::OnTimeout() {
    Fail(QStringLiteral("%1 did not finish in %2 ms")
             .arg(Settings_.Shell)
             .arg(Settings_.Timeout.count()));
}

void LoginEnvironment::KillShell() {
    Shell_->disconnect(this);
    const qint64 pid = Shell_->processId();
    if (pid > 1) {
        // Its session and process group: what the shell started goes too.
        ::kill(-static_cast<pid_t>(pid), SIGKILL);
    }
    if (Shell_->state() != QProcess::NotRunning) {
        Shell_->kill();
        Shell_->waitForFinished(1000);
    }
}

void LoginEnvironment::Fail(const QString& reason) {
    if (Ready_) {
        return;
    }
    qWarning(
        "roost: cannot read the login environment: %s; roostd gets the "
        "environment of the window",
        qPrintable(reason));
    if (Shell_ != nullptr) {
        KillShell();
        // Not deleted inside its own signal.
        Shell_.release()->deleteLater();
    }
    Finish(Settings_.Base, false);
}

void LoginEnvironment::Succeed(const QProcessEnvironment& environment) {
    Shell_.release()->deleteLater();
    Finish(environment, true);
}

void LoginEnvironment::Finish(QProcessEnvironment environment, bool fromShell) {
    Timeout_.stop();
    Environment_ = Strip(std::move(environment), Settings_.InAppImage);
    FromShell_ = fromShell;
    Ready_ = true;
    emit Finished();
}

}  // namespace roost
