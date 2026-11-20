// What the tests of the layout view share: pairs of a list and a terminal in
// a layout, found in the window by their instance id, and the window on a
// SessionsWindow whose daemon and config may outlive the window.

#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QString>

#include "helpers/sessions_window.hpp"
#include "roost/layout_model.hpp"

namespace roost::testing {

// A list and a terminal side by side, the terminal linked to the list.
inline LayoutNode Pair(const QString& list, const QString& terminal,
                       int listWidth = 320) {
    LayoutNode listLeaf;
    listLeaf.Instance = list;
    listLeaf.Panel = LIST;
    LayoutNode terminalLeaf;
    terminalLeaf.Instance = terminal;
    terminalLeaf.Panel = TERMINAL;
    terminalLeaf.Links.insert(
        "session", {.From = list, .Output = "selected", .Problem = {}});
    LayoutNode split;
    split.Kind = ELayoutNodeKind::Split;
    split.Horizontal = true;
    split.Children = {std::move(listLeaf), std::move(terminalLeaf)};
    split.Sizes = {LayoutSize::Px(listWidth), LayoutSize::Rest()};
    return split;
}

// The pairs one under the other, equal in height.
inline Layout Stacked(std::vector<LayoutNode> pairs) {
    Layout layout;
    layout.Root.Kind = ELayoutNodeKind::Split;
    layout.Root.Horizontal = false;
    for (LayoutNode& pair : pairs) {
        layout.Root.Sizes.push_back(LayoutSize::Rest());
        layout.Root.Children.push_back(std::move(pair));
    }
    return layout;
}

// The panels of a window by the instance id of their slots.
class LayoutWindow : public SessionsWindow {
public:
    QQuickItem* SlotByInstance(const QString& instance) {
        for (auto* item : Root()->findChildren<QQuickItem*>("panelSlot")) {
            if (item->property("instanceId").toString() == instance) {
                return item;
            }
        }
        return nullptr;
    }

    QQuickItem* PanelByInstance(const QString& instance) {
        QQuickItem* slot = SlotByInstance(instance);
        return slot == nullptr ? nullptr
                               : slot->property("panel").value<QQuickItem*>();
    }

    int SlotCount() {
        return static_cast<int>(
            Root()->findChildren<QQuickItem*>("panelSlot").size());
    }

    QQuickItem* ListViewOf(const QString& list) {
        QQuickItem* panel = PanelByInstance(list);
        return panel == nullptr ? nullptr
                                : panel->findChild<QQuickItem*>("sessionList");
    }

    QAbstractItemModel* ModelOf(const QString& list) {
        QQuickItem* panel = PanelByInstance(list);
        return panel == nullptr
                   ? nullptr
                   : panel->findChild<QAbstractItemModel*>("sessionsModel");
    }

    TerminalItem* TerminalOf(const QString& terminal) {
        QQuickItem* panel = PanelByInstance(terminal);
        return panel == nullptr ? nullptr
                                : panel->findChild<TerminalItem*>("terminal");
    }

    // What the session of the terminal shows now; empty without one.
    QString ScreenOf(const QString& terminal) {
        TerminalItem* item = TerminalOf(terminal);
        const auto* pane =
            item == nullptr ? nullptr : item->findChild<SessionPane*>();
        return pane == nullptr ? QString{} : Screen(pane->Session());
    }

    QString TerminalText(const QString& terminal) {
        QQuickItem* panel = PanelByInstance(terminal);
        QQuickItem* text =
            panel == nullptr ? nullptr
                             : panel->findChild<QQuickItem*>("terminalStatus");
        return text == nullptr ? QString() : text->property("text").toString();
    }

    QString SelectedOf(const QString& list) {
        QQuickItem* panel = PanelByInstance(list);
        return panel == nullptr
                   ? QString()
                   : panel->property("selected").toMap().value("id").toString();
    }

    // The id of the session with the number in the list's order.
    QString IdIn(const QString& list, int session) {
        QAbstractItemModel* model = ModelOf(list);
        const int row = SessionRowsOf(list).value(session, -1);
        return model == nullptr || row < 0
                   ? QString()
                   : model
                         ->data(model->index(row, 0),
                                model->roleNames().key("id"))
                         .toString();
    }

    // The list takes the keyboard, the cursor goes to the session with the
    // number in the list's order and Return opens it; waits until the linked
    // terminal has it open. The order of a list may change as the sessions
    // are used, so a test that needs the same session in two lists, or the
    // one it has named before, uses OpenIdIn.
    void OpenIn(const QString& list, const QString& terminal, int session) {
        OpenIdIn(list, terminal, IdIn(list, session));
    }

    // As OpenIn, for the session with the id, wherever it is in the list now.
    void OpenIdIn(const QString& list, const QString& terminal,
                  const QString& id) {
        QVERIFY(!id.isEmpty());
        QQuickItem* view = ListViewOf(list);
        QVERIFY(view != nullptr);
        view->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(view->hasActiveFocus(), TIMEOUT_MS);
        const auto cursor = [view] {
            return view->property("currentIndex").toInt();
        };
        const int row = RowOfId(list, id);
        QVERIFY(row >= 0);
        // The cursor goes by the model, not by key presses that may overtake
        // the view's own update of its current index.
        bool moved = false;
        QVERIFY(QMetaObject::invokeMethod(ModelOf(list), "setCursorRow",
                                          Q_RETURN_ARG(bool, moved),
                                          Q_ARG(int, row)));
        QVERIFY(moved);
        QTRY_COMPARE_WITH_TIMEOUT(cursor(), row, TIMEOUT_MS);
        Press(Qt::Key_Return);
        // The terminal may be open with the session shown before.
        QTRY_COMPARE_WITH_TIMEOUT(
            TerminalOf(terminal)->property("session").toMap().value("id"),
            QVariant(id), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(TerminalOf(terminal)->status(),
                                  QString("open"), TIMEOUT_MS);
        QCOMPARE(SelectedOf(list), id);
    }

    // The row of the session with the id; -1 if the list has none.
    int RowOfId(const QString& list, const QString& id) {
        QAbstractItemModel* model = ModelOf(list);
        if (model == nullptr) {
            return -1;
        }
        const int idRole = model->roleNames().key("id");
        for (const int row : SessionRowsOf(list)) {
            if (model->data(model->index(row, 0), idRole).toString() == id) {
                return row;
            }
        }
        return -1;
    }

    QList<int> SessionRowsOf(const QString& list) {
        QList<int> rows;
        QAbstractItemModel* model = ModelOf(list);
        if (model == nullptr) {
            return rows;
        }
        const int kind = model->roleNames().key("kind");
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->data(model->index(row, 0), kind).toString() ==
                "session") {
                rows.append(row);
            }
        }
        return rows;
    }

    // What the list stored for its instance: view.<list>.opened.
    QString OpenedStored(const QString& list) {
        return StoredDocument()
            .value("view")
            .toObject()
            .value(list)
            .toObject()
            .value("opened")
            .toString();
    }

    // The layout document as the daemon keeps it.
    QJsonObject StoredLayout() {
        return QJsonDocument::fromJson(
                   ReadFile(Dir_->filePath("config/layout.json")))
            .object();
    }

    // The window and the pairs of `layout` are up; `settled` waits for the
    // geometry of the terminals too, which a hidden one never gets.
    void WaitForLayout(const std::vector<std::pair<QString, QString>>& pairs,
                       bool settled = true) {
        for (const auto& [list, terminal] : pairs) {
            QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance(list) != nullptr &&
                                         PanelByInstance(terminal) != nullptr,
                                     TIMEOUT_MS);
            QTRY_COMPARE_WITH_TIMEOUT(
                static_cast<int>(SessionRowsOf(list).size()),
                static_cast<int>(ORDER.size()), TIMEOUT_MS);
            if (!settled) {
                continue;
            }
            // The geometry has settled: the greeting of the fake TUI is drawn
            // once at the size the session opens with, and wraps in a
            // narrower terminal.
            QTRY_VERIFY_WITH_TIMEOUT(TerminalOf(terminal) != nullptr &&
                                         TerminalOf(terminal)->columns() >= 60,
                                     TIMEOUT_MS);
        }
    }
};

}  // namespace roost::testing
