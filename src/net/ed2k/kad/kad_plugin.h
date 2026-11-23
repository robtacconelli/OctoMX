// SPDX-License-Identifier: GPL-3.0-or-later
// KadPlugin: the Kad NetworkPlugin facade. Runs a KadNode on the shared eD2k engine: the node's
// UDP goes through the engine's socket, firewall checks and buddies use the engine's peer
// connections, sources found via Kad are opened with the eD2k RangeSource.
//
// Settings (read with fallbacks; schema keys first): kad/udpPort (shared socket, see Ed2kCore),
// kad/nodesDatUrl, kad/bootstrapNodes ("host:port" list), kad/autoBootstrap,
// plus kad/storeIndex, kad/firewallChecks, kad/publish, kad/maxPacketsPerSec and the test switches
// kad/allowLan, kad/allowSameIp, kad/forceFirewalled.
#pragma once

#include "kad_node.h"

#include "../ed2k_core.h"

#include "mx/plugin.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <memory>

namespace mx::ed2k {
class PeerConnection;
}

namespace mx::ed2k::kad {

class KadPlugin : public mx::NetworkPlugin, public KadHost {
    Q_OBJECT
public:
    explicit KadPlugin(std::shared_ptr<Ed2kCore> core, QObject *parent = nullptr);
    ~KadPlugin() override;

    mx::Net net() const override { return mx::Net::Kad; }
    int capabilities() const override { return CanSearch | CanFindSources | CanFetchRanges | CanServe; }
    void init(const mx::PluginContext &ctx) override;
    void start() override;
    void stop() override;
    mx::NetworkStatus status() const override;

    QList<mx::ServerEntry> servers() const override;
    void addServer(const QString &address, const QString &name) override;
    void removeServer(const QString &id) override;
    void connectServer(const QString &id) override;

    void search(const mx::SearchQuery &q) override;
    void cancelSearch(quint32 queryId) override;
    void findSources(const QString &downloadId, const mx::FileHashes &h, qint64 size, const QString &name) override;
    void stopFindSources(const QString &downloadId) override;
    mx::RangeSource *openSource(const QString &downloadId, const mx::FileHashes &h, qint64 size,
                                const mx::SearchSource &src) override;
    void setSharedFiles(const QList<mx::SharedFile> &files) override;
    // [glue-3] upload queue control (forwarded to the shared UploadManager)
    bool startUploadNow(const QString &uploadId) override;
    bool setUploadPriority(const QString &uploadId, int priority) override;
    void cancelUpload(const QString &uploadId) override;
    // [diag-1]
    mx::PluginDiagnostics diagnostics() const override;
    void recheckConnectivity() override;
    void portMappingChanged(const mx::PortMapping &m) override;

    KadNode *node() const { return m_node.get(); }
    Ed2kCore *core() const { return m_core.get(); }
    bool started() const { return m_started; }
    // download a nodes.dat now (setting kad/nodesDatUrl, else the built-in list)
    void fetchNodesDat();
    int nodesDatFetches() const { return m_nodesFetches; }

    // KadHost
    qint64 kadNowMs() const override { return m_clock.elapsed(); }
    qint64 kadNowSecs() const override;
    void kadSendDatagram(quint32 ed2kIp, quint16 port, const QByteArray &bytes) override;
    QByteArray kadUserHash() const override { return m_core->userHash(); }
    quint16 kadTcpPort() const override { return m_core->tcpPort(); }
    quint16 kadUdpPort() const override { return m_core->udpLocalPort(); }
    quint8 kadCryptOptions() const override;
    quint32 kadPublicIp() const override { return m_core->publicIp(); }
    void kadLog(const QString &text) override { emit logMessage(text); }
    bool kadTcpFirewallCheck(quint32 ed2kIp, quint16 tcpPort, const QByteArray &userHash, quint8 cryptOptions) override;
    bool kadUdpFirewallCheck(quint32 ed2kIp, quint16 tcpPort, quint16 internPort, quint16 externPort, quint32 senderKey) override;
    bool kadRequestBuddy(quint32 ed2kIp, quint16 tcpPort, quint16 udpPort, const QByteArray &userHash, quint8 cryptOptions) override;
    bool kadIncomingBuddy(quint32 ed2kIp, quint16 tcpPort, const QByteArray &userHash) override;
    bool kadRelayCallback(const QByteArray &checkWire, const QByteArray &fileWire, quint32 requesterHostIp, quint16 requesterTcpPort) override;
    bool kadHasServedBuddy() const override;

private:
    void onTick();
    void onKadDatagram(quint32 ip, quint16 port, const QByteArray &plain, bool obfuscated, quint32 rk, quint32 sk);
    void onKadTcp(mx::ed2k::PeerConnection *pc, quint8 opcode, const QByteArray &payload);
    void onPeerHandshake(mx::ed2k::PeerConnection *pc);
    void onSources(const QString &downloadId, const QByteArray &fileHash, const QList<KadSource> &sources);
    void syncState();
    void fetchNext();
    bool primary() const;
    QStringList bootstrapSetting() const;

    std::shared_ptr<Ed2kCore> m_core;
    std::unique_ptr<KadNode> m_node;
    KadConfig m_cfg;
    mx::PluginContext m_ctx;
    bool m_inited = false, m_started = false;
    QTimer m_tick;
    QElapsedTimer m_clock;
    bool m_recheckedForMapping = false;   // [diag-1]
    int m_tickCount = 0;
    // nodes.dat download
    QStringList m_fetchQueue;
    bool m_fetching = false;
    int m_nodesFetches = 0;
    bool m_fetchedThisSession = false;
    // TCP helpers
    int m_fwChecksRunning = 0;
    QPointer<PeerConnection> m_buddyPc;           // our buddy (we are firewalled)
    qint64 m_buddyPingAtMs = 0;
    quint16 m_buddyUdpPort = 0;
    bool m_buddyConnecting = false;
    QPointer<PeerConnection> m_servedBuddyPc;     // we are the buddy of a firewalled node
    QByteArray m_expectBuddyHash;
    quint32 m_expectBuddyIp = 0;
    qint64 m_expectBuddyUntilMs = 0;
    qint64 m_servedPongAtMs = 0;
    QString m_lastStatusText;
};

} // namespace mx::ed2k::kad
