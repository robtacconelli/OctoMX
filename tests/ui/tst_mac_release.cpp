// SPDX-License-Identifier: GPL-3.0-or-later
// [mac-1] a release that also carries the macOS package: every platform's updater takes its own
// archive and never OctoMX-<v>-macos-universal.zip (and the Mac never a Windows / Linux one), the
// update box of the Mac names the macOS file and offers the download page, and the program's own
// release verifier (octomx --verify-update) accepts a SHA256SUMS that lists three archives.
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

#include "mx/update_check.h"
#include "mx/update_download.h"
#include "mx/update_install.h"
#include "upd1_support.h"

using namespace mx;
using namespace mx::upd;

namespace {
const QString kMac = QStringLiteral("macos-universal");
const QString kWin = QStringLiteral("win64");
const QString kLin = QStringLiteral("linux-x86_64");

ReleaseInfo releaseWith(const QStringList &assets, const QString &tag = QStringLiteral("v5.4.0"))
{
    QList<QPair<QString, QString>> a;
    for (const QString &n : assets)
        a << qMakePair(n, QStringLiteral("https://github.com/robtacconelli/OctoMX/releases/download/%1/%2").arg(tag, n));
    ReleaseInfo r;
    parseLatestRelease(upd1::releaseJson(tag, a), &r);
    return r;
}
} // namespace

class TstMacRelease : public QObject {
    Q_OBJECT
private slots:
    void assetChoiceWithThreeArchives()
    {
        const QString win = archiveName(QStringLiteral("5.4.0"), kWin), lin = archiveName(QStringLiteral("5.4.0"), kLin),
                      mac = archiveName(QStringLiteral("5.4.0"), kMac);
        QCOMPARE(mac, QStringLiteral("OctoMX-5.4-macos-universal.zip"));
        const QStringList sums = {QStringLiteral("SHA256SUMS"), QStringLiteral("SHA256SUMS.sig")};
        // all three, in every order: each platform its own
        const QList<QStringList> orders = {{win, lin, mac}, {mac, win, lin}, {lin, mac, win}};
        for (const QStringList &o : orders) {
            const ReleaseInfo r = releaseWith(o + sums);
            QCOMPARE(UpdateDownloader::platformAsset(r, kWin)->name, win);
            QCOMPARE(UpdateDownloader::platformAsset(r, kLin)->name, lin);
            QCOMPARE(UpdateDownloader::platformAsset(r, kMac)->name, mac);
        }
        // only the macOS package: Windows and Linux find nothing (download page), never the zip of the Mac
        const ReleaseInfo onlyMac = releaseWith(QStringList{mac} + sums);
        QVERIFY(!UpdateDownloader::platformAsset(onlyMac, kWin));
        QVERIFY(!UpdateDownloader::platformAsset(onlyMac, kLin));
        // only the others: the Mac finds nothing
        const ReleaseInfo noMac = releaseWith(QStringList{win, lin} + sums);
        QVERIFY(!UpdateDownloader::platformAsset(noMac, kMac));
        // a macOS package with another version spelling is still recognised only by the Mac
        for (const QString &pl : {kWin, kLin})
            QVERIFY(!isPackageArchiveName(QStringLiteral("OctoMX-5.4.0-macos-universal.zip"), pl));
        QVERIFY(isPackageArchiveName(QStringLiteral("OctoMX-5.4.0-macos-universal.zip"), kMac));
        QVERIFY(!isPackageArchiveName(QStringLiteral("OctoMX-5.4-macos-arm64.zip"), kMac));
#ifdef Q_OS_MACOS
        QCOMPARE(UpdateDownloader::platformAsset(releaseWith(QStringList{win, lin, mac} + sums), platformTag())->name, mac);
#endif
    }

    void verifyUpdateWithThreeArchives()
    {
#if !defined(MX_UPDATE_TEST_HOOKS) || !defined(MX_OCTOMX_EXE)
        QSKIP("needs a build with MX_TEST_HOOKS (the throwaway key) and the program");
#else
        QTemporaryDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/tst_mac_release-XXXXXX"));
        QVERIFY(dir.isValid());
        const QStringList names = {archiveName(QStringLiteral("5.4.0"), kWin), archiveName(QStringLiteral("5.4.0"), kLin),
                                   archiveName(QStringLiteral("5.4.0"), kMac)};
        QByteArray sums;
        for (const QString &n : names) {
            QFile f(dir.filePath(n));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("archive " + n.toUtf8());
            f.close();
            sums += sha256OfFile(dir.filePath(n)) + "  " + n.toUtf8() + '\n';
        }
        const QByteArray seed(32, '\x5a');
        const QByteArray pub = testPublicKeyFromSeed(seed);
        QFile s(dir.filePath(QStringLiteral("SHA256SUMS")));
        QVERIFY(s.open(QIODevice::WriteOnly));
        s.write(sums);
        s.close();
        QFile sig(dir.filePath(QStringLiteral("SHA256SUMS.sig")));
        QVERIFY(sig.open(QIODevice::WriteOnly));
        sig.write(formatSignatureFile(keyIdOf(pub), testSign(seed, sums)));
        sig.close();
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("MX_UPDATE_TEST_PUBKEY"), QString::fromLatin1(pub.toHex()));
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        p.setProcessEnvironment(env);
        const QString report = dir.filePath(QStringLiteral("report.txt"));
        p.start(QStringLiteral(MX_OCTOMX_EXE), {QStringLiteral("--verify-update"), dir.path(), QStringLiteral("--verify-report"), report});
        QVERIFY(p.waitForFinished(60000));
        QFile r(report);
        QVERIFY(r.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(r.readAll());
        QVERIFY2(p.exitCode() == 0, qPrintable(text));
        for (const QString &n : names)
            QVERIFY2(text.contains(n + QStringLiteral(": SHA-256 matches")), qPrintable(text));
        QVERIFY2(text.contains(QLatin1String("3 of 3 listed archives checked")), qPrintable(text));
        QVERIFY(text.contains(QLatin1String("RESULT: verified")));
        // one archive changed after signing: refused
        QFile m(dir.filePath(names.at(2)));
        QVERIFY(m.open(QIODevice::Append));
        m.write("x");
        m.close();
        p.start(QStringLiteral(MX_OCTOMX_EXE), {QStringLiteral("--verify-update"), dir.path(), QStringLiteral("--verify-report"), report});
        QVERIFY(p.waitForFinished(60000));
        QVERIFY(p.exitCode() != 0);
#endif
    }
};

QTEST_MAIN(TstMacRelease)
#include "tst_mac_release.moc"
