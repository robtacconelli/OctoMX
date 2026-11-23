// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: HTTP module internals: settings, request construction, header parsers, redirects.
// Private to mxnet_http (tests include it directly).
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QLoggingCategory>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkRequest>

#include "mx/types.h"

class QNetworkAccessManager;
class QNetworkReply;

Q_DECLARE_LOGGING_CATEGORY(lcHttp)

namespace mx {
class Settings;

namespace httpkeys {   // keys used by this module that the schema may not (yet) define
inline constexpr char ConnectionsPerSource[] = "http/connectionsPerSource";   // int, default 4
inline constexpr char AllowInsecureRedirect[] = "http/allowInsecureRedirect"; // bool, default false
inline constexpr char StallTimeoutSec[] = "http/stallTimeoutSec";             // int, default 30
inline constexpr char MaxRetries[] = "http/maxRetries";                       // int, default 5
inline constexpr char RetryBaseMs[] = "http/retryBaseMs";                     // int, default 1000
}

struct HttpConfig {
    QString userAgent = QStringLiteral("OctoMX/" MX_VERSION_STRING);
    int     timeoutSec = 30;           // connect / response headers / probe step
    int     stallSec = 30;             // no body bytes for this long → abort + retry
    bool    verifyTls = true;
    int     connectionsPerSource = 4;
    int     maxPerHost = 4;
    bool    allowInsecureRedirect = false;
    int     maxRedirects = 10;
    int     maxRetries = 5;
    int     retryBaseMs = 1000;
    static HttpConfig load(const Settings *s);
};

QNetworkProxy proxyFromSettings(const Settings *s);
// Apply proxy settings to a manager.
void configureManager(QNetworkAccessManager *nam, const Settings *s);

// Credentials embedded in the URL ("https://user:pw@host/..."): stripped from the URL and sent as
// "Authorization: Basic" only to the same origin.
struct HttpCredentials {
    QString scheme, host; int port = -1;
    QByteArray basic;                  // "Basic ..." or empty
    static HttpCredentials take(QUrl *url);   // removes the userinfo from *url
    bool appliesTo(const QUrl &u) const;
};

// GET/HEAD request with our defaults: UA, Accept-Encoding: identity (never let Qt decompress,
// ranges are of the raw entity), HTTP/1.1 (parallel ranges should be separate connections),
// manual redirects, no cache.
QNetworkRequest makeRequest(const QUrl &url, const HttpConfig &cfg, const HttpCredentials &cred);
// When TLS verification is disabled, make the reply ignore certificate errors.
void prepareReply(QNetworkReply *reply, const HttpConfig &cfg);
// Disconnect from everyone, abort, deleteLater. Safe with nullptr.
void dropReply(QNetworkReply *reply);

// ---------------------------------------------------------------- parsers
struct ContentRange { bool valid = false; qint64 first = -1, last = -1, total = -1; bool unsatisfied = false; };
ContentRange parseContentRange(const QByteArray &v);   // "bytes a-b/total", "bytes a-b/*", "bytes */total"
QString parseContentDisposition(const QByteArray &v);  // file name or empty (sanitised)
QString sanitizeFileName(const QString &name);
QString fileNameFromUrl(const QUrl &u);                // decoded last path segment (sanitised)
int parseRetryAfter(const QByteArray &v, const QDateTime &nowUtc = QDateTime::currentDateTimeUtc()); // seconds, -1 if absent/invalid
QStringList parseDuplicateLinks(const QByteArray &linkHeader, const QUrl &base);
// Digest (RFC 3230), Repr-Digest (RFC 9530), Content-MD5, X-Checksum-*, x-goog-hash.
// wholeEntity: the response describes the whole file (HEAD / 200), not a part (206).
FileHashes parseHashHeaders(const QNetworkReply *reply, bool wholeEntity);
bool isStrongEtag(const QString &etag);
bool looksLikeTorrentData(const QByteArray &head);
QString contentTypeOf(const QNetworkReply *reply);     // lower case, no parameters
qint64 contentLengthOf(const QNetworkReply *reply);    // -1 unknown
int statusOf(const QNetworkReply *reply);              // 0 if none
// Parse a user/server-provided URL tolerant of raw spaces / UTF-8 / IDN.
QUrl parseUrl(const QString &s);

// ---------------------------------------------------------------- redirects
struct RedirectTracker {
    int hops = 0;
    QSet<QString> seen;
    // Returns the next URL or an invalid URL with *error set.
    QUrl next(const QUrl &from, const QNetworkReply *reply, const HttpConfig &cfg, QString *error);
    QUrl next(const QUrl &from, const QByteArray &location, const HttpConfig &cfg, QString *error);
    void reset() { hops = 0; seen.clear(); }
};
bool isRedirectStatus(int status);

// Human readable text for a network error.
QString describeError(const QNetworkReply *reply);
// True if the error class is worth retrying (timeouts, resets, DNS, 5xx ...).
bool isTransientNetworkError(int qnetworkReplyError);

} // namespace mx
