#pragma once

#include <QDir>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSharedPointer>
#include <QString>
#include <QTest>

namespace roost::testing {

// With ROOST_TEST_SNAPSHOTS set to a directory, saves a picture of the window
// there as <test function>-<step>.png, for a person to look at; without it,
// does nothing. False when the picture could not be saved. The content item,
// not the window: grabWindow distorts the terminal in the offscreen platform.
inline bool SaveSnapshot(QQuickWindow* window, const QString& step) {
    const QString directory = qEnvironmentVariable("ROOST_TEST_SNAPSHOTS");
    if (directory.isEmpty()) {
        return true;
    }
    if (!QDir().mkpath(directory)) {
        return false;
    }
    const QString name =
        QStringLiteral("%1-%2.png")
            .arg(QString::fromLatin1(QTest::currentTestFunction()), step);
    QSharedPointer<QQuickItemGrabResult> result =
        window->contentItem()->grabToImage();
    if (result.isNull()) {
        return false;
    }
    bool ready = false;
    QObject::connect(result.data(), &QQuickItemGrabResult::ready,
                     [&ready] { ready = true; });
    constexpr int grabTimeoutMs = 10000;
    return QTest::qWaitFor([&ready] { return ready; }, grabTimeoutMs) &&
           result->saveToFile(QDir(directory).filePath(name));
}

}  // namespace roost::testing
