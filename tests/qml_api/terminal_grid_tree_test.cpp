// The grid of a TerminalItem stays in the scene graph from frame to frame. A
// frame must not take the grid out of the tree and put it back: that marks the
// whole subtree removed and added for the renderer, and the batch renderer
// registers every row again, a frame that changed nothing too. The check reads
// the notifications a scene graph sends to its renderer, which only the
// private Qt Quick headers expose.

#include "qml_api/terminal_item.hpp"
#include "terminal/terminal_session.hpp"

#include <private/qsgabstractrenderer_p.h>
#include <private/qsgadaptationlayer_p.h>
#include <QQuickWindow>
#include <QSGNode>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <memory>
#include <vector>

using roost::TerminalItem;
using roost::TerminalSession;

namespace {

constexpr quint16 COLUMNS = 20;
constexpr quint16 ROWS = 6;
constexpr int FRAME_TIMEOUT_MS = 10000;

std::shared_ptr<TerminalSession> NewSession(quint16 rows = ROWS) {
    auto session = TerminalSession::Create(COLUMNS, rows);
    if (!session) {
        qFatal("%s", qPrintable(session.error()));
    }
    return std::shared_ptr<TerminalSession>(std::move(*session));
}

void Write(TerminalSession& session, QByteArrayView bytes) {
    const auto lock = session.Lock();
    session.Write(lock, bytes);
}

// A TerminalItem that can be painted by hand.
class PaintableTerminalItem : public TerminalItem {
public:
    using TerminalItem::TerminalItem;

    // One call of updatePaintNode on the calling thread, outside a frame.
    QSGNode* Paint(QSGNode* oldNode) {
        return TerminalItem::updatePaintNode(oldNode, nullptr);
    }
};

// What a scene graph tells its renderer about the nodes under a root: every
// change of the tree (a node added or removed) and the node it concerns.
class ChangeLog : public QSGAbstractRenderer {
public:
    struct Change {
        const QSGNode* Node;
        QSGNode::DirtyState State;
    };

    std::vector<Change> Changes;

    // The root keeps the renderers it was given and asks them to let go when
    // it is destroyed, so this one must not outlive it unregistered.
    ~ChangeLog() override { setRootNode(nullptr); }

    void renderScene() override {}

protected:
    void nodeChanged(QSGNode* node, QSGNode::DirtyState state) override {
        Changes.push_back({node, state});
    }
};

constexpr QSGNode::DirtyState TREE_CHANGED =
    QSGNode::DirtyNodeAdded | QSGNode::DirtyNodeRemoved;

// How many of the notifications are about a node added or removed (the others
// are a changed colour or geometry).
std::size_t TreeChanges(const ChangeLog& log) {
    return static_cast<std::size_t>(
        std::ranges::count_if(log.Changes, [](const ChangeLog::Change& change) {
            return (change.State & TREE_CHANGED) != 0;
        }));
}

}  // namespace

class TerminalGridTreeTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        Item_ = new PaintableTerminalItem(Window_.contentItem());
        Item_->setSize(QSizeF(COLUMNS * 20, ROWS * 40));
        Window_.resize(COLUMNS * 20, ROWS * 40);
        QSignalSpy swapped(&Window_, &QQuickWindow::frameSwapped);
        Window_.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Window_));
        QVERIFY(swapped.count() > 0 || swapped.wait(FRAME_TIMEOUT_MS));
    }

    void cleanup() {
        Window_.hide();
        delete Item_;
        Item_ = nullptr;
    }

    // The grid stays in the tree from frame to frame. The item paints under a
    // root of its own that lets the changes reach ChangeLog: a frame with
    // nothing new, and one with text, neither takes the grid out nor adds it.
    void TheGridStaysInTheTreeFromFrameToFrame() {
        QSGRootNode root;
        ChangeLog log;
        log.setRootNode(&root);
        auto* container = new QSGNode;
        root.appendChildNode(container);
        auto session = NewSession();
        Item_->setSession(session);

        QSGNode* node = Item_->Paint(nullptr);
        QVERIFY(node != nullptr);
        container->appendChildNode(node);
        QCOMPARE(node->childCount(), 2);
        QSGNode* background = node->firstChild();
        QSGNode* grid = node->lastChild();
        QVERIFY(grid != background);
        QCOMPARE(grid->childCount(), ROWS + 1);

        // Nothing changed in the session: nothing in the tree changes.
        log.Changes.clear();
        QCOMPARE(Item_->Paint(node), node);
        QCOMPARE(node->firstChild(), background);
        QCOMPARE(node->lastChild(), grid);
        QCOMPARE(grid->parent(), node);
        QCOMPARE(node->childCount(), 2);
        QCOMPARE(TreeChanges(log), std::size_t{0});

        // Text in a row: that row is built again, the grid is not touched.
        Write(*session, "hello");
        log.Changes.clear();
        QCOMPARE(Item_->Paint(node), node);
        QCOMPARE(Item_->rowsRebuilt(), 1);
        QCOMPARE(node->firstChild(), background);
        QCOMPARE(node->lastChild(), grid);
        QCOMPARE(grid->parent(), node);
        for (const auto& change : log.Changes) {
            QVERIFY2(change.Node != grid, "the grid was taken out or added");
        }
        // The nodes of the row are what was replaced, under the grid.
        QVERIFY(TreeChanges(log) > 0);

        // A new size of the session: the renderer builds the grid anew in the
        // same root, and the item gives the tree what it still has.
        {
            const auto lock = session->Lock();
            QVERIFY(session->Resize(lock, COLUMNS, ROWS + 2));
        }
        QCOMPARE(Item_->Paint(node), node);
        QCOMPARE(node->lastChild(), grid);
        QCOMPARE(node->childCount(), 2);
        QCOMPARE(grid->childCount(), ROWS + 3);
    }

private:
    QQuickWindow Window_;
    PaintableTerminalItem* Item_ = nullptr;
};

QTEST_MAIN(TerminalGridTreeTest)
#include "terminal_grid_tree_test.moc"
