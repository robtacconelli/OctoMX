// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] outbound checks (see active_checks.h)
#include "core/diag/active_checks.h"
#include "mx/net_policy.h"
#include "mx/types.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QtEndian>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QUdpSocket>

#include <memory>

namespace mx::diag {

static const Settings *g_policy = nullptr;
void setPolicySettings(const Settings *s) { g_policy = s; }

namespace {
// Calls `done` once; later calls are ignored.
struct Once {
    CheckDone done;
    bool fired = false;
    void operator()(const CheckResult &r)
    {
        if (fired) return;
        fired = true;
        if (done) done(r);
    }
};
}

bool parseHostPortText(const QString &s, QString *host, quint16 *port, quint16 defaultPort)
{
    QString t = s.trimmed();
    if (t.isEmpty()) return false;
    int p = defaultPort;
    if (t.startsWith(QLatin1Char('['))) {
        const int e = int(t.indexOf(QLatin1Char(']')));
        if (e < 0) return false;
        *host = t.mid(1, e - 1);
        if (t.mid(e + 1).startsWith(QLatin1Char(':'))) p = t.mid(e + 2).toInt();
    } else if (t.count(QLatin1Char(':')) == 1) {
        *host = t.section(QLatin1Char(':'), 0, 0);
        p = t.section(QLatin1Char(':'), 1).toInt();
    } else {
        *host = t;
    }
    if (host->isEmpty() || p <= 0 || p > 65535) return false;
    *port = quint16(p);
    return true;
}

bool parseUdpTracker(const QString &url, QString *host, quint16 *port)
{
    const QUrl u(url.trimmed());
    if (u.scheme() != QLatin1String("udp") || u.host().isEmpty() || u.port() <= 0) return false;
    *host = u.host();
    *port = quint16(u.port());
    return true;
}

void dnsLookup(QObject *ctx, const QString &host, int timeoutMs, CheckDone done)
{
    auto once = std::make_shared<Once>(Once{std::move(done)});
    auto t = std::make_shared<QElapsedTimer>();
    t->start();
    QHostAddress literal;
    if (literal.setAddress(host)) {
        CheckResult r;
        r.ok = true;
        r.ms = 0;
        r.detail = host;
        QTimer::singleShot(0, ctx, [once, r] { (*once)(r); });
        return;
    }
    if (netpolicy::proxyResolvesNames(g_policy)) {   // [net-2] the proxy looks names up, this computer does not
        CheckResult r;
        r.error = QStringLiteral("not looked up here: the proxy resolves host names");
        QTimer::singleShot(0, ctx, [once, r] { (*once)(r); });
        return;
    }
    QHostInfo::lookupHost(host, ctx, [once, t](const QHostInfo &hi) {
        CheckResult r;
        r.ms = t->elapsed();
        if (hi.error() != QHostInfo::NoError || hi.addresses().isEmpty()) {
            r.error = hi.errorString().isEmpty() ? QStringLiteral("does not resolve") : hi.errorString();
        } else {
            r.ok = true;
            QStringList a;
            for (const QHostAddress &x : hi.addresses()) a << x.toString();
            r.detail = a.mid(0, 3).join(QStringLiteral(", "));
        }
        (*once)(r);
    });
    QTimer::singleShot(timeoutMs, ctx, [once, timeoutMs] {
        CheckResult r;
        r.ms = timeoutMs;
        r.error = QStringLiteral("no answer from the DNS server within %1 s").arg(timeoutMs / 1000);
        (*once)(r);
    });
}

// A UDP request/response exchange: resolve, send `packet` (re-sent every second), accept the
// first datagram `accept` returns true for.
static void udpExchange(QObject *ctx, const QString &host, quint16 port, int timeoutMs, const QByteArray &packet,
                        std::function<bool(const QByteArray &, QString *detail)> accept, CheckDone done)
{
    auto once = std::make_shared<Once>(Once{std::move(done)});
    auto t = std::make_shared<QElapsedTimer>();
    t->start();
    QString noUdp;
    if (!netpolicy::udpAllowed(g_policy, &noUdp)) {   // [net-2] never direct UDP behind a proxy
        CheckResult r;
        r.error = QStringLiteral("not tested: %1").arg(noUdp);
        QTimer::singleShot(0, ctx, [once, r] { (*once)(r); });
        return;
    }
    QPointer<QUdpSocket> sock = new QUdpSocket(ctx);
    auto finish = [once, sock](const CheckResult &r) {
        (*once)(r);
        if (sock) sock->deleteLater();
    };
    QTimer::singleShot(timeoutMs, ctx, [finish, t, timeoutMs] {
        CheckResult r;
        r.ms = t->elapsed();
        r.error = QStringLiteral("no answer within %1 s").arg(timeoutMs / 1000);
        finish(r);
    });
    dnsLookup(ctx, host, qMin(timeoutMs, 5000), [ctx, sock, port, packet, accept, finish, t](const CheckResult &dr) {
        if (!sock) return;
        if (!dr.ok) {
            CheckResult r;
            r.ms = t->elapsed();
            r.error = QStringLiteral("name does not resolve (%1)").arg(dr.error);
            finish(r);
            return;
        }
        const QHostAddress addr(dr.detail.section(QStringLiteral(", "), 0, 0));
        const netpolicy::Policy pol = netpolicy::fromSettings(g_policy);
        QHostAddress from = addr.protocol() == QAbstractSocket::IPv6Protocol ? QHostAddress(QHostAddress::AnyIPv6) : QHostAddress(QHostAddress::AnyIPv4);
        if (pol.bindWanted()) {   // [net-2] from the chosen adapter only
            const netpolicy::Adapter a = netpolicy::resolveAdapter(pol.bind);
            from = addr.protocol() == QAbstractSocket::IPv6Protocol ? a.v6() : a.v4();
        }
        if (from.isNull() || !sock->bind(from, 0)) {
            CheckResult r;
            r.error = QStringLiteral("cannot open a UDP socket: %1").arg(sock->errorString());
            finish(r);
            return;
        }
        QObject::connect(sock, &QUdpSocket::readyRead, sock, [sock, accept, finish, t, addr] {
            while (sock && sock->hasPendingDatagrams()) {
                QByteArray d;
                d.resize(int(qMax<qint64>(0, sock->pendingDatagramSize())));
                QHostAddress from;
                sock->readDatagram(d.data(), d.size(), &from);
                QString detail;
                if (accept(d, &detail)) {
                    CheckResult r;
                    r.ok = true;
                    r.answered = true;
                    r.ms = t->elapsed();
                    r.detail = detail.isEmpty() ? QStringLiteral("answer from %1").arg(addr.toString()) : detail;
                    finish(r);
                    return;
                }
            }
        });
        auto send = std::make_shared<std::function<void(int)>>();
        *send = [sock, addr, port, packet, ctx, w = std::weak_ptr<std::function<void(int)>>(send)](int n) {
            if (!sock || n >= 4) return;
            sock->writeDatagram(packet, addr, port);
            if (auto s = w.lock()) QTimer::singleShot(1000 << qMin(n, 2), ctx, [s, n] { (*s)(n + 1); });
        };
        (*send)(0);
    });
}

void udpTrackerHandshake(QObject *ctx, const QString &host, quint16 port, int timeoutMs, CheckDone done)
{
    QByteArray p(16, 0);
    qToBigEndian<quint64>(0x41727101980ULL, p.data());
    qToBigEndian<quint32>(0, p.data() + 8);                       // action connect
    const quint32 tx = QRandomGenerator::global()->generate();
    qToBigEndian<quint32>(tx, p.data() + 12);
    udpExchange(ctx, host, port, timeoutMs, p, [tx](const QByteArray &d, QString *detail) {
        if (d.size() < 16) return false;
        if (qFromBigEndian<quint32>(d.constData()) != 0 || qFromBigEndian<quint32>(d.constData() + 4) != tx) return false;
        *detail = QStringLiteral("connect handshake answered");
        return true;
    }, std::move(done));
}

void dhtPing(QObject *ctx, const QString &host, quint16 port, int timeoutMs, CheckDone done)
{
    QByteArray id(20, 0);
    for (int i = 0; i < 20; ++i) id[i] = char(QRandomGenerator::global()->bounded(256));
    const QByteArray p = QByteArray("d1:ad2:id20:") + id + QByteArray("e1:q4:ping1:t2:mx1:y1:qe");
    udpExchange(ctx, host, port, timeoutMs, p, [](const QByteArray &d, QString *detail) {
        if (!d.startsWith('d') || !d.contains("1:t2:mx")) return false;
        *detail = d.contains("1:y1:r") ? QStringLiteral("DHT ping answered") : QStringLiteral("DHT node answered");
        return true;
    }, std::move(done));
}

void httpReach(QObject *ctx, QNetworkAccessManager *nam, const QString &url, int timeoutMs, CheckDone done)
{
    auto once = std::make_shared<Once>(Once{std::move(done)});
    auto t = std::make_shared<QElapsedTimer>();
    t->start();
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(timeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OctoMX/" MX_VERSION_STRING));
    QNetworkReply *r = nam->head(req);
    QObject::connect(r, &QNetworkReply::finished, ctx, [r, once, t] {
        r->deleteLater();
        CheckResult res;
        res.ms = t->elapsed();
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status > 0) {
            res.ok = true;          // any HTTP answer: the server is reachable
            res.answered = true;
            res.detail = QStringLiteral("HTTP %1").arg(status);
        } else {
            res.error = r->errorString();
        }
        (*once)(res);
    });
}

void publicPortCheck(QObject *ctx, QNetworkAccessManager *nam, const QString &urlTemplate, int port, int timeoutMs,
                     CheckDone done)
{
    auto once = std::make_shared<Once>(Once{std::move(done)});
    auto t = std::make_shared<QElapsedTimer>();
    t->start();
    QNetworkRequest req{QUrl(QString(urlTemplate).replace(QLatin1String("%1"), QString::number(port)))};
    req.setTransferTimeout(timeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OctoMX/" MX_VERSION_STRING));
    QNetworkReply *r = nam->get(req);
    QObject::connect(r, &QNetworkReply::finished, ctx, [r, once, t] {
        r->deleteLater();
        CheckResult res;
        res.ms = t->elapsed();
        const QByteArray body = r->readAll().trimmed();
        if (r->error() != QNetworkReply::NoError) {
            res.error = r->errorString();
        } else if (body == "1") {
            res.ok = res.answered = true;
            res.detail = QStringLiteral("open");
        } else if (body == "0") {
            res.answered = true;
            res.detail = QStringLiteral("closed");
        } else {
            res.error = QStringLiteral("unexpected answer \"%1\"").arg(QString::fromUtf8(body.left(40)));
        }
        (*once)(res);
    });
}

} // namespace mx::diag
