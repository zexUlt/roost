#pragma once

#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QString>

#include <map>

#include "core/frame.hpp"

namespace roost::testing {

// A roostd of another build: it answers every hello with VersionMismatch, as
// the real one does for a client of another protocol (src/roostd/server.cpp),
// versions in the error `data` included, and counts the hellos it got.
class RefusingServer : public QObject {
    Q_OBJECT

public:
    explicit RefusingServer(QObject* parent = nullptr);

    bool Listen(const QString& path);

    // The hellos answered so far, over all connections.
    int Hellos() const { return Hellos_; }

    // The versions it claims, as they are in the error's data.
    static constexpr int PROTOCOL = 0;
    static constexpr const char* GHOSTTY_VT = "0.0.0-refusing";

private:
    void OnConnection();
    void OnReadyRead(QLocalSocket* socket);

    QLocalServer Server_;
    // One decoder per connection.
    std::map<QLocalSocket*, FrameDecoder> Decoders_;
    int Hellos_ = 0;
};

}  // namespace roost::testing
