// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/download_request.h"

#include "mxhash/links.h"

#include <QtCore/QFileInfo>
#include <QtCore/QRandomGenerator>
#include <QtCore/QUrl>

namespace mx {

QString newDownloadId()
{
    const quint64 v = QRandomGenerator::global()->generate64();
    return QStringLiteral("d%1").arg(v, 16, 16, QLatin1Char('0'));
}

bool resolveDownloadRequest(DownloadRequest &req, QString *error)
{
    const QString uri = req.uri.trimmed();
    if (!uri.isEmpty()) {
        const QString lower = uri.toLower();
        if (lower.startsWith(QLatin1String("magnet:")) || lower.startsWith(QLatin1String("ed2k:"))) {
            const hash::ParsedLink l = hash::parseLink(uri);
            if (!l.isValid()) {
                if (error) *error = l.error.isEmpty() ? QStringLiteral("Invalid link") : l.error;
                return false;
            }
            FileHashes h = req.hashes;
            h.mergeFrom(l.hashes);
            req.hashes = h;
            if (req.name.isEmpty()) req.name = l.name;
            if (req.size <= 0 && l.size > 0) req.size = l.size;
            if (req.wantedFiles.isEmpty() && !l.selectOnly.isEmpty()) req.wantedFiles = l.selectOnly;
            if (l.kind == hash::ParsedLink::Ed2k) {
                for (const QString &s : l.sources) {
                    SearchSource src;
                    src.net = Net::ED2K;
                    src.user.net = Net::ED2K;
                    src.user.id = s;
                    src.user.address = s;
                    src.uri = uri;
                    req.sources << src;
                }
                if (req.sources.isEmpty()) {
                    SearchSource src;
                    src.net = Net::ED2K;
                    src.uri = uri;
                    req.sources << src;
                }
            } else {
                if (!l.hashes.btih.isEmpty()) {
                    SearchSource src;
                    src.net = Net::BitTorrent;
                    src.uri = uri;
                    req.sources << src;
                }
                for (const QString &ws : l.webSeeds + l.acceptableSources) {
                    SearchSource src;
                    src.net = Net::Http;
                    src.uri = ws;
                    req.sources << src;
                }
                for (const QString &xs : l.exactSources) {
                    if (xs.startsWith(QLatin1String("http"), Qt::CaseInsensitive)) {
                        SearchSource src;
                        src.net = Net::Http;
                        src.uri = xs;
                        req.sources << src;
                    }
                }
            }
        } else if (lower.startsWith(QLatin1String("http://")) || lower.startsWith(QLatin1String("https://"))
                   || lower.startsWith(QLatin1String("ftp://"))) {
            const QUrl url(uri);
            if (req.name.isEmpty()) req.name = QFileInfo(url.path()).fileName();
            if (req.name.isEmpty()) req.name = url.host();
            if (lower.endsWith(QLatin1String(".torrent"))) {
                SearchSource src;
                src.net = Net::BitTorrent;
                src.uri = uri;
                req.sources << src;
            } else {
                SearchSource src;
                src.net = Net::Http;
                src.uri = uri;
                req.sources << src;
            }
        } else {
            // local .torrent file (path or file:// URL)
            const QString path = lower.startsWith(QLatin1String("file:")) ? QUrl(uri).toLocalFile() : uri;
            const QFileInfo fi(path);
            if (!fi.isFile()) {
                if (error) *error = QStringLiteral("Unsupported link or missing file: %1").arg(uri);
                return false;
            }
            req.uri = fi.absoluteFilePath();
            if (req.name.isEmpty()) req.name = fi.completeBaseName();
            SearchSource src;
            src.net = Net::BitTorrent;
            src.uri = req.uri;
            req.sources << src;
        }
    }
    // drop duplicate sources (same network, user and uri) and the "this index listed it" placeholders
    // of search-only networks (Web / Rats entries without a magnet: nothing to fetch from) [engine-4]
    QList<SearchSource> unique;
    for (const SearchSource &s : std::as_const(req.sources)) {
        if ((s.net == Net::Web || s.net == Net::Rats || s.net == Net::Unknown) && s.uri.trimmed().isEmpty()) continue;
        bool dup = false;
        for (const SearchSource &u : std::as_const(unique))
            if (u.net == s.net && u.uri == s.uri && u.user.id == s.user.id) { dup = true; break; }
        if (!dup) unique << s;
    }
    req.sources = unique;
    if (req.hashes.isEmpty() && req.uri.isEmpty() && req.sources.isEmpty()) {
        if (error) *error = QStringLiteral("Nothing to download");
        return false;
    }
    if (req.name.isEmpty()) {
        if (!req.hashes.ed2k.isEmpty()) req.name = QString::fromLatin1(req.hashes.ed2k.toHex());
        else if (!req.hashes.btih.isEmpty()) req.name = QString::fromLatin1(req.hashes.btih.toHex());
        else if (!req.hashes.sha1.isEmpty()) req.name = QString::fromLatin1(req.hashes.sha1.toHex());
        else req.name = QStringLiteral("download");
    }
    return true;
}

DownloadRequest requestFromHit(const SearchHit &hit, const QString &saveDir)
{
    DownloadRequest r;
    r.name = hit.fileName;
    r.size = hit.size;
    r.hashes = hit.hashes;
    r.sources = hit.sources;
    // [merge-1] files of a folder torrent that other networks have too (same name and size): their
    // sources serve those files (mapped by extra["bundlePath"], every block checked by the pieces)
    if (hit.isBundle)
        for (const SearchHit::InnerMatch &m : hit.innerMatches) r.sources << m.sources;
    r.saveDir = saveDir;
    for (const SearchSource &s : hit.sources) {
        if (!s.uri.isEmpty() && !s.extra.value(QStringLiteral("candidate")).toBool()) {
            r.uri = s.uri;
            break;
        }
    }
    return r;
}

} // namespace mx
