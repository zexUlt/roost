#pragma once

#include <QString>
#include <QtGlobal>

#include <expected>
#include <optional>

namespace roost {

// The platform a chord applies on. Passed as an argument rather than read
// from the build, so that one test binary covers both (design.md §16).
enum class EPlatform { Mac, Linux };

// The platform this build runs on.
constexpr EPlatform HostPlatform() {
#ifdef Q_OS_MACOS
    return EPlatform::Mac;
#else
    return EPlatform::Linux;
#endif
}

// A key with its modifiers, in the parsed form that keys are compared in
// (design.md §16). Modifiers name physical keys: Ctrl is the Control key and
// Cmd the Command key, whatever Qt calls them on the platform.
struct KeyChord {
    bool Cmd = false;
    bool Ctrl = false;
    bool Alt = false;
    bool Shift = false;
    // Normal form: one letter is upper case, a name has its canonical
    // spelling ("PageUp", "F5"), any other single character is as written.
    QString Key;

    bool operator==(const KeyChord&) const = default;
};

// "Cmd+Shift+N", "Ctrl+\", "Alt+Left", "F5", "J": modifiers joined by '+',
// then one key. Case and the order of modifiers do not matter. `platform` is
// where the chord applies: Cmd is refused for Linux, where the desktop owns
// Super and the application often never sees the press. The error is a
// message for the user.
std::expected<KeyChord, QString> ParseKeyChord(const QString& text,
                                               EPlatform platform);

// Whether a key is F1 to F24 in the normal form of KeyChord::Key.
bool IsFunctionKey(const QString& key);

// A key that the terminal or a field of text needs (design.md §16): no Cmd,
// Ctrl or Alt, and not F1 to F24. Shift alone does not count: Shift+J types a
// letter.
bool IsBareKey(const KeyChord& chord);

// The normal text: modifiers in the order Cmd, Ctrl, Alt, Shift, then the
// key. ParseKeyChord(WriteKeyChord(chord)) is `chord`.
QString WriteKeyChord(const KeyChord& chord);

// The chord a keybinding has on `platform`, from its `key` (all platforms)
// and `mac` (replaces `key` on macOS). Both are checked wherever the code
// runs, so a file shared by a Mac and a Linux machine reads the same on both.
// `key` is checked as a Linux chord, because it applies there. Null where the
// keybinding does not apply: only `mac` on Linux. The error names the field
// that is wrong.
std::expected<std::optional<KeyChord>, QString> ChooseKeyChord(
    const std::optional<QString>& key, const std::optional<QString>& mac,
    EPlatform platform);

}  // namespace roost
