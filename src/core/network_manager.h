// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: NetworkService implementation ("NetworkManager").
// Owns the plugins handed to createRealCore (deletes them), builds each PluginContext (creating
// <profile>/<net>/), inits all plugins, starts the enabled ones (<net>/enabled), aggregates their
// status into the classic WinMX "Connection Status" box, passes the server lists through, keeps the screen
// name (identity/screenName; a WinMX-style "Guest123_45678" is generated on first run; names set
// without a "_12345" suffix get one, stable per installation) and pushes
// the library's shared-file list to every CanServe plugin.
#pragma once

#include <deque>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QTimer>

#include <functional>

#include "mx/plugin.h"
#include "mx/services.h"

namespace mx {

class Pinger;
namespace netpolicy { class Monitor; }

class NetworkManager : public NetworkService {
    Q_OBJECT
public:
    NetworkManager(Settings *settings, const QList<NetworkPlugin *> &plugins, QObject *parent = nullptr);
    ~NetworkManager() override;

    // Builds contexts and calls init() on every plugin (once). library/limiter may be null in tests.
    void initPlugins(const QString &profileDir, LibraryAccess *library, RateLimiter *limiter);
    void setUploadSlots(UploadSlots *pool) { m_uploadSlots = pool; }   // [net-3] before initPlugins()
    // Starts the enabled plugins if network/connectOnStart (or force).
    void startNetworks(bool force = false);
    void stopAll();                                  // stop() on every running plugin (shutdown)
    QList<NetworkPlugin *> plugins() const { return m_plugins; }
    NetworkPlugin *plugin(Net n) const;
    bool isRunning(Net n) const;
    // Plugins that are enabled AND started, optionally filtered by capability.
    QList<NetworkPlugin *> activePlugins(int capability = 0) const;
    void setSharedFiles(const QList<SharedFile> &files);   // forwards to CanServe plugins
    void setSourceCounter(std::function<int()> fn) { m_sourceCounter = std::move(fn); }
    // [engine-4] "S=" / secondary connections: peers we exchange data or queue slots with
    void setSecondaryCounter(std::function<int()> fn) { m_secondaryCounter = std::move(fn); }
    // [engine-4] the "primary connections" figure of one network (see NetworkSummary::primary)
    static int primaryOf(const NetworkStatus &st);
    static QString generateScreenName();             // "Guest123_45678"
    static QString sanitizeScreenName(const QString &name);
    // [ovl-2] the one screen name: "<base>_<suffix>" where the suffix is the one the OctoMX network derives
    // from the user key (NetworkPlugin::localUser), so the name shown here is the name others see
    QString canonicalScreenName(const QString &raw) const;
    QString identitySuffix() const;      // "12345" from the OctoMX identity, empty before init

    // ---- NetworkService
    QList<NetworkStatus> networks() const override;
    NetworkStatus status(Net n) const override;
    ConnState overallState() const override;
    QString overallHeadline() const override;
    QString overallDetail() const override;
    void connectAll() override;
    void disconnectAll() override;
    void setEnabled(Net n, bool on) override;
    bool isEnabled(Net n) const override;
    void connectNetwork(Net n) override;
    void disconnectNetwork(Net n) override;
    QList<ServerEntry> servers(Net n = Net::Unknown) const override;
    void addServer(Net n, const QString &address, const QString &name = QString()) override;
    void removeServer(Net n, const QString &id) override;
    void connectServer(Net n, const QString &id) override;
    void disconnectServer(Net n, const QString &id) override;
    QString screenName() const override;
    void setScreenName(const QString &name) override;
    QString titleSummary() const override;
    // [glue-2]
    NetworkSummary summary() const override;
    bool editServer(Net n, const QString &id, const QString &address, const QString &name = QString()) override;
    int importServerList(Net n, const QString &filePath) override;
    int importServerData(Net n, const QByteArray &data);     // the same from memory (tests)
    QList<ServerMessage> serverMessages(Net n = Net::Unknown) const override;
    void pingUser(const UserRef &u) override;
    void setPinger(Pinger *p) { m_pinger = p; }
    void noteOwnSearch();                                    // SearchManager started a search (SPM)
    void checkSummary();                                     // emits summaryChanged if it changed
    // [net-2] the connection policy (proxy, chosen adapter): why a network does not run now
    netpolicy::Monitor *policyMonitor() const { return m_policy; }
    Settings *settings() const { return m_settings; }
    QString blockedReason(Net n) const;

signals:
    void logMessage(mx::Net net, const QString &text);

private:
    void startPlugin(NetworkPlugin *p);
    void stopPlugin(NetworkPlugin *p);
    void onPluginStatus(NetworkPlugin *p);
    void emitOverallIfChanged();
    void applyPolicy();                  // [net-2] stop / start / restart what the policy changed

    Settings *m_settings;
    QList<NetworkPlugin *> m_plugins;
    QHash<NetworkPlugin *, bool> m_running;
    bool m_inited = false;
    UploadSlots *m_uploadSlots = nullptr;   // [net-3]
    bool m_online = false;               // user wants to be connected (Connect pressed / auto)
    std::function<int()> m_sourceCounter;
    std::function<int()> m_secondaryCounter;
    QString m_lastOverall;
    QList<SharedFile> m_lastShared;
    bool m_haveShared = false;
    // [glue-2]
    QHash<int, QList<ServerMessage>> m_messages;   // per Net
    std::deque<qint64> m_incoming;                       // ms timestamps of incoming searches (≤ 60 s)
    std::deque<qint64> m_ownSearches;
    Pinger *m_pinger = nullptr;
    QTimer m_summaryTimer;
    QString m_lastSummary;
    // [net-2]
    netpolicy::Monitor *m_policy = nullptr;
    QHash<NetworkPlugin *, QString> m_blocked;   // not running because of the policy: why
    QString m_policyPrint;
    QTimer m_policyTimer;
};

} // namespace mx
