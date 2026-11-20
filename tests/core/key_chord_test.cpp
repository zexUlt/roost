// Key chords: modifiers and case are normalized, Cmd and Ctrl stay the
// physical keys they name, `mac` is chosen on macOS only, and a chord that
// cannot apply is refused with a message. Both platforms are covered on any
// host, as the platform is an argument.

#include <QTest>

#include <optional>

#include "core/key_chord.hpp"

using namespace roost;

namespace {

constexpr EPlatform MAC = EPlatform::Mac;
constexpr EPlatform LINUX = EPlatform::Linux;

}  // namespace

class KeyChordTest : public QObject {
    Q_OBJECT

private slots:
    // The input and what Write prints for it.
    void ParsesToTheNormalForm_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<EPlatform>("platform");
        QTest::addColumn<QString>("normal");

        QTest::newRow("one letter") << "j" << LINUX << "J";
        QTest::newRow("upper letter stays") << "J" << LINUX << "J";
        QTest::newRow("shift before ctrl")
            << "Shift+Ctrl+K" << LINUX << "Ctrl+Shift+K";
        QTest::newRow("ctrl before shift")
            << "Ctrl+Shift+K" << LINUX << "Ctrl+Shift+K";
        QTest::newRow("case of modifiers")
            << "cTRL+sHIFT+k" << LINUX << "Ctrl+Shift+K";
        QTest::newRow("all four, any order")
            << "Shift+Alt+Ctrl+Cmd+K" << MAC << "Cmd+Ctrl+Alt+Shift+K";
        QTest::newRow("cmd on mac") << "cmd+n" << MAC << "Cmd+N";
        QTest::newRow("spaces around parts")
            << " Ctrl + Alt + Left " << LINUX << "Ctrl+Alt+Left";
        QTest::newRow("named key case") << "alt+LEFT" << LINUX << "Alt+Left";
        QTest::newRow("page up") << "ctrl+pageup" << LINUX << "Ctrl+PageUp";
        QTest::newRow("alias Esc") << "esc" << LINUX << "Escape";
        QTest::newRow("alias Return") << "Ctrl+return" << LINUX << "Ctrl+Enter";
        QTest::newRow("function key") << "f5" << LINUX << "F5";
        QTest::newRow("last function key")
            << "Shift+f24" << LINUX << "Shift+F24";
        QTest::newRow("backslash") << "Ctrl+\\" << LINUX << "Ctrl+\\";
        QTest::newRow("a digit") << "Cmd+1" << MAC << "Cmd+1";
        QTest::newRow("plus is a key") << "Ctrl++" << LINUX << "Ctrl++";
        QTest::newRow("bare plus") << "+" << LINUX << "+";
        QTest::newRow("a letter of another script")
            << "Ctrl+ж" << LINUX << "Ctrl+Ж";
    }

    void ParsesToTheNormalForm() {
        QFETCH(QString, text);
        QFETCH(EPlatform, platform);
        QFETCH(QString, normal);
        const auto chord = ParseKeyChord(text, platform);
        QVERIFY2(chord, qPrintable(chord ? QString() : chord.error()));
        QCOMPARE(WriteKeyChord(*chord), normal);
        // Round trip: the normal form parses back to the same chord.
        const auto again = ParseKeyChord(normal, platform);
        QVERIFY(again);
        QCOMPARE(*again, *chord);
        QCOMPARE(WriteKeyChord(*again), normal);
    }

    // Cmd is the Command key and Ctrl the Control key, apart from each other.
    void CmdAndCtrlAreDifferentKeys() {
        const auto cmd = ParseKeyChord("Cmd+N", MAC);
        const auto ctrl = ParseKeyChord("Ctrl+N", MAC);
        QVERIFY(cmd && ctrl);
        QVERIFY(cmd->Cmd && !cmd->Ctrl);
        QVERIFY(ctrl->Ctrl && !ctrl->Cmd);
        QVERIFY(*cmd != *ctrl);
        QCOMPARE(WriteKeyChord(*cmd), QString("Cmd+N"));
        QCOMPARE(WriteKeyChord(*ctrl), QString("Ctrl+N"));
    }

    void SameChordsCompareEqual() {
        QCOMPARE(*ParseKeyChord("Shift+Ctrl+K", LINUX),
                 *ParseKeyChord("ctrl+shift+k", LINUX));
        QVERIFY(*ParseKeyChord("Ctrl+K", LINUX) !=
                *ParseKeyChord("Ctrl+Shift+K", LINUX));
        QVERIFY(*ParseKeyChord("Ctrl+K", LINUX) !=
                *ParseKeyChord("Alt+K", LINUX));
    }

    void RefusesWhatDoesNotParse_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<EPlatform>("platform");
        QTest::addColumn<QString>("message");

        QTest::newRow("empty") << "" << LINUX << "the key is empty";
        QTest::newRow("blank") << "   " << LINUX << "the key is empty";
        QTest::newRow("Cmd for Linux")
            << "Cmd+N" << LINUX << "Cmd works on macOS only";
        QTest::newRow("Cmd for Linux, any order")
            << "Shift+Cmd+N" << LINUX << "write it in \"mac\"";
        QTest::newRow("unknown modifier")
            << "Hyper+N" << MAC << "unknown modifier \"Hyper\"";
        QTest::newRow("Super is not a modifier")
            << "Super+N" << LINUX << "unknown modifier \"Super\"";
        QTest::newRow("unknown key name")
            << "Ctrl+Foo" << LINUX << "unknown key name \"Foo\"";
        QTest::newRow("F0") << "F0" << LINUX << "unknown key name";
        QTest::newRow("F25") << "F25" << LINUX << "unknown key name";
        QTest::newRow("F05") << "F05" << LINUX << "unknown key name";
        QTest::newRow("empty part in the middle")
            << "Ctrl++Shift+K" << LINUX << "empty part";
        QTest::newRow("empty key after modifier")
            << "Ctrl+" << LINUX << "empty part";
        QTest::newRow("empty modifier") << "+Ctrl+K" << LINUX << "empty part";
        QTest::newRow("only plus signs") << "++" << LINUX << "empty part";
        QTest::newRow("modifier only")
            << "Ctrl+Shift" << LINUX << "no key after the modifiers";
        QTest::newRow("repeated modifier")
            << "Ctrl+Ctrl+K" << LINUX << "is repeated";
        QTest::newRow("sequence is not a chord")
            << "Ctrl+K Ctrl+S" << LINUX << "unknown modifier";
    }

    void RefusesWhatDoesNotParse() {
        QFETCH(QString, text);
        QFETCH(EPlatform, platform);
        QFETCH(QString, message);
        const auto chord = ParseKeyChord(text, platform);
        QVERIFY2(!chord, qPrintable(chord ? WriteKeyChord(*chord) : QString()));
        QVERIFY2(chord.error().contains(message), qPrintable(chord.error()));
    }

    // What a keybinding with `key` and `mac` is on each platform.
    void ChoosesByPlatform_data() {
        QTest::addColumn<std::optional<QString>>("key");
        QTest::addColumn<std::optional<QString>>("mac");
        QTest::addColumn<EPlatform>("platform");
        QTest::addColumn<QString>("chosen");  // null: does not apply

        using Text = std::optional<QString>;
        const Text none;
        QTest::newRow("key only, mac")
            << Text("Ctrl+N") << none << MAC << "Ctrl+N";
        QTest::newRow("key only, linux")
            << Text("Ctrl+N") << none << LINUX << "Ctrl+N";
        QTest::newRow("both, mac takes mac")
            << Text("Ctrl+Shift+N") << Text("Cmd+N") << MAC << "Cmd+N";
        QTest::newRow("both, linux takes key")
            << Text("Ctrl+Shift+N") << Text("Cmd+N") << LINUX << "Ctrl+Shift+N";
        QTest::newRow("mac only, mac")
            << none << Text("Cmd+W") << MAC << "Cmd+W";
        QTest::newRow("mac only, linux does not apply")
            << none << Text("Cmd+W") << LINUX << QString();
    }

    void ChoosesByPlatform() {
        QFETCH(std::optional<QString>, key);
        QFETCH(std::optional<QString>, mac);
        QFETCH(EPlatform, platform);
        QFETCH(QString, chosen);
        const auto chord = ChooseKeyChord(key, mac, platform);
        QVERIFY2(chord, qPrintable(chord ? QString() : chord.error()));
        if (chosen.isNull()) {
            QVERIFY(!*chord);
        } else {
            QVERIFY(*chord);
            QCOMPARE(WriteKeyChord(**chord), chosen);
        }
    }

    void RefusesWhatDoesNotApplyAnywhere_data() {
        QTest::addColumn<std::optional<QString>>("key");
        QTest::addColumn<std::optional<QString>>("mac");
        QTest::addColumn<EPlatform>("platform");
        QTest::addColumn<QString>("message");

        using Text = std::optional<QString>;
        const Text none;
        QTest::newRow("neither")
            << none << none << MAC << "needs \"key\" or \"mac\"";
        // key applies on Linux whatever the platform that reads it.
        QTest::newRow("Cmd in key, read on mac")
            << Text("Cmd+N") << none << MAC << "\"key\": Cmd works on macOS";
        QTest::newRow("Cmd in key next to a mac")
            << Text("Cmd+N") << Text("Cmd+N") << MAC
            << "\"key\": Cmd works on macOS";
        // Both fields are checked, so a shared file reads the same on both.
        QTest::newRow("broken mac, read on linux")
            << Text("Ctrl+N") << Text("Foo+N") << LINUX
            << "\"mac\": unknown modifier \"Foo\"";
        QTest::newRow("broken key, read on mac")
            << Text("Foo+N") << Text("Cmd+N") << MAC
            << "\"key\": unknown modifier \"Foo\"";
        QTest::newRow("empty key")
            << Text("") << none << LINUX << "\"key\": the key is empty";
    }

    void RefusesWhatDoesNotApplyAnywhere() {
        QFETCH(std::optional<QString>, key);
        QFETCH(std::optional<QString>, mac);
        QFETCH(EPlatform, platform);
        QFETCH(QString, message);
        const auto chord = ChooseKeyChord(key, mac, platform);
        QVERIFY(!chord);
        QVERIFY2(chord.error().contains(message), qPrintable(chord.error()));
    }

    void HostPlatformIsOneOfTheTwo() {
#ifdef Q_OS_MACOS
        QCOMPARE(HostPlatform(), MAC);
#else
        QCOMPARE(HostPlatform(), LINUX);
#endif
    }
};

QTEST_GUILESS_MAIN(KeyChordTest)
#include "key_chord_test.moc"
