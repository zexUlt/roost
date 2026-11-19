#pragma once

#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QTimer>

#include <chrono>
#include <memory>
#include <optional>

namespace roost {

// The environment of the roostd that the window starts (design/04-protocol.md,
// "Запуск демона окном"), read once and cached for the life of the window.
//
// A window started from Finder or a desktop launcher has no terminal and the
// bare PATH of the session: no `claude`, and the sessions and attaches of
// roostd would inherit that. Then the login shell is asked for its environment
// (`$SHELL -l -i -c`, printing `env -0` between two markers, so that what the
// rc files print is ignored), asynchronously: the GUI thread never waits. With
// a terminal the window's own environment is the one to pass on.
//
// The read ends as soon as the end marker is in the output: the shell and what
// it started (its process group) are killed then, not waited for.
//
// Either way the variables of a bundle are stripped (see Strip). If the shell
// cannot be read (no SHELL, does not start, the timeout, no markers) the shell
// is killed, a warning is logged and the window's own environment is used.
//
// GUI thread only.
class LoginEnvironment : public QObject {
    Q_OBJECT

public:
    static constexpr std::chrono::milliseconds DEFAULT_TIMEOUT{
        std::chrono::seconds(10)};

    struct Settings {
        // The environment of the window.
        QProcessEnvironment Base;
        // Whether the window has a terminal: then Base is used as it is.
        bool HasTerminal = true;
        // The login shell ($SHELL).
        QString Shell;
        std::chrono::milliseconds Timeout = DEFAULT_TIMEOUT;
        // Whether the window runs inside an AppImage (InAppImage).
        bool InAppImage = false;

        // This process: its environment, TERM and a tty on stdin, the AppImage
        // of DaemonLauncher::CommandFor.
        static Settings OfThisProcess();
    };

    // Variables AppRun of an AppImage sets, and the user's own copies would
    // not be: dropped from the environment only inside an AppImage.
    static const QStringList& AppImageLibraryVariables();
    // Variables of the bundle itself: always dropped.
    static const QStringList& BundleVariables();

    // The window runs inside an AppImage or an extracted AppDir run through
    // AppRun (which sets APPDIR but no APPIMAGE): `appDir` set and the
    // executable directory inside it. (An APPDIR of another app's environment
    // does not count.)
    static bool InAppImage(const QString& appDir, const QString& executableDir);

    // `environment` without the variables of the bundle: BundleVariables
    // always, AppImageLibraryVariables when `inAppImage`.
    static QProcessEnvironment Strip(QProcessEnvironment environment,
                                     bool inAppImage);

    // The variables between the two markers of the output of the shell: the
    // NUL-separated KEY=VALUE entries; nullopt without both markers.
    static std::optional<QProcessEnvironment> Parse(const QByteArray& output,
                                                    const QByteArray& begin,
                                                    const QByteArray& end);

    explicit LoginEnvironment(Settings settings, QObject* parent = nullptr);
    ~LoginEnvironment() override;

    // Starts the read, once; later calls change nothing. Ready() is true
    // afterwards if there was nothing to wait for (a terminal, no shell).
    void Start();

    // Whether Environment() is final.
    bool Ready() const { return Ready_; }
    // The environment for roostd, stripped; meaningful once Ready().
    const QProcessEnvironment& Environment() const { return Environment_; }
    // Whether it is the one of the login shell, not the fallback.
    bool FromLoginShell() const { return FromShell_; }

signals:
    // Once, when Ready() becomes true.
    void Finished();

private:
    void OnOutput();
    void OnProcessFinished();
    void OnError(QProcess::ProcessError error);
    void OnTimeout();
    // Kills the shell and what it started (its process group).
    void KillShell();
    void Fail(const QString& reason);
    void Succeed(const QProcessEnvironment& environment);
    void Finish(QProcessEnvironment environment, bool fromShell);

    Settings Settings_;
    // The markers of this read (a new one each: nothing else prints it).
    QByteArray Begin_;
    QByteArray End_;
    std::unique_ptr<QProcess> Shell_;
    QByteArray Output_;
    QTimer Timeout_;
    bool Started_ = false;
    bool Ready_ = false;
    bool FromShell_ = false;
    QProcessEnvironment Environment_;
};

}  // namespace roost
