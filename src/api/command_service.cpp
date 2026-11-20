#include "api/command_service.hpp"

#include <QJsonObject>

#include <algorithm>

#include "api/thread_check.hpp"

namespace roost::api {

CommandService::CommandService(IoThread& io, QObject* parent)
    : QObject(parent), Io_(io) {}

bool CommandService::Register(const QString& command, const QObject* owner,
                              LocalHandler handler, FocusProbe focused) {
    if (!CheckThread(*this, "CommandService::Register") || command.isEmpty() ||
        !handler) {
        return false;
    }
    Locals_[command].append({.Handler = std::move(handler),
                             .Owner = owner,
                             .Focused = std::move(focused)});
    if (owner != nullptr) {
        // The owner is gone before its handlers could run again; only its own
        // entries go, the others of the id stay.
        connect(owner, &QObject::destroyed, this, [this, command, owner] {
            const auto it = Locals_.find(command);
            if (it == Locals_.end()) {
                return;
            }
            it->removeIf(
                [owner](const Local& local) { return local.Owner == owner; });
            if (it->isEmpty()) {
                Locals_.erase(it);
            }
        });
    }
    return true;
}

void CommandService::NoteFocus(const QObject* owner) {
    if (owner == nullptr) {
        return;
    }
    for (auto it = Locals_.cbegin(); it != Locals_.cend(); ++it) {
        if (std::ranges::any_of(*it, [owner](const Local& local) {
                return local.Owner == owner;
            })) {
            LastFocused_.insert(it.key(), owner);
        }
    }
}

bool CommandService::HasLocal(const QString& command) const {
    return Locals_.contains(command);
}

QFuture<rpc::Result> CommandService::Run(const QString& command,
                                         const QJsonValue& args) {
    if (!CheckThread(*this, "CommandService::Run")) {
        return rpc::Ready(std::unexpected(WrongThread()));
    }
    if (const auto it = Locals_.constFind(command); it != Locals_.cend()) {
        // From the most recent: the first that has no focus to tell of (a
        // registration without a probe is the stack's own) or has the focus;
        // else the one of the owner that had the focus last, if it is still
        // here; else the most recent. A copy: the handler may take its own
        // registration away.
        auto chosen =
            std::find_if(it->rbegin(), it->rend(), [](const Local& local) {
                return !local.Focused || local.Focused();
            });
        if (chosen == it->rend()) {
            const QObject* const last = LastFocused_.value(command).data();
            chosen = std::find_if(
                it->rbegin(), it->rend(), [last](const Local& local) {
                    return last != nullptr && local.Owner == last;
                });
        }
        const LocalHandler handler =
            chosen != it->rend() ? chosen->Handler : it->last().Handler;
        return rpc::Ready(handler(args));
    }
    return Io_.Request(QStringLiteral("commands.run"),
                       QJsonObject{{QStringLiteral("command"), command},
                                   {QStringLiteral("args"), args}});
}

}  // namespace roost::api
