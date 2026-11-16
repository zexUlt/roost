// What a key event encodes to, against a real terminal whose modes were set
// by the bytes a program would send. The expected bytes are those of the
// legacy xterm encoding and of the Kitty keyboard protocol
// (https://sw.kovidgoyal.net/kitty/keyboard-protocol/), written out by hand,
// not copied from what the library printed.

#include "core/vt_key_encoder.hpp"
#include "core/vt_terminal.hpp"

#include <QByteArray>
#include <QTest>

#include <utility>

Q_DECLARE_METATYPE(roost::KeyEvent)

using roost::EKey;
using roost::EKeyAction;
using roost::KeyEvent;
using roost::VtKeyEncoder;
using roost::VtTerminal;

namespace {

constexpr int SHIFT = 1;
constexpr int CTRL = 2;
constexpr int ALT = 4;

VtTerminal NewTerminal() {
    auto terminal = VtTerminal::Create(80, 24);
    if (!terminal) {
        qFatal("%s", qPrintable(terminal.error()));
    }
    return std::move(*terminal);
}

VtKeyEncoder NewEncoder() {
    auto encoder = VtKeyEncoder::Create();
    if (!encoder) {
        qFatal("%s", qPrintable(encoder.error()));
    }
    // The same on every platform: Alt is Alt (on macOS the library composes
    // text for Option otherwise).
    encoder->SetMacosOptionAsAlt(true);
    return std::move(*encoder);
}

KeyEvent Event(EKey key, int mods = 0, const QString& text = {},
               EKeyAction action = EKeyAction::Press) {
    KeyEvent event;
    event.Action = action;
    event.Key = key;
    event.Mods.Shift = (mods & SHIFT) != 0;
    event.Mods.Ctrl = (mods & CTRL) != 0;
    event.Mods.Alt = (mods & ALT) != 0;
    event.ConsumedMods.Shift = event.Mods.Shift && !text.isEmpty();
    event.Text = text;
    if (!text.isEmpty()) {
        event.UnshiftedCodepoint = text.toLower().toUcs4().front();
    } else if (key >= EKey::A && key <= EKey::Z) {
        // What the Qt mapping gives a letter key whatever it typed.
        event.UnshiftedCodepoint =
            U'a' + static_cast<char32_t>(static_cast<int>(key) -
                                         static_cast<int>(EKey::A));
    }
    return event;
}

}  // namespace

class VtKeyEncoderTest : public QObject {
    Q_OBJECT

private:
    // The terminal gets `setup`, the encoder follows it, then one key.
    static QByteArray Encode(const QByteArray& setup, const KeyEvent& event) {
        auto terminal = NewTerminal();
        terminal.Write(setup);
        auto encoder = NewEncoder();
        const auto synced = encoder.SyncFrom(terminal);
        if (!synced) {
            qFatal("%s", qPrintable(synced.error()));
        }
        const auto bytes = encoder.Encode(event);
        if (!bytes) {
            qFatal("%s", qPrintable(bytes.error()));
        }
        return *bytes;
    }

    static void AddRow(const char* name, const QByteArray& setup,
                       const KeyEvent& event, const QByteArray& expected) {
        QTest::newRow(name) << setup << event << expected;
    }

private slots:
    void EncodesKeys_data() {
        QTest::addColumn<QByteArray>("setup");
        QTest::addColumn<KeyEvent>("event");
        QTest::addColumn<QByteArray>("expected");

        const QByteArray none;
        const QByteArray cursorKeys = "\x1b[?1h";
        // DECKPAM (ESC =), and mode 1035 off: it is on by default and makes
        // the keypad numeric whatever DECKPAM says.
        const QByteArray keypadKeys = "\x1b=\x1b[?1035l";
        // Kitty: push the flags with CSI > flags u.
        const QByteArray disambiguate = "\x1b[>1u";
        const QByteArray reportEvents = "\x1b[>3u";
        const QByteArray reportAll = "\x1b[>8u";

        // Text keys: the text itself, in UTF-8.
        AddRow("a", none, Event(EKey::A, 0, "a"), "a");
        AddRow("A", none, Event(EKey::A, SHIFT, "A"), "A");
        AddRow("yu", none, Event(EKey::Unidentified, 0, "\xd1\x8e"),
               "\xd1\x8e");
        AddRow("space", none, Event(EKey::Space, 0, " "), " ");
        AddRow("ctrl_space", none, Event(EKey::Space, CTRL, " "),
               QByteArray(1, '\0'));

        AddRow("enter", none, Event(EKey::Enter), "\r");
        AddRow("tab", none, Event(EKey::Tab), "\t");
        AddRow("shift_tab", none, Event(EKey::Tab, SHIFT), "\x1b[Z");
        AddRow("backspace", none, Event(EKey::Backspace), "\x7f");
        AddRow("escape", none, Event(EKey::Escape), "\x1b");

        // Cursor keys: CSI in the normal mode, SS3 in the application mode
        // (DECCKM, CSI ? 1 h); a modifier forces the CSI form.
        AddRow("up", none, Event(EKey::ArrowUp), "\x1b[A");
        AddRow("down", none, Event(EKey::ArrowDown), "\x1b[B");
        AddRow("right", none, Event(EKey::ArrowRight), "\x1b[C");
        AddRow("left", none, Event(EKey::ArrowLeft), "\x1b[D");
        AddRow("up_application", cursorKeys, Event(EKey::ArrowUp), "\x1bOA");
        AddRow("left_application", cursorKeys, Event(EKey::ArrowLeft),
               "\x1bOD");
        AddRow("home_application", cursorKeys, Event(EKey::Home), "\x1bOH");
        AddRow("ctrl_up", none, Event(EKey::ArrowUp, CTRL), "\x1b[1;5A");
        AddRow("ctrl_up_application", cursorKeys, Event(EKey::ArrowUp, CTRL),
               "\x1b[1;5A");

        AddRow("ctrl_c", none, Event(EKey::C, CTRL), "\x03");
        AddRow("ctrl_z", none, Event(EKey::Z, CTRL), "\x1a");
        AddRow("alt_x", none, Event(EKey::X, ALT, "x"), "\x1bx");

        AddRow("f1", none, Event(EKey::F1), "\x1bOP");
        AddRow("f4", none, Event(EKey::F4), "\x1bOS");
        AddRow("f5", none, Event(EKey::F5), "\x1b[15~");
        AddRow("f12", none, Event(EKey::F12), "\x1b[24~");
        AddRow("home", none, Event(EKey::Home), "\x1b[H");
        AddRow("end", none, Event(EKey::End), "\x1b[F");
        AddRow("page_up", none, Event(EKey::PageUp), "\x1b[5~");
        AddRow("page_down", none, Event(EKey::PageDown), "\x1b[6~");
        AddRow("insert", none, Event(EKey::Insert), "\x1b[2~");
        AddRow("delete", none, Event(EKey::Delete), "\x1b[3~");

        // Keypad: the digit, or SS3 letters in the keypad application mode
        // (DECKPAM, ESC =).
        AddRow("keypad_5", none, Event(EKey::Keypad5, 0, "5"), "5");
        AddRow("keypad_5_application", keypadKeys, Event(EKey::Keypad5, 0, "5"),
               "\x1bOu");
        AddRow("keypad_enter", none, Event(EKey::KeypadEnter), "\r");
        AddRow("keypad_enter_application", keypadKeys, Event(EKey::KeypadEnter),
               "\x1bOM");

        // Legacy mode reports presses and repeats only; a modifier alone is
        // nothing.
        AddRow("release_a", none, Event(EKey::A, 0, "a", EKeyAction::Release),
               "");
        AddRow("release_escape", none,
               Event(EKey::Escape, 0, {}, EKeyAction::Release), "");
        AddRow("repeat_a", none, Event(EKey::A, 0, "a", EKeyAction::Repeat),
               "a");
        AddRow("shift_alone", none, Event(EKey::ShiftLeft, SHIFT), "");
        AddRow("ctrl_alone", none, Event(EKey::ControlLeft, CTRL), "");

        // Kitty, "disambiguate" (flag 1): Escape and keys with Ctrl or Alt
        // become CSI codepoint ; modifiers u, the modifiers sent as 1 + bits
        // (shift 1, alt 2, ctrl 4). Text keys, Enter, Tab and Backspace
        // without modifiers stay as they were.
        AddRow("kitty_escape", disambiguate, Event(EKey::Escape), "\x1b[27u");
        AddRow("kitty_ctrl_c", disambiguate, Event(EKey::C, CTRL, {}),
               "\x1b[99;5u");
        AddRow("kitty_alt_x", disambiguate, Event(EKey::X, ALT, "x"),
               "\x1b[120;3u");
        AddRow("kitty_ctrl_enter", disambiguate, Event(EKey::Enter, CTRL),
               "\x1b[13;5u");
        AddRow("kitty_a", disambiguate, Event(EKey::A, 0, "a"), "a");
        AddRow("kitty_enter", disambiguate, Event(EKey::Enter), "\r");
        AddRow("kitty_up", disambiguate, Event(EKey::ArrowUp), "\x1b[A");
        // The shifted letter is reported by its unshifted codepoint, and
        // Shift, which made the text, is not repeated as a modifier.
        AddRow("kitty_A", disambiguate, Event(EKey::A, SHIFT, "A"), "A");

        // Kitty, flags 1 and 2: events other than a press carry an event type
        // after the modifiers (1 press, 2 repeat, 3 release), which makes the
        // modifiers field (1 = none) mandatory. A release of a text key is
        // reported as a code too; Enter, Tab and Backspace are the exception,
        // so that a shell still works after a program died with the flags on.
        AddRow("kitty_escape_release", reportEvents,
               Event(EKey::Escape, 0, {}, EKeyAction::Release), "\x1b[27;1:3u");
        AddRow("kitty_escape_repeat", reportEvents,
               Event(EKey::Escape, 0, {}, EKeyAction::Repeat), "\x1b[27;1:2u");
        AddRow("kitty_escape_press", reportEvents, Event(EKey::Escape),
               "\x1b[27u");
        AddRow("kitty_a_release", reportEvents,
               Event(EKey::A, 0, "a", EKeyAction::Release), "\x1b[97;1:3u");
        AddRow("kitty_enter_release", reportEvents,
               Event(EKey::Enter, 0, {}, EKeyAction::Release), "");
        AddRow("kitty_backspace_release", reportEvents,
               Event(EKey::Backspace, 0, {}, EKeyAction::Release), "");

        // Kitty, flag 8 ("report all keys as escape codes"): even plain text
        // becomes a code.
        AddRow("kitty_all_a", reportAll, Event(EKey::A, 0, "a"), "\x1b[97u");
        AddRow("kitty_all_enter", reportAll, Event(EKey::Enter), "\x1b[13u");
    }

    void EncodesKeys() {
        QFETCH(QByteArray, setup);
        QFETCH(KeyEvent, event);
        QFETCH(QByteArray, expected);
        QCOMPARE(Encode(setup, event), expected);
    }

    void FollowsTheTerminalAfterResync() {
        // One encoder, a terminal whose mode changes between two keys: the
        // encoder keeps what it was synced with until it is synced again.
        auto terminal = NewTerminal();
        auto encoder = NewEncoder();
        const auto up = Event(EKey::ArrowUp);
        const auto encode = [&] {
            const auto bytes = encoder.Encode(up);
            return bytes ? *bytes : QByteArray("error");
        };

        QVERIFY(encoder.SyncFrom(terminal));
        QCOMPARE(encode(), QByteArray("\x1b[A"));

        terminal.Write("\x1b[?1h");
        QCOMPARE(encode(), QByteArray("\x1b[A"));
        QVERIFY(encoder.SyncFrom(terminal));
        QCOMPARE(encode(), QByteArray("\x1bOA"));

        terminal.Write("\x1b[?1l");
        QVERIFY(encoder.SyncFrom(terminal));
        QCOMPARE(encode(), QByteArray("\x1b[A"));
    }

    void KeepsTextOfLongSequencesIntact() {
        // Text longer than the encoder's first buffer still comes out whole.
        auto terminal = NewTerminal();
        auto encoder = NewEncoder();
        QVERIFY(encoder.SyncFrom(terminal));
        const QString text(100, QLatin1Char('x'));
        const auto bytes = encoder.Encode(Event(EKey::X, 0, text));
        QVERIFY(bytes);
        QCOMPARE(*bytes, text.toUtf8());
    }
};

QTEST_APPLESS_MAIN(VtKeyEncoderTest)
#include "vt_key_encoder_test.moc"
