// SPDX-License-Identifier: GPL-3.0-or-later
// [up-1] The eD2k upload row counts the bytes that really leave, whichever connection of the client
// carries them. A downloader that opens a second connection while its upload session runs (a
// reconnect after a stall, a callback, a client with two sockets) got its data on that connection,
// and the row stayed at "0 B" / 0 B/s: the byte counter was tied to the connection the session had
// started on (the report of bw-2: about 300 KB/s left the socket, the row said 0).
#include "ed2k_core.h"
#include "ed2k_plugin.h"
#include "ed2k_plugin_impl.h"
#include "ed2k_upload.h"
#include "test_support.h"

#include <QtCore/QRandomGenerator>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

using namespace mx::ed2k;

namespace {
struct Node {
    mxtest::MemSettings settings;
    mxtest::FileLibrary lib;
    mxtest::NullLimiter limiter;
    QTemporaryDir dir;
    Ed2kPlugin *plugin = nullptr;
    Node()
    {
        settings.setValue(QStringLiteral("ed2k/tcpPort"), 0);
        settings.setValue(QStringLiteral("ed2k/udpPort"), 0);
        settings.setValue(QStringLiteral("ed2k/autoConnect"), false);
        settings.setValue(QStringLiteral("ed2k/autoUpdateServerList"), false);
        settings.setValue(QStringLiteral("ed2k/udpPing"), false);
        settings.setValue(QStringLiteral("ed2k/udpSearch"), false);
        settings.setValue(QStringLiteral("ed2k/udpSources"), false);
        settings.setValue(QStringLiteral("ed2k/allowLan"), true);
        settings.setValue(QStringLiteral("ed2k/uploadSlots"), 3);
        settings.setValue(QStringLiteral("ed2k/obfuscation"), false);
        plugin = new Ed2kPlugin();
        plugin->init(mxtest::makeContext(&settings, &lib, &limiter, dir.path()));
        plugin->start();
    }
    ~Node() { plugin->stop(); delete plugin; }
    QString uri() const
    {
        return QStringLiteral("ed2k-peer://127.0.0.1:%1?uid=%2").arg(plugin->core()->tcpPort())
            .arg(QString::fromLatin1(plugin->core()->userHash().toHex()));
    }
};

QString makeFile(const QString &dir, qint64 size)
{
    const QString path = dir + QStringLiteral("/generated-upload-rows.bin");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    QRandomGenerator rng(4711);
    QByteArray buf(1 << 20, Qt::Uninitialized);
    for (qint64 done = 0; done < size;) {
        rng.fillRange(reinterpret_cast<quint32 *>(buf.data()), buf.size() / 4);
        const qint64 n = qMin<qint64>(buf.size(), size - done);
        f.write(buf.constData(), n);
        done += n;
    }
    return path;
}

} // namespace

class TstEd2kUploadRows : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { mx::registerMetaTypes(); }

    // two sources of the same client to us (two downloads of the same file, as the bandwidth probe
    // does): every byte that left is in the upload rows, and a running row shows a rate
    void twoConnectionsOfOneClient()
    {
        Node a, b;
        a.limiter.setLimit(mx::RateLimiter::Up, 2 * 1024 * 1024);
        const qint64 size = 8 * 1024 * 1024 + 99;
        const mx::SharedFile sf = a.lib.addFile(makeFile(a.dir.path(), size));
        QVERIFY(sf.hashes.ed2k.size() == 16);
        a.plugin->setSharedFiles(a.lib.sharedFiles());
        QHash<QString, qint64> lastDone;
        int maxRate = 0;
        QObject guard;   // the rows stop being collected when the test's locals go
        QObject::connect(a.plugin, &mx::NetworkPlugin::uploadChanged, &guard, [&](const mx::TransferInfo &t) {
            lastDone[t.id] = qMax(lastDone.value(t.id), t.done);
            maxRate = qMax(maxRate, t.rateUpBps);
        });
        mx::SearchSource ss;
        ss.net = mx::Net::ED2K;
        ss.uri = a.uri();
        mxtest::SimpleDownloader dl(b.dir.path() + QStringLiteral("/out.bin"), sf.size, sf.hashes.ed2k);
        for (int i = 0; i < 2; ++i) {
            mx::RangeSource *rs = b.plugin->openSource(QStringLiteral("pull%1").arg(i), sf.hashes, sf.size, ss);
            QVERIFY(rs);
            dl.addSource(rs);
        }
        QTRY_VERIFY2_WITH_TIMEOUT(dl.done(), qPrintable(dl.summary() + QLatin1Char(' ') + dl.sourceLines().join(QStringLiteral("; "))), 40000);
        QTest::qWait(1500);
        qint64 rows = 0;
        for (qint64 v : std::as_const(lastDone)) rows += v;
        qInfo("received %lld bytes; upload rows %d, %lld bytes in all, rate up to %d B/s", dl.receivedBytes(), int(lastDone.size()), rows, maxRate);
        QVERIFY2(rows >= dl.receivedBytes(), "bytes that left are missing from the upload rows");
        QVERIFY(maxRate > 0);
        for (mx::RangeSource *rs : dl.sources()) {   // before the nodes go
            rs->close();
            delete rs;
        }
    }
};

QTEST_MAIN(TstEd2kUploadRows)
#include "tst_ed2k_upload_rows.moc"
