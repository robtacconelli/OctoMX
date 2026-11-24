// SPDX-License-Identifier: GPL-3.0-or-later
// [str-1] Streaming API tests: a real core (RealCore: search, engine, the API) on a test profile with
// fake networks (local sources from tests/engine/engine_fakes.h) or a real libtorrent node; an HTTP
// client for the API calls and a "player" that reads a stream link like mpv / ffmpeg do (head, end,
// sequential at a bit rate, seeks) and checks every byte against the original.
#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QTest>

#include <memory>

#include "core/engine/download.h"
#include "core/engine/engine.h"
#include "core/real_core.h"
#include "core/stream/stream_service.h"
#include "engine_env.h"
#include "engine_fakes.h"
#include "mx/settings_keys.h"

namespace mx::stest {

using namespace mx::etest;

inline quint16 freePort()
{
    QTcpServer s;
    s.listen(QHostAddress::LocalHost, 0);
    const quint16 p = s.serverPort();
    s.close();
    return p;
}

// ---------------------------------------------------------------- a raw HTTP/1.1 exchange
struct Reply {
    int status = 0;
    QHash<QByteArray, QByteArray> headers;   // lower-case
    QByteArray body;
    bool closed = false;
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
    QByteArray header(const char *n) const { return headers.value(QByteArray(n)); }
};

inline bool parseHead(const QByteArray &head, Reply *r)
{
    const QList<QByteArray> lines = head.split('\n');
    const QList<QByteArray> sl = lines.value(0).trimmed().split(' ');
    if (sl.size() < 2) return false;
    r->status = sl.at(1).toInt();
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray l = lines.at(i).trimmed();
        const int c = l.indexOf(':');
        if (c > 0) r->headers.insert(l.left(c).trimmed().toLower(), l.mid(c + 1).trimmed());
    }
    return true;
}

// Send raw bytes, read one response (head + Content-Length body), within timeoutMs.
inline Reply rawExchange(quint16 port, const QByteArray &request, int timeoutMs = 15000, const QHostAddress &to = QHostAddress::LocalHost)
{
    Reply r;
    QTcpSocket s;
    s.connectToHost(to, port);
    if (!QTest::qWaitFor([&] { return s.state() == QAbstractSocket::ConnectedState || s.state() == QAbstractSocket::UnconnectedState; }, 5000)
        || s.state() != QAbstractSocket::ConnectedState) {
        r.status = -1;
        return r;
    }
    s.write(request);
    QByteArray buf;
    int headEnd = -1;
    qint64 need = -1;
    (void)QTest::qWaitFor([&] {
        buf += s.readAll();
        if (headEnd < 0) {
            headEnd = buf.indexOf("\r\n\r\n");
            if (headEnd >= 0) {
                parseHead(buf.left(headEnd), &r);
                need = request.startsWith("HEAD ") ? 0 : r.headers.value("content-length", "0").toLongLong();
            }
        }
        if (headEnd >= 0 && buf.size() - headEnd - 4 >= need) return true;
        return s.state() != QAbstractSocket::ConnectedState;
    }, timeoutMs);
    if (headEnd >= 0) r.body = buf.mid(headEnd + 4, int(qMax<qint64>(0, need)));
    if (headEnd < 0 && qEnvironmentVariableIsSet("MX_STREAM_TEST_DEBUG")) qInfo("no head; got %d bytes: %s", int(buf.size()), buf.left(80).constData());
    r.closed = s.state() != QAbstractSocket::ConnectedState;
    return r;
}

// ---------------------------------------------------------------- the player
// Reads [from, to) of a stream link over one connection. rateBps = 0: as fast as it can, else at
// that rate (like a player that plays); the socket's read buffer is small so TCP pushes back.
class Player : public QObject {
public:
    Player(quint16 port, const QByteArray &path, qint64 from, qint64 to, qint64 rateBps = 0, const QByteArray &host = QByteArray())
        : m_from(from), m_to(to), m_rate(rateBps)
    {
        m_sock.setReadBufferSize(128 * 1024);
        connect(&m_sock, &QTcpSocket::connected, this, [this, path, host, port] {
            QByteArray req = "GET " + path + " HTTP/1.1\r\nHost: " + (host.isEmpty() ? "127.0.0.1:" + QByteArray::number(port) : host)
                             + "\r\nUser-Agent: TestPlayer/1.0\r\nRange: bytes=" + QByteArray::number(m_from) + '-'
                             + (m_to > 0 ? QByteArray::number(m_to - 1) : QByteArray()) + "\r\n\r\n";
            m_sock.write(req);
        });
        m_tick.setInterval(10);
        connect(&m_tick, &QTimer::timeout, this, [this] { pump(); });
        m_clock.start();
        m_sock.connectToHost(QHostAddress::LocalHost, port);
        m_tick.start();
    }
    void pump()
    {
        const qint64 now = m_clock.elapsed();
        if (m_paused && m_headDone) return;
        if (!m_headDone) {
            m_head += m_sock.read(qMax<qint64>(0, 8192 - m_head.size()));
            const int e = m_head.indexOf("\r\n\r\n");
            if (e < 0) {
                if (m_sock.state() != QAbstractSocket::ConnectedState && m_sock.bytesAvailable() == 0 && now > 200) m_failed = true;
                return;
            }
            parseHead(m_head.left(e), &m_reply);
            m_data = m_head.mid(e + 4);
            m_headDone = true;
            m_headMs = now;
            m_lastRateMs = now;
            if (!m_data.isEmpty()) gotBytes(now);
        }
        qint64 want = m_rate > 0 ? qint64(double(m_rate) * double(now - m_lastRateMs) / 1000.0) + m_credit : qint64(1) << 30;
        if (m_rate > 0) {
            // a player plays at its rate: what it may read now (it never reads ahead of the clock)
            m_lastRateMs = now;
            m_credit = qMin<qint64>(want, m_rate);   // at most a second of catching up
        }
        const qint64 left = expected() - m_data.size();
        if (left <= 0) { m_done = true; m_tick.stop(); return; }
        const qint64 can = qMin(left, m_rate > 0 ? m_credit : qint64(1) << 30);
        if (can <= 0) return;
        const QByteArray b = m_sock.read(can);
        if (!b.isEmpty()) {
            m_data += b;
            if (m_rate > 0) m_credit -= b.size();
            gotBytes(now);
        } else if (m_firstMs >= 0) {
            // it wants bytes and there are none: the player waits
            if (m_waitStart < 0) m_waitStart = now;
            m_longestWait = qMax(m_longestWait, now - m_waitStart);
        }
        if (m_sock.state() != QAbstractSocket::ConnectedState && m_sock.bytesAvailable() == 0 && m_data.size() < expected()) {
            m_failed = true;
            m_tick.stop();
        }
        if (m_data.size() >= expected()) { m_done = true; m_tick.stop(); }
    }
    void gotBytes(qint64 now)
    {
        if (m_firstMs < 0) m_firstMs = now;
        m_waitStart = -1;
    }
    qint64 expected() const
    {
        const qint64 cl = m_reply.headers.value("content-length", "-1").toLongLong();
        return cl >= 0 ? cl : (m_to - m_from);
    }
    // a paused player: the connection stays open, nothing is read
    void stopReading() { m_paused = true; }
    void resumeReading()
    {
        m_paused = false;
        m_lastRateMs = m_clock.elapsed();
        m_credit = 0;
    }
    void stop()
    {
        m_tick.stop();
        m_sock.abort();
    }
    bool done() const { return m_done; }
    bool failed() const { return m_failed; }
    const QByteArray &data() const { return m_data; }
    const Reply &reply() const { return m_reply; }
    qint64 firstByteMs() const { return m_firstMs; }   // from the connect
    qint64 longestWaitMs() const { return m_longestWait; }
    qint64 elapsed() const { return m_clock.elapsed(); }
    bool waitDone(int ms) { return QTest::qWaitFor([this] { return m_done || m_failed; }, ms) && m_done; }

private:
    QTcpSocket m_sock;
    QTimer m_tick;
    QElapsedTimer m_clock;
    qint64 m_from, m_to, m_rate;
    qint64 m_credit = 0, m_lastRateMs = 0;
    QByteArray m_head, m_data;
    Reply m_reply;
    bool m_headDone = false, m_done = false, m_failed = false, m_paused = false;
    qint64 m_headMs = -1, m_firstMs = -1, m_waitStart = -1, m_longestWait = 0;
};

// ---------------------------------------------------------------- a real core with fake networks
class StreamEnv {
public:
    explicit StreamEnv(const QString &name)
    {
        root = testRoot() + QStringLiteral("/stream-") + name;
        QDir(root).removeRecursively();
        QDir().mkpath(root);
        profile = root + QStringLiteral("/profile");
    }
    ~StreamEnv() { down(); }

    TestPlugin *addPlugin(Net net, int caps)
    {
        auto *p = new TestPlugin(net, caps | NetworkPlugin::CanSearch);
        p->findDelayMs = 10;
        pending << p;
        return p;
    }
    // the core with the plugins added so far (and `more`), the API switched on at a free port
    void up(const QList<NetworkPlugin *> &more = {}, bool apiOn = true)
    {
        QList<NetworkPlugin *> all;
        for (TestPlugin *p : std::as_const(pending)) all << p;
        all << more;
        pending.clear();
        core.reset(static_cast<RealCore *>(createRealCore(profile, all)));
        Settings *s = core->settings();
        s->setValue(keys::transfers::IncomingDir, root + QStringLiteral("/incoming"));
        s->setValue(keys::transfers::IncompleteDir, root + QStringLiteral("/incomplete"));
        s->setValue(keys::transfers::MinFreeSpaceMB, 0);
        if (port == 0) port = freePort();
        s->setValue(keys::api::Port, int(port));
        s->setValue(keys::api::Enabled, apiOn);
        core->engine()->setTuning(tuning);
        stream::StreamService::Tuning st = core->streamService()->tuning();
        st.pollMs = 50;
        st.stallNoSourceMs = 8000;
        st.stallMs = 60000;
        st.metaWaitMs = 30000;
        core->streamService()->setTuning(st);
        if (configure) configure(s);
        core->start();
        token = s->s(keys::api::Token).toUtf8();
    }
    void down()
    {
        if (!core) return;
        core->shutdown();
        core.reset();
        QCoreApplication::processEvents();
    }
    engine::Tuning tuning = fastTuning();
    std::function<void(Settings *)> configure;   // more settings before the core starts

    Reply api(const QByteArray &method, const QByteArray &path, const QJsonObject &body = {}, const QByteArray &extraHeaders = QByteArray())
    {
        QByteArray b = body.isEmpty() && method != "POST" ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);
        QByteArray req = method + ' ' + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port)
                         + "\r\nAuthorization: Bearer " + token + "\r\nConnection: close\r\n" + extraHeaders;
        if (!b.isEmpty()) req += "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(b.size()) + "\r\n";
        req += "\r\n" + b;
        return rawExchange(port, req);
    }
    // search, wait for a result named `name`, select it: the stream's JSON (with "url")
    QJsonObject streamOf(const QString &text, const QString &name, int file = -1, bool download = false, int *httpStatus = nullptr)
    {
        const Reply s = api("POST", "/api/v1/searches", QJsonObject{{QStringLiteral("text"), text}});
        const int sid = s.json().value(QStringLiteral("id")).toInt();
        QString key;
        (void)QTest::qWaitFor([&] {
            const Reply r = api("GET", "/api/v1/searches/" + QByteArray::number(sid));
            for (const QJsonValue &v : r.json().value(QStringLiteral("results")).toArray())
                if (v.toObject().value(QStringLiteral("name")).toString() == name) key = v.toObject().value(QStringLiteral("key")).toString();
            return !key.isEmpty();
        }, 10000);
        QJsonObject sel{{QStringLiteral("search"), sid}, {QStringLiteral("result"), key}};
        if (file >= 0) sel.insert(QStringLiteral("file"), file);
        if (download) sel.insert(QStringLiteral("download"), true);
        const Reply r = api("POST", "/api/v1/streams", sel);
        if (httpStatus) *httpStatus = r.status;
        return r.json();
    }
    static QByteArray pathOf(const QJsonObject &stream) { return QUrl(stream.value(QStringLiteral("url")).toString()).toEncoded(QUrl::RemoveScheme | QUrl::RemoveAuthority); }
    QJsonObject status(const QString &streamId) { return api("GET", "/api/v1/streams/" + streamId.toLatin1()).json(); }
    engine::Download *download(const QJsonObject &stream) const
    {
        return core->engine()->downloadObject(stream.value(QStringLiteral("download")).toString());
    }

    QString root, profile;
    quint16 port = 0;
    QByteArray token;
    QList<TestPlugin *> pending;
    std::unique_ptr<RealCore> core;
};

// a search hit for the fake plugins: one file, these sources
inline SearchHit hitOf(const QString &name, const QByteArray &data, const FileHashes &h, const QList<SearchSource> &sources)
{
    SearchHit hit;
    hit.fileName = name;
    hit.size = data.size();
    hit.hashes = h;
    hit.type = mediaTypeForFileName(name);
    hit.sources = sources;
    return hit;
}

} // namespace mx::stest
