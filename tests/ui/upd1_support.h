// SPDX-License-Identifier: GPL-3.0-or-later
// [upd-1] Test support for the self-update tests (never GitHub): a local HTTP/1.1 server with binary
// bodies, redirects and Range requests, a throwaway Ed25519 key, fake packages (installed folder,
// archives) and a small zip writer.
#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QtEndian>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include "mx/update_check.h"
#include "mx/update_install.h"

namespace upd1 {

struct Route {
    int status = 200;
    QByteArray body;
    QByteArray headers;        // extra raw header lines
    bool ranges = false;       // answer "Range: bytes=N-" with 206
};

// a tiny HTTP/1.1 server on 127.0.0.1 (one request per connection)
class Server : public QObject {
public:
    QTcpServer server;
    QHash<QString, Route> routes;
    QList<QByteArray> requests;

    Server()
    {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, s, [this, s] {
                    QByteArray &buf = m_buf[s];
                    buf += s->readAll();
                    if (!buf.contains("\r\n\r\n"))
                        return;
                    const QByteArray head = buf;
                    buf.clear();
                    requests << head;
                    const QString path = QString::fromLatin1(head.split(' ').value(1));
                    const Route r = routes.value(path, Route{404, "not found", {}, false});
                    QByteArray body = r.body;
                    int status = r.status;
                    QByteArray extra = r.headers;
                    static const QRegularExpression range(QStringLiteral("(?i)\\r\\nrange: bytes=(\\d+)-"));
                    const QRegularExpressionMatch m = range.match(QString::fromLatin1(head));
                    if (r.ranges && m.hasMatch() && status == 200) {
                        const qint64 from = m.captured(1).toLongLong();
                        if (from >= body.size()) {
                            status = 416;
                            body.clear();
                        } else {
                            extra += "Content-Range: bytes " + QByteArray::number(from) + "-" + QByteArray::number(body.size() - 1) + "/" +
                                     QByteArray::number(body.size()) + "\r\n";
                            body = body.mid(from);
                            status = 206;
                        }
                    }
                    QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: application/octet-stream\r\n" + extra;
                    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                    s->write(out);
                    s->disconnectFromHost();
                });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
    QString url(const QString &path) const { return QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path); }
    int count(const QString &path) const
    {
        int n = 0;
        for (const QByteArray &r : requests)
            if (QString::fromLatin1(r.split(' ').value(1)) == path)
                ++n;
        return n;
    }

private:
    QHash<QTcpSocket *, QByteArray> m_buf;
};

// a throwaway release key, trusted in this process (test builds only)
struct Key {
    QByteArray seed, pub;
    QString id;
    static Key make(bool trust)
    {
        Key k;
        k.seed.resize(32);
        for (int i = 0; i < 32; ++i)
            k.seed[i] = char(QRandomGenerator::global()->bounded(256));
        k.pub = mx::upd::testPublicKeyFromSeed(k.seed);
        k.id = mx::upd::keyIdOf(k.pub);
        if (trust)
            mx::upd::addTestTrustedKey(k.pub);
        return k;
    }
    QByteArray sign(const QByteArray &msg) const { return mx::upd::formatSignatureFile(id, mx::upd::testSign(seed, msg)); }
};

inline QByteArray sha256Hex(const QByteArray &b)
{
    return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex();
}

inline bool writeFile(const QString &path, const QByteArray &data, bool exec = false)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size())
        return false;
    f.close();
    if (exec)
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);
    return true;
}

inline QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// the files of a fake package of this platform: the program file is this test executable (a real
// PE / ELF), the rest small text files; `extra` adds or replaces files
inline QMap<QString, QByteArray> packageFiles(const QString &version, const QMap<QString, QByteArray> &extra = {})
{
    static const QByteArray self = readFile(QCoreApplication::applicationFilePath());
    QMap<QString, QByteArray> f;
    const bool win = mx::upd::platformTag() == QLatin1String("win64");
    // a few bytes of the version make every build's program file different
    f.insert(win ? QStringLiteral("octomx.exe") : QStringLiteral("bin/octomx"), self + "\nversion " + version.toLatin1());
    if (!win) {
        f.insert(QStringLiteral("octomx"), "#!/bin/sh\nexec \"$(dirname \"$0\")/bin/octomx\" \"$@\"\n# " + version.toLatin1() + "\n");
        f.insert(QStringLiteral("lib/libQt6Core.so.6"), "core " + version.toLatin1());
    } else {
        f.insert(QStringLiteral("Qt6Core.dll"), "core " + version.toLatin1());
    }
    f.insert(QStringLiteral("README.txt"), "OctoMX " + version.toLatin1() + "\n");
    f.insert(QStringLiteral("fonts/Tahoma.ttf"), "font " + version.toLatin1());
    f.insert(QStringLiteral("plugins/platforms/qoffscreen"), "plugin " + version.toLatin1());
    for (auto it = extra.cbegin(); it != extra.cend(); ++it)
        f.insert(it.key(), it.value());
    return f;
}

inline mx::upd::Manifest manifestFor(const QString &version, const QMap<QString, QByteArray> &files)
{
    mx::upd::Manifest m;
    m.version = version;
    m.platform = mx::upd::platformTag();
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        m.files << mx::upd::ManifestEntry{it.key(), it.value().size(), sha256Hex(it.value())};
    return m;
}

// writes a package folder (files + MANIFEST.txt); returns its manifest
inline mx::upd::Manifest writePackage(const QString &dir, const QString &version, const QMap<QString, QByteArray> &files)
{
    const mx::upd::Manifest m = manifestFor(version, files);
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const bool exec = it.key() == QLatin1String("octomx") || it.key() == QLatin1String("bin/octomx");
        writeFile(QDir(dir).filePath(it.key()), it.value(), exec);
    }
    writeFile(QDir(dir).filePath(QStringLiteral("MANIFEST.txt")), mx::upd::manifestText(m));
    return m;
}

inline quint32 crc32(const QByteArray &d)
{
    static quint32 table[256];
    static bool init = false;
    if (!init) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    quint32 c = 0xffffffffu;
    for (char ch : d)
        c = table[(c ^ uchar(ch)) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

// a zip of every file under `root` (names relative to root's parent, i.e. with the top folder);
// deflate = raw deflate from qCompress (the zlib frame stripped)
inline QByteArray makeZip(const QString &root, bool deflate = true, const QStringList &extraNames = {})
{
    QByteArray out, cd;
    int n = 0;
    auto le16 = [](QByteArray &b, quint16 v) { b.append(char(v & 0xff)).append(char(v >> 8)); };
    auto le32 = [](QByteArray &b, quint32 v) { for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xff)); };
    const QDir base = QFileInfo(root).dir();
    QStringList files;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << base.relativeFilePath(it.next());
    files.sort();
    auto add = [&](const QString &name, const QByteArray &data) {
        QByteArray comp = data;
        quint16 method = 0;
        if (deflate && data.size() > 16) {
            const QByteArray z = qCompress(data, 9);   // 4 byte size + zlib header (2) + deflate + adler32 (4)
            comp = z.mid(6, z.size() - 10);
            method = 8;
        }
        const QByteArray nm = name.toUtf8();
        const quint32 off = quint32(out.size());
        QByteArray lh;
        le32(lh, 0x04034b50u); le16(lh, 20); le16(lh, 0x800); le16(lh, method); le16(lh, 0); le16(lh, 0);
        le32(lh, crc32(data)); le32(lh, quint32(comp.size())); le32(lh, quint32(data.size()));
        le16(lh, quint16(nm.size())); le16(lh, 0);
        out += lh + nm + comp;
        QByteArray ce;
        le32(ce, 0x02014b50u); le16(ce, 20); le16(ce, 20); le16(ce, 0x800); le16(ce, method); le16(ce, 0); le16(ce, 0);
        le32(ce, crc32(data)); le32(ce, quint32(comp.size())); le32(ce, quint32(data.size()));
        le16(ce, quint16(nm.size())); le16(ce, 0); le16(ce, 0); le16(ce, 0); le16(ce, 0); le32(ce, 0); le32(ce, off);
        cd += ce + nm;
        ++n;
    };
    for (const QString &f : std::as_const(files))
        add(f, readFile(base.filePath(f)));
    for (const QString &x : extraNames)
        add(x, "evil");
    QByteArray eocd;
    le32(eocd, 0x06054b50u); le16(eocd, 0); le16(eocd, 0); le16(eocd, quint16(n)); le16(eocd, quint16(n));
    le32(eocd, quint32(cd.size())); le32(eocd, quint32(out.size())); le16(eocd, 0);
    return out + cd + eocd;
}

// the archive of this platform for a package folder: zip (Windows) or tar.xz through the system tar
inline QByteArray makeArchive(const QString &pkgDir)
{
    if (mx::upd::platformTag() == QLatin1String("win64"))
        return makeZip(pkgDir);
    const QString out = pkgDir + QStringLiteral(".tar.xz");
    QProcess p;
    p.start(QStringLiteral("tar"), {QStringLiteral("-C"), QFileInfo(pkgDir).absolutePath(), QStringLiteral("-cJf"), out,
                                    QFileInfo(pkgDir).fileName()});
    p.waitForFinished(60000);
    return readFile(out);
}

// the releases/latest JSON with assets on the server
inline QByteArray releaseJson(const QString &tag, const QList<QPair<QString, QString>> &assets, const QMap<QString, qint64> &sizes = {})
{
    QJsonObject o;
    o.insert(QStringLiteral("tag_name"), tag);
    o.insert(QStringLiteral("name"), QStringLiteral("OctoMX ") + tag);
    o.insert(QStringLiteral("body"), QStringLiteral("## What's new\n\n* Updates that install themselves\n* Faster lists"));
    o.insert(QStringLiteral("html_url"), QStringLiteral("https://github.com/robtacconelli/OctoMX/releases/tag/") + tag);
    o.insert(QStringLiteral("draft"), false);
    o.insert(QStringLiteral("prerelease"), false);
    QJsonArray a;
    for (const auto &as : assets) {
        QJsonObject x;
        x.insert(QStringLiteral("name"), as.first);
        x.insert(QStringLiteral("browser_download_url"), as.second);
        if (sizes.contains(as.first))
            x.insert(QStringLiteral("size"), double(sizes.value(as.first)));
        a.append(x);
    }
    o.insert(QStringLiteral("assets"), a);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// every file under dir (relative paths) -> content
inline QMap<QString, QByteArray> treeOf(const QString &dir)
{
    QMap<QString, QByteArray> out;
    QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString f = it.next();
        out.insert(QDir(dir).relativeFilePath(f), readFile(f));
    }
    return out;
}

} // namespace upd1
