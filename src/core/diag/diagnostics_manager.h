// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] DiagnosticsManager: the real mx::DiagnosticsService ("Connection Test").
//
// * Owns the shared PortMapper: every 2 s it collects the listening ports of the running plugins
//   (NetworkPlugin::diagnostics().bindings) and keeps them forwarded while connection/upnp or
//   connection/natpmp is on; results go back to the owning plugins (portMappingChanged).
// * Every 5 s it derives the per-download sentence (diag_rules.h) and hands it to the engine
//   (TransferInfo::diagnosis).
// * The text log gets one block about adapters, listen results and mappings ~25 s after start,
//   and one summary line per torrent every 3 minutes while it has fewer than 3 connected peers.
// * report() is built from cached facts; startTest() runs the active checks with progress.
#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>

#include "core/diag/active_checks.h"
#include "core/diag/diag_rules.h"
#include "core/diag/net_env.h"
#include "core/diag/port_mapper.h"
#include "core/diag/win_firewall.h"
#include "mx/plugin.h"
#include "mx/services.h"

class QNetworkAccessManager;

namespace mx {

class NetworkManager;
class DownloadEngine;

class DiagnosticsManager : public DiagnosticsService {
    Q_OBJECT
public:
    DiagnosticsManager(Settings *settings, NetworkManager *network, DownloadEngine *engine, RateLimiter *limiter,
                       const QString &profileDir, QObject *parent = nullptr);
    ~DiagnosticsManager() override;

    void start();                  // after the plugins were started
    void shutdown(int timeoutMs);  // removes the router mappings (bounded wait)
    diag::PortMapper *mapper() const { return m_mapper; }
    TorrentBackend *torrents() const { return m_torrents; }
    void setTorrentBackend(TorrentBackend *tb) { m_torrents = tb; }

    // ---- DiagnosticsService
    DiagReport report() const override;
    void startTest() override;
    void cancelTest() override;
    bool isTesting() const override { return m_testRunning; }
    QString portCheckService() const override;
    void checkPortsOnline() override;
    QString downloadDiagnosis(const QString &transferId) const override;

    // for tests / mxcli
    void syncBindings();
    void updateVerdicts();
    void setTestTimings(int passiveWaitMs, int stepTimeoutMs) { m_passiveWaitMs = passiveWaitMs; m_stepTimeoutMs = stepTimeoutMs; }
    QString startupBlock() const;   // the text logged at start

private:
    struct NetFacts {
        NetworkPlugin *plugin = nullptr;
        NetworkStatus st;
        PluginDiagnostics d;
        bool enabled = false, running = false;
    };
    QList<NetFacts> gatherNets() const;
    DiagGroup generalGroup(const QList<NetFacts> &nets) const;
    DiagGroup networkGroup(const NetFacts &n) const;
    QList<DiagGroup> downloadGroups() const;
    QStringList hostsFor(const NetFacts &n) const;
    DiagItem mappingItem(const PortBinding &b, Net net) const;
    QString forwardAdvice(const QList<PortBinding> &bindings) const;
    QStringList recentProblems(int max) const;
    diag::DownloadContext downloadContext() const;
    void scheduleReportChanged();
    void testStep();
    void finishTest(bool cancelled);
    void progress(int pct, const QString &step);
    void runFirewallCheck();
    void logTorrentSummaries();
    void onMappingChanged(const PortMapping &m);
    void applyMapperConfig();
    void onProbeResult(quint32 token, const QList<PortProbe> &res);

    Settings *m_settings;
    NetworkManager *m_network;
    DownloadEngine *m_engine;
    RateLimiter *m_limiter;
    TorrentBackend *m_torrents = nullptr;
    QString m_profile;
    diag::PortMapper *m_mapper = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    QTimer m_bindTimer, m_verdictTimer, m_reportTimer, m_envTimer, m_summaryTimer;
    diag::NetEnv m_env;
    QDateTime m_started;
    bool m_startupLogged = false;
    // facts from the active checks
    struct Timed { diag::CheckResult r; QDateTime when; QString what; };
    QHash<QString, Timed> m_dns;          // host -> lookup
    QList<Timed> m_udpTrackers;
    QList<Timed> m_dhtPings;
    QList<Timed> m_https;
    QHash<int, Timed> m_portChecks;       // TCP port -> public checker answer
    bool m_portCheckRunning = false;
    QList<PortProbe> m_probes;            // overlay probe answers
    QDateTime m_probeWhen;
    bool m_probeAsked = false;
    diag::FirewallState m_firewall;
    bool m_firewallRunning = false;
    QHash<QString, diag::Verdict> m_verdicts;
    QHash<QString, qint64> m_lastSummaryLog;
    // the test
    bool m_testRunning = false;
    bool m_testCancel = false;
    int m_testStep = 0;
    int m_pending = 0;
    QDateTime m_lastTest;
    int m_testPercent = 0;
    QString m_testText;
    QTimer m_stepTimer;
    qint64 m_stepStartMs = 0;
    int m_passiveWaitMs = 20000;
    int m_stepTimeoutMs = 12000;
    quint32 m_probeToken = 1;
    QSet<quint32> m_probeTokens;
    QHash<QString, QList<NetworkPlugin *>> m_portOwners;   // "TCP/6881" -> plugins
};

} // namespace mx
