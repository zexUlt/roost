#pragma once

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QPointer>
#include <QQmlParserStatus>
#include <QString>
#include <QVariantMap>

namespace roost {

class RoostSingleton;

// What the keys section of the settings window (SettingsWindow.qml) needs of
// the key code that QML has not got, a type of the module Roost.Shell: the
// chord of a key press as the table writes it, two spellings of one key made
// equal, and who holds a key now. The window's own platform throughout: its
// copy of the table and its Cmd, Ctrl and Meta. GUI thread only.
//
// The names of the methods are QML's (camelCase), as in src/qml_api.
// NOLINTBEGIN(readability-identifier-naming)
class KeyChords : public QObject, public QQmlParserStatus {
    Q_OBJECT
    QML_ELEMENT
    Q_INTERFACES(QQmlParserStatus)

public:
    explicit KeyChords(QObject* parent = nullptr);

    void classBegin() override {}
    void componentComplete() override;

    // The chord of a QML KeyEvent in the text that the table and the file use
    // ("Cmd+Shift+N", design.md §16), as KeyMatcher::ChordOf names it: the
    // key by the layout and the position, Qt's Control and Meta swapped back
    // on macOS. Empty for a modifier alone, a key the table has no name for,
    // and, on Linux, a key with Super held. Call it from a key handler.
    Q_INVOKABLE QString chordOf(QObject* event) const;

    // The form in which two keys are the same key: the chord of `text` (as
    // the window's platform reads it) in the form of the table's index, in
    // the normal text. Empty when `text` does not parse. `Ctrl+Shift+N` and
    // `shift+ctrl+n` are one; so are `Ctrl++` and `Ctrl+Shift+=`.
    Q_INVOKABLE QString sameKey(const QString& text) const;

    // The binding of the table that holds `chordText` in the panel type
    // `when` (empty: for any panel): {command, args, source, key}, or an
    // empty map when no one holds it, the table is not loaded or the text
    // does not parse. `key` is the chord as the table writes it.
    Q_INVOKABLE QVariantMap holder(const QString& chordText,
                                   const QString& when) const;

private:
    QPointer<RoostSingleton> Singleton_;
};
// NOLINTEND(readability-identifier-naming)

}  // namespace roost
