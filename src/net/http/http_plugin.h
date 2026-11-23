// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: plain HTTP/HTTPS byte-range sources (mirrors, Internet Archive links, pasted URLs).
// Owner: http task. This is the ONLY public header of mxnet_http.
//
// ---------------------------------------------------------------- how the engine uses it
// createHttpPlugin() → a NetworkPlugin (net() == Net::Http, CanFetchRanges, no servers, status
// "Ready" once started). openSource(downloadId, hashes, size, SearchSource{net = Http,
// uri = "http(s)://..."}) returns an HttpRangeSource (private class) that also implements
// mx::HttpSourceInfo below (reach it with dynamic_cast or qobject_cast<mx::HttpSourceInfo*>).
//
// open():  probes the URL (HEAD, else GET "Range: bytes=0-0"/"0-63"), follows ≤ 10 redirects
//          (https→http refused unless http/allowInsecureRedirect), then goes to Ready, Queued
//          ("Server busy, retry in 30 s" on 429/503 + Retry-After) or Failed (stateText() says why:
//          404/410/403, size mismatch, TLS error, redirect loop ...).
//
// Size unknown (size <= 0 passed to openSource): the engine cannot link this module, so
// everything the probe learned is published in descriptor().extra once the source is Ready:
//     "size"           qint64   -1 = unknown
//     "fileName"       QString  Content-Disposition (incl. RFC 5987 filename*), else URL path
//     "contentType"    QString
//     "supportsRanges" bool
//     "etag", "lastModified", "finalUrl"   QString
//     "isTorrentFile"  bool     content type / ".torrent" / bencode sniff of the first bytes
//   (the same keys are kept up to date later, e.g. supportsRanges turns false when a server
//   answers a range request with 200). available() is {0, size} when the size is known, else
//   EMPTY: then only one sequential request from offset 0 is possible; its end is signalled by
//   requestFinished(0, receivedLength, true, "") with the real received length (≤ requested).
//   Persist extra["etag"]: when a source is re-opened with it (after a restart) and the server's
//   strong ETag differs, the source fails with "File changed on the server".
//
// Range-capable servers: maxPendingRequests() = http/connectionsPerSource (default 4, capped by
// http/maxConnectionsPerServer, which is also enforced across all sources of one host, extra
// requests simply wait for a slot), preferredRequestSize() = 4 MiB. Requests are ranged GETs
// guarded by If-Range (strong ETag, else Last-Modified); data() arrives in order in chunks of
// ≤ 256 KiB while bytes come in. Transient errors (timeouts, stalls, resets, 5xx, short bodies)
// are retried with back-off from the byte where they broke off (up to http/maxRetries); the
// engine just sees one contiguous stream. A file that changes on the server → Failed
// ("File changed on the server ..."), never silently mixed bytes.
//
// Servers WITHOUT range support (HttpSourceInfo::supportsRanges() == false,
// descriptor().extra["supportsRanges"] == false):
//   * available() still reports the whole file, maxPendingRequests() == 1,
//     preferredRequestSize() == the whole file (so the engine asks for [0, size) in one go).
//   * Only a request starting at offset 0 can be served (the body is streamed sequentially; it may
//     be shorter than the file: the connection is dropped once `length` bytes were delivered).
//   * request(offset != 0, ...) fails immediately (queued, not re-entrant) with an error that starts
//     with mx::kHttpErrorNoRanges. The engine should then use this source only while the
//     contiguous prefix [0, x) is still missing, i.e. give it the first missing chunk if that
//     chunk starts at 0, else treat it as useless for this download (do not count it as a strike).
//
// cancelRequests(): aborts every outstanding request silently (no requestFinished for them, and
// no data() after it returns). close(): the same, then state Closed.
// Bandwidth: every delivered byte passes ctx.limiter->acquire(Down) and account(Down, Net::Http).
//
// HttpProbe (below) is for the app/UI and anyone who can link this module: probe a pasted URL
// (size, name, ranges, torrent?) or fetch a small resource (a .torrent) into memory with a cap.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

#include "mx/plugin.h"

class QNetworkAccessManager;

namespace mx {

class Settings;

NetworkPlugin *createHttpPlugin(QObject *parent = nullptr);

// Error text prefix of a request() refused because the server cannot serve byte ranges.
inline constexpr char kHttpErrorNoRanges[] = "HTTP server does not support byte ranges";

// Extra interface of every RangeSource returned by the HTTP plugin.
class HttpSourceInfo {
public:
    virtual ~HttpSourceInfo() = default;
    virtual bool supportsRanges() const = 0;   // false → see "Servers WITHOUT range support"
    virtual qint64 remoteSize() const = 0;     // -1 = unknown
    virtual QUrl finalUrl() const = 0;         // after redirects
    virtual QString etag() const = 0;
    virtual QString lastModified() const = 0;
};

// ---------------------------------------------------------------- URL probing / small fetches
struct HttpProbeResult {
    bool       ok = false;
    QString    error;              // human readable when !ok
    int        httpStatus = 0;     // last HTTP status seen (0 = none)
    int        retryAfterSec = -1; // server busy (429/503): suggested wait
    QUrl       finalUrl;           // after redirects
    qint64     size = -1;          // -1 unknown
    bool       supportsRanges = false;
    QString    fileName;           // Content-Disposition (RFC 6266/5987), else URL path, else host
    QString    contentType;        // without parameters, lower case
    QString    etag;
    QString    lastModified;       // raw header value
    bool       isTorrentFile = false;
    QStringList mirrors;           // Link: <...>; rel=duplicate (RFC 6249)
    FileHashes hashes;             // Digest / Repr-Digest / Content-MD5 / X-Checksum-* headers
};

class HttpProbe : public QObject {
    Q_OBJECT
public:
    // settings may be null (defaults: no proxy, default user agent, TLS verified).
    explicit HttpProbe(Settings *settings = nullptr, QObject *parent = nullptr);
    ~HttpProbe() override;

    // One operation at a time per object; starting another aborts the running one silently.
    void probe(const QString &url);                       // → finished()
    void fetchSmall(const QString &url, qint64 maxBytes = 16 * 1024 * 1024);   // → fetched()
    void abort();                                         // no signal afterwards
    // Set to delete the object after it emitted its result.
    void setAutoDelete(bool on) { m_autoDelete = on; }

    // Helpers (also used internally): bencode sniff of the first bytes of a file.
    static bool looksLikeTorrent(const QByteArray &head);
    // Parse user input ("www.x.org/a b.iso", IDN host, credentials) into a URL; invalid if not http(s).
    static QUrl normalizeUrl(const QString &userInput);

signals:
    void finished(const mx::HttpProbeResult &result);
    void fetched(const mx::HttpProbeResult &result, const QByteArray &data);

private:
    void cleanup();
    Settings *m_settings = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QObject> m_job;
    bool m_autoDelete = false;
};

} // namespace mx

Q_DECLARE_INTERFACE(mx::HttpSourceInfo, "mx.HttpSourceInfo/1.0")
Q_DECLARE_METATYPE(mx::HttpProbeResult)
