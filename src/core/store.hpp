#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <expected>

namespace roost {

// Where roostd keeps its state (design.md §8): $ROOST_CONFIG if set, else
// $XDG_CONFIG_HOME/roost if that is an absolute path, else ~/.config/roost.
// An error if it comes to HOME and HOME is unset, relative or "/". Tests
// always set ROOST_CONFIG, so they never touch the real directory.
std::expected<QString, QString> ConfigDirectory();

// Writes `bytes` to `path`, a file of `directory`, for the owner only: the
// directory is created, 0700, if it is missing (one that exists is left as
// it is), the file is 0600, put in place by a rename of a temporary file, so
// a failure leaves the old file. Store::Save and the keymap.json writes of
// roostd (design.md §8) both go through it.
std::expected<void, QString> SaveOwnerOnly(const QString& directory,
                                           const QString& path,
                                           const QByteArray& bytes);

// One JSON object per plugin, in <directory>/<plugin id>.json.
//
// A save goes to a temporary file in the same directory and is renamed over
// the old one, so a crash or a full disk leaves either the old document or the
// new one, never half of either. Files are owner-only, 0600.
//
// Load reports a file that does not parse as an error rather than an empty
// document, so the caller can tell the user before a Save replaces it.
class Store {
public:
    // The directory must be an absolute path; Load and Save fail otherwise.
    explicit Store(QString directory);

    // Lowercase letters, digits, '.', '_' and '-', starting with a letter or a
    // digit, at most 64 characters. Lowercase only: on a case-insensitive file
    // system "Claude" and "claude" would be one file.
    static bool IsValidPluginId(const QString& id);

    // Ids that name a file of the state directory other than a plugin's
    // document: "keymap" is the user's keymap.json, "layout", "settings" and
    // "updates" are the documents of the window (design.md §8). The manifest
    // loader does not accept them as plugin ids. Store refuses "keymap", but
    // not the window documents (IsWindowDocumentId).
    static bool IsReservedPluginId(const QString& id);

    // "layout", "settings" and "updates": documents that belong to no plugin,
    // which Store reads and writes like any other.
    static bool IsWindowDocumentId(const QString& id);

    const QString& Directory() const { return Directory_; }

    // The plugin's document; an empty object if it has none yet.
    std::expected<QJsonObject, QString> Load(const QString& pluginId) const;

    // Creates the directory, owner-only, if it is missing.
    std::expected<void, QString> Save(const QString& pluginId,
                                      const QJsonObject& document) const;

private:
    std::expected<void, QString> CheckRequest(const QString& pluginId) const;
    QString PathOf(const QString& pluginId) const;

    QString Directory_;
};

// RFC 7386 JSON merge patch: for each member of the patch, null removes the
// member of the target, an object is merged into the member of the target (a
// member that is not an object counts as empty), anything else, arrays
// included, replaces it. Nulls inside an object that is merged into nothing
// are dropped, as the RFC has it.
void ApplyMergePatch(QJsonObject& target, const QJsonObject& patch);

// The patch that turns `before` into `after`: ApplyMergePatch(before, patch)
// gives `after`. A merge patch cannot say "set to null", so this holds for
// documents without null values; a null in `after` is left out of the patch.
QJsonObject MergeDiff(const QJsonObject& before, const QJsonObject& after);

}  // namespace roost
