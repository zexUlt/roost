#include "roost/shell.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QVariant>
#include <QtLogging>

#include <algorithm>
#include <utility>

#include "core/protocol.hpp"
#include "core/version.hpp"

namespace roost {

namespace {

// The name of the one entry of `declared` that has the type, or empty with the
// reason in `problem` if there is none or several.
QString SingleOfType(const QVariantMap& declared, const QString& type,
                     const QString& panel, const char* kind, QString& problem) {
    QStringList names;
    for (auto it = declared.begin(); it != declared.end(); ++it) {
        if (it.value().toString() == type) {
            names.append(it.key());
        }
    }
    if (names.size() == 1) {
        return names.first();
    }
    problem = QStringLiteral(
                  "panel %1 declares %2 %3 of type %4, the link "
                  "needs exactly one; the link is off")
                  .arg(panel)
                  .arg(names.isEmpty() ? QStringLiteral("no")
                                       : QString::number(names.size()))
                  .arg(QLatin1String(kind), type);
    return {};
}

}  // namespace

Shell::Shell(QObject& root, PluginLoader& loader, api::Api& api,
             QString socketPath, DaemonLauncher* launcher)
    : Root_(root),
      Loader_(loader),
      Api_(api),
      SocketPath_(std::move(socketPath)),
      Launcher_(launcher) {
    for (auto* item : root.findChildren<QQuickItem*>("panelSlot")) {
        Slots_.append({item, item->property("panelType").toString(), {}, {}});
    }
    if (Slots_.isEmpty()) {
        qWarning("roost: the window has no panel slot");
    }

    connect(&Loader_, &PluginLoader::PanelTypesChanged, this,
            &Shell::OnPanelTypesChanged);
    connect(&Loader_, &PluginLoader::ProblemsChanged, this,
            &Shell::UpdateProblems);
    // Direct: after the signal the item is gone.
    connect(&Loader_, &PluginLoader::PanelAboutToBeDestroyed, this,
            &Shell::OnPanelAboutToBeDestroyed, Qt::DirectConnection);
    connect(&Api_.Plugins(), &api::PluginService::Changed, this,
            &Shell::OnPanelTypesChanged);
    connect(&Api_, &api::Api::Connected, this,
            [this] { SetConnection(QStringLiteral("connected"), {}); });
    connect(&Api_, &api::Api::Disconnected, this, [this](const QString& why) {
        // A refusal is followed by a Disconnected; the refusal is what the
        // window shows until Retry or a connection.
        if (Connection_ != QLatin1String("refused")) {
            SetConnection(QStringLiteral("disconnected"), why);
        }
    });
    connect(&Api_, &api::Api::Refused, this, &Shell::OnRefused);
    if (Launcher_ != nullptr) {
        // Only the text of the banner follows the launcher.
        const auto refresh = [this] {
            SetConnection(Connection_, ConnectionMessage_);
        };
        connect(Launcher_, &DaemonLauncher::StartingChanged, this, refresh);
        connect(Launcher_, &DaemonLauncher::FailureChanged, this, refresh);
        connect(Launcher_, &DaemonLauncher::RestartingChanged, this, refresh);
        // The new roostd is coming up: connect as Retry does.
        connect(Launcher_, &DaemonLauncher::Restarted, this, &Shell::OnRetry);
    }
    connect(&Root_, SIGNAL(retryRequested()), this, SLOT(OnRetry()));
    connect(&Root_, SIGNAL(restartRequested()), this,
            SLOT(OnRestartRequested()));

    SetConnection(Api_.IsConnected() ? QStringLiteral("connected")
                                     : QStringLiteral("connecting"),
                  {});
    Refill();
    UpdateProblems();
}

Shell::~Shell() {
    disconnect(&Loader_, nullptr, this, nullptr);
    for (Slot& slot : Slots_) {
        QQuickItem* panel = slot.Panel.data();
        if (panel == nullptr) {
            continue;
        }
        Write(slot, "panel", QVariant::fromValue<QQuickItem*>(nullptr));
        slot.Panel.clear();
        Loader_.DestroyPanel(panel);
    }
}

Shell::Readiness Shell::CheckReadiness() const {
    QStringList failures;
    for (const auto& problem : Loader_.Problems()) {
        failures.append(
            problem.Panel.isEmpty()
                ? QStringLiteral("plugin %1: %2")
                      .arg(problem.Plugin, problem.Message)
                : QStringLiteral("plugin %1, panel %2: %3")
                      .arg(problem.Plugin, problem.Panel, problem.Message));
    }
    for (const Slot& slot : Slots_) {
        if (!slot.LinkProblem.isEmpty()) {
            failures.append(slot.LinkProblem);
        }
    }
    if (Connection_ == QLatin1String("refused")) {
        failures.append(QStringLiteral("roostd refused the connection"));
    }
    // What roostd found wrong with the plugins (a manifest it could not read).
    const auto listed = Api_.Plugins().Plugins().value("problems").toArray();
    for (const auto& problem : listed) {
        failures.append(QStringLiteral("roostd: %1").arg(problem.toString()));
    }
    // A missing slot cannot appear by waiting.
    for (const char* type : {LIST_PANEL, TERMINAL_PANEL}) {
        const bool present =
            std::ranges::any_of(Slots_, [type](const Slot& slot) {
                return slot.Type == QLatin1String(type);
            });
        if (!present) {
            failures.append(
                QStringLiteral("the window has no panel %1").arg(type));
        }
    }
    if (!failures.isEmpty()) {
        return {false, true, failures.join(QStringLiteral("; "))};
    }

    QStringList waiting;
    if (!Api_.IsConnected()) {
        waiting.append(
            QStringLiteral("not connected to roostd at %1").arg(SocketPath_));
    }
    for (const Slot& slot : Slots_) {
        if (slot.Panel.isNull()) {
            waiting.append(QStringLiteral("panel %1 is not created (%2)")
                               .arg(slot.Type, ErrorFor(slot.Type)));
        }
    }
    const auto& sessions = Api_.Sessions();
    if (!sessions.IsLoaded()) {
        waiting.append(QStringLiteral("sessions are not loaded"));
    } else if (sessions.Rows().isEmpty()) {
        waiting.append(QStringLiteral("roostd lists no sessions"));
    }
    if (!waiting.isEmpty()) {
        return {false, false, waiting.join(QStringLiteral("; "))};
    }
    return {true, false, {}};
}

void Shell::OnPanelTypesChanged() {
    Refill();
    UpdateProblems();
}

void Shell::OnPanelAboutToBeDestroyed(QQuickItem* panel) {
    for (Slot& slot : Slots_) {
        if (slot.Panel != panel) {
            continue;
        }
        // The slot first: its Binding lets go of the item while it lives.
        // `panel` must be written null before `linkInput` and `linkOutput`
        // are cleared: an empty `property` on a live target makes the Binding
        // warn. The Binding does not restore an old property when only its
        // name changes; Shell never does that, the names change only with the
        // panel.
        Write(slot, "panel", QVariant::fromValue<QQuickItem*>(nullptr));
        slot.Panel.clear();
        slot.LinkProblem.clear();
        Write(slot, "linkInput", QString());
        Write(slot, "linkOutput", QString());
        Write(slot, "error", ErrorFor(slot.Type));
    }
    UpdateProblems();
}

void Shell::Refill() {
    if (Filling_) {
        // CreatePanel emitted PanelTypesChanged; the outer loop goes again.
        FillAgain_ = true;
        return;
    }
    Filling_ = true;
    do {
        FillAgain_ = false;
        for (Slot& slot : Slots_) {
            if (slot.Item && slot.Panel.isNull()) {
                Fill(slot);
            }
        }
    } while (FillAgain_);
    Filling_ = false;
}

void Shell::Fill(Slot& slot) {
    const auto info = Loader_.Info(slot.Type);
    QQuickItem* panel = info ? Loader_.CreatePanel(slot.Type) : nullptr;
    if (panel == nullptr) {
        Write(slot, "error", ErrorFor(slot.Type));
        return;
    }
    slot.Panel = panel;
    SetLink(slot, *info);
    Write(slot, "error", QString());
    // Last: the slot reparents the panel, and its link becomes active.
    Write(slot, "panel", QVariant::fromValue<QQuickItem*>(panel));
}

QString Shell::ErrorFor(const QString& type) const {
    if (Connection_ == QLatin1String("refused")) {
        return QStringLiteral("roostd refused the connection");
    }
    if (!Api_.Plugins().IsLoaded()) {
        return QStringLiteral("waiting for roostd");
    }
    for (const auto& problem : Loader_.Problems()) {
        if (problem.Panel == type) {
            return problem.Message;
        }
    }
    return QStringLiteral("no panel %1").arg(type);
}

void Shell::SetLink(Slot& slot, const PluginLoader::PanelInfo& info) {
    slot.LinkProblem.clear();
    QString input;
    QString output;
    const bool isList = slot.Type == QLatin1String(LIST_PANEL);
    if (isList) {
        output = SingleOfType(info.Outputs, QLatin1String(SESSION_REF),
                              slot.Type, "output", slot.LinkProblem);
    } else if (slot.Type == QLatin1String(TERMINAL_PANEL)) {
        input = SingleOfType(info.Inputs, QLatin1String(SESSION_REF), slot.Type,
                             "input", slot.LinkProblem);
    }
    // A declared name without a property is the loader's problem; the link
    // would only write to nothing.
    const QMetaObject* meta = slot.Panel->metaObject();
    const QString name = isList ? output : input;
    if (!name.isEmpty() && meta->indexOfProperty(qPrintable(name)) < 0) {
        output.clear();
        input.clear();
    }
    Write(slot, "linkInput", input);
    Write(slot, "linkOutput", output);
}

void Shell::OnRefused(const rpc::Error& error) {
    const QJsonObject data = error.Data.toObject();
    QString message = error.Message;
    if (data.contains("protocol") || data.contains("ghosttyVt")) {
        message += QStringLiteral(
                       " (roostd: protocol %1, libghostty-vt %2; this window: "
                       "protocol %3, libghostty-vt %4)")
                       // One arg() call: a "%N" inside a string from roostd is
                       // not substituted by a later one.
                       .arg(data.value("protocol").toVariant().toString(),
                            data.value("ghosttyVt").toString(),
                            QString::number(protocol::VERSION),
                            QString::fromStdString(GhosttyVtVersion()));
    }
    Mismatch_ =
        error.Code == static_cast<int>(protocol::EErrorCode::VersionMismatch);
    if (Mismatch_ && Launcher_ != nullptr) {
        message += QStringLiteral(
            "\nRestarting closes the warm terminals; Claude sessions keep "
            "running.");
    } else {
        message += QStringLiteral(". Restart roostd, then press Retry.");
    }
    SetConnection(QStringLiteral("refused"), message);
}

void Shell::OnRetry() {
    SetConnection(QStringLiteral("connecting"), {});
    Api_.Connect(SocketPath_);
}

void Shell::OnRestartRequested() {
    // The button is there only for this state; a stray signal does nothing.
    if (Launcher_ == nullptr || Connection_ != QLatin1String("refused") ||
        !Mismatch_) {
        return;
    }
    Launcher_->Restart(SocketPath_, Api_.RefusedPeerPid());
}

bool Shell::IsWaiting() const {
    return Connection_ == QLatin1String("connecting") ||
           Connection_ == QLatin1String("disconnected");
}

void Shell::SetConnection(const QString& state, const QString& message) {
    Connection_ = state;
    ConnectionMessage_ = message;
    QString shown = state;
    QString shownMessage = message;
    bool offerRestart = false;
    if (Launcher_ != nullptr && state == QLatin1String("refused")) {
        if (Launcher_->Restarting()) {
            shown = QStringLiteral("restarting");
        } else {
            offerRestart = Mismatch_;
            if (!Launcher_->Failure().isEmpty()) {
                shownMessage =
                    Launcher_->Failure() + QLatin1Char('\n') + message;
            }
        }
    } else if (Launcher_ != nullptr && IsWaiting()) {
        if (!Launcher_->Failure().isEmpty()) {
            shown = QStringLiteral("startFailed");
            shownMessage = Launcher_->Failure();
        } else if (Launcher_->Starting()) {
            shown = QStringLiteral("starting");
        }
    }
    Root_.setProperty("connection", shown);
    Root_.setProperty("connectionMessage", shownMessage);
    Root_.setProperty("restartOffered", offerRestart);
    // The placeholders of the empty slots tell the state of the connection.
    for (const Slot& slot : Slots_) {
        if (slot.Item && slot.Panel.isNull()) {
            Write(slot, "error", ErrorFor(slot.Type));
        }
    }
}

void Shell::UpdateProblems() {
    QStringList problems;
    const auto listed = Api_.Plugins().Plugins().value("problems").toArray();
    for (const auto& problem : listed) {
        problems.append(problem.toString());
    }
    for (const auto& problem : Loader_.Problems()) {
        problems.append(
            problem.Panel.isEmpty()
                ? QStringLiteral("plugin %1: %2")
                      .arg(problem.Plugin, problem.Message)
                : QStringLiteral("plugin %1, panel %2: %3")
                      .arg(problem.Plugin, problem.Panel, problem.Message));
    }
    for (const Slot& slot : Slots_) {
        if (!slot.LinkProblem.isEmpty()) {
            problems.append(slot.LinkProblem);
        }
    }
    if (Root_.property("problems").toStringList() != problems) {
        Root_.setProperty("problems", problems);
    }
}

void Shell::Write(const Slot& slot, const char* name, const QVariant& value) {
    if (slot.Item) {
        slot.Item->setProperty(name, value);
    }
}

}  // namespace roost
