#pragma once

#include <QJsonArray>
#include <QMap>
#include <QProcess>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace roost::testing {

// The real roostd binary as a process, in a temporary directory of its own:
// ROOST_SOCKET, ROOST_CONFIG and ROOST_PLUGINS_PATH point into it, so that
// nothing touches the user's ~/.config/roost. Stopped and cleaned up with the
// object.
class RoostdProcess {
public:
    RoostdProcess();
    ~RoostdProcess();

    RoostdProcess(const RoostdProcess&) = delete;
    RoostdProcess& operator=(const RoostdProcess&) = delete;

    bool IsValid() const { return Dir_.isValid(); }
    QString SocketPath() const { return Dir_.filePath("roostd.sock"); }
    // Not created: roostd makes it on the first write.
    QString ConfigPath() const { return Dir_.filePath("config"); }
    QString PluginsPath() const { return Dir_.filePath("plugins"); }

    // A plugin for the next Start: its backend is the fixture backend of
    // tests/helpers/fixture_backend.cpp. It declares the commands fixture.echo,
    // fixture.pid, fixture.crash, fixture.hang and fixture.notify, and the
    // agent
    // "<id>-agent", which has one session, "s1", in state idle.
    bool AddFixturePlugin(const QString& id, const QString& fixtureBackendPath);

    // A plugin for the next Start with QML panels and no backend: plugin.json
    // with the panels as `contributes.panels` entries, written as given, and
    // the files (path relative to the plugin directory -> content). Called
    // again for the same id it rewrites them, which is how a test edits a
    // plugin between two Starts.
    bool AddQmlPlugin(const QString& id, const QJsonArray& panels,
                      const QMap<QString, QByteArray>& files,
                      const QString& version = "0.1.0");

    // Starts roostd and waits until it answers hello. After Stop it starts
    // again on the same socket and directories, and what is on disk stays.
    bool Start(const QString& roostdPath);
    // SIGTERM, then SIGKILL if roostd does not leave in time. True if it left
    // by itself.
    bool Stop();
    bool IsRunning() const;

private:
    QTemporaryDir Dir_;
    std::unique_ptr<QProcess> Process_;
};

}  // namespace roost::testing
