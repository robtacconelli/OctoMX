// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX [post-1]: the update check (see mx/update_check.h).
#include "mx/update_check.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include "mx/services.h"
#include "mx/settings_keys.h"

namespace mx {

namespace {

const QRegularExpression &versionRe()
{
    // v4, v4.2, v4.2.1 (a leading v or V; up to four numeric parts)
    static const QRegularExpression re(QStringLiteral("^[vV]?(\\d{1,5})(?:\\.(\\d{1,5}))?(?:\\.(\\d{1,5}))?(?:\\.(\\d{1,5}))?$"));
    return re;
}

} // namespace

QList<int> parseVersion(const QString &tag)
{
    const QRegularExpressionMatch m = versionRe().match(tag.trimmed());
    if (!m.hasMatch())
        return {};
    QList<int> out;
    for (int i = 1; i <= 4; ++i)
        if (m.capturedLength(i) > 0)
            out << m.captured(i).toInt();
    return out;
}

int compareVersions(const QString &a, const QString &b)
{
    const QList<int> va = parseVersion(a), vb = parseVersion(b);
    const int n = int(qMax(va.size(), vb.size()));
    for (int i = 0; i < n; ++i) {
        const int x = va.value(i, 0), y = vb.value(i, 0);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

QString displayVersion(const QString &version)
{
    QList<int> v = parseVersion(version);
    if (v.isEmpty())
        return version;
    while (v.size() > 2 && v.last() == 0)
        v.removeLast();
    if (v.size() == 1)
        v << 0;
    QStringList parts;
    for (int x : v)
        parts << QString::number(x);
    return parts.join(QLatin1Char('.'));
}

bool isPrereleaseTag(const QString &tag)
{
    static const QRegularExpression re(QStringLiteral("^[vV]?\\d+(?:\\.\\d+)*[-+._]?[A-Za-z]"));
    return re.match(tag.trimmed()).hasMatch();
}

bool parseLatestRelease(const QByteArray &json, ReleaseInfo *out, QString *error)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("not a JSON object");
        return false;
    }
    const QJsonObject o = doc.object();
    const QString tag = o.value(QStringLiteral("tag_name")).toString().trimmed();
    if (tag.isEmpty()) {
        if (error)
            *error = QStringLiteral("no tag_name");
        return false;
    }
    if (parseVersion(tag).isEmpty() && !isPrereleaseTag(tag)) {
        if (error)
            *error = QStringLiteral("tag_name '%1' is not a version").arg(tag.left(40));
        return false;
    }
    ReleaseInfo r;
    r.tag = tag;
    r.version = (tag.startsWith(QLatin1Char('v')) || tag.startsWith(QLatin1Char('V'))) ? tag.mid(1) : tag;
    r.name = o.value(QStringLiteral("name")).toString();
    r.body = o.value(QStringLiteral("body")).toString();
    r.htmlUrl = o.value(QStringLiteral("html_url")).toString();
    r.draft = o.value(QStringLiteral("draft")).toBool();
    r.prerelease = o.value(QStringLiteral("prerelease")).toBool() || isPrereleaseTag(tag);
    // [upd-1] the downloadable files of the release
    const QJsonArray assets = o.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue &v : assets) {
        const QJsonObject a = v.toObject();
        ReleaseAsset ra;
        ra.name = a.value(QStringLiteral("name")).toString();
        ra.url = a.value(QStringLiteral("browser_download_url")).toString();
        ra.size = a.contains(QStringLiteral("size")) ? qint64(a.value(QStringLiteral("size")).toDouble(-1)) : -1;
        if (!ra.name.isEmpty() && !ra.url.isEmpty() && ra.name.size() < 200)
            r.assets << ra;
    }
    *out = r;
    return true;
}

QString releaseNotesExcerpt(const QString &markdown, int maxLines, int maxChars)
{
    static const QRegularExpression heading(QStringLiteral("^#{1,6}\\s*"));
    static const QRegularExpression bullet(QStringLiteral("^\\s*(?:[-*+]|\\d+[.)])\\s+"));
    static const QRegularExpression link(QStringLiteral("!?\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression emph(QStringLiteral("(\\*\\*|__|\\*|`)"));
    static const QRegularExpression html(QStringLiteral("<[^>]{1,200}>"));
    QStringList lines;
    int chars = 0;
    bool inFence = false;
    QString text = markdown;
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    const QStringList src = text.split(QLatin1Char('\n'));
    for (QString l : src) {
        if (l.trimmed().startsWith(QLatin1String("```"))) {
            inFence = !inFence;
            continue;
        }
        if (inFence)
            continue;
        l.remove(html);
        const bool isBullet = bullet.match(l).hasMatch();
        l.remove(heading);
        l.remove(bullet);
        l.replace(link, QStringLiteral("\\1"));
        l.remove(emph);
        l = l.simplified();
        if (l.isEmpty() || l == QLatin1String("---") || l == QLatin1String("***"))
            continue;
        if (isBullet)
            l.prepend(QStringLiteral("* "));
        if (chars + l.size() > maxChars) {
            const int room = maxChars - chars;
            if (room > 20)
                lines << l.left(room).trimmed() + QStringLiteral("...");
            else if (!lines.isEmpty())
                lines.last() += QStringLiteral("...");
            break;
        }
        lines << l;
        chars += int(l.size());
        if (lines.size() >= maxLines) {
            if (src.size() > lines.size())
                lines.last() += QStringLiteral(" ...");
            break;
        }
    }
    return lines.join(QLatin1Char('\n'));
}

const ReleaseAsset *ReleaseInfo::asset(const QString &assetName) const
{
    for (const ReleaseAsset &a : assets)
        if (a.name == assetName)
            return &a;
    return nullptr;
}

QString releasePageUrl(const ReleaseInfo &r)
{
    const QUrl u(r.htmlUrl);
    if (u.isValid() && u.scheme() == QLatin1String("https") && u.host() == QLatin1String("github.com"))
        return u.toString();
    return QString::fromLatin1(kReleasesPage);
}

void applyUpdateProxy(QNetworkAccessManager *nam, Settings *settings)
{
    // the same rules as the web search plugin: SOCKS 5 / HTTP proxy from Settings > Connection
    QNetworkProxy proxy(QNetworkProxy::NoProxy);
    if (settings) {
        const QString type = settings->s(keys::connection::ProxyType).toLower();
        if (type == QLatin1String("socks5") || type == QLatin1String("http")) {
            proxy.setType(type == QLatin1String("http") ? QNetworkProxy::HttpProxy : QNetworkProxy::Socks5Proxy);
            proxy.setHostName(settings->s(keys::connection::ProxyHost));
            proxy.setPort(quint16(settings->i(keys::connection::ProxyPort)));
            if (settings->b(keys::connection::ProxyAuth)) {
                proxy.setUser(settings->s(keys::connection::ProxyUser));
                proxy.setPassword(settings->s(keys::connection::ProxyPassword));
            }
            QNetworkProxy::Capabilities caps = proxy.capabilities();
            if (settings->value(keys::connection::ProxyHostnames).toBool())
                caps |= QNetworkProxy::HostNameLookupCapability;
            else
                caps &= ~QNetworkProxy::HostNameLookupCapability;
            proxy.setCapabilities(caps);
            if (proxy.hostName().isEmpty())
                proxy = QNetworkProxy(QNetworkProxy::NoProxy);
        }
    }
    nam->setProxy(proxy);
}

// ============================================================================== UpdateChecker

UpdateChecker::UpdateChecker(Settings *settings, const QString &currentVersion, QObject *parent)
    : QObject(parent), m_settings(settings), m_current(currentVersion)
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (m_reply) {
            m_timedOut = true;
            m_reply->abort();
        }
    });
}

UpdateChecker::~UpdateChecker()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
}

QString UpdateChecker::userAgent() const
{
    return QStringLiteral("OctoMX/") + m_current;
}

void UpdateChecker::applyProxy()
{
    applyUpdateProxy(m_nam, m_settings);
}

void UpdateChecker::check()
{
    cancel();
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    applyProxy();
    const QString url = m_settings ? m_settings->s(keys::updates::Url).trimmed() : QString();
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    m_timedOut = false;
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::finished, this, &UpdateChecker::onFinished);
    m_timer->start(m_timeoutMs);
}

void UpdateChecker::cancel()
{
    m_timer->stop();
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
}

UpdateChecker::Result UpdateChecker::evaluate(const ReleaseInfo &r, const QString &currentVersion)
{
    Result res;
    res.release = r;
    res.current = currentVersion;
    if (r.draft || r.prerelease || parseVersion(r.tag).isEmpty()) {
        res.outcome = Outcome::NotOffered;
        return res;
    }
    const int c = compareVersions(r.version, currentVersion);
    res.outcome = c > 0 ? Outcome::Newer : c == 0 ? Outcome::UpToDate : Outcome::Older;
    return res;
}

void UpdateChecker::onFinished()
{
    QNetworkReply *reply = m_reply;
    if (!reply || sender() != reply)
        return;
    m_reply = nullptr;
    m_timer->stop();
    reply->deleteLater();
    Result res;
    res.current = m_current;
    res.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = m_timedOut ? QByteArray() : reply->readAll();
    if (m_timedOut) {
        res.timedOut = true;
        res.error = tr("The update server did not answer in time.");
    } else if (res.httpStatus == 429 || (res.httpStatus == 403 && reply->rawHeader("X-RateLimit-Remaining") == "0")) {
        res.rateLimited = true;
        res.error = tr("GitHub limits how often it can be asked. Please try again later.");
    } else if (res.httpStatus == 404) {
        res.error = tr("No published release was found.");
    } else if (reply->error() != QNetworkReply::NoError && res.httpStatus == 0) {
        res.error = tr("The update server could not be reached (%1).").arg(reply->errorString());
    } else if (res.httpStatus != 200) {
        res.error = tr("The update server answered with an error (HTTP %1).").arg(res.httpStatus);
    } else {
        ReleaseInfo r;
        QString why;
        if (!parseLatestRelease(body, &r, &why)) {
            res.error = tr("The update server sent an answer OctoMX does not understand.");
            qInfo("octomx: update check: malformed answer (%s)", qPrintable(why));
        } else {
            const int status = res.httpStatus;
            res = evaluate(r, m_current);
            res.httpStatus = status;
        }
    }
    if (res.outcome == Outcome::Failed && res.error.isEmpty())
        res.error = tr("The update check failed.");
    emit finished(res);
}

} // namespace mx
