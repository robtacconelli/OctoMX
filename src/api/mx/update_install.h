// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX [upd-1]: the signed self-update, everything below the UI.
//
// Trust: a release's SHA256SUMS is signed with the project's Ed25519 release key; SHA256SUMS.sig is
//   a short text file (see formatSignatureFile). The public keys the program trusts are a table built
//   into the source (update_keys.cpp; a second key can be added there for a key rotation). Tests trust
//   a throwaway key through addTestTrustedKey(), which only exists in builds with MX_TEST_HOOKS=ON (define MX_UPDATE_TEST_HOOKS; the
//   release build has MX_TEST_HOOKS=OFF: the function is not even compiled, a caller would not link).
// Packages: every package carries MANIFEST.txt (version, platform and "sha256 size path"
//   of every file, written by tools/release/manifest.sh). The manifest of the running installation
//   says which files are ours; the new one says what to expect and is checked file by file.
// Swap: the new package is unpacked into <program dir>/.new-<version>/ (same volume), checked, then
//   swapInstall() moves the old files into <program dir>/.old-<version>/ and the new ones in, one file
//   at a time, with a journal (<program dir>/.update-journal) written first. recoverInstall() at the
//   next start finishes (or undoes) a swap that was cut short and removes the old files of a finished
//   one. Files not listed in the old manifest (portable.txt, profile/, downloads, anything the user
//   put there) are never moved; a new file that would land on such a file stops the update before
//   anything is changed.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace mx::upd {

// ------------------------------------------------------------------------------------ trust
struct PublicKey {
    QString id;        // first 16 hex digits of SHA-256(raw key)
    QByteArray raw;    // 32 bytes
};
QList<PublicKey> trustedKeys();                 // built-in table (+ test keys in test builds)
QString keyIdOf(const QByteArray &raw32);
bool testHooksCompiled();                       // MX_UPDATE_TEST_HOOKS build (never a release build)
#ifdef MX_UPDATE_TEST_HOOKS
// TEST HOOK: trust an extra public key in this process (tests only; not compiled in release builds)
void addTestTrustedKey(const QByteArray &raw32);
void clearTestTrustedKeys();
// tests: a throwaway key pair (32-byte seed -> public key) and a signature with it
QByteArray testPublicKeyFromSeed(const QByteArray &seed32);
QByteArray testSign(const QByteArray &seed32, const QByteArray &message);
#endif

bool ed25519Verify(const QByteArray &publicKey32, const QByteArray &message, const QByteArray &signature64);
// kSignatureHeader + "\nkey <id>\ned25519 <base64 signature>\n" (see update_install.cpp)
QByteArray formatSignatureFile(const QString &keyId, const QByteArray &signature64);
bool parseSignatureFile(const QByteArray &text, QString *keyId, QByteArray *signature64, QString *error = nullptr);

struct SigCheck {
    bool ok = false;
    QString keyId;     // the key that signed it
    QString error;     // plain text for the user
};
// the signature file over the exact bytes of SHA256SUMS, against trustedKeys()
SigCheck verifySums(const QByteArray &sums, const QByteArray &sigFile);
// "<64 hex>  <name>" lines -> name -> lower-case hex; malformed lines are skipped
QHash<QString, QByteArray> parseSums(const QByteArray &sums);
// lower-case hex SHA-256 of a file (empty + *error when it cannot be read)
QByteArray sha256OfFile(const QString &path, QString *error = nullptr);

// --------------------------------------------------------------------------------- manifest
struct ManifestEntry {
    QString path;        // relative, '/' separated
    qint64 size = 0;
    QByteArray sha256;   // lower-case hex
};
struct Manifest {
    QString version;     // "4.3.0"
    QString platform;    // "win64" | "linux-x86_64" | "macos-universal"
    QList<ManifestEntry> files;
    bool contains(const QString &path) const;
    const ManifestEntry *find(const QString &path) const;
};
inline constexpr const char *kManifestName = "MANIFEST.txt";
inline constexpr const char *kJournalName = ".update-journal";
inline constexpr const char *kLockName = ".update-lock";
// a relative path the updater may move: no "..", no absolute or drive paths, no backslash, and not
// one of the names that belong to the user or to the updater itself (portable.txt, profile/,
// downloads/, incoming/, MANIFEST.txt, .old-*, .new-*, .update-*)
bool isSafeManifestPath(const QString &path);
bool parseManifest(const QByteArray &text, Manifest *out, QString *error = nullptr);
QByteArray manifestText(const Manifest &m);   // the format tools/release/manifest.sh writes
bool readManifest(const QString &dir, Manifest *out, QString *error = nullptr);
// every manifest file exists under dir with its size and SHA-256
bool verifyTree(const QString &dir, const Manifest &m, QString *error = nullptr);

// --------------------------------------------------------------------------------- archives
// A small zip reader over zlib (stored and deflated entries, CRC-32 checked; no zip64, no
// encryption). Refuses unsafe names and stops at maxTotalBytes unpacked.
bool unzip(const QString &zipPath, const QString &destDir, qint64 maxTotalBytes, QString *error = nullptr);
// tar.xz through the system tar (Linux)
bool untarXz(const QString &archive, const QString &destDir, QString *error = nullptr);
// by the file name: .zip -> unzip(), .tar.xz -> untarXz()
bool unpackArchive(const QString &archive, const QString &destDir, QString *error = nullptr);

// ---------------------------------------------------------------------------------- layout
QString platformTag();                                     // this build: "win64" | "linux-x86_64" | "macos-universal" [mac-1]
// ---- [ren-1] package naming: the ONE place that knows how our packages and program files are
// named (tools/release/* use the same names).
inline constexpr const char *kPackageBaseName = "OctoMX";
// "OctoMX-5.1-win64.zip" / "OctoMX-5.1-linux-x86_64.tar.xz" (major.minor, as tools/release.sh names them) /
// [mac-1] "OctoMX-5.1-macos-universal.zip" (a folder with OctoMX.app, tools/mac/package.sh)
QString archiveName(const QString &version, const QString &platform = platformTag());
// the top folder of the archive: "OctoMX-5.1-win64"
QString packageDirName(const QString &version, const QString &platform = platformTag());
// a release asset that is a package of this platform, any version ("OctoMX-<v>-<platform>.<ext>")
bool isPackageArchiveName(const QString &assetName, const QString &platform = platformTag());
// the program file inside the package folder: "octomx.exe" (Windows) / "bin/octomx" (Linux) /
// "OctoMX.app/Contents/MacOS/octomx" (macOS)
QString programFile(const QString &platform = platformTag());
// the start script at the top of the package folder: "" (Windows) / "octomx" (Linux)
QString launcherFile(const QString &platform = platformTag());

// ---- [upd-2] legacy starter files: a folder that came to 5.0 from a copy installed before 5.0 holds
// small starter files under the old program file names (the old package names were
// `WinMX-<v>-<platform>`, with `winmx.exe`, resp. `winmx` + `bin/winmx`) that start the program next to
// them. They are listed in the installed MANIFEST.txt; no package has them any more, and swapInstall()
// keeps them in place (see there). A 5.x only ever takes OctoMX-... assets (isPackageArchiveName()).
QStringList legacyStarterFiles(const QString &platform = platformTag());

struct Layout {
    bool ok = false;             // one of our packages, writable: an automatic install is possible
    bool ours = false;           // one of our packages (MANIFEST.txt, program files where expected)
    bool bridged = false;        // [upd-2] the installed manifest lists legacy starter files
    QString why;                 // why not (plain text for the user)
    QString programDir;          // the package folder (Windows: the exe's folder; Linux: bin/..)
    QString exe;                 // the file to start after the swap (octomx.exe / bin/octomx)
    Manifest manifest;           // the installed one
};
// the package folder for this program folder (Windows: appDir; Linux: appDir/.. when appDir is
// "bin"; macOS: the folder that holds OctoMX.app), empty when it cannot be one of ours; does not read
// anything. [mac-1] On macOS detectLayout() is never ok: the update box offers the download page.
QString programDirOf(const QString &appDir, const QString &platform = platformTag());
// appDir = QCoreApplication::applicationDirPath() of the running program
Layout detectLayout(const QString &appDir, const QString &platform = platformTag());
// can files be created, renamed and removed in dir (and every listed sub folder)?
bool dirWritable(const QString &dir, QString *error = nullptr);

// the unpacked package: one top folder packageDirName() (OctoMX-<X.Y>-<platform>/), its MANIFEST.txt for this version
// and platform, every listed file with the right size and hash, the program file valid (Windows: a
// x86-64 PE octomx.exe; Linux: the octomx launcher script and an x86-64 ELF bin/octomx, executable)
bool checkStagedTree(const QString &stageDir, const QString &version, const QString &platform, QString *topDir,
                     Manifest *manifest, QString *error = nullptr);
QString stageDirFor(const QString &programDir, const QString &version);   // <programDir>/.new-<version>
QString oldDirFor(const QString &programDir, const QString &version);     // <programDir>/.old-<version>

// ------------------------------------------------------------------------------------ swap
struct SwapPlan {
    QString programDir;
    QString stageTop;     // the unpacked package folder (inside stageDirFor())
    QString fromVersion;
    QString toVersion;
    Manifest oldManifest;
    Manifest newManifest;
};
// Moves the old files aside and the new ones in (journal first). On an error the swap is undone at
// once. crashAfterOps >= 0 stops after that many file operations without any clean-up, exactly as a
// killed process would leave the folder (tests). Returns true when the new version is in place.
// [upd-2] Legacy starter files (legacyStarterFiles()) that the old manifest lists and the new one does
// not are kept in place: they are left out of the journal's file lists, so neither the swap nor a
// roll-back or the recovery ever moves them (the journal names them on "k <path>" lines, for the
// record), and the ones still exactly as installed are appended to the new MANIFEST.txt after a
// "# kept" comment before the swap, so the installed manifest keeps listing them and the next update
// keeps them again. A starter file the user changed stays too, but is no longer listed (it is the
// user's file then).
bool swapInstall(const SwapPlan &plan, QString *error = nullptr, int crashAfterOps = -1);

struct Recovery {
    enum What { Nothing, Cleaned, Finished, RolledBack, Failed, Busy };
    What what = Nothing;
    QString from, to;     // versions of the journal
    QString error;
    int moved = 0;        // files moved by the recovery (0 for Nothing / Cleaned)
};
// At start: finish or undo an interrupted swap, remove the old files of a finished one (Cleaned) and
// any staging folder without a journal. crashAfterOps: as above (tests). Busy = another process
// holds the update lock.
Recovery recoverInstall(const QString &programDir, int crashAfterOps = -1);
// the journal of the folder, if any: state ("swapping" / "committed"), versions; kept = the files the
// swap keeps in place ([upd-2] legacy starter files)
bool readJournal(const QString &programDir, QString *state, QString *from, QString *to, QStringList *kept = nullptr);

} // namespace mx::upd
