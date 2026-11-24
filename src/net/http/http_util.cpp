// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_util.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QStringDecoder>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslError>
#include <chrono>

#include "mx/services.h"
#include "mx/net_policy.h"
#include "mx/settings_keys.h"

Q_LOGGING_CATEGORY(lcHttp, "mx.http")

namespace mx {

namespace {
QVariant sv(const Settings *s, const QString &k)
{
    return s ? s->value(k) : QVariant();
}
int si(const Settings *s, const QString &k, int def, int lo, int hi)
{
    const QVariant v = sv(s, k);
    bool ok = false;
    const int i = v.isValid() ? v.toInt(&ok) : 0;
    return ok ? qBound(lo, i, hi) : def;
}
bool sb(const Settings *s, const QString &k, bool def)
{
    const QVariant v = sv(s, k);
    return v.isValid() ? v.toBool() : def;
}
} // namespace

HttpConfig HttpConfig::load(const Settings *s)
{
    HttpConfig c;
    if (!s) return c;
    const QString ua = s->value(keys::http::UserAgent).toString().trimmed();
    if (!ua.isEmpty()) c.userAgent = ua;
    c.timeoutSec = si(s, keys::http::TimeoutSec, 30, 2, 600);
    c.verifyTls = sb(s, keys::http::VerifyTls, true);
    c.maxPerHost = si(s, keys::http::MaxConnectionsPerServer, 4, 1, 16);
    c.connectionsPerSource = si(s, QLatin1StringView(httpkeys::ConnectionsPerSource), 4, 1, 16);
    c.allowInsecureRedirect = sb(s, QLatin1StringView(httpkeys::AllowInsecureRedirect), false);
    c.stallSec = si(s, QLatin1StringView(httpkeys::StallTimeoutSec), 30, 1, 600);
    c.maxRetries = si(s, QLatin1StringView(httpkeys::MaxRetries), 5, 0, 100);
    c.retryBaseMs = si(s, QLatin1StringView(httpkeys::RetryBaseMs), 1000, 10, 60000);
    return c;
}

QNetworkProxy proxyFromSettings(const Settings *s)
{
    // [net-2] one policy for every network (mx/net_policy.h)
    return netpolicy::qtProxy(netpolicy::fromSettings(s).proxy);
}

void configureManager(QNetworkAccessManager *nam, const Settings *s)
{
    netpolicy::applyTo(nam, s);   // [net-2] the proxy, or the relay bound to the chosen adapter
    nam->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    nam->setAutoDeleteReplies(false);
    nam->setStrictTransportSecurityEnabled(false);
}

HttpCredentials HttpCredentials::take(QUrl *url)
{
    HttpCredentials c;
    if (!url || url->userInfo().isEmpty()) return c;
    const QString user = url->userName(QUrl::FullyDecoded);
    const QString pass = url->password(QUrl::FullyDecoded);
    c.scheme = url->scheme().toLower();
    c.host = url->host(QUrl::FullyEncoded).toLower();
    c.port = url->port(c.scheme == QLatin1String("https") ? 443 : 80);
    c.basic = "Basic " + (user + QLatin1Char(':') + pass).toUtf8().toBase64();
    url->setUserInfo(QString());
    return c;
}

bool HttpCredentials::appliesTo(const QUrl &u) const
{
    if (basic.isEmpty()) return false;
    const QString sch = u.scheme().toLower();
    return sch == scheme && u.host(QUrl::FullyEncoded).toLower() == host
        && u.port(sch == QLatin1String("https") ? 443 : 80) == port;
}

QNetworkRequest makeRequest(const QUrl &url, const HttpConfig &cfg, const HttpCredentials &cred)
{
    QUrl u = url;
    u.setUserInfo(QString());
    QNetworkRequest r(u);
    r.setRawHeader("User-Agent", cfg.userAgent.toUtf8());
    r.setRawHeader("Accept-Encoding", "identity");
    r.setRawHeader("Accept", "*/*");
    r.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    r.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    r.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    if (cred.appliesTo(u)) r.setRawHeader("Authorization", cred.basic);
    return r;
}

void prepareReply(QNetworkReply *reply, const HttpConfig &cfg)
{
    if (!reply || cfg.verifyTls) return;
    QObject::connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError> &) {
        reply->ignoreSslErrors();
    });
}

void dropReply(QNetworkReply *reply)
{
    if (!reply) return;
    reply->disconnect();
    if (reply->isRunning()) reply->abort();
    reply->deleteLater();
}

// ---------------------------------------------------------------- parsers
ContentRange parseContentRange(const QByteArray &v)
{
    ContentRange cr;
    static const QRegularExpression re(QStringLiteral("^\\s*bytes\\s+(?:(\\d+)\\s*-\\s*(\\d+)|(\\*))\\s*/\\s*(\\d+|\\*)\\s*$"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(QString::fromLatin1(v));
    if (!m.hasMatch()) return cr;
    if (m.captured(4) != QLatin1String("*")) {
        bool ok = false;
        cr.total = m.captured(4).toLongLong(&ok);
        if (!ok) return cr;
    }
    if (!m.captured(3).isEmpty()) {
        cr.unsatisfied = true;
        cr.valid = cr.total >= 0;
        return cr;
    }
    bool ok1 = false, ok2 = false;
    cr.first = m.captured(1).toLongLong(&ok1);
    cr.last = m.captured(2).toLongLong(&ok2);
    cr.valid = ok1 && ok2 && cr.first <= cr.last && (cr.total < 0 || cr.last < cr.total);
    return cr;
}

namespace {
// Split "a; b=\"x;y\"; c" on `sep` outside quotes.
QList<QByteArray> splitOutsideQuotes(const QByteArray &v, char sep, bool angle = false)
{
    QList<QByteArray> out;
    QByteArray cur;
    bool q = false, esc = false;
    int depth = 0;
    for (char ch : v) {
        if (esc) { cur += ch; esc = false; continue; }
        if (q && ch == '\\') { cur += ch; esc = true; continue; }
        if (ch == '"') q = !q;
        else if (angle && !q && ch == '<') ++depth;
        else if (angle && !q && ch == '>') depth = qMax(0, depth - 1);
        if (ch == sep && !q && depth == 0) { out << cur.trimmed(); cur.clear(); continue; }
        cur += ch;
    }
    if (!cur.trimmed().isEmpty()) out << cur.trimmed();
    return out;
}
QByteArray unquote(QByteArray v)
{
    v = v.trimmed();
    if (v.size() >= 2 && v.startsWith('"') && v.endsWith('"')) {
        QByteArray out;
        bool esc = false;
        for (int i = 1; i < v.size() - 1; ++i) {
            const char ch = v[i];
            if (!esc && ch == '\\') { esc = true; continue; }
            esc = false;
            out += ch;
        }
        return out;
    }
    return v;
}
QString decodeBytes(const QByteArray &b, const QByteArray &charset)
{
    const QByteArray cs = charset.toLower();
    if (cs == "iso-8859-1" || cs == "latin1" || cs == "us-ascii") return QString::fromLatin1(b);
    QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString s = dec(b);
    if (dec.hasError()) return QString::fromLatin1(b);
    return s;
}
} // namespace

QString sanitizeFileName(const QString &name)
{
    QString n = name;
    const int cut = qMax(n.lastIndexOf(QLatin1Char('/')), n.lastIndexOf(QLatin1Char('\\')));
    if (cut >= 0) n = n.mid(cut + 1);
    QString out;
    out.reserve(n.size());
    for (QChar c : n) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) continue;
        if (QStringLiteral("<>:\"|?*").contains(c)) { out += QLatin1Char('_'); continue; }
        out += c;
    }
    out = out.trimmed();
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))) out.chop(1);
    if (out == QLatin1String(".") || out == QLatin1String("..")) return QString();
    return out;
}

QString parseContentDisposition(const QByteArray &v)
{
    const QList<QByteArray> parts = splitOutsideQuotes(v, ';');
    QString plain, extended;
    for (int i = 0; i < parts.size(); ++i) {
        const QByteArray &p = parts[i];
        const int eq = p.indexOf('=');
        if (eq <= 0) continue;
        const QByteArray key = p.left(eq).trimmed().toLower();
        const QByteArray val = p.mid(eq + 1).trimmed();
        if (key == "filename*") {
            // RFC 5987: charset'language'percent-encoded
            const QByteArray raw = unquote(val);
            const int a = raw.indexOf('\'');
            const int b = a >= 0 ? raw.indexOf('\'', a + 1) : -1;
            if (a < 0 || b < 0) continue;
            const QByteArray charset = raw.left(a);
            extended = decodeBytes(QByteArray::fromPercentEncoding(raw.mid(b + 1)), charset);
        } else if (key == "filename") {
            plain = decodeBytes(unquote(val), "utf-8");
        }
    }
    const QString s = sanitizeFileName(!extended.isEmpty() ? extended : plain);
    return s;
}

QString fileNameFromUrl(const QUrl &u)
{
    QString n = u.fileName(QUrl::FullyDecoded);
    return sanitizeFileName(n);
}

int parseRetryAfter(const QByteArray &v, const QDateTime &nowUtc)
{
    const QByteArray t = v.trimmed();
    if (t.isEmpty()) return -1;
    bool ok = false;
    const qint64 n = t.toLongLong(&ok);
    if (ok) return n < 0 ? -1 : int(qMin<qint64>(n, 24 * 3600));
    QDateTime d = QDateTime::fromString(QString::fromLatin1(t), Qt::RFC2822Date);
    if (!d.isValid()) {
        // "Sun, 06 Nov 1994 08:49:37 GMT", be lenient about the zone name
        QString s = QString::fromLatin1(t);
        s.replace(QLatin1String(" GMT"), QLatin1String(" +0000"));
        d = QDateTime::fromString(s, Qt::RFC2822Date);
    }
    if (!d.isValid()) return -1;
    const qint64 secs = nowUtc.secsTo(d);
    return int(qBound(qint64(0), secs, qint64(24 * 3600)));
}

QStringList parseDuplicateLinks(const QByteArray &linkHeader, const QUrl &base)
{
    QStringList out;
    for (const QByteArray &entry : splitOutsideQuotes(linkHeader, ',', true)) {
        const int a = entry.indexOf('<');
        const int b = entry.indexOf('>', a + 1);
        if (a < 0 || b < 0) continue;
        const QByteArray target = entry.mid(a + 1, b - a - 1).trimmed();
        bool dup = false;
        for (const QByteArray &param : splitOutsideQuotes(entry.mid(b + 1), ';')) {
            const int eq = param.indexOf('=');
            if (eq <= 0) continue;
            if (param.left(eq).trimmed().toLower() != "rel") continue;
            const QList<QByteArray> rels = unquote(param.mid(eq + 1)).toLower().split(' ');
            if (rels.contains("duplicate")) dup = true;
        }
        if (!dup) continue;
        const QUrl u = base.resolved(parseUrl(QString::fromUtf8(target)));
        const QString sch = u.scheme().toLower();
        if (u.isValid() && (sch == QLatin1String("http") || sch == QLatin1String("https"))) {
            const QString s = u.toString(QUrl::FullyEncoded);
            if (!out.contains(s)) out << s;
        }
    }
    return out;
}

namespace {
QByteArray decodeHashValue(QByteArray v, int expectLen)
{
    v = v.trimmed();
    if (v.startsWith(':') && v.endsWith(':') && v.size() >= 2) v = v.mid(1, v.size() - 2);   // RFC 9530 byte sequence
    v = unquote(v);
    if (v.size() == expectLen * 2) {
        static const QRegularExpression hex(QStringLiteral("^[0-9a-fA-F]+$"));
        if (hex.match(QString::fromLatin1(v)).hasMatch()) return QByteArray::fromHex(v);
    }
    const QByteArray b = QByteArray::fromBase64(v);
    return b.size() == expectLen ? b : QByteArray();
}
void addDigest(FileHashes &h, const QByteArray &alg, const QByteArray &val)
{
    const QByteArray a = alg.trimmed().toLower();
    if (a == "sha-256") { const QByteArray d = decodeHashValue(val, 32); if (!d.isEmpty()) h.sha256 = d; }
    else if (a == "sha" || a == "sha-1" || a == "sha1") { const QByteArray d = decodeHashValue(val, 20); if (!d.isEmpty()) h.sha1 = d; }
    else if (a == "md5") { const QByteArray d = decodeHashValue(val, 16); if (!d.isEmpty()) h.md5 = d; }
}
void parseDigestList(FileHashes &h, const QByteArray &v)
{
    for (const QByteArray &item : splitOutsideQuotes(v, ',')) {
        const int eq = item.indexOf('=');
        if (eq <= 0) continue;
        addDigest(h, item.left(eq), item.mid(eq + 1));
    }
}
} // namespace

FileHashes parseHashHeaders(const QNetworkReply *reply, bool wholeEntity)
{
    FileHashes h;
    if (!reply) return h;
    // Digest (RFC 3230) and Repr-Digest (RFC 9530) describe the selected representation, i.e. the
    // whole file even in a 206; Content-MD5 / Content-Digest describe this message's body only.
    parseDigestList(h, reply->rawHeader("Digest"));
    parseDigestList(h, reply->rawHeader("Repr-Digest"));
    if (wholeEntity) {
        parseDigestList(h, reply->rawHeader("Content-Digest"));
        const QByteArray cmd5 = reply->rawHeader("Content-MD5");
        if (!cmd5.isEmpty()) addDigest(h, "md5", cmd5);
    }
    const QByteArray xs1 = reply->rawHeader("X-Checksum-Sha1");
    if (!xs1.isEmpty()) addDigest(h, "sha-1", xs1);
    const QByteArray xs256 = reply->rawHeader("X-Checksum-Sha256");
    if (!xs256.isEmpty()) addDigest(h, "sha-256", xs256);
    const QByteArray xmd5 = reply->rawHeader("X-Checksum-Md5");
    if (!xmd5.isEmpty()) addDigest(h, "md5", xmd5);
    const QByteArray goog = reply->rawHeader("x-goog-hash");
    if (!goog.isEmpty()) parseDigestList(h, goog);      // "crc32c=..., md5=..."
    return h;
}

bool isStrongEtag(const QString &etag)
{
    return !etag.isEmpty() && !etag.startsWith(QLatin1String("W/")) && etag.startsWith(QLatin1Char('"'));
}

bool looksLikeTorrentData(const QByteArray &head)
{
    // A .torrent is a bencoded dictionary: "d" <len> ":" <key> ... with keys like announce / info /
    // created by / comment / creation date / url-list.
    static const QRegularExpression re(QStringLiteral("^d(\\d{1,3}):([a-z][a-z0-9 ._-]*)"));
    const auto m = re.match(QString::fromLatin1(head.left(96)));
    if (!m.hasMatch()) return false;
    const int len = m.captured(1).toInt();
    const QString key = m.captured(2).left(len);
    static const QStringList known = {QStringLiteral("announce"), QStringLiteral("announce-list"), QStringLiteral("info"),
                                      QStringLiteral("comment"), QStringLiteral("created by"), QStringLiteral("creation date"),
                                      QStringLiteral("encoding"), QStringLiteral("url-list"), QStringLiteral("nodes"),
                                      QStringLiteral("httpseeds"), QStringLiteral("publisher"), QStringLiteral("piece layers")};
    for (const QString &k : known)
        if (k.startsWith(key) || key.startsWith(k.left(len))) return true;
    return false;
}

QString contentTypeOf(const QNetworkReply *reply)
{
    QByteArray ct = reply ? reply->rawHeader("Content-Type") : QByteArray();
    const int semi = ct.indexOf(';');
    if (semi >= 0) ct.truncate(semi);
    return QString::fromLatin1(ct.trimmed()).toLower();
}

qint64 contentLengthOf(const QNetworkReply *reply)
{
    if (!reply || !reply->hasRawHeader("Content-Length")) return -1;
    bool ok = false;
    const qint64 n = reply->rawHeader("Content-Length").trimmed().toLongLong(&ok);
    return ok && n >= 0 ? n : -1;
}

int statusOf(const QNetworkReply *reply)
{
    return reply ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
}

QUrl parseUrl(const QString &s)
{
    return QUrl(s.trimmed(), QUrl::TolerantMode);
}

bool isRedirectStatus(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

QUrl RedirectTracker::next(const QUrl &from, const QNetworkReply *reply, const HttpConfig &cfg, QString *error)
{
    return next(from, reply ? reply->rawHeader("Location") : QByteArray(), cfg, error);
}

QUrl RedirectTracker::next(const QUrl &from, const QByteArray &loc, const HttpConfig &cfg, QString *error)
{
    seen.insert(from.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded));
    if (loc.trimmed().isEmpty()) { *error = QStringLiteral("Redirect without a Location header"); return {}; }
    QStringDecoder dec(QStringDecoder::Utf8);
    QString locStr = dec(loc);
    if (dec.hasError()) locStr = QString::fromLatin1(loc);
    const QUrl to = from.resolved(parseUrl(locStr)).adjusted(QUrl::RemoveFragment);
    const QString sch = to.scheme().toLower();
    if (!to.isValid() || (sch != QLatin1String("http") && sch != QLatin1String("https"))) {
        *error = QStringLiteral("Unsupported redirect target: %1").arg(QString::fromLatin1(loc.left(200)));
        return {};
    }
    if (++hops > cfg.maxRedirects) {
        *error = QStringLiteral("Too many redirects (more than %1)").arg(cfg.maxRedirects);
        return {};
    }
    const QString key = to.toString(QUrl::FullyEncoded);
    if (seen.contains(key)) { *error = QStringLiteral("Redirect loop at %1").arg(key); return {}; }
    if (from.scheme().toLower() == QLatin1String("https") && sch == QLatin1String("http") && !cfg.allowInsecureRedirect) {
        *error = QStringLiteral("Refused insecure redirect from HTTPS to HTTP (%1)").arg(key);
        return {};
    }
    seen.insert(key);
    return to;
}

QString describeError(const QNetworkReply *reply)
{
    if (!reply) return QStringLiteral("No reply");
    const int st = statusOf(reply);
    switch (reply->error()) {
    case QNetworkReply::ConnectionRefusedError: return QStringLiteral("Connection refused");
    case QNetworkReply::RemoteHostClosedError: return QStringLiteral("Connection closed by the server");
    case QNetworkReply::HostNotFoundError: return QStringLiteral("Host not found: %1").arg(reply->url().host());
    case QNetworkReply::TimeoutError: return QStringLiteral("Connection timed out");
    case QNetworkReply::OperationCanceledError: return QStringLiteral("Cancelled");
    case QNetworkReply::SslHandshakeFailedError: return QStringLiteral("TLS error: %1").arg(reply->errorString());
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::ProxyAuthenticationRequiredError: return QStringLiteral("Proxy error: %1").arg(reply->errorString());
    default: break;
    }
    if (st >= 400) {
        const QString phrase = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        return QStringLiteral("HTTP %1 %2").arg(st).arg(phrase).trimmed();
    }
    return reply->errorString();
}

bool isTransientNetworkError(int e)
{
    switch (QNetworkReply::NetworkError(e)) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::InternalServerError:
    case QNetworkReply::ServiceUnavailableError:
    case QNetworkReply::UnknownServerError:
    case QNetworkReply::ProtocolFailure:
        return true;
    default:
        return false;
    }
}

} // namespace mx
