// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX [upd-1]: the update download (see mx/update_download.h).
#include "mx/update_download.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "mx/update_install.h"

namespace mx::upd {

namespace {
constexpr qint64 kMaxSums = 64 * 1024;
constexpr qint64 kMaxSig = 4 * 1024;

bool isLoopback(const QString &host)
{
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0)
        return true;
    const QHostAddress a(host);
    return !a.isNull() && a.isLoopback();
}
} // namespace

UpdateDownloader::UpdateDownloader(Settings *settings, const QString &dir, const QString &userAgent, QObject *parent)
    : QObject(parent), m_settings(settings), m_dir(dir), m_ua(userAgent)
{
    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    connect(m_idle, &QTimer::timeout, this, [this] {
        if (m_reply) {
            m_reply->setProperty("mxTimedOut", true);
            m_reply->abort();
        }
    });
}

UpdateDownloader::~UpdateDownloader()
{
    ++m_generation;
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
    delete m_part;
}

bool UpdateDownloader::urlAllowed(const QUrl &u, const QUrl &checkUrl, QString *why)
{
    static const QStringList github = {QStringLiteral("github.com"), QStringLiteral("api.github.com"),
                                       QStringLiteral("objects.githubusercontent.com"),
                                       QStringLiteral("release-assets.githubusercontent.com"),
                                       QStringLiteral("github-releases.githubusercontent.com")};
    const QString host = u.host().toLower();
    if (u.isValid() && u.scheme() == QLatin1String("https") && github.contains(host) && u.userInfo().isEmpty())
        return true;
    // a mirror chosen on purpose in updates/url (tests: a local server): exactly its scheme, host, port
    const QUrl def(QStringLiteral("https://api.github.com/repos/robtacconelli/OctoMX/releases/latest"));
    if (u.isValid() && checkUrl.isValid() && checkUrl.host() != def.host() && u.userInfo().isEmpty() &&
        u.scheme() == checkUrl.scheme() && host == checkUrl.host().toLower() && u.port() == checkUrl.port()) {
        if (u.scheme() == QLatin1String("https"))
            return true;
        if (u.scheme() == QLatin1String("http") && isLoopback(host))
            return true;
    }
    if (why)
        *why = QStringLiteral("OctoMX only downloads updates over https from GitHub (%1 was refused).")
                   .arg(u.toDisplayString(QUrl::RemoveUserInfo | QUrl::RemoveQuery).left(120));
    return false;
}

const ReleaseAsset *UpdateDownloader::platformAsset(const ReleaseInfo &release, const QString &platform)
{
    if (const ReleaseAsset *a = release.asset(archiveName(release.version, platform)))
        return a;
    for (const ReleaseAsset &a : release.assets)
        if (isPackageArchiveName(a.name, platform))
            return &a;
    return nullptr;
}

void UpdateDownloader::cancel()
{
    if (!m_running)
        return;
    Result r;
    r.cancelled = true;
    r.error = tr("The download was cancelled.");
    finish(r);
}

void UpdateDownloader::finish(Result r)
{
    ++m_generation;
    m_idle->stop();
    if (m_reply) {
        QNetworkReply *rep = m_reply;
        m_reply = nullptr;
        rep->disconnect(this);
        rep->abort();
        rep->deleteLater();
    }
    if (m_part) {
        m_part->close();
        delete m_part;
        m_part = nullptr;
    }
    m_running = false;
    if (!r.ok)
        qInfo("octomx: update download: %s", qPrintable(r.error));
    emit finished(r);
}

void UpdateDownloader::start(const ReleaseInfo &release, const QString &platform)
{
    if (m_running)
        cancel();
    m_running = true;
    ++m_generation;
    m_release = release;
    m_platform = platform.isEmpty() ? platformTag() : platform;
    m_sums.clear();
    m_expected.clear();
    m_keyId.clear();
    m_asset = platformAsset(m_release, m_platform);
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    applyUpdateProxy(m_nam, m_settings);
    const ReleaseAsset *sums = m_release.asset(QStringLiteral("SHA256SUMS"));
    const ReleaseAsset *sig = m_release.asset(QStringLiteral("SHA256SUMS.sig"));
    const int gen = m_generation;
    // defer: finished() never comes from inside start()
    QTimer::singleShot(0, this, [this, gen, sums, sig] {
        if (gen != m_generation)
            return;
        Result r;
        if (!m_asset) {
            r.error = tr("The release has no package for this system (%1).").arg(m_platform);
            finish(r);
            return;
        }
        if (!sums || !sig) {
            r.unsignedRelease = true;
            r.error = tr("This release is not signed, so OctoMX does not install it by itself.");
            finish(r);
            return;
        }
        QDir().mkpath(m_dir);
        fetch(QUrl(sums->url), kMaxSums, nullptr, 0, [this, sig](bool ok, const QByteArray &body, const QString &err) {
            if (!ok) {
                Result r;
                r.error = tr("The checksum list could not be downloaded: %1").arg(err);
                finish(r);
                return;
            }
            m_sums = body;
            fetch(QUrl(sig->url), kMaxSig, nullptr, 0, [this](bool ok2, const QByteArray &sigBody, const QString &err2) {
                Result r;
                if (!ok2) {
                    r.error = tr("The release signature could not be downloaded: %1").arg(err2);
                    finish(r);
                    return;
                }
                const SigCheck sc = verifySums(m_sums, sigBody);
                if (!sc.ok) {
                    r.badSignature = true;
                    r.error = sc.error;
                    finish(r);
                    return;
                }
                m_keyId = sc.keyId;
                m_expected = parseSums(m_sums).value(m_asset->name);
                if (m_expected.isEmpty()) {
                    r.badSignature = true;
                    r.error = tr("The signed checksum list does not list %1.").arg(m_asset->name);
                    finish(r);
                    return;
                }
                qInfo("octomx: update: SHA256SUMS of %s signed by key %s", qPrintable(m_release.tag), qPrintable(m_keyId));
                downloadArchive();
            });
        });
    });
}

void UpdateDownloader::downloadArchive()
{
    Result r;
    r.archiveName = m_asset->name;
    r.keyId = m_keyId;
    const QString final = QDir(m_dir).filePath(m_asset->name);
    const QString part = final + QStringLiteral(".part");
    // a verified copy from an earlier run
    if (QFileInfo::exists(final)) {
        if (sha256OfFile(final) == m_expected) {
            r.ok = true;
            r.archivePath = final;
            qInfo("octomx: update: %s already downloaded and verified", qPrintable(m_asset->name));
            finish(r);
            return;
        }
        QFile::remove(final);
    }
    qint64 limit = m_maxArchive;
    if (m_asset->size > 0) {
        if (m_asset->size > m_maxArchive) {
            r.error = tr("The package is larger than OctoMX accepts (%1 MB).").arg(m_asset->size / (1024 * 1024));
            finish(r);
            return;
        }
        limit = m_asset->size;
    }
    m_part = new QFile(part);
    if (m_part->exists() && m_part->size() > limit)
        m_part->remove();
    if (!m_part->open(QIODevice::ReadWrite)) {
        r.error = tr("Cannot write %1 (%2).").arg(QDir::toNativeSeparators(part), m_part->errorString());
        finish(r);
        return;
    }
    const qint64 have = m_part->size();
    m_part->seek(have);
    if (have > 0)
        qInfo("octomx: update: continuing %s at %lld bytes", qPrintable(m_asset->name), have);
    fetch(QUrl(m_asset->url), limit, m_part, have, [this, final, part](bool ok, const QByteArray &, const QString &err) {
        Result r;
        r.archiveName = m_asset->name;
        r.keyId = m_keyId;
        if (!ok) {
            r.error = tr("The package could not be downloaded: %1").arg(err);
            finish(r);
            return;
        }
        m_part->flush();
        m_part->close();
        delete m_part;
        m_part = nullptr;
        const QByteArray got = sha256OfFile(part);
        if (got != m_expected) {
            QFile::remove(part);
            r.badSignature = true;
            r.error = tr("The downloaded package does not match the signed checksum list: it was changed or damaged.");
            finish(r);
            return;
        }
        QFile::remove(final);
        if (!QFile::rename(part, final)) {
            r.error = tr("Cannot write %1.").arg(QDir::toNativeSeparators(final));
            finish(r);
            return;
        }
        r.ok = true;
        r.archivePath = final;
        qInfo("octomx: update: %s downloaded, SHA-256 matches the signed list", qPrintable(m_asset->name));
        finish(r);
    });
}

void UpdateDownloader::fetch(const QUrl &url, qint64 maxBytes, QFile *sink, qint64 resumeFrom, const Done &done, int hops)
{
    const QUrl check(m_settings ? m_settings->s(keys::updates::Url).trimmed() : QString());
    QString why;
    if (!urlAllowed(url, check, &why)) {
        done(false, {}, why);
        return;
    }
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, m_ua);
    req.setRawHeader("Accept", "application/octet-stream");
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    if (sink && resumeFrom > 0)
        req.setRawHeader("Range", "bytes=" + QByteArray::number(resumeFrom) + "-");
    QNetworkReply *rep = m_nam->get(req);
    m_reply = rep;
    m_idle->start(m_timeoutMs);
    const int gen = m_generation;
    auto buffer = std::make_shared<QByteArray>();
    auto state = std::make_shared<int>(0);   // 0 unknown, 1 body (200 / 206), 2 redirect / error status
    auto received = std::make_shared<qint64>(sink ? resumeFrom : 0);
    auto failed = std::make_shared<QString>();
    connect(rep, &QNetworkReply::readyRead, this, [=] {
        if (gen != m_generation)
            return;
        m_idle->start(m_timeoutMs);
        const int status = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (*state == 0) {
            if (status == 200 || status == 206) {
                *state = 1;
                if (sink && status == 200 && resumeFrom > 0) {   // the server ignored the Range: start again
                    sink->resize(0);
                    sink->seek(0);
                    *received = 0;
                }
            } else {
                *state = 2;
            }
        }
        const QByteArray chunk = rep->readAll();
        if (*state != 1)
            return;
        *received += chunk.size();
        if (*received > maxBytes) {
            *failed = tr("the file is larger than expected");
            rep->abort();
            return;
        }
        if (sink) {
            if (sink->write(chunk) != chunk.size()) {
                *failed = tr("cannot write the file (%1)").arg(sink->errorString());
                rep->abort();
                return;
            }
            emit progress(*received, maxBytes);
        } else {
            *buffer += chunk;
        }
    });
    connect(rep, &QNetworkReply::finished, this, [=] {
        if (gen != m_generation)
            return;
        if (m_reply == rep)
            m_reply = nullptr;
        m_idle->stop();
        rep->deleteLater();
        const int status = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!failed->isEmpty()) {
            done(false, {}, *failed);
            return;
        }
        if (rep->property("mxTimedOut").toBool()) {
            done(false, {}, tr("the server stopped sending"));
            return;
        }
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            const QUrl next = url.resolved(QUrl(QString::fromUtf8(rep->rawHeader("Location"))));
            if (hops >= 5) {
                done(false, {}, tr("too many redirects"));
                return;
            }
            fetch(next, maxBytes, sink, resumeFrom, done, hops + 1);
            return;
        }
        if (status == 416 && sink && resumeFrom > 0) {   // the partial file is stale: once more from the start
            sink->resize(0);
            sink->seek(0);
            fetch(url, maxBytes, sink, 0, done, hops);
            return;
        }
        if (rep->error() != QNetworkReply::NoError && status == 0) {
            done(false, {}, rep->errorString());
            return;
        }
        if (status != 200 && status != 206) {
            done(false, {}, tr("HTTP %1").arg(status));
            return;
        }
        if (rep->error() != QNetworkReply::NoError) {
            done(false, {}, rep->errorString());
            return;
        }
        done(true, *buffer, QString());
    });
}

} // namespace mx::upd
