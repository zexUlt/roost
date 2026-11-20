#include "roost/settings_pages.hpp"

#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QWindow>

#include <algorithm>

#include "core/store.hpp"

namespace roost {

SettingsPages::SettingsPages(QObject& window, PluginLoader& loader)
    : Window_(window), Loader_(loader) {
    if (const auto directory = ConfigDirectory()) {
        Window_.setProperty(
            "keymapFile",
            QUrl::fromLocalFile(*directory + QStringLiteral("/keymap.json")));
    }
    connect(&Loader_, &PluginLoader::SettingsPagesChanged, this,
            &SettingsPages::Refresh);
    // Direct: after the signal the item is gone.
    connect(&Loader_, &PluginLoader::PanelAboutToBeDestroyed, this,
            &SettingsPages::OnPanelAboutToBeDestroyed, Qt::DirectConnection);
    // A page runs code of its plugin when it is made: not before the window
    // is shown for the first time.
    if (auto* qwindow = qobject_cast<QWindow*>(&Window_)) {
        Shown_ = qwindow->isVisible();
        connect(qwindow, &QWindow::visibleChanged, this, [this](bool visible) {
            if (visible && !Shown_) {
                Shown_ = true;
                Refresh();
            }
        });
    } else {
        Shown_ = true;
    }
    Refresh();
}

SettingsPages::~SettingsPages() {
    disconnect(&Loader_, nullptr, this, nullptr);
    QList<Page> pages = std::move(Pages_);
    Pages_.clear();
    Write();
    for (const Page& page : pages) {
        Loader_.DestroyPanel(page.Item.data());
    }
}

QStringList SettingsPages::PluginIds() const {
    QStringList ids;
    for (const Page& page : Pages_) {
        ids.append(page.PluginId);
    }
    return ids;
}

void SettingsPages::Refresh() {
    if (!Shown_) {
        return;
    }
    if (Refreshing_) {
        // CreateSettingsPage disabled a page and emitted the signal; the
        // outer loop goes again.
        RefreshAgain_ = true;
        return;
    }
    Refreshing_ = true;
    do {
        RefreshAgain_ = false;
        const QStringList wanted = Loader_.SettingsPlugins();
        // A page of a plugin that is gone or was loaded again is already
        // destroyed (OnPanelAboutToBeDestroyed); what is left is in order.
        QList<Page> pages;
        for (const QString& id : wanted) {
            const auto old = std::ranges::find_if(Pages_, [&](const Page& p) {
                return p.PluginId == id && p.Item;
            });
            if (old != Pages_.end()) {
                pages.append(*old);
                continue;
            }
            if (QQuickItem* item = Loader_.CreateSettingsPage(id)) {
                pages.append({id, item});
            }
        }
        Pages_ = std::move(pages);
        Write();
    } while (RefreshAgain_);
    Refreshing_ = false;
}

void SettingsPages::OnPanelAboutToBeDestroyed(QQuickItem* panel) {
    const auto found = std::ranges::find_if(
        Pages_, [&](const Page& page) { return page.Item == panel; });
    if (found == Pages_.end()) {
        return;
    }
    // Out of the window's list first: it lets go of the item while it lives.
    Pages_.erase(found);
    Write();
}

void SettingsPages::Write() {
    QVariantList list;
    for (const Page& page : Pages_) {
        if (!page.Item) {
            continue;
        }
        list.append(
            QVariantMap{{QStringLiteral("pluginId"), page.PluginId},
                        {QStringLiteral("page"),
                         QVariant::fromValue<QQuickItem*>(page.Item.data())}});
    }
    Window_.setProperty("settingsPages", list);
}

}  // namespace roost
