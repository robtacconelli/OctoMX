// SPDX-License-Identifier: GPL-3.0-or-later
#include "ed2k_source.h"
#include "ed2k_core.h"
#include "ed2k_io.h"
#include "ed2k_opcodes.h"
#include "ed2k_server.h"

#include <QtCore/QtEndian>
#include <zlib.h>

namespace mx::ed2k {

static constexpr qint64 kDownloadTimeoutMs = 100000;   // eMule DOWNLOADTIMEOUT
static constexpr qint64 kReadyIdleMs = 60000;          // give the slot back if the engine sends nothing
static constexpr qint64 kCallbackTimeoutMs = 45000;
static constexpr qint64 kPhaseTimeoutMs = 60000;

Ed2kRangeSource::Ed2kRangeSource(Ed2kCore *core, const QString &downloadId, const mx::FileHashes &h, qint64 size,
                                 const mx::SearchSource &src, const PeerDescriptor &peer, QObject *parent)
    : mx::RangeSource(parent), m_core(core), m_downloadId(downloadId), m_hashes(h), m_size(size), m_src(src), m_peer(peer)
{
    m_clock.start();
    m_peer.fileHash = h.ed2k;
    m_tick.setInterval(2000);
    connect(&m_tick, &QTimer::timeout, this, &Ed2kRangeSource::tick);
    m_core->registerSource(this);
}

Ed2kRangeSource::~Ed2kRangeSource()
{
    resetBlocks();
    if (m_pc) m_pc->setDownloadSink(nullptr, nullptr);
    m_core->unregisterSource(this);
}

mx::SearchSource Ed2kRangeSource::descriptor() const
{
    mx::SearchSource s = m_src;
    s.net = mx::Net::ED2K;
    if (s.uri.isEmpty() || !s.uri.startsWith(QLatin1String("ed2k-peer://"))) s.uri = formatPeerUri(m_peer);
    s.user.net = mx::Net::ED2K;
    if (m_peer.userHash.size() == 16) s.user.id = toHex(m_peer.userHash);
    else if (s.user.id.isEmpty()) s.user.id = m_peer.key();
    if (!m_remoteName.isEmpty()) s.user.name = m_remoteName;
    else if (m_pc && !m_pc->info().name.isEmpty()) s.user.name = m_pc->info().name;
    if (s.user.name.isEmpty()) s.user.name = m_peer.key();
    if (!m_peer.isLowId()) s.user.address = QStringLiteral("%1:%2").arg(ipToString(m_peer.ip)).arg(m_peer.port);
    s.connection = QStringLiteral("eD2k");
    if (!m_software.isEmpty()) s.extra.insert(QStringLiteral("client"), m_software);
    if (m_rank) s.extra.insert(QStringLiteral("queueRank"), m_rank);
    if (m_pc) s.extra.insert(QStringLiteral("obfuscated"), m_pc->obfuscated());
    else if (m_wasObfuscated) s.extra.insert(QStringLiteral("obfuscated"), true);
    return s;
}

QString Ed2kRangeSource::clientName() const { return m_software; }

QString Ed2kRangeSource::stateText() const
{
    switch (m_phase) {
    case PIdle: return QStringLiteral("Idle");
    case PConnecting: return QStringLiteral("Connecting...");
    case PWaitCallback: return QStringLiteral("Waiting for callback...");
    case PFileRequest: return QStringLiteral("Requesting file...");
    case PHashset: return QStringLiteral("Requesting hashset...");
    case PUploadRequest: return QStringLiteral("Asking for upload...");
    case PQueued:
    case PQueuedOffline:
        return m_rank ? QStringLiteral("Remotely Queued (%1)").arg(m_rank) : QStringLiteral("Remotely Queued");
    case PNoParts: return QStringLiteral("No needed parts");
    case PDownloading: return m_reqActive ? QStringLiteral("Transferring") : QStringLiteral("Ready");
    case PFailed: return QStringLiteral("Failed: ") + m_text;
    case PClosed: return QStringLiteral("Closed");
    }
    return QString();
}

void Ed2kRangeSource::setPhase(Phase p, const QString &text)
{
    m_phase = p;
    m_phaseSinceMs = m_clock.elapsed();
    if (!text.isEmpty() || p == PFailed) m_text = text;
    updateState();
}

void Ed2kRangeSource::updateState()
{
    State s = m_state;
    switch (m_phase) {
    case PIdle: s = Idle; break;
    case PConnecting: case PWaitCallback: case PFileRequest: case PHashset: case PUploadRequest: s = Connecting; break;
    case PQueued: case PQueuedOffline: case PNoParts: s = Queued; break;
    case PDownloading: s = m_reqActive ? Busy : Ready; break;
    case PFailed: s = Failed; break;
    case PClosed: s = Closed; break;
    }
    if (s != m_state) { m_state = s; emit stateChanged(); }
    else emit stateChanged();   // text changed (rank, phase), cheap for the engine
}

void Ed2kRangeSource::open()
{
    if (m_phase != PIdle && m_phase != PFailed) return;
    m_tick.start();
    if (m_size <= 0 || m_hashes.ed2k.size() != 16) { fail(QStringLiteral("no eD2k hash")); return; }
    m_core->ensureDownload(m_downloadId, m_hashes, m_size, QString());
    if (m_pc && m_pc->isOpen()) {
        if (m_pc->handshakeDone()) peerReady(m_pc);
        else setPhase(PConnecting);
        return;
    }
    // respect the re-ask interval even across RangeSource objects
    // (not for a transfer the user stopped and started again: its slot is gone, it is asked at once)
    const qint64 since = m_core->msSinceAsk(m_peer.key(), m_hashes.ed2k);
    if (since >= 0 && since < FILEREASKTIME_MS && !m_core->takeEarlyAsk(m_peer.key(), m_hashes.ed2k)) {
        m_nextReaskMs = m_clock.elapsed() + (FILEREASKTIME_MS - since);
        m_everQueued = true;
        setPhase(PQueuedOffline);
        return;
    }
    connectNow(true);
}

void Ed2kRangeSource::connectNow(bool allowObf)
{
    if (m_peer.needsKadCallback()) {
        // firewalled Kad source: ask it (directly over UDP, or through its buddy) to connect to us
        if (!m_core->canAcceptIncoming()) { fail(QStringLiteral("firewalled Kad source and we are firewalled")); return; }
        const bool ok = m_peer.directCallback ? m_core->sendDirectCallback(m_peer)
                                              : (m_core->kadBuddyCallback && m_core->kadBuddyCallback(m_peer, m_hashes.ed2k));
        if (!ok) { fail(QStringLiteral("Kad callback not possible")); return; }
        setPhase(PWaitCallback);
        return;
    }
    if (m_peer.isLowId()) {
        if (m_core->lowId() || !m_core->clientId()) { fail(QStringLiteral("LowID peer and we are LowID / offline")); return; }
        if (!m_core->servers()->requestCallback(m_peer.lowId, m_peer.serverIp, m_peer.serverPort)) {
            fail(QStringLiteral("LowID peer on another server"));
            return;
        }
        setPhase(PWaitCallback);
        return;
    }
    if (!m_core->acceptableSourceIp(m_peer.ip)) { fail(QStringLiteral("address not allowed")); return; }
    PeerConnection *pc = m_core->connectPeer(m_peer, allowObf);
    if (!pc) { fail(QStringLiteral("connection limit")); return; }
    m_pc = pc;
    pc->setDownloadSink(this, this);
    setPhase(PConnecting);
}

bool Ed2kRangeSource::matchesIncoming(const PeerInfo &info, quint32 ip) const
{
    if (m_peer.userHash.size() == 16) return info.userHash == m_peer.userHash;
    if (m_peer.isLowId()) return info.clientId == m_peer.lowId && (!m_peer.serverIp || info.serverIp == m_peer.serverIp);
    return ip == m_peer.ip && (info.tcpPort == m_peer.port || !info.tcpPort);
}

bool Ed2kRangeSource::wantsConnection() const
{
    if (m_pc && m_pc->isOpen()) return false;
    return m_phase == PWaitCallback || m_phase == PQueued || m_phase == PQueuedOffline || m_phase == PNoParts ||
           m_phase == PConnecting || m_phase == PIdle;
}

void Ed2kRangeSource::attach(PeerConnection *pc)
{
    if (m_pc && m_pc != pc && m_pc->isOpen()) return;
    m_pc = pc;
    pc->setDownloadSink(this, this);
    if (m_phase == PIdle) m_tick.start();
    peerReady(pc);
}

void Ed2kRangeSource::peerReady(PeerConnection *pc)
{
    if (pc != m_pc) return;
    m_failedConnects = 0;
    if (pc->obfuscated()) m_wasObfuscated = true;
    const PeerInfo &i = pc->info();
    m_software = i.software();
    if (m_peer.userHash.isEmpty()) m_peer.userHash = i.userHash;
    m_peerUdpPort = i.udpPort;
    m_peerUdpVersion = i.udpVersion;
    m_udpPending = false;
    quint8 co = 0;
    if (i.supportsCrypt) co |= 0x01;
    if (i.requestsCrypt) co |= 0x02;
    if (i.requiresCrypt) co |= 0x04;
    m_peer.cryptOptions = co;
    if (m_size > OLD_MAX_FILE_SIZE && !i.largeFiles) { fail(QStringLiteral("peer does not support large files")); return; }
    pc->setIdleTimeoutMs(45000);
    switch (m_phase) {
    case PQueued:
    case PQueuedOffline:
    case PNoParts:
        // the peer came to us: either it offers a slot (OP_ACCEPTUPLOADREQ follows) or answers a
        // re-ask we are due anyway
        setPhase(PQueued);
        if (m_clock.elapsed() >= m_nextReaskMs) sendFileRequest();
        break;
    case PDownloading:
        break;
    default:
        sendFileRequest();
        break;
    }
}

void Ed2kRangeSource::sendFileRequest()
{
    if (!m_pc) return;
    const PeerInfo &i = m_pc->info();
    m_gotName = m_gotStatus = false;
    setPhase(PFileRequest);
    DownloadCtx *d = m_core->download(m_downloadId);
    const bool wantAich = i.aichVersion > 0 && d && !d->aichKnown;
    const QString key = m_pc->peerKey();
    const bool wantSources = (i.sourceExchange2 || i.sourceExchange1Version > 1) && m_core->sourceExchangeAllowed(key, m_hashes.ed2k);
    if (wantSources) m_core->noteSourceExchange(key, m_hashes.ed2k);
    const bool multi = i.multiPacket || i.extMultiPacket;
    if (multi) {
        // eMule sends the whole file request as one multi packet to clients that support it (aMule
        // even drops the connection on a stand-alone OP_REQUESTSOURCES2).
        Writer w;
        w.hash16(m_hashes.ed2k);
        if (i.extMultiPacket) w.u64(quint64(m_size));
        w.u8(OP_REQUESTFILENAME);
        if (i.extendedRequestsVersion > 0) w.bytes(m_core->partStatus(m_hashes.ed2k, m_size));   // our verified parts
        if (i.extendedRequestsVersion > 1) w.u16(0);      // complete sources we know
        if (m_size > PARTSIZE) w.u8(OP_SETREQFILEID);
        if (wantSources) {
            if (i.sourceExchange2) w.u8(OP_REQUESTSOURCES2).u8(SOURCEEXCHANGE2_VERSION).u16(0);
            else w.u8(OP_REQUESTSOURCES);
        }
        if (wantAich) w.u8(OP_AICHFILEHASHREQ);
        m_pc->send(PROT_EMULE, i.extMultiPacket ? OP_MULTIPACKET_EXT : OP_MULTIPACKET, w.data());
        return;
    }
    Writer w;
    w.hash16(m_hashes.ed2k);
    if (i.extendedRequestsVersion > 0) w.bytes(m_core->partStatus(m_hashes.ed2k, m_size));
    if (i.extendedRequestsVersion > 1) w.u16(0);
    m_pc->send(PROT_EDONKEY, OP_REQUESTFILENAME, w.data());
    if (m_size > PARTSIZE) {
        Writer s2; s2.hash16(m_hashes.ed2k);
        m_pc->send(PROT_EDONKEY, OP_SETREQFILEID, s2.data());
    }
    if (wantAich) { Writer a2; a2.hash16(m_hashes.ed2k); m_pc->send(PROT_EMULE, OP_AICHFILEHASHREQ, a2.data()); }
    if (wantSources) {
        if (i.sourceExchange2) {
            Writer x; x.u8(SOURCEEXCHANGE2_VERSION).u16(0).hash16(m_hashes.ed2k);
            m_pc->send(PROT_EMULE, OP_REQUESTSOURCES2, x.data());
        } else {
            Writer x; x.hash16(m_hashes.ed2k);
            m_pc->send(PROT_EMULE, OP_REQUESTSOURCES, x.data());
        }
    }
}

void Ed2kRangeSource::afterFileInfo()
{
    if (!m_gotName) return;
    if (m_size > PARTSIZE && !m_gotStatus) return;
    if (m_available.isEmpty()) {
        m_everQueued = true;
        scheduleReask();
        setPhase(PNoParts);
        return;
    }
    if (m_size >= PARTSIZE && !m_core->hashsetKnown(m_hashes.ed2k)) {
        setPhase(PHashset);
        Writer w; w.hash16(m_hashes.ed2k);
        m_pc->send(PROT_EDONKEY, OP_HASHSETREQUEST, w.data());
        return;
    }
    sendStartUpload();
}

void Ed2kRangeSource::sendStartUpload()
{
    if (!m_pc) return;
    setPhase(PUploadRequest);
    Writer w; w.hash16(m_hashes.ed2k);
    m_pc->send(PROT_EDONKEY, OP_STARTUPLOADREQ, w.data());
    m_core->noteAsk(m_pc->peerKey(), m_hashes.ed2k);
    m_core->noteAsk(m_peer.key(), m_hashes.ed2k);
}

void Ed2kRangeSource::scheduleReask()
{
    m_nextReaskMs = m_clock.elapsed() + FILEREASKTIME_MS;
}

void Ed2kRangeSource::handleFileStatus(Reader &r)
{
    const quint16 count = r.u16();
    if (!r.ok()) return;
    const int parts = dataPartCount(m_size);
    QList<bool> have;
    if (count == 0) {
        m_complete = true;
        for (int k = 0; k < parts; ++k) have.append(true);
    } else {
        if (count != ed2kPartCount(m_size) && count != parts) { fail(QStringLiteral("part count mismatch")); return; }
        const QByteArray bits = r.bytes((count + 7) / 8);
        if (!r.ok()) { fail(QStringLiteral("truncated part status")); return; }
        for (int k = 0; k < parts; ++k) have.append((quint8(bits[k / 8]) >> (k % 8)) & 1);
        m_complete = !have.contains(false);
    }
    m_available = m_complete ? QList<QPair<qint64, qint64>>{{0, m_size}} : rangesFromParts(have, m_size);
    m_gotStatus = true;
    emit availabilityChanged();
}

void Ed2kRangeSource::handleAnswerSources(const Packet &p)
{
    Reader r(p.payload);
    quint8 version = 0;
    if (p.opcode == OP_ANSWERSOURCES2) version = r.u8();
    const QByteArray hash = r.hash16();
    if (!r.ok() || hash != m_hashes.ed2k) return;
    const quint16 count = r.u16();
    const qint64 rest = r.remaining();
    if (p.opcode == OP_ANSWERSOURCES) {
        // SX1: version inferred from the record size
        if (count && rest == qint64(count) * 12) version = 1;
        else if (count && rest == qint64(count) * 28) version = 3;
        else if (count && rest == qint64(count) * 29) version = 4;
        else return;
    }
    const int rec = version == 1 ? 12 : (version <= 3 ? 28 : 29);
    if (version == 0 || version > 4 || qint64(count) * rec != rest || count > 500) return;
    QList<PeerDescriptor> list;
    for (int k = 0; k < count; ++k) {
        PeerDescriptor d;
        const quint32 id = r.u32();
        d.port = r.u16();
        d.serverIp = r.u32();
        d.serverPort = r.u16();
        if (version >= 2) d.userHash = r.hash16();
        if (version >= 4) d.cryptOptions = r.u8() & 0x7F;
        // v3+ carry "hybrid" IDs (byte-swapped IP) for HighIDs
        const quint32 ed2kId = version >= 3 && !isLowId(id) ? qbswap(id) : id;
        if (isLowId(ed2kId)) { d.lowId = ed2kId; }
        else d.ip = ed2kId;
        d.fileHash = hash;
        if (d.isValid()) list.append(d);
    }
    if (!list.isEmpty()) m_core->addSources(hash, list, QStringLiteral("source exchange"));
}

void Ed2kRangeSource::peerPacket(PeerConnection *pc, const Packet &p)
{
    if (pc != m_pc) return;
    Reader r(p.payload);
    if (p.proto == PROT_EDONKEY) {
        switch (p.opcode) {
        case OP_REQFILENAMEANSWER: {
            const QByteArray h = r.hash16();
            const QString name = r.str16(1024);
            if (!r.ok() || h != m_hashes.ed2k) return;
            m_remoteName = name;
            m_gotName = true;
            if (m_size <= PARTSIZE) {
                m_complete = true;
                m_available = {{0, m_size}};
                m_gotStatus = true;
                emit availabilityChanged();
            }
            if (m_phase == PFileRequest) afterFileInfo();
            return;
        }
        case OP_FILESTATUS: {
            const QByteArray h = r.hash16();
            if (!r.ok() || h != m_hashes.ed2k) return;
            handleFileStatus(r);
            if (m_phase == PFileRequest) afterFileInfo();
            return;
        }
        case OP_FILEREQANSNOFIL: {
            const QByteArray h = r.hash16();
            if (h == m_hashes.ed2k) fail(QStringLiteral("file not shared by peer"));
            return;
        }
        case OP_HASHSETANSWER: {
            const QByteArray h = r.hash16();
            const quint16 n = r.u16();
            if (!r.ok() || h != m_hashes.ed2k || n > 30000) return;
            QList<QByteArray> parts;
            for (int k = 0; k < n && r.ok(); ++k) parts.append(r.hash16());
            if (!r.ok()) return;
            if (verifyHashset(m_hashes.ed2k, m_size, parts)) {
                m_core->learnHashset(m_hashes.ed2k, parts);
            } else {
                m_core->log(QStringLiteral("bad hashset from %1 (%2 hashes), ignored").arg(pc->describe()).arg(n));
            }
            if (m_phase == PHashset) sendStartUpload();
            return;
        }
        case OP_QUEUERANK: {
            const quint32 rank = r.u32();
            if (!r.ok()) return;
            m_rank = int(qMin<quint32>(rank, 1000000));
            m_everQueued = true;
            scheduleReask();
            if (m_phase != PDownloading) setPhase(PQueued);
            return;
        }
        case OP_ACCEPTUPLOADREQ: {
            if (m_phase == PClosed || m_phase == PFailed) { pc->send(PROT_EDONKEY, OP_CANCELTRANSFER); return; }
            m_rank = 0;
            m_lastDataMs = m_idleSinceMs = m_clock.elapsed();
            pc->setIdleTimeoutMs(kDownloadTimeoutMs + 20000);
            setPhase(PDownloading);
            m_core->log(QStringLiteral("upload slot from %1").arg(pc->describe()));
            return;
        }
        case OP_OUTOFPARTREQS: {
            if (m_reqActive) finishRequest(false, QStringLiteral("peer ended the upload session"));
            m_everQueued = true;
            scheduleReask();
            setPhase(PQueued);
            pc->setIdleTimeoutMs(45000);
            return;
        }
        case OP_SENDINGPART: {
            const QByteArray h = r.hash16();
            const qint64 s = r.u32(), e = r.u32();
            if (!r.ok() || h != m_hashes.ed2k) return;
            onData(s, e, r.rest());
            return;
        }
        default: return;
        }
    }
    switch (p.opcode) {
    case OP_QUEUERANKING: {
        const quint16 rank = r.u16();
        if (!r.ok()) return;
        m_rank = rank;
        m_everQueued = true;
        scheduleReask();
        if (m_phase != PDownloading) setPhase(PQueued);
        return;
    }
    case OP_SENDINGPART_I64: {
        const QByteArray h = r.hash16();
        const qint64 s = qint64(r.u64()), e = qint64(r.u64());
        if (!r.ok() || h != m_hashes.ed2k) return;
        onData(s, e, r.rest());
        return;
    }
    case OP_COMPRESSEDPART:
    case OP_COMPRESSEDPART_I64: {
        const QByteArray h = r.hash16();
        const qint64 s = p.opcode == OP_COMPRESSEDPART_I64 ? qint64(r.u64()) : qint64(r.u32());
        const quint32 total = r.u32();
        if (!r.ok() || h != m_hashes.ed2k) return;
        onCompressed(s, total, r.rest());
        return;
    }
    case OP_ANSWERSOURCES:
    case OP_ANSWERSOURCES2:
        handleAnswerSources(p);
        return;
    case OP_AICHFILEHASHANS: {
        const QByteArray h = r.hash16();
        const QByteArray aich = r.bytes(20);
        if (r.ok() && h == m_hashes.ed2k) m_core->learnAich(h, aich, pc->ip());
        return;
    }
    case OP_MULTIPACKETANSWER: {
        const QByteArray h = r.hash16();
        if (!r.ok() || h != m_hashes.ed2k) return;
        while (r.remaining() > 0) {
            const quint8 sub = r.u8();
            if (sub == OP_REQFILENAMEANSWER) {
                m_remoteName = r.str16(1024);
                m_gotName = r.ok();
                if (m_size <= PARTSIZE && m_gotName) { m_complete = true; m_available = {{0, m_size}}; m_gotStatus = true; emit availabilityChanged(); }
            } else if (sub == OP_FILESTATUS) {
                handleFileStatus(r);
            } else if (sub == OP_AICHFILEHASHANS) {
                const QByteArray aich = r.bytes(20);
                if (r.ok()) m_core->learnAich(h, aich, pc->ip());
            } else break;
            if (!r.ok()) break;
        }
        if (m_phase == PFileRequest) afterFileInfo();
        return;
    }
    default:
        return;
    }
}

void Ed2kRangeSource::peerClosed(PeerConnection *pc, const QString &reason)
{
    if (pc != m_pc) return;
    m_pc = nullptr;
    const bool handshook = pc->handshakeDone();
    if (m_reqActive) finishRequest(false, QStringLiteral("connection lost: ") + reason);
    switch (m_phase) {
    case PConnecting:
        if (!handshook) {
            ++m_failedConnects;
            if (pc->triedObfuscation() && !m_triedPlain && !(m_peer.cryptOptions & 0x04) && !m_core->cryptRequired()) {
                // obfuscated connect failed: eMule peers that cannot decrypt just drop us, retry plain once
                m_triedPlain = true;
                PeerConnection *np = m_core->connectPeer(m_peer, false);
                if (np) { m_pc = np; np->setDownloadSink(this, this); setPhase(PConnecting); return; }
            }
            if (m_everQueued && m_failedConnects < 3) { scheduleReask(); setPhase(PQueuedOffline); return; }
            fail(QStringLiteral("connect failed: ") + reason);
            return;
        }
        Q_FALLTHROUGH();
    case PFileRequest:
    case PHashset:
    case PUploadRequest:
        if (m_everQueued) { scheduleReask(); setPhase(PQueuedOffline); }
        else fail(QStringLiteral("disconnected during request: ") + reason);
        return;
    case PQueued:
    case PNoParts:
        setPhase(m_phase == PNoParts ? PNoParts : PQueuedOffline);
        return;
    case PDownloading:
        m_everQueued = true;
        scheduleReask();
        setPhase(PQueuedOffline);
        return;
    default:
        return;
    }
}

void Ed2kRangeSource::request(qint64 offset, qint64 length)
{
    if (m_phase != PDownloading || !m_pc || m_reqActive) {
        QMetaObject::invokeMethod(this, [this, offset, length] {
            emit requestFinished(offset, length, false, QStringLiteral("source not ready"));
        }, Qt::QueuedConnection);
        return;
    }
    if (offset < 0 || length <= 0 || offset + length > m_size) {
        QMetaObject::invokeMethod(this, [this, offset, length] {
            emit requestFinished(offset, length, false, QStringLiteral("range outside file"));
        }, Qt::QueuedConnection);
        return;
    }
    resetBlocks();
    m_reqActive = true;
    m_reqOffset = offset;
    m_reqLength = length;
    for (const Range &b : planBlocks(offset, length, m_size)) {
        Block blk;
        blk.start = b.first;
        blk.end = b.second;
        m_blocks.append(blk);
    }
    m_lastDataMs = m_clock.elapsed();
    updateState();
    sendBlockRequests();
}

void Ed2kRangeSource::sendBlockRequests()
{
    if (!m_pc || !m_reqActive) return;
    // eMule style: always (re)send the up-to-3 oldest unfinished blocks; the uploader ignores
    // duplicates of blocks it already queued.
    QList<int> pick;
    bool anyNew = false;
    for (int k = 0; k < m_blocks.size() && pick.size() < 3; ++k) {
        if (m_blocks[k].done) continue;
        pick.append(k);
        if (!m_blocks[k].requested) anyNew = true;
    }
    if (pick.isEmpty() || !anyNew) return;
    bool large = false;
    for (int k : pick) if (m_blocks[k].end > 0xFFFFFFFFLL) large = true;
    Writer w;
    w.hash16(m_hashes.ed2k);
    for (int pass = 0; pass < 2; ++pass) {
        for (int slot = 0; slot < 3; ++slot) {
            qint64 v = 0;
            if (slot < pick.size()) v = pass == 0 ? m_blocks[pick[slot]].start : m_blocks[pick[slot]].end;
            if (large) w.u64(quint64(v)); else w.u32(quint32(v));
        }
    }
    for (int k : pick) m_blocks[k].requested = true;
    if (large) m_pc->send(PROT_EMULE, OP_REQUESTPARTS_I64, w.data());
    else m_pc->send(PROT_EDONKEY, OP_REQUESTPARTS, w.data());
}

void Ed2kRangeSource::onData(qint64 start, qint64 end, const QByteArray &data)
{
    if (!m_reqActive) return;
    if (end <= start || end - start != data.size()) { m_core->log(QStringLiteral("corrupt data block from %1").arg(m_pc ? m_pc->describe() : QString())); return; }
    m_lastDataMs = m_clock.elapsed();
    for (Block &b : m_blocks) {
        if (start < b.start || start >= b.end || b.done || b.z) continue;
        if (end > b.end) return;                         // straddles our block: ignore
        const qint64 expect = b.start + b.got;
        if (start > expect) return;                      // gap: not supported, block will be re-requested
        const qint64 skip = expect - start;              // overlap with what we have
        if (skip >= data.size()) return;
        b.buf.append(data.constData() + skip, data.size() - skip);
        b.got += data.size() - skip;
        m_bytesReceived += data.size() - skip;
        if (b.got == b.end - b.start) b.done = true;
        break;
    }
    flushEmits();
}

void Ed2kRangeSource::freeZ(Block &b)
{
    if (b.z) { inflateEnd(b.z); delete b.z; b.z = nullptr; }
}

void Ed2kRangeSource::onCompressed(qint64 blockStart, quint32 total, const QByteArray &data)
{
    if (!m_reqActive) return;
    m_lastDataMs = m_clock.elapsed();
    for (Block &b : m_blocks) {
        if (b.start != blockStart || b.done) continue;
        if (!b.z) {
            if (b.got != 0 || total == 0 || total > 4 * EMBLOCKSIZE) return;
            b.z = new z_stream{};
            if (inflateInit(b.z) != Z_OK) { delete b.z; b.z = nullptr; return; }
            b.zTotal = total;
            b.zGot = 0;
        }
        if (total != b.zTotal || b.zGot + quint32(data.size()) > b.zTotal) {
            // inconsistent stream: restart this block
            freeZ(b); b.buf.clear(); b.got = 0; b.emitted = 0; b.requested = false;
            return;
        }
        b.zGot += quint32(data.size());
        b.z->next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
        b.z->avail_in = uInt(data.size());
        const qint64 want = b.end - b.start;
        char out[32768];
        int rc = Z_OK;
        while (b.z->avail_in > 0 || rc == Z_OK) {
            b.z->next_out = reinterpret_cast<Bytef *>(out);
            b.z->avail_out = sizeof(out);
            rc = inflate(b.z, Z_NO_FLUSH);
            const qint64 produced = qint64(sizeof(out) - b.z->avail_out);
            if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) break;
            if (b.got + produced > want) { rc = Z_DATA_ERROR; break; }
            b.buf.append(out, produced);
            b.got += produced;
            m_bytesReceived += produced;
            if (rc == Z_STREAM_END || produced == 0) break;
        }
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            m_core->log(QStringLiteral("zlib error %1 in block %2 from %3").arg(rc).arg(b.start).arg(m_pc ? m_pc->describe() : QString()));
            freeZ(b); b.buf.clear(); b.got = 0; b.emitted = 0; b.requested = false;
            return;
        }
        if (b.got == want) { b.done = true; freeZ(b); }
        else if (b.zGot == b.zTotal) {
            // stream ended short: re-request
            freeZ(b); b.buf.clear(); b.got = 0; b.emitted = 0; b.requested = false;
        }
        break;
    }
    flushEmits();
}

void Ed2kRangeSource::flushEmits()
{
    QPointer<Ed2kRangeSource> self(this);
    for (Block &b : m_blocks) {
        if (!b.buf.isEmpty()) {
            const QByteArray chunk = b.buf;
            const qint64 at = b.start + b.emitted;
            b.emitted += chunk.size();
            b.buf.clear();
            emit data(at, chunk);
            if (!self || !m_reqActive) return;
        }
        if (!b.done) break;   // later blocks wait until this one is complete (in-order delivery)
    }
    bool all = true;
    for (const Block &b : m_blocks) if (!b.done || !b.buf.isEmpty()) { all = false; break; }
    if (all) { finishRequest(true, QString()); return; }
    sendBlockRequests();
}

void Ed2kRangeSource::finishRequest(bool ok, const QString &error)
{
    if (!m_reqActive) return;
    m_reqActive = false;
    const qint64 off = m_reqOffset, len = m_reqLength;
    resetBlocks();
    m_idleSinceMs = m_clock.elapsed();
    updateState();
    emit requestFinished(off, len, ok, error);
}

void Ed2kRangeSource::resetBlocks()
{
    for (Block &b : m_blocks) freeZ(b);
    m_blocks.clear();
}

void Ed2kRangeSource::cancelRequests()
{
    if (!m_reqActive) return;
    m_reqActive = false;
    resetBlocks();
    m_idleSinceMs = m_clock.elapsed();
    updateState();
}

void Ed2kRangeSource::releaseConnection(const QString &why)
{
    if (!m_pc) return;
    PeerConnection *pc = m_pc;
    m_pc = nullptr;
    pc->setDownloadSink(nullptr, nullptr);
    if (!pc->uploadRole) pc->close(why);
}

void Ed2kRangeSource::setCloseReason(const QString &code, const QString &text)
{
    Q_UNUSED(text)
    // pause, remove, and the program closing: the user's own stops ("dropped", "banned", "done" are the engine's)
    m_userStop = code == QLatin1String("paused") || code == QLatin1String("cancelled") || code == QLatin1String("quit");
}

void Ed2kRangeSource::close()
{
    if (m_phase == PClosed) return;
    m_tick.stop();
    if (m_pc && m_phase == PDownloading) m_pc->send(PROT_EDONKEY, OP_CANCELTRANSFER);
    if (m_phase == PDownloading && m_userStop) m_core->noteUserStop(m_peer.key(), m_hashes.ed2k);
    m_reqActive = false;
    resetBlocks();
    releaseConnection(QStringLiteral("source closed"));
    setPhase(PClosed);
}

bool Ed2kRangeSource::udpReaskPossible() const
{
    // eMule: only HighID peers with client UDP, only while we can be reached (a firewalled
    // downloader must re-ask over TCP to be able to take the slot), no source exchange due
    if (!m_core->caps().udpPort || !m_peerUdpPort || !m_peerUdpVersion || m_udpFails >= 3) return false;
    if (m_peer.isLowId() || m_peer.needsKadCallback() || !m_peer.ip) return false;
    if (!m_core->canAcceptIncoming()) return false;
    if (m_core->sourceExchangeAllowed(m_peer.key(), m_hashes.ed2k)) return false;
    return true;
}

void Ed2kRangeSource::sendUdpReask()
{
    Writer w(24);
    w.hash16(m_hashes.ed2k);
    if (m_peerUdpVersion > 3) w.bytes(m_core->partStatus(m_hashes.ed2k, m_size));   // our verified parts
    if (m_peerUdpVersion > 2) w.u16(0);   // complete sources we know
    const bool obf = (m_peer.cryptOptions & 0x01) && m_peer.userHash.size() == 16;
    if (!m_core->sendClientUdp(m_peer.ip, m_peerUdpPort, OP_REASKFILEPING, w.data(), obf ? m_peer.userHash : QByteArray())) {
        connectNow(!m_triedPlain);
        return;
    }
    m_udpPending = true;
    m_udpSentMs = m_clock.elapsed();
    ++m_udpReasks;
}

void Ed2kRangeSource::udpAnswer(quint8 opcode, const QByteArray &payload)
{
    m_udpPending = false;
    if (opcode == OP_FILENOTFOUND) { fail(QStringLiteral("file not shared by peer (UDP)")); return; }
    m_core->noteAsk(m_peer.key(), m_hashes.ed2k);
    m_everQueued = true;
    scheduleReask();
    if (opcode == OP_QUEUEFULL) {
        m_rank = 0;
        setPhase(PQueuedOffline, QStringLiteral("Queue full"));
        return;
    }
    Reader r(payload);
    if (m_peerUdpVersion > 3) {
        const int before = int(m_available.size());
        handleFileStatus(r);
        if (m_phase == PFailed) return;
        Q_UNUSED(before)
    }
    const quint16 rank = r.u16();
    if (r.ok()) m_rank = rank;
    setPhase(m_available.isEmpty() && m_gotStatus ? PNoParts : PQueuedOffline);
}

void Ed2kRangeSource::fail(const QString &why)
{
    if (m_reqActive) finishRequest(false, why);
    m_core->log(QStringLiteral("source %1: %2").arg(m_peer.key(), why));
    releaseConnection(why);
    setPhase(PFailed, why);
}

void Ed2kRangeSource::tick()
{
    const qint64 now = m_clock.elapsed();
    switch (m_phase) {
    case PWaitCallback:
        if (now - m_phaseSinceMs > kCallbackTimeoutMs) fail(QStringLiteral("no callback from LowID peer"));
        break;
    case PFileRequest:
    case PHashset:
    case PUploadRequest:
        if (now - m_phaseSinceMs > kPhaseTimeoutMs) {
            if (m_phase == PHashset) { sendStartUpload(); break; }   // continue without hashset
            fail(QStringLiteral("peer does not answer"));
        }
        break;
    case PQueued:
    case PQueuedOffline:
    case PNoParts:
        if (now >= m_nextReaskMs && m_nextReaskMs > 0) {
            const qint64 since = m_core->msSinceAsk(m_peer.key(), m_hashes.ed2k);
            if (since >= 0 && since < FILEREASKTIME_MS) { m_nextReaskMs = now + (FILEREASKTIME_MS - since); break; }
            if (m_pc && m_pc->isOpen() && m_pc->handshakeDone()) sendFileRequest();
            else if (!m_udpPending && udpReaskPossible()) sendUdpReask();
            else if (!m_udpPending) connectNow(!m_triedPlain);
        }
        if (m_udpPending && now - m_udpSentMs > 20000) {
            // no UDP answer: count the failure and re-ask over TCP
            m_udpPending = false;
            ++m_udpFails;
            connectNow(!m_triedPlain);
        }
        break;
    case PDownloading:
        if (m_reqActive && now - m_lastDataMs > kDownloadTimeoutMs) {
            finishRequest(false, QStringLiteral("download timeout"));
            if (m_pc) m_pc->send(PROT_EDONKEY, OP_CANCELTRANSFER);
            m_everQueued = true;
            scheduleReask();
            releaseConnection(QStringLiteral("download timeout"));
            setPhase(PQueuedOffline);
        } else if (!m_reqActive && now - m_idleSinceMs > kReadyIdleMs) {
            if (m_pc) m_pc->send(PROT_EDONKEY, OP_CANCELTRANSFER);
            m_everQueued = true;
            scheduleReask();
            setPhase(PQueued);
            if (m_pc) m_pc->setIdleTimeoutMs(45000);
        }
        break;
    default:
        break;
    }
}

} // namespace mx::ed2k
