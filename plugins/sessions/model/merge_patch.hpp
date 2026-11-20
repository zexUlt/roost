#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QStringList>

namespace roost::sessions {

// The JSON merge patch (RFC 7386) of the store (`store.patch`, design.md §8),
// as the list needs it. The same rules as ApplyMergePatch and MergeDiff of
// src/core/store.hpp, kept here so that the plugin does not link roost core;
// tests hold the two together.

// A key path into a document: {"view", "list", "opened"}.
using KeyPath = QStringList;

// null removes the member, an object is merged into the member, anything else,
// arrays included, replaces it.
void ApplyPatch(QJsonObject& target, const QJsonObject& patch);

// The patch that turns `before` into `after`, for documents without nulls.
QJsonObject DiffOf(const QJsonObject& before, const QJsonObject& after);

// The paths a patch touches: each member that is not a non-empty object (an
// empty object is a member of its own, it creates one).
QSet<KeyPath> PatchPaths(const QJsonObject& patch);

// The value at the path; undefined if there is none.
QJsonValue ValueAt(const QJsonObject& document, const KeyPath& path);

// Puts the value at the path, making the objects on the way; an undefined
// value removes the member. An empty object does not replace an object that is
// there: a patch that creates one changes nothing of it.
void SetAt(QJsonObject& document, const KeyPath& path, const QJsonValue& value);

}  // namespace roost::sessions
