#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QString>

#include "roost/plugin_loader.hpp"

namespace roost {

// The controller of the settings window (SettingsWindow.qml, design.md §5): it
// asks the loader for the settings page of each plugin that has one, hands
// the pages to the window and takes them back when a plugin goes. Plain C++,
// made from main() next to Shell; GUI thread only.
//
// The window takes (written here): `settingsPages`, a list of
// {pluginId, page} in load order, `page` being the live item (the window shows
// it, the loader owns it); and `keymapFile`, the path of the user's
// keymap.json, which the window offers to open and never writes. The pages
// are made when the window is shown for the first time (a page runs code of
// its plugin, so a window that is never opened makes none) and stay while
// their plugin is loaded; a plugin that is loaded again gets a new page.
//
// It must be destroyed before the loader and the engine: its destructor
// gives the pages back to the loader.
class SettingsPages : public QObject {
    Q_OBJECT

public:
    // `window` is the settings window (the QML object), used and not owned.
    SettingsPages(QObject& window, PluginLoader& loader);
    ~SettingsPages() override;

    // The plugins whose page the window has now, in the order it shows them.
    QStringList PluginIds() const;

private:
    struct Page {
        QString PluginId;
        QPointer<QQuickItem> Item;
    };

    void Refresh();
    void OnPanelAboutToBeDestroyed(QQuickItem* panel);
    void Write();

    QObject& Window_;
    PluginLoader& Loader_;
    QList<Page> Pages_;
    bool Shown_ = false;
    bool Refreshing_ = false;
    bool RefreshAgain_ = false;
};

}  // namespace roost
