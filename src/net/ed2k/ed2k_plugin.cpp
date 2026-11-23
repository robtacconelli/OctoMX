// SPDX-License-Identifier: GPL-3.0-or-later
#include "ed2k_plugin.h"
#include "ed2k_plugin_impl.h"

#include <QtCore/QTimer>
#include "ed2k_io.h"
#include "ed2k_opcodes.h"
#include "ed2k_server.h"
#include "ed2k_source.h"
#include "ed2k_upload.h"
#include "kad/kad_plugin.h"

namespace mx::ed2k {

std::shared_ptr<Ed2kCore> makeSharedCore()
{
    return std::shared_ptr<Ed2kCore>(new Ed2kCore(nullptr));
}

Ed2kPlugin::Ed2kPlugin(QObject *parent) : Ed2kPlugin(makeSharedCore(), parent) {}

Ed2kPlugin::Ed2kPlugin(std::shared_ptr<Ed2kCore> core, QObject *parent) : mx::NetworkPlugin(parent), m_core(std::move(core))
{
    wire();
}

bool Ed2kPlugin::primary() const
{
    return m_core->facadeActive(Ed2kCore::FacadeEd2k) || !m_core->facadeActive(Ed2kCore::FacadeKad);
}

void Ed2kPlugin::wire()
{
    Ed2kCore *c = m_core.get();
    connect(c, &Ed2kCore::statusChanged, this, [this] { emit statusChanged(status()); });
    connect(c, &Ed2kCore::serversChanged, this, &NetworkPlugin::serversChanged);
    connect(c, &Ed2kCore::searchHits, this, &NetworkPlugin::searchHits);
    connect(c, &Ed2kCore::searchDone, this, &NetworkPlugin::searchDone);
    connect(c, &Ed2kCore::sourcesFound, this, [this](const QString &id, const QList<mx::SearchSource> &list) {
        QList<mx::SearchSource> mine;
        for (const mx::SearchSource &s : list) if (s.net != mx::Net::Kad) mine << s;   // Kad sources go via the Kad plugin
        if (!mine.isEmpty()) emit sourcesFound(id, mine);
    });
    connect(c, &Ed2kCore::hashesLearned, this, [this](const QString &id, const mx::FileHashes &h) { if (primary()) emit hashesLearned(id, h); });
    connect(c, &Ed2kCore::hashSetLearned, this, [this](const QString &id, const QString &k, qint64 u, const QList<QByteArray> &hs) {
        if (primary()) emit hashSetLearned(id, k, u, hs);
    });
    connect(c, &Ed2kCore::uploadChanged, this, [this](const mx::TransferInfo &t) { if (primary()) emit uploadChanged(t); });
    connect(c, &Ed2kCore::uploadFinished, this, [this](const QString &id) { if (primary()) emit uploadFinished(id); });
    connect(c, &Ed2kCore::logMessage, this, [this](const QString &t) { if (primary()) emit logMessage(t); });
    connect(c, &Ed2kCore::serverMessage, this, &NetworkPlugin::serverMessage);   // [glue-2]
}

Ed2kPlugin::~Ed2kPlugin()
{
    if (m_started) m_core->release(Ed2kCore::FacadeEd2k);
}

void Ed2kPlugin::init(const mx::PluginContext &ctx)
{
    m_core->init(ctx);
    m_inited = true;
}

void Ed2kPlugin::start()
{
    if (!m_inited || m_started) return;
    m_started = true;
    m_core->acquire(Ed2kCore::FacadeEd2k);
}

void Ed2kPlugin::stop()
{
    if (!m_started) return;
    m_started = false;
    m_core->release(Ed2kCore::FacadeEd2k);
}

mx::NetworkStatus Ed2kPlugin::status() const
{
    mx::NetworkStatus st;
    st.net = mx::Net::ED2K;
    st.listenPort = m_core->tcpPort();
    if (!m_started) {
        st.state = mx::ConnState::Disconnected;
        st.statusText = QStringLiteral("Offline");
        return st;
    }
    ServerManager *sm = m_core->servers();
    ServerConnection *c = sm ? sm->current() : nullptr;
    st.statusText = sm ? sm->statusText() : QString();
    if (c) {
        st.state = mx::ConnState::Connected;
        st.peers = 1;
        if (c->info().users) st.usersOnline = c->info().users;
        if (c->info().files) st.filesOnline = c->info().files;
        st.firewalled = c->lowId() || !m_core->listening();
        if (c->reportedIp()) st.externalAddress = ipToString(c->reportedIp());
        const int up = m_core->uploads()->activeUploads();
        if (up) st.statusText += QStringLiteral(", %1 upload(s)").arg(up);
    } else {
        st.state = mx::ConnState::Connecting;
        st.firewalled = !m_core->listening();
    }
    st.rateDownBps = m_core->rateDownBps();
    st.rateUpBps = m_core->rateUpBps();
    return st;
}

QList<mx::ServerEntry> Ed2kPlugin::servers() const { return m_core->servers() ? m_core->servers()->entries() : QList<mx::ServerEntry>{}; }
void Ed2kPlugin::addServer(const QString &address, const QString &name) { if (m_core->servers()) m_core->servers()->addServer(address, name, true); }
void Ed2kPlugin::removeServer(const QString &id) { if (m_core->servers()) m_core->servers()->removeServer(id); }
void Ed2kPlugin::connectServer(const QString &id) { if (m_core->servers()) m_core->servers()->connectServer(id); }
void Ed2kPlugin::disconnectServer(const QString &id) { if (m_core->servers()) m_core->servers()->disconnectServer(id); }

// [glue-2] server.met import (plain or gzip'ed); text lists are left to the core (-1)
int Ed2kPlugin::importServers(const QByteArray &data)
{
    if (!m_core->servers()) return -1;
    QList<ServerInfo> list;
    if (!parseServerMetDownload(data, &list) || list.isEmpty()) return -1;
    m_core->servers()->mergeServers(list, true);
    m_core->servers()->save();
    emit serversChanged();
    return int(list.size());
}

void Ed2kPlugin::search(const mx::SearchQuery &q)
{
    if (!m_started || !m_core->servers()) {
        QMetaObject::invokeMethod(this, [this, id = q.id] { emit searchDone(id); }, Qt::QueuedConnection);
        return;
    }
    m_core->servers()->search(q);
}

void Ed2kPlugin::cancelSearch(quint32 queryId) { if (m_core->servers()) m_core->servers()->cancelSearch(queryId); }

void Ed2kPlugin::findSources(const QString &downloadId, const mx::FileHashes &h, qint64 size, const QString &name)
{
    m_core->findSources(downloadId, h, size, name);
}

void Ed2kPlugin::stopFindSources(const QString &downloadId) { m_core->stopFindSources(downloadId); }

mx::RangeSource *Ed2kPlugin::openSource(const QString &downloadId, const mx::FileHashes &h, qint64 size,
                                        const mx::SearchSource &src)
{
    if (src.net != mx::Net::ED2K && src.net != mx::Net::Kad && src.net != mx::Net::Unknown) return nullptr;
    return openEd2kSource(m_core.get(), downloadId, h, size, src);
}

mx::RangeSource *openEd2kSource(Ed2kCore *core, const QString &downloadId, const mx::FileHashes &h, qint64 size,
                                const mx::SearchSource &src)
{
    if (h.ed2k.size() != 16 || size <= 0 || size > MAX_FILE_SIZE) return nullptr;
    PeerDescriptor d;
    if (!parsePeerUri(src.uri, &d)) {
        // ed2k:// link (search hit) with a peer address
        const QString a = src.user.address;
        const int c = int(a.lastIndexOf(QLatin1Char(':')));
        if (c <= 0) return nullptr;
        bool ok = false;
        d.ip = ipFromString(a.left(c), &ok);
        const uint port = a.mid(c + 1).toUInt();
        if (!ok || port == 0 || port > 65535) return nullptr;
        d.port = quint16(port);
        const QByteArray uh = fromHex(src.user.id, 16);
        if (uh.size() == 16) d.userHash = uh;
    }
    if (d.fileHash.size() == 16 && d.fileHash != h.ed2k) return nullptr;
    if (!d.isLowId() && !d.needsKadCallback() && !core->acceptableSourceIp(d.ip)) return nullptr;
    core->ensureDownload(downloadId, h, size, QString());
    return new Ed2kRangeSource(core, downloadId, h, size, src, d);
}

void Ed2kPlugin::setSharedFiles(const QList<mx::SharedFile> &files) { m_core->setSharedFiles(files); }

bool Ed2kPlugin::startUploadNow(const QString &uploadId)
{
    return primary() && m_core->uploads() && m_core->uploads()->startUploadNow(uploadId);
}

bool Ed2kPlugin::setUploadPriority(const QString &uploadId, int priority)
{
    return primary() && m_core->uploads() && m_core->uploads()->setUploadPriority(uploadId, priority);
}

void Ed2kPlugin::cancelUpload(const QString &uploadId)
{
    if (primary() && m_core->uploads()) m_core->uploads()->cancelUpload(uploadId);
}

// ---- [diag-1] Connection Test
mx::PluginDiagnostics Ed2kPlugin::diagnostics() const
{
    mx::PluginDiagnostics d;
    d.valid = true;
    if (!m_started) return d;
    mx::PortBinding tcp;
    tcp.proto = QStringLiteral("TCP");
    tcp.port = m_core->tcpPort();
    tcp.ok = m_core->listening();
    tcp.error = m_core->tcpBindError();
    tcp.purpose = QStringLiteral("eD2k peers");
    d.bindings << tcp;
    mx::PortBinding udp;
    udp.proto = QStringLiteral("UDP");
    udp.port = m_core->udpLocalPort() ? m_core->udpLocalPort() : m_core->wantedUdpPort();
    udp.ok = m_core->udpLocalPort() != 0;
    udp.error = m_core->udpBindError();
    udp.purpose = QStringLiteral("eD2k client UDP and Kad");
    d.bindings << udp;
    ServerManager *sm = m_core->servers();
    ServerConnection *c = sm ? sm->current() : nullptr;
    if (c) {
        const QString name = c->info().name.isEmpty() ? c->id() : c->info().name;
        if (c->lowId()) {
            d.tcpIn = mx::Reach::No;
            d.tcpEvidence = QStringLiteral("server %1 assigned a LowID (%2): it could not connect back to TCP port %3").arg(name).arg(c->clientId()).arg(tcp.port);
        } else {
            d.tcpIn = mx::Reach::Yes;
            d.tcpEvidence = QStringLiteral("server %1 assigned a HighID").arg(name);
        }
        if (c->reportedIp()) {
            d.externalAddress = ipToString(c->reportedIp());
            d.externalSeenBy = QStringLiteral("eD2k server %1").arg(name);
        }
        d.items << mx::DiagItem{QStringLiteral("ed2k.id"), QStringLiteral("Client ID"), c->lowId() ? mx::DiagLevel::Limited : mx::DiagLevel::Ok,
                                c->lowId() ? QStringLiteral("LowID") : QStringLiteral("HighID"),
                                QStringLiteral("%1 on %2").arg(c->clientId()).arg(name),
                                c->lowId() ? QStringLiteral("A LowID means the server could not open a connection to your TCP port %1. You can still download, "
                                                            "but not from other LowID users, and you get fewer sources.").arg(tcp.port)
                                           : QString()};
    } else if (m_core->kadState().running && m_core->kadState().connected) {
        d.tcpIn = m_core->kadState().tcpFirewalled ? mx::Reach::No : mx::Reach::Yes;
        d.tcpEvidence = QStringLiteral("Kad firewall check");
    }
    if (m_core->kadState().running && m_core->kadState().udpChecked) {
        d.udpIn = m_core->kadState().udpOpen ? mx::Reach::Yes : mx::Reach::No;
        d.udpEvidence = QStringLiteral("Kad UDP firewall test on the shared UDP port");
    }
    {   // [enc-1] eMule protocol obfuscation
        int obf = 0, all = 0;
        m_core->obfuscationCounts(&obf, &all);
        mx::DiagItem e;
        e.id = QStringLiteral("ed2k.encryption");
        e.check = QStringLiteral("Obfuscation");
        const QString mode = !m_core->cryptSupported() ? QStringLiteral("off")
                             : m_core->cryptRequired() ? QStringLiteral("required") : QStringLiteral("on");
        if (!m_core->cryptSupported()) {
            e.level = mx::DiagLevel::Off;
            e.result = QStringLiteral("Off");
        } else {
            e.level = (c || all > 0) ? mx::DiagLevel::Ok : mx::DiagLevel::Unknown;
            e.result = c ? (c->obfuscated() ? QStringLiteral("Server link obfuscated") : QStringLiteral("Server link plain"))
                         : QStringLiteral("No server");
        }
        e.details = QStringLiteral("obfuscation %1, %2 of %3 obfuscated").arg(mode).arg(obf).arg(countNoun(all, QStringLiteral("peer connection")));
        e.explain = QStringLiteral("eMule protocol obfuscation hides the content of eD2k connections from simple filtering by a provider. "
                                   "Servers without an obfuscation port and peers whose user hash is unknown are reached plain unless "
                                   "obfuscation is required. It does not hide your IP address and it is not anonymity.");
        d.items << e;
    }
    d.connectAttempts = -1;
    return d;
}

void Ed2kPlugin::recheckConnectivity()
{
    ServerManager *sm = m_core->servers();
    ServerConnection *c = sm ? sm->current() : nullptr;
    if (c && c->lowId()) sm->reconnectCurrent();   // the server checks our port again at login
}

// A new TCP mapping while we are LowID: log in again, the server then sees the open port
void Ed2kPlugin::portMappingChanged(const mx::PortMapping &m)
{
    if (m.proto != QLatin1String("TCP") || m.result != mx::PortMapping::Mapped || m.internalPort != m_core->tcpPort()) return;
    ServerManager *sm = m_core->servers();
    ServerConnection *c = sm ? sm->current() : nullptr;
    if (!c || !c->lowId() || m_reconnectedForMapping) return;
    m_reconnectedForMapping = true;
    QTimer::singleShot(2000, this, [this] {
        ServerManager *sm = m_core->servers();
        if (sm && sm->current() && sm->current()->lowId()) sm->reconnectCurrent();
    });
}

} // namespace mx::ed2k

namespace mx {
QList<mx::NetworkPlugin *> createEd2kPlugins(QObject *parent)
{
    auto *ed2k = new mx::ed2k::Ed2kPlugin(parent);
    auto *kad = new mx::ed2k::kad::KadPlugin(ed2k->sharedCore(), parent);
    return {ed2k, kad};
}
} // namespace mx
