// SPDX-License-Identifier: GPL-3.0-or-later
// [upd-1] The signed self-update below the UI, against a local fake server only (never GitHub):
//   * Ed25519: good signature, wrong key, unknown key id, altered SHA256SUMS, damaged signature file
//   * manifest: format, unsafe and reserved paths, verifyTree
//   * zip reader: stored + deflated entries, CRC, unsafe names; tar.xz (Linux)
//   * staged tree check (top folder, manifest, program file), layout detection, unwritable folder
//   * download: good, wrong key, altered sums, altered archive, missing signature, redirect inside the
//     server and to a foreign host, https only, oversize, resume with Range, a cached archive
//   * the journalled swap: a crash after every single file operation, then the next start's
//     recovery (and a crash during the recovery), roll back when staged files are gone, an error in
//     the middle (undone at once), a foreign file in the way; portable.txt, profile/, downloads/ and
//     files the package never had are untouched in every case
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

#include "upd1_support.h"
#include "mx/simple_settings.h"
#include "mx/settings_keys.h"
#include "mx/update_download.h"
#include "mx/update_install.h"

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

Q_DECLARE_METATYPE(mx::upd::UpdateDownloader::Result)

using namespace mx;
using namespace mx::upd;

namespace {

const QString kOld = QStringLiteral("4.3.0");
const QString kNew = QStringLiteral("4.4.0");

// the user's things in the program folder: never moved, never changed
const QMap<QString, QByteArray> &userFiles()
{
    static const QMap<QString, QByteArray> f = {
        {QStringLiteral("portable.txt"), "portable\n"},
        {QStringLiteral("profile/settings.json"), "{\"misc/x\": 1}"},
        {QStringLiteral("profile/octomx.db"), "SQLite format 3 (fake)"},
        {QStringLiteral("downloads/song.ogg"), "music"},
        {QStringLiteral("incoming/part.bin"), "partial"},
        {QStringLiteral("my notes.txt"), "not ours"},
        {QStringLiteral("lib/user-plugin.so"), "a file the package never had, in one of our folders"},
    };
    return f;
}

struct Fixture {
    QString prog, stageTop;
    Manifest oldM, newM;
    QMap<QString, QByteArray> oldFiles, newFiles;
};

// an installed old package with the user's files, and the new package staged in .new-<version>
Fixture makeFixture(const QString &root)
{
    Fixture fx;
    QDir(root).removeRecursively();
    fx.prog = QDir(root).filePath(QStringLiteral("OctoMX"));
    // the old version has a file the new one drops; the new one adds a file and a folder
    fx.oldFiles = upd1::packageFiles(kOld, {{QStringLiteral("old-only.txt"), "dropped in 4.4"},
                                           {QStringLiteral("plugins/old/gone.bin"), "dropped folder"}});
    fx.newFiles = upd1::packageFiles(kNew, {{QStringLiteral("new-only.txt"), "new in 4.4"},
                                           {QStringLiteral("translations/octomx_de.qm"), "neu"}});
    fx.oldM = upd1::writePackage(fx.prog, kOld, fx.oldFiles);
    for (auto it = userFiles().cbegin(); it != userFiles().cend(); ++it)
        upd1::writeFile(QDir(fx.prog).filePath(it.key()), it.value());
    fx.stageTop = QDir(stageDirFor(fx.prog, kNew)).filePath(packageDirName(kNew));
    fx.newM = upd1::writePackage(fx.stageTop, kNew, fx.newFiles);
    return fx;
}

SwapPlan planOf(const Fixture &fx)
{
    return SwapPlan{fx.prog, fx.stageTop, kOld, kNew, fx.oldM, fx.newM};
}

// the program folder holds exactly one version (its manifest, every file), the user's files, and
// nothing of the update machinery
QString wholeAs(const Fixture &fx, bool isNew)
{
    const Manifest &m = isNew ? fx.newM : fx.oldM;
    const QMap<QString, QByteArray> &files = isNew ? fx.newFiles : fx.oldFiles;
    const QMap<QString, QByteArray> &other = isNew ? fx.oldFiles : fx.newFiles;
    QString why;
    if (!verifyTree(fx.prog, m, &why))
        return QStringLiteral("tree: ") + why;
    Manifest installed;
    if (!readManifest(fx.prog, &installed, &why) || installed.version != m.version)
        return QStringLiteral("installed manifest: %1 %2").arg(why, installed.version);
    for (auto it = other.cbegin(); it != other.cend(); ++it)
        if (!files.contains(it.key()) && QFileInfo::exists(QDir(fx.prog).filePath(it.key())))
            return QStringLiteral("a file of the other version is left: ") + it.key();
    for (auto it = userFiles().cbegin(); it != userFiles().cend(); ++it)
        if (upd1::readFile(QDir(fx.prog).filePath(it.key())) != it.value())
            return QStringLiteral("user file changed: ") + it.key();
    return QString();
}

bool machineryGone(const QString &prog)
{
    const QStringList left = QDir(prog).entryList({QStringLiteral(".old-*"), QStringLiteral(".new-*"), QStringLiteral(".update-*")},
                                                  QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    if (!left.isEmpty())
        qWarning("left: %s", qPrintable(left.join(QLatin1Char(' '))));
    return left.isEmpty();
}

} // namespace

class TstUpdateInstall : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    upd1::Key m_key;
    upd1::Server *m_srv = nullptr;

    UpdateDownloader::Result download(const ReleaseInfo &r, const QString &dir, qint64 maxBytes = -1)
    {
        SimpleSettings s;
        s.setValue(QString(keys::updates::Url), m_srv->url(QStringLiteral("/latest")));
        UpdateDownloader d(&s, dir, QStringLiteral("OctoMX/test"));
        d.setTimeoutMs(5000);
        if (maxBytes > 0)
            d.setMaxArchiveBytes(maxBytes);
        QSignalSpy spy(&d, &UpdateDownloader::finished);
        d.start(r);
        if (!spy.wait(20000))
            return {};
        return spy.first().first().value<UpdateDownloader::Result>();
    }

    // a release on the server: archive + SHA256SUMS + SHA256SUMS.sig under /<prefix>/
    ReleaseInfo publish(const QString &prefix, const QByteArray &archive, bool sign = true, const upd1::Key *key = nullptr,
                        bool alterSums = false, const QByteArray &served = QByteArray())
    {
        const QString name = archiveName(kNew);
        QByteArray sums = upd1::sha256Hex(archive) + "  " + name.toUtf8() + "\n";
        const QByteArray sig = (key ? *key : m_key).sign(sums);
        if (alterSums)
            sums = upd1::sha256Hex(served) + "  " + name.toUtf8() + "\n";
        m_srv->routes.insert(prefix + QStringLiteral("/") + name, upd1::Route{200, served.isEmpty() ? archive : served, {}, true});
        m_srv->routes.insert(prefix + QStringLiteral("/SHA256SUMS"), upd1::Route{200, sums, {}, false});
        QList<QPair<QString, QString>> assets = {{name, m_srv->url(prefix + QStringLiteral("/") + name)},
                                                 {QStringLiteral("SHA256SUMS"), m_srv->url(prefix + QStringLiteral("/SHA256SUMS"))}};
        if (sign) {
            m_srv->routes.insert(prefix + QStringLiteral("/SHA256SUMS.sig"), upd1::Route{200, sig, {}, false});
            assets << qMakePair(QStringLiteral("SHA256SUMS.sig"), m_srv->url(prefix + QStringLiteral("/SHA256SUMS.sig")));
        }
        ReleaseInfo r;
        parseLatestRelease(upd1::releaseJson(QStringLiteral("v") + kNew, assets), &r);
        return r;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(testHooksCompiled());
        qRegisterMetaType<UpdateDownloader::Result>();
        m_key = upd1::Key::make(true);
        QCOMPARE(m_key.pub.size(), 32);
        m_srv = new upd1::Server;
        QVERIFY(m_srv->server.isListening());
    }

    void signatures()
    {
        const QByteArray sums = "0123  OctoMX-5.0-win64.zip\n";
        SigCheck c = verifySums(sums, m_key.sign(sums));
        QVERIFY2(c.ok, qPrintable(c.error));
        QCOMPARE(c.keyId, m_key.id);
        // altered list
        c = verifySums(sums + "x", m_key.sign(sums));
        QVERIFY(!c.ok);
        QVERIFY(c.error.contains(QStringLiteral("does not match")));
        // a key nobody trusts, under its own id and under ours
        const upd1::Key stranger = upd1::Key::make(false);
        c = verifySums(sums, stranger.sign(sums));
        QVERIFY(!c.ok);
        QVERIFY2(c.error.contains(QStringLiteral("does not know")), qPrintable(c.error));
        c = verifySums(sums, formatSignatureFile(m_key.id, testSign(stranger.seed, sums)));
        QVERIFY(!c.ok);
        // damaged / foreign files
        QVERIFY(!verifySums(sums, "hello").ok);
        QVERIFY(!verifySums(sums, QByteArray()).ok);
        QVERIFY(!verifySums(sums, "winmx-ng-signature 1\nkey x\ned25519 AAAA\n").ok);
        // the built-in release key: a 32-byte key with the documented id
        bool builtin = false;
        for (const PublicKey &k : trustedKeys())
            if (k.id == QLatin1String("6b4bb49f391b4521")) {
                builtin = true;
                QCOMPARE(k.raw.size(), 32);
                QCOMPARE(keyIdOf(k.raw), k.id);
            }
        QVERIFY(builtin);
        // RFC 8032, test 1 (empty message)
        QVERIFY(ed25519Verify(QByteArray::fromHex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"), QByteArray(),
                              QByteArray::fromHex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")));
        // the sums parser: sha256sum's two formats, junk skipped
        const auto list = parseSums("aa  x\n" + QByteArray(64, 'a') + "  OctoMX-5.0-win64.zip\n" + QByteArray(64, 'B') + " *b.tar.xz\n");
        QCOMPARE(list.size(), 2);
        QCOMPARE(list.value(QStringLiteral("b.tar.xz")), QByteArray(64, 'b'));
    }

    void manifests()
    {
        QVERIFY(isSafeManifestPath(QStringLiteral("octomx.exe")));
        QVERIFY(isSafeManifestPath(QStringLiteral("lib/libQt6Core.so.6")));
        QVERIFY(isSafeManifestPath(QStringLiteral("licenses/Qt/LGPL-3.0-only.txt")));
        for (const char *bad : {"../x", "a/../b", "/etc/passwd", "C:/x", "a\\b", "portable.txt", "Portable.TXT", "profile/x",
                                "downloads/a", "incoming", "MANIFEST.txt", ".old-4.3.0/x", ".new-4.4.0", ".update-journal", "a//b",
                                "./a", "a/", ""})
            QVERIFY2(!isSafeManifestPath(QString::fromLatin1(bad)), bad);
        Manifest m;
        QString why;
        const QByteArray good = "# c\nversion 4.4.0\nplatform win64\n" + QByteArray(64, 'a') + " 12 octomx.exe\n" + QByteArray(64, 'b') +
                                " 3 fonts/a b.ttf\n";
        QVERIFY2(parseManifest(good, &m, &why), qPrintable(why));
        QCOMPARE(m.files.size(), 2);
        QCOMPARE(m.files.at(1).path, QStringLiteral("fonts/a b.ttf"));
        QCOMPARE(m.files.at(0).size, 12);
        Manifest again;
        QVERIFY(parseManifest(manifestText(m), &again));
        QCOMPARE(again.files.size(), 2);
        QVERIFY(!parseManifest("version 4.4.0\nplatform win64\n" + QByteArray(64, 'a') + " 1 ../evil\n", &m, &why));
        QVERIFY(!parseManifest("version 4.4.0\nplatform win64\n" + QByteArray(64, 'a') + " 1 portable.txt\n", &m, &why));
        QVERIFY(!parseManifest("platform win64\n" + QByteArray(64, 'a') + " 1 a\n", &m, &why));
        QVERIFY(!parseManifest("version 4.4.0\nplatform win64\n" + QByteArray(64, 'a') + " 1 a\n" + QByteArray(64, 'a') + " 1 A\n", &m, &why));
        QVERIFY(why.contains(QStringLiteral("twice")));
        // verifyTree notices a changed file
        const QString dir = m_dir.filePath(QStringLiteral("mtree"));
        const Manifest pm = upd1::writePackage(dir, kNew, upd1::packageFiles(kNew));
        QVERIFY(verifyTree(dir, pm, &why));
        upd1::writeFile(QDir(dir).filePath(QStringLiteral("README.txt")), "OctoMX 4.4.1\n");   // same size, other bytes
        QVERIFY(!verifyTree(dir, pm, &why));
        QVERIFY(why.contains(QStringLiteral("checksum")));
    }

    void archives()
    {
        const QString src = QDir(m_dir.filePath(QStringLiteral("zsrc"))).filePath(packageDirName(kNew));
        const Manifest m = upd1::writePackage(src, kNew, upd1::packageFiles(kNew));
        for (bool deflate : {false, true}) {
            const QString zip = m_dir.filePath(deflate ? QStringLiteral("d.zip") : QStringLiteral("s.zip"));
            QVERIFY(upd1::writeFile(zip, upd1::makeZip(src, deflate)));
            const QString out = m_dir.filePath(deflate ? QStringLiteral("zout-d") : QStringLiteral("zout-s"));
            QString why;
            QVERIFY2(unzip(zip, out, 1 << 30, &why), qPrintable(why));
            QVERIFY2(verifyTree(QDir(out).filePath(packageDirName(kNew)), m, &why), qPrintable(why));
            // the size limit
            QVERIFY(!unzip(zip, m_dir.filePath(QStringLiteral("zout-small")), 1000, &why));
            QVERIFY(why.contains(QStringLiteral("more than")));
        }
        // a damaged entry (CRC) and unsafe names are refused
        QByteArray z = upd1::makeZip(src, false);
        const int at = int(z.indexOf("OctoMX 4.4.0"));
        QVERIFY(at > 0);
        z[at] = 'w';
        QVERIFY(upd1::writeFile(m_dir.filePath(QStringLiteral("bad.zip")), z));
        QString why;
        QVERIFY(!unzip(m_dir.filePath(QStringLiteral("bad.zip")), m_dir.filePath(QStringLiteral("zbad")), 1 << 30, &why));
        QVERIFY2(why.contains(QStringLiteral("CRC")), qPrintable(why));
        for (const char *evil : {"../evil.txt", "/abs.txt", "a/../../b"}) {
            QVERIFY(upd1::writeFile(m_dir.filePath(QStringLiteral("evil.zip")), upd1::makeZip(src, false, {QString::fromLatin1(evil)})));
            QVERIFY(!unzip(m_dir.filePath(QStringLiteral("evil.zip")), m_dir.filePath(QStringLiteral("zevil")), 1 << 30, &why));
            QVERIFY2(why.contains(QStringLiteral("unsafe")), qPrintable(why));
        }
        QVERIFY(!QFileInfo::exists(m_dir.filePath(QStringLiteral("evil.txt"))));
        QVERIFY(!unzip(m_dir.filePath(QStringLiteral("nothing.zip")), m_dir.filePath(QStringLiteral("z0")), 1 << 30, &why));
        if (platformTag() == QLatin1String("linux-x86_64")) {
            const QByteArray tx = upd1::makeArchive(src);
            QVERIFY(tx.size() > 100);
            const QString out = m_dir.filePath(QStringLiteral("tout"));
            QVERIFY2(untarXz(src + QStringLiteral(".tar.xz"), out, &why), qPrintable(why));
            QVERIFY2(verifyTree(QDir(out).filePath(packageDirName(kNew)), m, &why), qPrintable(why));
            QVERIFY(QFileInfo(QDir(out).filePath(packageDirName(kNew) + QStringLiteral("/bin/octomx"))).isExecutable());
        }
    }

    void stagedTreeAndLayout()
    {
        const QString stage = m_dir.filePath(QStringLiteral("stage1"));
        const QString top = QDir(stage).filePath(packageDirName(kNew));
        upd1::writePackage(top, kNew, upd1::packageFiles(kNew));
        QString t, why;
        Manifest m;
        QVERIFY2(checkStagedTree(stage, kNew, platformTag(), &t, &m, &why), qPrintable(why));
        QCOMPARE(t, top);
        QVERIFY(!checkStagedTree(stage, QStringLiteral("4.5.0"), platformTag(), &t, &m, &why));   // another version
        // a second top folder
        QDir().mkpath(QDir(stage).filePath(QStringLiteral("other")));
        QVERIFY(!checkStagedTree(stage, kNew, platformTag(), &t, &m, &why));
        QDir(QDir(stage).filePath(QStringLiteral("other"))).removeRecursively();
        // the program file is not a program
        const bool win = platformTag() == QLatin1String("win64");
        const QString exeRel = win ? QStringLiteral("octomx.exe") : QStringLiteral("bin/octomx");
        const QString stage2 = m_dir.filePath(QStringLiteral("stage2"));
        upd1::writePackage(QDir(stage2).filePath(packageDirName(kNew)), kNew, upd1::packageFiles(kNew, {{exeRel, "MZ not really"}}));
        QVERIFY(!checkStagedTree(stage2, kNew, platformTag(), &t, &m, &why));
        QVERIFY2(why.contains(QStringLiteral("64-bit")), qPrintable(why));
        // a file that does not match the manifest
        upd1::writeFile(QDir(top).filePath(QStringLiteral("fonts/Tahoma.ttf")), "font 4.4.X");
        QVERIFY(!checkStagedTree(stage, kNew, platformTag(), &t, &m, &why));
        QVERIFY(why.contains(QStringLiteral("damaged")));

        // layout: our package folder (writable) / no manifest / a developer build
        const Fixture fx = makeFixture(m_dir.filePath(QStringLiteral("lay")));
        const QString appDir = win ? fx.prog : QDir(fx.prog).filePath(QStringLiteral("bin"));
        Layout l = detectLayout(appDir);
        QVERIFY2(l.ok, qPrintable(l.why));
        QCOMPARE(l.manifest.version, kOld);
        QCOMPARE(QFileInfo(l.exe).fileName(), win ? QStringLiteral("octomx.exe") : QStringLiteral("octomx"));
        QVERIFY(QDir(fx.prog).entryList({QStringLiteral(".update-probe*")}, QDir::Files | QDir::Hidden).isEmpty());
        QFile::rename(QDir(fx.prog).filePath(QStringLiteral("MANIFEST.txt")), QDir(fx.prog).filePath(QStringLiteral("M.txt")));
        l = detectLayout(appDir);
        QVERIFY(!l.ok && !l.ours);
        QVERIFY(l.why.contains(QStringLiteral("not one of the published packages")));
        QFile::rename(QDir(fx.prog).filePath(QStringLiteral("M.txt")), QDir(fx.prog).filePath(QStringLiteral("MANIFEST.txt")));
        l = detectLayout(QCoreApplication::applicationDirPath());   // the build tree
        QVERIFY(!l.ok);
#ifndef Q_OS_WIN
        // a folder OctoMX cannot write to (read-only mount, owned by root): no automatic install
        if (::geteuid() != 0) {
            QFile::setPermissions(QDir(fx.prog).filePath(QStringLiteral("lib")), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
            l = detectLayout(appDir);
            QVERIFY(l.ours && !l.ok);
            QVERIFY2(l.why.contains(QStringLiteral("cannot write")), qPrintable(l.why));
            QFile::setPermissions(QDir(fx.prog).filePath(QStringLiteral("lib")),
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        }
#endif
    }

    void downloads()
    {
        const QString pkg = QDir(m_dir.filePath(QStringLiteral("dl-src"))).filePath(packageDirName(kNew));
        upd1::writePackage(pkg, kNew, upd1::packageFiles(kNew));
        const QByteArray archive = upd1::makeArchive(pkg);
        QVERIFY(archive.size() > 1000);
        const QString name = archiveName(kNew);

        // good
        ReleaseInfo r = publish(QStringLiteral("/good"), archive);
        UpdateDownloader::Result res = download(r, m_dir.filePath(QStringLiteral("u1")));
        QVERIFY2(res.ok, qPrintable(res.error));
        QCOMPARE(res.keyId, m_key.id);
        QCOMPARE(upd1::readFile(res.archivePath), archive);
        QVERIFY(!QFileInfo::exists(res.archivePath + QStringLiteral(".part")));
        // the request: our user agent, nothing else of ours
        QByteArray head;
        for (const QByteArray &q : std::as_const(m_srv->requests))
            if (q.startsWith("GET /good/" + name.toUtf8()))
                head = q.toLower();
        QVERIFY(head.contains("user-agent: octomx/test"));
        QVERIFY(!head.contains("cookie"));
        // a second run uses the verified copy (no new archive request)
        const int before = m_srv->count(QStringLiteral("/good/") + name);
        res = download(r, m_dir.filePath(QStringLiteral("u1")));
        QVERIFY(res.ok);
        QCOMPARE(m_srv->count(QStringLiteral("/good/") + name), before);

        // signed by a key the program does not trust
        const upd1::Key stranger = upd1::Key::make(false);
        res = download(publish(QStringLiteral("/stranger"), archive, true, &stranger), m_dir.filePath(QStringLiteral("u2")));
        QVERIFY(!res.ok && res.badSignature);
        QCOMPARE(m_srv->count(QStringLiteral("/stranger/") + name), 0);   // the archive is never fetched
        // SHA256SUMS changed after signing (to match a different archive)
        QByteArray other = archive;
        other[other.size() / 2] = char(other[other.size() / 2] ^ 0x55);
        res = download(publish(QStringLiteral("/altsums"), archive, true, nullptr, true, other), m_dir.filePath(QStringLiteral("u3")));
        QVERIFY(!res.ok && res.badSignature);
        QVERIFY(res.error.contains(QStringLiteral("does not match")));
        // the archive changed (the signed list is right)
        res = download(publish(QStringLiteral("/altarch"), archive, true, nullptr, false, other), m_dir.filePath(QStringLiteral("u4")));
        QVERIFY(!res.ok && res.badSignature);
        QVERIFY2(res.error.contains(QStringLiteral("changed or damaged")), qPrintable(res.error));
        QVERIFY(!QFileInfo::exists(QDir(m_dir.filePath(QStringLiteral("u4"))).filePath(name)));
        QVERIFY(!QFileInfo::exists(QDir(m_dir.filePath(QStringLiteral("u4"))).filePath(name + QStringLiteral(".part"))));
        // no signature at all: never downloaded, never installed
        const int reqs = int(m_srv->requests.size());
        res = download(publish(QStringLiteral("/unsigned"), archive, false), m_dir.filePath(QStringLiteral("u5")));
        QVERIFY(!res.ok && res.unsignedRelease);
        QVERIFY(res.error.contains(QStringLiteral("not signed")));
        QCOMPARE(int(m_srv->requests.size()), reqs);

        // a redirect inside the same server is followed; one to another host or port is refused
        r = publish(QStringLiteral("/redir"), archive);
        m_srv->routes.insert(QStringLiteral("/redir/go"), upd1::Route{302, {}, "Location: /redir/" + name.toUtf8() + "\r\n", false});
        for (ReleaseAsset &a : r.assets)
            if (a.name == name)
                a.url = m_srv->url(QStringLiteral("/redir/go"));
        res = download(r, m_dir.filePath(QStringLiteral("u6")));
        QVERIFY2(res.ok, qPrintable(res.error));
        QTcpServer otherPort;
        QVERIFY(otherPort.listen(QHostAddress::LocalHost, 0));
        m_srv->routes.insert(QStringLiteral("/redir/away"),
                             upd1::Route{302, {}, "Location: http://127.0.0.1:" + QByteArray::number(otherPort.serverPort()) + "/x\r\n", false});
        for (ReleaseAsset &a : r.assets)
            if (a.name == name)
                a.url = m_srv->url(QStringLiteral("/redir/away"));
        res = download(r, m_dir.filePath(QStringLiteral("u7")));
        QVERIFY(!res.ok);
        QVERIFY2(res.error.contains(QStringLiteral("refused")), qPrintable(res.error));
        QVERIFY(!otherPort.hasPendingConnections());
        // only https on GitHub's hosts (or the chosen mirror itself)
        const QUrl check(m_srv->url(QStringLiteral("/latest")));
        QVERIFY(UpdateDownloader::urlAllowed(QUrl(QStringLiteral("https://github.com/robtacconelli/OctoMX/releases/download/v4.4.0/x.zip")), check));
        QVERIFY(UpdateDownloader::urlAllowed(QUrl(QStringLiteral("https://release-assets.githubusercontent.com/x")), check));
        QVERIFY(!UpdateDownloader::urlAllowed(QUrl(QStringLiteral("http://github.com/x")), check));
        QVERIFY(!UpdateDownloader::urlAllowed(QUrl(QStringLiteral("https://github.com.evil.example/x")), check));
        QVERIFY(!UpdateDownloader::urlAllowed(QUrl(QStringLiteral("https://evil.example/x")), check));
        QVERIFY(!UpdateDownloader::urlAllowed(QUrl(QStringLiteral("ftp://github.com/x")), check));
        const QUrl def(QStringLiteral("https://api.github.com/repos/robtacconelli/OctoMX/releases/latest"));
        QVERIFY(!UpdateDownloader::urlAllowed(QUrl(QStringLiteral("http://127.0.0.1/x")), def));   // no local files for the real address
        QVERIFY(!UpdateDownloader::urlAllowed(QUrl(QStringLiteral("http://192.0.2.7/x")), QUrl(QStringLiteral("http://192.0.2.7/latest"))));

        // oversize: more bytes than the API said, or than OctoMX accepts
        r = publish(QStringLiteral("/big"), archive);
        for (ReleaseAsset &a : r.assets)
            if (a.name == name)
                a.size = 100;
        res = download(r, m_dir.filePath(QStringLiteral("u8")));
        QVERIFY(!res.ok);
        QVERIFY2(res.error.contains(QStringLiteral("larger")), qPrintable(res.error));
        res = download(publish(QStringLiteral("/big2"), archive), m_dir.filePath(QStringLiteral("u9")), 1000);
        QVERIFY(!res.ok);
        QVERIFY(res.error.contains(QStringLiteral("larger")));

        // resume: half of the file is there from an earlier attempt
        r = publish(QStringLiteral("/resume"), archive);
        const QString dir = m_dir.filePath(QStringLiteral("u10"));
        upd1::writeFile(QDir(dir).filePath(name + QStringLiteral(".part")), archive.left(archive.size() / 2));
        res = download(r, dir);
        QVERIFY2(res.ok, qPrintable(res.error));
        QCOMPARE(upd1::readFile(res.archivePath), archive);
        bool ranged = false;
        for (const QByteArray &q : std::as_const(m_srv->requests))
            if (q.startsWith("GET /resume/") && q.toLower().contains("range: bytes=" + QByteArray::number(archive.size() / 2) + "-"))
                ranged = true;
        QVERIFY(ranged);
        // a stale partial file that is longer than the archive: started again
        upd1::writeFile(QDir(dir).filePath(name + QStringLiteral(".part")), archive + "trailing junk");
        QFile::remove(QDir(dir).filePath(name));
        res = download(r, dir);
        QVERIFY2(res.ok, qPrintable(res.error));
        // no package for this platform
        ReleaseInfo none = r;
        none.assets.removeIf([&](const ReleaseAsset &a) { return a.name == name; });
        res = download(none, m_dir.filePath(QStringLiteral("u11")));
        QVERIFY(!res.ok);
        QVERIFY(res.error.contains(QStringLiteral("no package")));
    }

    void swapComplete()
    {
        const Fixture fx = makeFixture(m_dir.filePath(QStringLiteral("swap0")));
        QVERIFY(wholeAs(fx, false).isEmpty());
        QString why;
        QVERIFY2(swapInstall(planOf(fx), &why), qPrintable(why));
        QVERIFY2(wholeAs(fx, true).isEmpty(), qPrintable(wholeAs(fx, true)));
        QString state, from, to;
        QVERIFY(readJournal(fx.prog, &state, &from, &to));
        QCOMPARE(state, QStringLiteral("committed"));
        QCOMPARE(from, kOld);
        // the old files wait in .old-<version> until the next start
        QVERIFY(QFileInfo::exists(QDir(oldDirFor(fx.prog, kOld)).filePath(QStringLiteral("old-only.txt"))));
        QVERIFY(!QFileInfo::exists(QDir(fx.prog).filePath(QStringLiteral("old-only.txt"))));
        QVERIFY(!QFileInfo::exists(QDir(fx.prog).filePath(QStringLiteral("plugins/old"))));   // emptied folder removed
        const Recovery rec = recoverInstall(fx.prog);
        QCOMPARE(int(rec.what), int(Recovery::Cleaned));
        QCOMPARE(rec.to, kNew);
        QCOMPARE(rec.moved, 0);
        QVERIFY(machineryGone(fx.prog));
        QVERIFY(wholeAs(fx, true).isEmpty());
        QCOMPARE(int(recoverInstall(fx.prog).what), int(Recovery::Nothing));
    }

    void swapCrashAtEveryStep()
    {
        // a crash (kill) after every single file operation of the swap; the next start finishes it
        int total = -1;
        for (int crash = 0; crash < 200; ++crash) {
            const Fixture fx = makeFixture(m_dir.filePath(QStringLiteral("crash")));
            QString why;
            if (swapInstall(planOf(fx), &why, crash)) {
                total = crash;
                break;
            }
            QVERIFY2(why.contains(QStringLiteral("simulated crash")), qPrintable(why));
            // a crash in the middle of the recovery too (every few steps), then the start after it
            if (crash % 4 == 1) {
                const Recovery half = recoverInstall(fx.prog, crash % 7);
                QVERIFY(half.what == Recovery::Failed || half.what == Recovery::Finished);
                if (half.what == Recovery::Finished) {   // short enough to finish within the steps
                    QVERIFY2(wholeAs(fx, true).isEmpty(), qPrintable(wholeAs(fx, true)));
                    QVERIFY(machineryGone(fx.prog));
                    continue;
                }
            }
            const Recovery rec = recoverInstall(fx.prog);
            QVERIFY2(rec.what == Recovery::Finished, qPrintable(QStringLiteral("crash %1: %2 %3").arg(crash).arg(int(rec.what)).arg(rec.error)));
            QVERIFY2(wholeAs(fx, true).isEmpty(), qPrintable(QStringLiteral("crash %1: %2").arg(crash).arg(wholeAs(fx, true))));
            QVERIFY(machineryGone(fx.prog));
        }
        QVERIFY2(total > 10, qPrintable(QString::number(total)));
        qInfo("swap of %d files: %d file operations, a crash after each one was recovered", int(upd1::packageFiles(kNew).size()) + 3, total);
    }

    void swapRollBack()
    {
        // the staged files are gone after a crash (deleted by hand, a full disk): the old version comes back
        for (int crash : {1, 3, 6, 9, 12, 15}) {
            for (bool shared : {false, true}) {   // a file only the new version has / one both have
            const Fixture fx = makeFixture(m_dir.filePath(QStringLiteral("back")));
            QString why;
            QVERIFY(!swapInstall(planOf(fx), &why, crash));
            // remove a staged file that was not moved yet
            bool removed = false;
            {   // (the iterator holds the folder open: on Windows it could not be removed meanwhile)
                QDirIterator it(fx.stageTop, QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext() && !removed) {
                    const QString f = it.next();
                    const QString rel = QDir(fx.stageTop).relativeFilePath(f);
                    const bool both = fx.oldFiles.contains(rel) || rel == QLatin1String("MANIFEST.txt");
                    if (both == shared) {
                        QFile::remove(f);
                        removed = true;
                    }
                }
            }
            if (!removed)
                continue;   // everything of that kind is in place already
            const Recovery rec = recoverInstall(fx.prog);
            QVERIFY2(rec.what == Recovery::RolledBack, qPrintable(QStringLiteral("crash %1: %2 %3").arg(crash).arg(int(rec.what)).arg(rec.error)));
            QVERIFY2(wholeAs(fx, false).isEmpty(), qPrintable(QStringLiteral("crash %1: %2").arg(crash).arg(wholeAs(fx, false))));
            QVERIFY(machineryGone(fx.prog));
            }
        }
    }

    void swapErrorIsUndone()
    {
        // a foreign file where the new version wants to put one: nothing is changed at all
        {
            Fixture fx = makeFixture(m_dir.filePath(QStringLiteral("inway")));
            upd1::writeFile(QDir(fx.prog).filePath(QStringLiteral("new-only.txt")), "the user's own file");
            QString why;
            QVERIFY(!swapInstall(planOf(fx), &why));
            QVERIFY2(why.contains(QStringLiteral("in the way")), qPrintable(why));
            QCOMPARE(upd1::readFile(QDir(fx.prog).filePath(QStringLiteral("new-only.txt"))), QByteArray("the user's own file"));
            QFile::remove(QDir(fx.prog).filePath(QStringLiteral("new-only.txt")));
            QVERIFY(wholeAs(fx, false).isEmpty());
            QVERIFY(!QFileInfo::exists(QDir(fx.prog).filePath(QString::fromLatin1(kJournalName))));
        }
#ifndef Q_OS_WIN
        // a folder that cannot be written in the middle of the swap: undone at once, old version whole
        if (::geteuid() != 0) {
            Fixture fx = makeFixture(m_dir.filePath(QStringLiteral("ro")));
            const QString ro = QDir(fx.prog).filePath(QStringLiteral("plugins/platforms"));
            QFile::setPermissions(ro, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
            QString why;
            QVERIFY(!swapInstall(planOf(fx), &why));
            QFile::setPermissions(ro, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            QVERIFY2(why.contains(QStringLiteral("undone")), qPrintable(why));
            QVERIFY2(wholeAs(fx, false).isEmpty(), qPrintable(wholeAs(fx, false)));
            QVERIFY(!QFileInfo::exists(QDir(fx.prog).filePath(QString::fromLatin1(kJournalName))));
            QCOMPARE(int(recoverInstall(fx.prog).what), int(Recovery::Cleaned));   // the staging folder goes
            QVERIFY(machineryGone(fx.prog));
        }
#endif
        // a staging folder without journal (prepared, never installed) is removed at the next start
        Fixture fx = makeFixture(m_dir.filePath(QStringLiteral("stale")));
        QCOMPARE(int(recoverInstall(fx.prog).what), int(Recovery::Cleaned));
        QVERIFY(machineryGone(fx.prog));
        QVERIFY(wholeAs(fx, false).isEmpty());
    }

    void cleanupTestCase()
    {
        delete m_srv;
        clearTestTrustedKeys();
    }
};

QTEST_MAIN(TstUpdateInstall)
#include "tst_update_install.moc"
