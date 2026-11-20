#pragma once

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QString>
#include <QVariantList>

namespace roost {

// The "Report a Bug…" dialog's side in C++ (BugReportDialog.qml), a type of
// the module Roost.Shell. It is the window's own: plugins do not see it, and
// it is not part of the plugin API. Everything the report needs is found
// here: the logs next to the socket, the store of ConfigDirectory(). GUI
// thread only.
//
// The names of the methods are QML's (camelCase), as in src/qml_api.
// NOLINTBEGIN(readability-identifier-naming)
class BugReport : public QObject {
    Q_OBJECT
    QML_ELEMENT
    // Where the zip goes; empty: the Downloads directory, else the home
    // directory. A test sets it.
    Q_PROPERTY(QString outputDirectory MEMBER OutputDirectory_ NOTIFY
                   outputDirectoryChanged)
    // The window whose graphics API goes into about.txt; optional.
    Q_PROPERTY(
        QQuickWindow* window READ window WRITE setWindow NOTIFY windowChanged)
    // The zip of the last create(), empty until it succeeded.
    Q_PROPERTY(QString zipPath READ zipPath NOTIFY zipPathChanged)
    // Why the last create() failed; empty if it did not.
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    explicit BugReport(QObject* parent = nullptr);

    QQuickWindow* window() const { return Window_; }
    void setWindow(QQuickWindow* window) {
        if (Window_ != window) {
            Window_ = window;
            emit windowChanged();
        }
    }
    QString zipPath() const { return ZipPath_; }
    QString error() const { return Error_; }

    // The files that go in, apart from about.txt and description.txt:
    // [{name, size, text}], `text` being "logs/roostd.log (12.3 KiB)".
    Q_INVOKABLE QVariantList files(bool includeSettings) const;
    // Writes the zip; false and `error` if that failed.
    Q_INVOKABLE bool create(const QString& description, bool includeSettings);
    // Forgets the error of the last create(); a reopened dialog starts clean.
    Q_INVOKABLE void clearError();
    // Reveals the zip in the file manager. Only on a call from the user.
    Q_INVOKABLE void showInFolder() const;
    // Opens the new-issue page of GitHub in the browser, the description
    // as the body, cut to fit the URL. Only on a call from the user.
    Q_INVOKABLE void openIssue(const QString& description) const;
    // The URL openIssue opens.
    Q_INVOKABLE QString issueUrl(const QString& description) const;

signals:
    void outputDirectoryChanged();
    void windowChanged();
    void zipPathChanged();
    void errorChanged();

private:
    QString OutputDirectory_;
    QPointer<QQuickWindow> Window_;
    QString ZipPath_;
    QString Error_;
};
// NOLINTEND(readability-identifier-naming)

}  // namespace roost
