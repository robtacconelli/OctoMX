// SPDX-License-Identifier: GPL-3.0-or-later
#include "single_instance.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

namespace mx::app {

SingleInstance::SingleInstance(const QString &profileDir, QObject *parent) : QObject(parent)
{
    const QString canon = QDir(profileDir).absolutePath();
    const QByteArray h = QCryptographicHash::hash(canon.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    // the server name prefix is an identifier kept for compatibility (byte for byte), so that copies
    // of every version on the same profile folder find each other (one program per profile)
    m_name = QStringLiteral("winmx-ng-") + QString::fromLatin1(h);
}

bool SingleInstance::forwardToRunning(const QStringList &links, int timeoutMs)
{
    QLocalSocket s;
    s.connectToServer(m_name);
    if (!s.waitForConnected(timeoutMs))
        return false;
    QJsonObject o;
    o.insert(QStringLiteral("links"), QJsonArray::fromStringList(links));
    o.insert(QStringLiteral("activate"), true);
    s.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
    s.flush();
    s.waitForBytesWritten(timeoutMs);
    s.waitForReadyRead(timeoutMs);   // the server answers "ok"
    s.disconnectFromServer();
    return true;
}

bool SingleInstance::listen()
{
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_name)) {
        // stale socket of a crashed instance
        QLocalServer::removeServer(m_name);
        if (!m_server->listen(m_name))
            return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *c = m_server->nextPendingConnection()) {
            connect(c, &QLocalSocket::readyRead, this, [this, c] {
                if (!c->canReadLine())
                    return;
                const QJsonObject o = QJsonDocument::fromJson(c->readLine()).object();
                c->write("ok\n");
                c->flush();
                QStringList links;
                for (const auto &v : o.value(QStringLiteral("links")).toArray())
                    links << v.toString();
                if (o.value(QStringLiteral("activate")).toBool())
                    emit activateRequested();
                if (!links.isEmpty())
                    emit linksReceived(links);
            });
            connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
        }
    });
    return true;
}

} // namespace mx::app
