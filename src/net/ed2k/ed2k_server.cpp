// SPDX-License-Identifier: GPL-3.0-or-later
#include "ed2k_server.h"
#include "ed2k_core.h"
#include "ed2k_io.h"
#include "ed2k_opcodes.h"
#include "ed2k_tags.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <algorithm>

namespace mx::ed2k {

static const char *const kDefaultServerListUrl =
    "http://upd.emule-security.org/server.met https://emuling.gitlab.io/server.met";

// ================================================================ ServerConnection
ServerConnection::ServerConnection(Ed2kCore *core, const ServerInfo &info, bool obfuscated, QObject *parent)
    : QObject(parent), m_core(core), m_info(info), m_obfuscated(obfuscated)
{
    m_loginTimeout.setSingleShot(true);
    m_loginTimeout.setInterval(45000);
    connect(&m_loginTimeout, &QTimer::timeout, this, [this] { close(QStringLiteral("login timeout")); });
    m_keepAlive.setInterval(60000);
    connect(&m_keepAlive, &QTimer::timeout, this, [this] {
        // eMule-style keep-alive: an empty OP_OFFERFILES after 15 min of silence
        if (m_state == Connected && m_lastSend.isValid() && m_lastSend.elapsed() > 15 * 60 * 1000) {
            Writer w; w.u32(0);
            send(OP_OFFERFILES, w.data());
        }
    });
}

ServerConnection::~ServerConnection()
{
    if (m_conn) m_conn->disconnect(this);
}

void ServerConnection::start()
{
    m_conn = new Ed2kConnection(this);
    m_conn->setMeter(m_core->ctx().limiter);   // [glue-3] server traffic in the bandwidth graph
    m_conn->setPolicySettings(m_core->ctx().settings);   // [net-2] proxy / adapter
    m_conn->setIdleTimeoutMs(0);
    m_conn->setConnectTimeoutMs(20000);
    m_conn->setMaxPacket(4 * 1024 * 1024);
    connect(m_conn, &Ed2kConnection::connected, this, &ServerConnection::onConnected);
    connect(m_conn, &Ed2kConnection::packetReceived, this, &ServerConnection::onPacket);
    connect(m_conn, &Ed2kConnection::closed, this, [this](const QString &why) {
        if (m_state == Closed) return;
        m_state = Closed;
        m_loginTimeout.stop();
        m_keepAlive.stop();
        emit closed(why);
    });
    const quint16 port = (m_obfuscated && m_info.obfuscationTcpPort) ? m_info.obfuscationTcpPort : m_info.port;
    if (m_obfuscated) m_conn->useServerObfuscation();
    m_state = Connecting;
    m_loginTimeout.start();
    if (m_info.ip == 0 && !m_info.dynHost.isEmpty()) m_conn->connectToHost(m_info.dynHost, port);
    else m_conn->connectTo(m_info.ip, port);
}

void ServerConnection::close(const QString &reason)
{
    if (m_state == Closed) return;
    if (m_conn) m_conn->close(reason);   // emits closed() → our handler
    else { m_state = Closed; emit closed(reason); }
}

void ServerConnection::send(quint8 opcode, const QByteArray &payload, bool allowPack)
{
    if (!m_conn || m_state == Closed) return;
    const bool pack = allowPack && (m_tcpFlags & SRV_TCPFLG_COMPRESSION);
    m_conn->sendPacket(PROT_EDONKEY, opcode, payload, pack);
    m_lastSend.restart();
}

void ServerConnection::onConnected()
{
    m_state = LoggingIn;
    Writer w;
    w.hash16(m_core->userHash()).u32(0).u16(m_core->tcpPort());
    quint32 flags = SRVCAP_ZLIB | SRVCAP_NEWTAGS | SRVCAP_LARGEFILES | SRVCAP_UNICODE;
    if (m_core->cryptSupported()) flags |= SRVCAP_SUPPORTCRYPT;
    if (m_core->cryptRequested()) flags |= SRVCAP_REQUESTCRYPT;
    if (m_core->cryptRequired()) flags |= SRVCAP_REQUIRECRYPT;
    TagList tags;
    tags << Tag::makeStr(CT_NAME, m_core->nick())
         << Tag::makeInt(CT_VERSION, EDONKEYVERSION)
         << Tag::makeInt(CT_SERVER_FLAGS, flags)
         << Tag::makeInt(CT_EMULE_VERSION, m_core->emuleVersionTag());
    writeTagList(w, tags, false);
    send(OP_LOGINREQUEST, w.data());
}

void ServerConnection::onPacket(const Packet &p)
{
    Reader r(p.payload);
    switch (p.opcode) {
    case OP_SERVERMESSAGE: {
        const QString msg = r.str16();
        if (r.ok()) {
            for (const QString &line : msg.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts))
                emit serverMessage(line.trimmed().left(500));
        }
        break;
    }
    case OP_IDCHANGE: {
        const quint32 id = r.u32();
        if (!r.ok()) { close(QStringLiteral("bad OP_IDCHANGE")); return; }
        if (r.remaining() >= 4) m_tcpFlags = r.u32();
        if (r.remaining() >= 12) {
            r.u32();   // aux port
            const quint32 rip = r.u32();
            const quint32 obfPort = r.u32();
            if (!isLowId(rip)) m_reportedIp = rip;
            if (obfPort && obfPort < 65536) m_info.obfuscationTcpPort = quint16(obfPort);
        }
        if (!isLowId(id)) m_reportedIp = id;
        if (id == 0) { close(QStringLiteral("server refused the login (ID 0)")); return; }
        m_clientId = id;
        const bool first = (m_state != Connected);
        m_state = Connected;
        m_loginTimeout.stop();
        m_keepAlive.start();
        if (first) m_loginClock.start();
        emit loggedIn();
        break;
    }
    case OP_SERVERSTATUS: {
        const quint32 users = r.u32();
        const quint32 files = r.u32();
        if (r.ok()) { m_info.users = users; m_info.files = files; emit statusUpdated(users, files); }
        break;
    }
    case OP_SERVERIDENT: {
        r.hash16(); r.u32(); r.u16();
        TagList tags;
        if (!readTagList(r, &tags, 64)) break;
        QString name, desc;
        if (const Tag *t = findTag(tags, quint8(ST_SERVERNAME))) if (t->isStr()) name = t->str.left(256);
        if (const Tag *t = findTag(tags, quint8(ST_DESCRIPTION))) if (t->isStr()) desc = t->str.left(512);
        if (!name.isEmpty()) m_info.name = name;
        if (!desc.isEmpty()) m_info.description = desc;
        emit identUpdated(name, desc);
        break;
    }
    case OP_SEARCHRESULT: {
        QList<SearchResultEntry> res;
        bool more = false;
        if (parseSearchResults(p.payload, &res, &more)) emit searchResults(res, more);
        else emit searchResults({}, false);
        break;
    }
    case OP_FOUNDSOURCES:
    case OP_FOUNDSOURCES_OBFU: {
        const QByteArray hash = r.hash16();
        const quint8 count = r.u8();
        QList<PeerDescriptor> list;
        for (int i = 0; i < count && r.ok(); ++i) {
            PeerDescriptor d;
            const quint32 id = r.u32();
            d.port = r.u16();
            if (p.opcode == OP_FOUNDSOURCES_OBFU) {
                d.cryptOptions = r.u8();
                if (d.cryptOptions & 0x80) d.userHash = r.hash16();
                d.cryptOptions &= 0x7F;
            }
            if (!r.ok()) break;
            if (isLowId(id)) { d.lowId = id; d.serverIp = m_info.ip; d.serverPort = m_info.port; }
            else d.ip = id;
            d.fileHash = hash;
            if (d.isValid()) list.append(d);
        }
        if (hash.size() == 16) emit sourcesFound(hash, list);
        break;
    }
    case OP_CALLBACKREQUESTED: {
        const quint32 ip = r.u32();
        const quint16 port = r.u16();
        quint8 crypt = 0;
        QByteArray uh;
        if (r.remaining() >= 17) { crypt = r.u8(); uh = r.hash16(); }
        if (r.ok() && ip && port) emit callbackRequested(ip, port, crypt, uh);
        break;
    }
    case OP_CALLBACK_FAIL: emit callbackFailed(); break;
    case OP_REJECT: emit rejected(); break;
    case OP_SERVERLIST: {
        const quint8 n = r.u8();
        QList<ServerInfo> list;
        for (int i = 0; i < n && r.ok(); ++i) {
            ServerInfo s;
            s.ip = r.u32();
            s.port = r.u16();
            if (r.ok() && s.port && isRoutableIp(s.ip)) list.append(s);
        }
        if (!list.isEmpty()) emit serverListReceived(list);
        break;
    }
    case OP_USERS_LIST:
    case OP_DISCONNECT:
    default:
        break;
    }
}

void ServerConnection::sendSearch(const QByteArray &payload) { send(OP_SEARCHREQUEST, payload); }
void ServerConnection::sendQueryMore() { send(OP_QUERY_MORE_RESULT, QByteArray()); }

void ServerConnection::sendGetSources(const QByteArray &hash, qint64 size)
{
    Writer w;
    w.hash16(hash);
    if (size > 0xFFFFFFFFLL) {
        if (!(m_tcpFlags & SRV_TCPFLG_LARGEFILES)) return;
        w.u32(0).u64(quint64(size));
    } else {
        w.u32(quint32(size));
    }
    const bool obfu = m_core->cryptSupported() && (m_tcpFlags & SRV_TCPFLG_TCPOBFUSCATION);
    send(obfu ? OP_GETSOURCES_OBFU : OP_GETSOURCES, w.data());
}

void ServerConnection::sendCallbackRequest(quint32 lowId)
{
    Writer w; w.u32(lowId);
    send(OP_CALLBACKREQUEST, w.data());
}

void ServerConnection::sendOfferFiles(const QList<mx::SharedFile> &files, bool complete)
{
    const bool newTags = m_tcpFlags & SRV_TCPFLG_NEWTAGS;
    const bool intTypes = m_tcpFlags & SRV_TCPFLG_TYPETAGINTEGER;
    Writer w;
    w.u32(quint32(files.size()));
    for (const mx::SharedFile &f : files) {
        w.hash16(f.hashes.ed2k);
        if (m_tcpFlags & SRV_TCPFLG_COMPRESSION) {
            w.u32(complete ? FILE_COMPLETE_ID : FILE_INCOMPLETE_ID).u16(complete ? FILE_COMPLETE_PORT : FILE_INCOMPLETE_PORT);
        } else if (!lowId()) {
            w.u32(m_clientId).u16(m_core->tcpPort());
        } else {
            w.u32(0).u16(0);
        }
        TagList tags;
        tags << Tag::makeStr(FT_FILENAME, f.name);
        if (f.size > 0xFFFFFFFFLL) {
            Tag lo; lo.type = TAGTYPE_UINT32; lo.id = FT_FILESIZE; lo.num = quint64(f.size) & 0xFFFFFFFFull;
            tags << lo << Tag::makeInt(FT_FILESIZE_HI, quint64(f.size) >> 32);
        } else {
            tags << Tag::makeInt(FT_FILESIZE, quint64(f.size));
        }
        const mx::MediaType mt = f.type != mx::MediaType::Any ? f.type : mx::mediaTypeForFileName(f.name);
        const QString ts = ed2kTypeForMedia(mt);
        if (!ts.isEmpty()) {
            if (intTypes) {
                int id = 0;
                switch (mt) {
                case mx::MediaType::Audio: id = 1; break;
                case mx::MediaType::Video: id = 2; break;
                case mx::MediaType::Image: id = 3; break;
                case mx::MediaType::Software: case mx::MediaType::Archive: id = 4; break;
                case mx::MediaType::Document: id = 5; break;
                default: break;
                }
                if (id) tags << Tag::makeInt(FT_FILETYPE, quint64(id));
            } else {
                tags << Tag::makeStr(FT_FILETYPE, ts);
            }
        }
        writeTagList(w, tags, newTags);
    }
    send(OP_OFFERFILES, w.data(), true);
}

// ================================================================ ServerManager
ServerManager::ServerManager(Ed2kCore *core) : QObject(core), m_core(core)
{
    m_clock.start();
    m_tick.setInterval(500);
    connect(&m_tick, &QTimer::timeout, this, &ServerManager::tick);
}

ServerManager::~ServerManager() = default;

static QList<ServerInfo> seedServers()
{
    static const struct { const char *addr; const char *name; } seeds[] = {
        {"176.123.5.89:4725", "eMule Sunrise"},
        {"85.17.116.222:6082", "ed2k-rust"},
        {"91.208.162.87:4232", "!! Sharing-Devils No.4 !!"},
        {"85.121.5.137:4232", "!! Sharing-Devils No.2 !!"},
        {"91.208.162.182:4232", "MO-Server"},
        {"213.141.198.207:4232", "Mazinga Server"},
        {"77.42.68.79:4232", "Nordic-Server"},
        {"212.95.35.240:4232", "eMule-Cosmic"},
        {"176.125.242.230:5555", "emule-qt.org - eNode"},
        {"146.19.248.158:4325", "!! Sharing-Devils No.1 !!"},
    };
    QList<ServerInfo> out;
    for (const auto &s : seeds) {
        const QString a = QString::fromLatin1(s.addr);
        const int c = int(a.indexOf(QLatin1Char(':')));
        ServerInfo i;
        i.ip = ipFromString(a.left(c));
        i.port = quint16(a.mid(c + 1).toUInt());
        i.name = QString::fromLatin1(s.name);
        out.append(i);
    }
    return out;
}

void ServerManager::load()
{
    QFile f(QDir(m_core->dataDir()).filePath(QStringLiteral("server.met")));
    QList<ServerInfo> list;
    if (f.open(QIODevice::ReadOnly)) {
        QString err;
        if (!parseServerMet(f.read(16 * 1024 * 1024), &list, &err))
            emit log(QStringLiteral("server.met unreadable (%1), using built-in list").arg(err));
    }
    m_servers.clear();
    mergeServers(list, true);
    if (m_servers.isEmpty()) mergeServers(seedServers(), true);
    emit changed();
}

void ServerManager::save()
{
    QDir().mkpath(m_core->dataDir());
    QSaveFile f(QDir(m_core->dataDir()).filePath(QStringLiteral("server.met")));
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(writeServerMet(m_servers));
    f.commit();
    m_dirty = false;
    m_lastSaveMs = m_clock.elapsed();
}

void ServerManager::start()
{
    if (m_running) return;
    m_running = true;
    m_manualDisconnect = false;
    m_nextAutoConnectMs = 0;
    for (ServerInfo &s : m_servers) { s.nextRetryMs = 0; s.pingedThisSession = false; s.udpPingTries = 0; }
    m_tick.start();
    const bool autoUpdate = m_core->cfg().b(QStringLiteral("ed2k/autoUpdateServerList"), true);
    if (autoUpdate || m_servers.size() < 3) {
        // the configured list first, then our known-good fallbacks
        QStringList urls = m_core->cfg().str(QStringLiteral("ed2k/serverListUrl"), QString())
                               .split(QRegularExpression(QStringLiteral("[\\s;,]+")), Qt::SkipEmptyParts);
        for (const QString &u : QString::fromLatin1(kDefaultServerListUrl).split(QLatin1Char(' ')))
            if (!urls.contains(u)) urls << u;
        fetchServerList(urls.join(QLatin1Char(' ')));
    }
    tick();
}

void ServerManager::stop()
{
    m_running = false;
    m_tick.stop();
    for (auto &a : m_attempts) if (a) { a->disconnect(this); a->close(QStringLiteral("stopping")); a->deleteLater(); }
    m_attempts.clear();
    if (m_current) { m_current->disconnect(this); m_current->close(QStringLiteral("stopping")); m_current->deleteLater(); }
    m_current = nullptr;
    const QList<quint32> ids = m_searches.keys();
    m_searches.clear();
    m_tcpQueue.clear();
    m_tcpActive = m_udpActive = 0;
    for (quint32 id : ids) emit searchDone(id);
    m_srcQueue.clear();
    m_udpSrcQueue.clear();
    m_offered.clear();
    save();
    emit connectedChanged();
    emit changed();
}

ServerConnection *ServerManager::current() const
{
    return (m_current && m_current->state() == ServerConnection::Connected) ? m_current.data() : nullptr;
}

QString ServerManager::statusText() const
{
    if (ServerConnection *c = current()) {
        return QStringLiteral("Connected to %1 (%2)").arg(c->info().name.isEmpty() ? c->id() : c->info().name,
                                                         c->lowId() ? QStringLiteral("LowID") : QStringLiteral("HighID"));
    }
    if (!m_attempts.isEmpty()) return QStringLiteral("Connecting to %1 server(s)...").arg(m_attempts.size());
    return m_running ? QStringLiteral("Not connected") : QStringLiteral("Offline");
}

int ServerManager::indexOf(const QString &id) const
{
    for (int i = 0; i < m_servers.size(); ++i) if (m_servers[i].id() == id) return i;
    return -1;
}
int ServerManager::indexOfIp(quint32 ip, quint16 port) const
{
    for (int i = 0; i < m_servers.size(); ++i)
        if (m_servers[i].ip == ip && (port == 0 || m_servers[i].port == port)) return i;
    return -1;
}

QList<mx::ServerEntry> ServerManager::entries() const
{
    QList<mx::ServerEntry> out;
    for (const ServerInfo &s : m_servers) {
        mx::ServerEntry e;
        e.net = mx::Net::ED2K;
        e.id = s.id();
        e.name = s.name.isEmpty() ? s.address() : s.name;
        e.address = s.address();
        e.state = mx::ConnState::Disconnected;
        if (m_current && m_current->id() == e.id)
            e.state = m_current->state() == ServerConnection::Connected ? mx::ConnState::Connected : mx::ConnState::Connecting;
        for (const auto &a : m_attempts) if (a && a->id() == e.id) e.state = mx::ConnState::Connecting;
        e.users = s.users ? qint64(s.users) : -1;
        e.files = s.files ? qint64(s.files) : -1;
        e.pingMs = s.ping ? int(s.ping) : -1;
        e.description = s.description;
        if (isKnownBadServer(s)) e.description = QStringLiteral("[suspected fake] ") + e.description;
        e.isStatic = s.isStatic;
        out.append(e);
    }
    return out;
}

void ServerManager::mergeServers(const QList<ServerInfo> &list, bool trusted)
{
    int added = 0;
    for (const ServerInfo &n : list) {
        if (!n.isStatic && isKnownBadServer(n)) continue;
        if (n.port == 0) continue;
        int i = n.ip ? indexOfIp(n.ip, n.port) : indexOf(n.id());
        if (i >= 0) {
            ServerInfo &o = m_servers[i];
            if (!n.name.isEmpty() && (trusted || o.name.isEmpty())) o.name = n.name;
            if (!n.description.isEmpty() && (trusted || o.description.isEmpty())) o.description = n.description;
            if (n.users) o.users = n.users;
            if (n.files) o.files = n.files;
            if (n.maxUsers) o.maxUsers = n.maxUsers;
            if (n.softFiles) o.softFiles = n.softFiles;
            if (n.hardFiles) o.hardFiles = n.hardFiles;
            if (n.udpFlags) o.udpFlags = n.udpFlags;
            if (n.obfuscationTcpPort) o.obfuscationTcpPort = n.obfuscationTcpPort;
            if (n.obfuscationUdpPort) o.obfuscationUdpPort = n.obfuscationUdpPort;
            o.isStatic = o.isStatic || n.isStatic;
            o.failCount = qMin(o.failCount, n.failCount);
            continue;
        }
        if (m_servers.size() >= 500) break;
        m_servers.append(n);
        ++added;
    }
    if (added) m_dirty = true;
}

void ServerManager::fetchServerList(const QString &urls)
{
    if (m_fetching) return;
    QStringList list = urls.split(QRegularExpression(QStringLiteral("[\\s;,]+")), Qt::SkipEmptyParts);
    if (list.isEmpty()) return;
    m_fetching = true;
    auto tryNext = std::make_shared<std::function<void(QStringList)>>();
    *tryNext = [this, tryNext](QStringList remaining) {
        if (remaining.isEmpty()) { m_fetching = false; return; }
        const QString url = remaining.takeFirst();
        QNetworkRequest req{QUrl(url)};
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OctoMX/" MX_VERSION_STRING));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setTransferTimeout(30000);
        QNetworkReply *reply = m_core->nam()->get(req);
        connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 got, qint64) {
            if (got > 4 * 1024 * 1024) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, url, remaining, tryNext] {
            reply->deleteLater();
            QList<ServerInfo> list;
            QString err;
            const QByteArray body = reply->readAll();
            if (reply->error() == QNetworkReply::NoError && parseServerMetDownload(body, &list, &err) && !list.isEmpty()) {
                const int before = int(m_servers.size());
                const bool trusted = url.contains(QLatin1String("emule-security.org"));
                mergeServers(list, trusted);
                emit log(QStringLiteral("server list %1: %2 servers (%3 new)").arg(url).arg(list.size()).arg(m_servers.size() - before));
                save();
                emit changed();
                m_fetching = false;
                return;
            }
            emit log(QStringLiteral("server list %1 failed: %2").arg(url, reply->error() != QNetworkReply::NoError ? reply->errorString() : err));
            (*tryNext)(remaining);
        });
    };
    (*tryNext)(list);
}

void ServerManager::addServer(const QString &address, const QString &name, bool isStatic)
{
    QString a = address.trimmed();
    if (a.startsWith(QLatin1String("ed2k://|server|"), Qt::CaseInsensitive)) {
        const QStringList p = a.split(QLatin1Char('|'));
        if (p.size() >= 4) a = p[2] + QLatin1Char(':') + p[3];
    }
    const int c = int(a.lastIndexOf(QLatin1Char(':')));
    if (c <= 0) return;
    bool ok = false;
    const uint port = a.mid(c + 1).toUInt(&ok);
    if (!ok || port == 0 || port > 65535) return;
    ServerInfo s;
    bool isIp = false;
    s.ip = ipFromString(a.left(c), &isIp);
    if (!isIp) { s.ip = 0; s.dynHost = a.left(c); }
    s.port = quint16(port);
    s.name = name;
    s.isStatic = isStatic;
    const int existing = indexOf(s.id());
    if (existing >= 0) { m_servers[existing].isStatic = true; if (!name.isEmpty()) m_servers[existing].name = name; }
    else m_servers.append(s);
    save();
    emit changed();
}

void ServerManager::removeServer(const QString &id)
{
    const int i = indexOf(id);
    if (i < 0) return;
    if (m_current && m_current->id() == id) disconnectServer(id);
    m_servers.removeAt(i);
    save();
    emit changed();
}

void ServerManager::connectServer(const QString &id)
{
    const int i = indexOf(id);
    if (i < 0) return;
    m_manualDisconnect = false;
    if (m_current && m_current->id() == id) return;
    if (m_current) { ServerConnection *c = m_current; m_current = nullptr; c->disconnect(this); c->close(QStringLiteral("switching server")); c->deleteLater(); emit connectedChanged(); }
    for (auto &a : m_attempts) if (a) { a->disconnect(this); a->close(QStringLiteral("switching server")); a->deleteLater(); }
    m_attempts.clear();
    if (!m_running) { m_running = true; m_tick.start(); }
    tryConnect(i, true);
    emit changed();
}

void ServerManager::reconnectCurrent()
{
    if (!m_current) return;
    const QString id = m_current->id();
    log(QStringLiteral("reconnecting to %1 for a new client ID").arg(m_current->info().name.isEmpty() ? id : m_current->info().name));
    disconnectServer(id);
    connectServer(id);
}

void ServerManager::disconnectServer(const QString &id)
{
    bool any = false;
    if (m_current && (id.isEmpty() || m_current->id() == id)) {
        ServerConnection *c = m_current;
        m_current = nullptr;
        c->disconnect(this);
        c->close(QStringLiteral("disconnected by user"));
        c->deleteLater();
        any = true;
    }
    for (int k = int(m_attempts.size()) - 1; k >= 0; --k) {
        auto a = m_attempts[k];
        if (a && (id.isEmpty() || a->id() == id)) { a->disconnect(this); a->close(QStringLiteral("cancelled")); a->deleteLater(); m_attempts.removeAt(k); any = true; }
    }
    if (any) {
        m_manualDisconnect = true;
        m_offered.clear();
        emit connectedChanged();
        emit changed();
    }
}

QList<int> ServerManager::connectOrder() const
{
    const qint64 now = m_clock.elapsed();
    const bool staticOnly = m_core->cfg().b(QStringLiteral("ed2k/staticServersOnly"), false);
    QList<int> idx;
    for (int i = 0; i < m_servers.size(); ++i) {
        const ServerInfo &s = m_servers[i];
        if (isKnownBadServer(s) && !s.isStatic) continue;
        if (s.nextRetryMs > now) continue;
        if (staticOnly && !s.isStatic) continue;
        // [enc-1] "Only allow obfuscated connections": servers without an obfuscation port (or whose
        // obfuscated login failed) would be a plain connection, as eMule we skip them
        if (m_core->cryptRequired() && (s.obfuscationTcpPort == 0 || s.obfFailed)) continue;
        idx.append(i);
    }
    std::stable_sort(idx.begin(), idx.end(), [this](int a, int b) {
        const ServerInfo &x = m_servers[a], &y = m_servers[b];
        auto pref = [](const ServerInfo &s) { return s.preference == 1 ? 2 : (s.preference == 2 ? 0 : 1); };
        if (pref(x) != pref(y)) return pref(x) > pref(y);
        if (x.isStatic != y.isStatic) return x.isStatic;
        const int tx = serverTrustScore(x), ty = serverTrustScore(y);
        if (tx != ty) return tx > ty;
        if ((x.failCount > 2) != (y.failCount > 2)) return x.failCount <= 2;
        // prefer big but not absurd servers (fakes report huge user counts)
        return x.users > y.users;
    });
    return idx;
}

void ServerManager::tryConnect(int i, bool manual)
{
    ServerInfo &s = m_servers[i];
    s.lastConnectAttemptMs = m_clock.elapsed();
    const bool obf = m_core->cryptSupported() && s.obfuscationTcpPort != 0 && !s.obfFailed;
    auto *c = new ServerConnection(m_core, s, obf, this);
    m_attempts.append(c);
    connect(c, &ServerConnection::loggedIn, this, [this, c] { onLoggedIn(c); });
    connect(c, &ServerConnection::closed, this, [this, c](const QString &why) { onClosed(c, why); });
    connect(c, &ServerConnection::serverMessage, this, [this, c](const QString &m) {
        emit log(QStringLiteral("[%1] %2").arg(c->info().name.isEmpty() ? c->id() : c->info().name, m));
        emit serverMessage(c->id(), m);   // [glue-2] NetworkPlugin::serverMessage
    });
    connect(c, &ServerConnection::statusUpdated, this, [this, c](quint32 users, quint32 files) {
        const int k = indexOf(c->id());
        if (k >= 0) { m_servers[k].users = users; m_servers[k].files = files; m_dirty = true; }
        emit changed();
    });
    connect(c, &ServerConnection::identUpdated, this, [this, c](const QString &name, const QString &desc) {
        const int k = indexOf(c->id());
        if (k >= 0) {
            if (!name.isEmpty() && !m_servers[k].isStatic) m_servers[k].name = name;
            if (!desc.isEmpty()) m_servers[k].description = desc;
            m_dirty = true;
        }
        emit changed();
    });
    connect(c, &ServerConnection::searchResults, this, &ServerManager::onSearchResults);
    connect(c, &ServerConnection::sourcesFound, this, [this](const QByteArray &h, const QList<PeerDescriptor> &l) {
        emit sourcesFound(h, l, QStringLiteral("server"));
    });
    connect(c, &ServerConnection::callbackRequested, this, &ServerManager::callbackRequested);
    connect(c, &ServerConnection::serverListReceived, this, [this](const QList<ServerInfo> &l) {
        // announced servers are added untrusted (never outrank the trusted ones), fakes filtered
        if (!m_core->cfg().b(QStringLiteral("ed2k/addServersFromServer"), true)) return;
        const int before = int(m_servers.size());
        mergeServers(l, false);
        if (m_servers.size() != before) { emit log(QStringLiteral("%1 server(s) added from server list").arg(m_servers.size() - before)); emit changed(); }
    });
    emit log(QStringLiteral("connecting to %1 (%2)%3%4").arg(s.name.isEmpty() ? s.id() : s.name, s.id(),
              obf ? QStringLiteral(" obfuscated") : QString(), manual ? QStringLiteral(" [manual]") : QString()));
    c->start();
    emit changed();
}

void ServerManager::onLoggedIn(ServerConnection *c)
{
    if (m_current && m_current != c) {
        if (m_current->state() == ServerConnection::Connected) {
            m_attempts.removeAll(c);
            c->disconnect(this);
            c->close(QStringLiteral("already connected elsewhere"));
            c->deleteLater();
            return;
        }
    }
    const bool fresh = (m_current != c);
    m_current = c;
    m_attempts.removeAll(c);
    for (auto &a : m_attempts) if (a) { a->disconnect(this); a->close(QStringLiteral("connected to another server")); a->deleteLater(); }
    m_attempts.clear();
    const int k = indexOf(c->id());
    if (k >= 0) {
        m_servers[k].failCount = 0;
        m_servers[k].nextRetryMs = 0;
        if (c->info().obfuscationTcpPort) m_servers[k].obfuscationTcpPort = c->info().obfuscationTcpPort;
        m_dirty = true;
    }
    m_consecutiveFailures = 0;
    if (fresh) {
        m_offered.clear();
        m_lastOfferMs = 0;
        emit log(QStringLiteral("connected to %1, %2 (ID %3)%4").arg(c->info().name.isEmpty() ? c->id() : c->info().name,
                  c->lowId() ? QStringLiteral("LowID") : QStringLiteral("HighID")).arg(c->clientId())
                  .arg(c->obfuscated() ? QStringLiteral(", obfuscated") : QString()));
    }
    emit connectedChanged();
    emit changed();
    pumpSearch();
}

void ServerManager::onClosed(ServerConnection *c, const QString &reason)
{
    const bool wasCurrent = (m_current == c);
    const bool wasLoggedIn = c->connectedForMs() > 0;
    m_attempts.removeAll(c);
    const int k = indexOf(c->id());
    if (wasCurrent) {
        m_current = nullptr;
        m_offered.clear();
        emit log(QStringLiteral("disconnected from %1: %2").arg(c->info().name.isEmpty() ? c->id() : c->info().name, reason));
        if (m_tcpActive) finishTcpSearch(m_tcpActive);
        m_nextAutoConnectMs = m_clock.elapsed() + 10000;
        if (!m_core->cfg().b(QStringLiteral("ed2k/reconnect"), true)) m_manualDisconnect = true;
        emit connectedChanged();
    } else if (k >= 0 && !wasLoggedIn) {
        ServerInfo &s = m_servers[k];
        if (c->obfuscated() && !s.obfFailed) {
            s.obfFailed = true;           // retry this server without obfuscation soon
            s.nextRetryMs = m_clock.elapsed() + 2000;
        } else {
            ++s.failCount;
            const qint64 backoff = qMin<qint64>(30LL * 60 * 1000, 30000LL << qMin<quint32>(s.failCount, 6));
            s.nextRetryMs = m_clock.elapsed() + backoff;
        }
        m_dirty = true;
        emit log(QStringLiteral("server %1 failed: %2").arg(s.name.isEmpty() ? s.id() : s.name, reason));
        if (m_attempts.isEmpty() && !m_current) {
            ++m_consecutiveFailures;
            m_nextAutoConnectMs = m_clock.elapsed() + (m_consecutiveFailures < 6 ? 1000 : 30000);
        }
    }
    c->deleteLater();
    emit changed();
}

void ServerManager::autoConnect()
{
    if (!m_running || m_manualDisconnect || m_current || !m_attempts.isEmpty()) return;
    if (!m_core->cfg().b(QStringLiteral("ed2k/autoConnect"), true)) return;
    if (m_clock.elapsed() < m_nextAutoConnectMs) return;
    QList<int> order = connectOrder();
    if (order.isEmpty()) {
        // everything is backing off: retry the least failed server after 30 s
        m_nextAutoConnectMs = m_clock.elapsed() + 30000;
        for (ServerInfo &s : m_servers) s.nextRetryMs = qMin(s.nextRetryMs, m_clock.elapsed() + 30000);
        return;
    }
    const int parallel = qBound(1, m_core->cfg().i(QStringLiteral("ed2k/parallelServerConnects"), 2), 4);
    for (int k = 0; k < order.size() && k < parallel; ++k) tryConnect(order[k], false);
}

void ServerManager::tick()
{
    if (!m_running) return;
    autoConnect();
    pumpSearch();
    pumpUdpSearch();
    pumpSources();
    pumpUdpSources();
    pumpOffers();
    pumpUdpPings();
    if (m_dirty && m_clock.elapsed() - m_lastSaveMs > 60000) save();
}

// ---------------------------------------------------------------- search
void ServerManager::search(const mx::SearchQuery &q)
{
    SearchEncodeOptions opt;
    ServerConnection *c = current();
    opt.largeFiles = !c || (c->tcpFlags() & SRV_TCPFLG_LARGEFILES);
    QString err;
    ServerSearch s;
    s.queryId = q.id;
    s.payload = encodeSearchRequest(q, opt, &err);
    s.maxResults = q.maxResults > 0 ? q.maxResults : 2000;
    s.createdMs = m_clock.elapsed();
    if (s.payload.isEmpty()) {
        emit log(QStringLiteral("search \"%1\": %2").arg(q.text, err));
        QMetaObject::invokeMethod(this, [this, id = q.id] { emit searchDone(id); }, Qt::QueuedConnection);
        return;
    }
    // UDP global search on other servers that answered our pings (best first)
    const bool udp = m_core->cfg().b(QStringLiteral("ed2k/udpSearch"), true);
    if (udp) {
        QList<int> order = connectOrder();
        int n = 0;
        for (int i : order) {
            const ServerInfo &sv = m_servers[i];
            if (c && sv.ip == c->info().ip && sv.port == c->info().port) continue;
            if (!sv.ping || !sv.ip) continue;
            s.udpTargets.append({sv.ip, sv.port});
            if (++n >= 12) break;
        }
    }
    s.udpDone = s.udpTargets.isEmpty();
    m_searches.insert(s.queryId, s);
    m_tcpQueue.append(s.queryId);
    m_udpActive = s.queryId;
    pumpSearch();
}

void ServerManager::cancelSearch(quint32 qid)
{
    if (!m_searches.contains(qid)) return;
    m_searches.remove(qid);
    m_tcpQueue.removeAll(qid);
    if (m_tcpActive == qid) m_tcpActive = 0;   // results of the running TCP search are dropped
    if (m_udpActive == qid) m_udpActive = 0;
}

void ServerManager::pumpSearch()
{
    const qint64 now = m_clock.elapsed();
    // timeout of the running TCP search
    if (m_tcpActive) {
        auto it = m_searches.find(m_tcpActive);
        if (it == m_searches.end()) m_tcpActive = 0;
        else if (now - it->tcpSentMs > 40000) finishTcpSearch(m_tcpActive);
    }
    if (m_tcpActive) return;
    while (!m_tcpQueue.isEmpty()) {
        const quint32 qid = m_tcpQueue.first();
        auto it = m_searches.find(qid);
        if (it == m_searches.end()) { m_tcpQueue.removeFirst(); continue; }
        ServerConnection *c = current();
        if (!c) {
            // wait up to 30 s for a server, then give up on the TCP part
            if (now - it->createdMs > 30000 || (!m_running)) { m_tcpQueue.removeFirst(); it->tcpDone = true; maybeFinishSearch(qid); continue; }
            return;
        }
        m_tcpQueue.removeFirst();
        it->tcpSent = true;
        it->tcpSentMs = now;
        m_tcpActive = qid;
        c->sendSearch(it->payload);
        return;
    }
}

void ServerManager::pumpUdpSearch()
{
    const qint64 now = m_clock.elapsed();
    for (auto it = m_searches.begin(); it != m_searches.end(); ++it) {
        ServerSearch &s = it.value();
        if (s.udpDone) continue;
        if (!s.udpTargets.isEmpty()) {
            if (now - s.lastUdpMs < 1000) continue;
            const auto t = s.udpTargets.takeFirst();
            const int i = indexOfIp(t.first, t.second);
            if (i >= 0) sendUdp(m_servers[i], OP_GLOBSEARCHREQ, s.payload);
            s.lastUdpMs = now;
        } else if (now - s.lastUdpMs > 6000) {
            s.udpDone = true;
            const quint32 qid = it.key();
            QMetaObject::invokeMethod(this, [this, qid] { maybeFinishSearch(qid); }, Qt::QueuedConnection);
        }
    }
}

void ServerManager::emitHits(ServerSearch &s, const QList<SearchResultEntry> &r)
{
    QList<mx::SearchHit> hits;
    for (const SearchResultEntry &e : r) {
        if (s.results >= s.maxResults) break;
        if (s.seen.contains(e.hash)) continue;
        s.seen.insert(e.hash);
        hits.append(toSearchHit(e, s.queryId));
        ++s.results;
    }
    if (!hits.isEmpty()) emit searchHits(s.queryId, hits);
}

void ServerManager::onSearchResults(const QList<SearchResultEntry> &r, bool more)
{
    if (sender() != m_current.data()) return;
    if (!m_tcpActive) return;
    auto it = m_searches.find(m_tcpActive);
    if (it == m_searches.end()) { m_tcpActive = 0; return; }
    emitHits(*it, r);
    ServerConnection *c = current();
    if (more && c && it->results < it->maxResults && it->moreRequests < 5) {
        ++it->moreRequests;
        it->tcpSentMs = m_clock.elapsed();
        c->sendQueryMore();
        return;
    }
    finishTcpSearch(m_tcpActive);
}

void ServerManager::finishTcpSearch(quint32 qid)
{
    if (m_tcpActive == qid) m_tcpActive = 0;
    auto it = m_searches.find(qid);
    if (it != m_searches.end()) it->tcpDone = true;
    maybeFinishSearch(qid);
    QMetaObject::invokeMethod(this, [this] { pumpSearch(); }, Qt::QueuedConnection);
}

void ServerManager::maybeFinishSearch(quint32 qid)
{
    auto it = m_searches.find(qid);
    if (it == m_searches.end()) return;
    if (!it->tcpDone || !it->udpDone) return;
    m_searches.erase(it);
    if (m_udpActive == qid) m_udpActive = 0;
    emit searchDone(qid);
}

// ---------------------------------------------------------------- sources
void ServerManager::requestSources(const QByteArray &hash, qint64 size, bool urgent)
{
    if (hash.size() != 16 || size <= 0) return;
    for (const SrcReq &q : m_srcQueue) if (q.hash == hash) return;
    const qint64 now = m_clock.elapsed();
    auto it = m_srcLastAsked.find(hash);
    if (!urgent && it != m_srcLastAsked.end() && now - it.value() < SERVERREASKTIME_MS) return;
    m_srcQueue.enqueue({hash, size});
    // UDP: ask a few other servers as well
    if (m_core->cfg().b(QStringLiteral("ed2k/udpSources"), true)) {
        ServerConnection *c = current();
        int n = 0;
        for (int i : connectOrder()) {
            const ServerInfo &s = m_servers[i];
            if (!s.ping || !s.ip) continue;
            if (c && s.ip == c->info().ip) continue;
            if (size > 0xFFFFFFFFLL && !(s.udpFlags & SRV_UDPFLG_LARGEFILES)) continue;
            m_udpSrcQueue.enqueue({s.ip, s.port, s.udpFlags, hash, size});
            if (++n >= 8) break;
        }
    }
}

void ServerManager::pumpSources()
{
    ServerConnection *c = current();
    if (!c || m_srcQueue.isEmpty()) return;
    const qint64 now = m_clock.elapsed();
    if (c->connectedForMs() < 3000 || now - m_lastSrcSentMs < 1500) return;
    const SrcReq q = m_srcQueue.dequeue();
    c->sendGetSources(q.hash, q.size);
    m_srcLastAsked.insert(q.hash, now);
    m_lastSrcSentMs = now;
}

void ServerManager::pumpUdpSources()
{
    if (m_udpSrcQueue.isEmpty()) return;
    const qint64 now = m_clock.elapsed();
    if (now - m_lastUdpSrcMs < 700) return;
    const UdpSrcReq q = m_udpSrcQueue.dequeue();
    const int i = indexOfIp(q.ip, q.port);
    if (i < 0) return;
    Writer w;
    w.hash16(q.hash);
    if (q.udpFlags & SRV_UDPFLG_EXT_GETSOURCES2) {
        if (q.size > 0xFFFFFFFFLL) w.u32(0).u64(quint64(q.size)); else w.u32(quint32(q.size));
        sendUdp(m_servers[i], OP_GLOBGETSOURCES2, w.data());
    } else {
        sendUdp(m_servers[i], OP_GLOBGETSOURCES, w.data());
    }
    m_lastUdpSrcMs = now;
}

bool ServerManager::requestCallback(quint32 lowId, quint32 serverIp, quint16 serverPort)
{
    ServerConnection *c = current();
    if (!c || c->lowId()) return false;
    if (serverIp && (c->info().ip != serverIp || (serverPort && c->info().port != serverPort))) return false;
    c->sendCallbackRequest(lowId);
    return true;
}

// ---------------------------------------------------------------- publishing
void ServerManager::publishFiles(const QList<mx::SharedFile> &files)
{
    m_shared.clear();
    QSet<QByteArray> present;
    for (const mx::SharedFile &f : files) {
        if (f.hashes.ed2k.size() != 16 || f.size <= 0 || f.size > MAX_FILE_SIZE) continue;
        if (present.contains(f.hashes.ed2k)) continue;
        present.insert(f.hashes.ed2k);
        m_shared.append(f);
    }
    pumpOffers();
}

void ServerManager::pumpOffers()
{
    ServerConnection *c = current();
    if (!c || c->connectedForMs() < 2000) return;
    const qint64 now = m_clock.elapsed();
    if (m_lastOfferMs && now - m_lastOfferMs < 2000) return;
    const int softLimit = c->info().softFiles ? int(qMin<quint32>(c->info().softFiles, 5000)) : 1000;
    QList<mx::SharedFile> batch;
    for (const mx::SharedFile &f : m_shared) {
        if (m_offered.size() + batch.size() >= softLimit) break;
        if (m_offered.contains(f.hashes.ed2k)) continue;
        if (f.size > 0xFFFFFFFFLL && !(c->tcpFlags() & SRV_TCPFLG_LARGEFILES)) continue;
        batch.append(f);
        if (batch.size() >= 200) break;
    }
    if (batch.isEmpty()) return;
    c->sendOfferFiles(batch, true);
    for (const mx::SharedFile &f : batch) m_offered.insert(f.hashes.ed2k);
    m_lastOfferMs = now;
    emit log(QStringLiteral("offered %1 file(s) to %2 (%3 total)").arg(batch.size())
             .arg(c->info().name.isEmpty() ? c->id() : c->info().name).arg(m_offered.size()));
}

// ---------------------------------------------------------------- UDP
void ServerManager::sendUdp(const ServerInfo &s, quint8 opcode, const QByteArray &payload)
{
    if (!s.ip) return;
    QByteArray d;
    d.append(char(PROT_EDONKEY)).append(char(opcode)).append(payload);
    m_core->sendUdp(s.ip, quint16(s.port + 4), d);
}

void ServerManager::pumpUdpPings()
{
    if (!m_core->cfg().b(QStringLiteral("ed2k/udpPing"), true)) return;
    const qint64 now = m_clock.elapsed();
    if (now - m_lastPingMs < 800 || m_servers.isEmpty()) return;
    m_lastPingMs = now;
    // time out outstanding pings
    for (ServerInfo &s : m_servers) {
        if (s.udpChallenge && now - s.udpPingSentMs > 15000) {
            s.udpChallenge = 0;
            if (s.udpPingTries >= 2) { s.pingedThisSession = true; s.ping = 0; }
        }
    }
    const qint64 epoch = QDateTime::currentSecsSinceEpoch();
    for (int n = 0; n < m_servers.size(); ++n) {
        m_pingCursor = (m_pingCursor + 1) % int(m_servers.size());
        ServerInfo &s = m_servers[m_pingCursor];
        if (!s.ip || s.udpChallenge || isKnownBadServer(s)) continue;
        const bool due = !s.pingedThisSession || (epoch - qint64(s.lastPingTime) > 4 * 3600);
        if (!due) continue;
        s.udpChallenge = 0x55AA0000u | quint32(QRandomGenerator::global()->bounded(0x10000));
        s.udpPingSentMs = now;
        ++s.udpPingTries;
        Writer w; w.u32(s.udpChallenge);
        sendUdp(s, OP_GLOBSERVSTATREQ, w.data());
        return;
    }
}

void ServerManager::handleUdp(quint32 ip, quint16 port, const QByteArray &d)
{
    if (d.size() < 2 || quint8(d[0]) != PROT_EDONKEY) return;
    const int idx = indexOfIp(ip, quint16(port - 4));
    const int idxAny = idx >= 0 ? idx : indexOfIp(ip);
    if (idxAny < 0) return;   // only answers from servers we know
    Reader r(d);
    r.skip(2);
    const quint8 op = quint8(d[1]);
    switch (op) {
    case OP_GLOBSERVSTATRES: {
        ServerInfo &s = m_servers[idxAny];
        const quint32 ch = r.u32();
        if (!r.ok() || ch != s.udpChallenge || !s.udpChallenge) return;
        s.udpChallenge = 0;
        s.udpPingTries = 0;
        s.pingedThisSession = true;
        s.ping = quint32(qMax<qint64>(1, m_clock.elapsed() - s.udpPingSentMs));
        s.lastPingTime = quint32(QDateTime::currentSecsSinceEpoch());
        s.users = r.u32();
        s.files = r.u32();
        if (r.remaining() >= 4) s.maxUsers = r.u32();
        if (r.remaining() >= 8) { s.softFiles = r.u32(); s.hardFiles = r.u32(); }
        if (r.remaining() >= 4) s.udpFlags = r.u32();
        if (r.remaining() >= 4) s.lowIdUsers = r.u32();
        if (r.remaining() >= 8) { s.obfuscationUdpPort = r.u16(); s.obfuscationTcpPort = r.u16(); s.udpKey = r.u32(); }
        s.failCount = 0;
        m_dirty = true;
        emit changed();
        break;
    }
    case OP_GLOBSEARCHRES: {
        // one or more concatenated <E3 99 entry> records
        QList<SearchResultEntry> list;
        for (;;) {
            SearchResultEntry e;
            if (!parseSearchResultEntry(r, &e)) break;
            if (e.size > 0) list.append(e);
            if (r.remaining() >= 2 && quint8(*r.cur()) == PROT_EDONKEY && quint8(*(r.cur() + 1)) == OP_GLOBSEARCHRES) r.skip(2);
            else break;
        }
        if (m_udpActive) {
            auto it = m_searches.find(m_udpActive);
            if (it != m_searches.end()) emitHits(*it, list);
        }
        break;
    }
    case OP_GLOBFOUNDSOURCES: {
        const ServerInfo &s = m_servers[idxAny];
        for (;;) {
            const QByteArray hash = r.hash16();
            const quint8 count = r.u8();
            QList<PeerDescriptor> list;
            for (int i = 0; i < count && r.ok(); ++i) {
                PeerDescriptor p;
                const quint32 id = r.u32();
                p.port = r.u16();
                if (!r.ok()) break;
                if (isLowId(id)) { p.lowId = id; p.serverIp = s.ip; p.serverPort = s.port; } else p.ip = id;
                p.fileHash = hash;
                if (p.isValid()) list.append(p);
            }
            if (!r.ok()) break;
            if (!list.isEmpty()) emit sourcesFound(hash, list, QStringLiteral("udp server"));
            if (r.remaining() >= 2 && quint8(*r.cur()) == PROT_EDONKEY && quint8(*(r.cur() + 1)) == OP_GLOBFOUNDSOURCES) r.skip(2);
            else break;
        }
        break;
    }
    default:
        break;
    }
}

} // namespace mx::ed2k
