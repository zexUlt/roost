// The US key at a native key code, and which Qt key a binding matches on a
// non-Latin layout. Pure data in, pure data out: no window. The codes are
// literals: kVK_ANSI_* of macOS HIToolbox/Events.h and KEY_* of Linux
// input-event-codes.h plus 8.

#include "terminal/physical_keys.hpp"

#include <QTest>

#include <optional>

using roost::BindingKey;
using roost::EKey;
using roost::ENativeKeyCodes;
using roost::IsNonLatinLetter;
using roost::PhysicalKey;

Q_DECLARE_METATYPE(roost::EKey)

namespace {

struct Letter {
    EKey Key;
    quint32 Mac;
    quint32 Xkb;
};

constexpr Letter LETTERS[] = {
    {EKey::A, 0x00, 38}, {EKey::B, 0x0B, 56}, {EKey::C, 0x08, 54},
    {EKey::D, 0x02, 40}, {EKey::E, 0x0E, 26}, {EKey::F, 0x03, 41},
    {EKey::G, 0x05, 42}, {EKey::H, 0x04, 43}, {EKey::I, 0x22, 31},
    {EKey::J, 0x26, 44}, {EKey::K, 0x28, 45}, {EKey::L, 0x25, 46},
    {EKey::M, 0x2E, 58}, {EKey::N, 0x2D, 57}, {EKey::O, 0x1F, 32},
    {EKey::P, 0x23, 33}, {EKey::Q, 0x0C, 24}, {EKey::R, 0x0F, 27},
    {EKey::S, 0x01, 39}, {EKey::T, 0x11, 28}, {EKey::U, 0x20, 30},
    {EKey::V, 0x09, 55}, {EKey::W, 0x0D, 25}, {EKey::X, 0x07, 53},
    {EKey::Y, 0x10, 29}, {EKey::Z, 0x06, 52},
};

}  // namespace

class PhysicalKeysTest : public QObject {
    Q_OBJECT

private slots:
    void MacLetters_data() {
        QTest::addColumn<quint32>("code");
        QTest::addColumn<EKey>("key");
        for (const Letter& letter : LETTERS) {
            QTest::newRow(qPrintable(QString::number(letter.Mac, 16)))
                << letter.Mac << letter.Key;
        }
    }

    void MacLetters() {
        QFETCH(quint32, code);
        QFETCH(EKey, key);
        QCOMPARE(PhysicalKey(ENativeKeyCodes::MacVirtualKey, code), key);
    }

    void XkbLetters_data() {
        QTest::addColumn<quint32>("code");
        QTest::addColumn<EKey>("key");
        for (const Letter& letter : LETTERS) {
            QTest::newRow(qPrintable(QString::number(letter.Xkb)))
                << letter.Xkb << letter.Key;
        }
    }

    void XkbLetters() {
        QFETCH(quint32, code);
        QFETCH(EKey, key);
        QCOMPARE(PhysicalKey(ENativeKeyCodes::XkbKeycode, code), key);
    }

    void Symbols_data() {
        QTest::addColumn<int>("codes");
        QTest::addColumn<quint32>("code");
        QTest::addColumn<EKey>("key");
        const auto row = [](const char* name, quint32 mac, quint32 xkb,
                            EKey key) {
            QTest::newRow(qPrintable(QString("mac_%1").arg(name)))
                << int(ENativeKeyCodes::MacVirtualKey) << mac << key;
            QTest::newRow(qPrintable(QString("xkb_%1").arg(name)))
                << int(ENativeKeyCodes::XkbKeycode) << xkb << key;
        };
        row("Backquote", 0x32, 49, EKey::Backquote);
        row("Backslash", 0x2A, 51, EKey::Backslash);
        row("BracketLeft", 0x21, 34, EKey::BracketLeft);
        row("BracketRight", 0x1E, 35, EKey::BracketRight);
        row("Comma", 0x2B, 59, EKey::Comma);
        row("Equal", 0x18, 21, EKey::Equal);
        row("Minus", 0x1B, 20, EKey::Minus);
        row("Period", 0x2F, 60, EKey::Period);
        row("Quote", 0x27, 48, EKey::Quote);
        row("Semicolon", 0x29, 47, EKey::Semicolon);
        row("Slash", 0x2C, 61, EKey::Slash);
        // The digits are not in order on macOS: 5 is 0x17 and 6 is 0x16.
        row("Digit0", 0x1D, 19, EKey::Digit0);
        row("Digit1", 0x12, 10, EKey::Digit1);
        row("Digit2", 0x13, 11, EKey::Digit2);
        row("Digit3", 0x14, 12, EKey::Digit3);
        row("Digit4", 0x15, 13, EKey::Digit4);
        row("Digit5", 0x17, 14, EKey::Digit5);
        row("Digit6", 0x16, 15, EKey::Digit6);
        row("Digit7", 0x1A, 16, EKey::Digit7);
        row("Digit8", 0x1C, 17, EKey::Digit8);
        row("Digit9", 0x19, 18, EKey::Digit9);
    }

    void Symbols() {
        QFETCH(int, codes);
        QFETCH(quint32, code);
        QFETCH(EKey, key);
        QCOMPARE(PhysicalKey(ENativeKeyCodes(codes), code), key);
    }

    void CodeSetsDoNotMix() {
        // The same number is a different key in each set: 38 is the xkb code of
        // A and 0x26 (38) the macOS code of J; 44 is the xkb code of J and 0x2C
        // (44) the macOS code of Slash.
        QCOMPARE(PhysicalKey(ENativeKeyCodes::XkbKeycode, 38), EKey::A);
        QCOMPARE(PhysicalKey(ENativeKeyCodes::MacVirtualKey, 38), EKey::J);
        QCOMPARE(PhysicalKey(ENativeKeyCodes::XkbKeycode, 44), EKey::J);
        QCOMPARE(PhysicalKey(ENativeKeyCodes::MacVirtualKey, 44), EKey::Slash);
    }

    void UnknownAndNone() {
        // kVK_ISO_Section, evdev 86 (the ISO key) plus 8, and a code past every
        // table.
        QCOMPARE(PhysicalKey(ENativeKeyCodes::MacVirtualKey, 0x0A),
                 EKey::Unidentified);
        QCOMPARE(PhysicalKey(ENativeKeyCodes::XkbKeycode, 94),
                 EKey::Unidentified);
        QCOMPARE(PhysicalKey(ENativeKeyCodes::MacVirtualKey, 0xFFFF),
                 EKey::Unidentified);
        QCOMPARE(PhysicalKey(ENativeKeyCodes::XkbKeycode, 0xFFFF),
                 EKey::Unidentified);
        QCOMPARE(PhysicalKey(ENativeKeyCodes::None, 0x26), EKey::Unidentified);
        QCOMPARE(PhysicalKey(ENativeKeyCodes::None, 38), EKey::Unidentified);
    }

    void MacZeroIsA() {
        QCOMPARE(PhysicalKey(ENativeKeyCodes::MacVirtualKey, 0), EKey::A);
        QCOMPARE(PhysicalKey(ENativeKeyCodes::XkbKeycode, 0),
                 EKey::Unidentified);
    }

    void NativeKeyCodeByCodeSet() {
        // nativeScanCode 44, nativeVirtualKey 0x26, nativeModifiers 0x100, as
        // a real event carries.
        const QKeyEvent event(QEvent::KeyPress, Qt::Key_J, Qt::NoModifier, 44,
                              0x26, 0x100);
        QCOMPARE(roost::NativeKeyCode(event, ENativeKeyCodes::MacVirtualKey),
                 std::optional<quint32>(0x26));
        QCOMPARE(roost::NativeKeyCode(event, ENativeKeyCodes::XkbKeycode),
                 std::optional<quint32>(44));
        QVERIFY(!roost::NativeKeyCode(event, ENativeKeyCodes::None));
    }

    void NativeKeyCodeMacPhysicalA() {
        // A real A press: virtual key 0 with the modifier flags of a key event.
        const QKeyEvent event(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, 0, 0,
                              0x100);
        QCOMPARE(roost::NativeKeyCode(event, ENativeKeyCodes::MacVirtualKey),
                 std::optional<quint32>(0));
    }

    void NativeKeyCodeSyntheticEvent() {
        // The constructor without native codes zeroes all three.
        const QKeyEvent event(QEvent::KeyPress, 0x44E, Qt::NoModifier,
                              QString(QChar(0x44E)));
        QVERIFY(!roost::NativeKeyCode(event, ENativeKeyCodes::MacVirtualKey));
        QVERIFY(!roost::NativeKeyCode(event, ENativeKeyCodes::XkbKeycode));
        QCOMPARE(PhysicalKey(ENativeKeyCodes::MacVirtualKey, std::nullopt),
                 EKey::Unidentified);
        QCOMPARE(PhysicalKey(ENativeKeyCodes::XkbKeycode, std::nullopt),
                 EKey::Unidentified);
        // A Cyrillic letter without a code is not remapped to A.
        QCOMPARE(::BindingKey(
                     0x44E,  // я
                     Qt::NoModifier,
                     PhysicalKey(ENativeKeyCodes::MacVirtualKey,
                                 roost::NativeKeyCode(
                                     event, ENativeKeyCodes::MacVirtualKey))),
                 0x44E);
    }

    void BindingKeySymbols_data() {
        QTest::addColumn<EKey>("physical");
        QTest::addColumn<int>("unshifted");
        QTest::addColumn<int>("shifted");
        const auto row = [](const char* name, EKey key, int unshifted,
                            int shifted) {
            QTest::newRow(name) << key << unshifted << shifted;
        };
        row("Backquote", EKey::Backquote, Qt::Key_QuoteLeft,
            Qt::Key_AsciiTilde);
        row("Backslash", EKey::Backslash, Qt::Key_Backslash, Qt::Key_Bar);
        row("BracketLeft", EKey::BracketLeft, Qt::Key_BracketLeft,
            Qt::Key_BraceLeft);
        row("BracketRight", EKey::BracketRight, Qt::Key_BracketRight,
            Qt::Key_BraceRight);
        row("Comma", EKey::Comma, Qt::Key_Comma, Qt::Key_Less);
        row("Equal", EKey::Equal, Qt::Key_Equal, Qt::Key_Plus);
        row("Minus", EKey::Minus, Qt::Key_Minus, Qt::Key_Underscore);
        row("Period", EKey::Period, Qt::Key_Period, Qt::Key_Greater);
        row("Quote", EKey::Quote, Qt::Key_Apostrophe, Qt::Key_QuoteDbl);
        row("Semicolon", EKey::Semicolon, Qt::Key_Semicolon, Qt::Key_Colon);
        row("Slash", EKey::Slash, Qt::Key_Slash, Qt::Key_Question);
        row("Digit0", EKey::Digit0, Qt::Key_0, Qt::Key_ParenRight);
        row("Digit1", EKey::Digit1, Qt::Key_1, Qt::Key_Exclam);
        row("Digit2", EKey::Digit2, Qt::Key_2, Qt::Key_At);
        row("Digit3", EKey::Digit3, Qt::Key_3, Qt::Key_NumberSign);
        row("Digit4", EKey::Digit4, Qt::Key_4, Qt::Key_Dollar);
        row("Digit5", EKey::Digit5, Qt::Key_5, Qt::Key_Percent);
        row("Digit6", EKey::Digit6, Qt::Key_6, Qt::Key_AsciiCircum);
        row("Digit7", EKey::Digit7, Qt::Key_7, Qt::Key_Ampersand);
        row("Digit8", EKey::Digit8, Qt::Key_8, Qt::Key_Asterisk);
        row("Digit9", EKey::Digit9, Qt::Key_9, Qt::Key_ParenLeft);
    }

    void BindingKeySymbols() {
        QFETCH(EKey, physical);
        QFETCH(int, unshifted);
        QFETCH(int, shifted);
        constexpr int cyrillicBe = 0x411;
        QCOMPARE(::BindingKey(cyrillicBe, Qt::NoModifier, physical), unshifted);
        QCOMPARE(::BindingKey(cyrillicBe, Qt::ShiftModifier, physical),
                 shifted);
        // Only Shift picks the shifted symbol.
        QCOMPARE(::BindingKey(cyrillicBe, Qt::ControlModifier, physical),
                 unshifted);
        QCOMPARE(::BindingKey(cyrillicBe, Qt::AltModifier, physical),
                 unshifted);
        QCOMPARE(::BindingKey(cyrillicBe, Qt::MetaModifier, physical),
                 unshifted);
        QCOMPARE(
            ::BindingKey(cyrillicBe, Qt::ControlModifier | Qt::ShiftModifier,
                         physical),
            shifted);
    }

    void BindingKey_data() {
        QTest::addColumn<int>("qtKey");
        QTest::addColumn<int>("mods");
        QTest::addColumn<EKey>("physical");
        QTest::addColumn<int>("expected");

        const auto row = [](const char* name, int qtKey, int mods,
                            EKey physical, int expected) {
            QTest::newRow(name) << qtKey << mods << physical << expected;
        };
        const int shift = Qt::ShiftModifier;
        // A Latin layout keeps its letter wherever the key is.
        row("dvorak_z_kept", Qt::Key_Z, 0, EKey::Slash, Qt::Key_Z);
        row("french_e_acute_kept", 0xC9, 0, EKey::Digit2, 0xC9);
        row("czech_e_caron_kept", 0x11A, 0, EKey::Digit2, 0x11A);
        // A letter of a non-Latin script gives the way to the physical key.
        row("ru_j", 0x41E, 0, EKey::J, Qt::Key_J);
        row("ru_shift_z", 0x42F, shift, EKey::Z, Qt::Key_Z);
        row("ru_comma", 0x411, 0, EKey::Comma, Qt::Key_Comma);
        row("ru_shift_comma", 0x411, shift, EKey::Comma, Qt::Key_Less);
        row("ru_shift_period", 0x42E, shift, EKey::Period, Qt::Key_Greater);
        row("ru_slash", 0x2F, 0, EKey::Backslash, 0x2F);
        // Shift+7 on the Russian layout types '?', which is not a letter: kept.
        row("ru_shift_digit_kept", Qt::Key_Question, shift, EKey::Digit7,
            Qt::Key_Question);
        // The physical A key, code 0 on macOS: the lower edge of the letters.
        row("ru_a", 0x424, 0, EKey::A, Qt::Key_A);
        row("ru_z", 0x42F, 0, EKey::Z, Qt::Key_Z);
        row("greek_j", 0x3BE, 0, EKey::J, Qt::Key_J);
        // µ is U+00B5 of the Common script: Option+m on a Latin macOS layout.
        row("micro_sign_kept", 0xB5, 0, EKey::M, 0xB5);
        // Not a letter: kept.
        row("numero_sign_kept", 0x2116, 0, EKey::Backslash, 0x2116);
        row("special_key_kept", Qt::Key_Escape, 0, EKey::A, Qt::Key_Escape);
        // No physical key to follow.
        row("ru_unidentified", 0x41E, 0, EKey::Unidentified, 0x41E);
        row("ru_not_main_block", 0x41E, 0, EKey::Enter, 0x41E);
    }

    void BindingKey() {
        QFETCH(int, qtKey);
        QFETCH(int, mods);
        QFETCH(EKey, physical);
        QFETCH(int, expected);
        QCOMPARE(::BindingKey(qtKey, Qt::KeyboardModifiers(mods), physical),
                 expected);
    }

    void IsNonLatinLetter_data() {
        QTest::addColumn<int>("qtKey");
        QTest::addColumn<bool>("expected");
        QTest::newRow("cyrillic") << 0x41E << true;
        QTest::newRow("greek") << 0x3A9 << true;
        QTest::newRow("hebrew") << 0x5D0 << true;
        QTest::newRow("micro_sign_common") << 0xB5 << false;
        QTest::newRow("combining_inherited") << 0x345 << false;
        QTest::newRow("unassigned") << 0x378 << false;
        QTest::newRow("latin") << int(Qt::Key_A) << false;
        QTest::newRow("latin_extended") << 0xC9 << false;
        QTest::newRow("digit") << int(Qt::Key_1) << false;
        QTest::newRow("symbol") << 0x2116 << false;
        QTest::newRow("special_key") << int(Qt::Key_Escape) << false;
        QTest::newRow("zero") << 0 << false;
    }

    void IsNonLatinLetter() {
        QFETCH(int, qtKey);
        QFETCH(bool, expected);
        QCOMPARE(::IsNonLatinLetter(qtKey), expected);
    }
};

QTEST_APPLESS_MAIN(PhysicalKeysTest)
#include "physical_keys_test.moc"
