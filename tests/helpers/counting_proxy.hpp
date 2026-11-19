#pragma once

#include <QByteArray>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include <memory>
#include <utility>

namespace roost::testing {

// A socket between the IO thread and roostd that passes the bytes through and
// counts what the client sends: session.open, session.resize and
// session.view requests and input frames. Runs in the thread that makes it.
class CountingProxy : public QObject {
public:
    CountingProxy(QString listenPath, QString upstreamPath);

    bool Listen() { return Server_.listen(Listen_); }
    const QString& Path() const { return Listen_; }
    int OpenRequests() const { return OpenRequests_; }
    // Of the session.open requests, those that carried columns and rows.
    int SizedOpenRequests() const { return SizedOpenRequests_; }
    int ResizeRequests() const { return ResizeRequests_; }
    int ViewRequests() const { return ViewRequests_; }
    int InputFrames() const { return InputFrames_; }
    // Cuts every connection the proxy has now, as a daemon that is gone would
    // while it is in fact still running; later connections work as usual.
    void DropClients();
    // Holds back the answers to the requests of the method that the clients
    // send from now on, and every frame the daemon sends after such an answer
    // on the same connection, so that the order stays as the daemon made it.
    // A request made before the call is answered as usual. The answers wait
    // until ReleaseAnswers; this makes an in-flight request last as long as
    // the test needs, not as long as the daemon takes.
    void HoldAnswers(QString method);
    // Passes on what is held, and holds nothing more.
    void ReleaseAnswers();

private:
    // What the proxy knows of one connection, for holding answers.
    struct Connection {
        QByteArray FromDaemon;
        // The ids of the held method's requests that have no answer yet.
        QSet<qint64> HeldIds;
        // Whole frames waiting for the release, in the order they came.
        QList<QByteArray> Held;
    };

    void Accept();
    // Takes the whole frames off the front of what the client sent.
    void Count(QByteArray& buffer, Connection& connection);
    // Passes on the whole frames of what the daemon sent, but those held.
    void Forward(QLocalSocket& client, Connection& connection,
                 const QByteArray& bytes);

    QLocalServer Server_;
    QString Listen_;
    QString Upstream_;
    QList<QPointer<QLocalSocket>> Clients_;
    QList<std::pair<QPointer<QLocalSocket>, std::shared_ptr<Connection>>>
        Connections_;
    QString HeldMethod_;
    int OpenRequests_ = 0;
    int SizedOpenRequests_ = 0;
    int ResizeRequests_ = 0;
    int ViewRequests_ = 0;
    int InputFrames_ = 0;
};

}  // namespace roost::testing
