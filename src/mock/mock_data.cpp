// SPDX-License-Identifier: GPL-3.0-or-later
#include "mock/mock_data.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QRegularExpression>
#include <QtCore/QtMath>

namespace mx::mock {

namespace {

QByteArray fakeHash(const QString &seed, int len)
{
    QByteArray h = QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256);
    return h.left(len);
}

int durationFor(qint64 size, int kbps)
{
    if (kbps <= 0)
        return -1;
    return int(size * 8 / (qint64(kbps) * 1000));
}

SearchSource src(Net n, const QString &userName, const QString &conn, int freeSlots, int total,
                 int queue = -1, int ping = -1)
{
    SearchSource s;
    s.net = n;
    s.user = user(n, userName);
    s.connection = conn;
    s.freeSlots = freeSlots;
    s.totalSlots = total;
    s.pingMs = ping;
    if (queue >= 0)
        s.extra.insert(QStringLiteral("queue"), queue);
    return s;
}

// [qa-1] owner request: modern connection types (fast = VDSL and faster, slow = Unknown / DSL / 4G)
const char *kConnFast[] = {"FTTB", "VDSL", "Cable", "FTTH"};
const char *kConnSlow[] = {"4G", "Unknown", "DSL", "DSL", "4G"};

QString userName(QRandomGenerator &rng, const char *const *stems, int n)
{
    return QStringLiteral("%1%2_%3")
        .arg(QString::fromLatin1(stems[rng.bounded(n)]))
        .arg(rng.bounded(10, 999))
        .arg(rng.bounded(10000, 99999));
}

// [demo-1] invented names only: no handle of a real user (the old ones were read off screenshots of the
// original program), no real artist, band or title anywhere in the demo
const char *kStems[] = {"Quill", "t0astcrumb", "tuikku", "QWmarbleX", "brimo", "pastinak", "mintymoth",
                        "zqvtra", "MangoLassi", "fuzzyfelts", "PLONKS", "Bobwib", "dorimunt", "biscotta",
                        "Twirl", "Knot4Puzzle", "mrgrom", "Tweed", "robot", "Pim", "mxfan", "oldskool",
                        "dj_tomek", "Kalle", "zesty", "lunchbox", "Shiro", "ManiaX", "bluefox", "Tia"};
constexpr int kStemCount = int(sizeof(kStems) / sizeof(kStems[0]));

// --------------------------------------------------------------------------- the "velorine" search
// The layout of reference/ref2 (164 results: groups of duplicates, single hits, partial files, an
// unrelated artist whose title has the word in it, long names that get elided), with an invented
// artist ("Velorine") and invented titles.
struct GroupDef {
    const char *name;
    qint64 size;
    int kbps;
    int fast, slow;            // sources by speed class
    int availFast, availSlow;  // sources with free slots
};
const GroupDef kArtistGroups[] = {
    {"Velorine - Beside The Mill Pond.mp3", 4388571, 128, 2, 0, 1, 0},
    {"Velorine - Velvet Engine.mp3", 4124549, 128, 2, 1, 1, 0},
    {"Velorine - Marigold.mp3", 5195650, 128, 2, 0, 1, 0},
    {"Velorine remix - Velvet Engine (Extended Smooth Jazz Version).mp3", 8495621, 128, 3, 2, 0, 2},
    {"Club_-_Velorine - Haunt The Hallway (Rare Jungle Mix).mp3", 6877936, 128, 0, 2, 0, 1},
    {"Velorine - No Ordinary Tuesday.mp3", 7033835, 128, 3, 2, 0, 1},
    {"velorine - Beside The Mill Pond (reggae remix).mp3", 5709476, 192, 0, 2, 0, 1},
    {"velorine - the quiet orchard.mp3", 4435310, 128, 3, 1, 0, 0},
    {"Velorine -  King Of Small Hours.mp3", 11718658, 320, 3, 1, 0, 0},
    {"Velorine - The Best Of Velorine - Entire Album(1)(1)(1).mp3", 106764288, 192, 1, 1, 1, 1},
    {"velorine - beside the mill pond.mp3", 6542733, 192, 2, 1, 0, 0},
};

struct SingleDef {
    const char *name;
    qint64 size;
    int kbps;
    const char *user;
    const char *conn;
    int freeSlots, total, partial;
};
const SingleDef kArtistSingles[] = {
    {"Velorine - King Small Hours.mp3", 5861376, 160, "Quill233_21212", "FTTB", 9, 12, 0},
    {"Velorine - King Of Small Hours .mp3", 5942533, 160, "Quill233_21212", "FTTB", 9, 12, 0},
    {"Velorine - Beside The Mill Pond(2).mp3", 11042816, 320, "t0astcrumb_48291", "VDSL", 8, 8, 0},
    {"kvt - Song For Velorine.mp3", 4302889, 128, "tuikku788_10293", "DSL", 8, 8, 0},
    {"Velorine - Beside The Mill Pond.MP3", 6543360, 192, "QWmarbleX1_55210", "Cable", 8, 8, 0},
    {"R&B Velorine - Paper Lighthouse - 03 - King Of Small Hours.mp3", 7021970, 192, "brimo65490_11234", "Cable", 7, 8, 0},
    {"R&B velorine - sing to the harbour.mp3", 4466021, 128, "brimo65490_11234", "Cable", 7, 8, 0},
    {"DJ Lanternfish - The Boathouse Sessions (Short) - Velorine, Ottilie Marsh, Benno Quist.mp3", 20363807, 128, "pastinak190_77321", "Cable", 7, 8, 0},
    {"Velorine - Nothing But The Wind - I Miss The Tide (Remix).mp3", 3481600, 112, "mintymoth22_90122", "Cable", 5, 8, 0},
    {"Velorine - Paper Lighthouse - Entire Album.mp3", 109878998, 320, "zqvtra662_44120", "Cable", 3, 8, 25},
    {"Velorine - Ember Road (Entire Album).mp3", 65255573, 160, "zqvtra662_44120", "Cable", 3, 8, 0},
    {"Club_-_Velorine - Velvet Engine (Extended).mp3", 8497152, 128, "MangoLassi6_33012", "Cable", 3, 3, 0},
    {"Velorine-Beside The Mill Pond.mp3", 4390243, 128, "fuzzyfelts_b_10023", "4G", 3, 3, 0},
    {"Velorine - Beside The Mill Pond.mp3", 11041564, 320, "PLONKS791_56002", "Unknown", 2, 3, 82},
    {"Velorine - Mind The Morning Tide.mp3", 6065969, 128, "PLONKS791_56002", "Unknown", 2, 3, 0},
    {"velorine - Glass Orchard Entire Album.mp3", 64738325, 192, "Bobwib234_40021", "VDSL", 2, 6, 0},
    {"02 - Velorine - Thimble Rain.mp3", 4438144, 128, "dorimunt891_10992", "VDSL", 1, 4, 0},
    {"DJ de Velorine - Lantern Radio 3-14-02 - 07 - Hollow Pines - Stillness.mp3", 4655227, 128, "biscotta407_20391", "VDSL", 1, 10, 0},
};

const char *kArtistTitles[] = {"Velvet Engine", "Your Kite Is Blue", "The Quiet Orchard", "Hold On To The Lantern",
                               "Tin Lighthouse", "No Ordinary Tuesday", "Letter From The Ferry", "Mind The Morning Tide",
                               "Beside The Mill Pond", "King Of Small Hours", "Sailor Of Dust", "Is It A Rumour",
                               "Never As Bright As The First Snow", "Rain Is Softer Than Pride", "Feel No Hurry",
                               "Marbles", "Like A Postcard", "Somebody Already Took The Last Train", "Marigold",
                               "Frankie's First Bicycle", "Hattie", "Paper Moth", "Why Can't We Walk Together"};
constexpr int kArtistTitleCount = int(sizeof(kArtistTitles) / sizeof(kArtistTitles[0]));

QList<SearchHit> artistHits(quint32 qid, QList<int> *batch)
{
    QList<SearchHit> out;
    QRandomGenerator rng(0x5ADE);
    int gi = 0;
    for (const GroupDef &g : kArtistGroups) {
        SearchHit h;
        h.queryId = qid;
        h.fileName = QString::fromUtf8(g.name);
        h.path = QStringLiteral("C:\\My Music\\Velorine\\") + h.fileName;
        h.size = g.size;
        h.type = MediaType::Audio;
        h.bitrateKbps = g.kbps;
        h.sampleRateHz = 44100;
        h.durationSec = durationFor(g.size, g.kbps);
        if (gi == 3) h.durationSec = 530;
        if (gi == 9) h.durationSec = 4448;
        h.hashes.ed2k = fakeHash(h.fileName + QString::number(g.size), 16);
        h.key = QStringLiteral("ed2k:") + QString::fromLatin1(h.hashes.ed2k.toHex());
        int af = g.availFast, as = g.availSlow;
        for (int i = 0; i < g.fast; ++i) {
            const bool avail = af-- > 0;
            h.sources << src(i % 3 == 2 ? Net::ED2K : Net::Overlay, userName(rng, kStems, kStemCount),
                             QString::fromLatin1(kConnFast[rng.bounded(4)]), avail ? rng.bounded(1, 6) : 0,
                             rng.bounded(3, 13), avail ? -1 : rng.bounded(3, 40), -1);
        }
        for (int i = 0; i < g.slow; ++i) {
            const bool avail = as-- > 0;
            h.sources << src(Net::Overlay, userName(rng, kStems, kStemCount),
                             QString::fromLatin1(kConnSlow[rng.bounded(5)]), avail ? 1 : 0, rng.bounded(1, 5),
                             avail ? -1 : rng.bounded(3, 40), -1);
        }
        out << h;
        if (batch)
            *batch << (gi < 4 ? 0 : 1);
        ++gi;
    }
    int si = 0;
    for (const SingleDef &d : kArtistSingles) {
        SearchHit h;
        h.queryId = qid;
        h.fileName = QString::fromUtf8(d.name);
        h.path = QStringLiteral("C:\\Documents and Settings\\Shared\\") + h.fileName;
        h.size = d.size;
        h.type = MediaType::Audio;
        h.bitrateKbps = d.kbps;
        h.sampleRateHz = 44100;
        h.durationSec = durationFor(d.size, d.kbps);
        h.hashes.ed2k = fakeHash(h.fileName + QString::number(d.size) + QString::fromLatin1(d.user), 16);
        h.key = QStringLiteral("ed2k:") + QString::fromLatin1(h.hashes.ed2k.toHex());
        SearchSource s = src(Net::Overlay, QString::fromLatin1(d.user), QString::fromLatin1(d.conn),
                             d.freeSlots, d.total, -1, -1);
        if (d.partial)
            s.extra.insert(QStringLiteral("partialPercent"), d.partial);
        h.sources << s;
        if (si == 12)
            h.canJoinRunningDownload = true;
        out << h;
        if (batch)
            *batch << (si < 6 ? 0 : 1);
        ++si;
    }
    // filler results up to 164, all single-source, mixed networks
    const char *prefixes[] = {"Velorine - ", "velorine - ", "Velorine-", "VELORINE - ", "Velorine - Best Of - ", "Velorine - Live - "};
    const char *suffixes[] = {".mp3", ".mp3", ".mp3", " (live).mp3", " (remix).mp3", ".ogg", " [192].mp3"};
    int fi = 0;
    while (out.size() < 164) {
        SearchHit h;
        h.queryId = qid;
        const QString title = QString::fromLatin1(kArtistTitles[rng.bounded(kArtistTitleCount)]);
        h.fileName = QString::fromLatin1(prefixes[rng.bounded(6)]) + title + QString::fromLatin1(suffixes[rng.bounded(7)]);
        h.type = MediaType::Audio;
        const int kbpsChoices[] = {64, 96, 112, 128, 128, 128, 160, 192, 192, 256, 320};
        h.bitrateKbps = kbpsChoices[rng.bounded(11)];
        h.sampleRateHz = rng.bounded(10) == 0 ? 22050 : 44100;
        h.durationSec = rng.bounded(180, 420);
        h.size = qint64(h.durationSec) * h.bitrateKbps * 125 + rng.bounded(4000);
        h.path = QStringLiteral("C:\\MP3\\") + h.fileName;
        h.hashes.ed2k = fakeHash(h.fileName + QString::number(fi), 16);
        h.key = QStringLiteral("ed2k:") + QString::fromLatin1(h.hashes.ed2k.toHex());
        const bool fast = rng.bounded(3) != 0;
        const int total = rng.bounded(1, 12);
        const int free = rng.bounded(4) == 0 ? 0 : rng.bounded(0, total + 1);
        const Net n = (fi % 5 == 3) ? Net::ED2K : (fi % 7 == 4 ? Net::Gnutella : Net::Overlay);
        SearchSource s = src(n, userName(rng, kStems, kStemCount),
                             QString::fromLatin1(fast ? kConnFast[rng.bounded(4)] : kConnSlow[rng.bounded(5)]),
                             free, total, free == 0 ? rng.bounded(1, 60) : -1, -1);
        if (n == Net::ED2K)
            s.extra.insert(QStringLiteral("server"), QStringLiteral("[eD2k] DonkeyServer No1"));
        h.sources << s;
        if (fi == 7)
            h.matchesIncompleteLocal = true;
        out << h;
        if (batch)
            *batch << (2 + (fi % 2));
        ++fi;
    }
    return out;
}

// --------------------------------------------------------------------------- the "linux" search
QList<SearchHit> linuxHits(quint32 qid, QList<int> *batch)
{
    struct D { const char *name; qint64 size; int seeds, peers; bool bundle; };
    const D defs[] = {
        {"debian-13.2.0-amd64-netinst.iso", 790626304, 1412, 88, false},
        {"ubuntu-24.04.3-desktop-amd64.iso", 6203355136, 2310, 341, false},
        {"Fedora-Workstation-Live-x86_64-43.iso", 2408448000, 812, 64, false},
        {"archlinux-2026.11.01-x86_64.iso", 1289748480, 950, 41, false},
        {"linuxmint-22.2-cinnamon-64bit.iso", 2995683328, 640, 77, false},
        {"Slackware 15.0 DVD + source", 9543200768, 41, 12, true},
        {"openSUSE-Leap-16.0-DVD-x86_64.iso", 4613734400, 233, 19, false},
        {"Linux kernel 6.17 source tarballs", 1546190848, 18, 5, true},
        {"knoppix_v9.1DVD-2021-01-25-EN.iso", 4653219840, 57, 3, false},
        {"linux-firmware-20261015.tar.xz", 412109824, 9, 1, false},
    };
    QList<SearchHit> out;
    int i = 0;
    for (const D &d : defs) {
        SearchHit h;
        h.queryId = qid;
        h.fileName = QString::fromLatin1(d.name);
        h.size = d.size;
        h.type = d.bundle ? MediaType::Any : MediaType::Software;   // [ui-f] bundles: by their files (.iso)
        h.isBundle = d.bundle;
        h.hashes.btih = fakeHash(h.fileName, 20);
        h.key = QStringLiteral("btih:") + QString::fromLatin1(h.hashes.btih.toHex());
        if (d.bundle) {
            for (int k = 0; k < 6; ++k) {
                BundleFile f;
                f.index = k;
                f.path = h.fileName + QStringLiteral("/part%1.iso").arg(k + 1);
                f.size = d.size / 6;
                h.files << f;
            }
        }
        SearchSource dht;
        dht.net = Net::BitTorrent;
        dht.connection = QStringLiteral("DHT");
        dht.seeds = d.seeds;
        dht.peers = d.peers;
        dht.uri = h.hashes.toMagnet(h.fileName, h.size);
        h.sources << dht;
        if (i % 3 == 0) {
            SearchSource tr = dht;
            tr.connection = QStringLiteral("tracker");
            tr.user = user(Net::BitTorrent, QStringLiteral("tracker.example.org:6969"));
            tr.seeds = d.seeds / 2;
            tr.peers = d.peers / 2;
            h.sources << tr;
        }
        if (i % 2 == 0) {
            SearchSource e = src(Net::ED2K, QStringLiteral("mule_%1").arg(1000 + i * 37), QStringLiteral("VDSL"), 2, 4);
            e.extra.insert(QStringLiteral("server"), QStringLiteral("[eD2k] eMule Security No1"));
            h.sources << e;
            SearchSource web;
            web.net = Net::Http;
            web.connection = QStringLiteral("FTTH");
            web.user = user(Net::Http, QStringLiteral("mirror.example.net"));
            web.freeSlots = 10;
            web.totalSlots = 10;
            h.sources << web;
        }
        out << h;
        if (batch)
            *batch << (i % 3);
        ++i;
    }
    return out;
}

// Three typical fakes (what the core's search hygiene flags, SearchHit::suspect), so --demo shows the
// suspect colour / tooltip / warning: an executable named like a video, an archive wrapped around a
// media file and a "1080p" video far too small for that.
QList<SearchHit> suspectHits(quint32 qid, const QString &base, QList<int> *batch)
{
    const struct { const char *fmt; qint64 size; MediaType type; const char *why; } fakes[] = {
        {"%1 1080p BluRay x264.avi.exe", 1843200, MediaType::Software, "executable named like a media file (.avi.exe)"},
        {"%1 (2008) DVDRip.avi.rar", 7340032, MediaType::Archive, "archive wrapped around a media file (.avi.rar)"},
        {"%1.2008.1080p.BluRay.mkv", 1341, MediaType::Video, "1080p video of 1 341 bytes (too small for a video)"},
    };
    QList<SearchHit> out;
    int i = 0;
    for (const auto &f : fakes) {
        SearchHit h;
        h.queryId = qid;
        h.fileName = QString::fromLatin1(f.fmt).arg(base);
        h.size = f.size;
        h.type = f.type;
        h.hashes.ed2k = fakeHash(h.fileName, 16);
        h.key = QStringLiteral("ed2k:") + QString::fromLatin1(h.hashes.ed2k.toHex());
        SearchSource s = src(Net::ED2K, QStringLiteral("fakefarm_%1").arg(4400 + i), QStringLiteral("Unknown"), 1, 1, -1, -1);
        s.seeds = 37;   // the same complete-source count on several files: a server-side fake farm
        s.extra.insert(QStringLiteral("server"), QStringLiteral("[eD2k] DonkeyServer No2"));
        h.sources << s;
        h.suspect = QString::fromLatin1(f.why);
        h.path = QStringLiteral("C:\\Shared\\") + h.fileName;
        out << h;
        if (batch)
            *batch << 1;
        ++i;
    }
    return out;
}

// --------------------------------------------------------------------------- [ui-f] open movies
// Blender open movies (CC BY): every file type, torrents named without an extension, bundles with and
// without a known file list, so each entry of the file-type menu has something to show (and hide).
QList<SearchHit> openMovieHits(quint32 qid, QList<int> *batch)
{
    struct D {
        const char *name;
        qint64 size;
        MediaType type;   // what the network / index reported (Any = nothing)
        Net net;
        int kbps;
        int kind;         // 0 file, 1 torrent (one file), 2 bundle with files, 3 bundle, files unknown
    };
    const D defs[] = {
        {"Sintel 2010 1080p", 1257283584, MediaType::Video, Net::BitTorrent, -1, 1},
        {"Big Buck Bunny (2008)", 928672000, MediaType::Any, Net::BitTorrent, -1, 2},
        {"Sintel.2010.720p.mkv", 652087296, MediaType::Video, Net::ED2K, -1, 0},
        {"sintel-trailer-1080p.webm", 31457280, MediaType::Any, Net::Gnutella, -1, 0},
        {"Sintel Fan Remix - Captain Lentil - Sintel Theme.flac", 43253760, MediaType::Audio, Net::ED2K, -1, 0},
        {"Sintel Fan Remix - Captain Lentil - Sintel Theme.mp3", 9175040, MediaType::Audio, Net::Overlay, 320, 0},
        {"Sintel Fan Remix - Captain Lentil - Snow Walk.mp3", 4587520, MediaType::Audio, Net::Overlay, 128, 0},
        {"Sintel Fan Remix (2010) [FLAC]", 312475648, MediaType::Audio, Net::BitTorrent, -1, 3},
        {"sintel_poster_2048.png", 6291456, MediaType::Any, Net::Gnutella, -1, 0},
        {"Sintel concept art.webp", 2097152, MediaType::Any, Net::Overlay, -1, 0},
        {"Sintel production script.pdf", 1048576, MediaType::Document, Net::ED2K, -1, 0},
        {"Sintel.2010.English.srt", 65536, MediaType::Any, Net::Gnutella, -1, 0},
        {"Sintel Blender production files", 7516192768, MediaType::Any, Net::BitTorrent, -1, 3},
        {"sintel-lite-2010.zip", 157286400, MediaType::Any, Net::ED2K, -1, 0},
        {"Sintel 4K", 6442450944, MediaType::Any, Net::BitTorrent, -1, 1},
        {"Sintel (2010) DVD.iso", 4700000000, MediaType::Any, Net::ED2K, -1, 0},
        {"sintel-blender-addon.exe", 3145728, MediaType::Any, Net::Gnutella, -1, 0},
    };
    QList<SearchHit> out;
    int i = 0;
    for (const D &d : defs) {
        SearchHit h;
        h.queryId = qid;
        h.fileName = QString::fromUtf8(d.name);
        h.size = d.size;
        h.type = d.type;
        h.bitrateKbps = d.kbps;
        if (d.kbps > 0) {
            h.sampleRateHz = 44100;
            h.durationSec = int(d.size * 8 / (qint64(d.kbps) * 1000));
        }
        if (d.kind == 0) {
            h.hashes.ed2k = fakeHash(h.fileName, 16);
            h.key = QStringLiteral("ed2k:") + QString::fromLatin1(h.hashes.ed2k.toHex());
            SearchSource s = src(d.net, QStringLiteral("openmovie_%1").arg(10200 + i * 13), QStringLiteral("FTTH"), 2, 4, -1, 40 + i);
            if (d.net == Net::ED2K)
                s.extra.insert(QStringLiteral("server"), QStringLiteral("[eD2k] DonkeyServer No2"));
            h.sources << s;
            h.path = QStringLiteral("C:\\Shared\\Open Movies\\") + h.fileName;
        } else {
            h.isBundle = d.kind >= 2;
            h.hashes.btih = fakeHash(h.fileName, 20);
            h.key = QStringLiteral("btih:") + QString::fromLatin1(h.hashes.btih.toHex());
            if (d.kind == 2) {
                h.files << BundleFile{QStringLiteral("Big Buck Bunny/Big Buck Bunny 1080p.mkv"), d.size - 4096, 0};
                h.files << BundleFile{QStringLiteral("Big Buck Bunny/Big Buck Bunny.nfo"), 4096, 1};
            }
            SearchSource dht;
            dht.net = Net::BitTorrent;
            dht.connection = QStringLiteral("DHT");
            dht.seeds = 120 - i * 5;
            dht.peers = 30 + i;
            dht.uri = h.hashes.toMagnet(h.fileName, h.size);
            h.sources << dht;
        }
        out << h;
        if (batch)
            *batch << (i % 3);
        ++i;
    }
    return out;
}

// --------------------------------------------------------------------------- anything else
QList<SearchHit> genericHits(const SearchQuery &q, QList<int> *batch)
{
    QList<SearchHit> out;
    const QString words = (q.text + QLatin1Char(' ') + q.text2).simplified();
    quint32 seed = 0;
    for (QChar c : words)
        seed = seed * 31 + c.unicode();
    QRandomGenerator rng(seed ^ 0xC0FFEE);
    QString base = words;
    if (base.isEmpty())
        base = QStringLiteral("untitled");
    QStringList caps;
    for (const QString &w : base.split(QLatin1Char(' '), Qt::SkipEmptyParts))
        caps << w.left(1).toUpper() + w.mid(1);
    const QString Base = caps.join(QLatin1Char(' '));
    struct T { const char *fmt; MediaType type; int kbps; };
    const T templates[] = {
        {"%1 - Track %2.mp3", MediaType::Audio, 192},
        {"%1 - Live At The Old Granary.mp3", MediaType::Audio, 128},
        {"%1 (Radio Edit).mp3", MediaType::Audio, 160},
        {"%1 - Greatest Hits.zip", MediaType::Archive, -1},
        {"%1 documentary.avi", MediaType::Video, -1},
        {"%1.pdf", MediaType::Document, -1},
        {"%1 - Remix %2.ogg", MediaType::Audio, 256},
        {"%1 wallpaper %2.jpg", MediaType::Image, -1},
        {"%1 - Demo %2.mp3", MediaType::Audio, 96},
        {"%1 complete collection", MediaType::Archive, -1},
    };
    const int n = 40 + rng.bounded(80);
    for (int i = 0; i < n; ++i) {
        const T &t = templates[rng.bounded(10)];
        SearchHit h;
        h.queryId = q.id;
        h.fileName = QString::fromLatin1(t.fmt).arg(i % 2 ? Base : base).arg(rng.bounded(1, 20), 2, 10, QLatin1Char('0'));
        h.type = t.type;
        h.bitrateKbps = t.kbps;
        if (t.kbps > 0) {
            h.sampleRateHz = 44100;
            h.durationSec = rng.bounded(150, 480);
            h.size = qint64(h.durationSec) * t.kbps * 125 + rng.bounded(5000);
        } else {
            h.size = qint64(rng.bounded(200, 900000)) * 1024;
        }
        const bool torrent = t.type == MediaType::Archive && rng.bounded(2);
        if (torrent) {
            h.isBundle = h.fileName.endsWith(QLatin1String("collection"));
            h.hashes.btih = fakeHash(h.fileName + QString::number(i), 20);
            h.key = QStringLiteral("btih:") + QString::fromLatin1(h.hashes.btih.toHex());
            SearchSource s;
            s.net = Net::BitTorrent;
            s.connection = QStringLiteral("DHT");
            s.seeds = rng.bounded(0, 300);
            s.peers = rng.bounded(0, 80);
            h.sources << s;
            if (h.isBundle)
                for (int k = 0; k < 4; ++k)
                    h.files << BundleFile{QStringLiteral("CD%1.zip").arg(k + 1), h.size / 4, k};
        } else {
            h.hashes.ed2k = fakeHash(h.fileName + QString::number(i), 16);
            h.key = QStringLiteral("ed2k:") + QString::fromLatin1(h.hashes.ed2k.toHex());
            const int nsrc = rng.bounded(6) == 0 ? rng.bounded(2, 7) : 1;
            for (int k = 0; k < nsrc; ++k) {
                const Net nets[] = {Net::Overlay, Net::ED2K, Net::Gnutella, Net::Kad};
                const Net net = nets[rng.bounded(4)];
                const bool fast = rng.bounded(3) != 0;
                const int total = rng.bounded(1, 10);
                const int free = rng.bounded(3) == 0 ? 0 : rng.bounded(0, total + 1);
                SearchSource s = src(net, userName(rng, kStems, kStemCount),
                                     QString::fromLatin1(fast ? kConnFast[rng.bounded(4)] : kConnSlow[rng.bounded(5)]),
                                     free, total, free == 0 ? rng.bounded(1, 50) : -1,
                                     rng.bounded(4) ? rng.bounded(20, 600) : -1);
                if (net == Net::ED2K)
                    s.extra.insert(QStringLiteral("server"), QStringLiteral("[eD2k] DonkeyServer No2"));
                h.sources << s;
            }
        }
        h.path = QStringLiteral("C:\\Shared\\") + h.fileName;
        out << h;
        if (batch)
            *batch << rng.bounded(4);
    }
    out << suspectHits(q.id, Base, batch);
    return out;
}

} // namespace

UserRef user(Net n, const QString &name)
{
    UserRef u;
    u.net = n;
    u.name = name;
    u.id = name;
    return u;
}

QList<SearchHit> searchHits(const SearchQuery &q, QList<int> *batch)
{
    const QString t = (q.text + QLatin1Char(' ') + q.text2).toLower();
    if (t.contains(QLatin1String("velorine")))
        return artistHits(q.id, batch);
    if (t.contains(QLatin1String("sintel")) || t.contains(QLatin1String("open movie")))   // [ui-f]
        return openMovieHits(q.id, batch);
    if (t.contains(QLatin1String("linux")) || t.contains(QLatin1String("debian")) || t.contains(QLatin1String("ubuntu")))
        return linuxHits(q.id, batch);
    return genericHits(q, batch);
}

// --------------------------------------------------------------------------- [ui-i] piece maps
PieceSim::PieceSim(qint64 size, qint64 done, Order order, int active, quint32 seed, qint64 piece)
    : m_size(size), m_order(order), m_wantActive(active), m_seed(seed ? seed : 1)
{
    if (size <= 0)
        return;
    // about a thousand pieces, a power of two of at least 16 KiB (as a torrent would choose)
    m_piece = piece > 0 ? piece : 16 * 1024;
    while (piece <= 0 && size / m_piece > 1500)
        m_piece *= 2;
    const int n = int((size + m_piece - 1) / m_piece);
    m_got = QVector<qint64>(n, 0);
    // the pieces that are already there, in the order they would have arrived
    qint64 have = 0;
    while (have + pieceLen(0) <= done) {
        const int p = pickNext();
        if (p < 0)
            break;
        m_got[p] = pieceLen(p);
        have += m_got[p];
    }
    setRunning(true);
    // the arriving pieces are partly there already (different amounts)
    for (int k = 0; k < m_active.size(); ++k) {
        const int p = m_active.at(k);
        m_got[p] = pieceLen(p) * ((k * 37 + 11) % 90) / 100;
    }
}

qint64 PieceSim::pieceLen(int i) const
{
    return qMin(m_piece, m_size - qint64(i) * m_piece);
}

int PieceSim::pickNext()
{
    const int n = int(m_got.size());
    auto free = [&](int p) { return p >= 0 && p < n && m_got.at(p) == 0 && !m_active.contains(p); };
    if (m_order == Sequential || m_order == FirstLast) {
        // first and last: the head and tail bands (1 % each, at least 2 pieces) before the rest
        const int band = qMax(2, n / 100);
        if (m_order == FirstLast) {
            for (int i = 0; i < band; ++i) {
                if (free(i))
                    return i;
                if (free(n - 1 - i))
                    return n - 1 - i;
            }
        } else {
            for (int p = 0; p < n; ++p)
                if (free(p))
                    return p;
            return -1;
        }
    }
    // rarest first looks random: a few tries at random, then the first free one after it
    for (int tries = 0; tries < 8; ++tries) {
        m_seed = m_seed * 1103515245u + 12345u;
        const int p = int((m_seed >> 8) % quint32(n));
        if (free(p))
            return p;
        for (int q = p + 1; q < qMin(n, p + 40); ++q)
            if (free(q))
                return q;
    }
    for (int p = 0; p < n; ++p)
        if (free(p))
            return p;
    return -1;
}

void PieceSim::setRunning(bool running)
{
    if (!running) {
        m_active.clear();
        return;
    }
    while (m_active.size() < m_wantActive) {
        const int p = pickNext();
        if (p < 0)
            break;
        m_active << p;
    }
}

void PieceSim::advance(qint64 bytes)
{
    setRunning(true);
    while (bytes > 0 && !m_active.isEmpty()) {
        const qint64 share = qMax<qint64>(1, bytes / m_active.size());
        for (int k = 0; k < m_active.size() && bytes > 0; ++k) {
            const int p = m_active.at(k);
            const qint64 take = qMin(qMin(share, bytes), pieceLen(p) - m_got.at(p));
            m_got[p] += take;
            bytes -= take;
        }
        for (int k = int(m_active.size()) - 1; k >= 0; --k)
            if (m_got.at(m_active.at(k)) >= pieceLen(m_active.at(k)))
                m_active.removeAt(k);
        setRunning(true);
    }
}

qint64 PieceSim::done() const
{
    qint64 d = 0;
    for (int i = 0; i < m_got.size(); ++i)
        if (m_got.at(i) >= pieceLen(i))
            d += m_got.at(i);
    return d;
}

void PieceSim::apply(TransferInfo &t) const
{
    if (!isValid())
        return;
    t.done = done();
    // the core's cells (TransferInfo::pieceMap): at most 512, verified share per cell
    const int pieces = int(m_got.size());
    const int n = int(qMin<qint64>(512, m_size));
    t.pieceMap = QByteArray(n, char(0));
    t.pieceFill = QByteArray(n, char(0));
    for (int i = 0; i < n; ++i) {
        const qint64 s = m_size * i / n, e = m_size * (i + 1) / n;
        qint64 ok = 0;
        bool moving = false;
        for (int p = int(s / m_piece); p < pieces && qint64(p) * m_piece < e; ++p) {
            const qint64 ps = qint64(p) * m_piece, pe = ps + pieceLen(p);
            const qint64 ov = qMin(e, pe) - qMax(s, ps);
            if (ov <= 0)
                continue;
            if (m_got.at(p) >= pieceLen(p))
                ok += ov;
            else if (m_active.contains(p)) {
                // what the engine reports as arriving: a torrent piece in the download queue as a
                // whole, of a large part (eD2k) the bytes received so far and the request ahead
                qint64 ae = pe;
                if (m_piece >= 2 * 1024 * 1024)
                    ae = qMin(pe, ps + m_got.at(p) + 512 * 1024);
                if (qMin(e, ae) > qMax(s, ps))
                    moving = true;
            }
        }
        const qint64 len = e - s;
        int f = int((ok * 255 + len / 2) / len);
        if (ok > 0 && f < 1)
            f = 1;
        if (ok < len && f > 254)
            f = 254;
        t.pieceFill[i] = char(f);
        t.pieceMap[i] = char(ok >= len ? 2 : moving ? 1 : 0);
    }
}

PieceSim demoPieces(const TransferInfo &t)
{
    const quint32 seed = quint32(qHash(t.id)) | 1u;
    PieceSim sim;
    if (t.id == QLatin1String("d2"))          // several networks: eD2k parts of 9.28 MB, two arriving
        sim = PieceSim(t.size, t.done, PieceSim::Scattered, 2, seed, 9728000);
    else if (t.id == QLatin1String("d3"))     // a torrent with "first and last pieces first"
        sim = PieceSim(t.size, t.done, PieceSim::FirstLast, 7, seed);
    else if (t.id == QLatin1String("d8"))     // paused: what it has, nothing arriving
        sim = PieceSim(t.size, t.done, PieceSim::Scattered, 0, seed);
    else if (t.id == QLatin1String("d10"))    // sequential order
        sim = PieceSim(t.size, t.done, PieceSim::Sequential, 2, seed);
    else if (t.id == QLatin1String("d11"))    // nearly complete
        sim = PieceSim(t.size, t.done, PieceSim::Scattered, 4, seed);
    if (sim.isValid() && t.state != TransferState::Transferring)
        sim.setRunning(false);
    return sim;
}

// --------------------------------------------------------------------------- transfers
QList<TransferInfo> downloads()
{
    QList<TransferInfo> l;
    const QDateTime base = QDateTime(QDate(2026, 11, 20), QTime(18, 0), QTimeZone::UTC);
    auto mk = [&](const QString &id, const QString &name, qint64 size, qint64 done, TransferState st,
                  const QString &status) {
        TransferInfo t;
        t.id = id;
        t.name = name;
        t.size = size;
        t.done = done;
        t.state = st;
        t.statusText = status;
        t.added = base;
        t.savePath = QStringLiteral("C:\\Downloads\\OctoMX\\") + name;
        t.hashes.ed2k = fakeHash(name, 16);
        return t;
    };
    {
        TransferInfo t = mk(QStringLiteral("d1"), QStringLiteral("The Gooseberry Big Band - Moonlit Ferry 192.mp3"), 5792882, 0,
                            TransferState::FindingSources, QStringLiteral("Finding sources..."));
        TransferSource s;
        s.net = Net::Overlay;
        s.user = user(Net::Overlay, QStringLiteral("Quill233_21212"));
        s.connection = QStringLiteral("FTTB");
        s.statusText = QStringLiteral("Waiting for Network Reply...");
        t.sources << s;
        t.sourcesTotal = 1;
        l << t;
    }
    {
        TransferInfo t = mk(QStringLiteral("d2"), QStringLiteral("Velorine - The Best Of Velorine - Entire Album.mp3"), 106764288,
                            51246858, TransferState::Transferring, QString());
        t.rateDownBps = 48210;
        t.etaSec = (t.size - t.done) / t.rateDownBps;
        t.networks = {Net::Overlay, Net::ED2K, Net::BitTorrent};
        const char *users[] = {"zqvtra662_44120", "Bobwib234_40021", "PLONKS791_56002", "brimo65490_11234"};
        const Net nets[] = {Net::Overlay, Net::ED2K, Net::Overlay, Net::Kad};
        const char *conns[] = {"Cable", "VDSL", "Unknown", "Cable"};
        const char *stats[] = {"", "", "Remotely Queued 7", "Busy (4 Users in Queue)"};
        const int rates[] = {30120, 18090, 0, 0};
        for (int i = 0; i < 4; ++i) {
            TransferSource s;
            s.net = nets[i];
            s.user = user(nets[i], QString::fromLatin1(users[i]));
            s.connection = QString::fromLatin1(conns[i]);
            s.statusText = QString::fromLatin1(stats[i]);
            s.rateBps = rates[i];
            s.bytes = i == 0 ? 30000000 : i == 1 ? 21246858 : 0;
            t.sources << s;
        }
        t.sourcesTotal = 4;
        t.sourcesActive = 2;
        l << t;
    }
    {
        TransferInfo t = mk(QStringLiteral("d3"), QStringLiteral("debian-13.2.0-amd64-DVD-1.iso"), 3994091520, 1157287936,
                            TransferState::Transferring, QString());
        t.rateDownBps = 412330;
        t.etaSec = int((t.size - t.done) / t.rateDownBps);
        t.hashes.btih = fakeHash(t.name, 20);
        t.networks = {Net::BitTorrent, Net::Http};
        TransferSource dht;
        dht.net = Net::BitTorrent;
        dht.user = user(Net::BitTorrent, QStringLiteral("DHT"));
        dht.connection = QStringLiteral("FTTH");
        dht.statusText = QString();
        dht.rateBps = 351000;
        dht.bytes = 1000000000;
        TransferSource web;
        web.net = Net::Http;
        web.user = user(Net::Http, QStringLiteral("cdimage.debian.org"));
        web.connection = QStringLiteral("FTTH");
        web.rateBps = 61330;
        web.bytes = 157287936;
        TransferSource more;
        more.net = Net::BitTorrent;
        more.user = user(Net::BitTorrent, QStringLiteral("+ 37 more peers"));
        more.connection = QString();
        more.statusText = QStringLiteral("Connected, Negotiating...");
        t.sources << dht << web << more;
        t.sourcesTotal = 40;
        t.sourcesActive = 2;
        l << t;
    }
    {
        TransferInfo t = mk(QStringLiteral("d4"), QStringLiteral("Velorine - Mind The Morning Tide.mp3"), 6065969, 0,
                            TransferState::RemotelyQueued, QStringLiteral("Remotely Queued 12"));
        TransferSource s;
        s.net = Net::Overlay;
        s.user = user(Net::Overlay, QStringLiteral("PLONKS791_56002"));
        s.connection = QStringLiteral("Unknown");
        s.statusText = QStringLiteral("Remotely Queued 12");
        t.sources << s;
        t.sourcesTotal = 1;
        l << t;
    }
    {
        TransferInfo t = mk(QStringLiteral("d5"), QStringLiteral("Big Buck Bunny 1080p.mkv"), 928670754, 928670754,
                            TransferState::Completed, QStringLiteral("Complete"));
        t.completed = base.addSecs(1800);
        l << t;
    }
    {
        TransferInfo t = mk(QStringLiteral("d6"), QStringLiteral("Sintel.2010.720p.mkv"), 652087296, 0, TransferState::Queued,
                            QStringLiteral("Locally Queued"));
        l << t;
    }
    {
        TransferInfo t = mk(QStringLiteral("d7"), QStringLiteral("Velorine - Marigold.mp3"), 5195650, 1234567, TransferState::Error,
                            QStringLiteral("File/User Unavailable"));
        TransferSource s;
        s.net = Net::Overlay;
        s.user = user(Net::Overlay, QStringLiteral("fuzzyfelts_b_10023"));
        s.connection = QStringLiteral("4G");
        s.statusText = QStringLiteral("Timed Out (Waiting for Network Reply)");
        t.sources << s;
        l << t;
    }
    {
        TransferInfo t = mk(QStringLiteral("d8"), QStringLiteral("Elephants Dream (all formats)"), 1610612736, 402653184,
                            TransferState::Paused, QStringLiteral("Paused"));
        t.isBundle = true;
        t.hashes.btih = fakeHash(t.name, 20);
        const char *files[] = {"ED_1024.avi", "ED_HD.avi", "ED_1280.mov", "elephantsdream_soundtrack.flac", "README.txt"};
        const qint64 sizes[] = {440401920, 853934080, 314572800, 1703936, 2000};
        for (int i = 0; i < 5; ++i)
            t.files << BundleFile{QString::fromLatin1(files[i]), sizes[i], i};
        l << t;
    }
    {
        TransferInfo t = mk(QStringLiteral("d9"), QStringLiteral("Alices Adventures In Wonderland - 01 - Down The Rabbit Hole (LibriVox).mp3"), 7340032, 0,
                            TransferState::Connecting, QStringLiteral("Connecting to Remote User"));
        TransferSource s;
        s.net = Net::Gnutella;
        s.user = user(Net::Gnutella, QStringLiteral("198.51.100.23:6346"));
        s.connection = QStringLiteral("Cable");
        s.statusText = QStringLiteral("Connecting to Remote User");
        t.sources << s;
        l << t;
    }
    // [ui-i] two more downloads for the map of the file: one in sequential order, one nearly complete
    {
        TransferInfo t = mk(QStringLiteral("d10"), QStringLiteral("Tears of Steel 1080p.mkv"), 584940544, 198000000,
                            TransferState::Transferring, QString());
        t.rateDownBps = 236500;
        t.etaSec = int((t.size - t.done) / t.rateDownBps);
        t.networks = {Net::Http};
        t.sequential = true;
        t.orderFollowedBy = {Net::Http};
        l << t;
    }
    {
        TransferInfo t = mk(QStringLiteral("d11"), QStringLiteral("Cosmos Laundromat 2160p.mkv"), 2345887744, 2270000000,
                            TransferState::Transferring, QString());
        t.rateDownBps = 1830000;
        t.etaSec = int((t.size - t.done) / t.rateDownBps);
        t.hashes.btih = fakeHash(t.name, 20);
        t.networks = {Net::BitTorrent};
        t.sourcesTotal = 112;
        t.sourcesActive = 9;
        l << t;
    }
    for (TransferInfo &t : l) {
        if (t.id == QLatin1String("d3")) {   // [dl-1] first and last pieces first, followed by BitTorrent
            t.firstLastFirst = true;
            t.orderFollowedBy = {Net::BitTorrent};
        }
        const PieceSim sim = demoPieces(t);
        sim.apply(t);
    }
    // stable source ids (TransferSource::id) for everything but BitTorrent peers / swarm summaries
    for (TransferInfo &t : l)
        for (int k = 0; k < t.sources.size(); ++k)
            if (t.sources[k].net != Net::BitTorrent)
                t.sources[k].id = QStringLiteral("%1.s%2").arg(t.id).arg(k + 1);
    // [diag-1] the "why is this slow" sentence of the Connection Test (status tooltip)
    for (TransferInfo &t : l) {
        if (t.id == QLatin1String("d3")) {
            t.diagnosis = QStringLiteral("Only 2 of 80 peers accept connections, both slow. Being reachable from the internet (port forwarding) helps.");
            t.diagnosisLevel = 2;   // DiagLevel::Limited
        } else if (t.id == QLatin1String("d6")) {
            t.diagnosis = QStringLiteral("Waiting for a free download slot: 5 downloads are active and the limit is 5 (Settings, File Transfers, Max total downloads).");
            t.diagnosisLevel = 2;
        } else if (t.id == QLatin1String("d8")) {
            t.diagnosis = QStringLiteral("The index (BitSearch) reports 192 seeds but neither the trackers nor the DHT know this torrent: the index numbers are probably out of date.");
            t.diagnosisLevel = 2;
        }
    }
    return l;
}

QList<TransferInfo> uploads()
{
    QList<TransferInfo> l;
    auto mk = [](const QString &id, const QString &name, const QString &userName, const QString &conn, qint64 size,
                 qint64 done, TransferState st, const QString &status, int rate) {
        TransferInfo t;
        t.id = id;
        t.isUpload = true;
        t.name = name;
        t.size = size;
        t.done = done;
        t.state = st;
        t.statusText = status;
        t.rateUpBps = rate;
        t.etaSec = rate > 0 ? int((size - done) / rate) : -1;
        t.peer = user(Net::Overlay, userName);
        TransferSource s;
        s.net = Net::Overlay;
        s.user = t.peer;
        s.connection = conn;
        t.sources << s;
        return t;
    };
    l << mk(QStringLiteral("u1"), QStringLiteral("Captain Lentil - Cardboard Rocket Dub.mp3"), QStringLiteral("dj_tomek_21212"),
            QStringLiteral("Unknown"), 5576633, 0, TransferState::TimedOut,
            QStringLiteral("Timed out (Waiting for incoming connection)"), 0);
    l << mk(QStringLiteral("u2"), QStringLiteral("The Gooseberry Big Band - Rooftop Pigeon Stomp.mp3"), QStringLiteral("mrgrom810003_61603"),
            QStringLiteral("Cable"), 6884352, 3416968, TransferState::Transferring, QString(), 6666);
    l << mk(QStringLiteral("u3"), QStringLiteral("Bach - Air On The G String.mp3"), QStringLiteral("Tweed317_48961"),
            QStringLiteral("VDSL"), 6110000, 583408, TransferState::Transferring, QString(), 5816);
    l << mk(QStringLiteral("u4"), QStringLiteral("Chopin - Nocturne Op 9 No 2.mp3"), QStringLiteral("robot000_61562"),
            QStringLiteral("4G"), 3654689, 0, TransferState::Connecting,
            QStringLiteral("Waiting for incoming connection..."), 0);
    l << mk(QStringLiteral("u5"), QStringLiteral("Big Buck Bunny 1080p.mkv"), QStringLiteral("203.0.113.77:51413"),
            QStringLiteral("FTTH"), 928670754, 0, TransferState::Queued, QStringLiteral("Queued"), 0);
    l.last().peer.net = Net::Gnutella;   // a Gnutella queue entry (PARQ): cannot be started early
    l.last().sources.first().net = Net::Gnutella;
    l << mk(QStringLiteral("u6"), QStringLiteral("Velorine - Velvet Engine.mp3"), QStringLiteral("eMule_Hans"),
            QStringLiteral("VDSL"), 4980736, 0, TransferState::Queued, QStringLiteral("Queued (3 in line)"), 0);
    l.last().peer.net = Net::ED2K;       // a queued eD2k client: Start Transfer gives it a slot now
    l.last().sources.first().net = Net::ED2K;
    return l;
}

// --------------------------------------------------------------------------- networks
QList<NetworkStatus> networks()
{
    QList<NetworkStatus> l;
    auto mk = [](Net n, ConnState s, const QString &text, int peers, qint64 users = -1, qint64 files = -1) {
        NetworkStatus st;
        st.net = n;
        st.state = s;
        st.statusText = text;
        st.peers = peers;
        st.usersOnline = users;
        st.filesOnline = files;
        st.listenPort = 6699;
        return st;
    };
    l << mk(Net::Overlay, ConnState::Connected, QStringLiteral("Ready (5 primary links, 12 incoming)"), 5, 18234, 9123456);
    l << mk(Net::BitTorrent, ConnState::Connected, QStringLiteral("Ready (412 DHT nodes)"), 412);
    l << mk(Net::ED2K, ConnState::Connected, QStringLiteral("Ready (512341 users, 98123456 files) High ID"), 1, 512341, 98123456);
    l << mk(Net::Kad, ConnState::Connected, QStringLiteral("Ready (firewalled: no, 1203 contacts)"), 1203);
    l << mk(Net::Gnutella, ConnState::Connecting, QStringLiteral("Connecting... (2 of 3 ultrapeers)"), 2);
    l << mk(Net::Http, ConnState::Connected, QStringLiteral("Ready"), 0);
    l << mk(Net::Web, ConnState::Connected, QStringLiteral("Ready (3 index sources)"), 3);
    return l;
}

QList<ServerEntry> servers()
{
    QList<ServerEntry> l;
    auto mk = [](Net n, const QString &name, const QString &addr, ConnState s, qint64 users = -1, qint64 files = -1,
                 const QString &desc = QString()) {
        ServerEntry e;
        e.net = n;
        e.id = addr;
        e.name = name;
        e.address = addr;
        e.state = s;
        e.users = users;
        e.files = files;
        e.description = desc;
        return e;
    };
    l << mk(Net::Overlay, QStringLiteral("seed1"), QStringLiteral("seed1.octomx.example:6699"), ConnState::Connected, 18234);
    l << mk(Net::Overlay, QStringLiteral("seed2"), QStringLiteral("seed2.octomx.example:6699"), ConnState::Connected);
    l << mk(Net::BitTorrent, QStringLiteral("BitTorrent DHT"), QStringLiteral("router.bittorrent.com:6881"), ConnState::Connected);
    l << mk(Net::BitTorrent, QStringLiteral("BitTorrent DHT"), QStringLiteral("dht.transmissionbt.com:6881"), ConnState::Connected);
    l << mk(Net::ED2K, QStringLiteral("eMule Security No1"), QStringLiteral("45.82.80.155:5687"), ConnState::Connected, 512341, 98123456);
    l << mk(Net::ED2K, QStringLiteral("DonkeyServer No2"), QStringLiteral("198.51.100.10:4661"), ConnState::Connecting);
    l << mk(Net::ED2K, QStringLiteral("GrupoTS Server"), QStringLiteral("203.0.113.5:4232"), ConnState::Disconnected);
    l << mk(Net::Kad, QStringLiteral("Kad"), QStringLiteral("(kad)"), ConnState::Connected);
    l << mk(Net::Gnutella, QStringLiteral("ultrapeer"), QStringLiteral("192.0.2.44:6346"), ConnState::Connected);
    l << mk(Net::Gnutella, QStringLiteral("ultrapeer"), QStringLiteral("192.0.2.91:6346"), ConnState::Connected);
    l << mk(Net::Gnutella, QStringLiteral("ultrapeer"), QStringLiteral("198.51.100.200:6346"), ConnState::Connecting);
    return l;
}

// --------------------------------------------------------------------------- chat
QString helpRoomName()
{
    return QStringLiteral("OctoMX Help_49717657BABE");
}

QString bigRoomName()
{
    return QStringLiteral("Late Night Chat_") + QString::fromLatin1(fakeHash(QStringLiteral("Late Night Chat"), 6).toHex()).toUpper();
}

QList<ChatRoomInfo> rooms()
{
    QList<ChatRoomInfo> l;
    const char *names[] = {"OctoMX Help", "A dads place", "Oldies but Goldies", "The Jazz Lounge",
                           "Linux Users", "Classic Rock Cafe", "Movie Talk", "Trivia Night", "Dutch Corner",
                           "Hip Hop Headquarters", "Chill Out Room", "Blues Bar", "Metal Mayhem", "80s Forever",
                           "Country Porch", "Reggae Vibes", "Techno Bunker", "Soul Shack", "Gamers Den",
                           "Late Night Chat", "Polski Pokoj", "Sala Latina", "Deutsch Ecke", "Anime Club"};
    // [ren-1] invented demo topics (no longer the ones of real rooms in the reference screenshots)
    const char *topics[] = {"Audio books and old radio plays, everyone welcome", "Friends from the old days",
                            "Talk about art, music, books and more", "Science fiction fans meet here",
                            "Spoken word and podcasts", "Just talk, no files", "Weekend quiz at 8 pm",
                            "Please read the rules before asking", "be nice and have fun", "music music music"};
    QRandomGenerator rng(0xC4A7);
    int i = 0;
    auto add = [&](const QString &base, const QString &topic) {
        ChatRoomInfo r;
        const QString hex = QString::fromLatin1(fakeHash(base, 6).toHex()).toUpper();
        r.name = base + QLatin1Char('_') + hex;
        r.id = r.name;
        r.limit = 40 + rng.bounded(4) * 20;
        r.users = qMin(r.limit, int(rng.bounded(2, 90)));
        r.topic = topic;
        l << r;
    };
    for (const char *n : names) {
        const QString base = QString::fromLatin1(n);
        if (i == 0) {
            ChatRoomInfo r;
            r.name = helpRoomName();
            r.id = r.name;
            r.users = 41;
            r.limit = 100;
            r.topic = QStringLiteral("OctoMX help: ask anything about the program, someone will answer");
            l << r;
        } else if (base == QLatin1String("Late Night Chat")) {
            ChatRoomInfo r;
            r.name = bigRoomName();
            r.id = r.name;
            r.users = 1500;
            r.limit = 2000;
            r.topic = QStringLiteral("The biggest room on the network - 1500 people and counting");
            l << r;
        } else {
            add(base, QStringLiteral("Welcome to %1 - %2").arg(base, QString::fromLatin1(topics[i % 10])));
        }
        ++i;
    }
    // a long tail of generated rooms like the real list (≈ 1959 channels in the reference)
    const char *a[] = {"AAA", "Audio", "Books", "Club", "Dance", "Easy", "Free", "Golden", "House", "Italia",
                       "Jungle", "Karaoke", "Latin", "Music", "Night", "Oldskool", "Party", "Quiet", "Retro", "Spanish"};
    const char *b[] = {"Lounge", "Exchange", "Corner", "Cafe", "Place", "Room", "Zone", "Club", "Bar", "Den",
                       "Hideout", "World", "Station", "Garden", "Palace"};
    for (int k = 0; k < 280; ++k) {
        const QString base = QStringLiteral("%1 %2 %3").arg(QString::fromLatin1(a[k % 20]), QString::fromLatin1(b[(k * 7) % 15])).arg(k + 1);
        add(base, QString::fromLatin1(topics[(k * 3) % 10]));
    }
    return l;
}

QList<ChatUser> roomUsers(const QString &roomId)
{
    QList<ChatUser> l;
    quint32 seed = 0;
    for (QChar c : roomId)
        seed = seed * 31 + c.unicode();
    QRandomGenerator rng(seed);
    int n = 12 + rng.bounded(20);
    if (roomId == helpRoomName())
        n = 41;
    else if (roomId == bigRoomName())
        n = 1500;
    const char *conns[] = {"VDSL", "Cable", "FTTB", "4G", "Unknown", "DSL", "FTTH", "5G", "10 Gbit+"};
    if (roomId == helpRoomName()) {
        ChatUser bot;
        bot.user = user(Net::Overlay, QStringLiteral("HelpBot_30534"));
        bot.files = 0;
        bot.connection = QStringLiteral("FTTH");
        bot.rank = 2;
        l << bot;
        ChatUser holly;
        holly.user = user(Net::Overlay, QStringLiteral("helping_holly353_20112"));
        holly.files = 1226;
        holly.connection = QStringLiteral("VDSL");
        holly.rank = 2;
        l << holly;
        ChatUser twirl;
        twirl.user = user(Net::Overlay, QStringLiteral("Twirl836_24616"));
        twirl.files = 178;
        twirl.connection = QStringLiteral("VDSL");
        l << twirl;
    }
    for (int i = int(l.size()); i < n; ++i) {
        ChatUser u;
        u.user = user(Net::Overlay, userName(rng, kStems, kStemCount));
        u.files = rng.bounded(5) == 0 ? 0 : rng.bounded(1, 3000);
        u.connection = QString::fromLatin1(conns[rng.bounded(9)]);
        u.rank = i == 0 ? 2 : (i < 3 ? 1 : 0);
        l << u;
    }
    return l;
}

QStringList chatLines()
{
    return {QStringLiteral("hi all"), QStringLiteral("anyone got the new version working?"),
            QStringLiteral("lol"), QStringLiteral("brb"), QStringLiteral("what's everyone listening to?"),
            QStringLiteral("check your incoming port settings, 6699 tcp"),
            QStringLiteral("thanks, that fixed it"), QStringLiteral("good evening from Amsterdam"),
            QStringLiteral("is the search working for you guys?"), QStringLiteral("yes, got 164 results"),
            QStringLiteral("welcome back"), QStringLiteral("nice to see this room busy again"),
            QStringLiteral("the FAQ is at https://github.com/robtacconelli/OctoMX"),
            QStringLiteral("does browsing work across networks now?"), QStringLiteral("yep, eD2k users too"),
            QStringLiteral("just shared my whole jazz collection"), QStringLiteral("anyone from Poland here?")};
}

QList<ChatMessage> roomBacklog(const ChatRoomInfo &room)
{
    QList<ChatMessage> l;
    const QDateTime t0 = QDateTime(QDate(2026, 11, 20), QTime(18, 30), QTimeZone::UTC);
    auto add = [&](ChatKind k, const QString &from, const QString &text, int sec) {
        ChatMessage m;
        m.roomId = room.id;
        m.kind = k;
        m.from = user(Net::Overlay, from);
        m.text = text;
        m.time = t0.addSecs(sec);
        l << m;
    };
    if (room.id == helpRoomName()) {
        // the layout of web/chat_room_window.png; [ren-1] the texts are our own demo texts
        add(ChatKind::Topic, QString(), room.topic, 0);
        add(ChatKind::Motd, QString(), QString(), 0);
        add(ChatKind::Motd, QString(), QStringLiteral("This is the OctoMX help room."), 0);
        add(ChatKind::Motd, QString(), QStringLiteral("Stuck with a setting, a port or a download? Ask here."), 0);
        add(ChatKind::Motd, QString(), QStringLiteral("Helpers answer when they can, please be patient,"), 0);
        add(ChatKind::Motd, QString(), QStringLiteral("and share what worked for you."), 0);
        add(ChatKind::Motd, QString(), QStringLiteral("Everyone is welcome, keep it friendly"), 0);
        add(ChatKind::Motd, QString(), QStringLiteral("No flooding, no ads."), 0);
        add(ChatKind::Action, QStringLiteral("HelpBot_30534"), QStringLiteral("says: Please read the room rules before asking!!!"), 1);
        add(ChatKind::Join, QStringLiteral("Twirl836_24616"), QStringLiteral("Twirl836_24616 (DSL 178 files) has entered"), 5);
        add(ChatKind::Message, QStringLiteral("HelpBot_30534"), QStringLiteral("Hi Twirl836_24616, this is the OctoMX help room."), 6);
        add(ChatKind::Message, QStringLiteral("HelpBot_30534"),
            QStringLiteral("New versions of OctoMX: https://github.com/robtacconelli/OctoMX"), 7);
        add(ChatKind::Message, QStringLiteral("HelpBot_30534"), QStringLiteral("Type !help for the list of bot commands"), 8);
        add(ChatKind::Message, QStringLiteral("HelpBot_30534"), QStringLiteral("Questions welcome, just type them in."), 9);
        add(ChatKind::Message, QStringLiteral("Twirl836_24616"), QStringLiteral("hi, my downloads stay at Finding sources... forever"), 40);
        add(ChatKind::Message, QStringLiteral("helping_holly353_20112"),
            QStringLiteral("Twirl836_24616: open Settings > Internet Connection and check the incoming TCP port"), 75);
        add(ChatKind::Message, QStringLiteral("Twirl836_24616"), QStringLiteral("thanks, that fixed it :)"), 130);
        return l;
    }
    add(ChatKind::Motd, QString(), room.topic, 0);
    add(ChatKind::Motd, QString(), QStringLiteral("Please read the rules before asking. No flooding, no ads."), 0);
    const QList<ChatUser> users = roomUsers(room.id);
    const QStringList lines = chatLines();
    for (int i = 0; i < 10 && i < users.size(); ++i) {
        const ChatUser &u = users.at(i);
        if (i % 3 == 0)
            add(ChatKind::Join, u.user.name, QStringLiteral("%1 (%2 %3 files) has entered").arg(u.user.name, u.connection).arg(u.files), 10 + i * 30);
        else
            add(ChatKind::Message, u.user.name, lines.at(i % lines.size()), 10 + i * 30);
    }
    add(ChatKind::Action, users.value(1).user.name, QStringLiteral("waves to everyone"), 400);
    add(ChatKind::Part, users.value(4).user.name, QStringLiteral("%1 has left").arg(users.value(4).user.name), 420);
    return l;
}

// --------------------------------------------------------------------------- hotlist / ignore
QList<HotlistEntry> hotlist()
{
    QList<HotlistEntry> l;
    auto mk = [](Net n, const QString &name, bool online, int files, const QString &conn) {
        HotlistEntry e;
        e.user = user(n, name);
        e.online = online;
        e.files = files;
        e.connection = conn;
        return e;
    };
    l << mk(Net::Overlay, QStringLiteral("marmotwanderer_20391"), true, 1226, QStringLiteral("VDSL"));
    l << mk(Net::Overlay, QStringLiteral("helping_holly_88120"), true, 412, QStringLiteral("Cable"));
    l << mk(Net::ED2K, QStringLiteral("eMuleFriend"), true, 3021, QStringLiteral("VDSL"));
    l << mk(Net::Overlay, QStringLiteral("Brokel_10021"), false, 0, QStringLiteral("Unknown"));
    l << mk(Net::Gnutella, QStringLiteral("198.51.100.7:6346"), false, 0, QStringLiteral("Cable"));
    return l;
}

QList<UserRef> ignored()
{
    return {user(Net::Overlay, QStringLiteral("Wobbly_77777")), user(Net::Overlay, QStringLiteral("NewsBot_42591")),
            user(Net::ED2K, QStringLiteral("spammer2000"))};
}

// --------------------------------------------------------------------------- library
QString sharedRoot()
{
#ifdef Q_OS_WIN
    return QStringLiteral("C:\\MX Shared");
#else
    return QStringLiteral("/home/demo/MX Shared");
#endif
}

QString downloadsRoot()
{
#ifdef Q_OS_WIN
    return QStringLiteral("C:\\Downloads\\OctoMX");
#else
    return QStringLiteral("/home/demo/Downloads/OctoMX");
#endif
}

QList<SharedFolder> sharedFolders()
{
    QList<SharedFolder> l;
    SharedFolder a;
    a.path = sharedRoot();
    a.recursive = true;
    SharedFolder b;
    b.path = downloadsRoot();
    b.recursive = false;
    const QList<SharedFile> files = sharedFiles();
    for (const SharedFile &f : files) {
        SharedFolder &fo = f.path.startsWith(a.path) ? a : b;
        fo.fileCount++;
        fo.totalBytes += f.size;
    }
    l << a << b;
    return l;
}

QList<SharedFile> demoFilesFor(const QString &root)
{
    QList<SharedFile> l;
    const QString sep = QDir::separator();
    struct D { const char *dir; const char *name; int kbps; int sec; };
    QList<D> defs;
    if (root == sharedRoot()) {
        defs = {
            {"", "Big Buck Bunny 1080p.mkv", -1, 596},
            {"", "Sintel.2010.720p.mkv", -1, 888},
            {"", "readme.txt", -1, -1},
            // [demo-1] an invented big band and dub artist, public-domain compositions (no performer named)
            {"The Gooseberry Big Band", "The Gooseberry Big Band - Rooftop Pigeon Stomp.mp3", 192, 213},
            {"The Gooseberry Big Band", "The Gooseberry Big Band - Moonlit Ferry.mp3", 128, 201},
            {"The Gooseberry Big Band", "The Gooseberry Big Band - Hollyhock Lane.mp3", 128, 198},
            {"The Gooseberry Big Band", "The Gooseberry Big Band - Tin Whistle Express.mp3", 160, 207},
            {"Bach", "Bach - Air On The G String.mp3", 160, 241},
            {"Bach", "Bach - Cello Suite No 1 - Prelude.mp3", 128, 187},
            {"Bach", "Bach - Toccata And Fugue In D Minor.mp3", 96, 192},
            {"Chopin", "Chopin - Nocturne Op 9 No 2.mp3", 128, 274},
            {"Chopin", "Chopin - Minute Waltz.mp3", 192, 176},
            {"Chopin", "Chopin - Raindrop Prelude.mp3", 112, 198},
            {"Captain Lentil", "Captain Lentil - Cardboard Rocket Dub.mp3", 128, 312},
            {"Captain Lentil", "Captain Lentil - Heavy Teacup Dub.mp3", 256, 247},
        };
        // a bigger collection with discs, as people shared them (invented titles)
        static const char *jazz[] = {"Cardamom Blues", "Velvet Staircase", "Midnight Lemonade", "Copper Kettle Swing",
                                     "Umbrella Shuffle", "Slow Barge To Ghent", "The Owl's Waltz", "Lantern Street",
                                     "Sixteen Steps Down", "Quiet Harbour", "Two Left Shoes", "Sundial Stomp",
                                     "Brass Lamp Ballad", "Late Tram Home"};
        static QList<QByteArray> store;   // keep the generated strings alive for the D pointers
        if (store.isEmpty()) {
            for (int disc = 1; disc <= 4; ++disc)
                for (int t = 0; t < 14; ++t) {
                    store << QStringLiteral("Jazz Collection%1Disc %2").arg(sep).arg(disc).toUtf8();
                    store << QStringLiteral("%1-%2 - %3.mp3").arg(disc).arg(t + 1, 2, 10, QLatin1Char('0')).arg(QString::fromLatin1(jazz[t])).toUtf8();
                }
        }
        for (int i = 0; i + 1 < store.size(); i += 2)
            defs.append(D{store.at(i).constData(), store.at(i + 1).constData(), (i / 2) % 5 == 0 ? 192 : 128, 180 + (i * 37) % 300});
    } else if (root == downloadsRoot()) {
        defs = {
            {"", "debian-13.2.0-amd64-netinst.iso", -1, -1},
            {"", "Elephants Dream 1080p.mkv", -1, 653},
            {"", "The Gooseberry Big Band - Paper Lantern Parade.mp3", 192, 183},
        };
    }
    QRandomGenerator rng(0x11B);
    for (const D &d : defs) {
        SharedFile f;
        const QString dir = QString::fromUtf8(d.dir);
        f.name = QString::fromUtf8(d.name);
        f.path = root + sep + (dir.isEmpty() ? QString() : dir + sep) + f.name;
        f.bitrateKbps = d.kbps;
        f.durationSec = d.sec;
        f.sampleRateHz = d.kbps > 0 ? 44100 : -1;
        if (d.kbps > 0)
            f.size = qint64(d.sec) * d.kbps * 125 + rng.bounded(3000);
        else if (f.name.endsWith(QLatin1String(".mkv")))
            f.size = 928670754 - qint64(rng.bounded(200000000));
        else if (f.name.endsWith(QLatin1String(".iso")))
            f.size = 663748608;
        else
            f.size = 1834;
        f.type = mediaTypeForFileName(f.name);
        f.hashed = true;
        f.hashes.ed2k = fakeHash(f.name, 16);
        f.hashes.sha1 = fakeHash(f.name + QStringLiteral("sha1"), 20);
        f.mtime = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
        l << f;
    }
    return l;
}

QList<SharedFile> sharedFiles()
{
    return demoFilesFor(sharedRoot()) + demoFilesFor(downloadsRoot());
}

QList<BrowseEntry> browseFiles(const UserRef &u)
{
    QList<BrowseEntry> l;
    quint32 seed = 0;
    for (QChar c : u.name)
        seed = seed * 31 + c.unicode();
    QRandomGenerator rng(seed);
    const char *dirs[] = {"C:\\My Music\\Velorine", "C:\\My Music\\Jazz", "C:\\My Music\\Soul", "D:\\Shared\\Live",
                          "D:\\Shared\\Rare Grooves", "C:\\My Music\\Velorine\\Remixes"};
    // two files the demo transfers know: one running (cyan / blue), one failed (yellow)
    {
        BrowseEntry e;
        e.path = QStringLiteral("C:\\My Music\\Velorine\\Velorine - The Best Of Velorine - Entire Album.mp3");
        e.size = 106764288;
        e.bitrateKbps = 192;
        e.durationSec = durationFor(e.size, 192);
        e.sampleRateHz = 44100;
        e.hashes.ed2k = fakeHash(QStringLiteral("Velorine - The Best Of Velorine - Entire Album.mp3"), 16);
        l << e;
        e.path = QStringLiteral("C:\\My Music\\Velorine\\Velorine - Marigold.mp3");
        e.size = 5195650;
        e.bitrateKbps = 128;
        e.durationSec = durationFor(e.size, 128);
        e.hashes.ed2k = fakeHash(QStringLiteral("Velorine - Marigold.mp3"), 16);
        l << e;
    }
    for (int i = 0; i < 300; ++i) {
        BrowseEntry e;
        const QString title = QString::fromLatin1(kArtistTitles[rng.bounded(kArtistTitleCount)]);
        const int kb[] = {128, 128, 192, 160, 320, 96, 112};
        e.bitrateKbps = kb[rng.bounded(7)];
        e.durationSec = rng.bounded(150, 400);
        e.sampleRateHz = 44100;
        e.size = qint64(e.durationSec) * e.bitrateKbps * 125;
        e.path = QString::fromLatin1(dirs[rng.bounded(6)]) + QStringLiteral("\\Track %1 - %2.mp3").arg(i + 1, 3, 10, QLatin1Char('0')).arg(title);
        e.hashes.ed2k = fakeHash(e.path, 16);
        l << e;
    }
    BrowseEntry v;
    v.path = QStringLiteral("D:\\Shared\\Video\\Big Buck Bunny 1080p.mkv");
    v.size = 928670754;
    v.durationSec = 596;
    l << v;
    return l;
}

QVariantMap whois(const UserRef &u)
{
    // the same keys the real core / overlay plugin report
    QVariantMap m;
    quint32 seed = 0;
    for (QChar c : u.name)
        seed = seed * 31 + c.unicode();
    QRandomGenerator rng(seed);
    m.insert(QStringLiteral("name"), u.name);
    m.insert(QStringLiteral("network"), netName(u.net));
    m.insert(QStringLiteral("connection"), QStringLiteral("VDSL"));
    m.insert(QStringLiteral("files"), 1234 + int(rng.bounded(3000)));
    m.insert(QStringLiteral("totalSize"), qint64(12345) * 1024 * 1024 + rng.bounded(1000000));
    m.insert(QStringLiteral("uploads"), QStringLiteral("2 of 3 available, 0 in queue"));
    m.insert(QStringLiteral("onlineSec"), 2 * 3600 + 13 * 60 + int(rng.bounded(50)));
    m.insert(QStringLiteral("client"), QStringLiteral("OctoMX " MX_VERSION_STRING));
    m.insert(QStringLiteral("reachable"), true);
    return m;
}

BandwidthSample bandwidthAt(qint64 t, qint64 timeMs)
{
    // Deterministic pseudo-random but smooth: sum of sines + hashed bursts.
    const double x = double(t);
    double down = 9000 + 5000 * std::sin(x / 37.0) + 2500 * std::sin(x / 11.0 + 1.3) + 1200 * std::sin(x / 3.1);
    double up = 7000 + 2500 * std::sin(x / 53.0 + 0.7) + 1500 * std::sin(x / 7.3) + 800 * std::sin(x / 2.3 + 2.0);
    const quint32 h = quint32(t * 2654435761u);
    if ((h >> 27) == 3)
        down += 6000;
    if ((h >> 28) == 5)
        up += 3000;
    BandwidthSample s;
    s.timeMs = timeMs;
    s.downBps = qMax(0, int(down));
    s.upBps = qMax(0, int(up));
    return s;
}

QList<NetRate> overheadAt(qint64 t)
{
    // protocol traffic per network, shaped like web/bandwidth_graph_page.png: a dominant, spiky
    // overlay-routing band, DHT packets with bursts, a little peer and server overhead
    const double x = double(t);
    const quint32 h = quint32(t * 2654435761u);
    auto spike = [h](int shift, int every, int amount) { return int((h >> shift) % every) == 0 ? amount : 0; };
    QList<NetRate> l;
    l << NetRate{Net::Overlay, int(5600 + 1100 * std::sin(x / 41.0) + 700 * std::sin(x / 6.3) + 500 * std::sin(x / 1.7)) + spike(3, 9, 3500),
                 int(5900 + 600 * std::sin(x / 33.0 + 1.0) + 400 * std::sin(x / 4.1)) + spike(5, 23, 2500)};
    l << NetRate{Net::Kad, int(700 + 400 * std::sin(x / 13.0) + 250 * std::sin(x / 2.9)) + spike(7, 6, 900),
                 int(800 + 350 * std::sin(x / 17.0 + 2.0) + 200 * std::sin(x / 3.3)) + spike(9, 7, 700)};
    l << NetRate{Net::BitTorrent, int(350 + 150 * std::sin(x / 5.0)), int(420 + 120 * std::sin(x / 7.0))};
    l << NetRate{Net::ED2K, int(160 + 80 * std::sin(x / 9.0)), int(110 + 40 * std::sin(x / 11.0))};
    l << NetRate{Net::Gnutella, int(90 + 40 * std::sin(x / 4.0)), int(70 + 30 * std::sin(x / 5.0))};
    for (NetRate &r : l) {
        r.down = qMax(0, r.down);
        r.up = qMax(0, r.up);
    }
    return l;
}

double payloadShape(qint64 secondsAgo, qint64 t)
{
    // history of the file transfers: running for the last ~7.5 minutes, with a stall in between
    if (secondsAgo > 450)
        return 0.0;
    if (secondsAgo > 250 && secondsAgo < 300)
        return 0.05;
    // never above today's rate (the UI explains the payload with today's transfer rates)
    return 0.87 + 0.08 * std::sin(double(t) / 3.0) + 0.05 * std::sin(double(t) / 17.0);
}

} // namespace mx::mock
