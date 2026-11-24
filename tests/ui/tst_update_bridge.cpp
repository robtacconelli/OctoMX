// SPDX-License-Identifier: GPL-3.0-or-later
// [upd-2] Legacy starter files (mx/update_install.h): a folder that came to 5.0 from a copy installed
// before 5.0 holds small starter files under the old program file names. The 5.x side, below the UI:
//   * names: legacyStarterFiles(); an asset under the old package name is never taken by 5.x
//   * asset choice in every combination (OctoMX package only, old-name package only, both in either
//     order, the other platform's, another version spelling): a 5.x only takes OctoMX-<v>-<platform>
//   * such a folder is recognised as a published package (layout "bridged")
//   * the starter files are kept in place by a normal update (and by the next one), recorded in the
//     installed manifest and the journal, untouched by a crash at every step, by the recovery and by
//     a roll-back; a starter file the user changed stays but is no longer listed; a package that has
//     a file of that name itself replaces it normally
//   * a journal exactly as an earlier version writes it (committed, its .old-4.5.0 folder, and still
//     swapping) finished and cleaned by this version
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

#include "upd1_support.h"
#include "mx/update_download.h"
#include "mx/update_install.h"

using namespace mx;
using namespace mx::upd;

namespace {

const bool kWin = platformTag() == QLatin1String("win64");

// the legacy starter files of a folder (their content is irrelevant to the swap: small stand-ins
// made here)
QMap<QString, QByteArray> starterFiles()
{
    QMap<QString, QByteArray> m;
    for (const QString &f : legacyStarterFiles())
        m.insert(f, f.endsWith(QLatin1String(".exe")) ? QByteArray("MZ starter of octomx.exe")
                    : f.startsWith(QLatin1String("bin/")) ? QByteArray("\x7f" "ELF starter of bin/octomx")
                                                          : QByteArray("#!/bin/sh\nexec \"$(dirname \"$0\")/octomx\" \"$@\"\n"));
    return m;
}

QMap<QString, QByteArray> userFiles()
{
    return {{QStringLiteral("portable.txt"), "portable\n"},
            {QStringLiteral("profile/settings.json"), "{}"},
            {QStringLiteral("downloads/song.ogg"), "music"},
            {QStringLiteral("my notes.txt"), "not ours"}};
}

QMap<QString, QByteArray> merged(QMap<QString, QByteArray> a, const QMap<QString, QByteArray> &b)
{
    for (auto it = b.cbegin(); it != b.cend(); ++it)
        a.insert(it.key(), it.value());
    return a;
}

struct Fx {
    QString prog, stageTop, from, to;
    Manifest oldM, newM;
    QMap<QString, QByteArray> oldFiles, newFiles;
    SwapPlan plan() const { return SwapPlan{prog, stageTop, from, to, oldM, newM}; }
};

// installed: `oldFiles` (version from) with the user's files; staged: `newFiles` (version to)
Fx makeFx(const QString &root, const QString &from, const QMap<QString, QByteArray> &oldFiles, const QString &to,
          const QMap<QString, QByteArray> &newFiles, const QString &stageName)
{
    Fx fx;
    QDir(root).removeRecursively();
    fx.prog = QDir(root).filePath(QStringLiteral("installed-") + platformTag());   // any folder name: the user's choice
    fx.from = from;
    fx.to = to;
    fx.oldFiles = oldFiles;
    fx.newFiles = newFiles;
    fx.oldM = upd1::writePackage(fx.prog, from, oldFiles);
    const QMap<QString, QByteArray> u = userFiles();
    for (auto it = u.cbegin(); it != u.cend(); ++it)
        upd1::writeFile(QDir(fx.prog).filePath(it.key()), it.value());
    fx.stageTop = QDir(stageDirFor(fx.prog, to)).filePath(stageName);
    fx.newM = upd1::writePackage(fx.stageTop, to, newFiles);
    return fx;
}

// a folder with the legacy starter files (5.0.0) and a normal later package staged
Fx bridgedFx(const QString &root, const QString &to = QStringLiteral("5.1.0"))
{
    return makeFx(root, QStringLiteral("5.0.0"), merged(upd1::packageFiles(QStringLiteral("5.0.0")), starterFiles()), to,
                  upd1::packageFiles(to, {{QStringLiteral("new-in-5.1.txt"), "new"}}), packageDirName(to));
}

QString machineryLeft(const QString &prog)
{
    return QDir(prog)
        .entryList({QStringLiteral(".old-*"), QStringLiteral(".new-*"), QStringLiteral(".update-*")}, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)
        .join(QLatin1Char(' '));
}

// the folder holds exactly `files` (version v, listed in its manifest), the starter files as installed
// (listed too when `listed`), the user's files, nothing of `other`, no update machinery
QString exactly(const Fx &fx, const QString &v, const QMap<QString, QByteArray> &files, const QMap<QString, QByteArray> &other, bool listed = true)
{
    Manifest installed;
    QString why;
    if (!readManifest(fx.prog, &installed, &why))
        return QStringLiteral("manifest: ") + why;
    if (installed.version != v)
        return QStringLiteral("installed version %1, expected %2").arg(installed.version, v);
    const QMap<QString, QByteArray> fw = starterFiles();
    QMap<QString, QByteArray> want = files;
    if (listed)
        want = merged(want, fw);
    QStringList inManifest;
    for (const ManifestEntry &e : installed.files)
        inManifest << e.path;
    QStringList expected = want.keys();
    inManifest.sort();
    expected.sort();
    if (inManifest != expected)
        return QStringLiteral("manifest lists [%1], expected [%2]").arg(inManifest.join(QLatin1Char(' ')), expected.join(QLatin1Char(' ')));
    if (!verifyTree(fx.prog, installed, &why))
        return QStringLiteral("tree: ") + why;
    for (auto it = fw.cbegin(); it != fw.cend(); ++it)
        if (upd1::readFile(QDir(fx.prog).filePath(it.key())) != it.value())
            return QStringLiteral("starter file changed or missing: ") + it.key();
    for (auto it = other.cbegin(); it != other.cend(); ++it)
        if (!want.contains(it.key()) && !fw.contains(it.key()) && QFileInfo::exists(QDir(fx.prog).filePath(it.key())))
            return QStringLiteral("a file of the other version is left: ") + it.key();
    const QMap<QString, QByteArray> u = userFiles();
    for (auto it = u.cbegin(); it != u.cend(); ++it)
        if (upd1::readFile(QDir(fx.prog).filePath(it.key())) != it.value())
            return QStringLiteral("user file changed: ") + it.key();
    if (!machineryLeft(fx.prog).isEmpty())
        return QStringLiteral("left: ") + machineryLeft(fx.prog);
    return QString();
}

ReleaseInfo releaseWith(const QStringList &assets, const QString &tag = QStringLiteral("v5.1.0"))
{
    QList<QPair<QString, QString>> a;
    for (const QString &n : assets)
        a << qMakePair(n, QStringLiteral("https://github.com/robtacconelli/OctoMX/releases/download/%1/%2").arg(tag, n));
    ReleaseInfo r;
    parseLatestRelease(upd1::releaseJson(tag, a), &r);
    return r;
}

} // namespace

class TstUpdateBridge : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;

private slots:
    void names()
    {
        QCOMPARE(archiveName(QStringLiteral("5.0.0"), QStringLiteral("win64")), QStringLiteral("OctoMX-5.0-win64.zip"));
        for (const QString &pl : {QStringLiteral("win64"), QStringLiteral("linux-x86_64")}) {
            const QString ext = pl == QLatin1String("win64") ? QStringLiteral(".zip") : QStringLiteral(".tar.xz");
            QVERIFY(!isPackageArchiveName(QStringLiteral("WinMX-5.0-") + pl + ext, pl));   // the old package name: never a package for 5.x
            QVERIFY(isPackageArchiveName(QStringLiteral("OctoMX-5.0-") + pl + ext, pl));
            QVERIFY(!isPackageArchiveName(QStringLiteral("OctoMX-5.0-") + pl + QStringLiteral(".7z"), pl));
        }
        QCOMPARE(legacyStarterFiles(QStringLiteral("win64")), QStringList{QStringLiteral("winmx.exe")});
        QCOMPARE(legacyStarterFiles(QStringLiteral("linux-x86_64")), (QStringList{QStringLiteral("winmx"), QStringLiteral("bin/winmx")}));
        for (const QString &f : legacyStarterFiles(QStringLiteral("linux-x86_64")) + legacyStarterFiles(QStringLiteral("win64")))
            QVERIFY(isSafeManifestPath(f));
    }

    void assetChoice()
    {
        // every combination: a 5.x takes its own OctoMX-<v>-<platform> and nothing else, in any folder
        for (const QString &pl : {QStringLiteral("win64"), QStringLiteral("linux-x86_64")}) {
            const QString other = pl == QLatin1String("win64") ? QStringLiteral("linux-x86_64") : QStringLiteral("win64");
            auto arch = [](const QString &base, const QString &v, const QString &p) {
                return base + QLatin1Char('-') + v + QLatin1Char('-') + p + (p == QLatin1String("win64") ? QStringLiteral(".zip") : QStringLiteral(".tar.xz"));
            };
            const QString normal = arch(QStringLiteral("OctoMX"), QStringLiteral("5.1"), pl);
            const QString bridge = arch(QStringLiteral("WinMX"), QStringLiteral("5.1"), pl);   // the old package name
            const QStringList sums = {QStringLiteral("SHA256SUMS"), QStringLiteral("SHA256SUMS.sig")};
            struct Case {
                QStringList assets;
                QString want;   // empty = no package for this system
            };
            const QList<Case> cases = {
                {QStringList{normal} + sums, normal},
                {QStringList{bridge} + sums, QString()},                      // only the old name: download page
                {QStringList{bridge, normal} + sums, normal},                 // both, the old name listed first
                {QStringList{normal, bridge} + sums, normal},
                {QStringList{bridge, arch(QStringLiteral("WinMX"), QStringLiteral("5.1"), other), arch(QStringLiteral("OctoMX"), QStringLiteral("5.1"), other)} + sums,
                 QString()},                                                  // only the other platform's normal package
                {QStringList{arch(QStringLiteral("OctoMX"), QStringLiteral("5.1"), other), bridge, normal, arch(QStringLiteral("WinMX"), QStringLiteral("5.1"), other)} + sums,
                 normal},
                {QStringList{bridge, arch(QStringLiteral("OctoMX"), QStringLiteral("5.1.0"), pl)} + sums,
                 arch(QStringLiteral("OctoMX"), QStringLiteral("5.1.0"), pl)},  // another version spelling: any OctoMX package
                {sums, QString()},
            };
            for (const Case &c : cases) {
                const ReleaseInfo r = releaseWith(c.assets);
                const ReleaseAsset *a = UpdateDownloader::platformAsset(r, pl);
                QCOMPARE(a ? a->name : QString(), c.want);
            }
        }
    }

    void bridgedLayout()
    {
#ifdef Q_OS_MACOS
        QSKIP("folders of copies before 5.0 exist only on Windows and Linux, and macOS does not install updates by itself");
#endif
        const Fx fx = bridgedFx(m_dir.filePath(QStringLiteral("lay")));
        const QString appDir = kWin ? fx.prog : QDir(fx.prog).filePath(QStringLiteral("bin"));
        const Layout l = detectLayout(appDir);
        QVERIFY2(l.ok, qPrintable(l.why));
        QVERIFY(l.ours && l.bridged);
        QCOMPARE(l.manifest.version, QStringLiteral("5.0.0"));
        QCOMPARE(QFileInfo(l.exe).fileName(), kWin ? QStringLiteral("octomx.exe") : QStringLiteral("octomx"));   // the real program
        // a normal package folder is not "bridged"
        const QString plain = m_dir.filePath(QStringLiteral("lay-plain/OctoMX"));
        upd1::writePackage(plain, QStringLiteral("5.0.0"), upd1::packageFiles(QStringLiteral("5.0.0")));
        const Layout p = detectLayout(kWin ? plain : QDir(plain).filePath(QStringLiteral("bin")));
        QVERIFY2(p.ok, qPrintable(p.why));
        QVERIFY(!p.bridged);
    }

    void startersKeptByNormalUpdates()
    {
#ifdef Q_OS_MACOS
        QSKIP("folders of copies before 5.0 exist only on Windows and Linux, and macOS does not install updates by itself");
#endif
        Fx fx = bridgedFx(m_dir.filePath(QStringLiteral("keep")));
        QString why;
        QVERIFY2(swapInstall(fx.plan(), &why), qPrintable(why));
        QString state, from, to;
        QStringList kept;
        QVERIFY(readJournal(fx.prog, &state, &from, &to, &kept));
        QCOMPARE(state, QStringLiteral("committed"));
        QStringList fw = starterFiles().keys();
        fw.sort();
        kept.sort();
        QCOMPARE(kept, fw);
        // nothing of the starter files went to .old-5.0.0
        for (const QString &f : fw)
            QVERIFY(!QFileInfo::exists(QDir(oldDirFor(fx.prog, fx.from)).filePath(f)));
        QCOMPARE(int(recoverInstall(fx.prog).what), int(Recovery::Cleaned));
        QVERIFY2(exactly(fx, fx.to, fx.newFiles, fx.oldFiles).isEmpty(), qPrintable(exactly(fx, fx.to, fx.newFiles, fx.oldFiles)));
        const QByteArray text = upd1::readFile(QDir(fx.prog).filePath(QStringLiteral("MANIFEST.txt")));
        QVERIFY2(text.contains("# kept in place from 5.0.0"), text.constData());
        // the installed manifest is the package's own one plus the kept lines
        const QByteArray pkg = manifestText(fx.newM);
        QVERIFY(text.startsWith(pkg));
        // still a published package, still bridged: the next normal update keeps them again
        const Layout l = detectLayout(kWin ? fx.prog : QDir(fx.prog).filePath(QStringLiteral("bin")));
        QVERIFY2(l.ok && l.bridged, qPrintable(l.why));
        Fx next = fx;
        next.from = fx.to;
        next.to = QStringLiteral("5.2.0");
        next.oldFiles = fx.newFiles;
        next.oldM = l.manifest;
        next.newFiles = upd1::packageFiles(next.to);
        next.stageTop = QDir(stageDirFor(fx.prog, next.to)).filePath(packageDirName(next.to));
        next.newM = upd1::writePackage(next.stageTop, next.to, next.newFiles);
        QVERIFY2(swapInstall(next.plan(), &why), qPrintable(why));
        QCOMPARE(int(recoverInstall(fx.prog).what), int(Recovery::Cleaned));
        QVERIFY2(exactly(next, next.to, next.newFiles, next.oldFiles).isEmpty(), qPrintable(exactly(next, next.to, next.newFiles, next.oldFiles)));
        QCOMPARE(upd1::readFile(QDir(fx.prog).filePath(QStringLiteral("MANIFEST.txt"))).count("# kept in place"), 1);
    }

    void changedStarterIsTheUsers()
    {
#ifdef Q_OS_MACOS
        QSKIP("folders of copies before 5.0 exist only on Windows and Linux, and macOS does not install updates by itself");
#endif
        // the user replaced a starter file: it stays where it is, but is no longer listed as ours
        Fx fx = bridgedFx(m_dir.filePath(QStringLiteral("changed")));
        const QString f = starterFiles().firstKey();
        upd1::writeFile(QDir(fx.prog).filePath(f), "the user's own file under that name");
        QString why;
        QVERIFY2(swapInstall(fx.plan(), &why), qPrintable(why));
        QCOMPARE(int(recoverInstall(fx.prog).what), int(Recovery::Cleaned));
        QCOMPARE(upd1::readFile(QDir(fx.prog).filePath(f)), QByteArray("the user's own file under that name"));
        Manifest m;
        QVERIFY(readManifest(fx.prog, &m));
        QVERIFY(!m.contains(f));
        QCOMPARE(m.version, fx.to);
        // a starter file that is gone stays gone (nothing to keep), the others are kept
        Fx g = bridgedFx(m_dir.filePath(QStringLiteral("gone")));
        QFile::remove(QDir(g.prog).filePath(f));
        QVERIFY2(swapInstall(g.plan(), &why), qPrintable(why));
        QCOMPARE(int(recoverInstall(g.prog).what), int(Recovery::Cleaned));
        QVERIFY(!QFileInfo::exists(QDir(g.prog).filePath(f)));
        QVERIFY(readManifest(g.prog, &m));
        QVERIFY(!m.contains(f));
        QCOMPARE(m.files.size(), g.newFiles.size() + starterFiles().size() - 1);
    }

    void packageWithTheStarterNameReplacesIt()
    {
#ifdef Q_OS_MACOS
        QSKIP("folders of copies before 5.0 exist only on Windows and Linux, and macOS does not install updates by itself");
#endif
        // a later package that has a file of that name itself: an ordinary file of the package
        QMap<QString, QByteArray> newFiles = merged(upd1::packageFiles(QStringLiteral("5.1.0")), starterFiles());
        for (auto it = newFiles.begin(); it != newFiles.end(); ++it)
            if (starterFiles().contains(it.key()))
                it.value() += " (5.1)";
        Fx fx = makeFx(m_dir.filePath(QStringLiteral("repl")), QStringLiteral("5.0.0"), merged(upd1::packageFiles(QStringLiteral("5.0.0")), starterFiles()),
                       QStringLiteral("5.1.0"), newFiles, packageDirName(QStringLiteral("5.1.0")));
        QString why;
        QVERIFY2(swapInstall(fx.plan(), &why), qPrintable(why));
        QStringList kept;
        QVERIFY(readJournal(fx.prog, nullptr, nullptr, nullptr, &kept));
        QVERIFY(kept.isEmpty());
        QCOMPARE(int(recoverInstall(fx.prog).what), int(Recovery::Cleaned));
        for (auto it = newFiles.cbegin(); it != newFiles.cend(); ++it)
            QCOMPARE(upd1::readFile(QDir(fx.prog).filePath(it.key())), it.value());
        QVERIFY(!upd1::readFile(QDir(fx.prog).filePath(QStringLiteral("MANIFEST.txt"))).contains("# kept"));
    }

    void startersSurviveCrashesAndRollBack()
    {
#ifdef Q_OS_MACOS
        QSKIP("folders of copies before 5.0 exist only on Windows and Linux, and macOS does not install updates by itself");
#endif
        // a crash after every single file operation of 5.0 -> 5.1 in such a folder, then the next start
        int total = -1;
        for (int crash = 0; crash < 200; ++crash) {
            const Fx fx = bridgedFx(m_dir.filePath(QStringLiteral("crash")));
            QString why;
            if (swapInstall(fx.plan(), &why, crash)) {
                total = crash;
                break;
            }
            QVERIFY2(why.contains(QStringLiteral("simulated crash")), qPrintable(why));
            const QMap<QString, QByteArray> fw = starterFiles();
            for (auto it = fw.cbegin(); it != fw.cend(); ++it)   // never touched meanwhile
                QCOMPARE(upd1::readFile(QDir(fx.prog).filePath(it.key())), it.value());
            const Recovery rec = recoverInstall(fx.prog);
            QVERIFY2(rec.what == Recovery::Finished, qPrintable(QStringLiteral("crash %1: %2 %3").arg(crash).arg(int(rec.what)).arg(rec.error)));
            const QString e = exactly(fx, fx.to, fx.newFiles, fx.oldFiles);
            QVERIFY2(e.isEmpty(), qPrintable(QStringLiteral("crash %1: %2").arg(crash).arg(e)));
        }
        QVERIFY2(total > 10, qPrintable(QString::number(total)));
        // roll back (a staged file vanished after a crash): the old version and its manifest come back,
        // the starter files in place and listed as before
        for (int crash : {1, 4, 8, 12}) {
            const Fx fx = bridgedFx(m_dir.filePath(QStringLiteral("back")));
            QString why;
            QVERIFY(!swapInstall(fx.plan(), &why, crash));
            bool removed = false;
            {
                QDirIterator it(fx.stageTop, QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext() && !removed) {
                    const QString f = it.next();
                    if (QFileInfo(f).fileName() != QLatin1String("MANIFEST.txt"))
                        removed = QFile::remove(f);
                }
            }
            if (!removed)
                continue;
            const Recovery rec = recoverInstall(fx.prog);
            QVERIFY2(rec.what == Recovery::RolledBack, qPrintable(QStringLiteral("crash %1: %2 %3").arg(crash).arg(int(rec.what)).arg(rec.error)));
            const QMap<QString, QByteArray> oldProgram = upd1::packageFiles(QStringLiteral("5.0.0"));
            const QString e = exactly(fx, fx.from, oldProgram, fx.newFiles);
            QVERIFY2(e.isEmpty(), qPrintable(QStringLiteral("crash %1: %2").arg(crash).arg(e)));
            QVERIFY(!upd1::readFile(QDir(fx.prog).filePath(QStringLiteral("MANIFEST.txt"))).contains("# kept"));
        }
    }

    void journalWrittenBy45()
    {
#ifdef Q_OS_MACOS
        QSKIP("folders of copies before 5.0 exist only on Windows and Linux, and macOS does not install updates by itself");
#endif
        // exactly what version 4.5 leaves after "Restart Now" (git show v4.5.0:src/api/update_install.cpp):
        // the new files in place, the old files in .old-4.5.0/, the staging folder emptied, and the
        // journal "committed"; the first start of this version removes the old files and reports it
        const QString prog = m_dir.filePath(QStringLiteral("j45/installed-") + platformTag());
        QDir(QDir(prog).absolutePath()).removeRecursively();
        const QMap<QString, QByteArray> bridge = merged(upd1::packageFiles(QStringLiteral("5.0.0")), starterFiles());
        upd1::writePackage(prog, QStringLiteral("5.0.0"), bridge);
        const QString old = oldDirFor(prog, QStringLiteral("4.5.0"));
        const QStringList oldFiles = kWin ? QStringList{QStringLiteral("winmx.exe"), QStringLiteral("README.txt"), QStringLiteral("MANIFEST.txt")}
                                          : QStringList{QStringLiteral("winmx"), QStringLiteral("bin/winmx"), QStringLiteral("README.txt"),
                                                        QStringLiteral("share/applications/menu.desktop"), QStringLiteral("MANIFEST.txt")};
        for (const QString &f : oldFiles)
            upd1::writeFile(QDir(old).filePath(f), "4.5 " + f.toUtf8());
        const QString stageRel = QStringLiteral(".new-5.0.0/WinMX-5.0-") + platformTag();   // the old package name
        QDir().mkpath(QDir(prog).filePath(stageRel));
        QByteArray j = "winmx-ng-update-journal 1\nstate committed\nfrom 4.5.0\nto 5.0.0\nstage " + stageRel.toUtf8() + "\nold .old-4.5.0\n";
        for (const QString &f : oldFiles)
            j += "o " + f.toUtf8() + '\n';
        QStringList nf = bridge.keys();
        const QString exe45 = kWin ? QStringLiteral("winmx.exe") : QStringLiteral("bin/winmx");   // 4.5 moves its own program file last
        nf.removeAll(exe45);
        nf.sort();
        nf << QStringLiteral("MANIFEST.txt") << exe45;
        for (const QString &f : nf)
            j += "n " + f.toUtf8() + '\n';
        upd1::writeFile(QDir(prog).filePath(QStringLiteral(".update-journal")), j);
        QString state, from, to;
        QVERIFY(readJournal(prog, &state, &from, &to));
        QCOMPARE(state, QStringLiteral("committed"));
        const Recovery rec = recoverInstall(prog);
        QCOMPARE(int(rec.what), int(Recovery::Cleaned));
        QCOMPARE(rec.from, QStringLiteral("4.5.0"));
        QCOMPARE(rec.to, QStringLiteral("5.0.0"));
        QCOMPARE(rec.moved, 0);
        QVERIFY2(machineryLeft(prog).isEmpty(), qPrintable(machineryLeft(prog)));
        Manifest m;
        QString why;
        QVERIFY(readManifest(prog, &m, &why));
        QVERIFY2(verifyTree(prog, m, &why), qPrintable(why));
        // the same journal still "swapping" with everything in place (it was killed before its last
        // journal write): finished, then cleaned
        upd1::writePackage(prog, QStringLiteral("5.0.0"), bridge);
        for (const QString &f : oldFiles)
            upd1::writeFile(QDir(old).filePath(f), "4.5 " + f.toUtf8());
        QDir().mkpath(QDir(prog).filePath(stageRel));
        upd1::writeFile(QDir(prog).filePath(QStringLiteral(".update-journal")), QByteArray(j).replace("state committed", "state swapping"));
        const Recovery rec2 = recoverInstall(prog);
        QCOMPARE(int(rec2.what), int(Recovery::Finished));
        QCOMPARE(rec2.to, QStringLiteral("5.0.0"));
        QVERIFY2(machineryLeft(prog).isEmpty(), qPrintable(machineryLeft(prog)));
        QVERIFY(readManifest(prog, &m, &why));
        QVERIFY2(verifyTree(prog, m, &why), qPrintable(why));
    }
};

QTEST_MAIN(TstUpdateBridge)
#include "tst_update_bridge.moc"
