// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: HTTP module internals: the URL probe / small-fetch state machine shared by
// HttpRangeSource::open() and the public HttpProbe helper.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include "http_plugin.h"
#include "http_util.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace mx {

struct ProbeOutcome {
    HttpProbeResult r;
    bool permanent = false;     // retrying will not help (404, TLS, redirect loop, ...)
    bool busy = false;          // 429 / 503: wait r.retryAfterSec (or back off) and retry
    QByteArray sniff;           // first bytes of the body, if read
    QByteArray body;            // fetch mode only
};

class ProbeJob : public QObject {
    Q_OBJECT
public:
    enum Mode { Probe, Fetch };
    ProbeJob(QNetworkAccessManager *nam, const HttpConfig &cfg, const HttpCredentials &cred, QObject *parent = nullptr);
    ~ProbeJob() override;
    // Probe: HEAD (→ GET bytes=0-N fallback / range check / sniff). Fetch: GET whole body ≤ maxBytes.
    void start(Mode mode, const QUrl &url, bool sniff = true, qint64 maxBytes = 0);
    void abort();
signals:
    void done(const mx::ProbeOutcome &o);

private:
    void sendHead(const QUrl &u);
    void sendGet(const QUrl &u);
    void onHeadFinished();
    void onGetMeta();
    void onGetData();
    void onGetFinished();
    void absorb(QNetworkReply *r, bool wholeEntity);
    void finish(bool ok, const QString &error = QString(), bool permanent = false);
    bool handleErrorStatus(int st, QNetworkReply *r);   // true if finished
    QNetworkReply *track(QNetworkReply *r);

    QNetworkAccessManager *m_nam;
    HttpConfig m_cfg;
    HttpCredentials m_cred;
    Mode m_mode = Probe;
    bool m_wantSniff = true;
    qint64 m_maxBytes = 0;
    QUrl m_origUrl, m_url;
    RedirectTracker m_redir;
    QPointer<QNetworkReply> m_reply;
    QTimer m_overall;
    ProbeOutcome m_out;
    QString m_cdName;
    bool m_rangesKnown = false;
    bool m_headDone = false;
    bool m_getMetaDone = false;
    bool m_finished = false;
    int m_getStatus = 0;
};

} // namespace mx

Q_DECLARE_METATYPE(mx::ProbeOutcome)
