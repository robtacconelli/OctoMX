// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX [upd-1]: signature, manifest, unpacking and the journalled file swap (see
// mx/update_install.h).
#include "mx/update_install.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QStandardPaths>
#include <QtCore/QtEndian>

#include <openssl/evp.h>
#include <zlib.h>

#include <memory>

#include "mx/update_check.h"

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mx::upd {

QList<PublicKey> builtinKeys();   // update_keys.cpp

// ======================================================================================= trust

namespace {
#ifdef MX_UPDATE_TEST_HOOKS
QList<PublicKey> &testKeys()
{
    static QList<PublicKey> k;
    return k;
}
#endif

struct PkeyFree {
    void operator()(EVP_PKEY *p) const { EVP_PKEY_free(p); }
};
struct MdFree {
    void operator()(EVP_MD_CTX *p) const { EVP_MD_CTX_free(p); }
};
} // namespace

QList<PublicKey> trustedKeys()
{
    QList<PublicKey> out = builtinKeys();
#ifdef MX_UPDATE_TEST_HOOKS
    // [ren-1] tests/update_e2e/e2e.sh --throwaway-key: packages built with MX_TEST_HOOKS=ON (tools/release.sh
    // --test-hooks, never published) also trust the throwaway key in MX_UPDATE_TEST_PUBKEY (64 hex digits)
    static const bool envKey = [] {
        const QByteArray hex = qgetenv("MX_UPDATE_TEST_PUBKEY").trimmed();
        const QByteArray raw = QByteArray::fromHex(hex);
        if (hex.size() == 64 && raw.size() == 32)
            addTestTrustedKey(raw);
        return true;
    }();
    Q_UNUSED(envKey)
    out << testKeys();
#endif
    return out;
}

QString keyIdOf(const QByteArray &raw32)
{
    return QString::fromLatin1(QCryptographicHash::hash(raw32, QCryptographicHash::Sha256).toHex().left(16));
}

bool testHooksCompiled()
{
#ifdef MX_UPDATE_TEST_HOOKS
    return true;
#else
    return false;
#endif
}

#ifdef MX_UPDATE_TEST_HOOKS
void addTestTrustedKey(const QByteArray &raw32)
{
    qWarning("octomx: TEST HOOK: trusting an extra update key %s", qPrintable(keyIdOf(raw32)));
    testKeys() << PublicKey{keyIdOf(raw32), raw32};
}

void clearTestTrustedKeys()
{
    testKeys().clear();
}

QByteArray testPublicKeyFromSeed(const QByteArray &seed32)
{
    std::unique_ptr<EVP_PKEY, PkeyFree> k(EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(seed32.constData()), size_t(seed32.size())));
    if (!k)
        return {};
    QByteArray pub(32, '\0');
    size_t n = 32;
    if (EVP_PKEY_get_raw_public_key(k.get(), reinterpret_cast<unsigned char *>(pub.data()), &n) != 1 || n != 32)
        return {};
    return pub;
}

QByteArray testSign(const QByteArray &seed32, const QByteArray &message)
{
    std::unique_ptr<EVP_PKEY, PkeyFree> k(EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(seed32.constData()), size_t(seed32.size())));
    std::unique_ptr<EVP_MD_CTX, MdFree> md(EVP_MD_CTX_new());
    if (!k || !md || EVP_DigestSignInit(md.get(), nullptr, nullptr, nullptr, k.get()) != 1)
        return {};
    QByteArray sig(64, '\0');
    size_t n = 64;
    if (EVP_DigestSign(md.get(), reinterpret_cast<unsigned char *>(sig.data()), &n,
                       reinterpret_cast<const unsigned char *>(message.constData()), size_t(message.size())) != 1 || n != 64)
        return {};
    return sig;
}
#endif

bool ed25519Verify(const QByteArray &publicKey32, const QByteArray &message, const QByteArray &signature64)
{
    if (publicKey32.size() != 32 || signature64.size() != 64)
        return false;
    std::unique_ptr<EVP_PKEY, PkeyFree> k(EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(publicKey32.constData()), 32));
    std::unique_ptr<EVP_MD_CTX, MdFree> md(EVP_MD_CTX_new());
    if (!k || !md || EVP_DigestVerifyInit(md.get(), nullptr, nullptr, nullptr, k.get()) != 1)
        return false;
    return EVP_DigestVerify(md.get(), reinterpret_cast<const unsigned char *>(signature64.constData()), 64,
                            reinterpret_cast<const unsigned char *>(message.constData()), size_t(message.size())) == 1;
}

namespace {
// format identifiers kept for compatibility (byte for byte): the header of SHA256SUMS.sig, which
// installed copies of every version check, and the header of the update journal, so a journal left
// by an earlier version is finished by this one and the other way round
const QByteArray kSignatureHeader = QByteArrayLiteral("winmx-ng-signature 1");
const QByteArray kJournalHeader = QByteArrayLiteral("winmx-ng-update-journal 1");
} // namespace

QByteArray formatSignatureFile(const QString &keyId, const QByteArray &signature64)
{
    return kSignatureHeader + "\nkey " + keyId.toLatin1() + "\ned25519 " + signature64.toBase64() + "\n";
}

bool parseSignatureFile(const QByteArray &text, QString *keyId, QByteArray *signature64, QString *error)
{
    auto fail = [error](const char *why) {
        if (error)
            *error = QString::fromLatin1(why);
        return false;
    };
    if (text.size() > 4096)
        return fail("the signature file is too large");
    QString id;
    QByteArray sig;
    bool header = false;
    for (QByteArray l : text.split('\n')) {
        l = l.trimmed();
        if (l.isEmpty() || l.startsWith('#'))
            continue;
        if (!header) {
            if (l != kSignatureHeader)
                return fail("not an OctoMX signature file");
            header = true;
        } else if (l.startsWith("key ")) {
            id = QString::fromLatin1(l.mid(4).trimmed());
        } else if (l.startsWith("ed25519 ")) {
            const auto r = QByteArray::fromBase64Encoding(l.mid(8).trimmed(), QByteArray::AbortOnBase64DecodingErrors);
            if (!r)
                return fail("the signature is not valid base64");
            sig = *r;
        }
    }
    if (!header)
        return fail("not an OctoMX signature file");
    if (sig.size() != 64)
        return fail("the signature has the wrong length");
    if (keyId)
        *keyId = id;
    if (signature64)
        *signature64 = sig;
    return true;
}

SigCheck verifySums(const QByteArray &sums, const QByteArray &sigFile)
{
    SigCheck r;
    QString id, why;
    QByteArray sig;
    if (!parseSignatureFile(sigFile, &id, &sig, &why)) {
        r.error = QStringLiteral("The release signature cannot be read (%1).").arg(why);
        return r;
    }
    const QList<PublicKey> keys = trustedKeys();
    bool known = false;
    for (const PublicKey &k : keys) {
        if (!id.isEmpty() && k.id != id)
            continue;
        known = true;
        if (ed25519Verify(k.raw, sums, sig)) {
            r.ok = true;
            r.keyId = k.id;
            return r;
        }
    }
    r.error = known ? QStringLiteral("The release signature does not match: the checksum list was changed or not signed by the OctoMX release key.")
                    : QStringLiteral("The release is signed with a key this version does not know (%1).").arg(id.left(32));
    return r;
}

QHash<QString, QByteArray> parseSums(const QByteArray &sums)
{
    static const QRegularExpression re(QStringLiteral("^([0-9a-fA-F]{64}) [ *](.+)$"));
    QHash<QString, QByteArray> out;
    for (const QByteArray &raw : sums.split('\n')) {
        const QRegularExpressionMatch m = re.match(QString::fromUtf8(raw).trimmed());
        if (m.hasMatch())
            out.insert(m.captured(2).trimmed(), m.captured(1).toLower().toLatin1());
    }
    return out;
}

QByteArray sha256OfFile(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot read %1 (%2)").arg(QDir::toNativeSeparators(path), f.errorString());
        return {};
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) {
        if (error)
            *error = QStringLiteral("cannot read %1").arg(QDir::toNativeSeparators(path));
        return {};
    }
    return h.result().toHex();
}

// ==================================================================================== manifest

bool Manifest::contains(const QString &path) const
{
    return find(path) != nullptr;
}

const ManifestEntry *Manifest::find(const QString &path) const
{
    for (const ManifestEntry &e : files)
        if (e.path == path)
            return &e;
    return nullptr;
}

bool isSafeManifestPath(const QString &path)
{
    if (path.isEmpty() || path.size() > 400 || path.startsWith(QLatin1Char('/')) || path.contains(QLatin1Char('\\')) ||
        path.contains(QLatin1Char(':')))
        return false;
    for (QChar c : path)
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return false;
    const QStringList parts = path.split(QLatin1Char('/'));
    for (const QString &p : parts)
        if (p.isEmpty() || p == QLatin1String(".") || p == QLatin1String("..") || p.endsWith(QLatin1Char(' ')) ||
            p.endsWith(QLatin1Char('.')))
            return false;
    const QString first = parts.first().toLower();
    static const QStringList reserved = {QStringLiteral("portable.txt"), QStringLiteral("profile"), QStringLiteral("downloads"),
                                         QStringLiteral("incoming"), QStringLiteral("manifest.txt")};
    if (reserved.contains(first))
        return false;
    if (first.startsWith(QLatin1String(".old-")) || first.startsWith(QLatin1String(".new-")) || first.startsWith(QLatin1String(".update-")))
        return false;
    return true;
}

bool parseManifest(const QByteArray &text, Manifest *out, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    static const QRegularExpression re(QStringLiteral("^([0-9a-f]{64}) (\\d{1,12}) (.+)$"));
    Manifest m;
    QSet<QString> seen;
    int lineNo = 0;
    for (const QByteArray &raw : text.split('\n')) {
        ++lineNo;
        QString l = QString::fromUtf8(raw);
        if (l.endsWith(QLatin1Char('\r')))
            l.chop(1);
        if (l.trimmed().isEmpty() || l.startsWith(QLatin1Char('#')))
            continue;
        if (l.startsWith(QLatin1String("version "))) {
            m.version = l.mid(8).trimmed();
            continue;
        }
        if (l.startsWith(QLatin1String("platform "))) {
            m.platform = l.mid(9).trimmed();
            continue;
        }
        const QRegularExpressionMatch mm = re.match(l);
        if (!mm.hasMatch())
            return fail(QStringLiteral("line %1 is not \"sha256 size path\"").arg(lineNo));
        ManifestEntry e;
        e.sha256 = mm.captured(1).toLatin1();
        e.size = mm.captured(2).toLongLong();
        e.path = mm.captured(3);
        if (!isSafeManifestPath(e.path))
            return fail(QStringLiteral("line %1: the path \"%2\" is not allowed").arg(lineNo).arg(e.path.left(80)));
        const QString key = e.path.toLower();   // Windows file names ignore case
        if (seen.contains(key))
            return fail(QStringLiteral("line %1: \"%2\" is listed twice").arg(lineNo).arg(e.path.left(80)));
        seen.insert(key);
        m.files << e;
    }
    if (parseVersion(m.version).isEmpty())
        return fail(QStringLiteral("no version line"));
    if (m.platform.isEmpty())
        return fail(QStringLiteral("no platform line"));
    if (m.files.isEmpty())
        return fail(QStringLiteral("no files"));
    *out = m;
    return true;
}

QByteArray manifestText(const Manifest &m)
{
    QByteArray t = "# OctoMX package manifest: sha256, size and path of every file of this package\n";
    t += "version " + m.version.toUtf8() + "\nplatform " + m.platform.toUtf8() + "\n";
    for (const ManifestEntry &e : m.files)
        t += e.sha256 + ' ' + QByteArray::number(e.size) + ' ' + e.path.toUtf8() + '\n';
    return t;
}

bool readManifest(const QString &dir, Manifest *out, QString *error)
{
    QFile f(QDir(dir).filePath(QString::fromLatin1(kManifestName)));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("no %1").arg(QString::fromLatin1(kManifestName));
        return false;
    }
    if (f.size() > 4 * 1024 * 1024) {
        if (error)
            *error = QStringLiteral("%1 is too large").arg(QString::fromLatin1(kManifestName));
        return false;
    }
    return parseManifest(f.readAll(), out, error);
}

bool verifyTree(const QString &dir, const Manifest &m, QString *error)
{
    const QDir d(dir);
    for (const ManifestEntry &e : m.files) {
        const QFileInfo fi(d.filePath(e.path));
        if (!fi.isFile() || fi.isSymLink()) {
            if (error)
                *error = QStringLiteral("%1 is missing").arg(e.path);
            return false;
        }
        if (fi.size() != e.size) {
            if (error)
                *error = QStringLiteral("%1 has the wrong size (%2, expected %3)").arg(e.path).arg(fi.size()).arg(e.size);
            return false;
        }
        QString why;
        const QByteArray h = sha256OfFile(fi.filePath(), &why);
        if (h != e.sha256) {
            if (error)
                *error = h.isEmpty() ? why : QStringLiteral("%1 has the wrong checksum").arg(e.path);
            return false;
        }
    }
    return true;
}

// ==================================================================================== archives

namespace {

quint16 le16(const uchar *p)
{
    return qFromLittleEndian<quint16>(p);
}
quint32 le32(const uchar *p)
{
    return qFromLittleEndian<quint32>(p);
}

bool safeArchiveName(const QString &name)
{
    QString n = name;
    if (n.endsWith(QLatin1Char('/')))
        n.chop(1);
    if (n.isEmpty() || n.startsWith(QLatin1Char('/')) || n.contains(QLatin1Char('\\')) || n.contains(QLatin1Char(':')))
        return false;
    for (const QString &p : n.split(QLatin1Char('/')))
        if (p.isEmpty() || p == QLatin1String(".") || p == QLatin1String(".."))
            return false;
    return true;
}

} // namespace

bool unzip(const QString &zipPath, const QString &destDir, qint64 maxTotalBytes, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    QFile f(zipPath);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open the archive (%1)").arg(f.errorString()));
    const qint64 size = f.size();
    if (size < 22)
        return fail(QStringLiteral("not a zip archive"));
    // end of central directory: the last 22 bytes + up to 64 KiB of comment
    const qint64 tailLen = qMin<qint64>(size, 22 + 65535);
    f.seek(size - tailLen);
    const QByteArray tail = f.read(tailLen);
    qint64 eocd = -1;
    for (qint64 i = tail.size() - 22; i >= 0; --i)
        if (le32(reinterpret_cast<const uchar *>(tail.constData()) + i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    if (eocd < 0)
        return fail(QStringLiteral("not a zip archive (no central directory)"));
    const uchar *e = reinterpret_cast<const uchar *>(tail.constData()) + eocd;
    const quint16 entries = le16(e + 10);
    const quint32 cdSize = le32(e + 12), cdOff = le32(e + 16);
    if (entries == 0xffff || cdSize == 0xffffffffu || cdOff == 0xffffffffu)
        return fail(QStringLiteral("zip64 archives are not supported"));
    if (qint64(cdOff) + cdSize > size || cdSize > 16 * 1024 * 1024)
        return fail(QStringLiteral("the zip central directory is damaged"));
    f.seek(cdOff);
    const QByteArray cd = f.read(cdSize);
    if (cd.size() != qint64(cdSize))
        return fail(QStringLiteral("the zip central directory is damaged"));
    const QDir dest(destDir);
    if (!QDir().mkpath(destDir))
        return fail(QStringLiteral("cannot create %1").arg(QDir::toNativeSeparators(destDir)));
    qint64 total = 0;
    qsizetype pos = 0;
    const uchar *c = reinterpret_cast<const uchar *>(cd.constData());
    QByteArray in(256 * 1024, Qt::Uninitialized), outBuf(256 * 1024, Qt::Uninitialized);
    for (int n = 0; n < entries; ++n) {
        if (pos + 46 > cd.size() || le32(c + pos) != 0x02014b50u)
            return fail(QStringLiteral("the zip central directory is damaged"));
        const quint16 madeBy = le16(c + pos + 4), flags = le16(c + pos + 8), method = le16(c + pos + 10);
        const quint32 crc = le32(c + pos + 16), csize = le32(c + pos + 20), usize = le32(c + pos + 24);
        const quint16 nameLen = le16(c + pos + 28), extraLen = le16(c + pos + 30), commentLen = le16(c + pos + 32);
        const quint32 extAttr = le32(c + pos + 38), lho = le32(c + pos + 42);
        if (pos + 46 + nameLen > cd.size())
            return fail(QStringLiteral("the zip central directory is damaged"));
        const QByteArray rawName = cd.mid(pos + 46, nameLen);
        pos += 46 + nameLen + extraLen + commentLen;
        const QString name = (flags & 0x800) ? QString::fromUtf8(rawName) : QString::fromLatin1(rawName);
        if (flags & 0x1)
            return fail(QStringLiteral("encrypted zip entries are not supported"));
        if (csize == 0xffffffffu || usize == 0xffffffffu || lho == 0xffffffffu)
            return fail(QStringLiteral("zip64 archives are not supported"));
        if (!safeArchiveName(name))
            return fail(QStringLiteral("the archive contains an unsafe name: %1").arg(name.left(80)));
        if ((madeBy >> 8) == 3 && ((extAttr >> 16) & 0170000) == 0120000)
            return fail(QStringLiteral("the archive contains a symbolic link: %1").arg(name.left(80)));
        if (name.endsWith(QLatin1Char('/'))) {
            if (!dest.mkpath(name))
                return fail(QStringLiteral("cannot create %1").arg(name));
            continue;
        }
        total += usize;
        if (total > maxTotalBytes)
            return fail(QStringLiteral("the archive unpacks to more than %1 MB").arg(maxTotalBytes / (1024 * 1024)));
        if (method != 0 && method != 8)
            return fail(QStringLiteral("unsupported zip compression method %1 (%2)").arg(method).arg(name.left(80)));
        // local header: its own name / extra lengths give the data offset
        if (!f.seek(lho))
            return fail(QStringLiteral("the zip archive is damaged"));
        const QByteArray lh = f.read(30);
        if (lh.size() != 30 || le32(reinterpret_cast<const uchar *>(lh.constData())) != 0x04034b50u)
            return fail(QStringLiteral("the zip archive is damaged (%1)").arg(name.left(80)));
        const qint64 dataOff = qint64(lho) + 30 + le16(reinterpret_cast<const uchar *>(lh.constData()) + 26) +
                               le16(reinterpret_cast<const uchar *>(lh.constData()) + 28);
        if (dataOff + csize > size || !f.seek(dataOff))
            return fail(QStringLiteral("the zip archive is damaged (%1)").arg(name.left(80)));
        const QString outPath = dest.filePath(name);
        if (!QDir().mkpath(QFileInfo(outPath).absolutePath()))
            return fail(QStringLiteral("cannot create the folder of %1").arg(name));
        QFile out(outPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail(QStringLiteral("cannot write %1 (%2)").arg(name, out.errorString()));
        uLong crcNow = crc32(0L, Z_NULL, 0);
        qint64 written = 0;
        quint32 left = csize;
        if (method == 0) {
            if (csize != usize)
                return fail(QStringLiteral("the zip archive is damaged (%1)").arg(name.left(80)));
            while (left > 0) {
                const qint64 got = f.read(in.data(), qMin<qint64>(left, in.size()));
                if (got <= 0)
                    return fail(QStringLiteral("the zip archive is truncated"));
                left -= quint32(got);
                crcNow = crc32(crcNow, reinterpret_cast<const Bytef *>(in.constData()), uInt(got));
                if (out.write(in.constData(), got) != got)
                    return fail(QStringLiteral("cannot write %1 (%2)").arg(name, out.errorString()));
                written += got;
            }
        } else {
            z_stream zs{};
            if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
                return fail(QStringLiteral("zlib error"));
            std::unique_ptr<z_stream, void (*)(z_stream *)> guard(&zs, [](z_stream *z) { inflateEnd(z); });
            int zr = Z_OK;
            while (zr != Z_STREAM_END) {
                if (zs.avail_in == 0) {
                    if (left == 0)
                        return fail(QStringLiteral("the zip entry %1 is truncated").arg(name.left(80)));
                    const qint64 got = f.read(in.data(), qMin<qint64>(left, in.size()));
                    if (got <= 0)
                        return fail(QStringLiteral("the zip archive is truncated"));
                    left -= quint32(got);
                    zs.next_in = reinterpret_cast<Bytef *>(in.data());
                    zs.avail_in = uInt(got);
                }
                zs.next_out = reinterpret_cast<Bytef *>(outBuf.data());
                zs.avail_out = uInt(outBuf.size());
                zr = inflate(&zs, Z_NO_FLUSH);
                if (zr != Z_OK && zr != Z_STREAM_END)
                    return fail(QStringLiteral("the zip entry %1 is damaged (zlib %2)").arg(name.left(80)).arg(zr));
                const qint64 produced = outBuf.size() - zs.avail_out;
                written += produced;
                if (written > qint64(usize))
                    return fail(QStringLiteral("the zip entry %1 is larger than stated").arg(name.left(80)));
                crcNow = crc32(crcNow, reinterpret_cast<const Bytef *>(outBuf.constData()), uInt(produced));
                if (produced && out.write(outBuf.constData(), produced) != produced)
                    return fail(QStringLiteral("cannot write %1 (%2)").arg(name, out.errorString()));
            }
        }
        if (written != qint64(usize) || crcNow != crc)
            return fail(QStringLiteral("the zip entry %1 is damaged (size or CRC)").arg(name.left(80)));
        if (!out.flush())
            return fail(QStringLiteral("cannot write %1 (%2)").arg(name, out.errorString()));
        out.close();
        // a Unix-made zip keeps the executable bit
        if ((madeBy >> 8) == 3 && ((extAttr >> 16) & 0111))
            out.setPermissions(out.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);
    }
    return true;
}

bool untarXz(const QString &archive, const QString &destDir, QString *error)
{
    const QString tar = QStandardPaths::findExecutable(QStringLiteral("tar"));
    if (tar.isEmpty()) {
        if (error)
            *error = QStringLiteral("the program tar is not installed");
        return false;
    }
    if (!QDir().mkpath(destDir)) {
        if (error)
            *error = QStringLiteral("cannot create %1").arg(destDir);
        return false;
    }
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    // GNU tar and bsdtar both refuse member names with ".." and strip a leading "/"
    p.start(tar, {QStringLiteral("-xJf"), archive, QStringLiteral("-C"), destDir, QStringLiteral("--no-same-owner")});
    if (!p.waitForStarted(10000) || !p.waitForFinished(10 * 60 * 1000) || p.exitStatus() != QProcess::NormalExit ||
        p.exitCode() != 0) {
        if (error)
            *error = QStringLiteral("tar could not unpack the archive (%1)").arg(QString::fromLocal8Bit(p.readAll()).trimmed().left(200));
        if (p.state() != QProcess::NotRunning) {
            p.kill();
            p.waitForFinished(2000);
        }
        return false;
    }
    return true;
}

bool unpackArchive(const QString &archive, const QString &destDir, QString *error)
{
    if (archive.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive))
        return unzip(archive, destDir, qint64(2) * 1024 * 1024 * 1024, error);
    if (archive.endsWith(QLatin1String(".tar.xz"), Qt::CaseInsensitive))
        return untarXz(archive, destDir, error);
    if (error)
        *error = QStringLiteral("unknown archive type");
    return false;
}

// ====================================================================================== layout

QString platformTag()
{
#ifdef Q_OS_WIN
    return QStringLiteral("win64");
#else
    return QStringLiteral("linux-x86_64");
#endif
}

QString packageDirName(const QString &version, const QString &platform)
{
    QList<int> v = parseVersion(version);
    while (v.size() < 2)
        v << 0;
    return QStringLiteral("%1-%2.%3-%4").arg(QLatin1String(kPackageBaseName)).arg(v[0]).arg(v[1]).arg(platform);
}

bool isPackageArchiveName(const QString &assetName, const QString &platform)
{
    const QString ext = platform == QLatin1String("win64") ? QStringLiteral("\\.zip") : QStringLiteral("\\.tar\\.xz");
    const QRegularExpression re(QStringLiteral("^%1-[0-9.]+-%2%3$")
                                    .arg(QRegularExpression::escape(QLatin1String(kPackageBaseName)), QRegularExpression::escape(platform), ext));
    return re.match(assetName).hasMatch();
}

QString programFile(const QString &platform)
{
    return platform == QLatin1String("win64") ? QStringLiteral("octomx.exe") : QStringLiteral("bin/octomx");
}

QString launcherFile(const QString &platform)
{
    return platform == QLatin1String("win64") ? QString() : QStringLiteral("octomx");
}

QString archiveName(const QString &version, const QString &platform)
{
    return packageDirName(version, platform) + (platform == QLatin1String("win64") ? QStringLiteral(".zip") : QStringLiteral(".tar.xz"));
}

// [upd-2] the legacy program file names (pre-5.0), kept in a folder that has them (see mx/update_install.h)
QStringList legacyStarterFiles(const QString &platform)
{
    if (platform == QLatin1String("win64"))
        return {QStringLiteral("winmx.exe")};
    return {QStringLiteral("winmx"), QStringLiteral("bin/winmx")};
}

QString stageDirFor(const QString &programDir, const QString &version)
{
    return QDir(programDir).filePath(QStringLiteral(".new-") + version);
}

QString oldDirFor(const QString &programDir, const QString &version)
{
    return QDir(programDir).filePath(QStringLiteral(".old-") + version);
}

bool dirWritable(const QString &dir, QString *error)
{
    const QString a = QDir(dir).filePath(QStringLiteral(".update-probe-%1").arg(QCoreApplication::applicationPid()));
    const QString b = a + QStringLiteral(".2");
    QFile::remove(a);
    QFile::remove(b);
    QFile f(a);
    bool ok = f.open(QIODevice::WriteOnly) && f.write("x", 1) == 1;
    f.close();
    ok = ok && QFile::rename(a, b) && QFile::remove(b);
    if (!ok) {
        QFile::remove(a);
        QFile::remove(b);
        if (error)
            *error = QStringLiteral("OctoMX cannot write to %1").arg(QDir::toNativeSeparators(dir));
    }
    return ok;
}

namespace {

bool isPe64(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray h = f.read(4096);
    if (h.size() < 64 || !h.startsWith("MZ"))
        return false;
    const quint32 off = le32(reinterpret_cast<const uchar *>(h.constData()) + 0x3c);
    if (off + 6 > quint32(h.size()))
        return false;
    return h.mid(off, 4) == QByteArray("PE\0\0", 4) && le16(reinterpret_cast<const uchar *>(h.constData()) + off + 4) == 0x8664;
}

bool isElf64(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray h = f.read(64);
    return h.size() >= 20 && h.startsWith("\x7f" "ELF") && h.at(4) == 2 && le16(reinterpret_cast<const uchar *>(h.constData()) + 18) == 62;
}

bool startsWithShebang(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) && f.read(2) == "#!";
}


} // namespace

QString programDirOf(const QString &appDir, const QString &platform)
{
    const QString d = QDir::cleanPath(QFileInfo(appDir).absoluteFilePath());
    if (platform == QLatin1String("win64"))
        return d;
    if (QFileInfo(d).fileName() != QLatin1String("bin"))
        return QString();
    return QDir::cleanPath(d + QStringLiteral("/.."));
}

Layout detectLayout(const QString &appDir, const QString &platform)
{
    Layout l;
    const bool win = platform == QLatin1String("win64");
    QString prog = QDir::cleanPath(QFileInfo(appDir).absoluteFilePath());
    if (!win) {
        if (QFileInfo(prog).fileName() != QLatin1String("bin")) {
            l.why = QStringLiteral("This copy of OctoMX is not one of the published packages (for example a developer build or a "
                                   "distribution package), so it is not updated automatically.");
            return l;
        }
        prog = QDir::cleanPath(prog + QStringLiteral("/.."));
    }
    l.programDir = prog;
    l.exe = QDir(prog).filePath(programFile(platform));
    QString why;
    if (!readManifest(prog, &l.manifest, &why)) {
        l.why = QStringLiteral("This copy of OctoMX is not one of the published packages (%1: for example a developer build or a "
                               "distribution package), so it is not updated automatically.").arg(why);
        return l;
    }
    if (l.manifest.platform != platform || !l.manifest.contains(programFile(platform)) || !QFileInfo::exists(l.exe) ||
        (!win && !l.manifest.contains(launcherFile(platform)))) {
        l.why = QStringLiteral("The folder of this OctoMX does not look like a published %1 package, so it is not updated automatically.")
                    .arg(platform);
        return l;
    }
    l.ours = true;
    for (const QString &f : legacyStarterFiles(platform))
        l.bridged = l.bridged || l.manifest.contains(f);
    // every folder the swap touches must be writable
    QSet<QString> dirs{prog};
    for (const ManifestEntry &e : std::as_const(l.manifest.files)) {
        const QString d = QFileInfo(QDir(prog).filePath(e.path)).absolutePath();
        if (QFileInfo(d).isDir())
            dirs.insert(QDir::cleanPath(d));
    }
    for (const QString &d : std::as_const(dirs)) {
        if (!dirWritable(d)) {
            l.why = QStringLiteral("OctoMX cannot write to its folder %1 (for example Program Files without administrator rights, a "
                                   "read-only drive or a folder owned by another user), so the update cannot be installed automatically.")
                        .arg(QDir::toNativeSeparators(d));
            return l;
        }
    }
    l.ok = true;
    return l;
}

bool checkStagedTree(const QString &stageDir, const QString &version, const QString &platform, QString *topDir, Manifest *manifest,
                     QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    const QStringList entries = QDir(stageDir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    const QString want = packageDirName(version, platform);
    if (entries != QStringList{want})
        return fail(QStringLiteral("the archive does not hold exactly one folder %1 (it holds: %2)").arg(want, entries.join(QStringLiteral(", ")).left(200)));
    const QString top = QDir(stageDir).filePath(want);
    if (!QFileInfo(top).isDir() || QFileInfo(top).isSymLink())
        return fail(QStringLiteral("%1 is not a folder").arg(want));
    Manifest m;
    QString why;
    if (!readManifest(top, &m, &why))
        return fail(QStringLiteral("the new package has no valid manifest (%1)").arg(why));
    if (compareVersions(m.version, version) != 0)
        return fail(QStringLiteral("the new package says it is version %1, not %2").arg(m.version, version));
    if (m.platform != platform)
        return fail(QStringLiteral("the new package is for %1, not %2").arg(m.platform, platform));
    QDirIterator it(top, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (it.fileInfo().isSymLink())
            return fail(QStringLiteral("the new package contains a symbolic link (%1)").arg(it.fileInfo().fileName()));
    }
    if (!verifyTree(top, m, &why))
        return fail(QStringLiteral("the new package is incomplete or damaged: %1").arg(why));
    const QString exe = QDir(top).filePath(programFile(platform));
    if (!m.contains(programFile(platform)))
        return fail(QStringLiteral("the new package has no %1").arg(programFile(platform)));
    if (platform == QLatin1String("win64")) {
        if (!isPe64(exe))
            return fail(QStringLiteral("%1 of the new package is not a 64-bit Windows program").arg(programFile(platform)));
    } else {
        const QString launcher = QDir(top).filePath(launcherFile(platform));
        if (!m.contains(launcherFile(platform)) || !startsWithShebang(launcher) || !QFileInfo(launcher).isExecutable())
            return fail(QStringLiteral("the new package has no %1 launcher").arg(launcherFile(platform)));
        if (!isElf64(exe) || !QFileInfo(exe).isExecutable())
            return fail(QStringLiteral("%1 of the new package is not a 64-bit Linux program").arg(programFile(platform)));
    }
    if (topDir)
        *topDir = top;
    if (manifest)
        *manifest = m;
    return true;
}

// ======================================================================================== swap

namespace {

QString nativeError()
{
#ifdef Q_OS_WIN
    return QStringLiteral("Windows error %1").arg(GetLastError());
#else
    return QString::fromLocal8Bit(strerror(errno));
#endif
}

// rename within one volume; replace = the target may exist and is replaced atomically (POSIX), or
// replaced by MoveFileEx (Windows; fails for a running program file, which is renamed aside first)
bool moveFile(const QString &from, const QString &to, bool replace, QString *error)
{
    QDir().mkpath(QFileInfo(to).absolutePath());
#ifdef Q_OS_WIN
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (MoveFileExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(from).utf16()),
                    reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(to).utf16()), flags))
        return true;
#else
    if (!replace && QFileInfo::exists(to)) {
        if (error)
            *error = QStringLiteral("%1 exists").arg(to);
        return false;
    }
    if (::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0)
        return true;
#endif
    if (error)
        *error = QStringLiteral("cannot move %1 to %2 (%3)").arg(QDir::toNativeSeparators(from), QDir::toNativeSeparators(to), nativeError());
    return false;
}

// keep the old file at `backup` while it stays in place: a hard link on POSIX (the live path is then
// replaced atomically and is never missing); Windows (and file systems without hard links): renamed
bool backupLive(const QString &live, const QString &backup, QString *error)
{
    QDir().mkpath(QFileInfo(backup).absolutePath());
#ifndef Q_OS_WIN
    if (::link(QFile::encodeName(live).constData(), QFile::encodeName(backup).constData()) == 0)
        return true;
#endif
    return moveFile(live, backup, false, error);
}

bool exists(const QString &p)
{
    return QFileInfo::exists(p);
}

// two names of one file (the hard link backupLive() made on POSIX)
bool sameFile(const QString &a, const QString &b)
{
#ifdef Q_OS_WIN
    Q_UNUSED(a);
    Q_UNUSED(b);
    return false;   // no hard links on Windows: the backup is always the renamed old file
#else
    struct stat sa, sb;
    return ::stat(QFile::encodeName(a).constData(), &sa) == 0 && ::stat(QFile::encodeName(b).constData(), &sb) == 0 &&
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#endif
}

void removeEmptyParents(const QString &root, const QString &rel)
{
    QString d = QFileInfo(QDir(root).filePath(rel)).absolutePath();
    const QString r = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    while (QDir::cleanPath(d).startsWith(r + QLatin1Char('/')) && QDir::cleanPath(d) != r) {
        if (!QDir(d).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            break;
        if (!QDir().rmdir(d))
            break;
        d = QFileInfo(d).absolutePath();
    }
}

struct Journal {
    QString state;   // swapping | committed
    QString from, to;
    QString stage;   // relative to the program dir
    QString old;     // relative to the program dir
    QStringList oldFiles, newFiles;
    QStringList kept;   // [upd-2] legacy starter files the swap keeps in place (never moved, only recorded)
};

bool writeJournal(const QString &programDir, const Journal &j, QString *error)
{
    QSaveFile f(QDir(programDir).filePath(QString::fromLatin1(kJournalName)));
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("cannot write the update journal (%1)").arg(f.errorString());
        return false;
    }
    QByteArray t = kJournalHeader + '\n';
    t += "state " + j.state.toUtf8() + "\nfrom " + j.from.toUtf8() + "\nto " + j.to.toUtf8() + "\n";
    t += "stage " + j.stage.toUtf8() + "\nold " + j.old.toUtf8() + "\n";
    for (const QString &p : j.oldFiles)
        t += "o " + p.toUtf8() + '\n';
    for (const QString &p : j.newFiles)
        t += "n " + p.toUtf8() + '\n';
    for (const QString &p : j.kept)   // [upd-2] ignored by earlier versions and by this code's swap and roll-back
        t += "k " + p.toUtf8() + '\n';
    f.write(t);
    if (!f.commit()) {
        if (error)
            *error = QStringLiteral("cannot write the update journal (%1)").arg(f.errorString());
        return false;
    }
    return true;
}

bool loadJournal(const QString &programDir, Journal *j)
{
    QFile f(QDir(programDir).filePath(QString::fromLatin1(kJournalName)));
    if (!f.open(QIODevice::ReadOnly) || f.size() > 8 * 1024 * 1024)
        return false;
    const QList<QByteArray> lines = f.readAll().split('\n');
    if (lines.isEmpty() || lines.first().trimmed() != kJournalHeader)
        return false;
    auto okPath = [](const QString &p) { return p == QLatin1String(kManifestName) || isSafeManifestPath(p); };
    auto okDir = [](const QString &p, const char *prefix) {
        return p.startsWith(QLatin1String(prefix)) && !p.contains(QLatin1String("..")) && !p.startsWith(QLatin1Char('/')) &&
               !p.contains(QLatin1Char('\\')) && !p.contains(QLatin1Char(':'));
    };
    for (const QByteArray &raw : lines.mid(1)) {
        const QString l = QString::fromUtf8(raw);
        if (l.startsWith(QLatin1String("state ")))
            j->state = l.mid(6).trimmed();
        else if (l.startsWith(QLatin1String("from ")))
            j->from = l.mid(5).trimmed();
        else if (l.startsWith(QLatin1String("to ")))
            j->to = l.mid(3).trimmed();
        else if (l.startsWith(QLatin1String("stage ")))
            j->stage = l.mid(6).trimmed();
        else if (l.startsWith(QLatin1String("old ")))
            j->old = l.mid(4).trimmed();
        else if (l.startsWith(QLatin1String("o ")) && okPath(l.mid(2)))
            j->oldFiles << l.mid(2);
        else if (l.startsWith(QLatin1String("n ")) && okPath(l.mid(2)))
            j->newFiles << l.mid(2);
        else if (l.startsWith(QLatin1String("k ")) && isSafeManifestPath(l.mid(2)))
            j->kept << l.mid(2);
    }
    return (j->state == QLatin1String("swapping") || j->state == QLatin1String("committed")) && okDir(j->stage, ".new-") &&
           okDir(j->old, ".old-") && !j->newFiles.isEmpty();
}

// the order of the swap: the new files (the program file last), then the old files that the new
// version no longer has
QStringList newOrder(QStringList files, const QString &exeRel)
{
    files.removeAll(exeRel);
    files.sort();
    files << exeRel;
    return files;
}

struct Swapper {
    QString prog, stage, old;
    QSet<QString> oldSet;
    QStringList newFiles, oldOnly;
    int crashAfter = -1;
    int ops = 0;
    bool crashed = false;
    QString error;

    QString live(const QString &f) const { return QDir(prog).filePath(f); }
    QString staged(const QString &f) const { return QDir(stage).filePath(f); }
    QString backup(const QString &f) const { return QDir(old).filePath(f); }
    // one counted file operation; false = the simulated crash point is reached
    bool tick()
    {
        if (crashAfter >= 0 && ops >= crashAfter) {
            crashed = true;
            return false;
        }
        ++ops;
        return true;
    }

    // roll forward: idempotent, continues wherever an earlier run stopped
    bool forward()
    {
        for (const QString &f : std::as_const(newFiles)) {
            if (!exists(staged(f)))
                continue;   // in place already
            const bool wasOurs = oldSet.contains(f);
            if (exists(live(f))) {
                if (!wasOurs) {
                    error = QStringLiteral("the file %1 is in the way and does not belong to OctoMX").arg(f);
                    return false;
                }
                if (!exists(backup(f))) {
                    if (!tick() || !backupLive(live(f), backup(f), &error))
                        return false;
                }
            }
            if (!tick() || !moveFile(staged(f), live(f), true, &error))
                return false;
        }
        for (const QString &g : std::as_const(oldOnly)) {
            if (!exists(live(g)))
                continue;
            if (exists(backup(g))) {   // a hard link made earlier: the backup is the same file
                if (!tick())
                    return false;
                if (!QFile::remove(live(g))) {
                    error = QStringLiteral("cannot remove %1").arg(g);
                    return false;
                }
            } else if (!tick() || !moveFile(live(g), backup(g), false, &error)) {
                return false;
            }
            removeEmptyParents(prog, g);
        }
        return true;
    }

    // roll back: every new file back to the staging folder, every old file back in place
    bool backward()
    {
        for (const QString &g : std::as_const(oldOnly)) {
            if (exists(backup(g)) && !exists(live(g)))
                if (!tick() || !moveFile(backup(g), live(g), false, &error))
                    return false;
        }
        for (auto it = newFiles.crbegin(); it != newFiles.crend(); ++it) {
            const QString &f = *it;
            const bool wasOurs = oldSet.contains(f);
            // the live file is the new one when it was put in place: staged gone and (for a file the
            // old version had too) its backup made; otherwise it is still the old one
            if (!exists(staged(f)) && exists(live(f)) && (!wasOurs || (exists(backup(f)) && !sameFile(live(f), backup(f))))) {
                if (!tick() || !moveFile(live(f), staged(f), false, &error))
                    return false;
            }
            if (wasOurs && exists(backup(f))) {
                if (!exists(live(f))) {
                    if (!tick() || !moveFile(backup(f), live(f), false, &error))
                        return false;
                } else {
                    // the old file is still in place and the backup is its hard link
                    if (!tick())
                        return false;
                    QFile::remove(backup(f));
                }
            }
            if (!wasOurs)
                removeEmptyParents(prog, f);
        }
        return true;
    }

    // every new file is either in place or still staged (in place = staged gone, the live file there
    // and, for a file the old version had too, the old one kept in the backup folder)
    bool canForward() const
    {
        for (const QString &f : newFiles) {
            if (exists(staged(f)))
                continue;
            if (!exists(live(f)) || (oldSet.contains(f) && (!exists(backup(f)) || sameFile(live(f), backup(f)))))
                return false;
        }
        return true;
    }
};

Swapper makeSwapper(const QString &prog, const Journal &j, int crashAfter)
{
    Swapper s;
    s.prog = prog;
    s.stage = QDir(prog).filePath(j.stage);
    s.old = QDir(prog).filePath(j.old);
    s.oldSet = QSet<QString>(j.oldFiles.cbegin(), j.oldFiles.cend());
    s.newFiles = j.newFiles;
    const QSet<QString> newSet(j.newFiles.cbegin(), j.newFiles.cend());
    for (const QString &o : j.oldFiles)
        if (!newSet.contains(o))
            s.oldOnly << o;
    s.crashAfter = crashAfter;
    return s;
}

void cleanupAfter(const QString &prog, const Journal &j)
{
    QDir(QDir(prog).filePath(j.old)).removeRecursively();
    // the staging folder (.new-<version>), not only the package folder inside it
    const QString stageRoot = QDir(prog).filePath(j.stage.section(QLatin1Char('/'), 0, 0));
    QDir(stageRoot).removeRecursively();
}

} // namespace

bool swapInstall(const SwapPlan &plan, QString *error, int crashAfterOps)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    const QString prog = QDir::cleanPath(plan.programDir);
    QLockFile lock(QDir(prog).filePath(QString::fromLatin1(kLockName)));
    if (!lock.tryLock(3000))
        return fail(QStringLiteral("another OctoMX is updating this folder"));
    const QString platform = plan.newManifest.platform;
    const QString exeRel = programFile(platform);
    QString stageRel = QDir(prog).relativeFilePath(plan.stageTop);
    if (!stageRel.startsWith(QLatin1String(".new-")) || stageRel.contains(QLatin1String("..")))
        return fail(QStringLiteral("the staging folder is not inside the program folder"));
    Journal j;
    j.state = QStringLiteral("swapping");
    j.from = plan.fromVersion;
    j.to = plan.toVersion;
    j.stage = stageRel;
    j.old = QDir(prog).relativeFilePath(oldDirFor(prog, plan.fromVersion));
    // [upd-2] legacy starter files that the new package does not have: kept where they are, never moved
    // (not in the journal's file lists); those still exactly as installed stay listed in the installed
    // manifest
    QList<ManifestEntry> keptEntries;
    for (const QString &f : legacyStarterFiles(platform)) {
        const ManifestEntry *e = plan.oldManifest.find(f);
        if (!e || plan.newManifest.contains(f))
            continue;
        j.kept << f;
        const QString live = QDir(prog).filePath(f);
        const QFileInfo fi(live);
        if (fi.isFile() && !fi.isSymLink() && fi.size() == e->size && sha256OfFile(live) == e->sha256)
            keptEntries << *e;
    }
    for (const ManifestEntry &e : plan.oldManifest.files)
        if (!j.kept.contains(e.path))
            j.oldFiles << e.path;
    j.oldFiles << QString::fromLatin1(kManifestName);
    QStringList nf;
    for (const ManifestEntry &e : plan.newManifest.files)
        nf << e.path;
    j.newFiles = newOrder(nf, exeRel);
    j.newFiles.insert(j.newFiles.size() - 1, QString::fromLatin1(kManifestName));   // the manifest just before the program
    // preflight: nothing is changed when any of this fails
    const QSet<QString> oldSet(j.oldFiles.cbegin(), j.oldFiles.cend());
    for (const QString &f : std::as_const(j.newFiles)) {
        if (!exists(QDir(plan.stageTop).filePath(f)))
            return fail(QStringLiteral("the staged file %1 is missing").arg(f));
        if (!oldSet.contains(f) && exists(QDir(prog).filePath(f)))
            return fail(QStringLiteral("the file %1 is in the way: it is not part of the installed OctoMX, so it is left alone and the "
                                       "update is not installed").arg(QDir::toNativeSeparators(f)));
    }
    if (exists(QDir(prog).filePath(QString::fromLatin1(kJournalName))))
        return fail(QStringLiteral("an earlier update of this folder is not finished"));
    const QString oldDir = QDir(prog).filePath(j.old);
    if (exists(oldDir) && !QDir(oldDir).removeRecursively())
        return fail(QStringLiteral("cannot remove the old files of an earlier update (%1)").arg(QDir::toNativeSeparators(oldDir)));
    QString why;
    if (!keptEntries.isEmpty()) {
        // the new MANIFEST.txt (still staged) goes on listing the kept starter files: "what the installed
        // package owns" stays true for the next update and for --verify-manifest
        const QString stagedManifest = QDir(plan.stageTop).filePath(QString::fromLatin1(kManifestName));
        QFile in(stagedManifest);
        if (!in.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("cannot read the new %1").arg(QString::fromLatin1(kManifestName)));
        QByteArray t = in.readAll();
        in.close();
        if (!t.endsWith('\n'))
            t += '\n';
        t += "# kept in place from " + plan.fromVersion.toUtf8() +
             " (starter files under the legacy program names, not part of this package):\n";
        QStringList names;
        for (const ManifestEntry &e : std::as_const(keptEntries)) {
            t += e.sha256 + ' ' + QByteArray::number(e.size) + ' ' + e.path.toUtf8() + '\n';
            names << e.path;
        }
        Manifest check;
        if (!parseManifest(t, &check, &why))
            return fail(QStringLiteral("the new %1 cannot list the kept files (%2)").arg(QString::fromLatin1(kManifestName), why));
        QSaveFile out(stagedManifest);
        if (!out.open(QIODevice::WriteOnly) || out.write(t) != t.size() || !out.commit())
            return fail(QStringLiteral("cannot write the new %1 (%2)").arg(QString::fromLatin1(kManifestName), out.errorString()));
        qInfo("octomx: update: kept in place (starter files under the legacy program names): %s", qPrintable(names.join(QLatin1Char(' '))));
    }
    if (!writeJournal(prog, j, &why))
        return fail(why);
    Swapper s = makeSwapper(prog, j, crashAfterOps);
    s.stage = QDir::cleanPath(plan.stageTop);
    if (!s.forward()) {
        if (s.crashed)
            return fail(QStringLiteral("simulated crash after %1 operations").arg(s.ops));
        const QString first = s.error;
        s.crashAfter = -1;
        if (s.backward()) {
            QFile::remove(QDir(prog).filePath(QString::fromLatin1(kJournalName)));
            QDir(oldDir).removeRecursively();
            return fail(QStringLiteral("%1; the update was undone, the installed version is unchanged").arg(first));
        }
        return fail(QStringLiteral("%1; undoing it failed too (%2): OctoMX finishes or undoes it at the next start").arg(first, s.error));
    }
    j.state = QStringLiteral("committed");
    if (!writeJournal(prog, j, &why))
        return fail(why);   // the next start sees "swapping", finds every file in place and finishes
    return true;
}

bool readJournal(const QString &programDir, QString *state, QString *from, QString *to, QStringList *kept)
{
    Journal j;
    if (!loadJournal(programDir, &j))
        return false;
    if (state)
        *state = j.state;
    if (from)
        *from = j.from;
    if (to)
        *to = j.to;
    if (kept)
        *kept = j.kept;
    return true;
}

Recovery recoverInstall(const QString &programDir, int crashAfterOps)
{
    Recovery r;
    const QString prog = QDir::cleanPath(programDir);
    const QString journalPath = QDir(prog).filePath(QString::fromLatin1(kJournalName));
    const bool haveJournal = exists(journalPath);
    if (!haveJournal) {
        // a staging folder without a journal: prepared, never swapped (or cleaned up halfway)
        bool any = false;
        for (const QString &d : QDir(prog).entryList({QStringLiteral(".new-*"), QStringLiteral(".old-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
            QLockFile lock(QDir(prog).filePath(QString::fromLatin1(kLockName)));
            if (!lock.tryLock(0)) {
                r.what = Recovery::Busy;
                return r;
            }
            QDir(QDir(prog).filePath(d)).removeRecursively();
            any = true;
        }
        r.what = any ? Recovery::Cleaned : Recovery::Nothing;
        return r;
    }
    QLockFile lock(QDir(prog).filePath(QString::fromLatin1(kLockName)));
    if (!lock.tryLock(3000)) {
        r.what = Recovery::Busy;
        return r;
    }
    Journal j;
    if (!loadJournal(prog, &j)) {
        r.what = Recovery::Failed;
        r.error = QStringLiteral("the update journal cannot be read");
        return r;
    }
    r.from = j.from;
    r.to = j.to;
    if (j.state == QLatin1String("committed")) {
        cleanupAfter(prog, j);
        QFile::remove(journalPath);
        r.what = Recovery::Cleaned;
        return r;
    }
    Swapper s = makeSwapper(prog, j, crashAfterOps);
    if (s.canForward()) {
        if (s.forward()) {
            r.moved = s.ops;
            j.state = QStringLiteral("committed");
            writeJournal(prog, j, nullptr);
            cleanupAfter(prog, j);
            QFile::remove(journalPath);
            r.what = Recovery::Finished;
            return r;
        }
        if (s.crashed) {
            r.what = Recovery::Failed;
            r.error = QStringLiteral("simulated crash");
            return r;
        }
    }
    const QString why = s.error;
    s.error.clear();
    if (s.backward()) {
        r.moved = s.ops;
        QDir(QDir(prog).filePath(j.old)).removeRecursively();
        QFile::remove(journalPath);
        QDir(QDir(prog).filePath(j.stage.section(QLatin1Char('/'), 0, 0))).removeRecursively();
        r.what = Recovery::RolledBack;
        r.error = why;
        return r;
    }
    r.what = Recovery::Failed;
    r.moved = s.ops;
    r.error = s.crashed ? QStringLiteral("simulated crash") : s.error;
    return r;
}

} // namespace mx::upd
