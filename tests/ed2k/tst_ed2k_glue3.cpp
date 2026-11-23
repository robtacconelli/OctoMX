// SPDX-License-Identifier: GPL-3.0-or-later
// glue-3: eD2k upload queue control over loopback (queued clients reported as Queued upload rows,
// priority reorders the queue, "Start Transfer" forces a slot, cancel drops a client) and the
// bandwidth accounting of eD2k TCP traffic (wire + payload) through the real core RateLimiter.
#include "ed2k_core.h"
#include "ed2k_opcodes.h"
#include "ed2k_plugin_impl.h"
#include "ed2k_upload.h"
#include "ed2k_util.h"
#include "test_support.h"

#include "core/rate_limiter.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

using namespace mx::ed2k;

namespace {

struct Node {
    mxtest::MemSettings settings;
    mxtest::FileLibrary lib;
    mx::CoreRateLimiter limiter{nullptr};
    QTemporaryDir dir;
    Ed2kPlugin *plugin = nullptr;
    QHash<QString, mx::TransferInfo> rows;   // upload id → last report
    QStringList finished;
    explicit Node(int nSlots = 3)
    {
        settings.setValue(QStringLiteral("ed2k/tcpPort"), 0);
        settings.setValue(QStringLiteral("ed2k/udpPort"), 0);
        settings.setValue(QStringLiteral("ed2k/autoConnect"), false);
        settings.setValue(QStringLiteral("ed2k/autoUpdateServerList"), false);
        settings.setValue(QStringLiteral("ed2k/udpPing"), false);
        settings.setValue(QStringLiteral("ed2k/udpSearch"), false);
        settings.setValue(QStringLiteral("ed2k/udpSources"), false);
        settings.setValue(QStringLiteral("ed2k/allowLan"), true);
        settings.setValue(QStringLiteral("ed2k/uploadSlots"), nSlots);
        settings.setValue(QStringLiteral("ed2k/obfuscation"), true);
        plugin = new Ed2kPlugin();
        QObject::connect(plugin, &mx::NetworkPlugin::uploadChanged, plugin, [this](const mx::TransferInfo &t) { rows.insert(t.id, t); });
        QObject::connect(plugin, &mx::NetworkPlugin::uploadFinished, plugin, [this](const QString &id) { finished << id; });
        plugin->init(mxtest::makeContext(&settings, &lib, &limiter, dir.path()));
        plugin->start();
    }
    ~Node() { plugin->stop(); delete plugin; }
    QString uri() const
    {
        return QStringLiteral("ed2k-peer://127.0.0.1:%1?uid=%2&crypt=3")
            .arg(plugin->core()->tcpPort())
            .arg(QString::fromLatin1(plugin->core()->userHash().toHex()));
    }
    QString userHash() const { return QString::fromLatin1(plugin->core()->userHash().toHex()); }
    // the newest row of the client `peerHash`
    mx::TransferInfo rowOf(const QString &peerHash) const
    {
        mx::TransferInfo best;
        for (const mx::TransferInfo &t : rows)
            if (t.peer.id == peerHash && !finished.contains(t.id)) best = t;
        return best;
    }
};

QString makeFile(const QString &dir, qint64 size)
{
    const QString path = dir + QStringLiteral("/queue test file.bin");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    QRandomGenerator rng(5);
    QByteArray buf(1 << 20, '\0');
    while (f.size() < size) {
        for (auto &c : buf) c = char(rng.bounded(256));
        f.write(buf.left(int(qMin<qint64>(buf.size(), size - f.size()))));
    }
    return path;
}

} // namespace

class TstEd2kGlue3 : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { mx::registerMetaTypes(); }
    void queueControlAndAccounting();
    void transfersObeySharedLimits();   // [bw-1]
};

void TstEd2kGlue3::queueControlAndAccounting()
{
    Node a(1), b1, b2, b3;
    a.limiter.setLimits(0, 1536 * 1024);   // the first session lasts several seconds
    a.limiter.setLimitLan(true);            // [bw-1] loopback is a LAN peer: exempt unless limitLan
    const QString src = makeFile(a.dir.path(), 3 * PARTSIZE + 4321);
    const mx::SharedFile sf = a.lib.addFile(src);
    a.plugin->setSharedFiles(a.lib.sharedFiles());
    struct Dl { Node *n; mx::RangeSource *rs; mxtest::SimpleDownloader *dl; };
    QList<Dl> dls;
    for (Node *n : {&b1, &b2, &b3}) {
        mx::SearchSource ss;
        ss.net = mx::Net::ED2K;
        ss.uri = a.uri();
        mx::RangeSource *rs = n->plugin->openSource(QStringLiteral("q"), sf.hashes, sf.size, ss);
        QVERIFY(rs);
        auto *dl = new mxtest::SimpleDownloader(n->dir.path() + QStringLiteral("/out.bin"), sf.size, sf.hashes.ed2k, this);
        connect(n->plugin, &mx::NetworkPlugin::hashSetLearned, dl, [dl](const QString &, const QString &, qint64, const QList<QByteArray> &h) { dl->setPartHashes(h); });
        dls.append({n, rs, dl});
    }
    UploadManager *um = a.plugin->core()->uploads();

    // b1 gets the only slot
    dls[0].dl->addSource(dls[0].rs);
    QTRY_VERIFY_WITH_TIMEOUT(a.rowOf(b1.userHash()).state == mx::TransferState::Transferring, 20000);
    // b2 and b3 wait: reported as Queued rows with their positions
    dls[1].dl->addSource(dls[1].rs);
    QTRY_VERIFY_WITH_TIMEOUT(!a.rowOf(b2.userHash()).id.isEmpty() && a.rowOf(b2.userHash()).state == mx::TransferState::Queued, 20000);
    dls[2].dl->addSource(dls[2].rs);
    QTRY_VERIFY_WITH_TIMEOUT(!a.rowOf(b3.userHash()).id.isEmpty() && a.rowOf(b3.userHash()).state == mx::TransferState::Queued, 20000);
    const QString id2 = a.rowOf(b2.userHash()).id, id3 = a.rowOf(b3.userHash()).id;
    QVERIFY(!id2.isEmpty() && !id3.isEmpty() && id2 != id3);
    QCOMPARE(um->waitingUploadIds(), (QStringList{id2, id3}));
    QTRY_COMPARE(a.rowOf(b2.userHash()).queuePosition, 1);
    QTRY_COMPARE(a.rowOf(b3.userHash()).queuePosition, 2);
    QVERIFY(a.rowOf(b3.userHash()).statusText.startsWith(QStringLiteral("Queued (position 2 of 2)")));
    QCOMPARE(a.rowOf(b3.userHash()).name, sf.name);

    // priority: b3 high → front of the queue
    QVERIFY(a.plugin->setUploadPriority(id3, 1));
    QCOMPARE(um->waitingUploadIds(), (QStringList{id3, id2}));
    QCOMPARE(a.rows.value(id3).queuePosition, 1);
    QCOMPARE(a.rows.value(id3).priority, 1);
    QCOMPARE(a.rows.value(id2).queuePosition, 2);
    QVERIFY(!a.plugin->setUploadPriority(QStringLiteral("nope"), 1));

    // "Start Transfer" on b2: a second slot although only one is configured; the row keeps its id
    QVERIFY(a.plugin->startUploadNow(id2));
    QTRY_COMPARE_WITH_TIMEOUT(a.rows.value(id2).state, mx::TransferState::Transferring, 20000);
    QCOMPARE(um->activeUploads(), 2);
    QCOMPARE(um->waitingUploadIds(), QStringList{id3});
    QVERIFY(!a.plugin->startUploadNow(QStringLiteral("nope")));

    // cancel the waiting b3: its row goes away, the queue is empty
    a.plugin->cancelUpload(id3);
    QVERIFY(a.finished.contains(id3));
    QVERIFY(um->waitingUploadIds().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(a.rows.value(id2).done > 0, 20000);
    QTest::qWait(1500);
    QVERIFY(um->waitingUploadIds().isEmpty());   // b3 is not taken back right away

    // bandwidth accounting: payload = what the upload manager sent; wire adds protocol traffic
    const qint64 payUp = a.limiter.payloadTotal(mx::RateLimiter::Up, mx::Net::ED2K);
    const qint64 wireUp = a.limiter.total(mx::RateLimiter::Up, mx::Net::ED2K);
    QVERIFY(payUp > 0);
    QCOMPARE(payUp, um->totalUploaded());
    QVERIFY2(wireUp > payUp, qPrintable(QStringLiteral("%1 %2").arg(wireUp).arg(payUp)));
    QVERIFY(wireUp < payUp + payUp / 20 + 64 * 1024);   // overhead is a few percent
    const qint64 rx = dls[0].dl->receivedBytes() + dls[1].dl->receivedBytes();
    qint64 payDown = 0, wireDown = 0;
    for (Node *n : {&b1, &b2}) {
        payDown += n->limiter.payloadTotal(mx::RateLimiter::Down, mx::Net::ED2K);
        wireDown += n->limiter.total(mx::RateLimiter::Down, mx::Net::ED2K);
    }
    QVERIFY2(payDown >= rx && payDown <= rx + 2 * 184320, qPrintable(QStringLiteral("%1 %2").arg(payDown).arg(rx)));
    QVERIFY(wireDown > payDown);
    qInfo() << "a sent payload" << payUp << "wire" << wireUp << "; b1+b2 got payload" << payDown << "wire" << wireDown;
    for (auto &d : dls) { d.rs->close(); delete d.rs; delete d.dl; }
}

// [bw-1] an eD2k transfer over loopback with the core's shared limiters on both ends: the uploader's
// limit holds, then the downloader's, a change applies while it runs, a LAN peer is exempt unless
// bandwidth/limitLan, unlimited is unlimited
void TstEd2kGlue3::transfersObeySharedLimits()
{
    Node a(2), b;
    a.limiter.setLimitLan(true);
    b.limiter.setLimitLan(true);
    const QString src = makeFile(a.dir.path(), 4 * PARTSIZE);
    const mx::SharedFile sf = a.lib.addFile(src);
    a.plugin->setSharedFiles(a.lib.sharedFiles());
    mx::SearchSource ss;
    ss.net = mx::Net::ED2K;
    ss.uri = a.uri();
    mx::RangeSource *rs = b.plugin->openSource(QStringLiteral("bw"), sf.hashes, sf.size, ss);
    QVERIFY(rs);
    auto *dl = new mxtest::SimpleDownloader(b.dir.path() + QStringLiteral("/out.bin"), sf.size, sf.hashes.ed2k, this);
    connect(b.plugin, &mx::NetworkPlugin::hashSetLearned, dl, [dl](const QString &, const QString &, qint64, const QList<QByteArray> &h) { dl->setPartHashes(h); });
    a.limiter.setLimits(0, 200 * 1024);   // uploader: 200 KB/s
    dl->addSource(rs);
    QTRY_VERIFY_WITH_TIMEOUT(dl->receivedBytes() > 0, 20000);
    auto rate = [&](int ms) {
        QTest::qWait(1500);   // a change settles; one second's burst is spent
        const qint64 r0 = dl->receivedBytes();
        QElapsedTimer t;
        t.start();
        QTest::qWait(ms);
        return double(dl->receivedBytes() - r0) * 1000.0 / double(t.elapsed());
    };
    const double up = rate(6000);
    qInfo("eD2k upload limit 200 KB/s: %.0f B/s", up);
    QVERIFY2(up > 200 * 1024 * 0.8 && up < 200 * 1024 * 1.12, qPrintable(QString::number(up)));
    // the downloader's limit: 120 KB/s, the uploader unlimited
    b.limiter.setLimits(120 * 1024, 0);
    a.limiter.setLimits(0, 0);
    const double down = rate(6000);
    qInfo("eD2k download limit 120 KB/s: %.0f B/s", down);
    QVERIFY2(down > 120 * 1024 * 0.8 && down < 120 * 1024 * 1.12, qPrintable(QString::number(down)));
    // a change while it runs
    b.limiter.setLimits(400 * 1024, 0);
    const double down2 = rate(5000);
    qInfo("eD2k download limit changed to 400 KB/s: %.0f B/s", down2);
    QVERIFY2(down2 > 400 * 1024 * 0.8 && down2 < 400 * 1024 * 1.12, qPrintable(QString::number(down2)));
    // LAN peers exempt (the default): far above the limit
    b.limiter.setLimitLan(false);
    const qint64 charged = b.limiter.chargedTotal(mx::RateLimiter::Down);
    const double lan = rate(2000);
    qInfo("eD2k download from a LAN peer, limit 400 KB/s, LAN exempt: %.0f B/s", lan);
    QVERIFY2(lan > 1500 * 1024 || dl->done(), qPrintable(QString::number(lan)));
    QVERIFY(b.limiter.chargedTotal(mx::RateLimiter::Down) - charged < 128 * 1024);
    QVERIFY(b.limiter.exemptTotal(mx::RateLimiter::Down) > 1024 * 1024);
    rs->close();   // the downloader (child of the test) goes with the test, as in the test above
}

QTEST_MAIN(TstEd2kGlue3)
#include "tst_ed2k_glue3.moc"
