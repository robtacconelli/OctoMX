// SPDX-License-Identifier: GPL-3.0-or-later
// eD2k servers: one TCP session (ServerConnection) and the list / auto-connect / UDP status
// logic around it (ServerManager).
#pragma once

#include "ed2k_connection.h"
#include "ed2k_opcodes.h"
#include "ed2k_search.h"
#include "ed2k_servermet.h"
#include "ed2k_util.h"

#include "mx/types.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QQueue>
#include <QtCore/QSet>
#include <QtCore/QTimer>

class QNetworkReply;
class QUdpSocket;

namespace mx::ed2k {

class Ed2kCore;

class ServerConnection : public QObject {
    Q_OBJECT
public:
    enum State { Connecting, LoggingIn, Connected, Closed };
    ServerConnection(Ed2kCore *core, const ServerInfo &info, bool obfuscated, QObject *parent = nullptr);
    ~ServerConnection() override;
    void start();
    void close(const QString &reason);

    State state() const { return m_state; }
    const ServerInfo &info() const { return m_info; }
    QString id() const { return m_info.id(); }
    bool obfuscated() const { return m_obfuscated; }
    quint32 clientId() const { return m_clientId; }
    bool lowId() const { return isLowId(m_clientId); }
    quint32 tcpFlags() const { return m_tcpFlags; }
    quint32 reportedIp() const { return m_reportedIp; }
    qint64 connectedForMs() const { return m_loginClock.isValid() ? m_loginClock.elapsed() : 0; }

    void sendSearch(const QByteArray &payload);
    void sendQueryMore();
    void sendGetSources(const QByteArray &hash, qint64 size);
    void sendOfferFiles(const QList<mx::SharedFile> &files, bool complete = true);
    void sendCallbackRequest(quint32 lowId);

signals:
    void loggedIn();
    void closed(const QString &reason);
    void serverMessage(const QString &text);
    void statusUpdated(quint32 users, quint32 files);
    void identUpdated(const QString &name, const QString &description);
    void searchResults(const QList<mx::ed2k::SearchResultEntry> &results, bool more);
    void sourcesFound(const QByteArray &fileHash, const QList<mx::ed2k::PeerDescriptor> &sources);
    void callbackRequested(quint32 ip, quint16 port, quint8 cryptOptions, const QByteArray &userHash);
    void callbackFailed();
    void rejected();
    void serverListReceived(const QList<mx::ed2k::ServerInfo> &servers);

private:
    void onConnected();
    void onPacket(const mx::ed2k::Packet &p);
    void send(quint8 opcode, const QByteArray &payload, bool allowPack = false);

    Ed2kCore *m_core;
    ServerInfo m_info;
    bool m_obfuscated;
    Ed2kConnection *m_conn = nullptr;
    State m_state = Connecting;
    quint32 m_clientId = 0;
    quint32 m_tcpFlags = 0;
    quint32 m_reportedIp = 0;
    QElapsedTimer m_loginClock, m_lastSend;
    QTimer m_loginTimeout, m_keepAlive;
};

// One keyword search: TCP on the connected server (+ "more" pages) and UDP on other servers.
struct ServerSearch {
    quint32 queryId = 0;
    QByteArray payload;
    int maxResults = 2000;
    int results = 0;
    int moreRequests = 0;
    bool tcpSent = false, tcpDone = false, udpDone = false;
    QList<QPair<quint32, quint16>> udpTargets;   // server ip/port still to ask
    qint64 createdMs = 0, lastUdpMs = 0, tcpSentMs = 0;
    QSet<QByteArray> seen;
};

class ServerManager : public QObject {
    Q_OBJECT
public:
    explicit ServerManager(Ed2kCore *core);
    ~ServerManager() override;

    void load();                 // dataDir/server.met (+ seed list)
    void save();
    void start();
    void stop();

    QList<mx::ServerEntry> entries() const;
    void addServer(const QString &address, const QString &name, bool isStatic = true);
    void removeServer(const QString &id);
    void connectServer(const QString &id);
    void disconnectServer(const QString &id);
    void reconnectCurrent();     // [diag-1] log in again (a new client ID after a port mapping)
    void fetchServerList(const QString &url);
    void mergeServers(const QList<ServerInfo> &list, bool fromTrustedSource);
    const QList<ServerInfo> &list() const { return m_servers; }

    ServerConnection *current() const;   // logged-in connection or nullptr
    bool isConnecting() const { return !m_attempts.isEmpty(); }
    QString statusText() const;

    // operations on the current server (no-ops if offline)
    void search(const mx::SearchQuery &q);
    void cancelSearch(quint32 queryId);
    void requestSources(const QByteArray &hash, qint64 size, bool urgent = false);
    void publishFiles(const QList<mx::SharedFile> &files);   // full list; incremental offers
    bool requestCallback(quint32 lowId, quint32 serverIp, quint16 serverPort);

    void handleUdp(quint32 ip, quint16 port, const QByteArray &datagram);

signals:
    void changed();                         // list or state changed
    void connectedChanged();
    void searchHits(quint32 queryId, const QList<mx::SearchHit> &hits);
    void searchDone(quint32 queryId);
    void sourcesFound(const QByteArray &fileHash, const QList<mx::ed2k::PeerDescriptor> &sources, const QString &origin);
    void callbackRequested(quint32 ip, quint16 port, quint8 cryptOptions, const QByteArray &userHash);
    void log(const QString &text);
    void serverMessage(const QString &serverId, const QString &text);   // [glue-2]

private:
    void tick();
    void autoConnect();
    void tryConnect(int index, bool manual);
    void onLoggedIn(ServerConnection *c);
    void onClosed(ServerConnection *c, const QString &reason);
    void onSearchResults(const QList<SearchResultEntry> &r, bool more);
    void finishTcpSearch(quint32 qid);
    void maybeFinishSearch(quint32 qid);
    void pumpSearch();
    void pumpUdpSearch();
    void pumpSources();
    void pumpUdpPings();
    void pumpOffers();
    void sendUdp(const ServerInfo &s, quint8 opcode, const QByteArray &payload);
    int indexOf(const QString &id) const;
    int indexOfIp(quint32 ip, quint16 port = 0) const;
    QList<int> connectOrder() const;
    void emitHits(ServerSearch &s, const QList<SearchResultEntry> &r);
    void pumpUdpSources();
    struct UdpSrcReq { quint32 ip; quint16 port; quint32 udpFlags; QByteArray hash; qint64 size; };
    QQueue<UdpSrcReq> m_udpSrcQueue;

    Ed2kCore *m_core;
    QList<ServerInfo> m_servers;
    QList<QPointer<ServerConnection>> m_attempts;
    QPointer<ServerConnection> m_current;
    bool m_running = false;
    bool m_manualDisconnect = false;
    int m_consecutiveFailures = 0;
    qint64 m_nextAutoConnectMs = 0;
    QTimer m_tick;
    QElapsedTimer m_clock;
    // searches
    QHash<quint32, ServerSearch> m_searches;
    QList<quint32> m_tcpQueue;
    quint32 m_tcpActive = 0;
    quint32 m_udpActive = 0;
    // sources
    struct SrcReq { QByteArray hash; qint64 size; };
    QQueue<SrcReq> m_srcQueue;
    QHash<QByteArray, qint64> m_srcLastAsked;
    qint64 m_lastSrcSentMs = 0;
    qint64 m_lastUdpSrcMs = 0;
    // offers
    QList<mx::SharedFile> m_shared;
    QSet<QByteArray> m_offered;
    qint64 m_lastOfferMs = 0;
    // udp pings
    int m_pingCursor = 0;
    qint64 m_lastPingMs = 0;
    bool m_fetching = false;
    qint64 m_lastSaveMs = 0;
    bool m_dirty = false;
};

} // namespace mx::ed2k
