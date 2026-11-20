#pragma once

#include <QtQml/qqmlregistration.h>
#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QModelIndex>
#include <QQmlParserStatus>
#include <QString>
#include <QVariant>

#include <vector>

#include "api/api.hpp"
#include "core/key_chord.hpp"

namespace roost {

// The rows of the command palette (CommandPalette.qml), a type of the module
// Roost.Shell: the window's own, not part of the plugin API. One row per
// command that the manifests in plugins.list declare (the commands of the
// window are the ones of the built-in plugin `shell`), in the order the
// plugins were loaded and then the manifest's order, with what the window's
// copy of the keymap table says about its key (design.md §16, "Палитра").
//
// Roles: commandId, title, plugin (the id of the plugin that declares the
// command), key (the keys of the table that run it, joined by ", "), when
// (the panel types of those keys, "" for any panel), conflict (a sentence that
// names the plugins that bind one key, "" if none), overridden (keymap.json
// took a key of the command or gave it another), capturesInput (a key of the
// command is one that the terminal needs: Ctrl or, on macOS, Alt with a
// letter, active in the terminal).
//
// The filter keeps the rows whose title or id has the typed letters in order,
// ignoring case; the rows stay in the order above, the filter ranks nothing.
// The rows follow plugins.list and the keymap table: an empty list while
// there is no API. GUI thread only.
//
// The names of the methods are QML's (camelCase), as in src/qml_api.
// NOLINTBEGIN(readability-identifier-naming)
class PaletteModel : public QAbstractListModel, public QQmlParserStatus {
    Q_OBJECT
    QML_ELEMENT
    Q_INTERFACES(QQmlParserStatus)
    // What the user typed; "" shows every command.
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    // How many commands there are, whatever the filter.
    Q_PROPERTY(int total READ total NOTIFY totalChanged)

public:
    enum ERole {
        CommandIdRole = Qt::UserRole + 1,
        TitleRole,
        PluginRole,
        KeyRole,
        WhenRole,
        ConflictRole,
        OverriddenRole,
        CapturesInputRole,
    };

    // One command of the palette.
    struct Entry {
        QString CommandId;
        QString Title;
        QString Plugin;
        QString Key;
        QString When;
        QString Conflict;
        bool Overridden = false;
        bool CapturesInput = false;
    };

    explicit PaletteModel(QObject* parent = nullptr);

    // The rows for the plugins.list answer and the keymap table, as the
    // window of `platform` has them. Not a member so that it has no state.
    static std::vector<Entry> BuildEntries(const QJsonObject& plugins,
                                           const QJsonObject& keymap,
                                           EPlatform platform);

    // Whether the letters of `pattern` are in `text` in that order, with
    // anything between them, ignoring case. An empty pattern matches.
    static bool Matches(const QString& pattern, const QString& text);

    // Follows the API of the window; found by itself in a QML engine that has
    // the Roost singleton. A second call changes the API.
    void SetApi(api::Api* api);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void classBegin() override {}
    void componentComplete() override;

    QString filter() const { return Filter_; }
    void setFilter(const QString& filter);
    int total() const { return static_cast<int>(Entries_.size()); }

    // The id of the command of the shown row, "" if there is none.
    Q_INVOKABLE QString commandAt(int row) const;

signals:
    void filterChanged();
    void totalChanged();

private:
    void Rebuild();
    // The indexes of the entries that the filter keeps, in their order.
    std::vector<int> Shown() const;

    api::Api* Api_ = nullptr;
    QString Filter_;
    std::vector<Entry> Entries_;
    std::vector<int> Shown_;
    QList<QMetaObject::Connection> Connections_;
};
// NOLINTEND(readability-identifier-naming)

}  // namespace roost
