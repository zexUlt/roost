#include "roost/key_matcher.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QQuickWindow>
#include <QtLogging>

#include <array>

#include "qml_api/terminal_item.hpp"
#include "terminal/physical_keys.hpp"
#include "terminal/qt_key_mapping.hpp"

namespace roost {

namespace {

const QString PANEL_SLOT = QStringLiteral("panelSlot");
// The class of the item that Popup and Dialog live in (Qt Quick Templates);
// the class is private, so it is found by name.
const char* const OVERLAY_CLASS = "QQuickOverlay";

struct NamedKey {
    int Qt;
    const char* Name;
};

// The keys of the table that are not one character, by the canonical name of
// ParseKeyChord. Return and the keypad Enter are both Enter.
constexpr std::array NAMED_KEYS = {
    NamedKey{Qt::Key_Left, "Left"},
    NamedKey{Qt::Key_Right, "Right"},
    NamedKey{Qt::Key_Up, "Up"},
    NamedKey{Qt::Key_Down, "Down"},
    NamedKey{Qt::Key_Home, "Home"},
    NamedKey{Qt::Key_End, "End"},
    NamedKey{Qt::Key_PageUp, "PageUp"},
    NamedKey{Qt::Key_PageDown, "PageDown"},
    NamedKey{Qt::Key_Insert, "Insert"},
    NamedKey{Qt::Key_Delete, "Delete"},
    NamedKey{Qt::Key_Backspace, "Backspace"},
    NamedKey{Qt::Key_Return, "Enter"},
    NamedKey{Qt::Key_Enter, "Enter"},
    NamedKey{Qt::Key_Tab, "Tab"},
    NamedKey{Qt::Key_Space, "Space"},
    NamedKey{Qt::Key_Escape, "Escape"},
};

constexpr int LAST_FUNCTION_KEY = 24;
// Qt::Key values from here are named keys; below are the Unicode characters.
constexpr int FIRST_SPECIAL_KEY = 0x01000000;

// The key part of a chord for a Qt key, or null for a key the table has no
// name for.
QString NameOf(int qtKey, bool& shiftFromBacktab) {
    if (qtKey == Qt::Key_Backtab) {
        // Shift+Tab: Qt reports the key as Backtab.
        shiftFromBacktab = true;
        return QStringLiteral("Tab");
    }
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F1 + LAST_FUNCTION_KEY - 1) {
        return QStringLiteral("F%1").arg(qtKey - Qt::Key_F1 + 1);
    }
    for (const NamedKey& named : NAMED_KEYS) {
        if (named.Qt == qtKey) {
            return QString::fromLatin1(named.Name);
        }
    }
    if (qtKey > Qt::Key_Space && qtKey < FIRST_SPECIAL_KEY) {
        const char32_t code = static_cast<char32_t>(qtKey);
        if (QChar::isPrint(code)) {
            return QString::fromUcs4(&code, 1).toUpper();
        }
    }
    return {};
}

}  // namespace

KeyMatcher::KeyMatcher(api::KeymapService& keymap,
                       api::CommandService& commands, QObject* parent)
    : QObject(parent), Keymap_(keymap), Commands_(commands) {
    if (QCoreApplication* const app = QCoreApplication::instance()) {
        app->installEventFilter(this);
    }
    if (QGuiApplication* const gui = qGuiApp) {
        // A key pressed in one focus span is not released in the next, and an
        // input method that is cancelled by moving the focus says nothing.
        connect(gui, &QGuiApplication::focusWindowChanged, this,
                [this] { Held_.clear(); });
        connect(gui, &QGuiApplication::focusObjectChanged, this,
                [this] { Composing_ = false; });
    }
}

KeyMatcher::~KeyMatcher() {
    if (QCoreApplication* const app = QCoreApplication::instance()) {
        app->removeEventFilter(this);
    }
}

std::optional<KeyChord> KeyMatcher::ChordOf(const QKeyEvent& event) {
    const ENativeKeyCodes codes = QtKeyOptions::ForThisPlatform().NativeKeys;
    const EKey physical = PhysicalKey(codes, NativeKeyCode(event, codes));
    const Qt::KeyboardModifiers modifiers = event.modifiers();
    const int qtKey = BindingKey(event.key(), modifiers, physical);
    bool shiftFromBacktab = false;
    QString name = NameOf(qtKey, shiftFromBacktab);
    if (name.isEmpty()) {
        return std::nullopt;
    }
    KeyChord chord;
    chord.Key = std::move(name);
    chord.Shift = modifiers.testFlag(Qt::ShiftModifier) || shiftFromBacktab;
    chord.Alt = modifiers.testFlag(Qt::AltModifier);
    if (HostPlatform() == EPlatform::Mac) {
        // Qt swaps the two on macOS: its Control is Command, its Meta is the
        // Control key.
        chord.Cmd = modifiers.testFlag(Qt::ControlModifier);
        chord.Ctrl = modifiers.testFlag(Qt::MetaModifier);
    } else {
        if (modifiers.testFlag(Qt::MetaModifier)) {
            return std::nullopt;
        }
        chord.Ctrl = modifiers.testFlag(Qt::ControlModifier);
    }
    return api::MatchForm(std::move(chord));
}

QString KeyMatcher::PanelTypeOf(const QQuickItem* focus) {
    for (const QQuickItem* item = focus; item != nullptr;
         item = item->parentItem()) {
        if (item->objectName() == PANEL_SLOT) {
            return item->property("panelType").toString();
        }
    }
    return {};
}

bool KeyMatcher::InOverlay(const QQuickItem* focus) {
    for (const QQuickItem* item = focus; item != nullptr;
         item = item->parentItem()) {
        if (item->inherits(OVERLAY_CLASS)) {
            return true;
        }
    }
    return false;
}

bool KeyMatcher::TakesText(const QQuickItem* focus) {
    return focus != nullptr &&
           (qobject_cast<const TerminalItem*>(focus) != nullptr ||
            focus->flags().testFlag(QQuickItem::ItemAcceptsInputMethod));
}

bool KeyMatcher::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
        case QEvent::InputMethod:
            Composing_ = !static_cast<const QInputMethodEvent&>(*event)
                              .preeditString()
                              .isEmpty();
            return false;
        case QEvent::KeyPress:
            return OnPress(watched, static_cast<const QKeyEvent&>(*event));
        case QEvent::KeyRelease:
            // The release of a key that was caught goes nowhere. Only the
            // window's copy of the event is looked at: the filter sees one
            // event for each receiver it is delivered to.
            return qobject_cast<QQuickWindow*>(watched) != nullptr &&
                   Held_.remove(static_cast<const QKeyEvent&>(*event).key());
        default:
            return false;
    }
}

const api::KeyBinding* KeyMatcher::Usable(
    const api::KeyBinding* binding) const {
    // A command of the built-in `shell` plugin has no backend: with no handler
    // in the window the request would only come back as -32003, and the key
    // is not ours (a terminal copies on its own, IsCopyShortcut).
    if (binding != nullptr && binding->Source == QStringLiteral("shell") &&
        !Commands_.HasLocal(binding->Command)) {
        return nullptr;
    }
    return binding;
}

bool KeyMatcher::OnPress(QObject* watched, const QKeyEvent& event) {
    auto* const window = qobject_cast<QQuickWindow*>(watched);
    if (window == nullptr || Composing_ || !Keymap_.IsLoaded()) {
        return false;
    }
    const std::optional<KeyChord> chord = ChordOf(event);
    if (!chord) {
        return false;
    }
    const QQuickItem* const focus = window->activeFocusItem();
    if (InOverlay(focus)) {
        return false;
    }
    if (IsBareKey(*chord) && TakesText(focus)) {
        return false;
    }
    const QString panel = PanelTypeOf(focus);
    const api::KeyBinding* binding =
        panel.isEmpty() ? nullptr : Usable(Keymap_.Find(*chord, panel));
    if (binding == nullptr) {
        binding = Usable(Keymap_.Find(*chord, QString()));
    }
    if (binding == nullptr) {
        return false;
    }
    Held_.insert(event.key());
    if (event.isAutoRepeat()) {
        return true;
    }
    const QString command = binding->Command;
    Commands_.Run(command, binding->Args)
        .then(this, [command](rpc::Result answer) {
            if (!answer) {
                qWarning("keymap: command %s failed: %s",
                         qUtf8Printable(command),
                         qUtf8Printable(answer.error().Message));
            }
        });
    return true;
}

}  // namespace roost
