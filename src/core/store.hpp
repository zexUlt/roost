#pragma once

#include <QJsonObject>
#include <QString>

#include <expected>

namespace roost {

// Where roostd keeps its state (design.md §8): $ROOST_CONFIG if set, else
// $XDG_CONFIG_HOME/roost if that is an absolute path, else ~/.config/roost.
// Tests always set ROOST_CONFIG, so they never touch the real directory.
QString ConfigDirectory();

// One JSON object per plugin, in <directory>/<plugin id>.json.
//
// A save goes to a temporary file in the same directory and is renamed over
// the old one, so a crash or a full disk leaves either the old document or the
// new one, never half of either. A file that does not parse is an error, not
// an empty document: saving over it would lose what the user had.
class Store {
public:
    explicit Store(QString directory);

    // Lowercase letters, digits, '.', '_' and '-', starting with a letter or a
    // digit, at most 64 characters. Lowercase only: on a case-insensitive file
    // system "Claude" and "claude" would be one file.
    static bool IsValidPluginId(const QString& id);

    const QString& Directory() const { return Directory_; }

    // The plugin's document; an empty object if it has none yet.
    std::expected<QJsonObject, QString> Load(const QString& pluginId) const;

    // Creates the directory, owner-only, if it is missing.
    std::expected<void, QString> Save(const QString& pluginId,
                                      const QJsonObject& document) const;

private:
    QString PathOf(const QString& pluginId) const;

    QString Directory_;
};

}  // namespace roost
