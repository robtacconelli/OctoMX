// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX [post-1]: "Check for Updates", the version check against the latest GitHub release.
//
// One GET of updates/url (default https://api.github.com/repos/robtacconelli/OctoMX/releases/latest)
// with `Accept: application/vnd.github+json` and `User-Agent: OctoMX/<version>`; nothing else is
// sent (no cookies, no install id, no settings). The answer's tag_name (vX.Y[.Z]) is compared
// numerically with the running version; drafts and pre-releases (flags or a "-beta" style tag) are
// never offered. The proxy settings (connection/proxy*) are used like the web search plugin does.
// [upd-1] The answer's assets (name, browser_download_url, size) are kept: the signed self-update
// (mx/update_install.h, mx/update_download.h) downloads from them.
#pragma once

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace mx {

class Settings;

struct ReleaseAsset {
    QString name;         // "OctoMX-5.1-win64.zip", "SHA256SUMS", "SHA256SUMS.sig"
    QString url;          // browser_download_url
    qint64 size = -1;     // as the API states it (-1 unknown)
};

struct ReleaseInfo {
    QString tag;          // "v4.2.0"
    QString version;      // "4.2.0" (tag without the leading v)
    QString name;         // release title
    QString body;         // release notes (Markdown)
    QString htmlUrl;      // the release page
    bool draft = false;
    bool prerelease = false;
    QList<ReleaseAsset> assets;   // [upd-1]
    // the asset with this exact name, or nullptr
    const ReleaseAsset *asset(const QString &assetName) const;
};

// "v4.2", "4.2.1", "V4.2.0" -> {4, 2[, 1]}; empty for anything else (also "4.2.0-rc1": a
// pre-release suffix makes it no plain version).
QList<int> parseVersion(const QString &tag);
// numeric comparison, missing parts count as 0 ("4.1" == "4.1.0"): -1 a < b, 0 equal, 1 a > b;
// an unparseable side compares as 0.0.0
int compareVersions(const QString &a, const QString &b);
// "4.2.0" -> "4.2", "4.2.1" -> "4.2.1" (how the version is shown to the user)
QString displayVersion(const QString &version);
// a tag that has a pre-release suffix ("v4.2.0-beta1", "4.2rc1")
bool isPrereleaseTag(const QString &tag);
// parse the releases/latest JSON object; false + *error for anything malformed
bool parseLatestRelease(const QByteArray &json, ReleaseInfo *out, QString *error = nullptr);
// the first lines of Markdown release notes as plain text: headings, emphasis, links and list
// markers reduced to their text, at most `maxLines` non-empty lines and `maxChars` characters
QString releaseNotesExcerpt(const QString &markdown, int maxLines = 8, int maxChars = 600);
// the page "Open Download Page" opens: the release's own page when it is on github.com,
// otherwise the project's releases page
QString releasePageUrl(const ReleaseInfo &r);
inline constexpr const char *kReleasesPage = "https://github.com/robtacconelli/OctoMX/releases";
// [upd-1] the proxy of Settings > Connection (SOCKS 5 / HTTP, as the web search plugin uses it) for
// every update request: the check and the downloads
void applyUpdateProxy(QNetworkAccessManager *nam, Settings *settings);

class UpdateChecker : public QObject {
    Q_OBJECT
public:
    enum class Outcome {
        Newer,        // a newer release exists (result.release)
        UpToDate,     // the latest release is this version
        Older,        // the latest release is older than this build (a development build)
        NotOffered,   // the latest release is a draft or a pre-release
        Failed,       // network error, timeout, HTTP error, bad JSON (result.error)
    };
    struct Result {
        Outcome outcome = Outcome::Failed;
        ReleaseInfo release;
        QString current;          // the running version
        QString error;            // plain text for the user (Failed)
        int httpStatus = 0;
        bool rateLimited = false; // 403 with no requests left, or 429
        bool timedOut = false;
    };

    // `settings` gives updates/url and the proxy; `currentVersion` e.g. "4.1.0"
    UpdateChecker(Settings *settings, const QString &currentVersion, QObject *parent = nullptr);
    ~UpdateChecker() override;

    void setTimeoutMs(int ms) { m_timeoutMs = ms; }
    bool isRunning() const { return m_reply != nullptr; }
    QString userAgent() const;
    // start one check (a running one is cancelled); finished() follows exactly once
    void check();
    void cancel();

    // pure decision for a parsed answer (exposed for tests)
    static Result evaluate(const ReleaseInfo &r, const QString &currentVersion);

signals:
    void finished(const mx::UpdateChecker::Result &result);

private:
    void onFinished();
    void applyProxy();

    Settings *m_settings;
    QString m_current;
    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_reply;
    QTimer *m_timer = nullptr;
    int m_timeoutMs = 15000;
    bool m_timedOut = false;
};

} // namespace mx
