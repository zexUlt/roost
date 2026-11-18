// A window of the shell against the real roostd with the real Claude plugin
// behind a fake `claude`, with the built-in plugins `sessions` and `terminal`:
// what the tests of the sessions panel drive. What the user sees is read from
// the panels, and keys go through the window, to whatever has the focus.

#pragma once

#include <QAbstractItemModel>
#include <QJsonObject>
#include <QKeyEvent>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

#include "api/api.hpp"
#include "qml_api/roost_singleton.hpp"
#include "qml_api/terminal_item.hpp"
#include "roost/plugin_loader.hpp"
#include "roost/shell.hpp"
#include "terminal/terminal_session.hpp"

namespace roost::testing {

inline constexpr int TIMEOUT_MS = 10000;
inline const QString LIST = "sessions.list";
inline const QString TERMINAL = "terminal.view";
inline constexpr int NO_CURSOR = -1;
inline constexpr int ON_HEADER = -2;
inline const QString EMPTY_TEXT = "No session. Choose one in the list.";

// The sessions of tests/fixtures/claude_agents.json that `claude agents` lists,
// in the order roostd answers.
inline const QString ALPHA = "bg-0a1b2c3d";
inline const QString BETA = "bg-1b2c3d4e";
inline const QString GAMMA = "bg-2c3d4e5f";
inline const QString DELTA = "bg-3d4e5f60";
inline const QStringList ORDER = {ALPHA, BETA, GAMMA, DELTA};

// Each query answered once; a digit after the 1 would be more than once.
inline const QRegularExpression ANSWERED_ONCE(
    QStringLiteral(R"(answers da1=1 cpr=1(?!\d))"));

// What row 5 of the TUI shows once "world" has arrived, whatever the split. No
// 'e', 'q' or 'x': fake_tui takes them for commands when one arrives in a read
// of its own.
inline const QRegularExpression TYPED_WORLD(
    QStringLiteral(R"(input (?:d|ld|rld|orld|world) total=5(?!\d))"));

// The modifier Qt delivers for the physical Control key, written out per
// platform on purpose: deriving it from the options under test would pass
// whatever they said. On macOS Qt names the Command key Control and the
// Control key Meta.
#ifdef Q_OS_MACOS
inline constexpr Qt::KeyboardModifier CONTROL_KEY = Qt::MetaModifier;
#else
inline constexpr Qt::KeyboardModifier CONTROL_KEY = Qt::ControlModifier;
#endif

// A fake `claude`, steered by the files next to it: agents.json is what
// `agents` prints, and `attach <id>` runs fake_tui, which shows the id as its
// greeting. In the root directory, so that the greeting shares its row with a
// short cwd and does not wrap.
inline constexpr const char* FAKE_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
attach) cd / && FAKE_TUI_GREETING=$2 exec "$dir/fake_tui" ;;
*) exit 2 ;;
esac
)sh";

// A `claude` that cannot list.
inline constexpr const char* BROKEN_CLAUDE = R"sh(#!/bin/sh
echo "claude is broken" >&2
exit 2
)sh";

// Whether a roostd, perhaps still starting, answers hello at the path.
bool Greets(const QString& socketPath);

// What the terminal shows, as the VT sequences that rebuild it.
QString Screen(const std::shared_ptr<TerminalSession>& session);

// Every warning fails the test but one of Qt's font database, which the first
// text of a process draws with and which depends on the machine's fonts.
void FailOnWarnings();

QString Greeting(const QString& id);

QByteArray ReadFile(const QString& path);

class SessionsWindow : public QObject {
public:
    // The start and the end of a test case: a new directory for the daemon, the
    // fake claude and the config; the window, the daemon and the switches of
    // the case gone.
    void Prepare();
    void Teardown();

    QString SocketPath() const { return Dir_->filePath("roostd.sock"); }
    QString AgentsPath() const { return Dir_->filePath("bin/agents.json"); }

    bool WriteFakeClaude(const char* script) const;

    // `claude agents` lists the sessions of the fixture but those in `gone`;
    // the file is replaced whole, so that no run reads half of it.
    bool ListWithout(const QStringList& gone) const;

    // Only the built-in plugins `claude` and `sessions`: no panel
    // terminal.view.
    bool LinkPluginsWithoutTerminal() const;

    bool StartDaemon();

    // roostd ends the way it does at a SIGTERM.
    void StopDaemon();

    // The window of the shell, connected to the daemon, with the panels
    // loaded; waits until both slots hold theirs and the list has its rows
    // (`rows`, none when the sessions cannot be listed or `empty` says the
    // list starts without any).
    void Boot(int rows = 4, const char* claude = FAKE_CLAUDE,
              bool empty = false);

    // A window of the shell, connected to the running daemon, with the panels
    // loaded; waits until both slots hold theirs and the list has its first
    // sessions.
    void OpenWindow();

    // Whether the list panel is loaded; with SpySelected_, the first time it
    // is, every change of its `selected` is recorded, before any list is in.
    bool WatchSelectedOnLoad();

    // The window goes, the daemon stays: the shell before the loader, the
    // loader before the engine, which holds the terminal that closes its
    // session through the API.
    void CloseWindow();

    QObject* Root() { return Engine_->rootObjects().first(); }
    QQuickWindow* Window() { return qobject_cast<QQuickWindow*>(Root()); }

    QQuickItem* Slot(const QString& type);

    QQuickItem* PanelOf(const QString& type);

    QQuickItem* ListView();

    // The model of the list: its rows are headers and sessions.
    QAbstractItemModel* List();

    QVariant Role(int row, const QByteArray& name);

    // The rows of the sessions, in the order of the list.
    QList<int> SessionRows();

    // The sessions of the list, whatever the headers between them; a row of
    // the view is a session's number here, so that the cases do not count
    // headers.
    int Count() { return static_cast<int>(SessionRows().size()); }
    int CursorRow() { return ListView()->property("currentIndex").toInt(); }
    // The number of the session under the cursor; NO_CURSOR without one,
    // ON_HEADER on a header.
    int Cursor();

    // The row of the view of the session with the number.
    int ViewRow(int session) { return SessionRows().value(session, -1); }

    void Invoke(const char* method, const QString& argument);

    // The kind of every row, "header" or "session", in the order of the list.
    QStringList Kinds();

    // What the list stored for the plugin, as the daemon keeps it.
    QJsonObject StoredDocument();

    QQuickItem* ListStatus();

    QString ListStatusNow();

    QString UnavailableNow();

    // The id of the row of the model, whatever the view shows.
    QString IdAt(int session);

    QString CursorId() { return IdAt(Cursor()); }

    // The ids of the list's sessions, in its order.
    QStringList Ids();

    // The id of the session in the list's output `selected`; empty without one.
    QString SelectedId();

    // The delegate of the view's row; of the session's number below.
    QQuickItem* ViewItem(int viewRow);

    QQuickItem* RowItem(int session) { return ViewItem(ViewRow(session)); }

    QString RowText(int row, const QString& objectName);

    TerminalItem* Terminal();

    QQuickItem* StatusText();

    QString Status() { return Terminal()->status(); }

    // Records "<status>|<text>" at each change of the terminal's status, from
    // now on, so that a state that passes quickly is still seen.
    void Watch();

    bool Opened() const;

    // What the session in the terminal shows now; empty without one.
    QString ScreenNow();

    // Waits until the session has had its queries answered.
    void Settle();

    // A tap at the centre of the row's delegate, through the window.
    void TapRow(int row);

    void Press(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);

    // A press of a physical key on another layout: the Qt key and the text are
    // the letter typed, and the event carries the native codes of both
    // platforms (the macOS virtual key, the xkb keycode).
    void PressNative(int key, const QString& text, quint32 mac, quint32 xkb,
                     Qt::KeyboardModifiers modifiers = Qt::NoModifier);

    // Moves the cursor to the row and opens it with Return; waits until the
    // session is open in the terminal.
    void OpenRow(int row);

    // Ctrl+\ from the terminal; waits until the list has the keyboard.
    void BackToList();

    QStringList Problems();

    // Auto-repeat of a key held down: the press arrives with `autorep` set.
    void PressRepeated(Qt::Key key,
                       Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                       const QString& text = QStringLiteral("\r"));

    // A press as Press, and whether the window's item took the event.
    bool PressAccepted(Qt::Key key,
                       Qt::KeyboardModifiers modifiers = Qt::NoModifier);

protected:
    QStringList Changes_;
    bool WithoutTerminal_ = false;
    bool SpySelected_ = false;
    std::unique_ptr<QSignalSpy> SelectedSpy_;
    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<QProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<QQmlApplicationEngine> Engine_;
    RoostSingleton* Singleton_ = nullptr;
    std::unique_ptr<PluginLoader> Loader_;
    std::unique_ptr<Shell> Shell_;

public:
    // Until the panel has nothing left to write: it writes the document by
    // itself after it is ready (the first read, then the open session once the
    // list is in), one write at a time, the next one sent when the answer to
    // the last comes. A write of a plugin made before that is stored between
    // the panel's own, and the stale one that follows it comes back as a
    // foreign change that replaces what the plugin wrote. `restored` is the
    // last event that makes the panel write, so at most one write is in flight
    // and one follows it. The answer to a request comes after those of the
    // requests sent before it, on the one connection and in one order of
    // delivery: so after the first round trip the follow-up is sent, and after
    // the second its answer is in too.
    void WaitForThePanelToStopWriting();
};

}  // namespace roost::testing
