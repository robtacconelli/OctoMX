// SPDX-License-Identifier: GPL-3.0-or-later
// glue-3: Gnutella upload queue control over loopback HTTP (queued requests are Queued upload rows
// that survive the client's reconnects, priority reorders the queue, "Start Transfer" grants the
// slot at the next poll, cancel drops the entry and refuses the client for a while) and the
// bandwidth accounting of the HTTP upload (headers = protocol, body = payload).
#include "gnut_plugin_impl.h"
#include "test_support.h"

#include "core/rate_limiter.h"
#include "mxhash/encoding.h"

#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QtTest>

using namespace mx::gnut;
using namespace gnuttest;

namespace {

struct Client {
    QTcpSocket s;
    bool open(quint16 port)
    {
        s.connectToHost(QHostAddress::LocalHost, port);
        QElapsedTimer t;   // the server runs in this thread: keep the event loop going
        t.start();
        while (s.state() != QAbstractSocket::ConnectedState && t.elapsed() < 5000) QTest::qWait(10);
        return s.state() == QAbstractSocket::ConnectedState;
    }
    // one request, returns the response head (and reads `bodyLen` bytes of body if 2xx)
    QByteArray get(const QByteArray &sha1, qint64 from, qint64 to, const QByteArray &xQueued = QByteArray())
    {
        QByteArray req = "GET /uri-res/N2R?urn:sha1:" + mx::hash::toBase32(sha1).toLatin1() + " HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\nUser-Agent: glue3-test/1.0\r\nX-Queue: 1.0\r\nConnection: Keep-Alive\r\n"
                         "Range: bytes=" + QByteArray::number(from) + "-" + QByteArray::number(to) + "\r\n";
        if (!xQueued.isEmpty()) req += "X-Queued: ID=" + xQueued + "\r\n";
        req += "\r\n";
        s.write(req);
        QByteArray resp;
        QElapsedTimer t;
        t.start();
        while (!resp.contains("\r\n\r\n") && t.elapsed() < 10000) {
            QTest::qWait(10);
            resp += s.readAll();
            if (s.state() != QAbstractSocket::ConnectedState && !s.bytesAvailable()) break;
        }
        const int end = int(resp.indexOf("\r\n\r\n"));
        if (end < 0) return resp;
        QByteArray head = resp.left(end + 4);
        QByteArray body = resp.mid(end + 4);
        if (head.startsWith("HTTP/1.1 2")) {
            const qint64 want = to - from + 1;
            while (body.size() < want && t.elapsed() < 10000) {
                QTest::qWait(10);
                body += s.readAll();
            }
        }
        return head;
    }
    static QByteArray header(const QByteArray &head, const QByteArray &name)
    {
        for (const QByteArray &l : head.split('\n'))
            if (l.toLower().startsWith(name.toLower() + ':')) return l.mid(name.size() + 1).trimmed();
        return QByteArray();
    }
    static QByteArray parqId(const QByteArray &head)
    {
        const QByteArray v = header(head, "X-Queued");
        const int i = int(v.indexOf("ID="));
        return i < 0 ? QByteArray() : v.mid(i + 3).split(';').first().trimmed();
    }
};

} // namespace

class TstGnutellaGlue3 : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { mx::registerMetaTypes(); }
    void queueControlAndAccounting();
    void uploadObeysSharedLimit();   // [bw-1]
};

void TstGnutellaGlue3::queueControlAndAccounting()
{
    QTemporaryDir dir(QDir::currentPath() + QStringLiteral("/gnut-glue3-XXXXXX"));
    QVERIFY(dir.isValid());
    MemSettings settings;
    FileLibrary lib;
    mx::CoreRateLimiter limiter(nullptr);
    settings.setValue(QStringLiteral("gnutella/enabled"), true);
    settings.setValue(QStringLiteral("gnutella/port"), 0);
    settings.setValue(QStringLiteral("gnutella/bootstrapHosts"), QStringList{QStringLiteral("127.0.0.1:1")});
    settings.setValue(QStringLiteral("gnutella/autoBootstrap"), false);
    settings.setValue(QStringLiteral("gnutella/uploadSlots"), 1);
    settings.setValue(QStringLiteral("gnutella/uploadQueuePollSec"), 2);
    settings.setValue(QStringLiteral("connection/firewalled"), QStringLiteral("open"));
    const QByteArray d1 = makeTestData(400000, 21), d2 = makeTestData(300000, 22);
    QVERIFY(writeFile(dir.filePath(QStringLiteral("share/one.bin")), d1));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("share/two.bin")), d2));
    const mx::SharedFile f1 = lib.addFile(dir.filePath(QStringLiteral("share/one.bin")));
    const mx::SharedFile f2 = lib.addFile(dir.filePath(QStringLiteral("share/two.bin")));
    auto *plugin = new GnutellaPlugin;
    QHash<QString, mx::TransferInfo> rows;
    QStringList finished;
    connect(plugin, &mx::NetworkPlugin::uploadChanged, this, [&](const mx::TransferInfo &t) { rows.insert(t.id, t); });
    connect(plugin, &mx::NetworkPlugin::uploadFinished, this, [&](const QString &id) { finished << id; });
    plugin->init(makeContext(&settings, &lib, &limiter, dir.filePath(QStringLiteral("profile"))));
    plugin->start();
    plugin->setSharedFiles({f1, f2});
    QTRY_VERIFY_WITH_TIMEOUT(plugin->listenPort() != 0, 5000);
    const quint16 port = plugin->listenPort();
    auto rowFor = [&](const QString &name, mx::TransferState st) {
        for (const mx::TransferInfo &t : rows) if (t.name == name && t.state == st && !finished.contains(t.id)) return t;
        return mx::TransferInfo();
    };

    // the hog takes the only slot and keeps the connection
    Client hog;
    QVERIFY(hog.open(port));
    const QByteArray hh = hog.get(f1.hashes.sha1, 0, 9);
    QVERIFY2(hh.startsWith("HTTP/1.1 206"), hh.left(200).constData());
    // c2 (one.bin) and c3 (two.bin) are queued: rows in state Queued with their positions
    Client c2, c3;
    QVERIFY(c2.open(port) && c3.open(port));
    QByteArray h2 = c2.get(f1.hashes.sha1, 0, 99999);
    QVERIFY2(h2.startsWith("HTTP/1.1 503"), h2.constData());
    QByteArray h3 = c3.get(f2.hashes.sha1, 0, 99999);
    QVERIFY2(h3.startsWith("HTTP/1.1 503"), h3.constData());
    const QByteArray parq3 = Client::parqId(h3);
    QVERIFY(!parq3.isEmpty());
    QTRY_VERIFY(!rowFor(f1.name, mx::TransferState::Queued).id.isEmpty());
    QTRY_VERIFY(!rowFor(f2.name, mx::TransferState::Queued).id.isEmpty());
    const QString idX = rowFor(f1.name, mx::TransferState::Queued).id, idY = rowFor(f2.name, mx::TransferState::Queued).id;
    QCOMPARE(plugin->queuedUploadIds(), (QStringList{idX, idY}));
    QCOMPARE(rows.value(idX).queuePosition, 1);
    QCOMPARE(rows.value(idY).queuePosition, 2);

    // c3 hangs up and polls again later (PARQ): the same row, no "finished" in between
    c3.s.disconnectFromHost();
    QTest::qWait(300);
    QVERIFY(!finished.contains(idY));
    Client c3b;
    QVERIFY(c3b.open(port));
    h3 = c3b.get(f2.hashes.sha1, 0, 99999, parq3);
    QVERIFY(h3.startsWith("HTTP/1.1 503"));
    QCOMPARE(plugin->queuedUploadIds(), (QStringList{idX, idY}));
    QVERIFY(!finished.contains(idY));

    // priority: Y to the front
    QVERIFY(plugin->setUploadPriority(idY, 1));
    QCOMPARE(plugin->queuedUploadIds(), (QStringList{idY, idX}));
    QCOMPARE(rows.value(idY).queuePosition, 1);
    QCOMPARE(rows.value(idY).priority, 1);
    QCOMPARE(rows.value(idX).queuePosition, 2);
    QVERIFY(!plugin->setUploadPriority(QStringLiteral("nope"), 1));

    // "Start Transfer" on X: its next poll gets data although the hog still holds the only slot
    QVERIFY(plugin->startUploadNow(idX));
    h2 = c2.get(f1.hashes.sha1, 0, 99999);
    QVERIFY2(h2.startsWith("HTTP/1.1 206"), h2.constData());
    QTRY_COMPARE(rows.value(idX).state, mx::TransferState::Transferring);   // same row id
    QCOMPARE(plugin->queuedUploadIds(), QStringList{idY});
    QVERIFY(!plugin->startUploadNow(QStringLiteral("nope")));

    // cancel Y: the row goes away and the client is refused for a while
    plugin->cancelUpload(idY);
    QTRY_VERIFY(finished.contains(idY));
    QVERIFY(plugin->queuedUploadIds().isEmpty());
    Client c3c;
    QVERIFY(c3c.open(port));
    h3 = c3c.get(f2.hashes.sha1, 0, 99999);
    QVERIFY2(h3.startsWith("HTTP/1.1 503") && !h3.contains("X-Queued"), h3.constData());
    QVERIFY(plugin->queuedUploadIds().isEmpty());

    // accounting: bodies are payload (10 + 100000 bytes), headers are protocol on top
    const qint64 payUp = limiter.payloadTotal(mx::RateLimiter::Up, mx::Net::Gnutella);
    const qint64 wireUp = limiter.total(mx::RateLimiter::Up, mx::Net::Gnutella);
    QCOMPARE(payUp, qint64(10 + 100000));
    QVERIFY2(wireUp > payUp + 4 * 100 && wireUp < payUp + 20000, qPrintable(QString::number(wireUp)));
    QVERIFY(limiter.total(mx::RateLimiter::Down, mx::Net::Gnutella) > 5 * 100);   // the requests
    QCOMPARE(limiter.payloadTotal(mx::RateLimiter::Down, mx::Net::Gnutella), qint64(0));
    qInfo() << "gnutella upload payload" << payUp << "wire" << wireUp << "requests in"
            << limiter.total(mx::RateLimiter::Down, mx::Net::Gnutella);
    plugin->stop();
    delete plugin;
}

// [bw-1] an HTTP upload over loopback with the core's shared limiter: the upload limit holds,
// loopback (a LAN peer) is exempt unless bandwidth/limitLan, unlimited is unlimited, a change
// applies to the running upload
void TstGnutellaGlue3::uploadObeysSharedLimit()
{
    QTemporaryDir dir(QDir::currentPath() + QStringLiteral("/gnut-bw1-XXXXXX"));
    QVERIFY(dir.isValid());
    MemSettings settings;
    FileLibrary lib;
    mx::CoreRateLimiter limiter(nullptr);
    settings.setValue(QStringLiteral("gnutella/enabled"), true);
    settings.setValue(QStringLiteral("gnutella/port"), 0);
    settings.setValue(QStringLiteral("gnutella/bootstrapHosts"), QStringList{QStringLiteral("127.0.0.1:1")});
    settings.setValue(QStringLiteral("gnutella/autoBootstrap"), false);
    settings.setValue(QStringLiteral("gnutella/uploadSlots"), 4);
    settings.setValue(QStringLiteral("connection/firewalled"), QStringLiteral("open"));
    const QByteArray d = makeTestData(8 * 1024 * 1024, 31);
    QVERIFY(writeFile(dir.filePath(QStringLiteral("share/big.bin")), d));
    const mx::SharedFile f = lib.addFile(dir.filePath(QStringLiteral("share/big.bin")));
    auto *plugin = new GnutellaPlugin;
    plugin->init(makeContext(&settings, &lib, &limiter, dir.filePath(QStringLiteral("profile"))));
    plugin->start();
    plugin->setSharedFiles({f});
    QTRY_VERIFY_WITH_TIMEOUT(plugin->listenPort() != 0, 5000);
    const quint16 port = plugin->listenPort();
    // receive `len` bytes from `from`; returns the milliseconds from the request to the last byte
    auto fetch = [&](qint64 from, qint64 len, int maxMs, qint64 *got) -> qint64 {
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, port);
        if (!s.waitForConnected(5000)) return -1;
        QElapsedTimer t;
        t.start();
        s.write("GET /uri-res/N2R?urn:sha1:" + mx::hash::toBase32(f.hashes.sha1).toLatin1() +
                " HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: bw1-test/1.0\r\nRange: bytes=" + QByteArray::number(from) + "-" +
                QByteArray::number(from + len - 1) + "\r\n\r\n");
        QByteArray buf;
        bool head = false;
        while (t.elapsed() < maxMs && buf.size() < len) {
            QTest::qWait(2);
            buf += s.readAll();
            if (!head) {
                const int e = int(buf.indexOf("\r\n\r\n"));
                if (e < 0) continue;
                if (!buf.startsWith("HTTP/1.1 2")) return -2;
                buf = buf.mid(e + 4);
                head = true;
            }
        }
        *got = buf.size();
        return t.elapsed();
    };
    // a token bucket moves at most rate * time + one second's burst, and close to rate * time
    auto check = [](const char *what, qint64 bytes, qint64 ms, int rate) {
        const double secs = ms / 1000.0;
        qInfo("%s: %lld bytes in %lld ms = %.0f B/s, limit %d B/s, at most %.0f bytes (rate x time + 1 s burst)", what, bytes, ms,
              bytes / secs, rate, rate * secs + rate);
        return bytes <= rate * secs + rate * 1.05 && bytes >= rate * secs * 0.8;
    };
    qint64 got = 0;
    // 1. 200 KB/s upload limit, loopback limited too ("Apply limits to LAN peers")
    limiter.setLimits(0, 200 * 1024);
    limiter.setLimitLan(true);
    qint64 ms = fetch(0, 1200 * 1024, 20000, &got);
    QCOMPARE(got, qint64(1200 * 1024));
    QVERIFY(check("gnutella upload, limit 200 KB/s", got, ms, 200 * 1024));
    QVERIFY(limiter.chargedTotal(mx::RateLimiter::Up) >= 1200 * 1024);
    // 2. LAN peers exempt (the default): loopback runs at full speed and is not charged
    limiter.setLimitLan(false);
    const qint64 chargedBefore = limiter.chargedTotal(mx::RateLimiter::Up);
    ms = fetch(1024 * 1024, 3 * 1024 * 1024, 15000, &got);
    qInfo("gnutella upload to a LAN peer, limit 200 KB/s, LAN exempt: %lld bytes in %lld ms", got, ms);
    QCOMPARE(got, qint64(3 * 1024 * 1024));
    QVERIFY2(ms < 1500, qPrintable(QString::number(ms)));
    QVERIFY(limiter.chargedTotal(mx::RateLimiter::Up) - chargedBefore < 64 * 1024);   // headers only
    QVERIFY(limiter.exemptTotal(mx::RateLimiter::Up) >= 3 * 1024 * 1024);
    // 3. unlimited really is
    limiter.setLimitLan(true);
    limiter.setLimits(0, 0);
    ms = fetch(4 * 1024 * 1024, 3 * 1024 * 1024, 15000, &got);
    qInfo("gnutella upload unlimited: %lld bytes in %lld ms", got, ms);
    QCOMPARE(got, qint64(3 * 1024 * 1024));
    QVERIFY2(ms < 1500, qPrintable(QString::number(ms)));
    // 4. a change applies to a running upload: 100 KB/s, after 2 s 400 KB/s. 1.5 MB: ~200 KB (+ burst)
    // in the first 2 s, the rest at 400 KB/s, about 5.5 s in all (at 100 KB/s only: ~15 s)
    limiter.setLimits(0, 100 * 1024);
    QTimer::singleShot(2000, this, [&limiter] { limiter.setLimits(0, 400 * 1024); });
    ms = fetch(0, 1536 * 1024, 20000, &got);
    qInfo("gnutella upload 100 -> 400 KB/s after 2 s: %lld bytes in %lld ms", got, ms);
    QCOMPARE(got, qint64(1536 * 1024));
    QVERIFY2(ms > 4000 && ms < 7500, qPrintable(QString::number(ms)));
    plugin->stop();
    delete plugin;
}

QTEST_MAIN(TstGnutellaGlue3)
#include "tst_gnutella_glue3.moc"
