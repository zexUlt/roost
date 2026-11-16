// What a mouse event encodes to, against a real terminal whose tracking mode
// and format were set by the DECSET sequences a program would send. The
// expected bytes are those of xterm's mouse protocol
// (https://invisible-island.net/xterm/ctlseqs/ctlseqs.html, "Mouse Tracking"),
// written out by hand, not copied from what the library printed.
//
// The grid is 400 columns of 10x20 pixel cells, so that a column past what the
// legacy formats can say exists. A cell (column, row), counted from 0, is hit
// at its middle.

#include "core/vt_mouse_encoder.hpp"
#include "core/vt_terminal.hpp"

#include <QByteArray>
#include <QTest>

#include <utility>

Q_DECLARE_METATYPE(roost::MouseEvent)

using roost::EMouseAction;
using roost::EMouseButton;
using roost::MouseEvent;
using roost::VtMouseEncoder;
using roost::VtTerminal;

namespace {

constexpr int SHIFT = 1;
constexpr int CTRL = 2;
constexpr int ALT = 4;

constexpr quint32 CELL_WIDTH = 10;
constexpr quint32 CELL_HEIGHT = 20;
constexpr quint32 GRID_COLUMNS = 400;
constexpr quint32 GRID_ROWS = 24;

const roost::MouseGeometry GEOMETRY{.ScreenWidth = GRID_COLUMNS * CELL_WIDTH,
                                    .ScreenHeight = GRID_ROWS * CELL_HEIGHT,
                                    .CellWidth = CELL_WIDTH,
                                    .CellHeight = CELL_HEIGHT};

VtTerminal NewTerminal() {
    auto terminal = VtTerminal::Create(80, 24);
    if (!terminal) {
        qFatal("%s", qPrintable(terminal.error()));
    }
    return std::move(*terminal);
}

VtMouseEncoder NewEncoder() {
    auto encoder = VtMouseEncoder::Create();
    if (!encoder) {
        qFatal("%s", qPrintable(encoder.error()));
    }
    return std::move(*encoder);
}

void Sync(VtMouseEncoder& encoder, const VtTerminal& terminal) {
    const auto synced = encoder.SyncFrom(terminal, GEOMETRY);
    if (!synced) {
        qFatal("%s", qPrintable(synced.error()));
    }
}

QByteArray EncodeOne(VtMouseEncoder& encoder, const MouseEvent& event) {
    const auto bytes = encoder.Encode(event);
    if (!bytes) {
        qFatal("%s", qPrintable(bytes.error()));
    }
    return *bytes;
}

// The middle of a cell.
MouseEvent At(EMouseAction action, EMouseButton button, int column, int row,
              int mods = 0) {
    MouseEvent event;
    event.Action = action;
    event.Button = button;
    event.Mods.Shift = (mods & SHIFT) != 0;
    event.Mods.Ctrl = (mods & CTRL) != 0;
    event.Mods.Alt = (mods & ALT) != 0;
    event.X = (static_cast<float>(column) + 0.5F) * CELL_WIDTH;
    event.Y = (static_cast<float>(row) + 0.5F) * CELL_HEIGHT;
    event.AnyButtonPressed =
        action == EMouseAction::Press || button != EMouseButton::None;
    return event;
}

MouseEvent Press(EMouseButton button, int column, int row, int mods = 0) {
    return At(EMouseAction::Press, button, column, row, mods);
}

MouseEvent Release(EMouseButton button, int column, int row, int mods = 0) {
    auto event = At(EMouseAction::Release, button, column, row, mods);
    event.AnyButtonPressed = false;
    return event;
}

// Motion with `button` held, or with none.
MouseEvent Move(EMouseButton button, int column, int row, int mods = 0) {
    return At(EMouseAction::Motion, button, column, row, mods);
}

}  // namespace

class VtMouseEncoderTest : public QObject {
    Q_OBJECT

private:
    // The terminal gets `setup`, the encoder follows it, then one event.
    static QByteArray Encode(const QByteArray& setup, const MouseEvent& event) {
        auto terminal = NewTerminal();
        terminal.Write(setup);
        auto encoder = NewEncoder();
        Sync(encoder, terminal);
        return EncodeOne(encoder, event);
    }

    static void AddRow(const char* name, const QByteArray& setup,
                       const MouseEvent& event, const QByteArray& expected) {
        QTest::newRow(name) << setup << event << expected;
    }

private slots:
    void EncodesMouseEvents_data() {
        QTest::addColumn<QByteArray>("setup");
        QTest::addColumn<MouseEvent>("event");
        QTest::addColumn<QByteArray>("expected");

        const QByteArray x10 = "\x1b[?9h";
        const QByteArray normal = "\x1b[?1000h";
        const QByteArray button = "\x1b[?1002h";
        const QByteArray any = "\x1b[?1003h";
        const QByteArray sgr = "\x1b[?1006h";
        const QByteArray utf8 = "\x1b[?1005h";
        const QByteArray urxvt = "\x1b[?1015h";
        const QByteArray pixels = "\x1b[?1016h";
        using enum EMouseButton;

        // No tracking: nothing, whatever the format.
        AddRow("none_press", "", Press(Left, 4, 2), "");
        AddRow("none_release", "", Release(Left, 4, 2), "");
        AddRow("none_motion", "", Move(None, 4, 2), "");
        AddRow("none_wheel", "", Press(WheelUp, 4, 2), "");
        AddRow("none_sgr_press", sgr, Press(Left, 4, 2), "");

        // X10 mode (9): a press of the three buttons, no modifiers, no
        // release, no wheel.
        AddRow("x10_left", x10, Press(Left, 4, 2), "\x1b[M %#");
        AddRow("x10_middle", x10, Press(Middle, 4, 2), "\x1b[M!%#");
        AddRow("x10_right", x10, Press(Right, 4, 2), "\x1b[M\"%#");
        AddRow("x10_ctrl_ignored", x10, Press(Left, 4, 2, CTRL), "\x1b[M %#");
        AddRow("x10_release", x10, Release(Left, 4, 2), "");
        AddRow("x10_motion", x10, Move(Left, 4, 2), "");
        AddRow("x10_wheel", x10, Press(WheelUp, 4, 2), "");

        // Normal mode (1000): presses and releases. A legacy release does
        // not say which button: it is button 3.
        AddRow("normal_press", normal, Press(Left, 4, 2), "\x1b[M %#");
        AddRow("normal_release", normal, Release(Left, 4, 2), "\x1b[M#%#");
        AddRow("normal_release_right", normal, Release(Right, 4, 2),
               "\x1b[M#%#");
        AddRow("normal_motion", normal, Move(Left, 5, 2), "");
        AddRow("normal_ctrl", normal, Press(Left, 4, 2, CTRL), "\x1b[M0%#");
        AddRow("normal_shift_alt", normal, Press(Left, 4, 2, SHIFT | ALT),
               "\x1b[M,%#");
        AddRow("normal_wheel_up", normal, Press(WheelUp, 4, 2), "\x1b[M`%#");
        AddRow("normal_wheel_down", normal, Press(WheelDown, 4, 2),
               "\x1b[Ma%#");

        // Button-event mode (1002): motion only with a button held; the
        // button's code plus 32.
        AddRow("button_press", button, Press(Left, 4, 2), "\x1b[M %#");
        AddRow("button_drag", button, Move(Left, 5, 2), "\x1b[M@&#");
        AddRow("button_drag_right", button, Move(Right, 5, 2), "\x1b[MB&#");
        AddRow("button_motion_no_button", button, Move(None, 5, 2), "");
        AddRow("button_release", button, Release(Left, 5, 2), "\x1b[M#&#");

        // Any-event mode (1003): motion with no button is button 3 plus 32.
        AddRow("any_motion", any, Move(None, 5, 2), "\x1b[MC&#");
        AddRow("any_drag", any, Move(Left, 5, 2), "\x1b[M@&#");
        AddRow("any_press", any, Press(Left, 4, 2), "\x1b[M %#");

        // SGR (1006): decimal, 1-based, and the release says which button.
        AddRow("sgr_press", normal + sgr, Press(Left, 4, 2), "\x1b[<0;5;3M");
        AddRow("sgr_release", normal + sgr, Release(Left, 4, 2),
               "\x1b[<0;5;3m");
        AddRow("sgr_middle", normal + sgr, Press(Middle, 4, 2), "\x1b[<1;5;3M");
        AddRow("sgr_right", normal + sgr, Press(Right, 4, 2), "\x1b[<2;5;3M");
        AddRow("sgr_right_release", normal + sgr, Release(Right, 4, 2),
               "\x1b[<2;5;3m");
        AddRow("sgr_wheel_up", normal + sgr, Press(WheelUp, 4, 2),
               "\x1b[<64;5;3M");
        AddRow("sgr_wheel_down", normal + sgr, Press(WheelDown, 4, 2),
               "\x1b[<65;5;3M");
        AddRow("sgr_wheel_left", normal + sgr, Press(WheelLeft, 4, 2),
               "\x1b[<66;5;3M");
        AddRow("sgr_wheel_right", normal + sgr, Press(WheelRight, 4, 2),
               "\x1b[<67;5;3M");
        AddRow("sgr_shift", normal + sgr, Press(Left, 4, 2, SHIFT),
               "\x1b[<4;5;3M");
        AddRow("sgr_alt", normal + sgr, Press(Left, 4, 2, ALT), "\x1b[<8;5;3M");
        AddRow("sgr_ctrl", normal + sgr, Press(Left, 4, 2, CTRL),
               "\x1b[<16;5;3M");
        AddRow("sgr_ctrl_shift_wheel_up", normal + sgr,
               Press(WheelUp, 4, 2, CTRL | SHIFT), "\x1b[<84;5;3M");
        AddRow("sgr_alt_wheel_down", normal + sgr, Press(WheelDown, 4, 2, ALT),
               "\x1b[<73;5;3M");
        AddRow("sgr_drag", button + sgr, Move(Left, 5, 2), "\x1b[<32;6;3M");
        AddRow("sgr_motion", any + sgr, Move(None, 5, 2), "\x1b[<35;6;3M");
        AddRow("sgr_drag_ctrl", button + sgr, Move(Left, 5, 2, CTRL),
               "\x1b[<48;6;3M");

        // Other formats.
        AddRow("urxvt_press", normal + urxvt, Press(Left, 4, 2),
               "\x1b[32;5;3M");
        AddRow("urxvt_release", normal + urxvt, Release(Left, 4, 2),
               "\x1b[35;5;3M");
        AddRow("utf8_press", normal + utf8, Press(Left, 4, 2), "\x1b[M %#");
        AddRow("pixels_press", normal + pixels, Press(Left, 4, 2),
               "\x1b[<0;45;50M");
        AddRow("pixels_release", normal + pixels, Release(Left, 4, 2),
               "\x1b[<0;45;50m");

        // Past column 223 the legacy format has no byte for the position
        // (32 + 1 + column must fit in one), so it sends nothing rather than
        // a wrong place; the others say it.
        AddRow("legacy_column_222", normal, Press(Left, 222, 2),
               QByteArray("\x1b[M \xff#", 6));
        AddRow("legacy_column_223", normal, Press(Left, 223, 2), "");
        AddRow("legacy_column_299", normal, Press(Left, 299, 2), "");
        AddRow("legacy_row_223", normal, Press(Left, 4, 223), "");
        AddRow("sgr_column_223", normal + sgr, Press(Left, 223, 2),
               "\x1b[<0;224;3M");
        AddRow("sgr_column_299", normal + sgr, Press(Left, 299, 2),
               "\x1b[<0;300;3M");
        AddRow("urxvt_column_299", normal + urxvt, Press(Left, 299, 2),
               "\x1b[32;300;3M");
        // UTF-8: column 299 is U+014C (299 + 33), two bytes.
        AddRow("utf8_column_299", normal + utf8, Press(Left, 299, 2),
               "\x1b[M \xc5\x8c#");
        AddRow("pixels_column_299", normal + pixels, Press(Left, 299, 2),
               "\x1b[<0;2995;50M");
    }

    void EncodesMouseEvents() {
        QFETCH(QByteArray, setup);
        QFETCH(MouseEvent, event);
        QFETCH(QByteArray, expected);
        QCOMPARE(Encode(setup, event), expected);
    }

    void TheLastSyncWins() {
        // The program turns tracking on, and then off: the encoder follows
        // each sync, and in between it keeps the copy it took.
        auto terminal = NewTerminal();
        auto encoder = NewEncoder();
        const auto click = Press(EMouseButton::Left, 4, 2);

        Sync(encoder, terminal);
        QCOMPARE(EncodeOne(encoder, click), QByteArray());

        terminal.Write("\x1b[?1000h");
        QCOMPARE(EncodeOne(encoder, click), QByteArray());
        Sync(encoder, terminal);
        QCOMPARE(EncodeOne(encoder, click), QByteArray("\x1b[M %#"));

        terminal.Write("\x1b[?1000l");
        QCOMPARE(EncodeOne(encoder, click), QByteArray("\x1b[M %#"));
        Sync(encoder, terminal);
        QCOMPARE(EncodeOne(encoder, click), QByteArray());
    }

    void ModeChangesFollowTheProgram() {
        auto terminal = NewTerminal();
        auto encoder = NewEncoder();
        const auto motion = Move(EMouseButton::None, 5, 2);

        terminal.Write("\x1b[?1000h");
        Sync(encoder, terminal);
        QCOMPARE(EncodeOne(encoder, motion), QByteArray());
        terminal.Write("\x1b[?1003h\x1b[?1006h");
        Sync(encoder, terminal);
        QCOMPARE(EncodeOne(encoder, motion), QByteArray("\x1b[<35;6;3M"));
    }

    void AMotionThatReportsTheSameIsDropped() {
        auto terminal = NewTerminal();
        auto encoder = NewEncoder();
        terminal.Write("\x1b[?1003h\x1b[?1006h");

        // A sync comes before every event, as in the window.
        const auto send = [&](const MouseEvent& event) {
            Sync(encoder, terminal);
            return EncodeOne(encoder, event);
        };
        auto inCell = Move(EMouseButton::None, 5, 2);
        QCOMPARE(send(inCell), QByteArray("\x1b[<35;6;3M"));
        // Another pixel of the same cell.
        inCell.X += 3;
        QCOMPARE(send(inCell), QByteArray());
        QCOMPARE(send(Move(EMouseButton::None, 6, 2)),
                 QByteArray("\x1b[<35;7;3M"));
        // A click between two moves makes the second one news again.
        QCOMPARE(send(Press(EMouseButton::Left, 6, 2)),
                 QByteArray("\x1b[<0;7;3M"));
        QCOMPARE(send(Move(EMouseButton::None, 6, 2)),
                 QByteArray("\x1b[<35;7;3M"));
    }

    void AWheelNotchDoesNotMakeTheSameMotionNews() {
        auto terminal = NewTerminal();
        auto encoder = NewEncoder();
        terminal.Write("\x1b[?1003h\x1b[?1006h");
        const auto send = [&](const MouseEvent& event) {
            Sync(encoder, terminal);
            return EncodeOne(encoder, event);
        };
        QCOMPARE(send(Move(EMouseButton::None, 5, 2)),
                 QByteArray("\x1b[<35;6;3M"));
        QCOMPARE(send(Press(EMouseButton::WheelUp, 5, 2)),
                 QByteArray("\x1b[<64;6;3M"));
        QCOMPARE(send(Press(EMouseButton::WheelRight, 5, 2)),
                 QByteArray("\x1b[<67;6;3M"));
        QCOMPARE(send(Move(EMouseButton::None, 5, 2)), QByteArray());
    }

    void AWheelOutsideTheGridNeedsAHeldButtonInMotionModes() {
        auto terminal = NewTerminal();
        auto encoder = NewEncoder();
        terminal.Write("\x1b[?1003h\x1b[?1006h");
        Sync(encoder, terminal);

        auto wheel = Press(EMouseButton::WheelUp, 5, 2);
        wheel.X = -5;
        wheel.AnyButtonPressed = false;
        QCOMPARE(EncodeOne(encoder, wheel), QByteArray());
        wheel.AnyButtonPressed = true;
        QCOMPARE(EncodeOne(encoder, wheel), QByteArray("\x1b[<64;1;3M"));

        // Normal mode has no motion: outside the grid nothing is reported.
        terminal.Write("\x1b[?1003l\x1b[?1000h");
        Sync(encoder, terminal);
        wheel.AnyButtonPressed = false;
        QCOMPARE(EncodeOne(encoder, wheel), QByteArray());
    }

    void SgrPixelsReportsEveryMoveInsideACell() {
        auto terminal = NewTerminal();
        auto encoder = NewEncoder();
        terminal.Write("\x1b[?1003h\x1b[?1016h");
        Sync(encoder, terminal);

        auto event = Move(EMouseButton::None, 5, 2);
        QCOMPARE(EncodeOne(encoder, event), QByteArray("\x1b[<35;55;50M"));
        event.X += 3;
        QCOMPARE(EncodeOne(encoder, event), QByteArray("\x1b[<35;58;50M"));
    }

    void MotionOutsideTheGridNeedsAHeldButton() {
        auto terminal = NewTerminal();
        auto encoder = NewEncoder();
        terminal.Write("\x1b[?1003h\x1b[?1006h");
        Sync(encoder, terminal);

        auto outside = Move(EMouseButton::None, 5, 2);
        outside.X = -5;
        QCOMPARE(EncodeOne(encoder, outside), QByteArray());

        // Dragged out of the grid with a button down: reported, at the edge.
        auto dragged = Move(EMouseButton::Left, 5, 2);
        dragged.X = -5;
        QCOMPARE(EncodeOne(encoder, dragged), QByteArray("\x1b[<32;1;3M"));

        // A release is reported wherever it happens.
        auto released = Release(EMouseButton::Left, 5, 2);
        released.X = -5;
        QCOMPARE(EncodeOne(encoder, released), QByteArray("\x1b[<0;1;3m"));
    }

    void AnEmptyCellIsRefused() {
        auto terminal = NewTerminal();
        auto encoder = NewEncoder();
        auto geometry = GEOMETRY;
        geometry.CellWidth = 0;
        QVERIFY(!encoder.SyncFrom(terminal, geometry));
    }
};

QTEST_APPLESS_MAIN(VtMouseEncoderTest)
#include "vt_mouse_encoder_test.moc"
