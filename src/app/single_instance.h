// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX app: single-instance guard. The first instance listens on a QLocalServer whose name is
// derived from the profile directory; a second instance started with the same profile forwards its
// links (and an "activate" request) to the first one and exits.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QStringList>

class QLocalServer;

namespace mx::app {

class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(const QString &profileDir, QObject *parent = nullptr);
    // true: another instance received the links -> this process should exit
    bool forwardToRunning(const QStringList &links, int timeoutMs = 1500);
    // become the primary instance; false if listening failed (the app still runs)
    bool listen();
    QString serverName() const { return m_name; }

signals:
    void linksReceived(const QStringList &links);
    void activateRequested();

private:
    QString m_name;
    QLocalServer *m_server = nullptr;
};

} // namespace mx::app
