#pragma once

#include <QEvent>
#include <QKeyEvent>
#include <QObject>
#include <QQuickItem>
#include <QSet>
#include <QString>

#include <optional>

#include "api/command_service.hpp"
#include "api/keymap_service.hpp"
#include "core/key_chord.hpp"

namespace roost {

// Matches the key presses of the window against the keymap table and runs the
// command of the entry (design.md §16). An application event filter, so a
// pressed key is looked up before the terminal or a field of text sees it:
// the key that is caught is taken from them. GUI thread only.
//
// A press is looked up as the pair (key, `when`) with `when` the type of the
// panel that has the focus, then as (key, no `when`). The panel is found from
// the item with the active focus: the nearest ancestor PanelSlot (the item
// with the objectName "panelSlot", as the shell finds them) names its
// `panelType`. Focus outside any slot, a dialog for one, has no `when`.
//
// Not matched at all:
//   - while an input method composes (a QInputMethodEvent with a preedit
//     string was the last one seen, no one with an empty string since);
//   - while the focus is in a Popup or a Dialog (the keys are the dialog's);
//   - before the table is loaded;
//   - a key whose command the built-in `shell` plugin declares (no backend)
//     when the window has no handler for it: the key falls through to the
//     item (the terminal copies on its own) instead of ending in -32003;
//   - a key without Cmd, Ctrl or Alt other than F1 to F24 while the focus is
//     in the terminal or in an item that takes text (it has the flag
//     ItemAcceptsInputMethod): such a key is text there;
//   - the repeat of a key that was caught, which is eaten without running the
//     command again, and the release of a caught key, which is eaten so that
//     the terminal does not see a release without a press.
//
// The command goes through CommandService::Run, as a click in the palette
// will; a refusal is a warning in the log.
class KeyMatcher : public QObject {
    Q_OBJECT

public:
    // Installs the filter on the application; both services must outlive it.
    KeyMatcher(api::KeymapService& keymap, api::CommandService& commands,
               QObject* parent = nullptr);
    ~KeyMatcher() override;

    KeyMatcher(const KeyMatcher&) = delete;
    KeyMatcher& operator=(const KeyMatcher&) = delete;

    // The chord of a key press in api::MatchForm, as the table has it: the
    // key by BindingKey (a letter by the layout if it is Latin, else by the
    // position on the US layout), the modifiers named by the physical keys
    // (Ctrl is the Control key; Qt's Control is Command on macOS). Nothing for
    // a modifier alone, a key without a name in the table, or, on Linux, a key
    // with Super held: the desktop owns it.
    static std::optional<KeyChord> ChordOf(const QKeyEvent& event);

    // The type of the panel that has the focus: the `panelType` of the nearest
    // PanelSlot above `focus`; empty with no slot above it or no focus.
    static QString PanelTypeOf(const QQuickItem* focus);

    // Whether `focus` is inside the window's overlay: in a Popup or a Dialog,
    // whose items live there.
    static bool InOverlay(const QQuickItem* focus);

    // Whether the focus takes text: the terminal, or an item that has an
    // input method (a TextInput, a TextEdit).
    static bool TakesText(const QQuickItem* focus);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // `binding` unless it is an entry of the plugin `shell` whose command has
    // no handler in the window; null for null.
    const api::KeyBinding* Usable(const api::KeyBinding* binding) const;
    bool OnPress(QObject* watched, const QKeyEvent& event);

    api::KeymapService& Keymap_;
    api::CommandService& Commands_;
    // An input method has a preedit string on now; reset when the focus object
    // changes.
    bool Composing_ = false;
    // The Qt keys of the caught presses that have not been released yet, in
    // the focus span of the window that caught them: cleared when the focus
    // window changes.
    QSet<int> Held_;
};

}  // namespace roost
