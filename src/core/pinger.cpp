// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/pinger.h"
#include "mx/net_policy.h"
#include "core/network_manager.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QTcpSocket>

#include <memory>

namespace mx {

Pinger::Pinger(NetworkManager *network, QObject *parent) : QObject(parent), m_network(network)
{
    if (!m_network) return;
    for (NetworkPlugin *p : m_network->plugins()) {
        connect(p, &NetworkPlugin::pingResult, this, [this](quint32 token, int ms) {
            const Done d = m_pluginWaits.take(token);
            if (d) d(ms);
        });
    }
}

bool Pinger::addressOf(const SearchSource &src, QString *host, quint16 *port)
{
    auto split = [&](const QString &hp) -> bool {
        const int c = hp.lastIndexOf(QLatin1Char(':'));
        if (c <= 0) return false;
        bool ok = false;
        const uint p = hp.mid(c + 1).toUInt(&ok);
        QString h = hp.left(c);
        if (h.startsWith(QLatin1Char('[')) && h.endsWith(QLatin1Char(']'))) h = h.mid(1, h.size() - 2);
        if (!ok || p == 0 || p > 65535 || h.isEmpty() || h == QLatin1String("0.0.0.0")) return false;
        *host = h;
        *port = quint16(p);
        return true;
    };
    if (!src.user.address.isEmpty() && split(src.user.address)) return true;
    const QString uri = src.uri.trimmed();
    if (uri.startsWith(QLatin1String("ed2k-peer://"), Qt::CaseInsensitive)) {
        QString hp = uri.mid(12);
        hp = hp.section(QLatin1Char('?'), 0, 0).section(QLatin1Char('/'), 0, 0);
        if (split(hp)) return true;
    }
    if (uri.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) || uri.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)
        || uri.startsWith(QLatin1String("ftp://"), Qt::CaseInsensitive)) {
        const QUrl u(uri);
        if (u.isValid() && !u.host().isEmpty()) {
            const int def = u.scheme() == QLatin1String("https") ? 443 : u.scheme() == QLatin1String("ftp") ? 21 : 80;
            *host = u.host();
            *port = quint16(u.port(def));
            return true;
        }
    }
    static const QRegularExpression hostPort(QStringLiteral("^[0-9a-fA-F.:\\[\\]]+:\\d{1,5}$"));
    if (hostPort.match(src.user.id).hasMatch() && split(src.user.id)) return true;
    return false;
}

void Pinger::ping(const SearchSource &src, Done done)
{
    if (!done) return;
    if (m_network) {
        NetworkPlugin *p = m_network->plugin(src.net);
        if (p && m_network->isRunning(src.net)) {
            const quint32 token = m_nextToken++;
            m_pluginWaits.insert(token, done);
            if (p->ping(token, src)) {
                QTimer::singleShot(m_timeoutMs + 1000, this, [this, token] {
                    const Done d = m_pluginWaits.take(token);
                    if (d) d(-1);
                });
                return;
            }
            m_pluginWaits.remove(token);
        }
    }
    Probe pr;
    if (!addressOf(src, &pr.host, &pr.port)) {
        QTimer::singleShot(0, this, [done] { done(-1); });
        return;
    }
    pr.done = std::move(done);
    m_queue.enqueue(pr);
    next();
}

void Pinger::next()
{
    while (m_running < kMaxParallel && !m_queue.isEmpty()) startProbe(m_queue.dequeue());
}

void Pinger::startProbe(const Probe &p)
{
    ++m_running;
    auto *sock = new QTcpSocket(this);
    auto timer = std::make_shared<QElapsedTimer>();
    auto finished = std::make_shared<bool>(false);
    const Done done = p.done;
    auto finish = [this, sock, finished, done](int ms) {
        if (*finished) return;
        *finished = true;
        sock->disconnect(this);
        sock->abort();
        sock->deleteLater();
        --m_running;
        done(ms);
        QTimer::singleShot(0, this, [this] { next(); });
    };
    connect(sock, &QTcpSocket::connected, this, [finish, timer] { finish(int(qMax<qint64>(1, timer->elapsed()))); });
    connect(sock, &QTcpSocket::errorOccurred, this, [finish] { finish(-1); });
    QTimer::singleShot(m_timeoutMs, sock, [finish] { finish(-1); });
    timer->start();
    // [net-2] through the proxy / from the chosen adapter, as the networks (else no ping at all)
    QString why;
    if (!netpolicy::prepareTcp(sock, m_network ? m_network->settings() : nullptr, &why)) {
        QTimer::singleShot(0, this, [finish] { finish(-1); });
        return;
    }
    sock->connectToHost(p.host, p.port);
}

} // namespace mx
