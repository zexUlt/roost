// MapQtKey and IsCopyShortcut on a non-Latin layout: a letter is the key at its
// position on the US layout, found by the native key code, while the text stays
// what the layout typed. Pure data in, pure data out: no window. The codes are
// literals: kVK_ANSI_* (macOS) and KEY_* + 8 (xkb).

#include "core/vt_key_encoder.hpp"
#include "core/vt_terminal.hpp"
#include "terminal/physical_keys.hpp"
#include "terminal/qt_key_mapping.hpp"

#include <QTest>

#include <optional>

using roost::EKey;
using roost::EKeyAction;
using roost::ENativeKeyCodes;
using roost::IsCopyShortcut;
using roost::MapQtKey;
using roost::QtKeyOptions;

namespace {

// Russian letters by Qt key, which names a letter by its capital.
constexpr int CYR_O = 0x041E;
constexpr int CYR_S = 0x0421;
constexpr int CYR_ZHE = 0x0416;
constexpr int CYR_EF = 0x0424;
constexpr int CYR_YA = 0x042F;
// Polish Ą: Qt names the letter by its capital.
constexpr int LATIN_A_OGONEK = 0x0104;
// Native codes of the keys under test.
constexpr quint32 MAC_J = 0x26;
constexpr quint32 XKB_J = 44;
constexpr quint32 MAC_C = 0x08;
constexpr quint32 XKB_C = 54;
constexpr quint32 MAC_A = 0x00;
constexpr quint32 XKB_A = 38;
constexpr quint32 MAC_Z = 0x06;
constexpr quint32 XKB_Z = 52;
constexpr quint32 MAC_V = 0x09;
constexpr quint32 XKB_V = 55;
constexpr quint32 MAC_SEMICOLON = 0x29;
constexpr quint32 XKB_SEMICOLON = 47;
constexpr quint32 MAC_SLASH = 0x2C;

enum EPlatform { Mac, Xkb };
enum EProtocol { Legacy, Kitty, KittyAlternates };

QtKeyOptions Options(int platform) {
    if (platform == Mac) {
        return {.SwapControlAndMeta = true,
                .OptionAsAlt = true,
                .NativeKeys = ENativeKeyCodes::MacVirtualKey};
    }
    return {.NativeKeys = ENativeKeyCodes::XkbKeycode};
}

// The modifier Qt delivers for the physical Control key, written out per
// platform: on macOS Qt names the Control key Meta.
Qt::KeyboardModifier ControlKey(int platform) {
    return platform == Mac ? Qt::MetaModifier : Qt::ControlModifier;
}

}  // namespace

class QtKeyLayoutTest : public QObject {
    Q_OBJECT

private slots:
    void MapsLettersByPhysicalKey_data() {
        QTest::addColumn<int>("platform");
        QTest::addColumn<int>("qtKey");
        QTest::addColumn<QString>("text");
        QTest::addColumn<quint32>("code");
        QTest::addColumn<EKeyAction>("action");
        QTest::addColumn<int>("key");
        QTest::addColumn<int>("unshifted");

        const auto row = [](const char* name, int platform, int qtKey,
                            const QString& text, quint32 code,
                            EKeyAction action, EKey key, int unshifted) {
            QTest::newRow(name) << platform << qtKey << text << code << action
                                << static_cast<int>(key) << unshifted;
        };

        // The key is the physical J; the text and the unshifted codepoint are
        // the Russian "о", which the Kitty protocol reports.
        row("ru_j_mac", Mac, CYR_O, "о", MAC_J, EKeyAction::Press, EKey::J,
            0x043E);
        row("ru_j_xkb", Xkb, CYR_O, "о", XKB_J, EKeyAction::Press, EKey::J,
            0x043E);
        row("ru_release", Xkb, CYR_O, "о", XKB_J, EKeyAction::Release, EKey::J,
            0x043E);
        row("ru_repeat", Xkb, CYR_O, "о", XKB_J, EKeyAction::Repeat, EKey::J,
            0x043E);
        // The ends of the letter range: the first and the last key.
        row("ru_a_mac", Mac, CYR_EF, "ф", MAC_A, EKeyAction::Press, EKey::A,
            0x0444);
        row("ru_a_xkb", Xkb, CYR_EF, "ф", XKB_A, EKeyAction::Press, EKey::A,
            0x0444);
        row("ru_z_mac", Mac, CYR_YA, "я", MAC_Z, EKeyAction::Press, EKey::Z,
            0x044F);
        row("ru_z_xkb", Xkb, CYR_YA, "я", XKB_Z, EKeyAction::Press, EKey::Z,
            0x044F);
        // Shift gives the capital; the unshifted codepoint stays lowercase.
        row("ru_shifted", Xkb, CYR_O, "О", XKB_J, EKeyAction::Press, EKey::J,
            0x043E);
        // Latin Z at the position of the US "/" (Dvorak): the Latin letter
        // wins over the position.
        // Latin C at the position of the US J (Dvorak): still C.
        row("dvorak_c_at_j", Mac, Qt::Key_C, "c", MAC_J, EKeyAction::Press,
            EKey::C, 'c');
        row("dvorak_z_kept", Mac, Qt::Key_Z, "z", MAC_SLASH, EKeyAction::Press,
            EKey::Z, 'z');
    }

    void MapsLettersByPhysicalKey() {
        QFETCH(int, platform);
        QFETCH(int, qtKey);
        QFETCH(QString, text);
        QFETCH(quint32, code);
        QFETCH(EKeyAction, action);
        QFETCH(int, key);
        QFETCH(int, unshifted);

        const auto shifted = text.front().isUpper();
        const auto event =
            MapQtKey(qtKey, shifted ? Qt::ShiftModifier : Qt::NoModifier, text,
                     code, action, Options(platform));
        QVERIFY(event.has_value());
        QCOMPARE(static_cast<int>(event->Key), key);
        QCOMPARE(event->Text, text);
        QCOMPARE(static_cast<int>(event->UnshiftedCodepoint), unshifted);
        QCOMPARE(event->Action, action);
    }

    void SymbolKeyOfANonLatinLayoutStaysText_data() {
        QTest::addColumn<int>("platform");
        QTest::addColumn<quint32>("code");
        QTest::newRow("mac") << int{Mac} << MAC_SEMICOLON;
        QTest::newRow("xkb") << int{Xkb} << XKB_SEMICOLON;
    }

    void SymbolKeyOfANonLatinLayoutStaysText() {
        QFETCH(int, platform);
        QFETCH(quint32, code);
        // The Russian "ж" is on the US ";" key: not a letter key, so the
        // terminal gets the text alone.
        const auto event = MapQtKey(CYR_ZHE, {}, "ж", code, EKeyAction::Press,
                                    Options(platform));
        QVERIFY(event.has_value());
        QCOMPARE(event->Key, EKey::Unidentified);
        QCOMPARE(event->Text, QString("ж"));
        QCOMPARE(static_cast<int>(event->UnshiftedCodepoint), 0x0436);
    }

    void WithoutCodesOrOptionsALetterIsTextOnly_data() {
        QTest::addColumn<std::optional<quint32>>("code");
        QTest::addColumn<bool>("withOptions");
        QTest::newRow("no_options") << std::optional<quint32>(MAC_J) << false;
        QTest::newRow("no_code") << std::optional<quint32>() << true;
        QTest::newRow("unknown_code") << std::optional<quint32>(0xFFFF) << true;
    }

    void WithoutCodesOrOptionsALetterIsTextOnly() {
        QFETCH(std::optional<quint32>, code);
        QFETCH(bool, withOptions);
        const auto event =
            MapQtKey(CYR_O, {}, "о", code, EKeyAction::Press,
                     withOptions ? Options(Mac) : QtKeyOptions{});
        QVERIFY(event.has_value());
        QCOMPARE(event->Key, EKey::Unidentified);
        QCOMPARE(event->Text, QString("о"));
        QCOMPARE(static_cast<int>(event->UnshiftedCodepoint), 0x043E);
    }

    void EncodesCtrlOfTheRussianLetter_data() {
        QTest::addColumn<int>("platform");
        QTest::addColumn<QByteArray>("setup");
        QTest::addColumn<QString>("text");
        QTest::addColumn<quint32>("code");
        QTest::addColumn<QByteArray>("expected");

        const QByteArray none;
        // What Qt may give under Ctrl: the letter, the control code, nothing.
        for (const int platform : {Mac, Xkb}) {
            const char* tag = platform == Mac ? "mac" : "xkb";
            const quint32 code = platform == Mac ? MAC_C : XKB_C;
            // The tag is returned by value: a pointer into a temporary string
            // would dangle before newRow reads it.
            const auto name = [tag](const char* what) {
                return QString("%1_%2").arg(what, tag).toUtf8();
            };
            QTest::newRow(name("ctrl_c_text").constData())
                << platform << none << QString("с") << code
                << QByteArray("\x03");
            QTest::newRow(name("ctrl_c_control_code").constData())
                << platform << none << QString("\x03") << code
                << QByteArray("\x03");
            QTest::newRow(name("ctrl_c_no_text").constData())
                << platform << none << QString() << code << QByteArray("\x03");
        }
    }

    void EncodesCtrlOfTheRussianLetter() {
        QFETCH(int, platform);
        QFETCH(QByteArray, setup);
        QFETCH(QString, text);
        QFETCH(quint32, code);
        QFETCH(QByteArray, expected);

        auto terminal = roost::VtTerminal::Create(80, 24);
        QVERIFY(terminal.has_value());
        terminal->Write(setup);
        auto encoder = roost::VtKeyEncoder::Create();
        QVERIFY(encoder.has_value());
        QVERIFY(encoder->SyncFrom(*terminal).has_value());

        const auto event = MapQtKey(CYR_S, ControlKey(platform), text, code,
                                    EKeyAction::Press, Options(platform));
        QVERIFY(event.has_value());
        const auto bytes = encoder->Encode(*event);
        QVERIFY(bytes.has_value());
        QCOMPARE(*bytes, expected);
    }

    void EncodesCtrlLettersOfTheRussianLayout_data() {
        QTest::addColumn<int>("platform");
        QTest::addColumn<int>("qtKey");
        QTest::addColumn<quint32>("code");
        QTest::addColumn<int>("latinKey");
        QTest::addColumn<bool>("shift");
        QTest::addColumn<int>("protocol");
        QTest::addColumn<int>("textKind");

        // What Qt gives as text under Ctrl: nothing, or the control code.
        enum { NoText, ControlCode };
        const struct {
            const char* Name;
            int QtKey;
            int LatinKey;
            quint32 MacCode;
            quint32 XkbCode;
        } letters[] = {
            {"a", CYR_EF, Qt::Key_A, MAC_A, XKB_A},
            {"c", CYR_S, Qt::Key_C, MAC_C, XKB_C},
            {"z", CYR_YA, Qt::Key_Z, MAC_Z, XKB_Z},
        };
        const char* const protocols[] = {"legacy", "kitty", "kitty_alternates"};
        for (const auto& letter : letters) {
            for (const int platform : {Mac, Xkb}) {
                for (const bool shift : {false, true}) {
                    for (const int protocol :
                         {Legacy, Kitty, KittyAlternates}) {
                        for (const int kind : {NoText, ControlCode}) {
                            const auto name =
                                QString("%1_%2_%3_%4_%5")
                                    .arg(letter.Name)
                                    .arg(platform == Mac ? "mac" : "xkb")
                                    .arg(shift ? "shift" : "plain")
                                    .arg(protocols[protocol])
                                    .arg(kind == NoText ? "no_text"
                                                        : "control");
                            QTest::newRow(qPrintable(name))
                                << platform << letter.QtKey
                                << (platform == Mac ? letter.MacCode
                                                    : letter.XkbCode)
                                << letter.LatinKey << shift << protocol << kind;
                        }
                    }
                }
            }
        }
    }

    // Ctrl+letter of the Russian layout: legacy is the Latin key's byte; Kitty
    // reports the layout's letter as the code and, when alternates are asked
    // for, the Latin key as the base-layout alternate.
    void EncodesCtrlLettersOfTheRussianLayout() {
        QFETCH(int, platform);
        QFETCH(int, qtKey);
        QFETCH(quint32, code);
        QFETCH(int, latinKey);
        QFETCH(bool, shift);
        QFETCH(int, protocol);
        QFETCH(int, textKind);

        auto terminal = roost::VtTerminal::Create(80, 24);
        QVERIFY(terminal.has_value());
        // Kitty keyboard protocol: disambiguate (1), plus alternates (4).
        terminal->Write(protocol == Legacy  ? QByteArray()
                        : protocol == Kitty ? QByteArray("\x1b[>1u")
                                            : QByteArray("\x1b[>5u"));
        auto encoder = roost::VtKeyEncoder::Create();
        QVERIFY(encoder.has_value());
        QVERIFY(encoder->SyncFrom(*terminal).has_value());

        Qt::KeyboardModifiers mods = ControlKey(platform);
        if (shift) {
            mods |= Qt::ShiftModifier;
        }
        // The control code the Latin key gives: A is 0x01.
        const int offset = latinKey - Qt::Key_A;
        const QString control =
            QString(QChar(static_cast<char16_t>(offset + 1)));
        const QString text = textKind == 0 ? QString() : control;

        const auto russian = MapQtKey(qtKey, mods, text, code,
                                      EKeyAction::Press, Options(platform));
        QVERIFY(russian.has_value());
        const auto bytes = encoder->Encode(*russian);
        QVERIFY(bytes.has_value());

        const int latinCode = 'a' + offset;
        const int layoutCode =
            static_cast<int>(QChar::toLower(static_cast<char32_t>(qtKey)));
        const int modifiers = shift ? 6 : 5;
        if (protocol == Legacy && !shift) {
            // The same byte as the Latin key.
            QCOMPARE(*bytes, QByteArray(1, static_cast<char>(offset + 1)));
        } else if (protocol == Legacy) {
            // Known limitation: the legacy encoder reads the character from
            // the text, which Qt leaves empty or as a control code here, and
            // the Kitty code must stay the layout's letter, so nothing is sent.
            QVERIFY(bytes->isEmpty());
        } else if (protocol == Kitty) {
            QCOMPARE(*bytes, QByteArray("\x1b[%1;%2u")
                                 .replace("%1", QByteArray::number(layoutCode))
                                 .replace("%2", QByteArray::number(modifiers)));
        } else {
            QCOMPARE(*bytes, QByteArray("\x1b[%1::%2;%3u")
                                 .replace("%1", QByteArray::number(layoutCode))
                                 .replace("%2", QByteArray::number(latinCode))
                                 .replace("%3", QByteArray::number(modifiers)));
        }
    }

    void ALatinLetterOfAnAltGrLayoutIsNotRemapped_data() {
        QTest::addColumn<int>("platform");
        QTest::addColumn<quint32>("code");
        QTest::newRow("mac") << int{Mac} << MAC_A;
        QTest::newRow("xkb") << int{Xkb} << XKB_A;
    }

    void ALatinLetterOfAnAltGrLayoutIsNotRemapped() {
        QFETCH(int, platform);
        QFETCH(quint32, code);
        // Polish AltGr+a types "ą" on the physical A key: a Latin letter, so
        // the layout is Latin and the letter stays text.
        const auto event = MapQtKey(LATIN_A_OGONEK, {}, "ą", code,
                                    EKeyAction::Press, Options(platform));
        QVERIFY(event.has_value());
        QCOMPARE(event->Key, EKey::Unidentified);
        QCOMPARE(event->Text, QString("ą"));
        QCOMPARE(static_cast<int>(event->UnshiftedCodepoint), 0x0105);
    }

    void CopyShortcutFollowsThePhysicalKey_data() {
        QTest::addColumn<int>("platform");
        QTest::addColumn<int>("qtKey");
        QTest::addColumn<int>("mods");
        QTest::addColumn<std::optional<quint32>>("code");
        QTest::addColumn<bool>("copy");

        using Code = std::optional<quint32>;
        const int cmd = Qt::ControlModifier;
        const int ctrlShift = Qt::ControlModifier | Qt::ShiftModifier;
        const auto row = [](const char* name, int platform, int qtKey, int mods,
                            Code code, bool copy) {
            QTest::newRow(name) << platform << qtKey << mods << code << copy;
        };

        row("copy_mac_ru", Mac, CYR_S, cmd, MAC_C, true);
        row("copy_linux_ru", Xkb, CYR_S, ctrlShift, XKB_C, true);
        // Latin keys as before, with no code at all.
        row("copy_mac_latin", Mac, Qt::Key_C, cmd, Code(), true);
        row("copy_linux_latin", Xkb, Qt::Key_C, ctrlShift, Code(), true);
        // Plain Ctrl+С on Linux stays the interrupt.
        row("interrupt_linux_ru", Xkb, CYR_S, Qt::ControlModifier, XKB_C,
            false);
        // Another physical key, or none known.
        row("copy_mac_ru_other_key", Mac, CYR_S, cmd, MAC_V, false);
        row("copy_linux_ru_other_key", Xkb, CYR_S, ctrlShift, XKB_V, false);
        row("copy_linux_ru_no_code", Xkb, CYR_S, ctrlShift, Code(), false);
        // A Latin letter other than C at the position of C does not copy.
        row("dvorak_j_at_c", Mac, Qt::Key_J, cmd, MAC_C, false);
    }

    void CopyShortcutFollowsThePhysicalKey() {
        QFETCH(int, platform);
        QFETCH(int, qtKey);
        QFETCH(int, mods);
        QFETCH(std::optional<quint32>, code);
        QFETCH(bool, copy);
        QCOMPARE(IsCopyShortcut(qtKey,
                                Qt::KeyboardModifiers::fromInt(
                                    static_cast<unsigned>(mods)),
                                code, Options(platform)),
                 copy);
    }
};

QTEST_APPLESS_MAIN(QtKeyLayoutTest)
#include "qt_key_layout_test.moc"
