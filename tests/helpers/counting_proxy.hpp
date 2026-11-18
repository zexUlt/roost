#pragma once

#include <QByteArray>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <QString>

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

private:
    void Accept();
    // Takes the whole frames off the front of what the client sent.
    void Count(QByteArray& buffer);

    QLocalServer Server_;
    QString Listen_;
    QString Upstream_;
    QList<QPointer<QLocalSocket>> Clients_;
    int OpenRequests_ = 0;
    int SizedOpenRequests_ = 0;
    int ResizeRequests_ = 0;
    int ViewRequests_ = 0;
    int InputFrames_ = 0;
};

}  // namespace roost::testing
