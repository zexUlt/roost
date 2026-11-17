#pragma once

#include <QJsonObject>
#include <QString>

#include <expected>

namespace roost {

// Where roostd keeps its state (design.md §8): $ROOST_CONFIG if set, else
// $XDG_CONFIG_HOME/roost if that is an absolute path, else ~/.config/roost.
// An error if it comes to HOME and HOME is unset, relative or "/". Tests
// always set ROOST_CONFIG, so they never touch the real directory.
std::expected<QString, QString> ConfigDirectory();

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
    // document: "keymap" is the user's keymap.json (design.md §8). Store
    // refuses them, and the manifest loader does not accept them as ids.
    static bool IsReservedPluginId(const QString& id);

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

}  // namespace roost
