// The command palette (CommandPalette.qml, PaletteModel) in the window of the
// shell, against the real roostd with the built-in plugins, two plugins of the
// case that bind one key, a plugin whose backend is the fixture backend, and a
// keymap.json. The palette is opened by the key of the command shell.palette,
// as a user does, and typed into through the window. Enter is seen where the
// command lands: a local command (shell.focusList) in the handler of the
// window, a command of a backend (fixture.crash) in roostd, which tells that
// the backend died. HOME, ROOST_SOCKET and ROOST_CONFIG are temporary.

#include <QAbstractItemModel>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExtensionPlugin>
#include <QQuickItemGrabResult>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTest>

#include <cmath>
#include <optional>

#include "helpers/scoped_env.hpp"
#include "helpers/sessions_window.hpp"

using namespace roost;
using namespace roost::testing;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

// Qt's Control is Command on macOS, so the same two modifiers are the key of
// the palette on both platforms: Cmd+Shift+P and Ctrl+Shift+P.
constexpr Qt::KeyboardModifiers PALETTE_MODIFIERS =
    Qt::ControlModifier | Qt::ShiftModifier;

#ifdef Q_OS_MACOS
const QString PALETTE_KEY = QStringLiteral("Cmd+Shift+P");
constexpr bool IS_MAC = true;
#else
const QString PALETTE_KEY = QStringLiteral("Ctrl+Shift+P");
constexpr bool IS_MAC = false;
#endif

QJsonObject Command(const QString& id, const QString& title) {
    return {{"id", id}, {"title", title}};
}

QJsonObject Key(const QString& command, const QString& key,
                const QString& when = {}) {
    QJsonObject binding{{"command", command}, {"key", key}};
    if (!when.isEmpty()) {
        binding.insert("when", when);
    }
    return binding;
}

// The words of a pattern as a regular expression that asks the same as the
// filter, written another way: the letters in order with anything between.
bool Subsequence(const QString& pattern, const QString& text) {
    QString expression;
    for (const QChar letter : pattern) {
        expression += QRegularExpression::escape(letter) + QStringLiteral(".*");
    }
    return QRegularExpression(
               expression, QRegularExpression::CaseInsensitiveOption |
                               QRegularExpression::DotMatchesEverythingOption)
        .match(text)
        .hasMatch();
}

// While it lives, counts the warnings that match the pattern and keeps them
// out of the test log; every other message goes on to the test's handler, so
// FailOnWarnings still holds for them. Unlike QTest::ignoreMessage, the test
// can wait for the warning: one that comes after an event the test waits for
// would otherwise come after the test.
class ExpectedWarning {
public:
    explicit ExpectedWarning(const QRegularExpression& pattern) {
        PATTERN = pattern;
        SEEN = 0;
        PREVIOUS = qInstallMessageHandler([](QtMsgType type,
                                             const QMessageLogContext& context,
                                             const QString& text) {
            if (type == QtWarningMsg && PATTERN.match(text).hasMatch()) {
                ++SEEN;
                return;
            }
            PREVIOUS(type, context, text);
        });
    }
    ~ExpectedWarning() { qInstallMessageHandler(PREVIOUS); }

    ExpectedWarning(const ExpectedWarning&) = delete;
    ExpectedWarning& operator=(const ExpectedWarning&) = delete;

    int Seen() const { return SEEN; }

private:
    static inline QRegularExpression PATTERN;
    static inline int SEEN = 0;
    static inline QtMessageHandler PREVIOUS = nullptr;
};

}  // namespace

class PaletteTest : public SessionsWindow {
    Q_OBJECT

private:
    // A plugin of the case: a manifest without a backend.
    bool WritePlugin(const QString& id, const QJsonObject& contributes,
                     const QJsonObject& more = {}) {
        const QDir directory(Dir_->filePath("plugins/" + id));
        if (!directory.mkpath(".")) {
            return false;
        }
        QJsonObject manifest{{"id", id},
                             {"version", "1"},
                             {"api", "0"},
                             {"contributes", contributes}};
        for (auto it = more.begin(); it != more.end(); ++it) {
            manifest.insert(it.key(), it.value());
        }
        QFile file(directory.filePath("plugin.json"));
        return file.open(QIODevice::WriteOnly) &&
               file.write(QJsonDocument(manifest).toJson()) > 0;
    }

    // The plugins of the case: `pa` and `pb` bind Ctrl+Alt+K (`pa` is loaded
    // first and keeps it), `pa.delta` has a key in the list panel only,
    // `pa.epsilon` has one key in the list panel and in any panel, and
    // `fixture` has the fixture backend, whose command fixture.crash kills it.
    bool WritePlugins() {
        if (!WritePlugin(
                "pa",
                {{"commands",
                  QJsonArray{Command("pa.alpha", "Alpha command"),
                             Command("pa.beta", "Beta command"),
                             Command("pa.delta", "Delta command"),
                             Command("pa.epsilon", "Epsilon command")}},
                 {"keybindings",
                  QJsonArray{Key("pa.alpha", "Ctrl+Alt+K"),
                             Key("pa.delta", "Ctrl+Alt+L", "sessions.list"),
                             Key("pa.epsilon", "Ctrl+Alt+M", "sessions.list"),
                             Key("pa.epsilon", "Ctrl+Alt+M")}}}) ||
            !WritePlugin(
                "pb",
                {{"commands", QJsonArray{Command("pb.gamma", "Gamma command")}},
                 {"keybindings", QJsonArray{Key("pb.gamma", "Ctrl+Alt+K")}}})) {
            return false;
        }
        const QDir fixture(Dir_->filePath("plugins/fixture"));
        if (!fixture.mkpath(".") ||
            !QFile::link(QStringLiteral(FIXTURE_BACKEND_PATH),
                         fixture.filePath("backend"))) {
            return false;
        }
        QFile answers(fixture.filePath("agent.json"));
        const QJsonObject listing{
            {"agent.listSessions",
             QJsonObject{{"fixture-agent",
                          QJsonObject{{"result", QJsonArray{QJsonObject{
                                                     {"id", "s1"},
                                                     {"state", "idle"}}}}}}}}};
        return answers.open(QIODevice::WriteOnly) &&
               answers.write(QJsonDocument(listing).toJson()) > 0 &&
               WritePlugin(
                   "fixture",
                   {{"commands",
                     QJsonArray{Command("fixture.crash", "Crash the fixture")}},
                    {"agents",
                     QJsonArray{QJsonObject{
                         {"id", "fixture-agent"},
                         {"capabilities", QJsonArray{"nativeTuiAttach"}}}}}},
                   {{"backend",
                     QJsonObject{{"command", QJsonArray{"backend"}}}}});
    }

    // A user keymap.json, there before the daemon starts or replaced while it
    // runs.
    bool WriteKeymap(const QByteArray& json) {
        if (!QDir().mkpath(Dir_->filePath("config"))) {
            return false;
        }
        QSaveFile file(Dir_->filePath("config/keymap.json"));
        return file.open(QIODevice::WriteOnly) && file.write(json) >= 0 &&
               file.commit();
    }

    // The case's window with the plugins above; four sessions of the fake
    // `claude` and the one of the fixture agent.
    void BootWithPlugins() {
        QVERIFY(WritePlugins());
        Boot(5);
    }

    QAbstractItemModel* Model() {
        return Root()->findChild<QAbstractItemModel*>("paletteModel");
    }

    QObject* Palette() { return Root()->findChild<QObject*>("commandPalette"); }

    bool IsOpen() { return Palette()->property("visible").toBool(); }

    QObject* BugReport() {
        return Root()->findChild<QObject*>("bugReportDialog");
    }

    QQuickItem* Field() {
        return Palette()->findChild<QQuickItem*>("paletteField");
    }

    // The value of a role of a shown row.
    QVariant Cell(int row, const char* role) {
        const QHash<int, QByteArray> names = Model()->roleNames();
        return Model()->data(Model()->index(row, 0), names.key(role, -1));
    }

    QStringList Shown() {
        QStringList ids;
        for (int row = 0; row < Model()->rowCount(); ++row) {
            ids.append(Cell(row, "commandId").toString());
        }
        return ids;
    }

    // The row of a command among the shown rows; -1 if it is not there.
    int RowOf(const QString& command) {
        return static_cast<int>(Shown().indexOf(command));
    }

    QVariant CellOf(const QString& command, const char* role) {
        const int row = RowOf(command);
        return row < 0 ? QVariant() : Cell(row, role);
    }

    // The commands that plugins.list declares, as the manifests order them:
    // the plugins in the order roostd loaded them, then each one's commands.
    QStringList DeclaredCommands() {
        QStringList ids;
        const QJsonArray plugins =
            Api_->Plugins().Plugins().value("plugins").toArray();
        for (const QJsonValue& plugin : plugins) {
            const QJsonArray commands = plugin.toObject()
                                            .value("contributes")
                                            .toObject()
                                            .value("commands")
                                            .toArray();
            for (const QJsonValue& command : commands) {
                ids.append(command.toObject().value("id").toString());
            }
        }
        return ids;
    }

    // The item with the objectName among the visual descendants of `item`.
    static QQuickItem* Descendant(QQuickItem* item, const QString& name) {
        for (QQuickItem* child : item->childItems()) {
            if (child->objectName() == name) {
                return child;
            }
            if (QQuickItem* found = Descendant(child, name)) {
                return found;
            }
        }
        return nullptr;
    }

    // The key label of the delegate of a shown command, as it reads on the
    // screen; "" if the command has no delegate.
    QString ShownKeyLabel(const QString& command) {
        auto* const content =
            Palette()->property("contentItem").value<QQuickItem*>();
        // The delegates are the children of the content item of the list.
        auto* const list = Descendant(content, "paletteList");
        if (list == nullptr) {
            return {};
        }
        const auto* view = list->property("contentItem").value<QQuickItem*>();
        for (QQuickItem* delegate : view->childItems()) {
            if (delegate->property("commandId").toString() != command) {
                continue;
            }
            const QQuickItem* label = Descendant(delegate, "paletteKey");
            return label == nullptr ? QString()
                                    : label->property("text").toString();
        }
        return {};
    }

    // Empty if the delegate of the current row lies inside the viewport of the
    // list (to half a pixel); else what is wrong with it.
    QString CurrentRowProblem() {
        constexpr qreal tolerance = 0.5;
        auto* const list =
            Descendant(Palette()->property("contentItem").value<QQuickItem*>(),
                       "paletteList");
        if (list == nullptr) {
            return QStringLiteral("no list");
        }
        const int current = list->property("currentIndex").toInt();
        const auto* view = list->property("contentItem").value<QQuickItem*>();
        for (QQuickItem* delegate : view->childItems()) {
            if (delegate->objectName() != "paletteRow" ||
                delegate->property("index").toInt() != current) {
                continue;
            }
            const qreal top = delegate->mapToItem(list, QPointF(0, 0)).y();
            const qreal bottom =
                delegate->mapToItem(list, QPointF(0, delegate->height())).y();
            if (top >= -tolerance && bottom <= list->height() + tolerance) {
                return {};
            }
            return QStringLiteral(
                       "row %1 out of view: top %2, bottom %3, list height %4")
                .arg(current)
                .arg(top)
                .arg(bottom)
                .arg(list->height());
        }
        return QStringLiteral("no delegate for row %1").arg(current);
    }

    // The delegate of a shown command; null if the list has none for it.
    QQuickItem* RowDelegate(const QString& command) {
        const auto* const list = PaletteItem("paletteList");
        const auto* const view =
            list->property("contentItem").value<QQuickItem*>();
        for (QQuickItem* delegate : view->childItems()) {
            if (delegate->objectName() == "paletteRow" &&
                delegate->property("commandId").toString() == command) {
                return delegate;
            }
        }
        return nullptr;
    }

    // The card: the item of the popup, the parent of its content.
    QQuickItem* Card() {
        return Palette()
            ->property("contentItem")
            .value<QQuickItem*>()
            ->parentItem();
    }

    QQuickItem* PaletteItem(const QString& name) {
        return Descendant(
            Palette()->property("contentItem").value<QQuickItem*>(), name);
    }

    // The height of the card as its parts add up: the header with its top
    // margin, the field with its top margin, the list with its margins (or,
    // without rows, the line that says so), and the footer with its hairline.
    // The numbers are the margins of CommandPalette.qml.
    qreal ContentHeight() {
        constexpr qreal headerTop = 17;
        constexpr qreal fieldTop = 6;
        constexpr qreal listMargins = 16;
        constexpr qreal emptyBottom = 8;
        constexpr qreal hairline = 1;
        const QQuickItem* const heading = PaletteItem("paletteHeading");
        const QQuickItem* const footer =
            PaletteItem("paletteFooterKeys")->parentItem();
        const QQuickItem* const empty = PaletteItem("paletteEmpty");
        qreal sum =
            headerTop + heading->parentItem()->height() + fieldTop +
            Field()->height() + listMargins +
            PaletteItem("paletteList")->property("contentHeight").toReal() +
            hairline + footer->height();
        if (empty->isVisible()) {
            sum += empty->height() + emptyBottom;
        }
        return sum;
    }

    // The palette opened by its key, as the user does; the field has the
    // keyboard.
    void OpenByKey() {
        QVERIFY(!IsOpen());
        Press(Qt::Key_P, PALETTE_MODIFIERS);
        QTRY_VERIFY_WITH_TIMEOUT(IsOpen(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Field()->hasActiveFocus(), TIMEOUT_MS);
        // The layout gives the list its height on a later frame than the
        // popup shows; the tests read the rows in view.
        const auto* const list =
            Descendant(Palette()->property("contentItem").value<QQuickItem*>(),
                       "paletteList");
        QVERIFY(list != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(list->height() > 0, TIMEOUT_MS);
    }

    // Saves a picture of the window as <case>-<step>.png when asked to
    // (ROOST_TEST_SNAPSHOTS), as layout_tabs_menu_test does.
    void Snapshot(const QString& step) {
        const QString directory = qEnvironmentVariable("ROOST_TEST_SNAPSHOTS");
        if (directory.isEmpty()) {
            return;
        }
        QVERIFY(QDir().mkpath(directory));
        const QString name =
            QStringLiteral("%1-%2.png")
                .arg(QString::fromLatin1(QTest::currentTestFunction()), step);
        // The content item, not the window: grabWindow distorts the terminal
        // in the offscreen platform.
        QSharedPointer<QQuickItemGrabResult> result =
            Window()->contentItem()->grabToImage();
        QVERIFY(!result.isNull());
        QSignalSpy ready(result.data(), &QQuickItemGrabResult::ready);
        QVERIFY(ready.wait(TIMEOUT_MS) || !ready.isEmpty());
        QVERIFY(result->saveToFile(QDir(directory).filePath(name)));
    }

    void Type(const QString& text) {
        for (const QChar letter : text) {
            QTest::keyClick(Window(), letter.toLatin1());
        }
        QCOMPARE(Field()->property("text").toString(), text);
    }

    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

private slots:
    void init() {
        Prepare();
        QVERIFY(QDir().mkpath(Dir_->filePath("home")));
        Home_.emplace("HOME", Dir_->filePath("home").toUtf8());
        Socket_.emplace("ROOST_SOCKET", SocketPath().toUtf8());
        Config_.emplace("ROOST_CONFIG", Dir_->filePath("config").toUtf8());
    }

    void cleanup() {
        Teardown();
        Config_.reset();
        Socket_.reset();
        Home_.reset();
    }

    // Every command of every manifest is a row, in the order of plugins.list;
    // the filter keeps the rows with the letters in order, whatever the case,
    // against the title or the id, and does not reorder them.
    void TheFilterIsFuzzyCaseBlindAndStable() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QStringList declared = DeclaredCommands();
        QVERIFY(declared.contains("pa.alpha"));
        QVERIFY(declared.contains("shell.palette"));
        QVERIFY(declared.contains("fixture.crash"));
        QCOMPARE(Shown(), declared);
        QCOMPARE(Model()->property("total").toInt(),
                 static_cast<int>(declared.size()));

        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        // The letters of a title, apart; the title or the id counts.
        const QStringList patterns = {"bta",   "BTA",  "cmd",  "pb.g", "a",
                                      "fcrsh", "zzzz", "p.al", "cmmnd"};
        for (const QString& pattern : patterns) {
            Field()->setProperty("text", pattern);
            QStringList expected;
            for (const QString& command : declared) {
                // The title of a command is the one its manifest gave.
                const QString title = [&] {
                    const QJsonArray plugins =
                        Api_->Plugins().Plugins().value("plugins").toArray();
                    for (const QJsonValue& plugin : plugins) {
                        for (const QJsonValue& item : plugin.toObject()
                                                          .value("contributes")
                                                          .toObject()
                                                          .value("commands")
                                                          .toArray()) {
                            if (item.toObject().value("id") == command) {
                                return item.toObject()
                                    .value("title")
                                    .toString();
                            }
                        }
                    }
                    return QString();
                }();
                if (Subsequence(pattern, title) ||
                    Subsequence(pattern, command)) {
                    expected.append(command);
                }
            }
            QCOMPARE(Shown(), expected);
        }
        // Spot checks, so that the reference above is not the only witness.
        Field()->setProperty("text", "BTA");
        QCOMPARE(Shown(), QStringList{"pa.beta"});
        Field()->setProperty("text", "pb.g");
        QCOMPARE(Shown(), QStringList{"pb.gamma"});
        Field()->setProperty("text", "zzzz");
        QCOMPARE(Shown(), QStringList{});
        Field()->setProperty("text", "");
        QCOMPARE(Shown(), declared);
    }

    // The two commands that bind Ctrl+Alt+K are marked, and each row names
    // both plugins; roostd gave the key to the first plugin only.
    void AConflictRowNamesBothPlugins() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(CellOf("pa.alpha", "key").toString(), "Ctrl+Alt+K");
        QCOMPARE(CellOf("pb.gamma", "key").toString(), "");
        for (const QString& command : {"pa.alpha", "pb.gamma"}) {
            const QString conflict = CellOf(command, "conflict").toString();
            QVERIFY2(conflict.contains("Ctrl+Alt+K"), qPrintable(conflict));
            QVERIFY2(conflict.contains("pa keeps it"), qPrintable(conflict));
            QVERIFY2(conflict.contains("pb is ignored"), qPrintable(conflict));
        }
        QCOMPARE(CellOf("pa.beta", "conflict").toString(), "");
        QCOMPARE(CellOf("shell.palette", "conflict").toString(), "");
        // Nothing here is from the user.
        QCOMPARE(CellOf("pa.alpha", "overridden").toBool(), false);
    }

    // A key of the file that takes a key of a plugin is shown on both sides:
    // the command that has it now, and the command that lost it; a row the
    // file leaves alone is not marked. A change of the file shows at once.
    void AnOverriddenEntryIsMarkedFromKeymapJson() {
        FailOnWarnings();
        QVERIFY(
            WriteKeymap(R"([{"key": "Ctrl+Alt+K", "command": "pa.beta"}])"));
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(CellOf("pa.beta", "key").toString(), "Ctrl+Alt+K");
        QCOMPARE(CellOf("pa.beta", "overridden").toBool(), true);
        // pa.alpha and pb.gamma wanted the key and lost it to the file: no
        // conflict is left, the override is.
        QCOMPARE(CellOf("pa.alpha", "key").toString(), "");
        QCOMPARE(CellOf("pa.alpha", "overridden").toBool(), true);
        QCOMPARE(CellOf("pb.gamma", "overridden").toBool(), true);
        QCOMPARE(CellOf("pa.alpha", "conflict").toString(), "");
        QCOMPARE(CellOf("pb.gamma", "conflict").toString(), "");
        QCOMPARE(CellOf("pa.delta", "overridden").toBool(), false);
        QCOMPARE(CellOf("fixture.crash", "overridden").toBool(), false);
        QCOMPARE(CellOf("shell.palette", "overridden").toBool(), false);

        // The file changes under the running window.
        QVERIFY(
            WriteKeymap(R"([{"key": "Ctrl+Alt+J", "command": "pa.delta"}])"));
        QTRY_COMPARE_WITH_TIMEOUT(CellOf("pa.delta", "overridden").toBool(),
                                  true, TIMEOUT_MS);
        // Its own key stays, the file's comes after it.
        QCOMPARE(CellOf("pa.delta", "key").toString(),
                 "Ctrl+Alt+L (sessions.list), Ctrl+Alt+J");
        QCOMPARE(CellOf("pa.beta", "overridden").toBool(), false);
        // The conflict is back, as the file no longer takes the key.
        QVERIFY(!CellOf("pa.alpha", "conflict").toString().isEmpty());
    }

    // The marks of the keys that the terminal needs: Ctrl with a letter,
    // active in the terminal (no `when`, or terminal.view); a key of another
    // panel, a key with Cmd and a key that is not a letter are not.
    void TheMarkOfCapturedInputIsOnKeysTheTerminalNeeds() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        // Ctrl+Alt+K, any panel.
        QCOMPARE(CellOf("pa.alpha", "capturesInput").toBool(), true);
        // Ctrl+Alt+L, in the list only.
        QCOMPARE(CellOf("pa.delta", "key").toString(),
                 "Ctrl+Alt+L (sessions.list)");
        QCOMPARE(CellOf("pa.delta", "capturesInput").toBool(), false);
        // Ctrl+\ is a control character too, but not a letter.
        QCOMPARE(CellOf("shell.focusList", "capturesInput").toBool(), false);
        // No key at all.
        QCOMPARE(CellOf("pa.beta", "capturesInput").toBool(), false);
        // Cmd+Shift+P on macOS, Ctrl+Shift+P elsewhere; the same for copy.
        QCOMPARE(CellOf("shell.palette", "key").toString(), PALETTE_KEY);
        QCOMPARE(CellOf("shell.palette", "capturesInput").toBool(), !IS_MAC);
        QCOMPARE(
            CellOf("terminal.copy", "key").toString(),
            IS_MAC ? "Cmd+C (terminal.view)" : "Ctrl+Shift+C (terminal.view)");
        QCOMPARE(CellOf("terminal.copy", "capturesInput").toBool(), !IS_MAC);
    }

    // The row shows each key with the panel it is active in: the same key in
    // the list and in any panel are two items, as the label of the row reads.
    void TheRowShowsEachKeyWithItsPanel() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        Type("pa.eps");
        QCOMPARE(Shown(), QStringList{"pa.epsilon"});
        const QString expected = "Ctrl+Alt+M (sessions.list), Ctrl+Alt+M";
        QTRY_COMPARE_WITH_TIMEOUT(ShownKeyLabel("pa.epsilon"), expected,
                                  TIMEOUT_MS);
        Field()->setProperty("text", "pa.del");
        QTRY_COMPARE_WITH_TIMEOUT(ShownKeyLabel("pa.delta"),
                                  QString("Ctrl+Alt+L (sessions.list)"),
                                  TIMEOUT_MS);
    }

    // The rows are rebuilt under an open palette (keymap.json changes): the
    // chosen command stays chosen, and Enter runs it.
    void TheChosenRowSurvivesARebuild() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        QVERIFY(Terminal()->hasActiveFocus());
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        const int target = RowOf("shell.focusList");
        QVERIFY(target > 0);
        for (int step = 0; step < target; ++step) {
            Press(Qt::Key_Down);
        }
        QCOMPARE(Palette()->property("currentRow").toInt(), target);

        QVERIFY(
            WriteKeymap(R"([{"key": "Ctrl+Alt+J", "command": "pa.delta"}])"));
        QTRY_COMPARE_WITH_TIMEOUT(CellOf("pa.delta", "overridden").toBool(),
                                  true, TIMEOUT_MS);
        QCOMPARE(Palette()->property("currentRow").toInt(),
                 RowOf("shell.focusList"));
        Press(Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!IsOpen(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
    }

    // The plugins change under an open palette (roostd restarts without `pa`):
    // the rows above the chosen one are gone, and the chosen command stays
    // chosen, wherever its row is now.
    void TheChosenCommandSurvivesTheLossOfRowsAboveIt() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        const int before = RowOf("pb.gamma");
        QVERIFY(before > 0);
        for (int step = 0; step < before; ++step) {
            Press(Qt::Key_Down);
        }
        QCOMPARE(Palette()->property("currentRow").toInt(), before);

        StopDaemon();
        QVERIFY(QDir(Dir_->filePath("plugins/pa")).removeRecursively());
        QVERIFY(StartDaemon());
        QTRY_VERIFY_WITH_TIMEOUT(
            RowOf("pa.alpha") < 0 && RowOf("pb.gamma") >= 0, TIMEOUT_MS * 2);
        QVERIFY(RowOf("pb.gamma") < before);
        QCOMPARE(Palette()->property("currentRow").toInt(), RowOf("pb.gamma"));
    }

    // The palette opens by its key from the terminal, and Escape closes it
    // and gives the keyboard back to the terminal; typed text is not sent to
    // the terminal, and a command that was typed but not accepted is not run.
    void EscapeClosesAndReturnsTheKeyboardToTheTerminal() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        QVERIFY(Terminal()->hasActiveFocus());
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(!Terminal()->hasActiveFocus());
        Type("focus");
        QCOMPARE(CellOf("shell.focusList", "commandId").toString(),
                 "shell.focusList");
        Press(Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!IsOpen(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasActiveFocus(), TIMEOUT_MS);
        // Not run: the keyboard did not go to the list.
        QTest::qWait(200);
        QVERIFY(Terminal()->hasActiveFocus());
        QVERIFY(!ListView()->hasActiveFocus());
    }

    // From the list the palette gives the keyboard back to the list.
    void EscapeReturnsTheKeyboardToTheList() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        ListView()->forceActiveFocus();
        QVERIFY(ListView()->hasActiveFocus());
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!IsOpen(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
    }

    // Enter on a local command: the handler in the window runs, after the
    // palette has closed, so the keyboard ends in the list and not back in the
    // terminal that had it.
    void EnterRunsALocalCommandAfterTheClose() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Api_->Commands().HasLocal("shell.focusList"));
        OpenRow(1);
        QVERIFY(Terminal()->hasActiveFocus());
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        Type("focus the");
        QCOMPARE(Shown(), QStringList{"shell.focusList"});
        Press(Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!IsOpen(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(!Terminal()->hasActiveFocus());
        // The palette is reopened clean.
        OpenByKey();
        QCOMPARE(Field()->property("text").toString(), "");
        QCOMPARE(Shown(), DeclaredCommands());
    }

    // A command that opens a dialog (shell.reportBug) from the palette: the
    // dialog is shown and has the keyboard.
    void EnterOnReportBugOpensItsDialog() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(BugReport() != nullptr);
        QVERIFY(!BugReport()->property("visible").toBool());
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        Type("report");
        QCOMPARE(Shown(), QStringList{"shell.reportBug"});
        Press(Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!IsOpen(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(BugReport()->property("visible").toBool(),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            Root()
                ->findChild<QQuickItem*>("descriptionField")
                ->hasActiveFocus(),
            TIMEOUT_MS);
    }

    // Enter on a command of a backend: roostd gets commands.run, and the
    // fixture backend, which exits at fixture.crash, is seen to die.
    void EnterRunsACommandOfABackendThroughRoostd() {
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        QStringList crashed;
        const auto subscription = Api_->Bus().Subscribe(
            "plugin.backendCrashed", Root(),
            [&crashed](const QString&, const QJsonValue& payload) {
                // Other plugins of the boot (codex has no backend here) crash
                // and restart on their own; only the fixture is checked.
                const QString plugin =
                    payload.toObject().value("plugin").toString();
                if (plugin == "fixture") {
                    crashed.append(plugin);
                }
            });
        QVERIFY(subscription.has_value());
        QVERIFY(!Api_->Commands().HasLocal("fixture.crash"));
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        Type("crash");
        QCOMPARE(Shown(), QStringList{"fixture.crash"});
        // One row: Down stays on it.
        Press(Qt::Key_Down);
        QCOMPARE(Palette()->property("currentRow").toInt(), 0);
        Press(Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!IsOpen(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(crashed, QStringList{"fixture"}, TIMEOUT_MS);
    }

    // Down picks another row than the first, and Enter runs that one.
    void TheArrowKeysChooseTheRowThatEnterRuns() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        // The rows of shell: focusList, reportBug, palette, copy, in order of
        // the manifest; "shell." keeps those four and no others.
        Type("shell.");
        const QStringList shown = Shown();
        QCOMPARE(shown.value(0), "shell.focusList");
        QVERIFY(shown.size() >= 2);
        QCOMPARE(Palette()->property("currentRow").toInt(), 0);
        Press(Qt::Key_Down);
        QCOMPARE(Palette()->property("currentRow").toInt(), 1);
        Press(Qt::Key_Up);
        QCOMPARE(Palette()->property("currentRow").toInt(), 0);
        Press(Qt::Key_Up);
        QCOMPARE(Palette()->property("currentRow").toInt(), 0);
        Press(Qt::Key_Down);
        QCOMPARE(Palette()->property("currentRow").toInt(), 1);
        Snapshot("typed-row-selected");
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!IsOpen(), TIMEOUT_MS);
    }

    // Down past the rows that fit keeps the chosen row in view, Up brings the
    // first one back, and Enter runs the row that is shown as chosen.
    void TheChosenRowStaysInView() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        QStringList crashed;
        const auto subscription = Api_->Bus().Subscribe(
            "plugin.backendCrashed", Root(),
            [&crashed](const QString&, const QJsonValue& payload) {
                // Other plugins of the boot (codex has no backend here) crash
                // and restart on their own; only the fixture is checked.
                const QString plugin =
                    payload.toObject().value("plugin").toString();
                if (plugin == "fixture") {
                    crashed.append(plugin);
                }
            });
        QVERIFY(subscription.has_value());
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        auto* const list =
            Descendant(Palette()->property("contentItem").value<QQuickItem*>(),
                       "paletteList");
        QVERIFY(list != nullptr);
        const int rows = Model()->rowCount();
        const int target = RowOf("fixture.crash");
        QVERIFY(target >= 0);
        // The rows that fit, from the height of the list and of the first row.
        const auto* view = list->property("contentItem").value<QQuickItem*>();
        qreal rowHeight = 0;
        for (QQuickItem* delegate : view->childItems()) {
            if (delegate->objectName() == "paletteRow") {
                rowHeight = delegate->height();
                break;
            }
        }
        QVERIFY(rowHeight > 0);
        const int fit = static_cast<int>(list->height() / rowHeight);
        if (rows <= fit + 1) {
            QFAIL("The palette has too few commands to scroll the list");
        }
        QVERIFY2(CurrentRowProblem().isEmpty(),
                 qPrintable(CurrentRowProblem()));
        // To the last row; every step ends with the chosen row in view.
        for (int step = 1; step < rows; ++step) {
            Press(Qt::Key_Down);
            QCOMPARE(Palette()->property("currentRow").toInt(), step);
            QVERIFY2(CurrentRowProblem().isEmpty(),
                     qPrintable(
                         CurrentRowProblem() + ", contentY " +
                         QString::number(list->property("contentY").toReal())));
        }
        QVERIFY(list->property("contentY").toReal() > 0);
        // Back to the top: the first row is shown again.
        for (int step = 1; step < rows; ++step) {
            Press(Qt::Key_Up);
            QVERIFY2(CurrentRowProblem().isEmpty(),
                     qPrintable(CurrentRowProblem()));
        }
        QCOMPARE(Palette()->property("currentRow").toInt(), 0);
        // Rows differ in height: the top of the content is originY, not 0.
        QCOMPARE(list->property("contentY").toReal(),
                 list->property("originY").toReal());
        // Down to the crash command, in view, and Enter runs it.
        for (int step = 0; step < target; ++step) {
            Press(Qt::Key_Down);
        }
        QCOMPARE(Palette()->property("currentRow").toInt(), target);
        QVERIFY2(CurrentRowProblem().isEmpty(),
                 qPrintable(CurrentRowProblem()));
        // The fixture backend exits at the command, and the shell says so.
        // roostd publishes the crash before it fails the request, so the
        // warning may come after the event.
        const ExpectedWarning failed(QRegularExpression(
            QStringLiteral("commands\\.run failed: the backend of fixture")));
        Press(Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!IsOpen(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(crashed, QStringList{"fixture"}, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(failed.Seen(), 1, TIMEOUT_MS);
    }

    // A filter that shrinks the list after Down went to the last row: the list
    // is back at the top with the first row chosen and no warning, and clearing
    // the filter brings the whole list back with the chosen row in view.
    void AFilterThatShrinksTheListScrollsToTheTop() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        auto* const list =
            Descendant(Palette()->property("contentItem").value<QQuickItem*>(),
                       "paletteList");
        QVERIFY(list != nullptr);
        const int rows = Model()->rowCount();
        QVERIFY(rows > 3);
        for (int step = 1; step < rows; ++step) {
            Press(Qt::Key_Down);
        }
        QCOMPARE(Palette()->property("currentRow").toInt(), rows - 1);
        QVERIFY(list->property("contentY").toReal() > 0);
        QVERIFY2(CurrentRowProblem().isEmpty(),
                 qPrintable(CurrentRowProblem()));

        Type("gamma");
        const int shown = Model()->rowCount();
        QVERIFY(shown >= 1 && shown < rows);
        QVERIFY(RowOf("pb.gamma") >= 0);
        QCOMPARE(Palette()->property("currentRow").toInt(), 0);
        const qreal top = list->property("originY").toReal();
        QCOMPARE(list->property("contentY").toReal(), top);
        QTRY_VERIFY_WITH_TIMEOUT(CurrentRowProblem().isEmpty(), TIMEOUT_MS);

        // Clear the filter.
        for (int step = 0; step < 5; ++step) {
            Press(Qt::Key_Backspace);
        }
        QCOMPARE(Field()->property("text").toString(), QString());
        QCOMPARE(Model()->rowCount(), rows);
        QCOMPARE(Palette()->property("currentRow").toInt(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(CurrentRowProblem().isEmpty(), TIMEOUT_MS);
    }

    // A rebuild of the rows (the keymap changes) while the chosen command is
    // the last row: the restored row is in view.
    void TheRestoredRowAfterARebuildIsInView() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        const int rows = Model()->rowCount();
        QVERIFY(rows > 3);
        for (int step = 1; step < rows; ++step) {
            Press(Qt::Key_Down);
        }
        const QString chosen = Shown().last();
        QVERIFY2(CurrentRowProblem().isEmpty(),
                 qPrintable(CurrentRowProblem()));

        QVERIFY(
            WriteKeymap(R"([{"key": "Ctrl+Alt+J", "command": "pa.delta"}])"));
        QTRY_COMPARE_WITH_TIMEOUT(CellOf("pa.delta", "overridden").toBool(),
                                  true, TIMEOUT_MS);
        QCOMPARE(Palette()->property("currentRow").toInt(), RowOf(chosen));
        QVERIFY(Palette()->property("currentRow").toInt() > 0);
        QTRY_VERIFY_WITH_TIMEOUT(CurrentRowProblem().isEmpty(), TIMEOUT_MS);
    }

    // A few rows: the card is as high as its parts, well below half of the
    // window, and the list does not scroll.
    void TheCardIsAsHighAsAFewRows() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        Type("pb.gamma");
        QCOMPARE(Model()->rowCount(), 1);
        const qreal half = Window()->height() / 2.0;
        QTRY_COMPARE_WITH_TIMEOUT(Palette()->property("height").toReal(),
                                  ContentHeight(), TIMEOUT_MS);
        QVERIFY(Palette()->property("height").toReal() < half - 50);
        const auto* const list = PaletteItem("paletteList");
        QVERIFY(list->property("contentHeight").toReal() <=
                list->height() + 0.5);
        // The one row has its delegate, is not lower than the row of the
        // style guide and is in view.
        const QQuickItem* const delegate = RowDelegate("pb.gamma");
        QVERIFY(delegate != nullptr);
        QVERIFY(delegate->height() >= 47);
        QVERIFY2(CurrentRowProblem().isEmpty(),
                 qPrintable(CurrentRowProblem()));
        Snapshot("few-rows");
    }

    // A command with a long key and both marks: the title keeps at least half
    // of the row, the key is cut on the right, and nothing of the row goes
    // past the inner right edge of the card.
    void ALongKeyDoesNotCrowdTheTitleOrLeaveTheCard() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(
            WriteKeymap(R"([{"key": "Ctrl+Alt+J", "command": "pa.delta"}])"));
        QTRY_COMPARE_WITH_TIMEOUT(CellOf("pa.delta", "overridden").toBool(),
                                  true, TIMEOUT_MS);
        QCOMPARE(CellOf("pa.delta", "key").toString(),
                 "Ctrl+Alt+L (sessions.list), Ctrl+Alt+J");
        QCOMPARE(CellOf("pa.delta", "capturesInput").toBool(), true);
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        Type("pa.delta");
        QCOMPARE(Model()->rowCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(RowDelegate("pa.delta") != nullptr,
                                 TIMEOUT_MS);
        QQuickItem* const delegate = RowDelegate("pa.delta");
        QQuickItem* const title = Descendant(delegate, "paletteTitle");
        QQuickItem* const key = Descendant(delegate, "paletteKey");
        QQuickItem* const overridden =
            Descendant(delegate, "paletteOverridden");
        QQuickItem* const captures = Descendant(delegate, "paletteCaptures");
        QVERIFY(title != nullptr && key != nullptr && overridden != nullptr &&
                captures != nullptr);
        QVERIFY(overridden->isVisible() && captures->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(key->property("truncated").toBool(),
                                 TIMEOUT_MS);
        const qreal inner = Card()->width() - 16;
        const qreal rowWidth = delegate->width() - 24;
        QVERIFY(title->width() >= rowWidth / 2);
        for (const QQuickItem* const item :
             {title, key, overridden, captures}) {
            const qreal right =
                item->mapToItem(Card(), QPointF(item->width(), 0)).x();
            QVERIFY2(
                right <= inner + 0.5,
                qPrintable(
                    QStringLiteral("%1 ends at %2, the card's inner edge is %3")
                        .arg(item->objectName())
                        .arg(right)
                        .arg(inner)));
        }
        // The marks are on the second line, under the title and the key.
        QVERIFY(overridden->mapToItem(delegate, QPointF(0, 0)).y() >=
                key->mapToItem(delegate, QPointF(0, key->height())).y() - 0.5);
        Snapshot("long-key");
    }

    // A window lower than the fixed parts of the card and a row: the card is
    // not shorter than them, the chosen row is in view, and the footer is
    // inside the card.
    void ASmallWindowKeepsTheChosenRowAndTheFooter() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        Window()->resize(Window()->width(), 300);
        QTRY_COMPARE_WITH_TIMEOUT(Window()->height(), 300, TIMEOUT_MS);
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        const auto* const list = PaletteItem("paletteList");
        QTRY_VERIFY_WITH_TIMEOUT(list->height() >= 47, TIMEOUT_MS);
        const int rows = Model()->rowCount();
        QVERIFY(rows > 3);
        for (int step = 1; step < rows; ++step) {
            Press(Qt::Key_Down);
        }
        QCOMPARE(Palette()->property("currentRow").toInt(), rows - 1);
        QTRY_VERIFY2_WITH_TIMEOUT(CurrentRowProblem().isEmpty(),
                                  qPrintable(CurrentRowProblem()), TIMEOUT_MS);
        QVERIFY(Card()->height() <= 300 - 2 * 16);
        QVERIFY(Card()->height() > 150);
        const QQuickItem* const footer =
            PaletteItem("paletteFooterBack")->parentItem();
        QVERIFY(footer->mapToItem(Card(), QPointF(0, footer->height())).y() <=
                Card()->height() + 0.5);
        QCOMPARE(Card()->height(), std::round(Card()->height()));
        QCOMPARE(Card()->y(), std::round(Card()->y()));
        Snapshot("small-window");
    }

    // Typing from many rows to one: the card grows and shrinks downward only,
    // so the field stays where it was in the window.
    void TheFieldDoesNotMoveWhileTyping() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        // The card has settled at the cap before the position is taken.
        QTRY_COMPARE_WITH_TIMEOUT(Palette()->property("height").toReal(),
                                  Window()->height() / 2.0, TIMEOUT_MS);
        const qreal fieldY = Field()->mapToScene(QPointF(0, 0)).y();
        const qreal cardY = Card()->mapToScene(QPointF(0, 0)).y();
        const qreal tall = Card()->height();
        Snapshot("before-typing");
        Type("pb.gamma");
        QCOMPARE(Model()->rowCount(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(Palette()->property("height").toReal(),
                                  ContentHeight(), TIMEOUT_MS);
        QVERIFY(Card()->height() < tall);
        QCOMPARE(Field()->mapToScene(QPointF(0, 0)).y(), fieldY);
        QCOMPARE(Card()->mapToScene(QPointF(0, 0)).y(), cardY);
        Snapshot("after-typing");
    }

    // No rows: the card is as high as the header, the field, the line "No
    // command matches." and the footer.
    void TheCardIsAsHighAsTheLineOfNoRows() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        Type("zzzz");
        QCOMPARE(Model()->rowCount(), 0);
        QVERIFY(PaletteItem("paletteEmpty")->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(Palette()->property("height").toReal(),
                                  ContentHeight(), TIMEOUT_MS);
        QVERIFY(Palette()->property("height").toReal() <
                Window()->height() / 2.0 - 50);
    }

    // Rows that do not fit in half of the window: the card is half of the
    // window, the list scrolls, and the chosen row after many Down presses is
    // in the visible part of the list.
    void TheCardStopsAtHalfOfTheWindowAndTheListScrolls() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        const qreal half = Window()->height() / 2.0;
        QTRY_COMPARE_WITH_TIMEOUT(Palette()->property("height").toReal(), half,
                                  TIMEOUT_MS);
        const auto* const list = PaletteItem("paletteList");
        QVERIFY(list->property("contentHeight").toReal() > list->height());
        QVERIFY(ContentHeight() > half);
        const int rows = Model()->rowCount();
        QVERIFY(rows > 3);
        for (int step = 1; step < rows; ++step) {
            Press(Qt::Key_Down);
        }
        QCOMPARE(Palette()->property("currentRow").toInt(), rows - 1);
        QVERIFY2(CurrentRowProblem().isEmpty(),
                 qPrintable(CurrentRowProblem()));
        QTRY_VERIFY_WITH_TIMEOUT(list->property("contentY").toReal() > 0,
                                 TIMEOUT_MS);
        QCOMPARE(Palette()->property("height").toReal(), half);
        Snapshot("many-rows");
    }

    // The cap follows the window: a smaller window, a smaller card, and the
    // chosen row is still in view.
    void TheCapFollowsAResizeOfTheWindow() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenByKey();
        if (QTest::currentTestFailed()) {
            return;
        }
        const qreal before = Window()->height() / 2.0;
        QTRY_COMPARE_WITH_TIMEOUT(Palette()->property("height").toReal(),
                                  before, TIMEOUT_MS);
        const int rows = Model()->rowCount();
        for (int step = 1; step < rows; ++step) {
            Press(Qt::Key_Down);
        }
        Window()->resize(Window()->width(), Window()->height() * 5 / 6);
        const qreal after = Window()->height() / 2.0;
        QVERIFY(after < before);
        QTRY_COMPARE_WITH_TIMEOUT(Palette()->property("height").toReal(), after,
                                  TIMEOUT_MS);
        QTRY_VERIFY2_WITH_TIMEOUT(CurrentRowProblem().isEmpty(),
                                  qPrintable(CurrentRowProblem()), TIMEOUT_MS);
    }

#ifndef Q_OS_MACOS
    // The toolbar button of Linux opens the palette too.
    void TheToolbarButtonOpensThePalette() {
        FailOnWarnings();
        BootWithPlugins();
        if (QTest::currentTestFailed()) {
            return;
        }
        auto* const button = Root()->findChild<QQuickItem*>("paletteButton");
        QVERIFY(button != nullptr);
        QVERIFY(button->isVisible());
        QTest::mouseClick(
            Window(), Qt::LeftButton, Qt::NoModifier,
            button
                ->mapToScene(QPointF(button->width() / 2, button->height() / 2))
                .toPoint());
        QTRY_VERIFY_WITH_TIMEOUT(IsOpen(), TIMEOUT_MS);
    }
#endif
};

QTEST_MAIN(PaletteTest)
#include "palette_test.moc"
