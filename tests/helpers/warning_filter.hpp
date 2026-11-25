#pragma once

#include <QRegularExpression>
#include <QString>

namespace roost::testing {

// The warnings that fail a window test: every one but two of Qt's own. QTest
// matches with `message.contains(regex)`, so the pattern is a negative
// lookahead at the start of the message.
// - The font database, which the first text of a process draws with and which
//   depends on the machine's fonts. Qt appends a tail, so it is a prefix.
// - "QUnifiedTimer::stopAnimationDriver: driver is not running", the whole
//   message (a longer one is another warning). Every ListView makes a pause
//   animation (QQuickItemViewPrivate::bufferPause) and Qt stops the animation
//   driver while only pauses run. Precondition: the timer of the QML
//   animations was emptied, which queued its stop, and `startAnimationPending`
//   is already true, so the queued start of a new pause posts nothing.
//   Then QQmlAnimationTimer::stopTimer restarts the driver through
//   resumeAnimationTimer/localRestart; the pending startAnimations sees only
//   a pause and stops the driver directly with driver->stop() (no warning);
//   then the QUnifiedTimer::stopTimer queued earlier calls
//   stopAnimationDriver on the stopped driver and warns. Nothing in roost can
//   order Qt's own events (the sequence is in fail_on_warnings_test).
inline QRegularExpression WarningFilter() {
    return QRegularExpression(
        QStringLiteral(R"(^(?!Populating font family aliases|)"
                       R"(QUnifiedTimer::stopAnimationDriver: )"
                       R"(driver is not running$))"));
}

}  // namespace roost::testing
