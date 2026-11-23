// SPDX-License-Identifier: GPL-3.0-or-later
// Parts of this file are adapted from eMule 0.50a (Copyright (C) the eMule team,
// https://www.emule-project.net; BaseClient.cpp), GPL-2.0-or-later;
// this file is distributed under GPL-3.0-or-later. See NOTICE.
#include "ed2k_peer.h"
#include "ed2k_core.h"
#include "ed2k_io.h"
#include "ed2k_opcodes.h"
#include "ed2k_tags.h"
#include "ed2k_upload.h"

#include <QtCore/QTimer>

namespace mx::ed2k {

QString PeerInfo::software() const
{
    if (!emuleProtocol) {
        if (mlDonkey) return QStringLiteral("MLdonkey");
        if (hybrid) return QStringLiteral("eDonkeyHybrid");
        return QStringLiteral("eDonkey");
    }
    QString name;
    switch (compatClient) {
    case 0: name = QStringLiteral("eMule"); break;
    case 1: name = QStringLiteral("cDonkey"); break;
    case 2: name = QStringLiteral("xMule"); break;
    case 3: name = QStringLiteral("aMule"); break;
    case 4: case 40: name = QStringLiteral("Shareaza"); break;
    case 10: name = QStringLiteral("MLdonkey"); break;
    case 20: name = QStringLiteral("lphant"); break;
    default: name = mlDonkey ? QStringLiteral("MLdonkey") : QStringLiteral("eMule Compat"); break;
    }
    QString ver;
    if (clientVersion) {
        const uint maj = (clientVersion >> 17) & 0x7F, min = (clientVersion >> 10) & 0x7F, upd = (clientVersion >> 7) & 0x07;
        if (compatClient == 0) ver = QStringLiteral(" v%1.%2%3").arg(maj).arg(min).arg(QChar(QLatin1Char('a').unicode() + upd));
        else ver = QStringLiteral(" v%1.%2.%3").arg(maj).arg(min).arg(upd);
    } else if (emuleInfoVersion) {
        ver = QStringLiteral(" v0.%1").arg((emuleInfoVersion >> 4) * 10 + (emuleInfoVersion & 0x0F));
    }
    QString s = name + ver;
    if (!modVersion.isEmpty()) s += QStringLiteral(" [%1]").arg(modVersion.left(40));
    return s;
}

bool isDownloadRolePacket(const Packet &p)
{
    if (p.proto == PROT_EDONKEY) {
        switch (p.opcode) {
        case OP_REQFILENAMEANSWER: case OP_FILESTATUS: case OP_FILEREQANSNOFIL: case OP_HASHSETANSWER:
        case OP_QUEUERANK: case OP_ACCEPTUPLOADREQ: case OP_OUTOFPARTREQS: case OP_SENDINGPART:
        case OP_CHANGE_SLOT:
            return true;
        default: return false;
        }
    }
    switch (p.opcode) {
    case OP_SENDINGPART_I64: case OP_COMPRESSEDPART: case OP_COMPRESSEDPART_I64: case OP_QUEUERANKING:
    case OP_ANSWERSOURCES: case OP_ANSWERSOURCES2: case OP_AICHFILEHASHANS: case OP_MULTIPACKETANSWER:
    case OP_MULTIPACKETANSWER_EXT2: case OP_HASHSETANSWER2: case OP_AICHANSWER: case OP_FILEDESC:
        return true;
    default: return false;
    }
}

bool isUploadRolePacket(const Packet &p)
{
    if (p.proto == PROT_EDONKEY) {
        switch (p.opcode) {
        case OP_REQUESTFILENAME: case OP_SETREQFILEID: case OP_HASHSETREQUEST: case OP_STARTUPLOADREQ:
        case OP_REQUESTPARTS: case OP_CANCELTRANSFER: case OP_END_OF_DOWNLOAD:
            return true;
        default: return false;
        }
    }
    switch (p.opcode) {
    case OP_REQUESTPARTS_I64: case OP_REQUESTSOURCES: case OP_REQUESTSOURCES2: case OP_AICHFILEHASHREQ:
    case OP_MULTIPACKET: case OP_MULTIPACKET_EXT: case OP_MULTIPACKET_EXT2: case OP_HASHSETREQUEST2:
    case OP_AICHREQUEST:
        return true;
    default: return false;
    }
}

PeerConnection::PeerConnection(Ed2kCore *core, Ed2kConnection *conn, bool outgoing, QObject *parent)
    : QObject(parent), m_core(core), m_conn(conn), m_outgoing(outgoing)
{
    m_conn->setParent(this);
    m_conn->setLimiter(core->ctx().limiter);
    m_conn->setIdleTimeoutMs(45000);
    m_conn->setConnectTimeoutMs(25000);
    m_conn->setMaxPacket(1024 * 1024);
    connect(m_conn, &Ed2kConnection::packetReceived, this, &PeerConnection::onPacket);
    connect(m_conn, &Ed2kConnection::closed, this, &PeerConnection::onConnClosed);
    connect(m_conn, &Ed2kConnection::dataSent, this, &PeerConnection::dataSent);
    m_handshakeTimeout.setSingleShot(true);
    m_handshakeTimeout.setInterval(40000);
    connect(&m_handshakeTimeout, &QTimer::timeout, this, [this] { close(QStringLiteral("handshake timeout")); });
}

PeerConnection::~PeerConnection()
{
    if (m_conn) m_conn->disconnect(this);
}

QString PeerConnection::peerKey() const
{
    return QStringLiteral("%1:%2").arg(ipToString(ip())).arg(listenPort());
}

QString PeerConnection::describe() const
{
    QString s = peerKey();
    if (m_handshakeDone) s += QStringLiteral(" (%1)").arg(m_info.software());
    if (obfuscated()) s += QStringLiteral(" [obf]");
    return s;
}

void PeerConnection::setDownloadSink(DownloadSink *s, QObject *owner)
{
    m_sink = s;
    m_sinkOwner = owner;
}

void PeerConnection::startOutgoing(const PeerDescriptor &d, bool allowObf)
{
    m_target = d;
    m_started = true;
    const bool targetSupports = d.cryptOptions & 0x01;
    const bool targetRequests = d.cryptOptions & 0x02;
    const bool targetRequires = d.cryptOptions & 0x04;
    bool obf = allowObf && m_core->cryptSupported() && d.userHash.size() == 16 &&
               ((targetSupports && (targetRequests || m_core->cryptRequested())) || targetRequires);
    if (m_core->cryptRequired() && d.userHash.size() == 16 && allowObf) obf = true;
    // [enc-1] no plain connection when we require obfuscation (unknown user hash: it cannot be
    // obfuscated), and none to a peer that requires it when we have it switched off (as eMule)
    const char *refuse = nullptr;
    if (!obf && m_core->cryptRequired()) refuse = "not obfuscated (obfuscation required)";
    else if (!obf && targetRequires && !m_core->cryptSupported()) refuse = "peer requires obfuscation";
    if (refuse) {
        const QString why = QString::fromLatin1(refuse);
        QTimer::singleShot(0, this, [this, why] { if (m_conn) m_conn->close(why); });
        return;
    }
    if (obf) { m_conn->useClientObfuscation(d.userHash); m_triedObf = true; }
    connect(m_conn, &Ed2kConnection::connected, this, [this] {
        const QByteArray hello = buildHelloPayload(m_core, false);
        m_conn->sendPacket(PROT_EDONKEY, OP_HELLO, hello);
        m_helloSent = true;
        m_handshakeTimeout.start();
    });
    m_conn->connectTo(d.ip, d.port);
}

void PeerConnection::startIncoming()
{
    m_started = true;
    m_handshakeTimeout.start();
}

void PeerConnection::send(quint8 proto, quint8 opcode, const QByteArray &payload, bool allowPacked)
{
    if (m_closed || !m_conn) return;
    qCDebug(lcEd2k).noquote() << describe() << ">>" << QString::number(proto, 16) << QString::number(opcode, 16) << payload.size();
    m_conn->sendPacket(proto, opcode, payload, allowPacked && m_info.dataCompVersion > 0);
}

void PeerConnection::close(const QString &reason)
{
    if (m_closed) return;
    if (m_conn) m_conn->close(reason);
}

void PeerConnection::onConnClosed(const QString &reason)
{
    if (m_closed) return;
    qCDebug(lcEd2k).noquote() << describe() << "closed:" << reason;
    m_closed = true;
    m_handshakeTimeout.stop();
    if (m_sinkOwner && m_sink) m_sink->peerClosed(this, reason);
    emit closed(this, reason);
}

QByteArray PeerConnection::buildHelloPayload(Ed2kCore *core, bool answer)
{
    Writer w;
    if (!answer) w.u8(16);
    w.hash16(core->userHash());
    w.u32(core->helloClientId());
    w.u16(core->tcpPort());
    const LocalCaps &caps = core->caps();
    const quint32 udpVer = caps.udpPort ? 4 : 0;
    const quint32 misc1 = (1u << 29)          // AICH
                        | (1u << 28)          // unicode
                        | (udpVer << 24)
                        | (1u << 20)          // data compression
                        | (0u << 16)          // secure ident: not supported
                        | (4u << 12)          // source exchange 1 (deprecated; we use SX2)
                        | (2u << 8)           // extended requests v2
                        | (1u << 4)           // accept comments
                        | (1u << 2)           // no "view shared files"
                        | (1u << 1);          // multi packet
    const quint32 misc2 = (quint32(caps.directCallback) << 12)   // direct UDP callback (firewalled, UDP open)
                        | (1u << 10)          // source exchange 2
                        | (quint32(core->cryptRequired()) << 9)
                        | (quint32(core->cryptRequested()) << 8)
                        | (quint32(core->cryptSupported()) << 7)
                        | (1u << 5)           // ext multi packet
                        | (1u << 4)           // large files
                        | (quint32(caps.kadVersion) & 0x0F);
    TagList tags;
    tags << Tag::makeStr(CT_NAME, core->nick())
         << Tag::makeInt(CT_VERSION, EDONKEYVERSION)
         << Tag::makeInt(CT_EMULE_UDPPORTS, (quint32(caps.kadPort) << 16) | caps.udpPort);
    if (caps.buddyIp && caps.buddyPort)
        tags << Tag::makeInt(CT_EMULE_BUDDYIP, caps.buddyIp) << Tag::makeInt(CT_EMULE_BUDDYUDP, caps.buddyPort);
    tags
         << Tag::makeInt(CT_EMULE_MISCOPTIONS1, misc1)
         << Tag::makeInt(CT_EMULE_MISCOPTIONS2, misc2)
         << Tag::makeInt(CT_EMULE_VERSION, core->emuleVersionTag())
         << Tag::makeStr(CT_MOD_VERSION, core->clientName());
    writeTagList(w, tags, false);
    w.u32(core->serverIp()).u16(core->serverPort());
    return w.take();
}

bool PeerConnection::parseHelloPayload(const QByteArray &payload, bool isAnswer, PeerInfo *out)
{
    Reader r(payload);
    PeerInfo i;
    if (!isAnswer) {
        const quint8 hs = r.u8();
        if (hs != 16) return false;
    }
    i.userHash = r.hash16();
    i.clientId = r.u32();
    i.tcpPort = r.u16();
    TagList tags;
    if (!readTagList(r, &tags, 64)) return false;
    for (const Tag &t : tags) {
        if (!t.name.isEmpty()) continue;
        switch (t.id) {
        case CT_NAME: if (t.isStr()) i.name = t.str.left(128); break;
        case CT_VERSION: if (t.isInt()) i.ed2kVersion = quint32(t.num); break;
        case CT_PORT: if (t.isInt()) i.tcpPort = quint16(t.num); break;
        case CT_MOD_VERSION:
            if (t.isStr()) i.modVersion = t.str.left(64);
            else if (t.isInt()) i.modVersion = QStringLiteral("ModID=%1").arg(t.num);
            break;
        case CT_EMULE_UDPPORTS:
            if (t.isInt()) { i.kadPort = quint16(t.num >> 16); i.udpPort = quint16(t.num); }
            break;
        case CT_EMULE_BUDDYUDP: if (t.isInt()) i.buddyPort = quint16(t.num); break;
        case CT_EMULE_BUDDYIP: if (t.isInt()) i.buddyIp = quint32(t.num); break;
        case CT_EMULE_MISCOPTIONS1:
            if (t.isInt()) {
                const quint32 v = quint32(t.num);
                i.aichVersion = (v >> 29) & 0x07;
                i.unicode = (v >> 28) & 0x01;
                i.udpVersion = (v >> 24) & 0x0F;
                i.dataCompVersion = (v >> 20) & 0x0F;
                i.secIdentVersion = (v >> 16) & 0x0F;
                i.sourceExchange1Version = (v >> 12) & 0x0F;
                i.extendedRequestsVersion = (v >> 8) & 0x0F;
                i.acceptCommentVersion = (v >> 4) & 0x0F;
                i.noViewSharedFiles = (v >> 2) & 0x01;
                i.multiPacket = (v >> 1) & 0x01;
                i.supportsPreview = v & 0x01;
            }
            break;
        case CT_EMULE_MISCOPTIONS2:
            if (t.isInt()) {
                const quint32 v = quint32(t.num);
                i.fileIdentifiers = (v >> 13) & 1;
                i.directUdpCallback = (v >> 12) & 1;
                i.captcha = (v >> 11) & 1;
                i.sourceExchange2 = (v >> 10) & 1;
                i.requiresCrypt = (v >> 9) & 1;
                i.requestsCrypt = (v >> 8) & 1;
                i.supportsCrypt = (v >> 7) & 1;
                i.extMultiPacket = (v >> 5) & 1;
                i.largeFiles = (v >> 4) & 1;
                i.kadVersion = v & 0x0F;
                i.requestsCrypt = i.requestsCrypt && i.supportsCrypt;
                i.requiresCrypt = i.requiresCrypt && i.requestsCrypt;
            }
            break;
        case CT_EMULE_VERSION:
            if (t.isInt()) {
                i.compatClient = quint8(t.num >> 24);
                i.clientVersion = quint32(t.num) & 0x00FFFFFF;
                i.emuleProtocol = true;
            }
            break;
        default: break;
        }
    }
    if (r.remaining() >= 6) {
        i.serverIp = r.u32();
        i.serverPort = r.u16();
    }
    if (r.remaining() == 4) {
        const quint32 test = r.u32();
        if (test == 0x4B444C4D) i.mlDonkey = true;   // "MLDK"
        else i.hybrid = true;
    }
    if (i.userHash.size() != 16) return false;
    if (out) *out = i;
    return true;
}

void PeerConnection::sendMuleInfo(bool answer)
{
    Writer w;
    w.u8(0x50).u8(0x01);   // "version 0.50" style short version, EMULE_PROTOCOL
    TagList tags;
    tags << Tag::makeInt(ET_COMPRESSION, 1) << Tag::makeInt(ET_UDPVER, m_core->caps().udpPort ? 4 : 0)
         << Tag::makeInt(ET_UDPPORT, m_core->caps().udpPort) << Tag::makeInt(ET_SOURCEEXCHANGE, 3)
         << Tag::makeInt(ET_COMMENTS, 1) << Tag::makeInt(ET_EXTENDEDREQUEST, 2) << Tag::makeInt(ET_FEATURES, 0)
         << Tag::makeStr(CT_MOD_VERSION, m_core->clientName());
    writeTagList(w, tags, false);
    send(PROT_EMULE, answer ? OP_EMULEINFOANSWER : OP_EMULEINFO, w.data());
}

void PeerConnection::finishHandshake()
{
    if (m_handshakeDone) return;
    if (m_info.userHash == m_core->userHash()) { close(QStringLiteral("connected to ourselves")); return; }
    m_handshakeDone = true;
    m_handshakeTimeout.stop();
    // A sink that was attached before the handshake (our own outgoing download connection) is told
    // here; one attached by Ed2kCore in response to handshakeComplete() gets peerReady() from attach().
    const bool hadSink = m_sinkOwner && m_sink;
    DownloadSink *sink = m_sink;
    emit handshakeComplete(this);
    if (m_closed) return;
    if (hadSink && m_sinkOwner && m_sink == sink) m_sink->peerReady(this);
}

bool PeerConnection::handleMisc(const Packet &p)
{
    Reader r(p.payload);
    if (p.proto == PROT_EDONKEY) {
        switch (p.opcode) {
        case OP_MESSAGE: return true;   // chat/spam: ignored
        case OP_ASKSHAREDFILES:
        case OP_ASKSHAREDDIRS:
        case OP_ASKSHAREDFILESDIR:
            send(PROT_EDONKEY, OP_ASKSHAREDDENIEDANS);
            return true;
        case OP_CHANGE_CLIENT_ID: {
            r.u32();
            const quint32 nid = r.u32();
            if (r.ok()) m_info.clientId = nid;
            return true;
        }
        case OP_ASKSHAREDFILESANSWER: case OP_ASKSHAREDDIRSANS: case OP_ASKSHAREDFILESDIRANS:
        case OP_ASKSHAREDDENIEDANS: case OP_PORTTEST:
            return true;
        default: return false;
        }
    }
    switch (p.opcode) {
    case OP_EMULEINFO:
    case OP_EMULEINFOANSWER: {
        m_info.emuleInfoVersion = r.u8();
        r.u8();
        TagList tags;
        if (readTagList(r, &tags, 64)) {
            for (const Tag &t : tags) {
                if (!t.isInt() && !t.is(quint8(CT_MOD_VERSION))) continue;
                switch (t.id) {
                case ET_COMPRESSION: m_info.dataCompVersion = quint8(t.num); break;
                case ET_UDPPORT: m_info.udpPort = quint16(t.num); break;
                case ET_UDPVER: m_info.udpVersion = quint8(t.num); break;
                case ET_SOURCEEXCHANGE: m_info.sourceExchange1Version = quint8(t.num); break;
                case ET_COMMENTS: m_info.acceptCommentVersion = quint8(t.num); break;
                case ET_EXTENDEDREQUEST: m_info.extendedRequestsVersion = quint8(t.num); break;
                case ET_COMPATIBLECLIENT: m_info.compatClient = quint8(t.num); break;
                case CT_MOD_VERSION: if (t.isStr()) m_info.modVersion = t.str.left(64); break;
                default: break;
                }
            }
        }
        m_info.emuleProtocol = true;
        if (p.opcode == OP_EMULEINFO) sendMuleInfo(true);
        return true;
    }
    case OP_PUBLICIP_REQ: {
        Writer w; w.u32(ip());
        send(PROT_EMULE, OP_PUBLICIP_ANSWER, w.data());
        return true;
    }
    case OP_BUDDYPING: case OP_BUDDYPONG: case OP_CALLBACK: case OP_REASKCALLBACKTCP:
    case OP_FWCHECKUDPREQ: case OP_KAD_FWTCPCHECK_ACK:
        emit m_core->kadTcpPacket(this, p.opcode, p.payload);   // handled by the Kad plugin (if running)
        return true;
    case OP_PUBLICIP_ANSWER: case OP_SECIDENTSTATE: case OP_PUBLICKEY: case OP_SIGNATURE:
    case OP_REQUESTPREVIEW: case OP_PREVIEWANSWER: case OP_CHATCAPTCHAREQ: case OP_CHATCAPTCHARES:
        return true;   // features we do not offer (no secure ident): ignore
    default: return false;
    }
}

void PeerConnection::onPacket(const Packet &p)
{
    if (m_closed) return;
    qCDebug(lcEd2k).noquote() << describe() << "<<" << QString::number(p.proto, 16) << QString::number(p.opcode, 16) << p.payload.size();
    if (!m_handshakeDone) {
        if (p.proto == PROT_EDONKEY && p.opcode == OP_HELLO) {
            PeerInfo i;
            if (!parseHelloPayload(p.payload, false, &i)) { close(QStringLiteral("malformed hello")); return; }
            m_info = i;
            send(PROT_EDONKEY, OP_HELLOANSWER, buildHelloPayload(m_core, true));
            finishHandshake();
            return;
        }
        if (p.proto == PROT_EDONKEY && p.opcode == OP_HELLOANSWER && m_helloSent) {
            PeerInfo i;
            if (!parseHelloPayload(p.payload, true, &i)) { close(QStringLiteral("malformed hello answer")); return; }
            m_info = i;
            finishHandshake();
            return;
        }
        if (p.proto == PROT_EMULE && (p.opcode == OP_EMULEINFO || p.opcode == OP_EMULEINFOANSWER)) { handleMisc(p); return; }
        // anything else before the handshake is a protocol violation; tolerate a few harmless ones
        if (p.proto == PROT_EMULE && (p.opcode == OP_SECIDENTSTATE || p.opcode == OP_PUBLICKEY)) return;
        close(QStringLiteral("packet 0x%1/0x%2 before hello").arg(p.proto, 2, 16, QLatin1Char('0')).arg(p.opcode, 2, 16, QLatin1Char('0')));
        return;
    }
    if (p.proto == PROT_EDONKEY && (p.opcode == OP_HELLO)) {
        // re-hello on an established connection (some clients do this after an ID change)
        PeerInfo i;
        if (parseHelloPayload(p.payload, false, &i) && i.userHash == m_info.userHash) {
            m_info = i;
            send(PROT_EDONKEY, OP_HELLOANSWER, buildHelloPayload(m_core, true));
        }
        return;
    }
    if (p.proto == PROT_EDONKEY && p.opcode == OP_HELLOANSWER) return;
    if (handleMisc(p)) return;
    if (isDownloadRolePacket(p)) {
        if (m_sinkOwner && m_sink) { m_sink->peerPacket(this, p); return; }
        if (p.proto == PROT_EDONKEY && p.opcode == OP_ACCEPTUPLOADREQ) {
            // a slot for a download we no longer run
            send(PROT_EDONKEY, OP_CANCELTRANSFER);
        }
        return;
    }
    if (isUploadRolePacket(p)) {
        if (m_core->uploads()) m_core->uploads()->handlePacket(this, p);
        return;
    }
    // unknown opcode: ignore (forward compatible)
}

} // namespace mx::ed2k
