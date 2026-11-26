// A panel says how narrow it may get (its optional property `minimumWidth`,
// and `minimumHeight`), and the split keeps its slot at least that big, while
// the size in the layout stays as the user set it; a panel that says nothing
// goes down to 40. The sessions list says 200, the width its rows and head are
// laid out for. The real window against a real roostd, the real sessions
// panel.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExtensionPlugin>
#include <QSaveFile>
#include <QTest>
#include <optional>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"

using namespace roost;
using namespace roost::testing;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

constexpr double LIST_MINIMUM = 200;
constexpr double DEFAULT_MINIMUM = 40;

QString KeyOf(const QString& id) { return "claude/" + id; }

}  // namespace

class PanelMinimumTest : public LayoutWindow {
    Q_OBJECT

private:
    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

    // A category with a long name and a colour, pinned, with an unread
    // session: its header has every mark, and the session the dot.
    bool SeedMarks() {
        const QString category = QStringLiteral("A category with a long name");
        const QJsonObject document{
            {"version", 1},
            {"categories",
             QJsonObject{{KeyOf(GAMMA), category}, {KeyOf(BETA), category}}},
            {"colors", QJsonObject{{category, 2}}},
            {"pinned", QJsonArray{category}},
            {"unread", QJsonArray{KeyOf(GAMMA), KeyOf(ALPHA)}}};
        if (!QDir().mkpath(Dir_->filePath("config"))) {
            return false;
        }
        QSaveFile file(Dir_->filePath("config/sessions.json"));
        return file.open(QIODevice::WriteOnly) &&
               file.write(QJsonDocument(document).toJson()) >= 0 &&
               file.commit();
    }

    void BootList(int listWidth) {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Layout layout;
        layout.Root = Pair("list", "terminal", listWidth);
        Shell_->SetLayout(layout);
        WaitForLayout({{"list", "terminal"}});
    }

    static LayoutNode Leaf(const QString& instance, const QString& panel) {
        LayoutNode leaf;
        leaf.Instance = instance;
        leaf.Panel = panel;
        return leaf;
    }

    static LayoutNode Split(bool horizontal, std::vector<LayoutNode> children,
                            std::vector<LayoutSize> sizes) {
        LayoutNode split;
        split.Kind = ELayoutNodeKind::Split;
        split.Horizontal = horizontal;
        split.Children = std::move(children);
        split.Sizes = std::move(sizes);
        return split;
    }

    double Wide(const QString& instance) {
        return SlotByInstance(instance)->width();
    }

    // The point on the divider to the right of the slot.
    QPoint DividerRightOf(const QString& instance) {
        QQuickItem* slot = SlotByInstance(instance);
        return slot->mapToScene(QPointF(slot->width(), 60)).toPoint();
    }

    // A drag with the mouse from `from` by `dx`, in steps.
    void Drag(QPoint from, int dx) {
        QTest::mousePress(Window(), Qt::LeftButton, Qt::NoModifier, from);
        for (int step = 1; step <= 8; ++step) {
            QTest::mouseMove(Window(), from + QPoint(dx * step / 8, 0));
        }
        QTest::mouseRelease(Window(), Qt::LeftButton, Qt::NoModifier,
                            from + QPoint(dx, 0));
    }

    QJsonArray RootSizes() {
        return StoredLayout().value("root").toObject().value("sizes").toArray();
    }

    QQuickItem* RowAt(int index) {
        QQuickItem* row = nullptr;
        QMetaObject::invokeMethod(ListViewOf("list"), "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem*, row),
                                  Q_ARG(int, index));
        return row;
    }

    // The parts of a row of the list that show something, by the rectangle
    // they take in the row.
    QList<std::pair<QString, QRectF>> ShownParts(QQuickItem* row) {
        QList<std::pair<QString, QRectF>> parts;
        for (const char* name :
             {"agentIcon", "sessionTitle", "unreadDot", "headerTitle",
              "categoryMark", "pinMark", "headerUnread", "headerCount"}) {
            auto* part = row->findChild<QQuickItem*>(name);
            if (part == nullptr || !part->isVisible() || part->width() <= 0) {
                continue;
            }
            parts.append({QString::fromLatin1(name),
                          QRectF(part->mapToItem(row, QPointF(0, 0)),
                                 QSizeF(part->width(), part->height()))});
        }
        return parts;
    }

    // No part of any row of the list takes the place of another, and none
    // goes out of the row.
    void VerifyRowsDoNotOverlap() {
        int checked = 0;
        QSet<QString> seen;
        for (int index = 0; index < ModelOf("list")->rowCount(); ++index) {
            QQuickItem* row = RowAt(index);
            if (row == nullptr) {
                continue;
            }
            const auto parts = ShownParts(row);
            for (int a = 0; a < parts.size(); ++a) {
                seen.insert(parts[a].first);
                QVERIFY2(parts[a].second.left() >= 0 &&
                             parts[a].second.right() <= row->width(),
                         qPrintable(parts[a].first));
                for (int b = a + 1; b < parts.size(); ++b) {
                    QVERIFY2(!parts[a].second.intersects(parts[b].second),
                             qPrintable(QStringLiteral("row %1: %2 %3 meets %4 "
                                                       "%5")
                                            .arg(index)
                                            .arg(parts[a].first)
                                            .arg(parts[a].second.x())
                                            .arg(parts[b].first)
                                            .arg(parts[b].second.x())));
                }
            }
            ++checked;
        }
        QVERIFY(checked > 4);
        // The case shows every kind of part, or it checks nothing.
        for (const char* name :
             {"agentIcon", "sessionTitle", "unreadDot", "headerTitle",
              "categoryMark", "pinMark", "headerCount"}) {
            QVERIFY2(seen.contains(QString::fromLatin1(name)), name);
        }
    }

private slots:
    void init() {
        Prepare();
        QVERIFY(QDir().mkpath(Dir_->filePath("home")));
        Home_.emplace("HOME", Dir_->filePath("home").toUtf8());
        Socket_.emplace("ROOST_SOCKET", SocketPath().toUtf8());
        Config_.emplace("ROOST_CONFIG", Dir_->filePath("config").toUtf8());
    }

    void cleanup() {
        Teardown();
        Config_.reset();
        Socket_.reset();
        Home_.reset();
    }

    void TheListKeepsItsMinimumWhereTheLayoutAsksForLess() {
        BootList(100);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Wide("list"), LIST_MINIMUM, TIMEOUT_MS);
        // The layout keeps what it was given.
        QCOMPARE(PanelByInstance("list")->property("minimumWidth").toInt(),
                 200);
        QCOMPARE(Shell_->CurrentLayout().Root.Sizes.at(0).Pixels, 100);
    }

    void ADragOfTheDividerStopsAtTheMinimumOfTheList() {
        BootList(320);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Wide("list"), 320.0, TIMEOUT_MS);
        Drag(DividerRightOf("list"), -250);
        QTRY_COMPARE_WITH_TIMEOUT(Wide("list"), LIST_MINIMUM, TIMEOUT_MS);
        // What the drag reports is what is shown.
        QTRY_COMPARE_WITH_TIMEOUT(RootSizes().at(0).toInt(),
                                  static_cast<int>(LIST_MINIMUM), TIMEOUT_MS);
    }

    void NothingInTheRowsAndTheHeadOverlapsAtTheMinimum() {
        QVERIFY(SeedMarks());
        BootList(100);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Wide("list"), LIST_MINIMUM, TIMEOUT_MS);
        // The headers: the pinned category with its colour, its count; the
        // folded one also counts its unread.
        QTRY_VERIFY_WITH_TIMEOUT(
            ModelOf("list")->rowCount() > 4 && RowAt(5) != nullptr, TIMEOUT_MS);
        VerifyRowsDoNotOverlap();
        if (QTest::currentTestFailed()) {
            return;
        }
        QQuickItem* head =
            PanelByInstance("list")->findChild<QQuickItem*>("sessionsHead");
        QQuickItem* button = head->findChild<QQuickItem*>("sessionsNewButton");
        const QRectF box(button->mapToItem(head, QPointF(0, 0)),
                         QSizeF(button->width(), button->height()));
        QVERIFY(box.left() >= 0);
        QVERIFY(box.right() <= head->width());
        QQuickItem* title = head->findChild<QQuickItem*>("sessionsTitle");
        const QRectF titleBox(title->mapToItem(head, QPointF(0, 0)),
                              QSizeF(title->width(), title->height()));
        QVERIFY(titleBox.right() <= box.left());
    }

    void APanelWithoutAMinimumGoesDownTo40() {
        BootList(320);
        if (QTest::currentTestFailed()) {
            return;
        }
        Layout layout;
        layout.Root =
            Split(true, {Leaf("list", LIST), Leaf("terminal", TERMINAL)},
                  {LayoutSize::Rest(), LayoutSize::Px(10)});
        layout.Root.Children[1].Links.insert(
            "session", {.From = "list", .Output = "selected", .Problem = {}});
        Shell_->SetLayout(layout);
        WaitForLayout({{"list", "terminal"}}, false);
        QTRY_COMPARE_WITH_TIMEOUT(Wide("terminal"), DEFAULT_MINIMUM,
                                  TIMEOUT_MS);
        QVERIFY(
            PanelByInstance("terminal")->property("minimumWidth").isNull() ||
            !PanelByInstance("terminal")->property("minimumWidth").isValid());
    }

    void ASplitInsideASplitNeedsWhatItsChildrenNeed() {
        BootList(320);
        if (QTest::currentTestFailed()) {
            return;
        }
        // A split across the other way (a split in a split of the same
        // direction is merged into it): the list over a terminal, 100 px wide
        // in the layout. The column is as wide as its widest child needs.
        LayoutNode terminal = Leaf("terminal", TERMINAL);
        terminal.Links.insert(
            "session", {.From = "list", .Output = "selected", .Problem = {}});
        Layout layout;
        layout.Root = Split(true,
                            {Split(false, {Leaf("list", LIST), terminal},
                                   {LayoutSize::Rest(), LayoutSize::Rest()}),
                             Leaf("other", TERMINAL)},
                            {LayoutSize::Px(100), LayoutSize::Rest()});
        Shell_->SetLayout(layout);
        WaitForLayout({{"list", "terminal"}}, false);
        QTRY_COMPARE_WITH_TIMEOUT(Wide("list"), LIST_MINIMUM, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Wide("terminal"), LIST_MINIMUM, TIMEOUT_MS);
        const auto left = [&](const QString& instance) {
            return SlotByInstance(instance)->mapToScene(QPointF(0, 0)).x();
        };
        QTRY_VERIFY_WITH_TIMEOUT(
            SlotByInstance("other") != nullptr &&
                left("other") >= left("list") + Wide("list"),
            TIMEOUT_MS);
    }

    void AStackOfTabsNeedsTheLargestMinimumOfItsPages() {
        BootList(320);
        if (QTest::currentTestFailed()) {
            return;
        }
        LayoutNode tabs;
        tabs.Kind = ELayoutNodeKind::Tabs;
        tabs.Active = 1;
        tabs.Children = {Leaf("list", LIST), Leaf("shell", TERMINAL)};
        Layout layout;
        layout.Root = Split(true, {tabs, Leaf("other", TERMINAL)},
                            {LayoutSize::Px(100), LayoutSize::Rest()});
        Shell_->SetLayout(layout);
        // The terminal page is the one shown, and the stack is as wide as the
        // list page needs.
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("shell") != nullptr &&
                                     SlotByInstance("shell")->isVisible(),
                                 TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Wide("shell"), LIST_MINIMUM, TIMEOUT_MS);
    }

    void ATinyWindowStaysUsable() {
        BootList(320);
        if (QTest::currentTestFailed()) {
            return;
        }
        Window()->resize(150, Window()->height());
        QTRY_COMPARE_WITH_TIMEOUT(static_cast<int>(Window()->width()), 150,
                                  TIMEOUT_MS);
        QTest::qWait(200);
        QVERIFY(Wide("list") >= LIST_MINIMUM);
        QVERIFY(Wide("terminal") >= DEFAULT_MINIMUM);
        qInfo() << "tiny window: list" << Wide("list") << "terminal"
                << Wide("terminal") << "left of terminal"
                << SlotByInstance("terminal")->mapToScene(QPointF(0, 0)).x();
        // It grows back: the sizes are where they were.
        Window()->resize(900, Window()->height());
        QTRY_COMPARE_WITH_TIMEOUT(Wide("list"), 320.0, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Wide("terminal") > 500, TIMEOUT_MS);
    }
};

QTEST_MAIN(PanelMinimumTest)
#include "panel_minimum_test.moc"
