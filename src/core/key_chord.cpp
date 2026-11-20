#include "core/key_chord.hpp"

#include <QList>
#include <QStringList>

#include <array>

namespace roost {

namespace {

using Chord = std::expected<KeyChord, QString>;

Chord Fail(const QString& message) { return std::unexpected(message); }

struct KeyName {
    QLatin1StringView Lower;
    QLatin1StringView Canonical;
};

// Aliases come last; Write prints the canonical spelling.
constexpr std::array KEY_NAMES = {
    KeyName{QLatin1StringView("left"), QLatin1StringView("Left")},
    KeyName{QLatin1StringView("right"), QLatin1StringView("Right")},
    KeyName{QLatin1StringView("up"), QLatin1StringView("Up")},
    KeyName{QLatin1StringView("down"), QLatin1StringView("Down")},
    KeyName{QLatin1StringView("home"), QLatin1StringView("Home")},
    KeyName{QLatin1StringView("end"), QLatin1StringView("End")},
    KeyName{QLatin1StringView("pageup"), QLatin1StringView("PageUp")},
    KeyName{QLatin1StringView("pagedown"), QLatin1StringView("PageDown")},
    KeyName{QLatin1StringView("insert"), QLatin1StringView("Insert")},
    KeyName{QLatin1StringView("delete"), QLatin1StringView("Delete")},
    KeyName{QLatin1StringView("backspace"), QLatin1StringView("Backspace")},
    KeyName{QLatin1StringView("enter"), QLatin1StringView("Enter")},
    KeyName{QLatin1StringView("tab"), QLatin1StringView("Tab")},
    KeyName{QLatin1StringView("space"), QLatin1StringView("Space")},
    KeyName{QLatin1StringView("escape"), QLatin1StringView("Escape")},
    KeyName{QLatin1StringView("return"), QLatin1StringView("Enter")},
    KeyName{QLatin1StringView("esc"), QLatin1StringView("Escape")},
};

constexpr int LAST_FUNCTION_KEY = 24;

// "F5" for "f5", and null for anything that is not F1 to F24.
QString FunctionKey(const QString& lower) {
    if (lower.size() < 2 || lower.size() > 3 || lower.front() != u'f') {
        return {};
    }
    bool isNumber = false;
    const int number = lower.mid(1).toInt(&isNumber);
    // toInt takes "+5" and "05"; the key is written plainly.
    if (!isNumber || number < 1 || number > LAST_FUNCTION_KEY ||
        lower.mid(1) != QString::number(number)) {
        return {};
    }
    return QStringLiteral("F%1").arg(number);
}

// The normal form of the key part, or an error.
std::expected<QString, QString> NormalizeKey(const QString& part,
                                             const QString& text) {
    if (part.size() == 1 ||
        (part.size() == 2 && part.front().isHighSurrogate())) {
        const QChar first = part.front();
        if (part.size() == 1 && !first.isPrint()) {
            return std::unexpected(
                QStringLiteral("the key in \"%1\" is not a printable "
                               "character; write a name such as Space")
                    .arg(text));
        }
        const QString upper = part.toUpper();
        // "ß" is "SS" in upper case: the normal form would not parse back.
        if (upper.size() != part.size()) {
            return std::unexpected(
                QStringLiteral("\"%1\" in \"%2\" has no one-character upper "
                               "case; write the letter of the US key")
                    .arg(part, text));
        }
        // A letter of another script never fires: the binding matches the US
        // key of a non-Latin layout. The upper case is checked, as "µ" turns
        // into the Greek "Μ".
        const char32_t code = upper.toUcs4().front();
        if (QChar::isLetter(code)) {
            switch (QChar::script(code)) {
                case QChar::Script_Latin:
                case QChar::Script_Common:
                case QChar::Script_Inherited:
                case QChar::Script_Unknown:
                    break;
                default:
                    return std::unexpected(
                        QStringLiteral("\"%1\" in \"%2\" is not a Latin "
                                       "letter: write the letter of the US "
                                       "key")
                            .arg(part, text));
            }
        }
        return upper;
    }
    const QString lower = part.toLower();
    for (const KeyName& name : KEY_NAMES) {
        if (lower == name.Lower) {
            return QString(name.Canonical);
        }
    }
    if (const QString function = FunctionKey(lower); !function.isEmpty()) {
        return function;
    }
    return std::unexpected(
        QStringLiteral("unknown key name \"%1\" in \"%2\"").arg(part, text));
}

}  // namespace

std::expected<KeyChord, QString> ParseKeyChord(const QString& text,
                                               EPlatform platform) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return Fail(QStringLiteral("the key is empty"));
    }
    // '+' is both the separator and a key: "Ctrl++" and a bare "+" are the
    // key.
    QString keyPart;
    QStringList modifierParts;
    if (trimmed == u"+") {
        keyPart = trimmed;
    } else if (trimmed.endsWith(u"++")) {
        keyPart = QStringLiteral("+");
        const QString rest = trimmed.chopped(2);
        if (rest.isEmpty()) {
            return Fail(QStringLiteral("empty part in \"%1\"").arg(text));
        }
        modifierParts = rest.split(u'+');
    } else {
        modifierParts = trimmed.split(u'+');
        keyPart = modifierParts.takeLast().trimmed();
    }

    KeyChord chord;
    for (const QString& raw : std::as_const(modifierParts)) {
        const QString part = raw.trimmed();
        if (part.isEmpty()) {
            return Fail(QStringLiteral("empty part in \"%1\"").arg(text));
        }
        const QString lower = part.toLower();
        bool* flag = lower == u"ctrl"    ? &chord.Ctrl
                     : lower == u"alt"   ? &chord.Alt
                     : lower == u"shift" ? &chord.Shift
                     : lower == u"cmd"   ? &chord.Cmd
                                         : nullptr;
        if (flag == nullptr) {
            return Fail(QStringLiteral("unknown modifier \"%1\" in \"%2\"; "
                                       "use Ctrl, Alt, Shift or Cmd")
                            .arg(part, text));
        }
        if (*flag) {
            return Fail(QStringLiteral("modifier \"%1\" is repeated in \"%2\"")
                            .arg(part, text));
        }
        *flag = true;
    }
    if (chord.Cmd && platform == EPlatform::Linux) {
        return Fail(QStringLiteral("Cmd works on macOS only, so \"%1\" "
                                   "cannot apply on Linux; write it in "
                                   "\"mac\"")
                        .arg(text));
    }
    if (keyPart.isEmpty()) {
        return Fail(QStringLiteral("empty part in \"%1\"").arg(text));
    }
    const QString lowerKey = keyPart.toLower();
    if (lowerKey == u"ctrl" || lowerKey == u"alt" || lowerKey == u"shift" ||
        lowerKey == u"cmd") {
        return Fail(
            QStringLiteral("\"%1\" has no key after the modifiers").arg(text));
    }
    const auto key = NormalizeKey(keyPart, text);
    if (!key) {
        return Fail(key.error());
    }
    chord.Key = *key;
    return chord;
}

bool IsFunctionKey(const QString& key) {
    // The same rule as the parser: F1 to F24 written plainly.
    return !key.isEmpty() && FunctionKey(key.toLower()) == key;
}

bool IsBareKey(const KeyChord& chord) {
    return !chord.Cmd && !chord.Ctrl && !chord.Alt && !IsFunctionKey(chord.Key);
}

QString WriteKeyChord(const KeyChord& chord) {
    QString text;
    if (chord.Cmd) {
        text += QStringLiteral("Cmd+");
    }
    if (chord.Ctrl) {
        text += QStringLiteral("Ctrl+");
    }
    if (chord.Alt) {
        text += QStringLiteral("Alt+");
    }
    if (chord.Shift) {
        text += QStringLiteral("Shift+");
    }
    return text + chord.Key;
}

std::expected<std::optional<KeyChord>, QString> ChooseKeyChord(
    const std::optional<QString>& key, const std::optional<QString>& mac,
    EPlatform platform) {
    std::optional<KeyChord> parsedKey;
    if (key) {
        const Chord parsed = ParseKeyChord(*key, EPlatform::Linux);
        if (!parsed) {
            return std::unexpected(
                QStringLiteral("\"key\": %1").arg(parsed.error()));
        }
        parsedKey = *parsed;
    }
    std::optional<KeyChord> parsedMac;
    if (mac) {
        const Chord parsed = ParseKeyChord(*mac, EPlatform::Mac);
        if (!parsed) {
            return std::unexpected(
                QStringLiteral("\"mac\": %1").arg(parsed.error()));
        }
        parsedMac = *parsed;
    }
    if (!parsedKey && !parsedMac) {
        return std::unexpected(QStringLiteral("needs \"key\" or \"mac\""));
    }
    if (platform == EPlatform::Mac && parsedMac) {
        return parsedMac;
    }
    return parsedKey;
}

}  // namespace roost
