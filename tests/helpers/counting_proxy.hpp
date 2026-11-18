#pragma once

#include <QByteArray>
#include <QLocalServer>
#include <QObject>
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
    int ResizeRequests() const { return ResizeRequests_; }
    int ViewRequests() const { return ViewRequests_; }
    int InputFrames() const { return InputFrames_; }

private:
    void Accept();
    // Takes the whole frames off the front of what the client sent.
    void Count(QByteArray& buffer);

    QLocalServer Server_;
    QString Listen_;
    QString Upstream_;
    int OpenRequests_ = 0;
    int ResizeRequests_ = 0;
    int ViewRequests_ = 0;
    int InputFrames_ = 0;
};

}  // namespace roost::testing
