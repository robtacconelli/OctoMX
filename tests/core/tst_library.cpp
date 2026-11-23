// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest/QtTest>
#include <QtTest/QSignalSpy>

#include "core/dao.h"
#include "core/database.h"
#include "core/library.h"
#include "core/media_info.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "mxhash/multi_hasher.h"

#include <QtCore/QRandomGenerator>
#include <QtCore/QTemporaryDir>

using namespace mx;

namespace {

QByteArray mp3Frames(int frames, bool xing = false)
{
    // MPEG-1 layer III, 128 kbps, 44100 Hz, stereo: FF FB 90 00, frame length 417 bytes
    QByteArray frame(417, '\0');
    frame[0] = char(0xFF);
    frame[1] = char(0xFB);
    frame[2] = char(0x90);
    frame[3] = char(0x00);
    QByteArray out;
    out.reserve(frames * 417);
    for (int i = 0; i < frames; ++i) {
        QByteArray f = frame;
        if (xing && i == 0) {
            f.replace(4 + 32, 4, "Xing");
            const quint32 flags = 3, nframes = 2000, bytes = 2000 * 300;
            for (int k = 0; k < 4; ++k) {
                f[4 + 32 + 4 + k] = char((flags >> (24 - 8 * k)) & 0xFF);
                f[4 + 32 + 8 + k] = char((nframes >> (24 - 8 * k)) & 0xFF);
                f[4 + 32 + 12 + k] = char((bytes >> (24 - 8 * k)) & 0xFF);
            }
        }
        out += f;
    }
    return out;
}

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write(data), qint64(data.size()));
}

QByteArray randomBytes(qint64 n, quint32 seed)
{
    QByteArray b(n, Qt::Uninitialized);
    QRandomGenerator g(seed);
    g.fillRange(reinterpret_cast<quint32 *>(b.data()), int(n / 4));
    return b;
}


// One Ogg page (no CRC: the parser does not check it) carrying `body` as a single packet.
QByteArray oggPage(quint32 serial, quint64 granule, const QByteArray &body, bool bos = false)
{
    QByteArray p("OggS", 4);
    p += char(0);
    p += char(bos ? 2 : 0);
    for (int i = 0; i < 8; ++i) p += char((granule >> (8 * i)) & 0xFF);
    for (int i = 0; i < 4; ++i) p += char((serial >> (8 * i)) & 0xFF);
    p += QByteArray(8, '\0');   // sequence number, CRC
    p += char(1);
    p += char(qMin(255, int(body.size())));
    return p + body.left(255);
}

QByteArray vorbisIdHeader(quint32 rate, quint32 nominal)
{
    QByteArray b("\x01vorbis", 7);
    b += QByteArray(4, '\0');   // version
    b += char(2);                // channels
    for (int i = 0; i < 4; ++i) b += char((rate >> (8 * i)) & 0xFF);
    b += QByteArray(4, '\0');   // bitrate maximum
    for (int i = 0; i < 4; ++i) b += char((nominal >> (8 * i)) & 0xFF);
    b += QByteArray(4, '\0');   // bitrate minimum
    b += char(0xB8);
    b += char(1);
    return b;
}

} // namespace

class TestLibrary : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_share, m_incoming, m_outside;
    SimpleSettings m_settings;
    Database m_db;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_share = m_dir.filePath(QStringLiteral("share"));
        m_incoming = m_dir.filePath(QStringLiteral("incoming"));
        m_outside = m_dir.filePath(QStringLiteral("outside"));
        writeFile(m_share + QStringLiteral("/a.mp3"), mp3Frames(1000));
        writeFile(m_share + QStringLiteral("/b.bin"), randomBytes(10000000, 7));
        writeFile(m_share + QStringLiteral("/sub/c.txt"), "hello world");
        writeFile(m_share + QStringLiteral("/skip.tmp"), "temporary");
        writeFile(m_share + QStringLiteral("/.hidden"), "hidden");
        writeFile(m_outside + QStringLiteral("/x.dat"), "outside the shares");
        m_settings.setValue(keys::transfers::IncomingDir, m_incoming);
        m_settings.setValue(keys::transfers::ShareCompleted, true);
        m_settings.setValue(keys::library::WatchFolders, true);
        QVERIFY(m_db.open(m_dir.filePath(QStringLiteral("octomx.db"))));
    }

    void mp3Parsing()
    {
        MediaInfo cbr = parseMp3(mp3Frames(1000), 417000);
        QCOMPARE(cbr.sampleRateHz, 44100);
        QCOMPARE(cbr.bitrateKbps, 128);
        QCOMPARE(cbr.durationSec, 26);
        // ID3v2 tag in front
        QByteArray tagged("ID3\x03\x00\x00\x00\x00\x00\x0a", 10);
        tagged += QByteArray(10, 'x') + mp3Frames(100);
        MediaInfo t = parseMp3(tagged, tagged.size());
        QCOMPARE(t.sampleRateHz, 44100);
        QCOMPARE(t.bitrateKbps, 128);
        // Xing VBR header: 2000 frames * 1152 / 44100 = 52.2 s; 600000 bytes → 92 kbps
        MediaInfo v = parseMp3(mp3Frames(50, true), 50 * 417);
        QCOMPARE(v.durationSec, 52);
        QCOMPARE(v.bitrateKbps, 92);
        // garbage
        QVERIFY(!parseMp3(randomBytes(4096, 3).replace(char(0xFF), char(0x00)), 4096).isValid());
    }

    // [core-5] Ogg: the length comes from the AUDIO stream's last granule (a video stream's
    // granules are keyframe numbers); an Ogg with a video stream gets no bitrate (the file size
    // is mostly video, so size per second is not the audio bitrate).
    void oggLengthAndBitrate()
    {
        const quint64 tenMin = 600ull * 44100;
        QByteArray audio = oggPage(7, 0, vorbisIdHeader(44100, 160000), true);
        QByteArray audioTail = oggPage(7, tenMin / 2, QByteArray(200, 'a')) + oggPage(7, tenMin, QByteArray(200, 'a'));
        MediaInfo a = parseOggForTest(audio, audioTail, 12000000);
        QCOMPARE(a.durationSec, 600);
        QCOMPARE(a.sampleRateHz, 44100);
        QCOMPARE(a.bitrateKbps, 160);   // 12 MB in 600 s
        // Theora + Vorbis: the last page belongs to the video stream with a huge granule
        QByteArray theoraBos = QByteArray("\x80theora", 7) + QByteArray(40, '\x01');
        QByteArray av = oggPage(3, 0, theoraBos, true) + oggPage(7, 0, vorbisIdHeader(44100, 128000), true);
        const quint64 theoraGranule = (quint64(14399) << 22) | 5;   // keyframe number in the high bits
        QByteArray avTail = oggPage(7, tenMin, QByteArray(200, 'a')) + oggPage(3, theoraGranule, QByteArray(200, 'v'));
        MediaInfo v = parseOggForTest(av, avTail, 1500000000);
        QCOMPARE(v.durationSec, 600);
        QCOMPARE(v.bitrateKbps, -1);
        // only a video stream's granule at the end and no audio page in the tail: no length at all
        MediaInfo n = parseOggForTest(av, oggPage(3, theoraGranule, QByteArray(200, 'v')), 1500000000);
        QCOMPARE(n.durationSec, -1);
        QCOMPARE(n.bitrateKbps, -1);
    }

    // [core-5] A cached length / bitrate of a non-audio file (written by an older version) is not
    // shown: the scanner only reads audio files, a video's bitrate cell stays empty.
    void staleVideoMediaDropped()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString share = dir.filePath(QStringLiteral("share"));
        writeFile(share + QStringLiteral("/Big_Buck_Bunny_1080p.mkv"), randomBytes(300000, 11));
        writeFile(share + QStringLiteral("/song.mp3"), mp3Frames(1000));
        Database db;
        QVERIFY(db.open(dir.filePath(QStringLiteral("octomx.db"))));
        SimpleSettings settings;
        settings.setValue(keys::library::WatchFolders, false);
        {
            Library lib(&settings, &db);
            lib.addFolder(share, true);
            lib.start();
            QVERIFY(lib.waitIdle(60000));
            QCOMPARE(lib.fileCount(), 2);
            lib.stop();
        }
        {
            LibraryDao dao(&db);
            const QString path = QFileInfo(share + QStringLiteral("/Big_Buck_Bunny_1080p.mkv")).absoluteFilePath();
            LibraryRecord rec;
            bool found = false;
            for (const LibraryRecord &r : dao.all())
                if (r.path.endsWith(QLatin1String("Big_Buck_Bunny_1080p.mkv"))) { rec = r; found = true; }
            QVERIFY2(found, qPrintable(path));
            rec.bitrateKbps = 14;
            rec.durationSec = 1714286;
            dao.store(rec);
            db.flush();
        }
        Library lib(&settings, &db);
        lib.start();
        QVERIFY(lib.waitIdle(60000));
        int seen = 0;
        for (const SharedFile &f : lib.files()) {
            if (f.name.endsWith(QLatin1String(".mkv"))) {
                ++seen;
                QCOMPARE(f.bitrateKbps, -1);
                QCOMPARE(f.durationSec, -1);
            } else {
                ++seen;
                QCOMPARE(f.bitrateKbps, 128);
                QCOMPARE(f.durationSec, 26);
            }
        }
        QCOMPARE(seen, 2);
        lib.stop();
    }

    void scanAndHash()
    {
        Library lib(&m_settings, &m_db);
        QSignalSpy pushed(&lib, &Library::sharedFilesUpdated);
        QSignalSpy hashed(&lib, &Library::fileHashed);
        lib.addFolder(m_share, true);
        lib.start();
        QVERIFY(lib.waitIdle(60000));
        QCOMPARE(hashed.count(), 3);
        QCOMPARE(lib.fileCount(), 3);  // a.mp3, b.bin, sub/c.txt (skip.tmp excluded, .hidden hidden)
        QVERIFY(lib.isScanning() == false);
        QVERIFY(lib.scanStatus().startsWith(QStringLiteral("Sharing 3 files")));

        const QList<SharedFile> files = lib.files();
        QCOMPARE(files.size(), 3);
        SharedFile b;
        for (const SharedFile &f : files)
            if (f.name == QLatin1String("b.bin")) b = f;
        QVERIFY(b.hashed);
        hash::MultiHasher mh;
        QVERIFY(hash::MultiHasher::hashFile(m_share + QStringLiteral("/b.bin"), mh));
        QCOMPARE(b.hashes.ed2k, mh.hashes().ed2k);
        QCOMPARE(b.hashes.sha1, mh.hashes().sha1);
        QCOMPARE(b.hashes.tth, mh.hashes().tth);
        QCOMPARE(b.hashes.aich, mh.hashes().aich);
        QCOMPARE(b.hashes.sha256, mh.hashes().sha256);
        QCOMPARE(lib.ed2kPartHashes(b.hashes).size(), 2);
        QCOMPARE(lib.ed2kPartHashes(b.hashes), mh.ed2kPartHashes());

        for (const SharedFile &f : files) {
            if (f.name == QLatin1String("a.mp3")) {
                QCOMPARE(f.type, MediaType::Audio);
                QCOMPARE(f.durationSec, 26);
                QCOMPARE(f.bitrateKbps, 128);
                QCOMPARE(f.sampleRateHz, 44100);
            }
        }
        QCOMPARE(lib.files(m_share + QStringLiteral("/sub")).size(), 1);
        const QList<SharedFolder> folders = lib.folders();
        QCOMPARE(folders.size(), 2);   // share + implicit incoming dir
        QCOMPARE(folders.at(0).fileCount, 3);

        // LibraryAccess: find by a single hash kind, positional reads
        FileHashes q;
        q.sha1 = b.hashes.sha1;
        SharedFile found;
        QVERIFY(lib.findByHash(q, &found));
        QCOMPARE(found.name, QStringLiteral("b.bin"));
        QFile raw(m_share + QStringLiteral("/b.bin"));
        QVERIFY(raw.open(QIODevice::ReadOnly));
        raw.seek(9727990);
        QCOMPARE(lib.read(q, 9727990, 100), raw.read(100));
        QCOMPARE(lib.read(q, 9999990, 100).size(), 10);   // short read at EOF
        QVERIFY(lib.read(q, 20000000, 10).isEmpty());
        FileHashes wrong;
        wrong.sha1 = QByteArray(20, 'x');
        QVERIFY(!lib.findByHash(wrong, nullptr));
        // conflicting hash (right sha1, wrong ed2k) is not served
        FileHashes conflict = q;
        conflict.ed2k = QByteArray(16, 'y');
        QVERIFY(!lib.findByHash(conflict, nullptr));

        QTRY_VERIFY_WITH_TIMEOUT(pushed.count() >= 1, 5000);
        QCOMPARE(pushed.last().at(0).value<QList<SharedFile>>().size(), 3);
        lib.stop();
    }

    void cacheAvoidsRehash()
    {
        Library lib(&m_settings, &m_db);
        QSignalSpy hashed(&lib, &Library::fileHashed);
        lib.start();
        // the cached list is available immediately, before the scan finishes
        QCOMPARE(lib.sharedFiles().size(), 3);
        QVERIFY(lib.waitIdle(30000));
        QCOMPARE(hashed.count(), 0);
        QCOMPARE(lib.fileCount(), 3);

        // a changed file is re-hashed, the others are not
        QTest::qWait(20);
        {
            QFile f(m_share + QStringLiteral("/sub/c.txt"));
            QVERIFY(f.open(QIODevice::Append));
            f.write(" and more");
        }
        lib.rescan();
        QVERIFY(lib.waitIdle(30000));
        QCOMPARE(hashed.count(), 1);
        QCOMPARE(hashed.at(0).at(0).value<SharedFile>().name, QStringLiteral("c.txt"));
        lib.stop();
    }

    void unshareAndIncoming()
    {
        {
            Library lib(&m_settings, &m_db);
            lib.start();
            QVERIFY(lib.waitIdle(30000));
            lib.unshareFile(m_share + QStringLiteral("/a.mp3"));
            QCOMPARE(lib.fileCount(), 2);
            lib.rescan();
            QVERIFY(lib.waitIdle(30000));
            QCOMPARE(lib.fileCount(), 2);
            lib.stop();
        }
        {
            Library lib(&m_settings, &m_db);   // exclusion persisted
            lib.start();
            QVERIFY(lib.waitIdle(30000));
            QCOMPARE(lib.fileCount(), 2);
            QCOMPARE(lib.excludedPaths().size(), 1);
            lib.reshareFile(m_share + QStringLiteral("/a.mp3"));
            QVERIFY(lib.waitIdle(30000));
            QCOMPARE(lib.fileCount(), 3);

            // completed downloads in the incoming dir are shared while transfers/shareCompleted is on
            writeFile(m_incoming + QStringLiteral("/done.txt"), "a completed download");
            lib.rescan();
            QVERIFY(lib.waitIdle(30000));
            QCOMPARE(lib.fileCount(), 4);
            m_settings.setValue(keys::transfers::ShareCompleted, false);
            QVERIFY(lib.waitIdle(30000));
            QCOMPARE(lib.fileCount(), 3);
            m_settings.setValue(keys::transfers::ShareCompleted, true);
            QVERIFY(lib.waitIdle(30000));
            QCOMPARE(lib.fileCount(), 4);
            lib.stop();
        }
    }

    void requestHashOutsideShares()
    {
        Library lib(&m_settings, &m_db);
        lib.start();
        QVERIFY(lib.waitIdle(30000));
        QSignalSpy hashed(&lib, &Library::fileHashed);
        const int before = lib.fileCount();
        lib.requestHash(m_outside + QStringLiteral("/x.dat"));
        QTRY_COMPARE_WITH_TIMEOUT(hashed.count(), 1, 10000);
        const SharedFile f = hashed.at(0).at(0).value<SharedFile>();
        QVERIFY(f.hashed);
        QVERIFY(!f.hashes.ed2k.isEmpty());
        QCOMPARE(lib.fileCount(), before);             // hashed, but not shared
        QVERIFY(!lib.findByHash(f.hashes, nullptr));
        // a second request is answered from memory
        lib.requestHash(m_outside + QStringLiteral("/x.dat"));
        QTRY_COMPARE_WITH_TIMEOUT(hashed.count(), 2, 5000);
        lib.stop();
    }

    void watcherPicksUpNewFiles()
    {
        Library lib(&m_settings, &m_db);
        lib.start();
        QVERIFY(lib.waitIdle(30000));
        const int before = lib.fileCount();
        writeFile(m_share + QStringLiteral("/sub/new.txt"), "dropped in while running");
        QTRY_COMPARE_WITH_TIMEOUT(lib.fileCount(), before + 1, 15000);
        QVERIFY(lib.waitIdle(30000));
        lib.stop();
    }

    void removeFolder()
    {
        Library lib(&m_settings, &m_db);
        lib.start();
        QVERIFY(lib.waitIdle(30000));
        lib.removeFolder(m_share);
        QVERIFY(lib.waitIdle(30000));
        QCOMPARE(lib.fileCount(), 1);   // only the incoming dir's file is left
        QCOMPARE(lib.folders().size(), 1);
        lib.stop();
    }
};

QTEST_GUILESS_MAIN(TestLibrary)
#include "tst_library.moc"
