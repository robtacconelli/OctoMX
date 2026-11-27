// SPDX-License-Identifier: GPL-3.0-or-later
// [api-2] Compatibility: the walk-through of the 5.7 streaming API (docs/API.md, "Walk-through:
// search, pick, play" and the examples of every call it uses) played call by call against this
// version, checking every field the document shows, with its type and, where the document gives it,
// its value. A program written for 5.7 must keep working unchanged: answers may only gain fields.
#include <QtTest/QTest>

#include "stream_support.h"

using namespace mx;
using namespace mx::stest;

namespace {

constexpr qint64 MiB = 1024 * 1024;

void hasNumber(const QJsonObject &o, const char *k)
{
    QVERIFY2(o.value(QLatin1String(k)).isDouble(), k);
}
void hasString(const QJsonObject &o, const char *k)
{
    QVERIFY2(o.value(QLatin1String(k)).isString(), k);
}
void hasBool(const QJsonObject &o, const char *k)
{
    QVERIFY2(o.value(QLatin1String(k)).isBool(), k);
}

} // namespace

class TstApiCompat : public QObject {
    Q_OBJECT
private slots:
    void walkThrough57();
};

void TstApiCompat::walkThrough57()
{
    StreamEnv env(QStringLiteral("compat57"));
    // the two results of the document: a bundle (BitTorrent, file list known) and a single file
    TestPlugin *web = env.addPlugin(Net::Web, 0);
    SearchHit bundle;
    bundle.fileName = QStringLiteral("Big Buck Bunny");
    bundle.size = 140 + 8 * MiB + 310380;
    bundle.isBundle = true;
    bundle.type = MediaType::Video;
    bundle.hashes.btih = QByteArray(20, '\x61');
    SearchSource bs;
    bs.net = Net::BitTorrent;
    bs.uri = QStringLiteral("magnet:?xt=urn:btih:") + QString::fromLatin1(bundle.hashes.btih.toHex());
    bs.seeds = 380;
    bs.peers = 32;   // leechers: 412 peers with the seeds
    bundle.sources << bs;
    bundle.files = {BundleFile{QStringLiteral("Big Buck Bunny/Big Buck Bunny.en.srt"), 140, 0},
                    BundleFile{QStringLiteral("Big Buck Bunny/Big Buck Bunny.mp4"), 8 * MiB, 1},
                    BundleFile{QStringLiteral("Big Buck Bunny/poster.jpg"), 310380, 2}};
    web->cannedHits << bundle;
    TestPlugin *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(8 * MiB + 4321, 57);
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 3 * 1000 * 1000;
    c.hashSet = ed2kParts(data);
    SearchHit avi = hitOf(QStringLiteral("big_buck_bunny_480p_surround-fix.avi"), data, hashesOf(data, 3),
                          {ed->addSource(QStringLiteral("test-ed2k://compat"), c, QStringLiteral("198.51.100.57:4662"))});
    avi.durationSec = 60;
    ed->cannedHits << avi;
    env.up();

    // ---- GET /api/v1/status
    {
        const Reply r = env.api("GET", "/api/v1/status");
        QCOMPARE(r.status, 200);
        QCOMPARE(r.header("content-type"), QByteArray("application/json; charset=utf-8"));
        const QJsonObject o = r.json();
        QCOMPARE(o.value(QStringLiteral("api")).toString(), QStringLiteral("on"));
        QCOMPARE(o.value(QStringLiteral("version")).toString(), QStringLiteral(MX_VERSION_STRING));
        QCOMPARE(o.value(QStringLiteral("searches")).toInt(-1), 0);
        QCOMPARE(o.value(QStringLiteral("streams")).toInt(-1), 0);
        const QJsonArray nets = o.value(QStringLiteral("networks")).toArray();
        QVERIFY(!nets.isEmpty());
        static const QStringList states{QStringLiteral("disabled"), QStringLiteral("disconnected"), QStringLiteral("connecting"),
                                        QStringLiteral("connected"), QStringLiteral("error")};
        for (const QJsonValue &v : nets) {
            const QJsonObject n = v.toObject();
            hasString(n, "name");
            hasNumber(n, "peers");
            QVERIFY2(states.contains(n.value(QStringLiteral("state")).toString()), qPrintable(n.value(QStringLiteral("state")).toString()));
        }
    }
    // ---- 1. search
    int sid = 0;
    {
        const Reply r = env.api("POST", "/api/v1/searches", QJsonObject{{QStringLiteral("text"), QStringLiteral("big buck bunny")}, {QStringLiteral("type"), QStringLiteral("video")}});
        QCOMPARE(r.status, 201);
        const QJsonObject o = r.json();
        sid = o.value(QStringLiteral("id")).toInt();
        QCOMPARE(sid, 1);
        QCOMPARE(o.value(QStringLiteral("running")).toBool(), true);
        QCOMPARE(o.value(QStringLiteral("url")).toString(), QStringLiteral("/api/v1/searches/1"));
    }
    // ---- 2. the results (?limit=2, most sources first)
    QString aviKey, bundleKey;
    {
        QJsonObject o;
        QVERIFY(QTest::qWaitFor([&] {
            o = env.api("GET", "/api/v1/searches/1?limit=2").json();
            return o.value(QStringLiteral("results")).toArray().size() == 2;
        }, 10000));
        hasNumber(o, "count");
        QCOMPARE(o.value(QStringLiteral("count")).toInt(), 2);
        QCOMPARE(o.value(QStringLiteral("id")).toInt(), 1);
        QCOMPARE(o.value(QStringLiteral("offset")).toInt(-1), 0);
        hasBool(o, "running");
        QCOMPARE(o.value(QStringLiteral("text")).toString(), QStringLiteral("big buck bunny"));
        const QJsonArray res = o.value(QStringLiteral("results")).toArray();
        QCOMPARE(res.at(0).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Big Buck Bunny"));   // 412 peers first
        for (const QJsonValue &v : res) {
            const QJsonObject h = v.toObject();
            for (const char *k : {"key", "name", "type"}) hasString(h, k);
            for (const char *k : {"peers", "seeds", "size", "sources"}) hasNumber(h, k);
            for (const char *k : {"bundle", "streamable"}) hasBool(h, k);
            QVERIFY(h.value(QStringLiteral("networks")).isArray());
            QCOMPARE(h.value(QStringLiteral("key")).toString().size(), 16);
            QCOMPARE(h.value(QStringLiteral("type")).toString(), QStringLiteral("video"));
            if (h.value(QStringLiteral("bundle")).toBool()) {
                bundleKey = h.value(QStringLiteral("key")).toString();
                QCOMPARE(h.value(QStringLiteral("peers")).toInt(), 412);
                QCOMPARE(h.value(QStringLiteral("seeds")).toInt(), 380);
                QVERIFY(h.value(QStringLiteral("networks")).toArray().contains(QStringLiteral("BitTorrent")));
                const QJsonArray files = h.value(QStringLiteral("files")).toArray();
                QCOMPARE(files.size(), 3);
                for (int i = 0; i < 3; ++i) {
                    const QJsonObject f = files.at(i).toObject();
                    QCOMPARE(f.value(QStringLiteral("index")).toInt(), i);
                    hasString(f, "path");
                    hasNumber(f, "size");
                    QCOMPARE(f.value(QStringLiteral("streamable")).toBool(), i == 1);
                }
                QCOMPARE(files.at(1).toObject().value(QStringLiteral("path")).toString(), QStringLiteral("Big Buck Bunny/Big Buck Bunny.mp4"));
                QCOMPARE(h.value(QStringLiteral("streamable")).toBool(), true);
            } else {
                aviKey = h.value(QStringLiteral("key")).toString();
                QCOMPARE(h.value(QStringLiteral("name")).toString(), QStringLiteral("big_buck_bunny_480p_surround-fix.avi"));
                QCOMPARE(qint64(h.value(QStringLiteral("size")).toDouble()), qint64(data.size()));
                QCOMPARE(h.value(QStringLiteral("networks")).toArray(), QJsonArray{QStringLiteral("eD2k")});
                QCOMPARE(h.value(QStringLiteral("sources")).toInt(), 1);
                QCOMPARE(h.value(QStringLiteral("streamable")).toBool(), true);
                QVERIFY(!h.contains(QStringLiteral("files")));
            }
        }
        QVERIFY(!aviKey.isEmpty() && !bundleKey.isEmpty());
        // one result in full
        const Reply one = env.api("GET", "/api/v1/searches/1/results/" + bundleKey.toLatin1());
        QCOMPARE(one.status, 200);
        QCOMPARE(one.json().value(QStringLiteral("files")).toArray().size(), 3);
        QCOMPARE(env.api("GET", "/api/v1/searches").json().value(QStringLiteral("searches")).toArray().size(), 1);
    }
    // ---- 3. pick one
    QJsonObject so;
    {
        const Reply r = env.api("POST", "/api/v1/streams", QJsonObject{{QStringLiteral("search"), sid}, {QStringLiteral("result"), aviKey}});
        QCOMPARE(r.status, 201);
        so = r.json();
        QCOMPARE(so.value(QStringLiteral("contentType")).toString(), QStringLiteral("video/x-msvideo"));
        hasString(so, "download");
        hasString(so, "id");
        QCOMPARE(so.value(QStringLiteral("id")).toString().size(), 12);
        QCOMPARE(so.value(QStringLiteral("name")).toString(), QStringLiteral("big_buck_bunny_480p_surround-fix.avi"));
        QCOMPARE(so.value(QStringLiteral("ownCache")).toBool(), true);
        QCOMPARE(so.value(QStringLiteral("state")).toString(), QStringLiteral("waiting for a player"));
        const QString url = so.value(QStringLiteral("url")).toString();
        const QString prefix = QStringLiteral("http://127.0.0.1:%1/stream/%2/").arg(env.port).arg(so.value(QStringLiteral("id")).toString());
        QVERIFY2(url.startsWith(prefix), qPrintable(url));
        const QString keyPart = url.mid(prefix.size()).section(QLatin1Char('/'), 0, 0);
        QCOMPARE(keyPart.size(), 32);
        QVERIFY(url.endsWith(QStringLiteral("/big_buck_bunny_480p_surround-fix.avi")));
        // the same selection again: the same stream (200)
        const Reply again = env.api("POST", "/api/v1/streams", QJsonObject{{QStringLiteral("search"), sid}, {QStringLiteral("result"), aviKey}});
        QCOMPARE(again.status, 200);
        QCOMPARE(again.json().value(QStringLiteral("id")).toString(), so.value(QStringLiteral("id")).toString());
    }
    const QString id = so.value(QStringLiteral("id")).toString();
    // ---- 4. play the "url" (a player at its bit rate)
    auto *p = new Player(env.port, StreamEnv::pathOf(so), 0, 0, 400 * 1000);
    QVERIFY(QTest::qWaitFor([&] { return p->data().size() >= 600 * 1024; }, 30000));
    QCOMPARE(p->reply().status, 206);
    QCOMPARE(p->reply().header("accept-ranges"), QByteArray("bytes"));
    QCOMPARE(p->reply().header("content-type"), QByteArray("video/x-msvideo"));
    QCOMPARE(p->reply().header("content-range"), "bytes 0-" + QByteArray::number(data.size() - 1) + '/' + QByteArray::number(data.size()));
    // ---- 5. see what it does: every field of the document's example
    {
        QJsonObject o;
        QVERIFY(QTest::qWaitFor([&] {
            o = env.status(id);
            return o.value(QStringLiteral("state")).toString() == QLatin1String("playing") && !o.value(QStringLiteral("players")).toArray().isEmpty()
                   && o.contains(QStringLiteral("bitrateBps"));
        }, 10000));
        for (const char *k : {"bitrateBps", "cachedBytes", "failedChecks", "longestWaitMs", "served", "size", "sources", "sourcesActive", "speedBps",
                              "unverifiedServed"})
            hasNumber(o, k);
        for (const char *k : {"contentType", "download", "id", "name", "state", "transferStatus", "verification"}) hasString(o, k);
        hasBool(o, "sizeKnown");
        QCOMPARE(o.value(QStringLiteral("contentType")).toString(), QStringLiteral("video/x-msvideo"));
        QCOMPARE(o.value(QStringLiteral("download")).toString(), so.value(QStringLiteral("download")).toString());
        QCOMPARE(o.value(QStringLiteral("id")).toString(), id);
        QCOMPARE(qint64(o.value(QStringLiteral("size")).toDouble()), qint64(data.size()));
        QCOMPARE(o.value(QStringLiteral("sizeKnown")).toBool(), true);
        QVERIFY(o.value(QStringLiteral("transferStatus")).toString().startsWith(QStringLiteral("Streaming: ")));
        QVERIFY(o.value(QStringLiteral("fetchedByNetwork")).toObject().value(QStringLiteral("eD2k")).toDouble() > 0);
        QCOMPARE(o.value(QStringLiteral("networks")).toArray(), QJsonArray{QStringLiteral("eD2k")});
        const QJsonObject limits = o.value(QStringLiteral("limits")).toObject();
        QCOMPARE(limits.value(QStringLiteral("connections")).toString(), QStringLiteral("the proxy and network adapter settings apply as for every download"));
        QCOMPARE(limits.value(QStringLiteral("downloadsAtOnce")).toString(), QStringLiteral("not counted: a stream always runs"));
        QCOMPARE(limits.value(QStringLiteral("speed")).toString(), QStringLiteral("no download limit"));
        const QJsonObject pl = o.value(QStringLiteral("players")).toArray().at(0).toObject();
        for (const char *k : {"bufferTarget", "bufferedAhead", "bufferedAheadSec", "end", "firstByteMs", "from", "id", "longestWaitMs", "position", "rateBps", "sent"})
            hasNumber(pl, k);
        QCOMPARE(qint64(pl.value(QStringLiteral("end")).toDouble()), qint64(data.size()));
        QCOMPARE(pl.value(QStringLiteral("from")).toInt(-1), 0);
        const QJsonArray ranges = o.value(QStringLiteral("ranges")).toArray();
        QVERIFY(!ranges.isEmpty());
        QCOMPARE(ranges.at(0).toArray().size(), 2);
        QCOMPARE(ranges.at(0).toArray().at(0).toInt(-1), 0);
        // the stream list and the status call
        const QJsonObject list = env.api("GET", "/api/v1/streams").json();
        QCOMPARE(list.value(QStringLiteral("streams")).toArray().size(), 1);
        QVERIFY(list.contains(QStringLiteral("cache")));
        QCOMPARE(env.api("GET", "/api/v1/status").json().value(QStringLiteral("streams")).toInt(), 1);
        QCOMPARE(env.api("GET", "/api/v1/status").json().value(QStringLiteral("searches")).toInt(), 1);
    }
    // ---- 6. done: stop it, the cache goes
    {
        const Reply r = env.api("DELETE", "/api/v1/streams/" + id.toLatin1());
        QCOMPARE(r.status, 200);
        const QJsonObject o = r.json();
        QCOMPARE(o.value(QStringLiteral("message")).toString(), QStringLiteral("stopped through the API"));
        QCOMPARE(o.value(QStringLiteral("state")).toString(), QStringLiteral("ended"));
        QCOMPARE(o.value(QStringLiteral("stopped")).toBool(), true);
        QVERIFY(QTest::qWaitFor([&] { return p->failed() || p->done(); }, 10000));
        delete p;
        const Reply link = rawExchange(env.port, "GET " + StreamEnv::pathOf(so) + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(env.port)
                                                     + "\r\nConnection: close\r\n\r\n");
        QCOMPARE(link.status, 410);
        QCOMPARE(env.status(id).value(QStringLiteral("state")).toString(), QStringLiteral("ended"));   // readable for a while
        QVERIFY(env.status(id).contains(QStringLiteral("ended")));
        QCOMPARE(env.api("GET", "/api/v1/status").json().value(QStringLiteral("streams")).toInt(-1), 0);
        const QString cacheRoot = env.core->engine()->streamCacheRoot();
        QVERIFY(QTest::qWaitFor([&] { return QDir(cacheRoot).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(); }, 10000));
        // the stream's own cache never was a download of the list
        QCOMPARE(env.api("GET", "/api/v1/downloads").json().value(QStringLiteral("count")).toInt(-1), 0);
    }
    // ---- the errors the document names for the select call keep their codes
    {
        const Reply r = env.api("POST", "/api/v1/streams", QJsonObject{{QStringLiteral("search"), 99}, {QStringLiteral("result"), aviKey}});
        QCOMPARE(r.status, 404);
        QCOMPARE(r.json().value(QStringLiteral("error")).toString(), QStringLiteral("not_found"));
        QCOMPARE(env.api("DELETE", "/api/v1/searches/1").json().value(QStringLiteral("deleted")).toBool(), true);
    }
}

QTEST_MAIN(TstApiCompat)
#include "tst_api_compat.moc"
