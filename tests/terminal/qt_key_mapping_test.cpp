// How a Qt key event (Qt::Key, modifiers, text) becomes the encoder's event:
// the keypad, Backtab, the macOS Command/Control swap and Option as Alt, and
// the events that map to nothing. Pure data in, pure data out: no window.

#include "terminal/qt_key_mapping.hpp"
#include "core/vt_key_encoder.hpp"
#include "core/vt_terminal.hpp"

#include <QTest>

using roost::EKey;
using roost::EKeyAction;
using roost::KeyEvent;
using roost::MapQtKey;
using roost::QtKeyOptions;

namespace {

constexpr int SHIFT = 1;
constexpr int CTRL = 2;
constexpr int ALT = 4;
constexpr int SUPER = 8;

// Which conventions the row is mapped by.
enum EPlatform { Linux, Mac, MacOptionComposes };

QtKeyOptions Options(int platform) {
    switch (platform) {
        case Mac:
            return {.SwapControlAndMeta = true, .OptionAsAlt = true};
        case MacOptionComposes:
            return {.SwapControlAndMeta = true, .OptionAsAlt = false};
        default:
            return {};
    }
}

int Bits(const roost::KeyMods& mods) {
    return (mods.Shift ? SHIFT : 0) | (mods.Ctrl ? CTRL : 0) |
           (mods.Alt ? ALT : 0) | (mods.Super ? SUPER : 0);
}

}  // namespace

class QtKeyMappingTest : public QObject {
    Q_OBJECT

private slots:
    void MapsKeys_data() {
        QTest::addColumn<int>("platform");
        QTest::addColumn<int>("qtKey");
        QTest::addColumn<int>("qtMods");
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("key");
        QTest::addColumn<int>("mods");
        QTest::addColumn<int>("consumed");
        QTest::addColumn<QString>("typed");
        QTest::addColumn<int>("unshifted");

        const auto row = [](const char* name, int platform, int qtKey,
                            int qtMods, const QString& text, EKey key, int mods,
                            int consumed, const QString& typed, int unshifted) {
            QTest::newRow(name)
                << platform << qtKey << qtMods << text << static_cast<int>(key)
                << mods << consumed << typed << unshifted;
        };
        const int shift = Qt::ShiftModifier;
        const int control = Qt::ControlModifier;
        const int meta = Qt::MetaModifier;
        const int alt = Qt::AltModifier;
        const int keypad = Qt::KeypadModifier;

        row("a", Linux, Qt::Key_A, 0, "a", EKey::A, 0, 0, "a", 'a');
        // Shift made the capital: it is consumed, and the unshifted
        // codepoint stays the lowercase one.
        row("A", Linux, Qt::Key_A, shift, "A", EKey::A, SHIFT, SHIFT, "A", 'a');
        row("digit", Linux, Qt::Key_5, 0, "5", EKey::Digit5, 0, 0, "5", '5');
        row("shifted_symbol", Linux, Qt::Key_Exclam, shift, "!", EKey::Digit1,
            SHIFT, SHIFT, "!", '1');
        row("symbol", Linux, Qt::Key_Comma, 0, ",", EKey::Comma, 0, 0, ",",
            ',');
        row("space", Linux, Qt::Key_Space, 0, " ", EKey::Space, 0, 0, " ", ' ');
        // A layout the key table does not know: no physical key, the text
        // alone, lowercased for the unshifted codepoint.
        row("cyrillic", Linux, 0x042E, 0, "\xd1\x8e", EKey::Unidentified, 0, 0,
            "\xd1\x8e", 0x44e);
        row("cyrillic_capital", Linux, 0x042E, shift, "\xd0\xae",
            EKey::Unidentified, SHIFT, SHIFT, "\xd0\xae", 0x44e);

        // Control codes are not text. Under Ctrl the library wants what the
        // key types without it, and makes the code itself.
        row("ctrl_c", Linux, Qt::Key_C, control, "\x03", EKey::C, CTRL, 0, "c",
            'c');
        row("ctrl_shift_underscore", Linux, Qt::Key_Underscore, control | shift,
            "\x1f", EKey::Minus, CTRL | SHIFT, SHIFT, "_", '-');
        row("ctrl_bracket", Linux, Qt::Key_BracketLeft, control, "\x1b",
            EKey::BracketLeft, CTRL, 0, "[", '[');
        row("ctrl_shift_at_without_text", Linux, Qt::Key_At, control | shift,
            "", EKey::Digit2, CTRL | SHIFT, SHIFT, "@", '2');
        row("enter", Linux, Qt::Key_Return, 0, "\r", EKey::Enter, 0, 0, "", 0);
        row("tab", Linux, Qt::Key_Tab, 0, "\t", EKey::Tab, 0, 0, "", 0);
        row("backspace", Linux, Qt::Key_Backspace, 0, "\x08", EKey::Backspace,
            0, 0, "", 0);
        row("escape", Linux, Qt::Key_Escape, 0, "\x1b", EKey::Escape, 0, 0, "",
            0);
        // Backtab is Shift+Tab, with or without Qt saying so.
        row("backtab", Linux, Qt::Key_Backtab, shift, "\t", EKey::Tab, SHIFT, 0,
            "", 0);
        row("backtab_without_shift", Linux, Qt::Key_Backtab, 0, "", EKey::Tab,
            SHIFT, 0, "", 0);
        row("arrow", Linux, Qt::Key_Up, 0, "", EKey::ArrowUp, 0, 0, "", 0);
        // macOS puts the keypad flag on arrows: it means nothing for them.
        row("arrow_keypad_flag", Mac, Qt::Key_Left, keypad, "", EKey::ArrowLeft,
            0, 0, "", 0);
        row("f1", Linux, Qt::Key_F1, 0, "", EKey::F1, 0, 0, "", 0);
        row("f13", Linux, Qt::Key_F13, 0, "", EKey::F13, 0, 0, "", 0);
        row("page_down", Linux, Qt::Key_PageDown, 0, "", EKey::PageDown, 0, 0,
            "", 0);
        // A function-key code of macOS in the private use area is not text.
        row("mac_function_code", Mac, Qt::Key_Home, keypad, "\xef\x9c\xa9",
            EKey::Home, 0, 0, "", 0);

        // The keypad: the same Qt key as the main block's, told apart by
        // the flag. Enter has a key of its own.
        row("keypad_5", Linux, Qt::Key_5, keypad, "5", EKey::Keypad5, 0, 0, "5",
            0);
        row("keypad_plus", Linux, Qt::Key_Plus, keypad, "+", EKey::KeypadAdd, 0,
            0, "+", 0);
        row("keypad_minus", Linux, Qt::Key_Minus, keypad, "-",
            EKey::KeypadSubtract, 0, 0, "-", 0);
        row("keypad_slash", Linux, Qt::Key_Slash, keypad, "/",
            EKey::KeypadDivide, 0, 0, "/", 0);
        row("keypad_period", Linux, Qt::Key_Period, keypad, ".",
            EKey::KeypadDecimal, 0, 0, ".", 0);
        row("keypad_enter", Linux, Qt::Key_Enter, keypad, "\r",
            EKey::KeypadEnter, 0, 0, "", 0);
        row("main_plus", Linux, Qt::Key_Plus, shift, "+", EKey::Equal, SHIFT,
            SHIFT, "+", '=');

        // Linux: Control is Control, Meta is the Super key, Alt is Alt.
        row("linux_alt_x", Linux, Qt::Key_X, alt, "x", EKey::X, ALT, 0, "x",
            'x');
        row("linux_meta", Linux, Qt::Key_X, meta, "x", EKey::X, SUPER, 0, "x",
            'x');
        // macOS: Qt's Control is Command and its Meta is the Control key.
        row("mac_ctrl_c", Mac, Qt::Key_C, meta, "\x03", EKey::C, CTRL, 0, "c",
            'c');
        row("mac_command_c", Mac, Qt::Key_C, control, "c", EKey::C, SUPER, 0,
            "c", 'c');
        row("mac_control_key", Mac, Qt::Key_Meta, meta, "", EKey::ControlLeft,
            CTRL, 0, "", 0);
        row("mac_command_key", Mac, Qt::Key_Control, control, "",
            EKey::SuperLeft, SUPER, 0, "", 0);
        row("linux_control_key", Linux, Qt::Key_Control, control, "",
            EKey::ControlLeft, CTRL, 0, "", 0);
        // macOS Option composes "≈"; as Alt the key is x again (X with
        // Shift), and the composed text is dropped.
        row("mac_option_as_alt", Mac, Qt::Key_X, alt, "\xe2\x89\x88", EKey::X,
            ALT, 0, "x", 'x');
        row("mac_option_shift_as_alt", Mac, Qt::Key_X, alt | shift, "\xcb\x9b",
            EKey::X, ALT | SHIFT, SHIFT, "X", 'x');
        // Without it Option is a composing modifier: the text stands and
        // Alt is used up by it.
        row("mac_option_composes", MacOptionComposes, Qt::Key_X, alt,
            "\xe2\x89\x88", EKey::X, ALT, ALT, "\xe2\x89\x88", 'x');
        row("shift_key", Linux, Qt::Key_Shift, shift, "", EKey::ShiftLeft,
            SHIFT, 0, "", 0);
    }

    void MapsKeys() {
        QFETCH(int, platform);
        QFETCH(int, qtKey);
        QFETCH(int, qtMods);
        QFETCH(QString, text);
        QFETCH(int, key);
        QFETCH(int, mods);
        QFETCH(int, consumed);
        QFETCH(QString, typed);
        QFETCH(int, unshifted);

        const auto event = MapQtKey(
            qtKey,
            Qt::KeyboardModifiers::fromInt(static_cast<unsigned>(qtMods)), text,
            EKeyAction::Press, Options(platform));
        QVERIFY(event.has_value());
        QCOMPARE(static_cast<int>(event->Key), key);
        QCOMPARE(Bits(event->Mods), mods);
        QCOMPARE(Bits(event->ConsumedMods), consumed);
        QCOMPARE(event->Text, typed);
        QCOMPARE(static_cast<int>(event->UnshiftedCodepoint), unshifted);
        QCOMPARE(event->Action, EKeyAction::Press);
    }

    void EncodesCtrlKeys_data() {
        QTest::addColumn<QByteArray>("setup");
        QTest::addColumn<int>("qtKey");
        QTest::addColumn<int>("qtMods");
        QTest::addColumn<QString>("text");
        QTest::addColumn<QByteArray>("expected");

        const QByteArray none;
        const QByteArray disambiguate = "\x1b[>1u";
        const int ctrl = Qt::ControlModifier;
        const int shift = Qt::ShiftModifier;
        const auto row = [](const char* name, const QByteArray& setup,
                            int qtKey, int qtMods, const QString& text,
                            const QByteArray& expected) {
            QTest::newRow(name) << setup << qtKey << qtMods << text << expected;
        };

        // What Qt gives for the key, then the library's rules (ctrlSeq in
        // key_encode.zig): C0 codes for the keys of the Kitty table, CSI u for
        // the ones fixterms keeps apart (Ctrl+[, Ctrl+Shift+@, Ctrl+Shift+
        // letter).
        row("ctrl_c", none, Qt::Key_C, ctrl, "\x03", "\x03");
        row("ctrl_shift_minus", none, Qt::Key_Underscore, ctrl | shift, "\x1f",
            "\x1f");
        row("ctrl_slash", none, Qt::Key_Slash, ctrl, "\x1f", "\x1f");
        row("ctrl_shift_6", none, Qt::Key_AsciiCircum, ctrl | shift, "\x1e",
            "\x1e");
        row("ctrl_bracket", none, Qt::Key_BracketLeft, ctrl, "\x1b",
            "\x1b[91;5u");
        row("ctrl_shift_2", none, Qt::Key_At, ctrl | shift, "", "\x1b[64;5u");
        row("ctrl_shift_a", none, Qt::Key_A, ctrl | shift, "\x01",
            "\x1b[97;6u");
        row("kitty_ctrl_shift_minus", disambiguate, Qt::Key_Underscore,
            ctrl | shift, "\x1f", "\x1b[45;6u");
    }

    void EncodesCtrlKeys() {
        QFETCH(QByteArray, setup);
        QFETCH(int, qtKey);
        QFETCH(int, qtMods);
        QFETCH(QString, text);
        QFETCH(QByteArray, expected);

        auto terminal = roost::VtTerminal::Create(80, 24);
        QVERIFY(terminal.has_value());
        terminal->Write(setup);
        auto encoder = roost::VtKeyEncoder::Create();
        QVERIFY(encoder.has_value());
        QVERIFY(encoder->SyncFrom(*terminal).has_value());

        const auto event = MapQtKey(
            qtKey,
            Qt::KeyboardModifiers::fromInt(static_cast<unsigned>(qtMods)), text,
            EKeyAction::Press, {});
        QVERIFY(event.has_value());
        const auto bytes = encoder->Encode(*event);
        QVERIFY(bytes.has_value());
        QCOMPARE(*bytes, expected);
    }

    void MapsToNothing_data() {
        QTest::addColumn<int>("qtKey");
        QTest::addColumn<QString>("text");

        // A dead key waiting for the next one, a compose step, a key Qt has
        // no name for, and one outside the mapped set that types nothing.
        QTest::newRow("dead_acute") << int{Qt::Key_Dead_Acute} << QString();
        QTest::newRow("dead_grave") << int{Qt::Key_Dead_Grave} << QString();
        QTest::newRow("compose") << int{Qt::Key_Multi_key} << QString();
        QTest::newRow("unknown") << int{Qt::Key_unknown} << QString();
        QTest::newRow("media_key") << int{Qt::Key_VolumeUp} << QString();
        QTest::newRow("control_code_only") << 0x042E << QString("\x03");
    }

    void MapsToNothing() {
        QFETCH(int, qtKey);
        QFETCH(QString, text);
        QVERIFY(!MapQtKey(qtKey, {}, text, EKeyAction::Press, {}).has_value());
    }

    void KeepsTheAction() {
        const auto action = [](EKeyAction wanted) {
            return MapQtKey(Qt::Key_A, {}, "a", wanted, {})->Action;
        };
        QCOMPARE(action(EKeyAction::Press), EKeyAction::Press);
        QCOMPARE(action(EKeyAction::Repeat), EKeyAction::Repeat);
        QCOMPARE(action(EKeyAction::Release), EKeyAction::Release);
    }

    void NumLockAndCapsLockAreNotInferred() {
        // Qt does not say, so the mapping never claims them.
        const auto event =
            MapQtKey(Qt::Key_5, Qt::KeypadModifier, "5", EKeyAction::Press, {});
        QVERIFY(event.has_value());
        QVERIFY(!event->Mods.NumLock);
        QVERIFY(!event->Mods.CapsLock);
    }
};

QTEST_APPLESS_MAIN(QtKeyMappingTest)
#include "qt_key_mapping_test.moc"
