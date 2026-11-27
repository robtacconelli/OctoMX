// SPDX-License-Identifier: GPL-3.0-or-later
// [api-2] Downloads through the local API (docs/API.md "Downloads") with a real core and local fake
// networks (nothing live): every call with its errors and methods, the empty body of the action
// calls, paging and the state filter; a document from a local source to completion with the status
// moving sensibly; pause stops the bytes at the source and resume does not fetch twice; ed2k and http
// links (a local HTTP server); removing: off the list keeps the data (and the same file continues),
// with files (partial, finished, paused, to the trash, for good), a file the download did not create
// untouched, the switch off = 403 and nothing touched; downloads of the window ("origin": "user") and
// changes made there; streams and downloads together in both orders; the search's types and networks.
#include <QtCore/QCryptographicHash>
#include <QtCore/QDirIterator>
#include <QtTest/QTest>

#include "http_plugin.h"
#include "mx/trash.h"
#include "mxhash/links.h"
#include "stream_support.h"
#include "test_server.h"

using namespace mx;
using namespace mx::stest;

namespace {

constexpr qint64 MiB = 1024 * 1024;

SourceConfig ed2kConfig(const QByteArray &data, int bps)
{
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = bps;
    c.hashSet = ed2kParts(data);
    return c;
}

// a raw call with exactly this body and Content-Type (empty = none sent)
Reply raw(StreamEnv &env, const QByteArray &method, const QByteArray &path, const QByteArray &body, const QByteArray &contentType,
          bool sendLength = true)
{
    QByteArray req = method + ' ' + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(env.port) + "\r\nAuthorization: Bearer "
                     + env.token + "\r\nConnection: close\r\n";
    if (!contentType.isEmpty()) req += "Content-Type: " + contentType + "\r\n";
    if (sendLength) req += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    req += "\r\n" + body;
    return rawExchange(env.port, req);
}

QString err(const Reply &r) { return r.json().value(QStringLiteral("error")).toString(); }

// a search for `text`: its id, once the results named in `names` are there (name -> key)
int searchFor(StreamEnv &env, const QJsonObject &query, const QStringList &names, QHash<QString, QString> *keys)
{
    const Reply s = env.api("POST", "/api/v1/searches", query);
    if (s.status != 201) return -1;
    const int sid = s.json().value(QStringLiteral("id")).toInt();
    (void)QTest::qWaitFor([&] {
        keys->clear();
        for (const QJsonValue &v : env.api("GET", "/api/v1/searches/" + QByteArray::number(sid) + "?limit=500").json().value(QStringLiteral("results")).toArray())
            keys->insert(v.toObject().value(QStringLiteral("name")).toString(), v.toObject().value(QStringLiteral("key")).toString());
        for (const QString &n : names)
            if (!keys->contains(n)) return false;
        return true;
    }, 10000);
    return sid;
}

QJsonObject getDl(StreamEnv &env, const QString &id, const QByteArray &query = QByteArray())
{
    return env.api("GET", "/api/v1/downloads/" + id.toLatin1() + query).json();
}

QString pathOf(const QJsonObject &dl) { return QDir::fromNativeSeparators(dl.value(QStringLiteral("path")).toString()); }

bool waitState(StreamEnv &env, const QString &id, const QString &state, int ms)
{
    return QTest::qWaitFor([&] { return getDl(env, id).value(QStringLiteral("state")).toString() == state; }, ms);
}

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

QStringList filesBelow(const QString &dir)
{
    QStringList out;
    QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) out << it.next();
    out.sort();
    return out;
}

} // namespace

class TstApiDownloads : public QObject {
    Q_OBJECT
private slots:
    void callsMethodsAndErrors();
    void documentToCompletion();
    void pauseStopsTheBytes();
    void linkDownloads();
    void deleteKeepAndContinue();
    void deleteWithFiles();
    void deleteSwitch();
    void deleteLaterAndNoTrash();
    void windowDownloadsAndChanges();
    void streamsAndDownloads();
    void searchTypesAndNetworks();
};

// ---------------------------------------------------------------- every call, every error
void TstApiDownloads::callsMethodsAndErrors()
{
    StreamEnv env(QStringLiteral("dl-calls"));
    TestPlugin *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    QStringList names;
    for (int i = 0; i < 5; ++i) {
        const QByteArray data = randomBytes(300 * 1024 + i, 500 + i);
        const QString n = QStringLiteral("Big Buck Bunny part %1.mkv").arg(i);
        ed->cannedHits << hitOf(n, data, hashesOf(data, 3), {ed->addSource(QStringLiteral("test-ed2k://calls%1").arg(i), ed2kConfig(data, 0),
                                                                          QStringLiteral("198.51.100.%1:4662").arg(30 + i))});
        names << n;
    }
    const QByteArray zip = randomBytes(200 * 1024, 520);
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny extras.zip"), zip, hashesOf(zip, 3),
                            {ed->addSource(QStringLiteral("test-ed2k://zip"), ed2kConfig(zip, 0), QStringLiteral("198.51.100.40:4662"))});
    // a result nobody can deliver: no source, no hash
    SearchHit nothing;
    nothing.fileName = QStringLiteral("Big Buck Bunny nothing.mkv");
    nothing.size = 1000;
    ed->cannedHits << nothing;
    env.up();
    // ---- the status call gained the downloads, the type codes and the delete switch; nothing went
    {
        const QJsonObject st = env.api("GET", "/api/v1/status").json();
        for (const char *k : {"api", "networks", "searches", "streams", "version", "cache"}) QVERIFY2(st.contains(QLatin1String(k)), k);
        const QJsonObject dls = st.value(QStringLiteral("downloads")).toObject();
        QCOMPARE(dls.value(QStringLiteral("count")).toInt(), 0);
        QCOMPARE(dls.value(QStringLiteral("byState")).toObject().size(), 12);
        QVERIFY(dls.contains(QStringLiteral("speedBps")) && dls.contains(QStringLiteral("uploadBps")));
        QCOMPARE(st.value(QStringLiteral("deleteFilesAllowed")).toBool(true), false);
        QStringList codes;
        for (const QJsonValue &v : st.value(QStringLiteral("types")).toArray()) codes << v.toObject().value(QStringLiteral("code")).toString();
        QCOMPARE(codes, FileTypeFilter::kinds());
    }
    // ---- methods
    {
        const Reply put = raw(env, "PUT", "/api/v1/downloads", "{}", "application/json");
        QCOMPARE(put.status, 405);
        const Reply r = env.api("POST", "/api/v1/downloads/pause/x");
        QCOMPARE(r.status, 404);
        const Reply g = env.api("GET", "/api/v1/downloads/pause");
        QCOMPARE(g.status, 405);
        QCOMPARE(g.header("allow"), QByteArray("POST"));
        QCOMPARE(env.api("GET", "/api/v1/downloads/d0123456789abcdef").status, 404);
        QCOMPARE(err(env.api("GET", "/api/v1/downloads/d0123456789abcdef")), QStringLiteral("not_found"));
        QCOMPARE(env.api("POST", "/api/v1/downloads/d0123456789abcdef/pause").status, 404);
        QCOMPARE(env.api("GET", "/api/v1/downloads?state=bogus").status, 400);
        QCOMPARE(env.api("DELETE", "/api/v1/downloads").status, 400);
        QCOMPARE(env.api("DELETE", "/api/v1/downloads?state=paused").status, 400);
    }
    // ---- the body of POST /downloads
    QHash<QString, QString> keys;
    const int sid = searchFor(env, QJsonObject{{QStringLiteral("text"), QStringLiteral("bunny")}}, names, &keys);
    QVERIFY(sid > 0);
    auto hit = [&](const QString &n, const QJsonObject &more = {}) {
        QJsonObject o{{QStringLiteral("search"), sid}, {QStringLiteral("result"), keys.value(n)}};
        for (auto it = more.begin(); it != more.end(); ++it) o.insert(it.key(), it.value());
        return o;
    };
    {
        QCOMPARE(raw(env, "POST", "/api/v1/downloads", "", QByteArray()).status, 415);   // a call that needs a body
        QCOMPARE(raw(env, "POST", "/api/v1/downloads", "search=1", "application/x-www-form-urlencoded").status, 415);
        QCOMPARE(raw(env, "POST", "/api/v1/downloads", "{\"search\":", "application/json").status, 400);
        QCOMPARE(err(env.api("POST", "/api/v1/downloads", QJsonObject{})), QStringLiteral("what"));
        QCOMPARE(err(env.api("POST", "/api/v1/downloads", hit(names.at(0), {{QStringLiteral("link"), QStringLiteral("magnet:?xt=urn:btih:00")}}))),
                 QStringLiteral("what"));
        // no folder, no file name, no file of this computer
        for (const char *k : {"saveDir", "path", "name"})
            QCOMPARE(err(env.api("POST", "/api/v1/downloads", hit(names.at(0), {{QLatin1String(k), QStringLiteral("/tmp/x")}}))), QStringLiteral("not_allowed"));
        for (const QString &l : {QStringLiteral("file:///etc/hostname"), QStringLiteral("/etc/hostname"), QStringLiteral("C:\\x.torrent"),
                                 QStringLiteral("ftp://198.51.100.1/x"), QStringLiteral("magnet:")}) {
            const Reply r = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("link"), l}});
            QCOMPARE(r.status, 400);
            QCOMPARE(err(r), QStringLiteral("link"));
        }
        QCOMPARE(err(env.api("POST", "/api/v1/downloads", hit(names.at(0), {{QStringLiteral("priority"), 5}}))), QStringLiteral("priority"));
        QCOMPARE(err(env.api("POST", "/api/v1/downloads", hit(names.at(0), {{QStringLiteral("paused"), QStringLiteral("yes")}}))), QStringLiteral("paused"));
        QCOMPARE(err(env.api("POST", "/api/v1/downloads", hit(names.at(0), {{QStringLiteral("files"), 3}}))), QStringLiteral("files"));
        QCOMPARE(err(env.api("POST", "/api/v1/downloads", hit(names.at(0), {{QStringLiteral("files"), QJsonArray{1}}}))), QStringLiteral("files"));   // not a bundle
        QCOMPARE(env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("search"), 999}, {QStringLiteral("result"), QStringLiteral("x")}}).status, 404);
        QCOMPARE(env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("search"), sid}, {QStringLiteral("result"), QStringLiteral("x")}}).status, 404);
        // a result nobody can deliver: said in the result, and the engine's reason when started
        QJsonObject res;
        for (const QJsonValue &v : env.api("GET", "/api/v1/searches/" + QByteArray::number(sid) + "?limit=500").json().value(QStringLiteral("results")).toArray())
            if (v.toObject().value(QStringLiteral("name")).toString() == nothing.fileName) res = v.toObject();
        QCOMPARE(res.value(QStringLiteral("downloadable")).toBool(true), false);
        QVERIFY(!res.value(QStringLiteral("notDownloadable")).toString().isEmpty());
        const Reply ns = env.api("POST", "/api/v1/downloads", hit(nothing.fileName));
        QCOMPARE(ns.status, 409);
        QCOMPARE(err(ns), QStringLiteral("no_sources"));
        QCOMPARE(ns.json().value(QStringLiteral("message")).toString(), res.value(QStringLiteral("notDownloadable")).toString());
        QCOMPARE(env.api("GET", "/api/v1/downloads").json().value(QStringLiteral("count")).toInt(), 0);
        // the engine's own reason when it does not start one: a link it cannot use
        const Reply bad = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("link"), QStringLiteral("magnet:?dn=nothing")}});
        QCOMPARE(bad.status, 409);
        QCOMPARE(err(bad), QStringLiteral("not_started"));
        QVERIFY(!bad.json().value(QStringLiteral("message")).toString().isEmpty());
        // a search still empty
        ed->respondDelayMs = 3000;
        const int empty = env.api("POST", "/api/v1/searches", QJsonObject{{QStringLiteral("text"), QStringLiteral("late")}}).json().value(QStringLiteral("id")).toInt();
        const Reply se = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("search"), empty}, {QStringLiteral("result"), QStringLiteral("x")}});
        QCOMPARE(se.status, 409);
        QCOMPARE(err(se), QStringLiteral("search_empty"));
        ed->respondDelayMs = 0;
        QCOMPARE(env.api("DELETE", "/api/v1/searches/" + QByteArray::number(empty)).status, 200);
    }
    // ---- start five, paused: the list, paging, the state filter
    QStringList ids;
    for (const QString &n : std::as_const(names)) {
        const Reply r = env.api("POST", "/api/v1/downloads", hit(n, {{QStringLiteral("paused"), true}}));
        QCOMPARE(r.status, 201);
        const QJsonObject o = r.json();
        QCOMPARE(o.value(QStringLiteral("existing")).toBool(true), false);
        QCOMPARE(o.value(QStringLiteral("state")).toString(), QStringLiteral("paused"));
        QCOMPARE(o.value(QStringLiteral("paused")).toBool(), true);
        QCOMPARE(o.value(QStringLiteral("origin")).toString(), QStringLiteral("api"));
        QCOMPARE(o.value(QStringLiteral("name")).toString(), n);
        QVERIFY(!o.value(QStringLiteral("warning")).toString().contains(QStringLiteral("harmful")));   // not a program (a small video may look like a fake)
        ids << o.value(QStringLiteral("id")).toString();
    }
    {
        // the same file again: that download, unchanged
        const Reply again = env.api("POST", "/api/v1/downloads", hit(names.at(0)));
        QCOMPARE(again.status, 200);
        QCOMPARE(again.json().value(QStringLiteral("existing")).toBool(), true);
        QCOMPARE(again.json().value(QStringLiteral("id")).toString(), ids.at(0));
        QCOMPARE(again.json().value(QStringLiteral("state")).toString(), QStringLiteral("paused"));
    }
    {
        const QJsonObject all = env.api("GET", "/api/v1/downloads").json();
        QCOMPARE(all.value(QStringLiteral("count")).toInt(), 5);
        QCOMPARE(all.value(QStringLiteral("downloads")).toArray().size(), 5);
        QCOMPARE(all.value(QStringLiteral("totals")).toObject().value(QStringLiteral("byState")).toObject().value(QStringLiteral("paused")).toInt(), 5);
        const QJsonObject first = all.value(QStringLiteral("downloads")).toArray().at(0).toObject();
        for (const char *k : {"id", "url", "name", "state", "statusText", "error", "size", "sizeKnown", "done", "progress", "speedBps", "uploadBps",
                              "etaSec", "sources", "sourcesActive", "peers", "seeds", "networks", "priority", "paused", "bundle", "origin", "stream",
                              "added", "completed"})
            QVERIFY2(first.contains(QLatin1String(k)), k);
        QVERIFY(!first.contains(QStringLiteral("path")));   // the short form
        QVERIFY(first.value(QStringLiteral("added")).toString().endsWith(QLatin1Char('Z')));
        const QJsonObject page = env.api("GET", "/api/v1/downloads?offset=1&limit=2").json();
        QCOMPARE(page.value(QStringLiteral("count")).toInt(), 5);
        QCOMPARE(page.value(QStringLiteral("offset")).toInt(), 1);
        const QJsonArray pa = page.value(QStringLiteral("downloads")).toArray();
        QCOMPARE(pa.size(), 2);
        QCOMPARE(pa.at(0).toObject().value(QStringLiteral("id")).toString(), all.value(QStringLiteral("downloads")).toArray().at(1).toObject().value(QStringLiteral("id")).toString());
        QCOMPARE(env.api("GET", "/api/v1/downloads?offset=4&limit=10").json().value(QStringLiteral("downloads")).toArray().size(), 1);
        QCOMPARE(env.api("GET", "/api/v1/downloads?state=paused").json().value(QStringLiteral("count")).toInt(), 5);
        QCOMPARE(env.api("GET", "/api/v1/downloads?state=transferring,completed").json().value(QStringLiteral("count")).toInt(), 0);
        QCOMPARE(env.api("GET", "/api/v1/status").json().value(QStringLiteral("downloads")).toObject().value(QStringLiteral("count")).toInt(), 5);
    }
    // ---- one in full
    {
        const QJsonObject o = getDl(env, ids.at(0), "?sources=1");
        for (const char *k : {"path", "hashes", "magnet", "diagnosis", "sequential", "firstLastFirst", "limits", "sourceList"})
            QVERIFY2(o.contains(QLatin1String(k)), k);
        QVERIFY(o.value(QStringLiteral("hashes")).toObject().contains(QStringLiteral("ed2k")));
        QVERIFY(o.value(QStringLiteral("hashes")).toObject().contains(QStringLiteral("sha1")));
        QVERIFY(o.value(QStringLiteral("magnet")).toString().startsWith(QStringLiteral("magnet:?")));
        QVERIFY(!getDl(env, ids.at(0)).contains(QStringLiteral("sourceList")));
    }
    // ---- the empty body of the action calls: accepted with or without a Content-Type; a body that
    // is there must still be JSON
    {
        const QByteArray p = "/api/v1/downloads/" + ids.at(0).toLatin1();
        QCOMPARE(raw(env, "POST", p + "/resume", "", QByteArray()).status, 200);
        QCOMPARE(raw(env, "POST", p + "/pause", "", QByteArray(), /*sendLength*/ false).status, 200);
        QCOMPARE(raw(env, "POST", p + "/resume", "", "application/json").status, 200);
        QCOMPARE(raw(env, "POST", p + "/pause", "{}", "application/json").status, 200);
        QCOMPARE(raw(env, "POST", p + "/pause", "x", "text/plain").status, 415);
        QCOMPARE(raw(env, "POST", p + "/pause", "{", "application/json").status, 400);
        QCOMPARE(raw(env, "POST", p + "/sources", "", QByteArray()).status, 200);
        QCOMPARE(raw(env, "POST", "/api/v1/downloads/pause", "", QByteArray()).status, 200);
        QCOMPARE(raw(env, "GET", p + "/pause", "", QByteArray()).status, 405);
        // the others still need JSON (as before)
        QCOMPARE(raw(env, "POST", p, "", QByteArray()).status, 415);
        QCOMPARE(raw(env, "POST", "/api/v1/searches", "", QByteArray()).status, 415);
    }
    // ---- pause / resume: idempotent, the new status in the answer
    {
        const QByteArray p = "/api/v1/downloads/" + ids.at(1).toLatin1();
        const Reply r1 = env.api("POST", p + "/resume");
        QCOMPARE(r1.status, 200);
        QCOMPARE(r1.json().value(QStringLiteral("paused")).toBool(), false);
        QVERIFY(waitState(env, ids.at(1), QStringLiteral("completed"), 30000));
        QCOMPARE(env.api("POST", p + "/resume").json().value(QStringLiteral("state")).toString(), QStringLiteral("completed"));
        QCOMPARE(env.api("POST", p + "/pause").json().value(QStringLiteral("state")).toString(), QStringLiteral("completed"));   // finished: nothing to pause
        QVERIFY(!env.api("POST", p + "/sources").json().value(QStringLiteral("message")).toString().isEmpty());
        const Reply r2 = env.api("POST", "/api/v1/downloads/" + ids.at(2).toLatin1() + "/pause");
        QCOMPARE(r2.json().value(QStringLiteral("state")).toString(), QStringLiteral("paused"));
        QCOMPARE(env.api("POST", "/api/v1/downloads/" + ids.at(2).toLatin1() + "/pause").json().value(QStringLiteral("state")).toString(), QStringLiteral("paused"));
    }
    // ---- change it
    {
        const QByteArray p = "/api/v1/downloads/" + ids.at(3).toLatin1();
        QCOMPARE(err(env.api("POST", p, QJsonObject{})), QStringLiteral("nothing"));
        QCOMPARE(err(env.api("POST", p, QJsonObject{{QStringLiteral("priority"), 2}})), QStringLiteral("priority"));
        QCOMPARE(err(env.api("POST", p, QJsonObject{{QStringLiteral("sequential"), 1}})), QStringLiteral("sequential"));
        QCOMPARE(err(env.api("POST", p, QJsonObject{{QStringLiteral("files"), QJsonArray{0}}})), QStringLiteral("files"));
        QCOMPARE(err(env.api("POST", p, QJsonObject{{QStringLiteral("saveDir"), QStringLiteral("/tmp")}})), QStringLiteral("not_allowed"));
        const Reply ok = env.api("POST", p, QJsonObject{{QStringLiteral("priority"), 1}, {QStringLiteral("sequential"), true}, {QStringLiteral("firstLastFirst"), true}});
        QCOMPARE(ok.status, 200);
        QCOMPARE(ok.json().value(QStringLiteral("priority")).toInt(), 1);
        QCOMPARE(ok.json().value(QStringLiteral("sequential")).toBool(), true);
        QCOMPARE(ok.json().value(QStringLiteral("firstLastFirst")).toBool(), true);
        QCOMPARE(env.core->engine()->transfer(ids.at(3)).priority, 1);
    }
    // ---- all of them
    {
        const Reply rs = env.api("POST", "/api/v1/downloads/resume");
        QCOMPARE(rs.status, 200);
        QVERIFY(rs.json().value(QStringLiteral("resumed")).toInt() >= 3);
        QVERIFY(QTest::qWaitFor([&] { return env.api("GET", "/api/v1/downloads?state=completed").json().value(QStringLiteral("count")).toInt() == 5; }, 30000));
        // a dangerous type: downloaded, with the window's warning in the answer
        const Reply z = env.api("POST", "/api/v1/downloads", hit(QStringLiteral("Big Buck Bunny extras.zip")));
        QCOMPARE(z.status, 201);
        QVERIFY2(z.json().value(QStringLiteral("warning")).toString().contains(QStringLiteral("harmful")), z.body.constData());
        const QString zid = z.json().value(QStringLiteral("id")).toString();
        QVERIFY(waitState(env, zid, QStringLiteral("completed"), 30000));
        // the user switched the warning off: none
        env.core->settings()->setValue(keys::transfers::SuppressDangerousWarning, true);
        QVERIFY(!env.api("POST", "/api/v1/downloads", hit(QStringLiteral("Big Buck Bunny extras.zip"))).json().value(QStringLiteral("warning")).toString().contains(QStringLiteral("harmful")));
        // clear the finished ones off the list: their files stay; uploads are not touched
        QStringList finishedFiles;
        for (const QString &id : std::as_const(ids)) finishedFiles << pathOf(getDl(env, id));
        const Reply c = env.api("DELETE", "/api/v1/downloads?state=completed");
        QCOMPARE(c.status, 200);
        QCOMPARE(c.json().value(QStringLiteral("cleared")).toInt(), 6);
        QCOMPARE(env.api("GET", "/api/v1/downloads").json().value(QStringLiteral("count")).toInt(), 0);
        for (const QString &f : std::as_const(finishedFiles)) QVERIFY2(QFileInfo::exists(f), qPrintable(f));
        const Reply gone = env.api("GET", "/api/v1/downloads/" + ids.at(0).toLatin1());
        QCOMPARE(gone.status, 410);
        QCOMPARE(gone.json().value(QStringLiteral("files")).toString(), QStringLiteral("keep"));
    }
}

// ---------------------------------------------------------------- a document, start to finish
void TstApiDownloads::documentToCompletion()
{
    StreamEnv env(QStringLiteral("dl-doc"));
    TestPlugin *g = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(5 * MiB + 321, 610);
    const QString name = QStringLiteral("Big Buck Bunny production notes.pdf");
    SourceConfig c;
    c.data = data;
    c.bytesPerSec = 1500 * 1000;
    c.openDelayMs = 300;
    const QString uri = QStringLiteral("test-gnut://doc");
    g->cannedHits << hitOf(name, data, hashesOf(data, 1), {g->addSource(uri, c, QStringLiteral("198.51.100.61:6346"))});
    env.up();
    QHash<QString, QString> keys;
    const int sid = searchFor(env, QJsonObject{{QStringLiteral("text"), QStringLiteral("notes")}, {QStringLiteral("type"), QStringLiteral("documents")}}, {name}, &keys);
    const Reply r = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("search"), sid}, {QStringLiteral("result"), keys.value(name)}});
    QCOMPARE(r.status, 201);
    const QString id = r.json().value(QStringLiteral("id")).toString();
    QCOMPARE(r.json().value(QStringLiteral("size")).toDouble(), double(data.size()));
    QCOMPARE(r.json().value(QStringLiteral("sizeKnown")).toBool(), true);
    // the status while it runs: the state words, done only grows, a speed, a source
    QStringList states;
    qint64 lastDone = -1;
    bool grows = true;
    int maxSpeed = 0, maxSources = 0, maxActive = 0;
    bool sawEta = false;
    QSet<QString> nets;
    QJsonObject o;
    QVERIFY(QTest::qWaitFor([&] {
        o = getDl(env, id);
        const QString st = o.value(QStringLiteral("state")).toString();
        if (states.isEmpty() || states.last() != st) states << st;
        const qint64 done = qint64(o.value(QStringLiteral("done")).toDouble());
        if (done < lastDone) grows = false;
        lastDone = done;
        maxSpeed = qMax(maxSpeed, o.value(QStringLiteral("speedBps")).toInt());
        maxSources = qMax(maxSources, o.value(QStringLiteral("sources")).toInt());
        maxActive = qMax(maxActive, o.value(QStringLiteral("sourcesActive")).toInt());
        sawEta = sawEta || o.value(QStringLiteral("etaSec")).isDouble();
        for (const QJsonValue &v : o.value(QStringLiteral("networks")).toArray()) nets.insert(v.toString());
        const double pr = o.value(QStringLiteral("progress")).toDouble();
        if (pr < 0 || pr > 1) grows = false;
        return st == QLatin1String("completed");
    }, 60000));
    qInfo("states: %s; speed at most %d B/s", qPrintable(states.join(QStringLiteral(" > "))), maxSpeed);
    QVERIFY(grows);
    QVERIFY2(states.contains(QStringLiteral("transferring")), qPrintable(states.join(QLatin1Char(' '))));
    QCOMPARE(states.last(), QStringLiteral("completed"));
    QVERIFY(maxSpeed > 100 * 1000);
    QVERIFY(maxSources >= 1 && maxActive >= 1);
    QVERIFY(sawEta);
    QCOMPARE(o.value(QStringLiteral("progress")).toDouble(), 1.0);
    QCOMPARE(qint64(o.value(QStringLiteral("done")).toDouble()), qint64(data.size()));
    QVERIFY(o.value(QStringLiteral("completed")).toString().endsWith(QLatin1Char('Z')));
    QCOMPARE(nets, QSet<QString>{QStringLiteral("Gnutella")});
    // the finished file equals the original and sits in the incoming folder
    const QString path = pathOf(o);
    QCOMPARE(QFileInfo(path).absolutePath(), env.root + QStringLiteral("/incoming"));
    QCOMPARE(readFile(path), data);
    QCOMPARE(env.api("GET", "/api/v1/downloads?state=completed").json().value(QStringLiteral("count")).toInt(), 1);
    QVERIFY(g->served(uri) <= data.size() + 256 * 1024);
}

// ---------------------------------------------------------------- pause stops the bytes
void TstApiDownloads::pauseStopsTheBytes()
{
    StreamEnv env(QStringLiteral("dl-pause"));
    TestPlugin *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(12 * MiB + 77, 620);
    const QString name = QStringLiteral("ubuntu-24.04-desktop-amd64.iso");
    const QString uri = QStringLiteral("test-ed2k://pause");
    ed->cannedHits << hitOf(name, data, hashesOf(data, 3), {ed->addSource(uri, ed2kConfig(data, 2 * 1000 * 1000), QStringLiteral("198.51.100.62:4662"))});
    env.up();
    QHash<QString, QString> keys;
    const int sid = searchFor(env, QJsonObject{{QStringLiteral("text"), QStringLiteral("ubuntu")}}, {name}, &keys);
    const QString id = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("search"), sid}, {QStringLiteral("result"), keys.value(name)}})
                           .json().value(QStringLiteral("id")).toString();
    QVERIFY(QTest::qWaitFor([&] { return ed->served(uri) >= 2 * MiB; }, 30000));
    const Reply p = env.api("POST", "/api/v1/downloads/" + id.toLatin1() + "/pause");
    QCOMPARE(p.status, 200);
    QCOMPARE(p.json().value(QStringLiteral("state")).toString(), QStringLiteral("paused"));
    QCOMPARE(p.json().value(QStringLiteral("paused")).toBool(), true);
    // what was on its way when it paused may still arrive; then nothing
    QTest::qWait(800);
    const qint64 at = ed->served(uri);
    QTest::qWait(2500);
    const qint64 after = ed->served(uri);
    qInfo("served %lld bytes at the pause (+0.8 s), %lld 2.5 s later", at, after);
    QCOMPARE(after, at);
    QVERIFY(at < data.size());
    const QJsonObject ps = getDl(env, id);
    QCOMPARE(ps.value(QStringLiteral("speedBps")).toInt(), 0);
    QCOMPARE(ps.value(QStringLiteral("sourcesActive")).toInt(), 0);
    // pausing again changes nothing
    QCOMPARE(env.api("POST", "/api/v1/downloads/" + id.toLatin1() + "/pause").json().value(QStringLiteral("state")).toString(), QStringLiteral("paused"));
    // resume: it goes on and nothing is fetched twice
    const Reply rs = env.api("POST", "/api/v1/downloads/" + id.toLatin1() + "/resume");
    QCOMPARE(rs.json().value(QStringLiteral("paused")).toBool(), false);
    QVERIFY(waitState(env, id, QStringLiteral("completed"), 60000));
    QCOMPARE(readFile(pathOf(getDl(env, id))), data);
    const qint64 total = ed->served(uri);
    qInfo("served %lld bytes for a file of %lld", total, qint64(data.size()));
    QVERIFY2(total <= data.size() + 512 * 1024, qPrintable(QString::number(total)));
}

// ---------------------------------------------------------------- links
void TstApiDownloads::linkDownloads()
{
    StreamEnv env(QStringLiteral("dl-link"));
    TestPlugin *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray data = randomBytes(3 * MiB + 5, 630);
    const FileHashes h = hashesOf(data, 2);
    ed->onFind << ed->addSource(QStringLiteral("test-ed2k://link"), ed2kConfig(data, 0), QStringLiteral("198.51.100.63:4662"));
    mxtest::TestServer http;
    env.up({createHttpPlugin()});
    // ed2k:// (sources from the network's lookup)
    const QString link = hash::formatEd2kLink(QStringLiteral("debian-12.8.0-amd64-netinst.iso"), data.size(), h);
    const Reply r = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("link"), link}, {QStringLiteral("priority"), 1}});
    QCOMPARE(r.status, 201);
    const QString id = r.json().value(QStringLiteral("id")).toString();
    QCOMPARE(r.json().value(QStringLiteral("name")).toString(), QStringLiteral("debian-12.8.0-amd64-netinst.iso"));
    QCOMPARE(r.json().value(QStringLiteral("priority")).toInt(), 1);
    QCOMPARE(r.json().value(QStringLiteral("hashes")).toObject().value(QStringLiteral("ed2k")).toString(), QString::fromLatin1(h.ed2k.toHex()));
    QVERIFY(waitState(env, id, QStringLiteral("completed"), 30000));
    QCOMPARE(readFile(pathOf(getDl(env, id))), data);
    // the same link again: the same download
    const Reply again = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("link"), link}});
    QCOMPARE(again.status, 200);
    QCOMPARE(again.json().value(QStringLiteral("id")).toString(), id);
    // http:// against a local server
    const qint64 size = 2 * MiB + 99;
    const QString url = QString::fromLatin1(http.base()) + QStringLiteral("/f/ubuntu-24.04-live-server-amd64.iso?size=%1").arg(size);
    const Reply hr = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("link"), url}, {QStringLiteral("paused"), true}});
    QCOMPARE(hr.status, 201);
    const QString hid = hr.json().value(QStringLiteral("id")).toString();
    QCOMPARE(hr.json().value(QStringLiteral("state")).toString(), QStringLiteral("paused"));
    QTest::qWait(500);
    QCOMPARE(http.count("GET", "/f/"), 0);   // paused: nothing asked
    QCOMPARE(env.api("POST", "/api/v1/downloads/" + hid.toLatin1() + "/resume").status, 200);
    QVERIFY(waitState(env, hid, QStringLiteral("completed"), 30000));
    const QJsonObject ho = getDl(env, hid);
    QCOMPARE(ho.value(QStringLiteral("name")).toString(), QStringLiteral("ubuntu-24.04-live-server-amd64.iso"));
    QCOMPARE(readFile(pathOf(ho)), mxtest::generated(mxtest::seedFor("/f/ubuntu-24.04-live-server-amd64.iso"), 0, size));
}

// ---------------------------------------------------------------- off the list, the data kept
void TstApiDownloads::deleteKeepAndContinue()
{
    StreamEnv env(QStringLiteral("dl-keep"));
    TestPlugin *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(10 * MiB + 1, 640);
    const QString name = QStringLiteral("debian-12.8.0-amd64-DVD-1.iso");
    const QString uri = QStringLiteral("test-ed2k://keep");
    ed->cannedHits << hitOf(name, data, hashesOf(data, 3), {ed->addSource(uri, ed2kConfig(data, 2 * 1000 * 1000), QStringLiteral("198.51.100.64:4662"))});
    env.up();
    QHash<QString, QString> keys;
    const int sid = searchFor(env, QJsonObject{{QStringLiteral("text"), QStringLiteral("debian")}}, {name}, &keys);
    const QJsonObject sel{{QStringLiteral("search"), sid}, {QStringLiteral("result"), keys.value(name)}};
    const QString id = env.api("POST", "/api/v1/downloads", sel).json().value(QStringLiteral("id")).toString();
    QVERIFY(QTest::qWaitFor([&] { return getDl(env, id).value(QStringLiteral("done")).toDouble() >= 3 * MiB; }, 30000));
    const QString part = pathOf(getDl(env, id));
    QVERIFY(QFileInfo::exists(part));
    // DELETE without ?files: off the list, its data stays (always allowed, the switch is off)
    const Reply d = env.api("DELETE", "/api/v1/downloads/" + id.toLatin1());
    QCOMPARE(d.status, 200);
    const QJsonObject dj = d.json();
    QCOMPARE(dj.value(QStringLiteral("files")).toString(), QStringLiteral("keep"));
    QCOMPARE(dj.value(QStringLiteral("removed")).toBool(), true);
    QVERIFY(dj.value(QStringLiteral("kept")).toArray().contains(QDir::toNativeSeparators(part)));
    QVERIFY(dj.value(QStringLiteral("deleted")).toArray().isEmpty());
    QVERIFY(QFileInfo::exists(part));
    QCOMPARE(env.api("GET", "/api/v1/downloads").json().value(QStringLiteral("count")).toInt(), 0);
    // the removed download stays readable for a while: 410 with the record
    const Reply g = env.api("GET", "/api/v1/downloads/" + id.toLatin1());
    QCOMPARE(g.status, 410);
    QCOMPARE(err(g), QStringLiteral("removed"));
    QCOMPARE(g.json().value(QStringLiteral("download")).toObject().value(QStringLiteral("name")).toString(), name);
    QCOMPARE(env.api("POST", "/api/v1/downloads/" + id.toLatin1() + "/pause").status, 410);
    const qint64 servedBefore = ed->served(uri);
    // the same file again: it continues from the data (as in the window)
    const Reply again = env.api("POST", "/api/v1/downloads", sel);
    QCOMPARE(again.status, 201);
    QCOMPARE(again.json().value(QStringLiteral("continued")).toBool(), true);
    const QString id2 = again.json().value(QStringLiteral("id")).toString();
    QVERIFY(again.json().value(QStringLiteral("done")).toDouble() >= 2 * MiB);
    QVERIFY(waitState(env, id2, QStringLiteral("completed"), 60000));
    QCOMPARE(readFile(pathOf(getDl(env, id2))), data);
    const qint64 total = ed->served(uri);
    qInfo("served %lld before the removal, %lld in all, file %lld", servedBefore, total, qint64(data.size()));
    QVERIFY2(total <= data.size() + MiB, qPrintable(QString::number(total)));
}

// ---------------------------------------------------------------- with files
void TstApiDownloads::deleteWithFiles()
{
    StreamEnv env(QStringLiteral("dl-files"));
    TestPlugin *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanFindSources);
    const QByteArray slow = randomBytes(10 * MiB, 650), fast = randomBytes(2 * MiB + 3, 651), third = randomBytes(MiB + 9, 652),
                     fourth = randomBytes(3 * MiB, 653);
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny partial.mkv"), slow, hashesOf(slow, 3),
                            {ed->addSource(QStringLiteral("test-ed2k://slow"), ed2kConfig(slow, 1000 * 1000), QStringLiteral("198.51.100.65:4662"))});
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny finished.mkv"), fast, hashesOf(fast, 3),
                            {ed->addSource(QStringLiteral("test-ed2k://fast"), ed2kConfig(fast, 0), QStringLiteral("198.51.100.66:4662"))});
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny trash.mkv"), third, hashesOf(third, 3),
                            {ed->addSource(QStringLiteral("test-ed2k://third"), ed2kConfig(third, 0), QStringLiteral("198.51.100.67:4662"))});
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny paused.mkv"), fourth, hashesOf(fourth, 3),
                            {ed->addSource(QStringLiteral("test-ed2k://fourth"), ed2kConfig(fourth, 500 * 1000), QStringLiteral("198.51.100.68:4662"))});
    env.configure = [](Settings *s) { s->setValue(keys::api::AllowDelete, true); };
    env.up();
    // files the downloads did not create, beside theirs
    const QString incoming = env.root + QStringLiteral("/incoming"), incomplete = env.root + QStringLiteral("/incomplete");
    writeFile(incoming + QStringLiteral("/my notes.txt"), "mine");
    writeFile(incomplete + QStringLiteral("/other.part"), "other");
    QHash<QString, QString> keys;
    const int sid = searchFor(env, QJsonObject{{QStringLiteral("text"), QStringLiteral("bunny")}},
                              {QStringLiteral("Big Buck Bunny partial.mkv"), QStringLiteral("Big Buck Bunny finished.mkv"),
                               QStringLiteral("Big Buck Bunny trash.mkv"), QStringLiteral("Big Buck Bunny paused.mkv")}, &keys);
    auto start = [&](const QString &n) {
        return env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("search"), sid}, {QStringLiteral("result"), keys.value(n)}})
            .json().value(QStringLiteral("id")).toString();
    };
    // ---- partial data, for good
    const QString a = start(QStringLiteral("Big Buck Bunny partial.mkv"));
    QVERIFY(QTest::qWaitFor([&] { return getDl(env, a).value(QStringLiteral("done")).toDouble() >= MiB; }, 30000));
    const QString aPart = pathOf(getDl(env, a));
    const DownloadDiskInfo adi = env.core->engine()->diskInfo(a);
    QCOMPARE(adi.paths, QStringList{aPart});
    const Reply ra = env.api("DELETE", "/api/v1/downloads/" + a.toLatin1() + "?files=delete");
    QCOMPARE(ra.status, 200);
    QCOMPARE(ra.json().value(QStringLiteral("pending")).toBool(true), false);
    QCOMPARE(ra.json().value(QStringLiteral("deleted")).toArray(), QJsonArray{QDir::toNativeSeparators(aPart)});
    QVERIFY(ra.json().value(QStringLiteral("trashed")).toArray().isEmpty());
    QVERIFY(!QFileInfo::exists(aPart));
    QVERIFY(env.core->engine()->keptDownloads().isEmpty());   // nothing kept to continue from
    // ---- a finished file, for good
    const QString b = start(QStringLiteral("Big Buck Bunny finished.mkv"));
    QVERIFY(waitState(env, b, QStringLiteral("completed"), 30000));
    const QString bFile = pathOf(getDl(env, b));
    QVERIFY(QFileInfo::exists(bFile));
    const Reply rb = env.api("DELETE", "/api/v1/downloads/" + b.toLatin1() + "?files=delete");
    QCOMPARE(rb.status, 200);
    QCOMPARE(rb.json().value(QStringLiteral("deleted")).toArray(), QJsonArray{QDir::toNativeSeparators(bFile)});
    QVERIFY(!QFileInfo::exists(bFile));
    // ---- a finished file to the trash (the test seam under MX_OS_ROOT)
    const QString c = start(QStringLiteral("Big Buck Bunny trash.mkv"));
    QVERIFY(waitState(env, c, QStringLiteral("completed"), 30000));
    const QString cFile = pathOf(getDl(env, c));
    QVERIFY(trash::redirected());
    const QStringList trashBefore = filesBelow(trash::seamDir());
    const Reply rc = env.api("DELETE", "/api/v1/downloads/" + c.toLatin1() + "?files=trash");
    QCOMPARE(rc.status, 200);
    QCOMPARE(rc.json().value(QStringLiteral("trashed")).toArray(), QJsonArray{QDir::toNativeSeparators(cFile)});
    QVERIFY(!QFileInfo::exists(cFile));
    bool inTrash = false;
    for (const QString &f : filesBelow(trash::seamDir()))
        if (!trashBefore.contains(f) && f.contains(QStringLiteral("/files/")) && readFile(f) == third) inTrash = true;
    QVERIFY(inTrash);
    // ---- a paused download
    const QString d = start(QStringLiteral("Big Buck Bunny paused.mkv"));
    QVERIFY(QTest::qWaitFor([&] { return getDl(env, d).value(QStringLiteral("done")).toDouble() >= 256 * 1024; }, 30000));
    QCOMPARE(env.api("POST", "/api/v1/downloads/" + d.toLatin1() + "/pause").status, 200);
    const QString dPart = pathOf(getDl(env, d));
    QVERIFY(QFileInfo::exists(dPart));
    const Reply rd = env.api("DELETE", "/api/v1/downloads/" + d.toLatin1() + "?files=delete");
    QCOMPARE(rd.status, 200);
    QVERIFY(!QFileInfo::exists(dPart));
    // ---- resumed in place from a file the user picked (the window's Manually Resume): that file is the
    // download's data by the engine's rule (diskInfo), and only it goes
    const QByteArray resumed = randomBytes(4 * MiB, 654);
    const QString own = incoming + QStringLiteral("/kept by the user/ubuntu-24.04-desktop-amd64.iso");
    writeFile(own, resumed.left(MiB));
    {
        QFile f(own);
        QVERIFY(f.resize(resumed.size()));
    }
    ed->onFind << ed->addSource(QStringLiteral("test-ed2k://resumed"), ed2kConfig(resumed, 200 * 1000), QStringLiteral("198.51.100.69:4662"));
    DownloadRequest rq;
    rq.uri = hash::formatEd2kLink(QStringLiteral("ubuntu-24.04-desktop-amd64.iso"), resumed.size(), hashesOf(resumed, 2));
    const QString e = env.core->engine()->resumeDownload(rq, own);
    QVERIFY(!e.isEmpty());
    const DownloadDiskInfo edi = env.core->engine()->diskInfo(e);
    QCOMPARE(edi.inPlaceFile, QFileInfo(own).absoluteFilePath());
    const Reply re = env.api("DELETE", "/api/v1/downloads/" + e.toLatin1() + "?files=delete");
    QCOMPARE(re.status, 200);
    QCOMPARE(re.json().value(QStringLiteral("inPlaceFile")).toString(), QDir::toNativeSeparators(QFileInfo(own).absoluteFilePath()));
    QJsonArray want;
    for (const QString &p : edi.paths) want << QDir::toNativeSeparators(p);
    QCOMPARE(re.json().value(QStringLiteral("deleted")).toArray(), want);
    QVERIFY(!QFileInfo::exists(own));
    // ---- nothing else was touched
    QCOMPARE(readFile(incoming + QStringLiteral("/my notes.txt")), QByteArray("mine"));
    QCOMPARE(readFile(incomplete + QStringLiteral("/other.part")), QByteArray("other"));
    QVERIFY(QFileInfo(incoming + QStringLiteral("/kept by the user")).isDir());
    // ---- the records
    QCOMPARE(env.api("GET", "/api/v1/downloads/" + b.toLatin1()).status, 410);
    QCOMPARE(env.api("GET", "/api/v1/downloads/" + b.toLatin1()).json().value(QStringLiteral("files")).toString(), QStringLiteral("delete"));
    QCOMPARE(env.api("DELETE", "/api/v1/downloads/" + b.toLatin1() + "?files=delete").status, 410);
    QCOMPARE(env.api("DELETE", "/api/v1/downloads/x?files=bogus").status, 404);
}

// ---------------------------------------------------------------- the switch
void TstApiDownloads::deleteSwitch()
{
    StreamEnv env(QStringLiteral("dl-switch"));
    TestPlugin *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(MiB + 1, 660);
    const QString name = QStringLiteral("Big Buck Bunny switch.mkv");
    ed->cannedHits << hitOf(name, data, hashesOf(data, 3), {ed->addSource(QStringLiteral("test-ed2k://sw"), ed2kConfig(data, 0), QStringLiteral("198.51.100.70:4662"))});
    env.up();
    QCOMPARE(env.core->settings()->b(keys::api::AllowDelete), false);   // off by default
    QHash<QString, QString> keys;
    const int sid = searchFor(env, QJsonObject{{QStringLiteral("text"), QStringLiteral("bunny")}}, {name}, &keys);
    const QString id = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("search"), sid}, {QStringLiteral("result"), keys.value(name)}})
                           .json().value(QStringLiteral("id")).toString();
    QVERIFY(waitState(env, id, QStringLiteral("completed"), 30000));
    const QString file = pathOf(getDl(env, id));
    // off: 403, nothing touched, still in the list
    for (const QByteArray &mode : {QByteArray("delete"), QByteArray("trash")}) {
        const Reply r = env.api("DELETE", "/api/v1/downloads/" + id.toLatin1() + "?files=" + mode);
        QCOMPARE(r.status, 403);
        QCOMPARE(err(r), QStringLiteral("delete_not_allowed"));
    }
    QCOMPARE(readFile(file), data);
    QCOMPARE(getDl(env, id).value(QStringLiteral("state")).toString(), QStringLiteral("completed"));
    QCOMPARE(env.api("GET", "/api/v1/status").json().value(QStringLiteral("deleteFilesAllowed")).toBool(true), false);
    QCOMPARE(env.api("DELETE", "/api/v1/downloads/" + id.toLatin1() + "?files=bogus").status, 400);
    // on: applies at once
    env.core->settings()->setValue(keys::api::AllowDelete, true);
    QCOMPARE(env.api("GET", "/api/v1/status").json().value(QStringLiteral("deleteFilesAllowed")).toBool(), true);
    const Reply ok = env.api("DELETE", "/api/v1/downloads/" + id.toLatin1() + "?files=delete");
    QCOMPARE(ok.status, 200);
    QVERIFY(!QFileInfo::exists(file));
}

// ---------------------------------------------------------------- the outcome later (202), a volume without a trash
void TstApiDownloads::deleteLaterAndNoTrash()
{
    StreamEnv env(QStringLiteral("dl-later"));
    TestPlugin *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray a = randomBytes(MiB + 1, 665), b = randomBytes(MiB + 2, 666);
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny later.mkv"), a, hashesOf(a, 3),
                            {ed->addSource(QStringLiteral("test-ed2k://later"), ed2kConfig(a, 0), QStringLiteral("198.51.100.77:4662"))});
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny notrash.mkv"), b, hashesOf(b, 3),
                            {ed->addSource(QStringLiteral("test-ed2k://notrash"), ed2kConfig(b, 0), QStringLiteral("198.51.100.78:4662"))});
    env.configure = [](Settings *s) { s->setValue(keys::api::AllowDelete, true); };
    env.up();
    QHash<QString, QString> keys;
    const int sid = searchFor(env, QJsonObject{{QStringLiteral("text"), QStringLiteral("bunny")}},
                              {QStringLiteral("Big Buck Bunny later.mkv"), QStringLiteral("Big Buck Bunny notrash.mkv")}, &keys);
    auto start = [&](const QString &n) {
        const QString id = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("search"), sid}, {QStringLiteral("result"), keys.value(n)}})
                               .json().value(QStringLiteral("id")).toString();
        return waitState(env, id, QStringLiteral("completed"), 30000) ? id : QString();
    };
    // the outcome does not wait (deleteWaitMs 0): 202, pending; read later: 410 with what happened
    const QString x = start(QStringLiteral("Big Buck Bunny later.mkv"));
    QVERIFY(!x.isEmpty());
    const QString xf = pathOf(getDl(env, x));
    stream::StreamService::Tuning t = env.core->streamService()->tuning();
    t.deleteWaitMs = 0;
    env.core->streamService()->setTuning(t);
    const Reply r = env.api("DELETE", "/api/v1/downloads/" + x.toLatin1() + "?files=delete");
    QCOMPARE(r.status, 202);
    QCOMPARE(r.json().value(QStringLiteral("pending")).toBool(), true);
    Reply later;
    QVERIFY(QTest::qWaitFor([&] {
        later = env.api("GET", "/api/v1/downloads/" + x.toLatin1());
        return later.status == 410 && !later.json().value(QStringLiteral("pending")).toBool(true);
    }, 10000));
    QCOMPARE(later.json().value(QStringLiteral("deleted")).toArray(), QJsonArray{QDir::toNativeSeparators(xf)});
    QVERIFY(!QFileInfo::exists(xf));
    t.deleteWaitMs = 15000;
    env.core->streamService()->setTuning(t);
    // files=trash where there is no trash: the file stays and the answer says so
    const QString y = start(QStringLiteral("Big Buck Bunny notrash.mkv"));
    QVERIFY(!y.isEmpty());
    const QString yf = pathOf(getDl(env, y));
    qputenv("MX_TRASH", "none");
    const Reply n = env.api("DELETE", "/api/v1/downloads/" + y.toLatin1() + "?files=trash");
    qunsetenv("MX_TRASH");
    QCOMPARE(n.status, 200);
    QCOMPARE(n.json().value(QStringLiteral("failed")).toArray(), QJsonArray{QDir::toNativeSeparators(yf)});
    QVERIFY(n.json().value(QStringLiteral("trashed")).toArray().isEmpty());
    QVERIFY(n.json().value(QStringLiteral("deleted")).toArray().isEmpty());
    QVERIFY(!n.json().value(QStringLiteral("errors")).toArray().isEmpty());
    QVERIFY(n.json().value(QStringLiteral("message")).toString().contains(QStringLiteral("could not be removed")));
    QCOMPARE(readFile(yf), b);
}

// ---------------------------------------------------------------- the window's downloads
void TstApiDownloads::windowDownloadsAndChanges()
{
    StreamEnv env(QStringLiteral("dl-window"));
    TestPlugin *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray data = randomBytes(8 * MiB, 670), mine = randomBytes(MiB, 671);
    const SearchSource src = ed->addSource(QStringLiteral("test-ed2k://win"), ed2kConfig(data, 1000 * 1000), QStringLiteral("198.51.100.71:4662"));
    const SearchSource src2 = ed->addSource(QStringLiteral("test-ed2k://mine"), ed2kConfig(mine, 0), QStringLiteral("198.51.100.72:4662"));
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny api.mkv"), mine, hashesOf(mine, 3), {src2});
    env.up();
    // the user downloads in the window
    DownloadRequest r;
    r.name = QStringLiteral("Big Buck Bunny window.mkv");
    r.size = data.size();
    r.hashes = hashesOf(data, 3);
    r.sources = {src};
    const QString id = env.core->engine()->download(r);
    QVERIFY(!id.isEmpty());
    QJsonObject o = getDl(env, id);
    QCOMPARE(o.value(QStringLiteral("origin")).toString(), QStringLiteral("user"));
    // paused here: paused in the window
    QCOMPARE(env.api("POST", "/api/v1/downloads/" + id.toLatin1() + "/pause").status, 200);
    QVERIFY(env.core->engine()->downloadObject(id)->isPausedByUser());
    QCOMPARE(env.core->engine()->transfer(id).state, TransferState::Paused);
    // changed in the window: the next read shows it
    env.core->engine()->resume(id);
    env.core->engine()->setPriority(id, -1);
    o = getDl(env, id);
    QCOMPARE(o.value(QStringLiteral("paused")).toBool(), false);
    QVERIFY(o.value(QStringLiteral("state")).toString() != QStringLiteral("paused"));
    QCOMPARE(o.value(QStringLiteral("priority")).toInt(), -1);
    env.core->engine()->pause(id);
    QCOMPARE(getDl(env, id).value(QStringLiteral("state")).toString(), QStringLiteral("paused"));
    // one of the API's: its origin survives a restart; the window's stays the user's
    QHash<QString, QString> keys;
    const int sid = searchFor(env, QJsonObject{{QStringLiteral("text"), QStringLiteral("bunny")}}, {QStringLiteral("Big Buck Bunny api.mkv")}, &keys);
    const QString aid = env.api("POST", "/api/v1/downloads", QJsonObject{{QStringLiteral("search"), sid}, {QStringLiteral("result"), keys.value(QStringLiteral("Big Buck Bunny api.mkv"))}})
                            .json().value(QStringLiteral("id")).toString();
    QCOMPARE(getDl(env, aid).value(QStringLiteral("origin")).toString(), QStringLiteral("api"));
    QVERIFY(waitState(env, aid, QStringLiteral("completed"), 30000));
    env.down();
    env.up();
    QCOMPARE(getDl(env, aid).value(QStringLiteral("origin")).toString(), QStringLiteral("api"));
    QCOMPARE(getDl(env, id).value(QStringLiteral("origin")).toString(), QStringLiteral("user"));
    QCOMPARE(getDl(env, id).value(QStringLiteral("state")).toString(), QStringLiteral("paused"));
    // removed in the window: gone from the API (404: the API did not remove it)
    env.core->engine()->removeDownload(id, FileRemoval::Keep);
    QCOMPARE(env.api("GET", "/api/v1/downloads/" + id.toLatin1()).status, 404);
}

// ---------------------------------------------------------------- streams and downloads
void TstApiDownloads::streamsAndDownloads()
{
    StreamEnv env(QStringLiteral("dl-streams"));
    TestPlugin *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    const QByteArray a = randomBytes(6 * MiB, 680), b = randomBytes(6 * MiB + 1, 681), c = randomBytes(6 * MiB + 2, 682), e = randomBytes(5 * MiB + 3, 683);
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny a.mp4"), a, hashesOf(a, 3),
                            {ed->addSource(QStringLiteral("test-ed2k://sa"), ed2kConfig(a, 1000 * 1000), QStringLiteral("198.51.100.73:4662"))});
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny b.mp4"), b, hashesOf(b, 3),
                            {ed->addSource(QStringLiteral("test-ed2k://sb"), ed2kConfig(b, 1000 * 1000), QStringLiteral("198.51.100.74:4662"))});
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny c.mp4"), c, hashesOf(c, 3),
                            {ed->addSource(QStringLiteral("test-ed2k://sc"), ed2kConfig(c, 2 * 1000 * 1000), QStringLiteral("198.51.100.75:4662"))});
    ed->cannedHits << hitOf(QStringLiteral("Big Buck Bunny e.mp4"), e, hashesOf(e, 3),
                            {ed->addSource(QStringLiteral("test-ed2k://se"), ed2kConfig(e, 2 * 1000 * 1000), QStringLiteral("198.51.100.76:4662"))});
    env.up();
    QHash<QString, QString> keys;
    const int sid = searchFor(env, QJsonObject{{QStringLiteral("text"), QStringLiteral("bunny")}},
                              {QStringLiteral("Big Buck Bunny a.mp4"), QStringLiteral("Big Buck Bunny b.mp4"), QStringLiteral("Big Buck Bunny c.mp4"),
                               QStringLiteral("Big Buck Bunny e.mp4")}, &keys);
    auto sel = [&](const QString &n) { return QJsonObject{{QStringLiteral("search"), sid}, {QStringLiteral("result"), keys.value(n)}}; };
    // ---- 1. a stream with "download": true, then the download is deleted: the stream ends first
    {
        QJsonObject s = sel(QStringLiteral("Big Buck Bunny a.mp4"));
        s.insert(QStringLiteral("download"), true);
        const Reply r = env.api("POST", "/api/v1/streams", s);
        QCOMPARE(r.status, 201);
        const QString streamId = r.json().value(QStringLiteral("id")).toString(), did = r.json().value(QStringLiteral("download")).toString();
        const QJsonObject dl = getDl(env, did);
        QCOMPARE(dl.value(QStringLiteral("stream")).toString(), streamId);
        QCOMPARE(dl.value(QStringLiteral("origin")).toString(), QStringLiteral("api"));
        const QByteArray link = StreamEnv::pathOf(r.json());
        Player p(env.port, link, 0, 100000);
        QVERIFY(p.waitDone(30000));
        QCOMPARE(p.data(), a.left(100000));
        const Reply del = env.api("DELETE", "/api/v1/downloads/" + did.toLatin1());
        QCOMPARE(del.status, 200);
        QCOMPARE(del.json().value(QStringLiteral("streamEnded")).toString(), streamId);
        QCOMPARE(env.status(streamId).value(QStringLiteral("state")).toString(), QStringLiteral("ended"));
        QCOMPARE(rawExchange(env.port, "GET " + link + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(env.port) + "\r\nConnection: close\r\n\r\n").status, 410);
    }
    // ---- 2. a download, then a stream on it (joined); the stream stops, the download stays; then the
    // other order: the download goes while the stream is open
    {
        const Reply r = env.api("POST", "/api/v1/downloads", sel(QStringLiteral("Big Buck Bunny b.mp4")));
        QCOMPARE(r.status, 201);
        const QString did = r.json().value(QStringLiteral("id")).toString();
        const Reply s = env.api("POST", "/api/v1/streams", sel(QStringLiteral("Big Buck Bunny b.mp4")));
        QCOMPARE(s.status, 201);
        QCOMPARE(s.json().value(QStringLiteral("ownCache")).toBool(true), false);
        QCOMPARE(s.json().value(QStringLiteral("download")).toString(), did);
        const QString streamId = s.json().value(QStringLiteral("id")).toString();
        QCOMPARE(getDl(env, did).value(QStringLiteral("stream")).toString(), streamId);
        QCOMPARE(env.api("DELETE", "/api/v1/streams/" + streamId.toLatin1()).status, 200);
        QCOMPARE(env.api("GET", "/api/v1/downloads/" + did.toLatin1()).status, 200);
        QVERIFY(getDl(env, did).value(QStringLiteral("stream")).isNull());
        const Reply s2 = env.api("POST", "/api/v1/streams", sel(QStringLiteral("Big Buck Bunny b.mp4")));
        const QString stream2 = s2.json().value(QStringLiteral("id")).toString();
        QCOMPARE(env.api("DELETE", "/api/v1/downloads/" + did.toLatin1()).json().value(QStringLiteral("streamEnded")).toString(), stream2);
        QCOMPARE(env.status(stream2).value(QStringLiteral("state")).toString(), QStringLiteral("ended"));
    }
    // ---- 3. a stream with its own cache is not a download of the list; downloading the same result
    // turns its cache into the download, and the stream goes on
    {
        const Reply s = env.api("POST", "/api/v1/streams", sel(QStringLiteral("Big Buck Bunny c.mp4")));
        QCOMPARE(s.status, 201);
        QCOMPARE(s.json().value(QStringLiteral("ownCache")).toBool(), true);
        const QString streamId = s.json().value(QStringLiteral("id")).toString(), cache = s.json().value(QStringLiteral("download")).toString();
        const QByteArray link = StreamEnv::pathOf(s.json());
        {
            Player p(env.port, link, 0, 200000);
            QVERIFY(p.waitDone(30000));
        }
        QCOMPARE(env.api("GET", "/api/v1/downloads").json().value(QStringLiteral("count")).toInt(), 0);
        const Reply nf = env.api("GET", "/api/v1/downloads/" + cache.toLatin1());
        QCOMPARE(nf.status, 404);
        QCOMPARE(nf.json().value(QStringLiteral("stream")).toString(), streamId);
        QCOMPARE(env.api("DELETE", "/api/v1/downloads/" + cache.toLatin1()).status, 404);   // a stream's cache is not deleted here
        const Reply d = env.api("POST", "/api/v1/downloads", sel(QStringLiteral("Big Buck Bunny c.mp4")));
        QCOMPARE(d.status, 201);
        QCOMPARE(d.json().value(QStringLiteral("id")).toString(), cache);
        QCOMPARE(d.json().value(QStringLiteral("fromStream")).toString(), streamId);
        QCOMPARE(d.json().value(QStringLiteral("stream")).toString(), streamId);
        QCOMPARE(env.status(streamId).value(QStringLiteral("ownCache")).toBool(true), false);
        Player p(env.port, link, 3 * MiB, 3 * MiB + 100000);
        QVERIFY(p.waitDone(30000));
        QCOMPARE(p.data(), c.mid(3 * MiB, 100000));
        // the stream stops: the download stays and completes into the incoming folder
        QCOMPARE(env.api("DELETE", "/api/v1/streams/" + streamId.toLatin1()).status, 200);
        QVERIFY(waitState(env, cache, QStringLiteral("completed"), 60000));
        const QString file = pathOf(getDl(env, cache));
        QCOMPARE(QFileInfo(file).absolutePath(), env.root + QStringLiteral("/incoming"));
        QCOMPARE(readFile(file), c);
    }
    // ---- 4. the 5.7 way: a stream with its own cache selected again with "download": true (the same
    // stream, 200); its download is in the list and stays when the stream stops
    {
        const Reply s = env.api("POST", "/api/v1/streams", sel(QStringLiteral("Big Buck Bunny e.mp4")));
        QCOMPARE(s.status, 201);
        QCOMPARE(s.json().value(QStringLiteral("ownCache")).toBool(), true);
        const QString streamId = s.json().value(QStringLiteral("id")).toString(), did = s.json().value(QStringLiteral("download")).toString();
        {
            Player p(env.port, StreamEnv::pathOf(s.json()), 0, 100000);
            QVERIFY(p.waitDone(30000));
        }
        QJsonObject again = sel(QStringLiteral("Big Buck Bunny e.mp4"));
        again.insert(QStringLiteral("download"), true);
        const Reply r = env.api("POST", "/api/v1/streams", again);
        QCOMPARE(r.status, 200);
        QCOMPARE(r.json().value(QStringLiteral("id")).toString(), streamId);
        QCOMPARE(env.api("GET", "/api/v1/downloads/" + did.toLatin1()).status, 200);
        QCOMPARE(env.api("DELETE", "/api/v1/streams/" + streamId.toLatin1()).status, 200);
        QCOMPARE(env.api("GET", "/api/v1/downloads/" + did.toLatin1()).status, 200);
        QVERIFY(waitState(env, did, QStringLiteral("completed"), 60000));
        QCOMPARE(readFile(pathOf(getDl(env, did))), e);
    }
}

// ---------------------------------------------------------------- the search: types, networks
void TstApiDownloads::searchTypesAndNetworks()
{
    StreamEnv env(QStringLiteral("dl-search"));
    TestPlugin *ed = env.addPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges);
    TestPlugin *gn = env.addPlugin(Net::Gnutella, NetworkPlugin::CanFetchRanges);
    const QStringList names{QStringLiteral("Big Buck Bunny 1080p.mkv"), QStringLiteral("Big Buck Bunny soundtrack.mp3"),
                            QStringLiteral("Big Buck Bunny script.pdf"), QStringLiteral("ubuntu-24.04-desktop-amd64.iso"),
                            QStringLiteral("Big Buck Bunny poster.png")};
    int seed = 690;
    for (const QString &n : names) {
        const QByteArray d = randomBytes(10000, quint32(seed++));
        ed->cannedHits << hitOf(n, d, hashesOf(d, 3), {ed->addSource(QStringLiteral("test-ed2k://") + QString::number(seed), ed2kConfig(d, 0), QStringLiteral("198.51.100.80:4662"))});
    }
    const QByteArray gd = randomBytes(20000, 699);
    gn->cannedHits << hitOf(QStringLiteral("debian-12.8.0-amd64-netinst.iso"), gd, hashesOf(gd, 1),
                            {gn->addSource(QStringLiteral("test-gnut://deb"), SourceConfig{}, QStringLiteral("198.51.100.81:6346"))});
    env.up();
    env.core->network()->setEnabled(Net::Gnutella, false);   // switched off in Networks
    auto results = [&](const QJsonObject &q, int expect, QJsonObject *answer = nullptr) {
        const Reply s = env.api("POST", "/api/v1/searches", q);
        if (answer) *answer = s.json();
        const QByteArray sid = QByteArray::number(s.json().value(QStringLiteral("id")).toInt());
        QStringList got;
        (void)QTest::qWaitFor([&] {
            got.clear();
            for (const QJsonValue &v : env.api("GET", "/api/v1/searches/" + sid).json().value(QStringLiteral("results")).toArray())
                got << v.toObject().value(QStringLiteral("name")).toString();
            return got.size() >= expect && !env.api("GET", "/api/v1/searches/" + sid).json().value(QStringLiteral("running")).toBool();
        }, 10000);
        got.sort();
        env.api("DELETE", "/api/v1/searches/" + sid);
        return got;
    };
    // one type
    QCOMPARE(results(QJsonObject{{QStringLiteral("text"), QStringLiteral("x")}, {QStringLiteral("type"), QStringLiteral("discimages")}}, 1),
             QStringList{QStringLiteral("ubuntu-24.04-desktop-amd64.iso")});
    QCOMPARE(results(QJsonObject{{QStringLiteral("text"), QStringLiteral("x")}, {QStringLiteral("type"), QStringLiteral("documents")}}, 1),
             QStringList{QStringLiteral("Big Buck Bunny script.pdf")});
    // several types at once
    QCOMPARE(results(QJsonObject{{QStringLiteral("text"), QStringLiteral("x")}, {QStringLiteral("types"), QJsonArray{QStringLiteral("video"), QStringLiteral("audio")}}}, 2),
             (QStringList{QStringLiteral("Big Buck Bunny 1080p.mkv"), QStringLiteral("Big Buck Bunny soundtrack.mp3")}));
    QCOMPARE(results(QJsonObject{{QStringLiteral("text"), QStringLiteral("x")}, {QStringLiteral("type"), QStringLiteral("pictures")},
                                 {QStringLiteral("types"), QJsonArray{QStringLiteral("discimages")}}}, 2),
             (QStringList{QStringLiteral("Big Buck Bunny poster.png"), QStringLiteral("ubuntu-24.04-desktop-amd64.iso")}));
    QCOMPARE(results(QJsonObject{{QStringLiteral("text"), QStringLiteral("x")}, {QStringLiteral("types"), QJsonArray{QStringLiteral("mp3")}}}, 1),
             QStringList{QStringLiteral("Big Buck Bunny soundtrack.mp3")});
    QCOMPARE(results(QJsonObject{{QStringLiteral("text"), QStringLiteral("x")}, {QStringLiteral("types"), QJsonArray{QStringLiteral("any"), QStringLiteral("video")}}}, 5).size(), 5);
    QCOMPARE(env.api("POST", "/api/v1/searches", QJsonObject{{QStringLiteral("text"), QStringLiteral("x")}, {QStringLiteral("types"), QJsonArray{QStringLiteral("films")}}}).status, 400);
    QCOMPARE(env.api("POST", "/api/v1/searches", QJsonObject{{QStringLiteral("text"), QStringLiteral("x")}, {QStringLiteral("types"), QStringLiteral("video")}}).status, 400);
    // networks: none named = every network that can be asked; a named one that is off is said
    QJsonObject ans;
    QCOMPARE(results(QJsonObject{{QStringLiteral("text"), QStringLiteral("x")}}, 5, &ans).size(), 5);
    QCOMPARE(ans.value(QStringLiteral("networks")).toArray(), QJsonArray{QStringLiteral("eD2k")});
    QVERIFY(!ans.contains(QStringLiteral("skipped")));
    QCOMPARE(results(QJsonObject{{QStringLiteral("text"), QStringLiteral("x")}, {QStringLiteral("networks"), QJsonArray{QStringLiteral("eD2k"), QStringLiteral("Gnutella")}}}, 5, &ans).size(), 5);
    QCOMPARE(ans.value(QStringLiteral("networks")).toArray(), QJsonArray{QStringLiteral("eD2k")});
    const QJsonArray skipped = ans.value(QStringLiteral("skipped")).toArray();
    QCOMPARE(skipped.size(), 1);
    QCOMPARE(skipped.at(0).toObject().value(QStringLiteral("network")).toString(), QStringLiteral("Gnutella"));
    QCOMPARE(skipped.at(0).toObject().value(QStringLiteral("reason")).toString(), QStringLiteral("switched off"));
    QVERIFY(!ans.contains(QStringLiteral("note")));
    // only networks that cannot be asked: said, and the search finds nothing
    QCOMPARE(results(QJsonObject{{QStringLiteral("text"), QStringLiteral("x")}, {QStringLiteral("networks"), QJsonArray{QStringLiteral("Kad"), QStringLiteral("Gnutella")}}}, 0, &ans).size(), 0);
    QCOMPARE(ans.value(QStringLiteral("skipped")).toArray().size(), 2);
    QVERIFY(ans.value(QStringLiteral("note")).toString().contains(QStringLiteral("finds nothing")));
    QCOMPARE(ans.value(QStringLiteral("networks")).toArray().size(), 0);
    // switched on again: asked
    env.core->network()->setEnabled(Net::Gnutella, true);
    QVERIFY(QTest::qWaitFor([&] { return env.core->network()->status(Net::Gnutella).state == ConnState::Connected; }, 5000));
    QCOMPARE(results(QJsonObject{{QStringLiteral("text"), QStringLiteral("x")}, {QStringLiteral("type"), QStringLiteral("discimages")}}, 2, &ans),
             (QStringList{QStringLiteral("debian-12.8.0-amd64-netinst.iso"), QStringLiteral("ubuntu-24.04-desktop-amd64.iso")}));
    QCOMPARE(ans.value(QStringLiteral("networks")).toArray().size(), 2);
    // the type codes in the status call, with what they match
    for (const QJsonValue &v : env.api("GET", "/api/v1/status").json().value(QStringLiteral("types")).toArray()) {
        const QJsonObject t = v.toObject();
        QVERIFY(!t.value(QStringLiteral("matches")).toString().isEmpty());
        if (t.value(QStringLiteral("code")).toString() == QLatin1String("discimages"))
            QVERIFY(t.value(QStringLiteral("extensions")).toArray().contains(QStringLiteral("iso")));
    }
}

QTEST_MAIN(TstApiDownloads)
#include "tst_api_downloads.moc"
