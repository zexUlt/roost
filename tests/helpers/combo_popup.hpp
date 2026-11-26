#pragma once

#include <QMetaObject>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStringList>
#include <QTest>
#include <QVariant>

namespace roost::testing {

// What a user sees when a combo box is opened with the mouse: the texts of the
// rows of its list, in order, `count` of them. Empty when the list did not
// open or did not make the rows in `timeoutMs`. The list is left open. The
// click is in the middle of the box, or on its arrow with `onArrow`: a box
// with a text to edit opens its list from the arrow only.
inline QStringList OpenedRowTexts(QQuickWindow* window, QQuickItem* box,
                                  int count, int timeoutMs,
                                  bool onArrow = false) {
    const qreal x = onArrow ? box->width() - 15 : box->width() / 2;
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      box->mapToScene(QPointF(x, box->height() / 2)).toPoint());
    auto* popup = box->property("popup").value<QObject*>();
    if (popup == nullptr ||
        !QTest::qWaitFor([&] { return popup->property("opened").toBool(); },
                         timeoutMs)) {
        return {};
    }
    QStringList texts;
    const auto collect = [&] {
        texts.clear();
        auto* list = popup->property("contentItem").value<QQuickItem*>();
        if (list == nullptr) {
            return false;
        }
        for (int i = 0; i < count; ++i) {
            QQuickItem* row = nullptr;
            QMetaObject::invokeMethod(list, "itemAtIndex",
                                      Q_RETURN_ARG(QQuickItem*, row),
                                      Q_ARG(int, i));
            if (row == nullptr) {
                return false;
            }
            texts.append(row->property("text").toString());
        }
        return true;
    };
    return QTest::qWaitFor(collect, timeoutMs) ? texts : QStringList{};
}

// Clicks the row `index` of the opened list of `box` with the mouse, as a user
// does. False when the list or the row is not there.
inline bool ClickOpenedRow(QQuickWindow* window, QQuickItem* box, int index) {
    auto* popup = box->property("popup").value<QObject*>();
    auto* list = popup == nullptr
                     ? nullptr
                     : popup->property("contentItem").value<QQuickItem*>();
    QQuickItem* row = nullptr;
    if (list == nullptr ||
        !QMetaObject::invokeMethod(list, "itemAtIndex",
                                   Q_RETURN_ARG(QQuickItem*, row),
                                   Q_ARG(int, index)) ||
        row == nullptr) {
        return false;
    }
    QTest::mouseClick(
        window, Qt::LeftButton, Qt::NoModifier,
        row->mapToScene(QPointF(row->width() / 2, row->height() / 2))
            .toPoint());
    return true;
}

}  // namespace roost::testing
