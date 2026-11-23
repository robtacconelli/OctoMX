// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX [upd-1]: downloads a release for the self-update and proves it is ours.
//
// From the release's assets (the browser_download_url of the API answer): SHA256SUMS (at most
// 64 KiB) and SHA256SUMS.sig (at most 4 KiB) first, the signature is checked against the built-in
// keys (mx/update_install.h), and only then the archive for this platform (at most the size the API
// states, and never more than maxArchiveBytes), into <dir>/<archive>.part; an interrupted download
// continues with a Range request (or starts again when the server ignores it). The finished file's
// SHA-256 must be the one of the signed list, then it is renamed to <dir>/<archive>.
//
// Addresses: https only, on GitHub's own hosts (github.com, api.github.com and the release asset
// hosts *.githubusercontent.com that github.com redirects to), redirects followed by hand (at most 5)
// and checked the same way. A mirror chosen on purpose in updates/url (tests: a local server) may
// also serve the files from exactly that scheme, host and port (plain http only on the loopback
// address). The proxy of Settings > Connection is used. Nothing else is sent: no cookies, the
// OctoMX/<version> user agent.
#pragma once

#include <QtCore/QFile>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QUrl>

#include <functional>

#include "mx/update_check.h"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace mx::upd {

class UpdateDownloader : public QObject {
    Q_OBJECT
public:
    struct Result {
        bool ok = false;
        QString error;            // plain text for the user
        QString archivePath;      // the verified archive
        QString archiveName;
        QString keyId;            // the key that signed SHA256SUMS
        bool unsignedRelease = false;   // no SHA256SUMS.sig (or no SHA256SUMS) in the release
        bool badSignature = false;      // signature or checksum wrong: the files were changed
        bool cancelled = false;
    };

    // dir = <profile>/updates
    UpdateDownloader(Settings *settings, const QString &dir, const QString &userAgent, QObject *parent = nullptr);
    ~UpdateDownloader() override;

    void setMaxArchiveBytes(qint64 n) { m_maxArchive = n; }
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }   // no data for this long = failure
    bool isRunning() const { return m_running; }
    // finished() follows exactly once (also after cancel())
    void start(const ReleaseInfo &release, const QString &platform = QString());
    void cancel();

    // the asset of this platform in the release (exact name first, else any isPackageArchiveName())
    static const ReleaseAsset *platformAsset(const ReleaseInfo &release, const QString &platform);
    // may an update file be fetched from u? checkUrl = the configured updates/url
    static bool urlAllowed(const QUrl &u, const QUrl &checkUrl, QString *why = nullptr);

signals:
    void progress(qint64 done, qint64 total);
    void finished(const mx::upd::UpdateDownloader::Result &result);

private:
    using Done = std::function<void(bool ok, const QByteArray &body, const QString &error)>;
    void fetch(const QUrl &url, qint64 maxBytes, QFile *sink, qint64 resumeFrom, const Done &done, int hops = 0);
    void finish(Result r);
    void downloadArchive();

    Settings *m_settings;
    QString m_dir;
    QString m_ua;
    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_reply;
    QTimer *m_idle = nullptr;
    qint64 m_maxArchive = qint64(400) * 1024 * 1024;
    int m_timeoutMs = 60000;
    bool m_running = false;
    int m_generation = 0;
    ReleaseInfo m_release;
    QString m_platform;
    QByteArray m_sums;
    QByteArray m_expected;
    QString m_keyId;
    const ReleaseAsset *m_asset = nullptr;
    QFile *m_part = nullptr;
};

} // namespace mx::upd
