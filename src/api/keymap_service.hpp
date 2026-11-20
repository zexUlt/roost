#pragma once

#include <QFuture>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <cstdint>
#include <optional>

#include "api/bus_service.hpp"
#include "core/key_chord.hpp"
#include "core/rpc.hpp"
#include "terminal/io_thread.hpp"

namespace roost::api {

// The form in which a chord is compared with a pressed key (design.md §16,
// "Клавиша"). Shift with a symbol or digit key is part of the symbol the key
// types on the US layout: Ctrl+Shift+= and Ctrl++ are one physical key and one
// form, Ctrl++; so are Cmd+Shift+4 and Cmd+$. Shift with a letter, a name or
// a function key stays a modifier.
KeyChord MatchForm(KeyChord chord);

// One entry of the table: what a pressed key runs.
struct KeyBinding {
    // In MatchForm.
    KeyChord Chord;
    // The type of panel the key is for; empty for any panel.
    QString When;
    QString Command;
    QJsonValue Args;
    // "user" for keymap.json, else the id of the plugin.
    QString Source;
};

// The keymap table for the window's own platform (keymap.get and
// keymap.changed, design.md §4, §16), cached. Not the daemon's
// roost::KeymapService, which puts the table together: this one is the
// window's copy. Use it from the GUI thread; the signal is emitted there.
//
// Start (Api calls it after each hello) subscribes to keymap.changed and asks
// keymap.get {platform}; every keymap.changed of the window's platform
// replaces the table whole. The answer and the events come in the order that
// roostd made them (see StoreService for why the last received wins), and the
// subscription is made before the request on the same socket. When the
// connection is lost the table is dropped and the subscription with it: no
// key runs a command of a roostd that is gone, and Start begins again after
// the reconnect.
class KeymapService : public QObject {
    Q_OBJECT

public:
    KeymapService(IoThread& io, BusService& bus, QObject* parent = nullptr);

    // The connection is up: from now on the table is kept fresh. A second
    // call with no loss in between does nothing.
    void Start();

    // keymap.get as the dispatcher answers it: {bindings, problems} of the
    // window's platform, from the cache once it is loaded; otherwise asked of
    // roostd. roostd's errors pass unchanged and keep nothing. The future
    // finishes in the IO thread when roostd is asked, so attach a
    // continuation with a context.
    QFuture<rpc::Result> Get();

    bool IsLoaded() const;

    // The binding of the pair (chord, when) or null; `chord` is in MatchForm
    // (a pressed key is turned into it by the matcher). An empty `when` is the
    // pair without one. The pointer is good until the next signal Changed.
    const KeyBinding* Find(const KeyChord& chord, const QString& when) const;

    // The platform's name in the protocol: "mac" or "linux".
    static QString PlatformName();

signals:
    // The table was replaced, by the answer of roostd or by an event.
    void Changed();

private:
    void Subscribe();
    QFuture<rpc::Result> Fetch();
    void Load(const QJsonObject& table);
    void OnChanged(const QJsonValue& payload);
    void OnDisconnected();

    IoThread& Io_;
    BusService& Bus_;
    // {bindings, problems} as roostd said them; null object while not loaded.
    QJsonObject Table_;
    bool Loaded_ = false;
    // By (chord in MatchForm, when).
    QHash<QString, KeyBinding> Index_;
    bool Started_ = false;
    // Counts the connections lost; an answer of an older one is not ours.
    std::uint64_t Epoch_ = 0;
    // Zero while not subscribed to keymap.changed.
    BusService::LocalId Subscription_ = 0;
};

}  // namespace roost::api
