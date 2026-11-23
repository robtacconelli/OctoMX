// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest/QtTest>
#include <QtTest/QSignalSpy>

#include "core/network_manager.h"
#include "core/search_manager.h"
#include "core/search_hygiene.h"
#include "core/search_merge.h"
#include "fake_plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

using namespace mx;
using mx::test::FakePlugin;

namespace {
QByteArray h(char c, int n) { return QByteArray(n, c); }
FileHashes ed2k(char c) { FileHashes f; f.ed2k = h(c, 16); return f; }
FileHashes sha1(char c) { FileHashes f; f.sha1 = h(c, 20); return f; }
constexpr qint64 MB = 1024 * 1024;
}

class TestSearch : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { registerMetaTypes(); }

    // [core-5] media columns of network hits: units are kbps / seconds / Hz; values a file cannot
    // have are dropped (empty cell), never replaced by size / length
    void hitMediaSanitized()
    {
        auto hit = [](const QString &name, qint64 size, int dur, int kbps, int hz = -1) {
            SearchHit h;
            h.fileName = name;
            h.size = size;
            h.durationSec = dur;
            h.bitrateKbps = kbps;
            h.sampleRateHz = hz;
            hygiene::sanitizeMedia(h);
            return h;
        };
        SearchHit a = hit(QStringLiteral("Song.mp3"), 4800000, 300, 128, 44100);
        QCOMPARE(a.bitrateKbps, 128);
        QCOMPARE(a.durationSec, 300);
        QCOMPARE(a.sampleRateHz, 44100);
        // the qa-1 case: a 1080p video tagged "14" (a unit slip): no bitrate, the length stays
        SearchHit v = hit(QStringLiteral("Big_Buck_Bunny_1080p.mkv"), 928670754, 596, 14);
        QCOMPARE(v.bitrateKbps, -1);
        QCOMPARE(v.durationSec, 596);
        v = hit(QStringLiteral("Big_Buck_Bunny_1080p.mkv"), 928670754, -1, 14);
        QCOMPARE(v.bitrateKbps, -1);
        // a plausible video bitrate (eMule FT_MEDIA_BITRATE, kbps) is kept: 210 MB / 596 s ~ 2.9 Mbit/s
        v = hit(QStringLiteral("Big_Buck_Bunny_854x480_Surround.avi"), 220524624, 596, 2957);
        QCOMPARE(v.bitrateKbps, 2957);
        // bps instead of kbps
        v = hit(QStringLiteral("Sintel.2010.1080p.mkv"), 1200000000, 888, 10810000);
        QCOMPARE(v.bitrateKbps, -1);
        // nonsense values and media columns on a non-media file
        SearchHit z = hit(QStringLiteral("Song.mp3"), 4800000, 0, 0, 7);
        QCOMPARE(z.durationSec, -1);
        QCOMPARE(z.bitrateKbps, -1);
        QCOMPARE(z.sampleRateHz, -1);
        SearchHit iso = hit(QStringLiteral("debian-12.iso"), 700000000, 300, 128, 44100);
        QCOMPARE(iso.durationSec, -1);
        QCOMPARE(iso.bitrateKbps, -1);
        QCOMPARE(iso.sampleRateHz, -1);
    }

    void normalizedNames()
    {
        QCOMPARE(SearchMerger::normalizedName(QStringLiteral("Velorine - Beside The Mill Pond.MP3")), QStringLiteral("velorine beside the mill pond.mp3"));
        QCOMPARE(SearchMerger::normalizedName(QStringLiteral("velorine_-_beside__the_mill.pond.mp3")), QStringLiteral("velorine beside the mill pond.mp3"));
        QCOMPARE(SearchMerger::normalizedName(QStringLiteral("C:\\music\\Velorine (live).mp3")), QStringLiteral("velorine live.mp3"));
        QVERIFY(SearchMerger::normalizedName(QStringLiteral("a.mp3")) != SearchMerger::normalizedName(QStringLiteral("a.ogg")));
    }

    void mergeByHash()
    {
        SearchMerger m;
        FileHashes both = ed2k('a');
        both.sha1 = h('s', 20);
        auto r1 = m.add(FakePlugin::makeHit(QStringLiteral("song.mp3"), 5 * MB, ed2k('a'), Net::ED2K, QStringLiteral("u1")));
        QVERIFY(r1.added);
        QCOMPARE(m.hits().at(0).key, QStringLiteral("ed2k:") + QString::fromLatin1(h('a', 16).toHex()));
        // same ed2k from another user/network → merged, hashes combined
        auto r2 = m.add(FakePlugin::makeHit(QStringLiteral("Song (better name).mp3"), 5 * MB, both, Net::Overlay, QStringLiteral("u2")));
        QVERIFY(!r2.added);
        QCOMPARE(r2.index, 0);
        QCOMPARE(m.size(), 1);
        QCOMPARE(m.hits().at(0).sources.size(), 2);
        QCOMPARE(m.hits().at(0).hashes.sha1, both.sha1);
        QCOMPARE(m.hits().at(0).fileName, QStringLiteral("Song (better name).mp3"));   // longest name wins
        // now a sha1-only hit joins through the learned sha1
        auto r3 = m.add(FakePlugin::makeHit(QStringLiteral("song.mp3"), 5 * MB, sha1('s'), Net::Gnutella, QStringLiteral("u3")));
        QCOMPARE(r3.index, 0);
        QCOMPARE(m.hits().at(0).sources.size(), 3);
        QVERIFY(!m.hits().at(0).sources.at(2).extra.value(QStringLiteral("candidate")).toBool());
        // the same source again only refreshes it
        SearchHit again = FakePlugin::makeHit(QStringLiteral("song.mp3"), 5 * MB, ed2k('a'), Net::ED2K, QStringLiteral("u1"));
        again.sources[0].freeSlots = 3;
        m.add(again);
        QCOMPARE(m.hits().at(0).sources.size(), 3);
        QCOMPARE(m.hits().at(0).sources.at(0).freeSlots, 3);
        // metadata is filled from any hit
        SearchHit meta = FakePlugin::makeHit(QStringLiteral("song.mp3"), 5 * MB, ed2k('a'), Net::ED2K, QStringLiteral("u4"));
        meta.durationSec = 215;
        meta.bitrateKbps = 192;
        m.add(meta);
        QCOMPARE(m.hits().at(0).durationSec, 215);
        QCOMPARE(m.hits().at(0).bitrateKbps, 192);
    }

    void conflictsNeverMerge()
    {
        SearchMerger m;
        FileHashes a = ed2k('a');
        a.sha1 = h('1', 20);
        FileHashes b = ed2k('a');
        b.sha1 = h('2', 20);   // same ed2k, different sha1 → conflict
        m.add(FakePlugin::makeHit(QStringLiteral("x.avi"), 700 * MB, a, Net::ED2K, QStringLiteral("u1")));
        auto r = m.add(FakePlugin::makeHit(QStringLiteral("x.avi"), 700 * MB, b, Net::Gnutella, QStringLiteral("u2")));
        QVERIFY(r.added);
        QCOMPARE(m.size(), 2);
        QVERIFY(m.hits().at(0).key != m.hits().at(1).key);   // keys stay unique
        QVERIFY(m.hits().at(1).key.endsWith(QStringLiteral("#2")));
        // different ed2k but same name/size: hashes of the same kind differ → not a candidate either
        auto r2 = m.add(FakePlugin::makeHit(QStringLiteral("x.avi"), 700 * MB, ed2k('z'), Net::ED2K, QStringLiteral("u3")));
        QVERIFY(r2.added);
        QCOMPARE(m.size(), 3);
    }

    void candidateBySizeAndName()
    {
        SearchMerger m;
        m.add(FakePlugin::makeHit(QStringLiteral("Big Buck Bunny.avi"), 200 * MB, ed2k('e'), Net::ED2K, QStringLiteral("u1")));
        // no shared hash kind, same size, same normalised name → candidate
        auto r = m.add(FakePlugin::makeHit(QStringLiteral("big_buck_bunny.AVI"), 200 * MB, sha1('s'), Net::Gnutella, QStringLiteral("u2")));
        QVERIFY(!r.added);
        QVERIFY(r.candidate);
        const SearchHit &hit = m.hits().at(0);
        QCOMPARE(hit.sources.size(), 2);
        QVERIFY(hit.sources.at(1).extra.value(QStringLiteral("candidate")).toBool());
        QCOMPARE(FileHashes::fromVariant(hit.sources.at(1).extra.value(QStringLiteral("hashes")).toMap()).sha1, h('s', 20));
        QVERIFY(hit.hashes.sha1.isEmpty());   // candidate hashes are NOT merged into the hit
        // different size → separate
        QVERIFY(m.add(FakePlugin::makeHit(QStringLiteral("Big Buck Bunny.avi"), 201 * MB, sha1('t'), Net::Gnutella, QStringLiteral("u3"))).added);
        // too small for name matching (< 1 MiB) → separate
        SearchMerger small;
        small.add(FakePlugin::makeHit(QStringLiteral("tiny.txt"), 1000, ed2k('1'), Net::ED2K, QStringLiteral("a")));
        QVERIFY(small.add(FakePlugin::makeHit(QStringLiteral("tiny.txt"), 1000, sha1('2'), Net::Gnutella, QStringLiteral("b"))).added);
        // no hashes at all: size+name key
        SearchMerger plain;
        plain.add(FakePlugin::makeHit(QStringLiteral("Some File.mkv"), 2 * MB, FileHashes(), Net::Web, QStringLiteral("a")));
        QCOMPARE(plain.hits().at(0).key, QStringLiteral("size+name:%1:some file.mkv").arg(2 * MB));
        QVERIFY(!plain.add(FakePlugin::makeHit(QStringLiteral("some-file.mkv"), 2 * MB, FileHashes(), Net::Web, QStringLiteral("b"))).added);
    }

    void bundles()
    {
        SearchMerger m;
        SearchHit bundle = FakePlugin::makeHit(QStringLiteral("Album"), 100 * MB, FileHashes(), Net::BitTorrent);
        bundle.hashes.btih = h('b', 20);
        bundle.isBundle = true;
        m.add(bundle);
        QCOMPARE(m.hits().at(0).key, QStringLiteral("btih:") + QString::fromLatin1(h('b', 20).toHex()));
        // a single file with the same name/size is not merged with a bundle
        QVERIFY(m.add(FakePlugin::makeHit(QStringLiteral("Album"), 100 * MB, ed2k('q'), Net::ED2K, QStringLiteral("u"))).added);
        // the same torrent again merges (swarm source deduplicated by uri)
        SearchHit again = bundle;
        again.sources[0].seeds = 12;
        QVERIFY(!m.add(again).added);
        QCOMPARE(m.hits().at(0).sources.size(), 1);
        QCOMPARE(m.hits().at(0).sources.at(0).seeds, 12);
        // file 2 inside the same torrent is a different file
        SearchHit f2 = FakePlugin::makeHit(QStringLiteral("track2.mp3"), 5 * MB, FileHashes(), Net::BitTorrent);
        f2.hashes.btih = h('b', 20);
        f2.hashes.btFileIndex = 2;
        SearchHit f3 = f2;
        f3.hashes.btFileIndex = 3;
        f3.fileName = QStringLiteral("track3.mp3");
        QVERIFY(m.add(f2).added);
        QVERIFY(m.add(f3).added);
        QCOMPARE(m.hits().last().key, QStringLiteral("btih:%1:3").arg(QString::fromLatin1(h('b', 20).toHex())));
    }

    void retroMergeOfLinkedRows()
    {
        SearchMerger m;
        m.add(FakePlugin::makeHit(QStringLiteral("a.iso"), 700 * MB, ed2k('e'), Net::ED2K, QStringLiteral("u1")));
        m.add(FakePlugin::makeHit(QStringLiteral("other.bin"), 5 * MB, ed2k('o'), Net::ED2K, QStringLiteral("u9")));
        m.add(FakePlugin::makeHit(QStringLiteral("a-renamed.iso"), 700 * MB, sha1('s'), Net::Gnutella, QStringLiteral("u2")));
        QCOMPARE(m.size(), 3);
        const QString sha1Key = m.hits().at(2).key;
        // a hit carrying both hashes proves rows 0 and 2 are one file
        FileHashes both = ed2k('e');
        both.sha1 = h('s', 20);
        auto r = m.add(FakePlugin::makeHit(QStringLiteral("a.iso"), 700 * MB, both, Net::Overlay, QStringLiteral("u3")));
        QCOMPARE(r.index, 0);
        QCOMPARE(r.removedKeys, QStringList{sha1Key});
        QCOMPARE(m.size(), 2);
        const SearchHit &merged = m.hits().at(0);
        QCOMPARE(merged.sources.size(), 3);
        QCOMPARE(merged.hashes.sha1, h('s', 20));
        QCOMPARE(m.indexOfKey(sha1Key), -1);
        QCOMPARE(m.indexOfKey(m.hits().at(1).key), 1);   // indexes rebuilt after the removal
        // later hits find the merged row through either hash
        QCOMPARE(m.add(FakePlugin::makeHit(QStringLiteral("a.iso"), 700 * MB, sha1('s'), Net::Gnutella, QStringLiteral("u4"))).index, 0);
        QCOMPARE(m.add(FakePlugin::makeHit(QStringLiteral("x"), 5 * MB, ed2k('o'), Net::ED2K, QStringLiteral("u5"))).index, 1);
        // conflicting rows are never absorbed
        SearchMerger c;
        FileHashes a = ed2k('1');
        a.tth = QByteArray(24, 'A');
        FileHashes b = sha1('2');
        b.tth = QByteArray(24, 'B');
        c.add(FakePlugin::makeHit(QStringLiteral("f"), 3 * MB, a, Net::ED2K, QStringLiteral("x")));
        c.add(FakePlugin::makeHit(QStringLiteral("g"), 3 * MB, b, Net::Gnutella, QStringLiteral("y")));
        FileHashes link = ed2k('1');
        link.sha1 = h('2', 20);   // links both, but rows 0 and 1 conflict on tth
        auto rc = c.add(FakePlugin::makeHit(QStringLiteral("f"), 3 * MB, link, Net::Overlay, QStringLiteral("z")));
        QVERIFY(rc.removedKeys.isEmpty());
        QCOMPARE(c.size(), 2);
    }

    // [merge-1] the one name rule
    void nameRule()
    {
        QCOMPARE(SearchMerger::probableExtension(QStringLiteral("Sintel.2010.720p.MKV")), QStringLiteral("mkv"));
        QCOMPARE(SearchMerger::probableExtension(QStringLiteral("Sintel 2010 720p mkv")), QStringLiteral("mkv"));   // dots lost by an index
        QCOMPARE(SearchMerger::probableExtension(QStringLiteral("Sintel.2010.4K.DMRip.x264-GROUP")), QString());
        QCOMPARE(SearchMerger::probableExtension(QStringLiteral("Sintel (2010) [720p]")), QString());
        QCOMPARE(SearchMerger::nameWords(QStringLiteral("Sintel 2010 720p mkv")), QStringList({QStringLiteral("sintel"), QStringLiteral("2010"), QStringLiteral("720p")}));
        QVERIFY(SearchMerger::sameName(QStringLiteral("Sintel.2010.720p.mkv"), QStringLiteral("Sintel 2010 720p mkv")));
        QVERIFY(SearchMerger::sameName(QStringLiteral("Sintel.mp4"), QStringLiteral("sintel 2010 Surround mp4")));   // 1 of 1 words
        QVERIFY(SearchMerger::sameName(QStringLiteral("Big_Buck_Bunny_1080p_surround.avi"), QStringLiteral("big buck bunny 1080p.avi")));
        QVERIFY(!SearchMerger::sameName(QStringLiteral("Big Buck Bunny.avi"), QStringLiteral("Elephants Dream.avi")));
        QVERIFY(!SearchMerger::sameName(QStringLiteral("01.mp3"), QStringLiteral("02.mp3")));
        const qint64 s = 673935402;
        QVERIFY(SearchMerger::sameFile(QStringLiteral("Sintel.2010.720p.mkv"), s, QStringLiteral("Sintel 2010 720p mkv"), s));
        QVERIFY(!SearchMerger::sameFile(QStringLiteral("Sintel.2010.720p.mkv"), s, QStringLiteral("Sintel 2010 720p mkv"), s + 1));   // one byte off
        QVERIFY(!SearchMerger::sameFile(QStringLiteral("Sintel.2010.720p.mkv"), s, QStringLiteral("Sintel.2010.720p.mp4"), s));     // extension
        QVERIFY(!SearchMerger::sameFile(QStringLiteral("Sintel"), s, QStringLiteral("Sintel"), s));                                   // no extension
        QVERIFY(!SearchMerger::sameFile(QStringLiteral("a.mkv"), 1000, QStringLiteral("a.mkv"), 1000));                               // < 1 MiB
        QVERIFY(SearchMerger::isPadFile(QStringLiteral(".pad/1234")));
        QVERIFY(SearchMerger::isPadFile(QStringLiteral("Folder/_____padding_file_3_")));
    }

    // [merge-1] rule 2: a single-file torrent and a plain file of another network are one row,
    // in both orders, for every pair of networks
    void singleFileTorrentMeetsPlainFile()
    {
        const qint64 size = 673935402;
        auto torrent = [&](const QString &name, char c, bool bundleFlag, int files = -1) {
            SearchHit t = FakePlugin::makeHit(name, size, FileHashes(), Net::BitTorrent);
            t.hashes.btih = h(c, 20);
            t.isBundle = bundleFlag;
            t.sources[0].uri = QStringLiteral("magnet:?xt=urn:btih:") + QString::fromLatin1(h(c, 20).toHex());
            t.sources[0].seeds = 30;
            if (files >= 0) t.sources[0].extra.insert(QStringLiteral("files"), files);
            return t;
        };
        // the owner's case: an index lost the dots, so the torrent was flagged as a bundle
        for (int order = 0; order < 2; ++order) {
            SearchMerger m;
            const SearchHit bt = torrent(QStringLiteral("Sintel 2010 720p mkv"), 'b', true);
            const SearchHit ed = FakePlugin::makeHit(QStringLiteral("Sintel.2010.720p.mkv"), size, ed2k('e'), Net::ED2K, QStringLiteral("u1"));
            const SearchHit kad = FakePlugin::makeHit(QStringLiteral("Sintel.2010.720p.mkv"), size, ed2k('e'), Net::Kad, QStringLiteral("u2"));
            if (order == 0) { m.add(bt); m.add(ed); m.add(kad); } else { m.add(ed); m.add(bt); m.add(kad); }
            QCOMPARE(m.size(), 1);
            const SearchHit &row = m.hits().at(0);
            QVERIFY(!row.isBundle);
            QCOMPARE(row.sources.size(), 3);
            int cand = 0;
            for (const SearchSource &src : row.sources) if (src.extra.value(QStringLiteral("candidate")).toBool()) ++cand;
            QVERIFY(cand >= 1);
            // the side matched by name and size carries the flag: eD2k when the torrent came first
            const Net candNet = order == 0 ? Net::ED2K : Net::BitTorrent;
            bool found = false;
            for (const SearchSource &src : row.sources)
                if (src.net == candNet && src.extra.value(QStringLiteral("candidate")).toBool()) found = true;
            QVERIFY(found);
        }
        // Gnutella (SHA-1) against eD2k, HTTP against the overlay: every pair of networks
        SearchMerger g;
        g.add(FakePlugin::makeHit(QStringLiteral("Sintel.2010.720p.mkv"), size, ed2k('e'), Net::ED2K, QStringLiteral("u1")));
        QVERIFY(g.add(FakePlugin::makeHit(QStringLiteral("sintel_2010_720p.mkv"), size, sha1('s'), Net::Gnutella, QStringLiteral("g1"))).candidate);
        QVERIFY(g.add(FakePlugin::makeHit(QStringLiteral("Sintel 2010 720p.mkv"), size, FileHashes(), Net::Http, QStringLiteral("h1"))).candidate);
        QCOMPARE(g.size(), 1);
        // an index reporting the file count: one file is a file, several files a folder
        SearchMerger c;
        c.add(FakePlugin::makeHit(QStringLiteral("Sintel.2010.720p.mkv"), size, ed2k('e'), Net::ED2K, QStringLiteral("u1")));
        QVERIFY(c.add(torrent(QStringLiteral("Sintel.2010.720p.mkv"), 'c', true, 1)).candidate);
        QVERIFY(c.add(torrent(QStringLiteral("Sintel 2010 720p mkv"), 'd', true, 3)).added);   // a folder: not merged
        QCOMPARE(c.size(), 2);
        // another index lists the same torrent with a rounded size: joins through the candidate's hash
        SearchHit rounded = torrent(QStringLiteral("Sintel.2010.720p.mkv"), 'c', false);
        rounded.size = 674000000;
        const auto rr = c.add(rounded);
        QCOMPARE(rr.index, 0);
        QVERIFY(rr.candidate);
    }

    // [merge-1] do not over-merge
    void noOverMerge()
    {
        const qint64 size = 463357580;
        SearchMerger m;
        m.add(FakePlugin::makeHit(QStringLiteral("Sintel.2010.2K.mp4"), size, ed2k('e'), Net::ED2K, QStringLiteral("u1")));
        QVERIFY(m.add(FakePlugin::makeHit(QStringLiteral("Sintel.2010.2K.mp4"), size + 1, sha1('a'), Net::Gnutella, QStringLiteral("g"))).added);
        QVERIFY(m.add(FakePlugin::makeHit(QStringLiteral("Sintel.2010.2K.mkv"), size, sha1('b'), Net::Gnutella, QStringLiteral("g"))).added);
        QVERIFY(m.add(FakePlugin::makeHit(QStringLiteral("Elephants.Dream.mp4"), size, sha1('c'), Net::Gnutella, QStringLiteral("g"))).added);
        // conflicting hashes never merge, even with the same name and size
        QVERIFY(m.add(FakePlugin::makeHit(QStringLiteral("Sintel.2010.2K.mp4"), size, ed2k('z'), Net::Kad, QStringLiteral("k"))).added);
        QCOMPARE(m.size(), 5);
        // the 4.4 rule (setting off): a torrent flagged as a bundle never meets a plain file
        SearchMerger old(1024 * 1024, false);
        SearchHit bt = FakePlugin::makeHit(QStringLiteral("Sintel 2010 2K mp4"), size, FileHashes(), Net::BitTorrent);
        bt.hashes.btih = h('b', 20);
        bt.isBundle = true;
        old.add(bt);
        QVERIFY(old.add(FakePlugin::makeHit(QStringLiteral("Sintel.2010.2K.mp4"), size, ed2k('e'), Net::ED2K, QStringLiteral("u1"))).added);
        QCOMPARE(old.size(), 2);
    }

    // [merge-1] rule 3: a file inside a folder torrent; a late file list; split again; two torrents
    void innerFilesAndLateLists()
    {
        const qint64 movie = 143100616;
        auto folder = [&](char c, const QString &name, bool withList) {
            SearchHit t = FakePlugin::makeHit(name, movie + 55668, FileHashes(), Net::BitTorrent);
            t.hashes.btih = h(c, 20);
            t.isBundle = true;
            t.sources[0].uri = QStringLiteral("magnet:?xt=urn:btih:") + QString::fromLatin1(h(c, 20).toHex());
            t.sources[0].seeds = 100;
            if (withList) {
                t.files << BundleFile{QStringLiteral("Sintel (2010)/Sintel.2010.720p.BluRay.x264.AAC.mp4"), movie, 0}
                        << BundleFile{QStringLiteral("Sintel (2010)/www.example.txt"), 200, 1}
                        << BundleFile{QStringLiteral("Sintel (2010)/Subs/English.srt"), 55468, 2};
            }
            return t;
        };
        SearchMerger m;
        m.add(FakePlugin::makeHit(QStringLiteral("Sintel.2010.720p.BluRay.x264.AAC.mp4"), movie, ed2k('e'), Net::ED2K, QStringLiteral("u1")));
        // the folder arrives without its list: one file-list request is worth it
        const auto rb = m.add(folder('f', QStringLiteral("Sintel (2010) [720p] [BluRay]"), false));
        QVERIFY(rb.added);
        QCOMPARE(m.size(), 2);
        QCOMPARE(m.bundlesWorthFileList(0), QStringList{m.hits().at(1).key});
        QCOMPARE(m.bundlesWorthFileList(1), QStringList{m.hits().at(1).key});
        // the list arrives: the plain row gains the torrent as a source, the folder row stays
        SearchHit::InnerMatch none;
        Q_UNUSED(none)
        SearchHit withList = folder('f', QString(), true);
        const auto u = m.setBundleFiles(h('f', 20), withList.files);
        QVERIFY(u.removedKeys.isEmpty());
        QCOMPARE(m.size(), 2);
        const SearchHit &plain = m.hits().at(0);
        const SearchHit &bundle = m.hits().at(1);
        QVERIFY(u.changedKeys.contains(plain.key));
        QVERIFY(bundle.isBundle);
        QCOMPARE(bundle.innerMatches.size(), 1);
        QCOMPARE(bundle.innerMatches.first().file, 0);
        QCOMPARE(bundle.innerMatches.first().rowKey, plain.key);
        QCOMPARE(bundle.innerMatches.first().networks, QList<Net>{Net::ED2K});
        QCOMPARE(plain.sources.size(), 2);
        const SearchSource inner = plain.sources.at(1);
        QCOMPARE(inner.net, Net::BitTorrent);
        QVERIFY(inner.extra.value(QStringLiteral("candidate")).toBool());
        QCOMPARE(inner.extra.value(QStringLiteral("innerOf")).toString(), bundle.key);
        QCOMPARE(inner.extra.value(QStringLiteral("btFileIndex")).toInt(), 0);
        QVERIFY(inner.uri.endsWith(QStringLiteral("&so=0")));
        QCOMPARE(inner.seeds, 100);
        // a second, different torrent holding the same file: a source too, its own bundle row
        const auto r2 = m.add(folder('g', QStringLiteral("Sintel 2010 720p BluRay pack"), true));
        QVERIFY(r2.added);
        QCOMPARE(m.size(), 3);
        QCOMPARE(m.hits().at(0).sources.size(), 3);
        QVERIFY(r2.changedKeys.contains(m.hits().at(0).key));
        // the plain file arrives after the folder (other order): linked at once
        SearchMerger o;
        o.add(folder('f', QStringLiteral("Sintel (2010) [720p] [BluRay]"), true));
        const auto rp = o.add(FakePlugin::makeHit(QStringLiteral("Sintel.2010.720p.BluRay.x264.AAC.mp4"), movie, sha1('s'), Net::Gnutella, QStringLiteral("g1")));
        QVERIFY(rp.added);
        QCOMPARE(o.hits().at(1).sources.size(), 2);
        QCOMPARE(o.hits().at(0).innerMatches.size(), 1);
        QVERIFY(rp.changedKeys.contains(o.hits().at(0).key));
        // a different size (one byte) is not the file
        SearchMerger off;
        off.add(folder('f', QStringLiteral("Sintel (2010) [720p] [BluRay]"), true));
        off.add(FakePlugin::makeHit(QStringLiteral("Sintel.2010.720p.BluRay.x264.AAC.mp4"), movie + 1, sha1('s'), Net::Gnutella, QStringLiteral("g1")));
        QCOMPARE(off.hits().at(1).sources.size(), 1);
        QVERIFY(off.hits().at(0).innerMatches.isEmpty());
    }

    void splitWhenTheTorrentIsAFolder()
    {
        const qint64 size = 310674005;
        SearchMerger m;
        m.add(FakePlugin::makeHit(QStringLiteral("Sintel.mp4"), size, ed2k('e'), Net::ED2K, QStringLiteral("u1")));
        SearchHit bt = FakePlugin::makeHit(QStringLiteral("sintel 2010 Surround mp4"), size, FileHashes(), Net::BitTorrent);
        bt.hashes.btih = h('b', 20);
        bt.isBundle = true;
        bt.sources[0].uri = QStringLiteral("magnet:?xt=urn:btih:") + QString::fromLatin1(h('b', 20).toHex());
        QVERIFY(m.add(bt).candidate);
        QCOMPARE(m.size(), 1);
        const QString key = m.hits().at(0).key;
        // metadata: two files of which none is the plain file → split again, plain row keeps its key
        QList<BundleFile> files{BundleFile{QStringLiteral("Sintel/sintel-part1.mp4"), size - 1000, 0},
                                BundleFile{QStringLiteral("Sintel/info.txt"), 1000, 1}};
        const auto u = m.setBundleFiles(h('b', 20), files);
        QCOMPARE(m.size(), 2);
        QCOMPARE(m.hits().at(0).key, key);
        QCOMPARE(m.hits().at(0).sources.size(), 1);
        QVERIFY(m.hits().at(1).isBundle);
        QCOMPARE(m.hits().at(1).files.size(), 2);
        QVERIFY(u.changedKeys.contains(key));
        // a late list with ONE real file (and a pad file) makes a bundle-flagged torrent a single file
        SearchMerger s;
        s.add(FakePlugin::makeHit(QStringLiteral("Sintel.2010.Surround.mp4"), size, ed2k('e'), Net::ED2K, QStringLiteral("u1")));
        SearchHit folderish = bt;
        folderish.fileName = QStringLiteral("Sintel (2010)");   // no extension: no match by name yet
        s.add(folderish);
        QCOMPARE(s.size(), 2);
        s.setBundleFiles(h('b', 20), {BundleFile{QStringLiteral("Sintel.2010.Surround.mp4"), size, 0}, BundleFile{QStringLiteral(".pad/12"), 12, 1}});
        QCOMPARE(s.size(), 1);
        QCOMPARE(s.hits().at(0).sources.size(), 2);
    }

    void managerAnnouncesRemovedRows()
    {
        SimpleSettings settings;
        auto *ed = new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch);
        auto *gn = new FakePlugin(Net::Gnutella, NetworkPlugin::CanSearch);
        auto *ov = new FakePlugin(Net::Overlay, NetworkPlugin::CanSearch);
        ed->cannedHits << FakePlugin::makeHit(QStringLiteral("film.mkv"), 700 * MB, ed2k('e'), Net::ED2K, QStringLiteral("u1"));
        // [merge-1] a name the name rule does not match ("film (1).mkv" would now merge at once)
        gn->cannedHits << FakePlugin::makeHit(QStringLiteral("holiday clip.mkv"), 700 * MB, sha1('s'), Net::Gnutella, QStringLiteral("u2"));
        FileHashes both = ed2k('e');
        both.sha1 = h('s', 20);
        ov->cannedHits << FakePlugin::makeHit(QStringLiteral("film.mkv"), 700 * MB, both, Net::Overlay, QStringLiteral("u3"));
        gn->respondDelayMs = 50;
        ov->respondDelayMs = 150;
        NetworkManager net(&settings, {ed, gn, ov});
        net.initPlugins(QString(), nullptr, nullptr);
        net.connectAll();
        SearchManager sm(&settings, &net, nullptr);
        sm.setFlushInterval(10);
        QSignalSpy changed(&sm, &SearchService::hitsChanged);
        QSignalSpy removed(&sm, &SearchService::hitsRemoved);
        QSignalSpy done(&sm, &SearchService::searchFinished);
        SearchQuery q;
        q.text = QStringLiteral("film");
        const quint32 id = sm.startSearch(q);
        QVERIFY(done.wait(3000));
        QCOMPARE(sm.hits(id).size(), 1);
        QCOMPARE(sm.hits(id).at(0).sources.size(), 3);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(removed.at(0).at(0).toUInt(), id);
        const QStringList gone = removed.at(0).at(1).toStringList();
        QCOMPARE(gone, QStringList{QStringLiteral("sha1:") + QString::fromLatin1(h('s', 20).toHex())});
        // the row had been shown before it was absorbed, and the survivor is re-emitted afterwards
        bool shownBefore = false;
        for (int i = 0; i < changed.count(); ++i)
            for (const SearchHit &hit : changed.at(i).at(1).value<QList<SearchHit>>())
                if (hit.key == gone.first()) shownBefore = true;
        QVERIFY(shownBefore);
        const QList<SearchHit> last = changed.last().at(1).value<QList<SearchHit>>();
        QCOMPARE(last.size(), 1);
        QCOMPARE(last.at(0).key, QStringLiteral("ed2k:") + QString::fromLatin1(h('e', 16).toHex()));
        QCOMPARE(last.at(0).sources.size(), 3);
    }

    void bundleFilesOnDemand()
    {
        SimpleSettings settings;
        auto *rats = new FakePlugin(Net::Rats, NetworkPlugin::CanSearch);
        SearchHit bundle = FakePlugin::makeHit(QStringLiteral("Album"), 100 * MB, FileHashes(), Net::BitTorrent);
        bundle.hashes.btih = h('b', 20);
        bundle.isBundle = true;
        rats->cannedHits << bundle;
        const QList<BundleFile> files = {BundleFile{QStringLiteral("Album/01.mp3"), 5 * MB, 0},
                                         BundleFile{QStringLiteral("Album/02.mp3"), 6 * MB, 1}};
        rats->bundleAnswers.insert(h('b', 20), files);
        NetworkManager net(&settings, {rats});
        net.initPlugins(QString(), nullptr, nullptr);
        net.connectAll();
        SearchManager sm(&settings, &net, nullptr);
        sm.setFlushInterval(10);
        QSignalSpy changed(&sm, &SearchService::hitsChanged);
        QSignalSpy done(&sm, &SearchService::searchFinished);
        SearchQuery q;
        q.text = QStringLiteral("album");
        const quint32 id = sm.startSearch(q);
        QVERIFY(done.wait(3000));
        const QString key = sm.hits(id).at(0).key;
        QVERIFY(sm.hits(id).at(0).files.isEmpty());
        const int before = changed.count();
        sm.requestBundleFiles(id, key);
        QCOMPARE(rats->bundleRequests.size(), 1);
        QCOMPARE(rats->bundleRequests.at(0).btih, h('b', 20));
        QTRY_COMPARE_WITH_TIMEOUT(changed.count(), before + 1, 2000);
        const SearchHit updated = changed.last().at(1).value<QList<SearchHit>>().at(0);
        QCOMPARE(updated.key, key);
        QCOMPARE(updated.files.size(), 2);
        QCOMPARE(sm.hits(id).at(0).files.at(1).path, QStringLiteral("Album/02.mp3"));
        // cached: a second search gets the files without asking again
        const quint32 id2 = sm.startSearch(q);
        QVERIFY(done.wait(3000));
        QCOMPARE(sm.hits(id2).at(0).files.size(), 2);
        sm.requestBundleFiles(id2, key);
        QCOMPARE(rats->bundleRequests.size(), 1);
        sm.requestBundleFiles(id, QStringLiteral("no-such-key"));   // harmless
    }

    void typedSearchAsksForUnknownBundles()
    {
        // [ui-f] a Video search keeps a bundle named "Album" (type unknown), asks for its file list, and
        // drops the row when the list says audio; the cached list then classifies it at once
        SimpleSettings settings;
        auto *rats = new FakePlugin(Net::Rats, NetworkPlugin::CanSearch);
        SearchHit bundle = FakePlugin::makeHit(QStringLiteral("Album"), 100 * MB, FileHashes(), Net::BitTorrent);
        bundle.hashes.btih = h('c', 20);
        bundle.isBundle = true;
        SearchHit film = FakePlugin::makeHit(QStringLiteral("Film"), 700 * MB, FileHashes(), Net::BitTorrent);
        film.hashes.btih = h('d', 20);
        film.isBundle = true;
        rats->cannedHits << bundle << film;
        rats->bundleAnswers.insert(h('c', 20), {BundleFile{QStringLiteral("Album/01.flac"), 30 * MB, 0},
                                                BundleFile{QStringLiteral("Album/cover.jpg"), 1 * MB, 1}});
        rats->bundleAnswers.insert(h('d', 20), {BundleFile{QStringLiteral("Film/film.mkv"), 700 * MB, 0}});
        NetworkManager net(&settings, {rats});
        net.initPlugins(QString(), nullptr, nullptr);
        net.connectAll();
        SearchManager sm(&settings, &net, nullptr);
        sm.setFlushInterval(10);
        QSignalSpy removed(&sm, &SearchService::hitsRemoved);
        QSignalSpy done(&sm, &SearchService::searchFinished);
        SearchQuery q;
        q.text = QStringLiteral("x");
        FileTypeFilter::fromCode(QStringLiteral("video")).applyTo(q);
        const quint32 id = sm.startSearch(q);
        QVERIFY(done.wait(3000));
        QTRY_COMPARE_WITH_TIMEOUT(rats->bundleRequests.size(), 2, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(sm.hits(id).size(), 1, 2000);
        // [merge-1] a list with one file makes the torrent that one file
        QCOMPARE(sm.hits(id).at(0).fileName, QStringLiteral("film.mkv"));
        QVERIFY(!sm.hits(id).at(0).isBundle);
        QCOMPARE(removed.count(), 1);
        for (const SearchHit &x : sm.hits(id)) QVERIFY(hitMatchesFileType(q, x));
        // an untyped search asks for nothing; a typed one with the lists cached asks for nothing either
        const quint32 id2 = sm.startSearch(q);
        QVERIFY(done.wait(3000));
        QCOMPARE(sm.hits(id2).size(), 1);
        SearchQuery any;
        any.text = QStringLiteral("x");
        sm.startSearch(any);
        QVERIFY(done.wait(3000));
        QTest::qWait(50);
        QCOMPARE(rats->bundleRequests.size(), 2);
    }

    void managerFanOutAndMerge()
    {
        SimpleSettings settings;
        auto *ed = new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch);
        auto *gn = new FakePlugin(Net::Gnutella, NetworkPlugin::CanSearch);
        auto *bt = new FakePlugin(Net::BitTorrent, NetworkPlugin::CanServe);   // cannot search
        auto *off = new FakePlugin(Net::Kad, NetworkPlugin::CanSearch);        // disabled
        settings.setValue(keys::kad::Enabled, false);
        FileHashes both = ed2k('a');
        both.sha1 = h('s', 20);
        ed->cannedHits << FakePlugin::makeHit(QStringLiteral("Velorine - Beside The Mill Pond.mp3"), 4388571, ed2k('a'), Net::ED2K, QStringLiteral("quill"))
                       << FakePlugin::makeHit(QStringLiteral("Velorine - Marigold.mp3"), 5195650, ed2k('j'), Net::ED2K, QStringLiteral("quill"));
        gn->cannedHits << FakePlugin::makeHit(QStringLiteral("velorine - beside the mill pond.mp3"), 4388571, both, Net::Gnutella, QStringLiteral("1.2.3.4:6346"))
                       << FakePlugin::makeHit(QStringLiteral("unrelated.exe"), 1000, FileHashes(), Net::Gnutella, QStringLiteral("x")); // blocked ext
        gn->respondDelayMs = 30;
        NetworkManager net(&settings, {ed, gn, bt, off});
        net.initPlugins(QString(), nullptr, nullptr);
        net.connectAll();
        SearchManager sm(&settings, &net, nullptr);
        sm.setFlushInterval(10);
        QSignalSpy hitsSpy(&sm, &SearchService::hitsChanged);
        QSignalSpy doneSpy(&sm, &SearchService::searchFinished);
        QSignalSpy progSpy(&sm, &SearchService::searchProgress);

        SearchQuery q;
        q.text = QStringLiteral("velorine");
        const quint32 id = sm.startSearch(q);
        QVERIFY(id > 0);
        QVERIFY(sm.isRunning(id));
        QTRY_COMPARE_WITH_TIMEOUT(doneSpy.count(), 1, 3000);
        QCOMPARE(doneSpy.at(0).at(0).toUInt(), id);
        QVERIFY(!sm.isRunning(id));
        QCOMPARE(ed->searches.size(), 1);
        QCOMPARE(gn->searches.size(), 1);
        QCOMPARE(off->searches.size(), 0);       // disabled network is not asked
        QCOMPARE(ed->searches.at(0).id, id);
        const QList<SearchHit> hits = sm.hits(id);
        QCOMPARE(hits.size(), 2);                // merged "Beside The Mill Pond" + Marigold (exe filtered)
        const SearchHit &side = hits.at(0);
        QCOMPARE(side.sources.size(), 2);
        QCOMPARE(side.sources.at(1).net, Net::Gnutella);
        QCOMPARE(side.hashes.sha1, both.sha1);
        QCOMPARE(side.type, MediaType::Audio);
        QVERIFY(hitsSpy.count() >= 2);           // eD2k batch, then the Gnutella update
        bool sawDone = false;
        for (const auto &args : progSpy)
            if (args.at(2).toString() == QStringLiteral("Gnutella: 1 result (done)")) sawDone = true;
        QVERIFY(sawDone);
        QCOMPARE(sm.history(), QStringList{QStringLiteral("velorine")});
        sm.clearHistory();
        QVERIFY(sm.history().isEmpty());
        sm.closeSearch(id);
        QVERIFY(sm.hits(id).isEmpty());
    }

    void timeoutAndLateHits()
    {
        SimpleSettings settings;
        settings.setValue(keys::search::TimeoutSec, 10);  // minimum allowed
        auto *slow = new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch);
        slow->autoRespond = false;                       // never says done
        NetworkManager net(&settings, {slow});
        net.initPlugins(QString(), nullptr, nullptr);
        net.connectAll();
        SearchManager sm(&settings, &net, nullptr);
        sm.setFlushInterval(10);
        QSignalSpy doneSpy(&sm, &SearchService::searchFinished);
        QSignalSpy hitsSpy(&sm, &SearchService::hitsChanged);
        SearchQuery q;
        q.text = QStringLiteral("slow");
        const quint32 id = sm.startSearch(q);
        QTRY_COMPARE_WITH_TIMEOUT(slow->searches.size(), 1, 1000);
        QVERIFY(doneSpy.wait(12000));                    // timeout finishes the search
        QVERIFY(slow->cancelledSearches.isEmpty());      // not cancelled: late hits may still come
        slow->emitHits(id, {FakePlugin::makeHit(QStringLiteral("late.mp3"), 3 * MB, ed2k('l'), Net::ED2K, QStringLiteral("u"))});
        QTRY_COMPARE_WITH_TIMEOUT(hitsSpy.count(), 1, 1000);
        QCOMPARE(sm.hits(id).size(), 1);
        // after stopSearch, hits are ignored and the plugin is told to cancel
        sm.stopSearch(id);
        QCOMPARE(slow->cancelledSearches, QList<quint32>{id});
        slow->emitHits(id, {FakePlugin::makeHit(QStringLiteral("later.mp3"), 3 * MB, ed2k('m'), Net::ED2K, QStringLiteral("u"))});
        QTest::qWait(50);
        QCOMPARE(sm.hits(id).size(), 1);
    }

    void filtersAndLimits()
    {
        SimpleSettings settings;
        settings.setValue(keys::search::AdultFilter, true);
        auto *p = new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch);
        for (int i = 0; i < 80; ++i)
            p->cannedHits << FakePlugin::makeHit(QStringLiteral("file %1.mp3").arg(i), 2 * MB + i, ed2k(char(i)), Net::ED2K, QStringLiteral("u"));
        p->cannedHits << FakePlugin::makeHit(QStringLiteral("hot xxx clip.mp3"), 3 * MB, ed2k('X'), Net::ED2K, QStringLiteral("u"))
                      << FakePlugin::makeHit(QStringLiteral("song.mp3.exe"), 3 * MB, FileHashes(), Net::ED2K, QStringLiteral("u"));
        NetworkManager net(&settings, {p});
        net.initPlugins(QString(), nullptr, nullptr);
        net.connectAll();
        SearchManager sm(&settings, &net, nullptr);
        QSignalSpy doneSpy(&sm, &SearchService::searchFinished);
        SearchQuery q;
        q.text = QStringLiteral("file");
        q.maxResults = 50;                       // capped (setting minimum is 50 too)
        const quint32 id = sm.startSearch(q);
        QVERIFY(doneSpy.wait(3000));
        QCOMPARE(sm.hits(id).size(), 50);

        // query filters
        SearchQuery f;
        f.text = QStringLiteral("x");
        f.type = MediaType::Video;
        QVERIFY(!sm.acceptHit(f, FakePlugin::makeHit(QStringLiteral("a.mp3"), 5 * MB)));
        QVERIFY(sm.acceptHit(f, FakePlugin::makeHit(QStringLiteral("a.avi"), 5 * MB)));
        SearchQuery sz;
        sz.minSize = 10 * MB;
        QVERIFY(!sm.acceptHit(sz, FakePlugin::makeHit(QStringLiteral("a.avi"), 5 * MB)));
        SearchQuery br;
        br.minBitrate = 192;
        SearchHit lowBr = FakePlugin::makeHit(QStringLiteral("a.mp3"), 5 * MB);
        lowBr.bitrateKbps = 128;
        QVERIFY(!sm.acceptHit(br, lowBr));
        SearchQuery ext;
        ext.extension = QStringLiteral(".ogg");
        QVERIFY(!sm.acceptHit(ext, FakePlugin::makeHit(QStringLiteral("a.mp3"), 5 * MB)));
        QVERIFY(sm.acceptHit(ext, FakePlugin::makeHit(QStringLiteral("a.OGG"), 5 * MB)));
        // [ui-f] torrents named without an extension are not hidden by a type; bundles by their files
        FileHashes bt;
        bt.btih = QByteArray(20, 'b');
        SearchHit named = FakePlugin::makeHit(QStringLiteral("Big Buck Bunny 1080p"), 5 * MB, bt, Net::BitTorrent);
        QVERIFY(sm.acceptHit(f, named));
        SearchHit plain = FakePlugin::makeHit(QStringLiteral("Big Buck Bunny 1080p"), 5 * MB, FileHashes(), Net::ED2K);
        QVERIFY(!sm.acceptHit(f, plain));                    // a plain file needs its extension
        SearchHit folder = FakePlugin::makeHit(QStringLiteral("Big Buck Bunny (2008)"), 5 * MB, bt, Net::BitTorrent);
        folder.isBundle = true;
        folder.files = {BundleFile{QStringLiteral("bbb.mkv"), 5 * MB, 0}, BundleFile{QStringLiteral("bbb.nfo"), 100, 1}};
        QVERIFY(sm.acceptHit(f, folder));
        SearchQuery audioQ;
        FileTypeFilter::fromCode(QStringLiteral("audio")).applyTo(audioQ);
        QVERIFY(!sm.acceptHit(audioQ, folder));              // was kept: bundles passed every type
        SearchQuery lossless;
        FileTypeFilter::fromCode(QStringLiteral("lossless")).applyTo(lossless);
        QVERIFY(sm.acceptHit(lossless, FakePlugin::makeHit(QStringLiteral("a.FLAC"), 5 * MB)));
        QVERIFY(!sm.acceptHit(lossless, FakePlugin::makeHit(QStringLiteral("a.mp3"), 5 * MB)));
        SearchQuery best;
        FileTypeFilter::fromCode(QStringLiteral("audio:max:128")).applyTo(best);
        QVERIFY(sm.acceptHit(best, lowBr));
        SearchHit highBr = lowBr;
        highBr.bitrateKbps = 320;
        QVERIFY(!sm.acceptHit(best, highBr));
        QVERIFY(!sm.acceptHit(SearchQuery(), FakePlugin::makeHit(QStringLiteral("hot xxx clip.mp3"), 5 * MB)));
        QVERIFY(!sm.acceptHit(SearchQuery(), FakePlugin::makeHit(QStringLiteral("song.mp3.exe"), 5 * MB)));
    }

    void noNetworks()
    {
        SimpleSettings settings;
        SearchManager sm(&settings, nullptr, nullptr);
        QSignalSpy doneSpy(&sm, &SearchService::searchFinished);
        QSignalSpy progSpy(&sm, &SearchService::searchProgress);
        SearchQuery q;
        q.text = QStringLiteral("anything");
        const quint32 id = sm.startSearch(q);
        QVERIFY(doneSpy.wait(1000));
        QCOMPARE(doneSpy.at(0).at(0).toUInt(), id);
        QCOMPARE(progSpy.count(), 1);
        QCOMPARE(sm.history(), QStringList{QStringLiteral("anything")});
    }

    void localFlags()
    {
        SimpleSettings settings;
        auto *p = new FakePlugin(Net::ED2K, NetworkPlugin::CanSearch);
        p->cannedHits << FakePlugin::makeHit(QStringLiteral("mine.mp3"), 5 * MB, ed2k('m'), Net::ED2K, QStringLiteral("u"));
        NetworkManager net(&settings, {p});
        net.initPlugins(QString(), nullptr, nullptr);
        net.connectAll();
        SearchManager sm(&settings, &net, nullptr);
        sm.setLocalFlagger([](SearchHit &h) {
            if (h.hashes.ed2k == QByteArray(16, 'm')) {
                h.alreadyInLibrary = true;
                h.matchesIncompleteLocal = true;
            }
        });
        QSignalSpy hitsSpy(&sm, &SearchService::hitsChanged);
        SearchQuery q;
        q.text = QStringLiteral("mine");
        const quint32 id = sm.startSearch(q);
        QTRY_VERIFY_WITH_TIMEOUT(hitsSpy.count() >= 1, 2000);
        const QList<SearchHit> emitted = hitsSpy.at(0).at(1).value<QList<SearchHit>>();
        QCOMPARE(emitted.size(), 1);
        QVERIFY(emitted.at(0).alreadyInLibrary);
        QVERIFY(emitted.at(0).matchesIncompleteLocal);
        QVERIFY(sm.hits(id).at(0).alreadyInLibrary);
    }
};

QTEST_GUILESS_MAIN(TestSearch)
#include "tst_search.moc"
