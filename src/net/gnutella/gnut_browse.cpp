// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_browse.h"

#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>

namespace mx::gnut {

BrowseClient::BrowseClient(const HostPort &addr, const QString &userAgent, QObject *parent)
    : QObject(parent), m_addr(addr), m_ua(userAgent), m_hdr(16384, 100, 4096)
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(60000);
    connect(&m_timeout, &QTimer::timeout, this, [this] { finish(m_count > 0, QStringLiteral("timeout")); });
}

BrowseClient::~BrowseClient()
{
    if (m_sock) { m_sock->disconnect(this); m_sock->abort(); }
}

void BrowseClient::start()
{
    QTcpSocket *s = m_factory ? m_factory(this) : new QTcpSocket(this);
    m_sock = s;
    connect(s, &QTcpSocket::connected, this, [this, s] { attach(s); });
    connect(s, &QTcpSocket::errorOccurred, this, [this, s](QAbstractSocket::SocketError e) {
        if (e == QAbstractSocket::RemoteHostClosedError && m_inBody) return;   // handled by disconnected
        finish(false, s->errorString());
    });
    connect(s, &QTcpSocket::disconnected, this, [this] {
        if (m_inBody && (m_contentLeft <= 0)) finish(true, QString());
        else finish(m_count > 0, QStringLiteral("connection closed"));
    });
    connect(s, &QTcpSocket::readyRead, this, &BrowseClient::onReadyRead);
    m_timeout.start(25000);
    s->connectToHost(QHostAddress(m_addr.ip), m_addr.port);
}

void BrowseClient::startWithSocket(QTcpSocket *s)
{
    s->setParent(this);
    s->disconnect();
    m_sock = s;
    connect(s, &QTcpSocket::errorOccurred, this, [this, s](QAbstractSocket::SocketError e) {
        if (e == QAbstractSocket::RemoteHostClosedError && m_inBody) return;
        finish(false, s->errorString());
    });
    connect(s, &QTcpSocket::disconnected, this, [this] {
        if (m_inBody && (m_contentLeft <= 0)) finish(true, QString());
        else finish(m_count > 0, QStringLiteral("connection closed"));
    });
    connect(s, &QTcpSocket::readyRead, this, &BrowseClient::onReadyRead);
    attach(s);
}

void BrowseClient::attach(QTcpSocket *s)
{
    HeaderBlock h;
    h.firstLine = QStringLiteral("GET / HTTP/1.1");
    h.add(QStringLiteral("Host"), m_addr.toString());
    h.add(QStringLiteral("User-Agent"), m_ua);
    h.add(QStringLiteral("Accept"), QStringLiteral("text/html, application/x-gnutella-packets"));
    h.add(QStringLiteral("Accept-Encoding"), QStringLiteral("deflate"));
    h.add(QStringLiteral("Connection"), QStringLiteral("close"));
    s->write(h.serialize());
    m_timeout.start();
}

void BrowseClient::abort() { finish(false, QStringLiteral("cancelled")); }

void BrowseClient::onReadyRead()
{
    if (m_done || !m_sock) return;
    m_timeout.start();
    while (!m_done && m_sock && m_sock->bytesAvailable() > 0) {
        const QByteArray data = m_sock->read(65536);
        if (!m_inBody) {
            m_hdr.feed(data);
            if (m_hdr.error()) { finish(false, m_hdr.errorText()); return; }
            if (!m_hdr.complete()) continue;
            const HeaderBlock &h = m_hdr.block();
            if (h.code != 200) {
                finish(false, QStringLiteral("HTTP %1 %2").arg(h.code).arg(h.reason.left(60)));
                return;
            }
            const QString ct = h.first(QStringLiteral("Content-Type"));
            if (!ct.contains(QLatin1String("x-gnutella-packets"), Qt::CaseInsensitive)) {
                finish(false, QStringLiteral("browse not supported (%1)").arg(ct.left(40)));
                return;
            }
            if (h.first(QStringLiteral("Content-Encoding")).contains(QLatin1String("deflate"), Qt::CaseInsensitive))
                m_inflater.reset(new Inflater);
            bool ok = false;
            const qint64 cl = h.first(QStringLiteral("Content-Length")).toLongLong(&ok);
            m_contentLeft = ok ? cl : -1;
            m_inBody = true;
            processBody(m_hdr.takeRemainder());
        } else {
            processBody(data);
        }
    }
}

void BrowseClient::processBody(const QByteArray &raw)
{
    if (m_done) return;
    QByteArray in = raw;
    if (m_contentLeft >= 0) {
        if (in.size() > m_contentLeft) in.truncate(int(m_contentLeft));
        m_contentLeft -= in.size();
    }
    m_rawTotal += in.size();
    if (m_rawTotal > 64LL * 1024 * 1024) { finish(m_count > 0, QStringLiteral("browse reply too large")); return; }
    if (m_inflater) {
        QByteArray plain;
        if (!m_inflater->feed(in, &plain, 16 * 1024 * 1024)) { finish(m_count > 0, QStringLiteral("inflate error")); return; }
        m_framer.feed(plain);
    } else {
        m_framer.feed(in);
    }
    Message m;
    while (m_framer.next(&m)) {
        if (m.type != MsgQueryHit) continue;
        QueryHit hit;
        if (!parseQueryHit(m.payload, &hit)) continue;
        for (const HitResult &r : hit.results) {
            if (m_count >= MaxEntries) break;
            mx::BrowseEntry e;
            QString p = r.path.isEmpty() ? r.name : r.path + QLatin1Char('/') + r.name;
            p.replace(QLatin1Char('/'), QLatin1Char('\\'));
            e.path = p;
            e.size = qint64(r.size);
            e.hashes = r.hashes;
            e.bitrateKbps = r.bitrateKbps;
            e.durationSec = r.durationSec;
            e.sampleRateHz = r.sampleRateHz;
            m_batch << e;
            ++m_count;
        }
        if (m_batch.size() >= 200) { emit entries(m_batch, false); m_batch.clear(); }
        if (m_count >= MaxEntries) { finish(true, QString()); return; }
    }
    if (m_framer.error()) { finish(m_count > 0, m_framer.errorText()); return; }
    if (m_contentLeft == 0) finish(true, QString());
}

void BrowseClient::finish(bool ok, const QString &why)
{
    if (m_done) return;
    m_done = true;
    m_timeout.stop();
    if (m_sock) { m_sock->disconnect(this); m_sock->abort(); }
    if (ok) emit entries(m_batch, true);
    else emit failed(why.isEmpty() ? QStringLiteral("browse failed") : why);
    m_batch.clear();
}

} // namespace mx::gnut
