#pragma once

#include <QByteArrayView>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <expected>
#include <vector>

// What `claude agents --json --all` prints, read into the sessions the
// adapter tells roostd about (design.md §7).
namespace roost::claude {

struct Session {
    QString Id;
    QString Title;
    QString WorkingDirectory;
    // working, blocked, idle, done or failed.
    QString State;
    // Milliseconds since the epoch; 0 when unknown.
    qint64 StartedAt = 0;

    bool operator==(const Session&) const = default;
};

// {id, state, title, cwd, startedAt?}, as agent.listSessions answers.
QJsonObject ToJson(const Session& session);

struct Listing {
    // Background sessions in the order Claude Code lists them.
    std::vector<Session> Sessions;
    // Entries skipped and states guessed, worth a line in the log.
    QStringList Warnings;
};

// Fails only if the output is not a JSON array.
//
// An entry is kept if its kind is "background" and it has a string id; a
// repeated id keeps the first entry. The title is the name, or the id if
// there is none. The state comes from "state": needs_input is blocked, done
// while "status" is busy is working, and an unknown one is idle.
std::expected<Listing, QString> ParseAgents(QByteArrayView output);

}  // namespace roost::claude
