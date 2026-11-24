// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] PortMapper: the ONE place that asks the router to forward ports (UPnP IGD and NAT-PMP),
// for every listening network (eD2k, Kad, Gnutella, OctoMX overlay, Rats, BitTorrent). Plugins
// do not map ports themselves when PluginContext::coreMapsPorts is set.
//
// * Discovery: SSDP M-SEARCH for an Internet Gateway Device (description XML: friendly name,
//   model, the WANIPConnection / WANPPPConnection control URL; GetExternalIPAddress) and, in
//   parallel, a NAT-PMP external address request to the default gateway. UPnP wins when both answer.
// * One SOAP request at a time (several routers answer 500 to parallel requests).
// * AddPortMapping asks for external = internal port with a 1 h lease (renewed at half time; 0
//   = permanent when the router only accepts that). On a conflict (718) or any other error it
//   reads GetSpecificPortMappingEntry: a stale mapping that already points to this computer and
//   port is re-claimed (deleted and added again, same external port); one that belongs to another
//   computer makes us pick another external port, reported in PortMapping::externalPort.
// * Failures are retried with back-off (5 s, 15 s, 60 s, then every 10 min).
// * shutdown() deletes the mappings with a bounded wait; a failed delete at shutdown is logged as
//   such ("expires by itself"), never as a mapping failure.
#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QVariantMap>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>

#include <deque>
#include <functional>

#include "mx/plugin.h"

class QNetworkAccessManager;
class QNetworkReply;
class QUdpSocket;

namespace mx::diag {

struct RouterInfo {
    bool    searching = false;
    bool    searched = false;          // a discovery round finished
    bool    upnpFound = false;
    QString friendlyName, manufacturer, model;
    QString location;                  // description URL
    QString controlUrl;
    QString serviceType;               // urn:schemas-upnp-org:service:WANIPConnection:1
    QString address;                   // the IGD's IP
    QString localAddress;              // our address towards it (NewInternalClient)
    QString upnpWan;                   // GetExternalIPAddress
    QString upnpError;                 // discovery / description / external IP error
    int     ssdpReplies = 0;
    bool    natPmpAnswered = false;
    QString natPmpWan;
    QString natPmpError;
    QString gateway;
    QDateTime when;
    // every internet gateway device that answered (the chosen one first); "name at ip, WAN x"
    QStringList others;
    // [diag-1 follow-up] what discovery tried (for the "nothing answered" diagnosis)
    QStringList adapters;              // "Wi-Fi 192.168.1.20" per adapter address searched ...
    QList<int>  adapterAnswers;        // ... and how many SSDP answers arrived on it
    QList<bool> adapterRouter;         // ... and whether an internet gateway answered there
    QString answeredOn;                // adapter (name and address) whose search got the router's answer
    QString foundBy;                   // "search", "announcement", "cache", "Windows"
    int     igdAnswers = 0;            // answers that named an internet gateway
    bool    unicastTried = false;      // M-SEARCH sent straight to the gateway too
    bool    searchSkipped = false;     // MX_UPNP_FORCE_WINDOWS=1: only the Windows API was asked
    bool    cacheTried = false;        // the description remembered from an earlier start
    bool    cacheWorked = false;
    bool    notifyListening = false;   // listening for router announcements (ssdp:alive)
    QString notifyError;
    bool    winTried = false;          // the Windows UPnP NAT API (fallback)
    bool    winNat = false;            // ... found a router: mappings go through it
    QString winResult;
    bool isGateway() const { return !address.isEmpty() && address == gateway; }
    QString method() const
    {
        return upnpFound ? QStringLiteral("UPnP") : winNat ? QStringLiteral("UPnP (Windows)")
                                                           : natPmpAnswered ? QStringLiteral("NAT-PMP") : QString();
    }
    QString wan() const { return !upnpWan.isEmpty() ? upnpWan : natPmpWan; }
};

class PortMapper : public QObject {
    Q_OBJECT
public:
    struct Want {
        Net net = Net::Unknown;
        QString proto;                 // "TCP" / "UDP"
        int port = 0;
        QString label;                 // "eD2k + Kad" when shared
    };
    explicit PortMapper(QObject *parent = nullptr);
    ~PortMapper() override;

    // offReason: what the Off rows say (default: switched off in the settings)
    void setEnabled(bool upnp, bool natpmp, const QString &offReason = QString());
    // [net-2] the chosen adapter (connection/bindInterface): search, NAT-PMP and the router's
    // control requests use only its address, and nothing runs while it is missing. Empty: all.
    void setAdapter(const QString &wanted);
    bool upnpEnabled() const { return m_upnp; }
    bool natPmpEnabled() const { return m_natpmp; }
    // The complete set of ports to forward; ports no longer wanted are deleted on the router.
    void setWanted(const QList<Want> &wants);
    QList<PortMapping> mappings() const;
    RouterInfo router() const { return m_router; }
    bool busy() const;                 // discovery or requests in flight
    // Test Now: discover again and re-add every mapping.
    void refresh();
    // Delete every mapping we made (bounded wait, spins an event loop). Afterwards idle.
    void shutdown(int timeoutMs);

    // tests: skip SSDP and use this description URL / NAT-PMP endpoint
    void setTestDescriptionUrl(const QUrl &u) { m_testDescription = u; }
    void setTestNatPmp(const QHostAddress &a, quint16 port) { m_testPmpAddr = a; m_testPmpPort = port; }
    void setTestRetryScale(double s) { m_retryScale = s; }
    void setTestInternalClient(const QString &ip) { m_testClient = ip; }
    // tests: search on these "adapters" (name, address) and send the searches to target:port
    // instead of the SSDP group; listen for announcements on notifyPort (127.0.0.1)
    void setTestSsdp(const QList<QPair<QString, QHostAddress>> &adapters, const QHostAddress &target, quint16 port)
    {
        m_testAdapters = adapters;
        m_testSsdpTarget = target;
        m_testSsdpPort = port;
    }
    void setTestNotifyPort(quint16 port) { m_testNotifyPort = port; }
    void setTestWindowsFallback(bool available) { m_testWin = true; m_testWinAvailable = available; }   // simulate the Windows API answer

    // Description / control URL per gateway address remembered between starts (a shortcut only:
    // the random path may change after a router reboot). Saved by the owner (settings).
    void setCache(const QVariantMap &c) { m_cache = c; }
    QVariantMap cache() const { return m_cache; }

signals:
    void mappingChanged(const mx::PortMapping &m);
    void routerChanged();
    void cacheUpdated();

private:
    struct Entry {
        Want want;
        PortMapping m;
        int  attempts = 0;
        qint64 nextTryMs = 0;
        bool permanentOnly = false;
        int  tryExternal = 0;          // external port to ask for (0 = internal)
        int  conflicts = 0;
        bool remove = false;           // not wanted any more: delete, then forget
    };
    void startDiscovery();
    void onSsdpReady(QUdpSocket *sock);
    void sendSearches(int round);
    void ensureSearchSockets();
    void ensureNotifySocket();
    void onNotifyReady();
    void checkNetworkChange();
    void recordAdapters();
    void winFallback();
    void winAdd(const QString &key);
    void winRemove(const QString &proto, int external, std::function<void(bool, const QString &)> done);
    void ssdpDone();
    void fetchDescription(const QUrl &url);
    void natPmpStart();
    void onPmpReady();
    void discoveryFinished();
    void pump();
    void scheduleRetry(Entry &e, const QString &error);
    void addUpnp(const QString &key);
    void checkSpecific(const QString &key, const QString &addError);
    void deleteUpnp(const QString &proto, int external, std::function<void(bool, const QString &)> done);
    void mapPmp(const QString &key, int lifetime);
    using SoapDone = std::function<void(bool ok, const QHash<QString, QString> &out, const QString &error)>;
    void soap(const QString &action, const QList<QPair<QString, QString>> &args, SoapDone done);
    void soapTo(const QString &controlUrl, const QString &serviceType, const QString &action,
                const QList<QPair<QString, QString>> &args, SoapDone done);
    void chooseRouter();
    void finishRequest();
    void setResult(Entry &e, PortMapping::Result r, const QString &error);
    QString keyOf(const QString &proto, int port) const { return proto + QLatin1Char('/') + QString::number(port); }
    qint64 now() const;
    QString internalClient() const;

    bool m_upnp = true, m_natpmp = true;
    bool m_shuttingDown = false;
    RouterInfo m_router;
    QHash<QString, Entry> m_entries;
    QStringList m_order;
    QNetworkAccessManager *m_nam = nullptr;
    struct SearchSocket { QString name; QHostAddress addr; QUdpSocket *sock = nullptr; int answers = 0; };
    QList<SearchSocket> m_search;       // one per adapter IPv4 address
    QString m_searchKey;                // the adapter list the sockets were made for
    QUdpSocket *m_notify = nullptr;     // passive: router announcements on 239.255.255.250:1900
    QTimer m_netTimer;
    QSet<QString> m_fetched;            // description URLs fetched in this round
    QHash<QString, QString> m_locIface; // description URL -> adapter that heard it
    QHash<QString, QString> m_locSource;// description URL -> "search" / "announcement" / "cache"
    QList<QUrl> m_seeds;                // locations to try first in the next round (announcement)
    int m_failRounds = 0;
    QVariantMap m_cache;
    QList<QPair<QString, QHostAddress>> m_testAdapters;
    QString m_adapter;   // [net-2]
    QHostAddress m_testSsdpTarget;
    quint16 m_testSsdpPort = 0;
    quint16 m_testNotifyPort = 0;
    bool m_testWin = false, m_testWinAvailable = false;
    QUdpSocket *m_pmp = nullptr;
    QTimer m_ssdpTimer, m_pmpTimer, m_pump;
    int m_pmpTries = 0;
    QList<QUrl> m_locations;
    QList<RouterInfo> m_candidates;     // IGDs whose description (and WAN address) arrived
    int m_wanPending = 0;
    int m_descPending = 0;
    bool m_requestBusy = false;
    bool m_pmpDiscovering = false;
    QHash<int, QString> m_pmpWaiting;   // NAT-PMP: (op << 16 | internal port) -> entry key
    QUrl m_testDescription;
    QHostAddress m_testPmpAddr;
    quint16 m_testPmpPort = 0;
    QString m_testClient;
    double m_retryScale = 1.0;
    QString m_lastControl, m_lastService;   // the last router's control URL: removals during a new search
    qint64 m_epoch = 0;
};

} // namespace mx::diag
