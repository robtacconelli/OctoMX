// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX web module: a SearchBackend driven by a ProviderDef: HTTP(S) request, JSON/RSS/XML/HTML
// response (private).
#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include <deque>
#include <functional>

#include "mx/plugin.h"
#include "provider_def.h"
#include "response_parser.h"
#include "search_backend.h"

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace mx::web {

// Shared by all HTTP providers; owned by the plugin.
struct HttpEnvironment {
    QNetworkAccessManager *nam = nullptr;
    QString userAgent;
    bool verifyTls = true;
    mx::RateLimiter *limiter = nullptr;   // [glue-3] bandwidth graph accounting (Net::Web)
};
// [glue-3] account a reply's request + received bytes as Net::Web traffic
void meterReply(QNetworkReply *reply, mx::RateLimiter *limiter, qint64 requestBytes);

class HttpIndexProvider : public SearchBackend {
    Q_OBJECT
public:
    HttpIndexProvider(const ProviderDef &def, HttpEnvironment *env, QObject *parent = nullptr);
    ~HttpIndexProvider() override;

    const ProviderDef &def() const { return m_def; }
    void setDef(const ProviderDef &def);   // on reload (running jobs keep their copy)

    QString id() const override { return m_def.id; }
    QString displayName() const override { return m_def.name; }
    QString description() const override;
    QString address() const override;
    bool isUserDefined() const override { return m_def.userDefined; }

    bool accepts(const BackendQuery &q) const override;
    void search(const BackendQuery &q) override;
    void cancel(quint32 searchId) override;
    void cancelAll() override;
    void probe() override;

    // Request building (exposed for tests).
    static QString encodeQuery(const QString &text, const QString &spaceAs);
    static QString categoryValue(const ProviderDef &def, MediaType t);
    // [ui-f] records of a query sent with the type's own category are of that type when nothing else
    // says what they are ("classify by the index category")
    static void tagQueryCategory(const ProviderDef &def, const BackendQuery &q, QList<TorrentRecord> *records);
    static QString expandRequest(const QString &tmpl, const ProviderDef &def, const BackendQuery &q, int page, bool urlContext);

private:
    struct Job {
        quint64 serial = 0;
        quint32 searchId = 0;
        bool probe = false;
        BackendQuery q;
        ProviderDef def;            // snapshot
        int page = 0;
        int pagesDone = 0;
        int delivered = 0;
        QPointer<QNetworkReply> reply;
        QList<QPointer<QNetworkReply>> detailReplies;
        QElapsedTimer started;
        bool anyOk = false;
        QString lastError;
    };

    void enqueue(quint64 serial, std::function<void()> fn);
    void pumpQueue();
    void startPage(Job *job);
    void pageFinished(quint64 serial, QNetworkReply *reply, bool tooLarge, qint64 latency);
    void parsed(quint64 serial, const ParseResult &res, qint64 latency);
    void runDetails(quint64 serial, QList<TorrentRecord> records, QList<ItemContext> contexts, int rawItems);
    void deliver(Job *job, const QList<TorrentRecord> &records, int rawItems);
    void finishJob(Job *job, bool ok, const QString &error);
    QNetworkReply *send(const QNetworkRequest &req, const QString &method, const QByteArray &body, int maxBytes,
                        std::function<void(QNetworkReply *, bool tooLarge)> done);
    Job *job(quint64 serial) const { return m_jobs.value(serial, nullptr); }

    ProviderDef m_def;
    HttpEnvironment *m_env = nullptr;
    QHash<quint64, Job *> m_jobs;
    quint64 m_nextSerial = 1;
    std::deque<std::pair<quint64, std::function<void()>>> m_queue;
    QTimer m_gate;
    QElapsedTimer m_lastRequest;
};

} // namespace mx::web
