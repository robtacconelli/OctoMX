// SPDX-License-Identifier: GPL-3.0-or-later
// Offline tests of the web search plugin: transforms, provider loading, every response format against
// real (trimmed) samples, record → hit mapping, de-duplication, filters, and the plugin end to end
// against a local fake HTTP server (streaming, timeouts, size cap, cancel, enable/disable).
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include "hit_builder.h"
#include "http_index_provider.h"
#include "mx/plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "provider_def.h"
#include "response_parser.h"
#include "transforms.h"
#include "web_plugin.h"
#include "web_search_plugin.h"

// JSON snippets as raw string literals live in a separate header: moc cannot parse raw strings.
#ifndef Q_MOC_RUN
#include "tst_web_data.h"
#endif

using namespace mx;
using namespace mx::web;

static QByteArray sample(const QString &name)
{
    QFile f(QStringLiteral(MX_WEB_TEST_DATA "/") + name);
    if (!f.open(QIODevice::ReadOnly)) qFatal("missing sample %s", qPrintable(name));
    return f.readAll();
}

static ProviderDef builtin(const QString &id)
{
    const ProviderSet set = loadProviders({});
    for (const ProviderDef &d : set.providers)
        if (d.id == id) return d;
    qFatal("no builtin provider %s", qPrintable(id));
    return {};
}

static QByteArray hx(const char *hex) { return QByteArray::fromHex(hex); }

// ---------------------------------------------------------------- a tiny HTTP server
class FakeHttp : public QObject {
public:
    struct Route { int status = 200; QByteArray type = "application/json"; QByteArray body; int delayMs = 0; };
    QHash<QString, Route> routes;   // path (without query) → response
    QStringList requests;           // "METHOD /path?query"
    QList<QByteArray> heads;        // raw request heads
    QTcpServer server;

    FakeHttp()
    {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                auto *buf = new QByteArray;
                connect(s, &QTcpSocket::disconnected, s, [s, buf] { delete buf; s->deleteLater(); });
                connect(s, &QTcpSocket::readyRead, s, [this, s, buf] {
                    buf->append(s->readAll());
                    const int end = buf->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray head = buf->left(end);
                    int len = 0;
                    for (const QByteArray &l : head.split('\n'))
                        if (l.toLower().startsWith("content-length:")) len = l.mid(15).trimmed().toInt();
                    if (buf->size() < end + 4 + len) return;
                    const QList<QByteArray> first = head.split('\n').first().trimmed().split(' ');
                    const QString target = QString::fromLatin1(first.value(1));
                    requests << QString::fromLatin1(first.value(0)) + QLatin1Char(' ') + target;
                    heads << head;
                    const Route r = routes.value(target.section(QLatin1Char('?'), 0, 0),
                                                 Route{404, "text/plain", "not found", 0});
                    buf->clear();
                    QPointer<QTcpSocket> ps(s);
                    QTimer::singleShot(r.delayMs, s, [ps, r] {
                        if (!ps) return;
                        QByteArray resp = "HTTP/1.1 " + QByteArray::number(r.status) + " X\r\nContent-Type: " + r.type
                                          + "\r\nContent-Length: " + QByteArray::number(r.body.size())
                                          + "\r\nConnection: close\r\n\r\n" + r.body;
                        ps->write(resp);
                        ps->disconnectFromHost();
                    });
                });
            }
        });
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
};

class TestWeb : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { mx::registerMetaTypes(); }

    // ------------------------------------------------------------ transforms
    void sizes()
    {
        QCOMPARE(parseSize(QStringLiteral("123456")), qint64(123456));
        QCOMPARE(parseSize(QStringLiteral("1.4 GiB")), qint64(1503238554));
        QCOMPARE(parseSize(QStringLiteral("650.55 MB")), qint64(682151117));   // binary: YTS reports size_bytes 682151117
        QCOMPARE(parseSize(QStringLiteral("650.55 MB"), true), qint64(650550000));
        QCOMPARE(parseSize(QStringLiteral("4.44GB")), qint64(4767413699));
        QCOMPARE(parseSize(QStringLiteral("624.0 MiB")), qint64(654311424));
        QCOMPARE(parseSize(QStringLiteral("1,234 KB")), qint64(1263616));
        QCOMPARE(parseSize(QStringLiteral("1,5 GB")), qint64(1610612736));
        QCOMPARE(parseSize(QStringLiteral("700 bytes")), qint64(700));
        QCOMPARE(parseSize(QStringLiteral("2 TB")), qint64(2199023255552));
        QCOMPARE(parseSize(QStringLiteral("6517612871")), qint64(6517612871));
        QCOMPARE(parseSize(QStringLiteral("")), qint64(-1));
        QCOMPARE(parseSize(QStringLiteral("big")), qint64(-1));
        QCOMPARE(parseSize(QStringLiteral("-5 MB")), qint64(-1));
    }

    void hashes()
    {
        const QByteArray h = hx("2c6b6858d61da9543d4231a71db4b1c9264b0685");
        QCOMPARE(parseInfoHash(QStringLiteral("2C6B6858D61DA9543D4231A71DB4B1C9264B0685")), h);
        QCOMPARE(parseInfoHash(QStringLiteral("urn:btih:2c6b6858d61da9543d4231a71db4b1c9264b0685")), h);
        // base32 of the same 20 bytes
        QCOMPARE(parseInfoHash(QStringLiteral("FRVWQWGWDWUVIPKCGGTR3NFRZETEWBUF")), h);
        QCOMPARE(parseInfoHash(QStringLiteral("frvwqwgwdwuvipkcggtr3nfrzetewbuf")), h);
        QVERIFY(parseInfoHash(QStringLiteral("0000000000000000000000000000000000000000")).isEmpty());
        QVERIFY(parseInfoHash(QStringLiteral("zz6b6858d61da9543d4231a71db4b1c9264b0685")).isEmpty());
        QVERIFY(parseInfoHash(QStringLiteral("abc")).isEmpty());
        QCOMPARE(base32Decode(QStringLiteral("MZXW6YTBOI======")), QByteArray("foobar"));
        const QString magnet = QStringLiteral("magnet:?xt=urn:btih:2C6B6858D61DA9543D4231A71DB4B1C9264B0685&dn=Ubuntu%2022.04+LTS"
                                              "&tr=udp%3A%2F%2Ftracker.example%3A1337%2Fannounce&tr=http://b/announce&xl=99");
        QCOMPARE(infoHashFromMagnet(magnet), h);
        QCOMPARE(infoHashFromMagnet(QStringLiteral("magnet:?xt=urn:btih:FRVWQWGWDWUVIPKCGGTR3NFRZETEWBUF")), h);
        QCOMPARE(magnetParam(magnet, QStringLiteral("dn")), QStringLiteral("Ubuntu 22.04 LTS"));
        QCOMPARE(magnetParams(magnet, QStringLiteral("tr")),
                 QStringList({QStringLiteral("udp://tracker.example:1337/announce"), QStringLiteral("http://b/announce")}));
        QVERIFY(infoHashFromMagnet(QStringLiteral("http://x/?xt=urn:btih:2c6b6858d61da9543d4231a71db4b1c9264b0685")).isEmpty());
        QVERIFY(infoHashFromMagnet(QStringLiteral("magnet:?xt=urn:sha1:ABC")).isEmpty());
    }

    void dates()
    {
        QCOMPARE(parseDate(QStringLiteral("1652877231")), QDateTime::fromSecsSinceEpoch(1652877231, QTimeZone::UTC));
        QCOMPARE(parseDate(QStringLiteral("2026-04-25T15:35:00+00:00")), QDateTime(QDate(2026, 4, 25), QTime(15, 35), QTimeZone::UTC));
        QCOMPARE(parseDate(QStringLiteral("2025-08-10T06:15:42Z")), QDateTime(QDate(2025, 8, 10), QTime(6, 15, 42), QTimeZone::UTC));
        QCOMPARE(parseDate(QStringLiteral("2026-11-05 15:43:48")), QDateTime(QDate(2026, 11, 5), QTime(15, 43, 48), QTimeZone::UTC));
        QCOMPARE(parseDate(QStringLiteral("Tue, 03 Nov 2009 07:03:00 -0000")), QDateTime(QDate(2009, 11, 3), QTime(7, 3), QTimeZone::UTC));
        QCOMPARE(parseDate(QStringLiteral("Tue,17 Nov 2026 22:14:06 -0000")), QDateTime(QDate(2026, 11, 17), QTime(22, 14, 6), QTimeZone::UTC));
        QCOMPARE(parseDate(QStringLiteral("14 Nov 2026 11:40:20 +0200")), QDateTime(QDate(2026, 11, 14), QTime(9, 40, 20), QTimeZone::UTC));
        QCOMPARE(parseDate(QStringLiteral("Oct 25, 2026")), QDateTime(QDate(2026, 10, 25), QTime(0, 0), QTimeZone::UTC));
        QVERIFY(!parseDate(QStringLiteral("yesterday")).isValid());
        QVERIFY(!parseDate(QStringLiteral("0")).isValid());
    }

    void bundleRule()
    {
        QVERIFY(guessIsBundle(3, QStringLiteral("x.iso")));
        QVERIFY(!guessIsBundle(1, QStringLiteral("Ubuntu 22.04 LTS")));
        QVERIFY(!guessIsBundle(-1, QStringLiteral("ubuntu-22.04.5-desktop-amd64.iso")));
        QVERIFY(guessIsBundle(-1, QStringLiteral("Ubuntu 22.04 LTS")));
        QVERIFY(guessIsBundle(-1, QStringLiteral("Big.Buck.Bunny.2008.1080p.x264-GRP")));
        QVERIFY(!guessIsBundle(-1, QStringLiteral("Big.Buck.Bunny.2008.x264.BDRip.1080p.mkv")));
    }

    void magnets()
    {
        const QByteArray h = hx("2c6b6858d61da9543d4231a71db4b1c9264b0685");
        const QString m = buildMagnet(h, QStringLiteral("A & B"), 42, {QStringLiteral("udp://t:1/a"), QStringLiteral("udp://t:1/a"), QString()});
        QCOMPARE(m, QStringLiteral("magnet:?xt=urn:btih:2c6b6858d61da9543d4231a71db4b1c9264b0685&dn=A%20%26%20B&xl=42&tr=udp%3A%2F%2Ft%3A1%2Fa"));
        QCOMPARE(infoHashFromMagnet(m), h);
        QCOMPARE(magnetParam(m, QStringLiteral("dn")), QStringLiteral("A & B"));
    }

    void htmlText()
    {
        QCOMPARE(stripHtml(QStringLiteral("<b>a</b><br>b &amp; c&#39;s &lt;x&gt;")), QStringLiteral("a b & c's <x>"));
        QCOMPARE(decodeHtmlEntities(QStringLiteral("&#x41;&#66;&unknown;")), QStringLiteral("AB&unknown;"));
        QCOMPARE(normalizeForMatch(QStringLiteral("Big.Buck_Bunny (2008)")), QStringLiteral("big buck bunny 2008"));
        // real limetorrents title: UTF-8 shown as cp1252 and re-encoded
        const QString moji = QString::fromUtf8("\xc3\x90\xe2\x80\x98\xc3\x90\xc2\xbe\xc3\x90\xc2\xbb Big Buck Bunny");
        QCOMPARE(repairMojibake(moji), QString::fromUtf8("\xd0\x91\xd0\xbe\xd0\xbb Big Buck Bunny"));
        QCOMPARE(repairMojibake(QStringLiteral("Beyonc\u00e9 \u00c3 la carte")), QStringLiteral("Beyonc\u00e9 \u00c3 la carte"));
        QCOMPARE(repairMojibake(QString::fromUtf8("\xd0\x91\xd0\xbe\xd0\xbb")), QString::fromUtf8("\xd0\x91\xd0\xbe\xd0\xbb"));
    }

    // ------------------------------------------------------------ provider definitions
    void builtinProviders()
    {
        const ProviderSet set = loadProviders({}, {}, QStringLiteral("standard"));
        QVERIFY2(set.messages.isEmpty(), qPrintable(set.messages.join(QLatin1Char('\n'))));
        QStringList ids;
        for (const ProviderDef &d : set.providers) ids << d.id;
        QCOMPARE(ids, QStringList({QStringLiteral("apibay"), QStringLiteral("knaben"), QStringLiteral("torrents-csv"),
                                   QStringLiteral("bitsearch"), QStringLiteral("yts"), QStringLiteral("nyaa"),
                                   QStringLiteral("bt4g"), QStringLiteral("limetorrents"), QStringLiteral("internet-archive"),
                                   QStringLiteral("linuxtracker")}));
        for (const ProviderDef &d : set.providers) {
            QVERIFY(d.enabled);
            QVERIFY(!d.userDefined);
            QCOMPARE(d.origin, QStringLiteral("builtin"));
        }
        QCOMPARE(loadProviders({}).providers.size(), set.providers.size());   // build default profile
    }

    void defaultProfiles()
    {
        // conservative: every provider present, only the listed ones enabled
        const ProviderSet c = loadProviders({}, {}, QStringLiteral("conservative"));
        QVERIFY2(c.messages.isEmpty(), qPrintable(c.messages.join(QLatin1Char('\n'))));
        QCOMPARE(c.providers.size(), 10);
        QStringList on;
        for (const ProviderDef &d : c.providers) if (d.enabled) on << d.id;
        QCOMPARE(on, QStringList({QStringLiteral("internet-archive"), QStringLiteral("linuxtracker")}));
        // a user override file can still switch a disabled one on
        QTemporaryDir dir;
        const QString user = dir.filePath(QStringLiteral("providers.json"));
        { QFile f(user); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(R"([{"id":"nyaa","enabled":true}])"); }
        for (const ProviderDef &d : loadProviders({user}, {}, QStringLiteral("conservative")).providers)
            if (d.id == QLatin1String("nyaa")) QVERIFY(d.enabled);
        // unknown profile: warning, everything enabled
        const ProviderSet u = loadProviders({}, {}, QStringLiteral("nope"));
        QCOMPARE(u.messages.size(), 1);
        for (const ProviderDef &d : u.providers) QVERIFY(d.enabled);
    }

    void overrides()
    {
        QTemporaryDir dir;
        const QString user = dir.filePath(QStringLiteral("providers.json"));
        QFile f(user);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(kData3);
        f.close();
        const QString second = dir.filePath(QStringLiteral("more.json"));
        QFile f2(second);
        QVERIFY(f2.open(QIODevice::WriteOnly));
        f2.write(kData4);
        f2.close();
        const QString garbage = dir.filePath(QStringLiteral("garbage.json"));
        QFile f3(garbage);
        QVERIFY(f3.open(QIODevice::WriteOnly));
        f3.write("{not json");
        f3.close();
        const ProviderSet set = loadProviders({user, second, garbage, dir.filePath(QStringLiteral("missing.json"))});
        QCOMPARE(set.messages.size(), 3);   // broken, no id, garbage
        const ProviderDef *apibay = nullptr, *mine = nullptr;
        for (const ProviderDef &d : set.providers) {
            if (d.id == QLatin1String("apibay")) apibay = &d;
            if (d.id == QLatin1String("mine")) mine = &d;
            QVERIFY(d.id != QLatin1String("broken"));
        }
        QVERIFY(apibay && mine);
        QVERIFY(!apibay->enabled);
        QCOMPARE(apibay->timeoutMs, 5000);
        QCOMPARE(apibay->minIntervalMs, 1000);           // untouched keys kept
        QVERIFY(apibay->fields.contains(QStringLiteral("seeders")));
        QVERIFY(!apibay->userDefined);
        QCOMPARE(mine->name, QStringLiteral("My index v2"));
        QVERIFY(mine->userDefined);
        QCOMPARE(mine->origin, second);
    }

    void requestBuilding()
    {
        ProviderDef d = builtin(QStringLiteral("knaben"));
        BackendQuery q;
        q.text = QStringLiteral("big \"buck\" bunny");
        q.type = MediaType::Video;
        q.adultFilter = true;
        const QByteArray body = HttpIndexProvider::expandRequest(d.body, d, q, 2, false).toUtf8();
        const QJsonObject o = QJsonDocument::fromJson(body).object();
        QVERIFY2(!o.isEmpty(), body.constData());
        QCOMPARE(o.value(QLatin1String("query")).toString(), q.text);
        QCOMPARE(o.value(QLatin1String("from")).toInt(), 50);
        QCOMPARE(o.value(QLatin1String("hide_xxx")).toBool(), true);
        QCOMPARE(o.value(QLatin1String("categories")).toArray().size(), 3);
        q.type = MediaType::Any;
        QVERIFY(!QJsonDocument::fromJson(HttpIndexProvider::expandRequest(d.body, d, q, 1, false).toUtf8()).object().contains(QLatin1String("categories")));

        ProviderDef a = builtin(QStringLiteral("apibay"));
        q.text = QStringLiteral("debian netinst");
        q.type = MediaType::Audio;
        QCOMPARE(HttpIndexProvider::expandRequest(a.url, a, q, 1, true), QStringLiteral("https://apibay.org/q.php?q=debian%20netinst&cat=100"));
        q.type = MediaType::Archive;   // not mapped → "any"
        QCOMPARE(HttpIndexProvider::expandRequest(a.url, a, q, 1, true), QStringLiteral("https://apibay.org/q.php?q=debian%20netinst&cat=0"));
        ProviderDef b = builtin(QStringLiteral("bitsearch"));
        QCOMPARE(HttpIndexProvider::expandRequest(b.url, b, q, 2, true), QStringLiteral("https://bitsearch.eu/api/v1/search?q=debian+netinst&page=2"));

        // strict categories: YTS only answers Any / Video
        HttpEnvironment env;
        HttpIndexProvider yts(builtin(QStringLiteral("yts")), &env);
        BackendQuery bq;
        QVERIFY(yts.accepts(bq));
        bq.type = MediaType::Video;
        QVERIFY(yts.accepts(bq));
        bq.type = MediaType::Audio;
        QVERIFY(!yts.accepts(bq));
        HttpIndexProvider ab(a, &env);
        QVERIFY(ab.accepts(bq));
    }

    // ------------------------------------------------------------ response formats (synthetic samples
    // of the real response shapes, written by tests/web/make_fixtures.py; all content is invented)
    void apibay()
    {
        const ParseResult r = parseResponse(builtin(QStringLiteral("apibay")), sample(QStringLiteral("apibay.json")));
        QVERIFY(r.ok);
        QCOMPARE(r.records.size(), 5);
        const TorrentRecord &t = r.records.first();
        QCOMPARE(t.providerId, QStringLiteral("apibay"));
        QCOMPARE(t.name, QStringLiteral("Example OS 22.04 LTS"));
        QCOMPARE(t.btih, hx("1111111111111111111111111111111111111111"));
        QCOMPARE(t.size, qint64(3000000000));
        QCOMPARE(t.seeders, 12);
        QCOMPARE(t.leechers, 3);
        QCOMPARE(t.numFiles, 1);
        QCOMPARE(t.category, QStringLiteral("303"));
        QCOMPARE(t.type, MediaType::Software);
        QCOMPARE(t.published, QDateTime::fromSecsSinceEpoch(1700000000, QTimeZone::UTC));
        QCOMPARE(t.detailsUrl, QStringLiteral("https://thepiratebay.org/description.php?id=1000001"));
        QVERIFY(!t.adult);
        // "No results returned" row has an all-zero hash → skipped, document still OK
        const ParseResult e = parseResponse(builtin(QStringLiteral("apibay")), sample(QStringLiteral("apibay-empty.json")));
        QVERIFY(e.ok);
        QCOMPARE(e.records.size(), 0);
        QCOMPARE(e.skipped, 1);
    }

    void torrentsCsv()
    {
        const ParseResult r = parseResponse(builtin(QStringLiteral("torrents-csv")), sample(QStringLiteral("torrents-csv.json")));
        QVERIFY(r.ok);
        QCOMPARE(r.records.size(), 5);
        const TorrentRecord &t = r.records.first();
        QCOMPARE(t.name, QStringLiteral("Example OS Handbook (fictional) 2nd ed"));
        QCOMPARE(t.btih, hx("2121212121212121212121212121212121212121"));
        QCOMPARE(t.size, qint64(8000000));
        QCOMPARE(t.seeders, 27);
        QCOMPARE(t.leechers, 0);
        QCOMPARE(t.numFiles, -1);
        QCOMPARE(t.published.toSecsSinceEpoch(), qint64(1700000000));
    }

    void knaben()
    {
        const ParseResult r = parseResponse(builtin(QStringLiteral("knaben")), sample(QStringLiteral("knaben.json")));
        QVERIFY(r.ok);
        QCOMPARE(r.records.size(), 5);
        const TorrentRecord &t = r.records.first();
        QCOMPARE(t.btih, hx("3131313131313131313131313131313131313131"));
        QCOMPARE(t.size, qint64(6000000000));
        QCOMPARE(t.category, QStringLiteral("PC / Unix"));
        QCOMPARE(t.type, MediaType::Software);
        QVERIFY(t.magnet.startsWith(QLatin1String("magnet:?xt=urn:btih:31313131")));
        QCOMPARE(t.detailsUrl, QStringLiteral("https://index.example.org/description.php?id=7001"));
        QCOMPARE(t.published, QDateTime(QDate(2026, 4, 25), QTime(15, 35), QTimeZone::UTC));
        QVERIFY(t.seeders >= 0);
        QVERIFY(t.torrentUrl.isEmpty());   // "link": null
    }

    void bitsearch()
    {
        const ParseResult r = parseResponse(builtin(QStringLiteral("bitsearch")), sample(QStringLiteral("bitsearch.json")));
        QVERIFY(r.ok);
        QCOMPARE(r.records.size(), 5);
        const TorrentRecord &t = r.records.first();
        QCOMPARE(t.name, QStringLiteral("example-os-19.04-desktop-amd64.iso"));
        QCOMPARE(t.btih, hx("4141414141414141414141414141414141414141"));
        QCOMPARE(t.size, qint64(2097152000));
        QCOMPARE(t.seeders, 28);
        QCOMPARE(t.leechers, 41);
        QVERIFY(!t.published.isValid());   // createdAt: null
        QCOMPARE(t.detailsUrl, QStringLiteral("https://bitsearch.eu/torrent/fixture00000000000000001"));
    }

    void yts()
    {
        const ParseResult r = parseResponse(builtin(QStringLiteral("yts")), sample(QStringLiteral("yts.json")));
        QVERIFY(r.ok);
        QCOMPARE(r.records.size(), 4);   // 2 movies × 2 torrents (nested items path)
        const TorrentRecord &t = r.records.first();
        QCOMPARE(t.name, QStringLiteral("Sample Open Movie (2026) [720p] [x264] [web] [YTS]"));
        QCOMPARE(t.btih, hx("5151515151515151515151515151515151515151"));
        QCOMPARE(t.size, qint64(682151117));
        QCOMPARE(t.seeders, 22);
        QCOMPARE(t.type, MediaType::Video);
        QCOMPARE(t.torrentUrl, QStringLiteral("https://movies.example.org/torrent/download/5151515151515151515151515151515151515151"));
        QCOMPARE(t.detailsUrl, QStringLiteral("https://movies.example.org/movies/sample-open-movie-2026"));   // ../url = the movie's
        QVERIFY(t.magnet.contains(QLatin1String("tr=udp")));                                  // provider trackers
    }

    void nyaa()
    {
        const ParseResult r = parseResponse(builtin(QStringLiteral("nyaa")), sample(QStringLiteral("nyaa.xml")));
        QVERIFY(r.ok);
        QCOMPARE(r.records.size(), 1);
        const TorrentRecord &t = r.records.first();
        QCOMPARE(t.name, QStringLiteral("Example OS Live CD Release 3 (fictional, x86)"));
        QCOMPARE(t.btih, hx("6161616161616161616161616161616161616161"));
        QCOMPARE(t.size, qint64(654311424));
        QCOMPARE(t.seeders, 0);
        QCOMPARE(t.category, QStringLiteral("Software - Applications"));
        QCOMPARE(t.type, MediaType::Software);
        QCOMPARE(t.torrentUrl, QStringLiteral("https://rss.example.org/download/4242.torrent"));
        QCOMPARE(t.published, QDateTime(QDate(2009, 11, 3), QTime(7, 3), QTimeZone::UTC));
    }

    void bt4g()
    {
        const ParseResult r = parseResponse(builtin(QStringLiteral("bt4g")), sample(QStringLiteral("bt4g.xml")));
        QVERIFY(r.ok);
        QCOMPARE(r.records.size(), 5);
        const TorrentRecord &t = r.records.first();
        QCOMPARE(t.name, QStringLiteral("example-os-22.04.5-desktop-amd64.iso"));
        QCOMPARE(t.btih, hx("7171717171717171717171717171717171717171"));
        QCOMPARE(t.size, parseSize(QStringLiteral("4.44GB")));
        QCOMPARE(t.category, QStringLiteral("Archive file"));
        QCOMPARE(t.seeders, -1);
        QVERIFY(t.magnet.startsWith(QLatin1String("magnet:?xt=urn:btih:71717171")));
        QCOMPARE(t.published, QDateTime(QDate(2026, 11, 17), QTime(22, 14, 6), QTimeZone::UTC));
    }

    void limetorrents()
    {
        const ParseResult r = parseResponse(builtin(QStringLiteral("limetorrents")), sample(QStringLiteral("limetorrents.xml")));
        QVERIFY(r.ok);
        QCOMPARE(r.records.size(), 5);
        const TorrentRecord &t = r.records.first();
        QVERIFY(t.name.contains(QLatin1String("Example OS Users, Groups and Permissions")));
        QCOMPARE(t.btih, hx("8181818181818181818181818181818181818181"));
        QCOMPARE(t.size, qint64(1141536878));
        QCOMPARE(t.seeders, 41);
        QCOMPARE(t.leechers, 17);
        QVERIFY(t.torrentUrl.startsWith(QLatin1String("https://cache.example.org/torrent/81818181")));
        QCOMPARE(t.published, QDateTime(QDate(2026, 11, 14), QTime(9, 40, 20), QTimeZone::UTC));
        // double-escaped entities in titles are decoded ("--&amp;gt;" → "-->")
        bool sawEscaped = false, sawArrow = false;
        for (const TorrentRecord &x : r.records) {
            sawEscaped |= x.name.contains(QLatin1String("&gt;"));
            sawArrow |= x.name.contains(QLatin1String("-->"));
        }
        QVERIFY(!sawEscaped);
        QVERIFY(sawArrow);
    }

    void internetArchive()
    {
        const ProviderDef d = builtin(QStringLiteral("internet-archive"));
        const ParseResult r = parseResponse(d, sample(QStringLiteral("internet-archive.json")));
        QVERIFY(r.ok);
        QCOMPARE(r.rawItems, 5);
        QCOMPARE(r.records.size(), 3);   // two items of this (unfiltered) sample have no torrent (no btih)
        QCOMPARE(r.skipped, 2);
        const TorrentRecord &t = r.records.at(1);
        QCOMPARE(t.name, QStringLiteral("Sample Open Movie (fictional)"));
        QCOMPARE(t.btih, hx("9292929292929292929292929292929292929292"));
        QCOMPARE(t.size, qint64(167352831));
        QCOMPARE(t.type, MediaType::Video);
        QCOMPARE(t.torrentUrl, QStringLiteral("https://archive.org/download/sample_open_movie_fixture/sample_open_movie_fixture_archive.torrent"));
        QCOMPARE(t.detailsUrl, QStringLiteral("https://archive.org/details/sample_open_movie_fixture"));
        QVERIFY(t.files.isEmpty());
        QVERIFY(t.magnet.contains(QLatin1String("bt1.archive.org")));
        // details: the file list (originals only, metadata excluded) with HTTP URLs
        TorrentRecord withFiles = t;
        QVERIFY(applyDetails(d, r.contexts.at(1), sample(QStringLiteral("internet-archive-files.json")), &withFiles));
        QCOMPARE(withFiles.files.size(), 1);
        QCOMPARE(withFiles.files.first().path, QStringLiteral("Sample Open Movie - fictional.mkv"));
        QCOMPARE(withFiles.files.first().size, qint64(81782463));
        QCOMPARE(withFiles.files.first().url,
                 QStringLiteral("https://archive.org/download/sample_open_movie_fixture/Sample%20Open%20Movie%20-%20fictional.mkv"));
        QCOMPARE(withFiles.numFiles, -1);   // filtered list: the real file count stays unknown
        QVERIFY(!applyDetails(d, r.contexts.at(1), "garbage", &withFiles));

        // → hit: one BitTorrent source + one HTTP source per original file
        const SearchHit h = recordToHit(withFiles, 7, QStringLiteral("Internet Archive"),
                                        buildMagnet(withFiles.btih, withFiles.name, withFiles.size, {}));
        QVERIFY(h.isBundle);   // no extension in the name, file count unknown
        QCOMPARE(h.files.size(), 1);
        QCOMPARE(h.files.first().index, -1);
        QCOMPARE(h.sources.size(), 2);
        QCOMPARE(h.sources.at(0).net, Net::BitTorrent);
        QCOMPARE(h.sources.at(1).net, Net::Http);
        QCOMPARE(h.sources.at(1).uri, withFiles.files.first().url);
        QCOMPARE(h.sources.at(1).extra.value(QStringLiteral("bundlePath")).toString(), withFiles.files.first().path);
        QCOMPARE(h.sources.at(0).extra.value(QStringLiteral("torrentUrl")).toString(), t.torrentUrl);
    }

    void linuxtracker()
    {
        const ParseResult r = parseResponse(builtin(QStringLiteral("linuxtracker")), sample(QStringLiteral("linuxtracker.html")));
        QVERIFY(r.ok);
        QCOMPARE(r.records.size(), 5);
        const TorrentRecord &t = r.records.first();
        QCOMPARE(t.name, QStringLiteral("Example-OS-13.5-amd64.iso"));
        QCOMPARE(t.btih, hx("a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1"));
        QCOMPARE(t.category, QStringLiteral("Example Live"));
        QCOMPARE(t.size, parseSize(QStringLiteral("4.1 GB")));
        QCOMPARE(t.seeders, 7);
        QCOMPARE(t.leechers, 0);
        QCOMPARE(t.published, QDateTime(QDate(2026, 10, 25), QTime(0, 0), QTimeZone::UTC));
        QCOMPARE(t.type, MediaType::Software);
        QCOMPARE(t.torrentUrl, QStringLiteral("https://linuxtracker.org/torrents/a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1/download/"));
    }

    void tolerantParsing()
    {
        ProviderDef d = builtin(QStringLiteral("apibay"));
        // garbage / wrong shape
        QVERIFY(!parseResponse(d, "<html>Cloudflare</html>").ok);
        QVERIFY(!parseResponse(d, "").ok);
        ParseResult r = parseResponse(d, kData5);
        QVERIFY(r.ok);
        QCOMPARE(r.records.size(), 0);
        // malformed rows are skipped, good rows kept
        r = parseResponse(d, kData6);
        QVERIFY(r.ok);
        QCOMPARE(r.records.size(), 1);
        QCOMPARE(r.skipped, 6);
        QCOMPARE(r.records.first().size, qint64(-1));
        QCOMPARE(r.records.first().seeders, -1);
        // truncated RSS keeps the complete items
        QByteArray rss = sample(QStringLiteral("limetorrents.xml"));
        rss.truncate(rss.indexOf("</item>", rss.indexOf("</item>") + 1) + 7 + 40);
        r = parseResponse(builtin(QStringLiteral("limetorrents")), rss);
        QVERIFY(r.ok);
        QCOMPARE(r.records.size(), 2);
        // empty RSS channel
        r = parseResponse(builtin(QStringLiteral("nyaa")), "<?xml version=\"1.0\"?><rss><channel><title>x</title></channel></rss>");
        QVERIFY(r.ok);
        QCOMPARE(r.records.size(), 0);
        // too large
        QVERIFY(!parseResponse(d, QByteArray(5 * 1024 * 1024, ' ')).ok);
    }

    // ------------------------------------------------------------ filters
    void filters()
    {
        TorrentRecord r;
        r.providerId = QStringLiteral("p");
        r.name = QStringLiteral("Big.Buck.Bunny.2008.1080p.x264.mkv");
        r.btih = hx("2c6b6858d61da9543d4231a71db4b1c9264b0685");
        r.size = 700 * 1024 * 1024;
        FilterOptions opt;
        SearchQuery q;
        q.text = QStringLiteral("big buck bunny");
        QVERIFY(recordMatches(r, q, opt));
        q.text2 = QStringLiteral("1080p");
        QVERIFY(recordMatches(r, q, opt));
        q.text2 = QStringLiteral("720p");
        QString why;
        QVERIFY(!recordMatches(r, q, opt, &why));
        QVERIFY(why.contains(QLatin1String("720p")));
        opt.matchName = false;
        QVERIFY(recordMatches(r, q, opt));
        opt.matchName = true;
        q.text2.clear();
        q.type = MediaType::Video;
        QVERIFY(recordMatches(r, q, opt));
        q.type = MediaType::Audio;
        QVERIFY(!recordMatches(r, q, opt));
        q.type = MediaType::Any;
        q.extension = QStringLiteral(".MKV");
        QVERIFY(recordMatches(r, q, opt));
        q.extension = QStringLiteral("avi");
        QVERIFY(!recordMatches(r, q, opt));
        q.extension.clear();
        q.minSize = 800LL * 1024 * 1024;
        QVERIFY(!recordMatches(r, q, opt));
        q.minSize = 100;
        q.maxSize = 800LL * 1024 * 1024;
        QVERIFY(recordMatches(r, q, opt));
        r.size = -1;
        QVERIFY(!recordMatches(r, q, opt));   // unknown size cannot satisfy a size limit
        q.minSize = q.maxSize = 0;
        QVERIFY(recordMatches(r, q, opt));
        // adult filter: by provider category flag and by keyword
        opt.adultFilter = true;
        opt.adultKeywords = defaultAdultKeywords();
        QVERIFY(recordMatches(r, q, opt));
        r.adult = true;
        QVERIFY(!recordMatches(r, q, opt));
        r.adult = false;
        r.name = QStringLiteral("Big Buck Bunny XXX parody");
        QVERIFY(!recordMatches(r, q, opt));
        r.name = QStringLiteral("Big Buck Bunny Sussex edition");   // whole words only
        QVERIFY(recordMatches(r, q, opt));
        // the apibay adult category regex sets the flag
        const ParseResult pr = parseResponse(builtin(QStringLiteral("apibay")),
            kData7);
        QCOMPARE(pr.records.size(), 1);
        QVERIFY(pr.records.first().adult);
        // bundle extension check uses the file list
        TorrentRecord b;
        b.name = QStringLiteral("Some Album");
        b.btih = r.btih;
        b.files = {RecordFile{QStringLiteral("01.flac"), 10, {}}, RecordFile{QStringLiteral("cover.jpg"), 1, {}}};
        q.text = QStringLiteral("album");
        q.extension = QStringLiteral("flac");
        QVERIFY(recordMatches(b, q, opt));
        QCOMPARE(guessType(b), MediaType::Audio);
        // [ui-f] names without an extension are not hidden by a type ("A Video search must not come
        // back empty"); several extensions; the category of a bundle
        TorrentRecord n;
        n.name = QStringLiteral("Big Buck Bunny");
        n.btih = r.btih;
        n.size = 900LL * 1024 * 1024;
        SearchQuery v;
        v.text = QStringLiteral("big buck bunny");
        FileTypeFilter::fromCode(QStringLiteral("video")).applyTo(v);
        QVERIFY(recordMatches(n, v, opt));                   // unknown type: kept
        n.type = MediaType::Audio;                            // the index says music
        QVERIFY(!recordMatches(n, v, opt));
        n.type = MediaType::Any;
        n.numFiles = 2;
        n.files = {RecordFile{QStringLiteral("bbb/Big Buck Bunny.mkv"), 900LL * 1024 * 1024, {}},
                   RecordFile{QStringLiteral("bbb/Big Buck Bunny.nfo"), 4096, {}}};
        QVERIFY(recordMatches(n, v, opt));                    // the dominant file: video
        FileTypeFilter::fromCode(QStringLiteral("documents")).applyTo(v);
        QVERIFY(!recordMatches(n, v, opt));
        FileTypeFilter::fromCode(QStringLiteral("custom:srt,nfo")).applyTo(v);
        QVERIFY(recordMatches(n, v, opt));                    // a listed file
        FileTypeFilter::fromCode(QStringLiteral("custom:srt,ass")).applyTo(v);
        QVERIFY(!recordMatches(n, v, opt));
        FileTypeFilter::fromCode(QStringLiteral("lossless")).applyTo(v);
        TorrentRecord ost;
        ost.name = QStringLiteral("Big Buck Bunny OST [FLAC]");
        ost.btih = r.btih;
        ost.numFiles = 12;
        QVERIFY(recordMatches(ost, v, opt));                  // the extension written as a word
        // records of a query sent with the type's category are of that type unless they say otherwise
        ProviderDef cat;
        cat.categories.insert(int(MediaType::Any), QStringLiteral("0"));
        cat.categories.insert(int(MediaType::Video), QStringLiteral("200"));
        cat.hasAnyCategory = true;
        BackendQuery cq;
        cq.type = MediaType::Video;
        TorrentRecord plainName;
        plainName.name = QStringLiteral("Sintel");
        TorrentRecord typedRec = plainName;
        typedRec.type = MediaType::Audio;
        QList<TorrentRecord> recs = {plainName, typedRec};
        HttpIndexProvider::tagQueryCategory(cat, cq, &recs);
        QCOMPARE(recs.at(0).type, MediaType::Video);
        QCOMPARE(recs.at(1).type, MediaType::Audio);
        cq.type = MediaType::Archive;                         // no archive category: nothing said
        recs = {plainName};
        HttpIndexProvider::tagQueryCategory(cat, cq, &recs);
        QCOMPARE(recs.at(0).type, MediaType::Any);
    }

    void typeGuess()
    {
        TorrentRecord r;
        r.name = QStringLiteral("Some Show S01E02 720p WEB H264");
        QCOMPARE(guessType(r), MediaType::Video);
        r.name = QStringLiteral("Artist - Album (2020) [FLAC]");
        QCOMPARE(guessType(r), MediaType::Audio);
        r.name = QStringLiteral("debian-12.5.0-amd64-netinst.iso");
        QCOMPARE(guessType(r), MediaType::Software);
        r.name = QStringLiteral("Ubuntu 22.04 LTS");
        QCOMPARE(guessType(r), MediaType::Software);
        r.name = QStringLiteral("Random words");
        QCOMPARE(guessType(r), MediaType::Other);
        r.type = MediaType::Image;
        QCOMPARE(guessType(r), MediaType::Image);
        r.type = MediaType::Archive;   // provider category vs. a known extension: the extension wins
        r.name = QStringLiteral("debian-13.7.0-amd64-netinst.iso");
        QCOMPARE(guessType(r), MediaType::Software);
    }

    // ------------------------------------------------------------ hits + dedupe
    void mergeAcrossProviders()
    {
        const QByteArray h = hx("2c6b6858d61da9543d4231a71db4b1c9264b0685");
        TorrentRecord a;
        a.providerId = QStringLiteral("a");
        a.name = QStringLiteral("Ubuntu 22.04 LTS");
        a.btih = h;
        a.size = 3654957056;
        a.seeders = 50;
        a.leechers = 5;
        a.magnet = QStringLiteral("magnet:?xt=urn:btih:2c6b6858d61da9543d4231a71db4b1c9264b0685&tr=udp%3A%2F%2Fprov.example%3A1%2Fannounce");
        TorrentRecord b = a;
        b.providerId = QStringLiteral("b");
        b.name = QStringLiteral("ubuntu-22.04-desktop-amd64.iso");
        b.seeders = 80;
        b.numFiles = 1;
        b.magnet.clear();
        TorrentRecord c;
        c.providerId = QStringLiteral("a");
        c.name = QStringLiteral("other");
        c.btih = hx("d540fc48eb12f2833163eed6421d449dd8f1ce1f");
        const QStringList trackers = {QStringLiteral("udp://tracker.opentrackr.org:1337/announce")};
        HitMerger m(2);
        QCOMPARE(m.add(a, 9, QStringLiteral("A"), trackers), 0);
        QCOMPARE(m.add(b, 9, QStringLiteral("B"), trackers), 0);
        QCOMPARE(m.add(a, 9, QStringLiteral("A"), trackers), 0);   // same provider again: replaced, not added
        QCOMPARE(m.add(c, 9, QStringLiteral("A"), trackers), 1);
        TorrentRecord d = c;
        d.btih = hx("0000000000000000000000000000000000000001");
        QCOMPARE(m.add(d, 9, QStringLiteral("A"), trackers), -1);  // limit 2 reached
        QCOMPARE(m.size(), 2);
        const SearchHit &hit = m.hit(0);
        QCOMPARE(hit.queryId, quint32(9));
        QCOMPARE(hit.fileName, QStringLiteral("Ubuntu 22.04 LTS"));   // first name kept
        QCOMPARE(hit.hashes.btih, h);
        QCOMPARE(hit.hashes.btFileIndex, -1);
        QCOMPARE(hit.size, qint64(3654957056));
        QVERIFY(!hit.isBundle);                                      // provider b said 1 file
        QCOMPARE(hit.sources.size(), 2);
        QCOMPARE(hit.sources.at(0).seeds, 50);                       // ascending: max seeds last
        QCOMPARE(hit.sources.at(1).seeds, 80);
        QCOMPARE(hit.sources.at(1).extra.value(QStringLiteral("provider")).toString(), QStringLiteral("b"));
        QCOMPARE(hit.sources.at(0).extra.value(QStringLiteral("providers")).toStringList(), QStringList({QStringLiteral("a"), QStringLiteral("b")}));
        for (const SearchSource &s : hit.sources) {
            QCOMPARE(s.net, Net::BitTorrent);
            QCOMPARE(s.uri, hit.sources.first().uri);   // one canonical magnet per torrent
        }
        const QString magnet = hit.sources.first().uri;
        QCOMPARE(infoHashFromMagnet(magnet), h);
        QCOMPARE(magnetParam(magnet, QStringLiteral("dn")), QStringLiteral("Ubuntu 22.04 LTS"));
        QVERIFY(magnetParams(magnet, QStringLiteral("tr")).contains(QStringLiteral("udp://tracker.opentrackr.org:1337/announce")));
        QVERIFY(magnetParams(magnet, QStringLiteral("tr")).contains(QStringLiteral("udp://prov.example:1/announce")));
        QCOMPARE(m.hit(1).sources.size(), 1);
        QVERIFY(m.hit(1).isBundle);   // "other": no extension, unknown file count
        TorrentRecord c2 = c;          // another index names it like a file → not a bundle
        c2.providerId = QStringLiteral("z");
        c2.name = QStringLiteral("other.iso");
        QCOMPARE(m.add(c2, 9, QStringLiteral("Z"), trackers), 1);
        QVERIFY(!m.hit(1).isBundle);
        QCOMPARE(m.hit(1).fileName, QStringLiteral("other"));
    }

    // ------------------------------------------------------------ the plugin end to end (local server)
    void pluginEndToEnd()
    {
        FakeHttp http;
        // fast JSON provider (2 hits, one of them shared with "rss"), RSS provider, slow and huge ones
        http.routes.insert(QStringLiteral("/json"), {200, "application/json", kData8, 0});
        http.routes.insert(QStringLiteral("/rss"), {200, "application/rss+xml",
            "<rss><channel><item><title>Big.Buck.Bunny.1080p.mkv</title><hash>2C6B6858D61DA9543D4231A71DB4B1C9264B0685</hash>"
            "<seeds>99</seeds></item><item><title>Big Buck Bunny 4K</title><hash>45008e48c8800b7d7643337b2e70a634e4c69f6a</hash>"
            "<seeds>5</seeds></item></channel></rss>", 300});
        http.routes.insert(QStringLiteral("/slow"), {200, "application/json", "[]", 4000});
        http.routes.insert(QStringLiteral("/huge"), {200, "application/json", QByteArray(5 * 1024 * 1024, ' '), 0});
        const QString b = http.base();
        const QByteArray defs = QString::fromUtf8(kData1).arg(b).toUtf8();

        QTemporaryDir dir;
        SimpleSettings settings;
        settings.setValue(wkeys::ProbeOnStart, false);
        settings.setValue(keys::search::AdultFilter, true);
        PluginContext ctx;
        ctx.settings = &settings;
        ctx.dataDir = dir.path();
        WebSearchPlugin plugin;
        plugin.setBuiltinProviders(defs);
        plugin.init(ctx);
        QCOMPARE(plugin.net(), Net::Web);
        QCOMPARE(plugin.capabilities(), int(NetworkPlugin::CanSearch));
        QCOMPARE(plugin.backends().size(), 4);
        plugin.start();
        QCOMPARE(plugin.status().state, ConnState::Connecting);

        QSignalSpy hits(&plugin, &NetworkPlugin::searchHits);
        QSignalSpy done(&plugin, &NetworkPlugin::searchDone);
        SearchQuery q;
        q.id = 42;
        q.text = QStringLiteral("big buck");
        q.text2 = QStringLiteral("bunny");
        plugin.search(q);
        QVERIFY(done.wait(8000));
        QCOMPARE(done.size(), 1);
        QCOMPARE(done.first().first().toUInt(), 42u);
        QVERIFY(hits.size() >= 2);    // streamed: one batch per provider
        QHash<QString, SearchHit> byHash;
        for (const auto &args : hits) {
            QCOMPARE(args.at(0).toUInt(), 42u);
            for (const SearchHit &h : args.at(1).value<QList<SearchHit>>())
                byHash.insert(QString::fromLatin1(h.hashes.btih.toHex()), h);
        }
        QCOMPARE(byHash.size(), 2);   // XXX dropped (adult filter), "Unrelated" dropped (keywords)
        const SearchHit merged = byHash.value(QStringLiteral("2c6b6858d61da9543d4231a71db4b1c9264b0685"));
        QCOMPARE(merged.sources.size(), 2);
        QCOMPARE(merged.sources.last().seeds, 99);
        QCOMPARE(merged.sources.last().extra.value(QStringLiteral("provider")).toString(), QStringLiteral("rss"));
        QVERIFY(merged.sources.last().uri.startsWith(QLatin1String("magnet:?xt=urn:btih:2c6b6858")));

        // health: json + rss ok; slow timed out; huge rejected by the size cap
        QCOMPARE(plugin.backend(QStringLiteral("json"))->health().state, BackendState::Ok);
        QCOMPARE(plugin.backend(QStringLiteral("rss"))->health().state, BackendState::Ok);
        QCOMPARE(plugin.backend(QStringLiteral("slow"))->health().state, BackendState::BackingOff);
        QCOMPARE(plugin.backend(QStringLiteral("slow"))->health().lastError, QStringLiteral("timed out"));
        QVERIFY(plugin.backend(QStringLiteral("huge"))->health().lastError.contains(QLatin1String("larger")));
        NetworkStatus st = plugin.status();
        QCOMPARE(st.state, ConnState::Connected);
        QCOMPARE(st.statusText, QStringLiteral("2 of 4 indexes responding"));
        const QList<ServerEntry> rows = plugin.servers();
        QCOMPARE(rows.size(), 4);
        QCOMPARE(rows.at(0).net, Net::Web);
        QCOMPARE(rows.at(0).id, QStringLiteral("json"));
        QCOMPARE(rows.at(0).state, ConnState::Connected);
        // [core-5] the last answer's result count is in the description, not in "files"
        QVERIFY2(rows.at(0).description.startsWith(QLatin1String("127.0.0.1, ")), qPrintable(rows.at(0).description));
        QVERIFY(rows.at(0).description.endsWith(QLatin1String(" results last search")));
        QCOMPARE(rows.at(0).files, qint64(-1));
        QVERIFY(rows.at(0).pingMs >= 0);
        QCOMPARE(rows.at(2).state, ConnState::Error);

        // a second search skips the providers in back-off
        http.requests.clear();
        done.clear();
        q.id = 43;
        plugin.search(q);
        QVERIFY(done.wait(5000));
        QCOMPARE(http.requests.size(), 2);

        // networks filter: an eD2k-only search is not answered
        done.clear();
        hits.clear();
        q.id = 44;
        q.networks = {Net::ED2K};
        plugin.search(q);
        QVERIFY(done.wait(1000));
        QCOMPARE(hits.size(), 0);
        q.networks = {Net::BitTorrent};

        // disable a provider: persisted in the settings, not asked any more
        plugin.disconnectServer(QStringLiteral("rss"));
        QCOMPARE(settings.value(wkeys::ProviderEnabled).toMap().value(QStringLiteral("rss")).toBool(), false);
        http.requests.clear();
        done.clear();
        q.id = 45;
        plugin.search(q);
        QVERIFY(done.wait(5000));
        QCOMPARE(http.requests, QStringList({QStringLiteral("GET /json?q=big%20buck%20bunny")}));
        // a new plugin instance restores the toggle
        {
            WebSearchPlugin p2;
            p2.setBuiltinProviders(defs);
            p2.init(ctx);
            QVERIFY(!p2.backend(QStringLiteral("rss"))->isEnabled());
            QVERIFY(p2.backend(QStringLiteral("json"))->isEnabled());
        }
        plugin.connectServer(QStringLiteral("rss"));
        QVERIFY(plugin.backend(QStringLiteral("rss"))->isEnabled());

        // cancel: no hits / done for a cancelled search
        hits.clear();
        done.clear();
        q.id = 46;
        plugin.search(q);
        plugin.cancelSearch(46);
        QVERIFY(!done.wait(1500));
        QCOMPARE(hits.size(), 0);

        // stop: offline
        plugin.stop();
        QCOMPARE(plugin.status().state, ConnState::Disconnected);
        done.clear();
        q.id = 47;
        plugin.search(q);
        QVERIFY(done.wait(1000));
    }

    void addAndRemoveServer()
    {
        QTemporaryDir dir;
        SimpleSettings settings;
        settings.setValue(wkeys::ProbeOnStart, false);
        PluginContext ctx;
        ctx.settings = &settings;
        ctx.dataDir = dir.path();
        WebSearchPlugin plugin;
        plugin.init(ctx);
        const int builtinCount = plugin.backends().size();
        QCOMPARE(builtinCount, 10);
        QSignalSpy logs(&plugin, &NetworkPlugin::logMessage);
        // a bad file is rejected with a log message
        const QString bad = dir.filePath(QStringLiteral("bad.json"));
        { QFile f(bad); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(kData9); }
        plugin.addServer(bad, QString());
        QCOMPARE(plugin.backends().size(), builtinCount);
        QVERIFY(logs.last().first().toString().contains(QLatin1String("cannot add")));
        // a good one is installed into <dataDir>/providers.d and loaded
        const QString good = dir.filePath(QStringLiteral("good.json"));
        { QFile f(good); QVERIFY(f.open(QIODevice::WriteOnly));
          f.write(kData10); }
        plugin.addServer(good, QString());
        QCOMPARE(plugin.backends().size(), builtinCount + 1);
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("providers.d/my-index.json"))));
        SearchBackend *mine = plugin.backend(QStringLiteral("my-index"));
        QVERIFY(mine && mine->isUserDefined());
        QCOMPARE(mine->displayName(), QStringLiteral("My Index"));
        bool found = false;
        for (const ServerEntry &e : plugin.servers())
            if (e.id == QLatin1String("my-index")) { found = true; QVERIFY(e.isStatic); QCOMPARE(e.description, QStringLiteral("example.org")); }
        QVERIFY(found);
        // an override file for a built-in provider is accepted too
        { QFile f(good); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(kData11); }
        plugin.addServer(good, QString());
        QCOMPARE(plugin.backend(QStringLiteral("apibay"))->displayName(), QStringLiteral("TPB renamed"));
        // removing a user provider deletes its file; removing a built-in one only disables it
        plugin.removeServer(QStringLiteral("my-index"));
        QVERIFY(!plugin.backend(QStringLiteral("my-index")));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("providers.d/my-index.json"))));
        plugin.removeServer(QStringLiteral("bt4g"));
        QVERIFY(plugin.backend(QStringLiteral("bt4g")));
        QVERIFY(!plugin.backend(QStringLiteral("bt4g"))->isEnabled());
        // <dataDir>/providers.json disables / extends
        { QFile f(dir.filePath(QStringLiteral("providers.json"))); QVERIFY(f.open(QIODevice::WriteOnly));
          f.write(kData12); }
        settings.setValue(wkeys::ProviderEnabled, QVariantMap());
        plugin.reloadProviders();
        QVERIFY(!plugin.backend(QStringLiteral("yts"))->isEnabled());
        QCOMPARE(plugin.backend(QStringLiteral("bt4g"))->isEnabled(), builtin(QStringLiteral("bt4g")).enabled);   // back to the build's default
    }

    void proxySettings()
    {
        SimpleSettings settings;
        settings.setValue(keys::connection::ProxyType, QStringLiteral("socks5"));
        settings.setValue(keys::connection::ProxyHost, QStringLiteral("127.0.0.1"));
        settings.setValue(keys::connection::ProxyPort, 9050);
        settings.setValue(wkeys::UserAgent, QStringLiteral("TestAgent/1"));
        settings.setValue(wkeys::ProbeOnStart, false);
        QTemporaryDir dir;
        PluginContext ctx;
        ctx.settings = &settings;
        ctx.dataDir = dir.path();
        WebSearchPlugin plugin;
        plugin.init(ctx);
        auto *nam = plugin.findChild<QNetworkAccessManager *>();
        QVERIFY(nam);
        QCOMPARE(nam->proxy().type(), QNetworkProxy::Socks5Proxy);
        QCOMPARE(nam->proxy().port(), quint16(9050));
        settings.setValue(keys::connection::ProxyType, QStringLiteral("none"));
        QCOMPARE(nam->proxy().type(), QNetworkProxy::NoProxy);

        // the configured User-Agent is sent
        FakeHttp http;
        http.routes.insert(QStringLiteral("/s"), {200, "application/json", "[]", 0});
        const QByteArray defs = QString::fromUtf8(kData2).arg(http.base()).toUtf8();
        WebSearchPlugin p2;
        p2.setBuiltinProviders(defs);
        p2.init(ctx);
        p2.start();
        QSignalSpy done(&p2, &NetworkPlugin::searchDone);
        SearchQuery q;
        q.id = 1;
        q.text = QStringLiteral("x");
        p2.search(q);
        QVERIFY(done.wait(5000));
        QCOMPARE(p2.backend(QStringLiteral("s"))->health().state, BackendState::Ok);
        QCOMPARE(p2.status().statusText, QStringLiteral("1 of 1 indexes responding"));
        QCOMPARE(http.heads.size(), 1);
        QVERIFY(http.heads.first().contains("User-Agent: TestAgent/1"));
    }
};

QTEST_GUILESS_MAIN(TestWeb)
#include "tst_web.moc"
