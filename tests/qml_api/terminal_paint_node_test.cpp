// What a TerminalItem leaves in the scene graph. updatePaintNode that returns
// null without deleting the node it was given makes Qt forget the node but not
// remove it: it stays in the tree, drawn, and the next node is prepended
// under it. A session switch through "no session" must leave one root.

#include "qml_api/terminal_item.hpp"
#include "terminal/terminal_session.hpp"

#include <QQuickWindow>
#include <QSGNode>
#include <QSignalSpy>
#include <QTest>

#include <atomic>
#include <memory>

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

// A TerminalItem that learns its child container node: the parent of the paint
// node it was given back. The container lives as long as the item, and public
// QSGNode API is enough to count what is under it.
class ProbedTerminalItem : public TerminalItem {
public:
    using TerminalItem::TerminalItem;

    // Null until the item has been given its own paint node back once.
    QSGNode* Container() const { return Container_.load(); }

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode,
                             UpdatePaintNodeData* data) override {
        if (oldNode != nullptr && oldNode->parent() != nullptr) {
            Container_ = oldNode->parent();
        }
        return TerminalItem::updatePaintNode(oldNode, data);
    }

private:
    std::atomic<QSGNode*> Container_ = nullptr;
};

}  // namespace

class TerminalPaintNodeTest : public QObject {
    Q_OBJECT

private:
    // One frame; the nodes under the item are counted while the GUI thread is
    // blocked in the sync, so the count is safe with a render thread too.
    bool DrawFrame() {
        QSignalSpy synced(&Window_, &QQuickWindow::afterSynchronizing);
        Item_->update();
        return synced.wait(FRAME_TIMEOUT_MS);
    }

    // The nodes the item has put in the scene graph: its paint node, and
    // whatever an earlier one left behind. -1: the container is not known yet.
    int NodesOfItem() const { return Nodes_.load(); }

private slots:
    void init() {
        Item_ = new ProbedTerminalItem(Window_.contentItem());
        Item_->setSize(QSizeF(COLUMNS * 20, ROWS * 40));
        Window_.resize(COLUMNS * 20, ROWS * 40);
        connect(
            &Window_, &QQuickWindow::afterSynchronizing, this,
            [this] {
                const QSGNode* container = Item_->Container();
                Nodes_ = container != nullptr ? container->childCount() : -1;
            },
            Qt::DirectConnection);
        Item_->setSession(NewSession());
        QSignalSpy swapped(&Window_, &QQuickWindow::frameSwapped);
        Window_.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Window_));
        QVERIFY(swapped.count() > 0 || swapped.wait(FRAME_TIMEOUT_MS));
    }

    void cleanup() {
        Window_.hide();
        delete Item_;
        Item_ = nullptr;
        Window_.disconnect(this);
    }

    void SwitchThroughNoSessionLeavesOneRoot() {
        QVERIFY(DrawFrame());
        QCOMPARE(NodesOfItem(), 1);

        // What a cold session does: the item has no session for a frame.
        Item_->setSession(nullptr);
        QVERIFY(DrawFrame());
        QCOMPARE(NodesOfItem(), 0);

        auto other = NewSession();
        Write(*other, "\x1b[41m\x1b[2J\x1b[0m");
        Item_->setSession(other);
        QVERIFY(DrawFrame());
        QCOMPARE(NodesOfItem(), 1);
    }

    void NoSessionTwiceThenBackStillOneRoot() {
        QVERIFY(DrawFrame());
        Item_->setSession(nullptr);
        QVERIFY(DrawFrame());
        QVERIFY(DrawFrame());
        Item_->setSession(NewSession());
        QVERIFY(DrawFrame());
        QCOMPARE(NodesOfItem(), 1);
    }

private:
    QQuickWindow Window_;
    ProbedTerminalItem* Item_ = nullptr;
    std::atomic<int> Nodes_ = 0;
};

QTEST_MAIN(TerminalPaintNodeTest)
#include "terminal_paint_node_test.moc"
