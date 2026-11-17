#include "helpers/channels_left.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFuture>
#include <QObject>
#include <QTest>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace roost::testing {

int ChannelsLeft(IoThread& io, const QString& agent, const QString& id,
                 int timeoutMs) {
    std::vector<std::uint32_t> closed;
    QObject scope;
    QObject::connect(
        &io, &IoThread::Closed, &scope,
        [&closed](std::uint32_t channel) { closed.push_back(channel); });
    QFuture<OpenResult> open = io.Open(agent, id, 80, 24);
    if (!QTest::qWaitFor([&] { return open.isFinished(); }, timeoutMs) ||
        !open.result()) {
        return -1;
    }
    const std::uint32_t direct = open.result()->Channel;
    // fake_tui reacts to a key only if it arrives in a read of its own, so the
    // key is sent again until the session ends.
    QElapsedTimer sinceKey;
    const auto ended = QTest::qWaitFor(
        [&] {
            if (!sinceKey.isValid() || sinceKey.hasExpired(100)) {
                io.SendInput(direct, "x");
                sinceKey.start();
            }
            return std::ranges::find(closed, direct) != closed.end();
        },
        timeoutMs);
    if (!ended) {
        return -1;
    }
    QFuture<rpc::Result> round = io.Request(QStringLiteral("plugins.list"));
    if (!QTest::qWaitFor([&] { return round.isFinished(); }, timeoutMs)) {
        return -1;
    }
    // The Closed signals queued before the answer finished.
    QCoreApplication::processEvents();
    return static_cast<int>(closed.size()) - 1;
}

}  // namespace roost::testing
