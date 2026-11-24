// SPDX-License-Identifier: GPL-3.0-or-later
// Parts of this file are adapted from eMule 0.50a (Copyright (C) the eMule team,
// https://www.emule-project.net; ClientUDPSocket.cpp), GPL-2.0-or-later;
// this file is distributed under GPL-3.0-or-later. See NOTICE.
#include "ed2k_upload.h"
#include "ed2k_core.h"
#include "ed2k_io.h"
#include "ed2k_opcodes.h"
#include "ed2k_peer.h"

#include <QtCore/QFileInfo>

#include <algorithm>
#include <QtCore/QRunnable>

namespace mx::ed2k {

static constexpr qint64 kSessionMaxBytes = PARTSIZE + 20 * 1024;     // eMule SESSIONMAXTRANS
static constexpr qint64 kSessionMaxMs = 60LL * 60 * 1000;
static constexpr qint64 kPurgeQueueMs = 60LL * 60 * 1000;
static constexpr int kMaxQueue = 1000;
static constexpr qint64 kChunk = 10240;

UploadManager::UploadManager(Ed2kCore *core) : QObject(core), m_core(core)
{
    m_pool.setMaxThreadCount(2);
    m_pool.setExpiryTimeout(30000);
    m_clock.start();
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &UploadManager::tick);
    m_tick.start();
}

UploadManager::~UploadManager()
{
    m_pool.waitForDone();   // workers post results to us: never let them outlive the manager
}

int UploadManager::activeUploads() const
{
    int n = 0;
    for (const Client &c : m_clients) if (c.uploading) ++n;
    return n;
}

int UploadManager::rateBps() const
{
    int r = 0;
    for (const Client &c : m_clients) if (c.uploading) r += c.rateBps;
    return r;
}

void UploadManager::stop()
{
    const QStringList keys = m_clients.keys();
    for (const QString &k : keys) {
        auto it = m_clients.find(k);
        if (it == m_clients.end()) continue;
        if (it->uploading) endSession(*it, QStringLiteral("stopping"), false);
        else dropClient(k);
    }
    m_clients.clear();
    m_waiting.clear();
}

bool UploadManager::lookupFile(const QByteArray &hash, mx::SharedFile *out) const
{
    if (hash.size() != 16) return false;
    return m_core->sharedFile(hash, out);
}

UploadManager::Client *UploadManager::clientFor(PeerConnection *pc, bool create)
{
    const QString key = pc->info().userHash.size() == 16 ? toHex(pc->info().userHash) : pc->peerKey();
    auto it = m_clients.find(key);
    if (it == m_clients.end()) {
        if (!create) return nullptr;
        Client c;
        c.key = key;
        it = m_clients.insert(key, c);
    }
    Client &c = *it;
    // [up-1] an upload session belongs to the connection it runs on: a second connection of the same
    // client (two downloads of one file, a reconnect while the old socket lingers) must not take it
    // over here, or the requests of one were answered on the other and the bytes went uncounted
    // (the row said "0 B sent" while the data flowed). onStartUpload() decides about moving it.
    if (!(c.uploading && c.pc && c.pc != pc && c.pc->isOpen())) c.pc = pc;
    c.userHash = pc->info().userHash;
    c.ip = pc->ip();
    c.port = pc->listenPort();
    c.udpPort = pc->info().udpPort;
    c.udpVersion = pc->info().udpVersion;
    c.cryptUdp = pc->info().supportsCrypt;
    c.cryptOptions = quint8((pc->info().supportsCrypt ? 0x01 : 0) | (pc->info().requestsCrypt ? 0x02 : 0) |
                            (pc->info().requiresCrypt ? 0x04 : 0));
    c.lowId = isLowId(pc->info().clientId) && pc->info().clientId != 0 && !pc->outgoing();
    c.name = pc->info().name;
    c.software = pc->info().software();
    return &c;
}

void UploadManager::sendNoFile(PeerConnection *pc, const QByteArray &hash)
{
    Writer w; w.hash16(hash);
    pc->send(PROT_EDONKEY, OP_FILEREQANSNOFIL, w.data());
}

static void skipExtInfo(Reader &r, const PeerInfo &info)
{
    if (info.extendedRequestsVersion > 0 && r.remaining() >= 2) {
        const quint16 n = r.u16();
        if (n) r.skip((n + 7) / 8);
    }
    if (info.extendedRequestsVersion > 1 && r.remaining() >= 2) r.u16();
}

void UploadManager::answerFileName(PeerConnection *pc, const QByteArray &hash, Reader &ext, const mx::SharedFile &f)
{
    skipExtInfo(ext, pc->info());
    Writer w;
    w.hash16(hash).str16(f.name);
    pc->send(PROT_EDONKEY, OP_REQFILENAMEANSWER, w.data());
}

void UploadManager::answerFileStatus(PeerConnection *pc, const mx::SharedFile &f)
{
    Writer w;
    w.hash16(f.hashes.ed2k);
    if (m_core->isPartialFile(f.hashes.ed2k)) w.bytes(m_core->partStatus(f.hashes.ed2k, f.size));   // verified parts
    else w.u16(0);   // complete file
    pc->send(PROT_EDONKEY, OP_FILESTATUS, w.data());
}

QList<QByteArray> UploadManager::partHashes(const mx::SharedFile &f) const
{
    if (!m_core->ctx().library) return {};
    QList<QByteArray> list = m_core->ctx().library->ed2kPartHashes(f.hashes);
    if (list.isEmpty()) list = m_core->downloadPartHashes(f.hashes.ed2k);   // partial file: the hashset we verified
    const int expected = ed2kHashsetCount(f.size);
    if (expected == 0) return {};
    if (list.size() == expected - 1 && f.size % PARTSIZE == 0) list.append(md4(QByteArray()));
    if (list.size() != expected || !verifyHashset(f.hashes.ed2k, f.size, list)) return {};
    return list;
}

void UploadManager::answerHashset(PeerConnection *pc, const QByteArray &hash)
{
    mx::SharedFile f;
    if (!lookupFile(hash, &f)) { sendNoFile(pc, hash); return; }
    const QList<QByteArray> parts = partHashes(f);
    if (parts.isEmpty() && f.size >= PARTSIZE) return;   // cannot answer (hasher not done?)
    Writer w;
    w.hash16(hash).u16(quint16(parts.size()));
    for (const QByteArray &h : parts) w.hash16(h);
    pc->send(PROT_EDONKEY, OP_HASHSETANSWER, w.data(), true);
}

void UploadManager::answerMultiPacket(PeerConnection *pc, const Packet &p)
{
    Reader r(p.payload);
    const QByteArray hash = r.hash16();
    qint64 size = 0;
    if (p.opcode == OP_MULTIPACKET_EXT) size = qint64(r.u64());
    if (!r.ok()) return;
    mx::SharedFile f;
    if (!lookupFile(hash, &f) || (size && size != f.size)) { sendNoFile(pc, hash); return; }
    Writer out;
    out.hash16(hash);
    while (r.remaining() > 0) {
        const quint8 sub = r.u8();
        bool stop = false;
        switch (sub) {
        case OP_REQUESTFILENAME:
            skipExtInfo(r, pc->info());
            out.u8(OP_REQFILENAMEANSWER).str16(f.name);
            break;
        case OP_SETREQFILEID:
            out.u8(OP_FILESTATUS);
            if (m_core->isPartialFile(hash)) out.bytes(m_core->partStatus(hash, f.size)); else out.u16(0);
            break;
        case OP_REQUESTSOURCES2:
            r.skip(3);
            break;
        case OP_REQUESTSOURCES:
            break;
        case OP_AICHFILEHASHREQ:
            if (f.hashes.aich.size() == 20 && pc->info().aichVersion > 0) out.u8(OP_AICHFILEHASHANS).bytes(f.hashes.aich);
            break;
        default:
            stop = true;   // unknown sub-opcode: its length is unknown
            break;
        }
        if (stop || !r.ok()) break;
    }
    if (out.size() > 16) pc->send(PROT_EMULE, OP_MULTIPACKETANSWER, out.data());
    if (Client *c = clientFor(pc, false)) c->lastAskMs = m_clock.elapsed();
}

void UploadManager::handlePacket(PeerConnection *pc, const Packet &p)
{
    Reader r(p.payload);
    if (p.proto == PROT_EDONKEY) {
        switch (p.opcode) {
        case OP_REQUESTFILENAME: {
            const QByteArray h = r.hash16();
            if (!r.ok()) return;
            mx::SharedFile f;
            if (!lookupFile(h, &f)) { sendNoFile(pc, h); return; }
            answerFileName(pc, h, r, f);
            return;
        }
        case OP_SETREQFILEID: {
            const QByteArray h = r.hash16();
            if (!r.ok()) return;
            mx::SharedFile f;
            if (!lookupFile(h, &f)) { sendNoFile(pc, h); return; }
            answerFileStatus(pc, f);
            return;
        }
        case OP_HASHSETREQUEST: {
            const QByteArray h = r.hash16();
            if (r.ok()) answerHashset(pc, h);
            return;
        }
        case OP_STARTUPLOADREQ: {
            const QByteArray h = r.hash16();
            if (r.ok()) onStartUpload(pc, h);
            return;
        }
        case OP_REQUESTPARTS: onRequestParts(pc, p, false); return;
        case OP_CANCELTRANSFER:
        case OP_END_OF_DOWNLOAD: {
            Client *c = clientFor(pc, false);
            if (!c) return;
            if (c->uploading) endSession(*c, QStringLiteral("cancelled by peer"), false);
            else dropClient(c->key);
            return;
        }
        default: return;
        }
    }
    switch (p.opcode) {
    case OP_REQUESTPARTS_I64: onRequestParts(pc, p, true); return;
    case OP_AICHFILEHASHREQ: {
        const QByteArray h = r.hash16();
        mx::SharedFile f;
        if (r.ok() && lookupFile(h, &f) && f.hashes.aich.size() == 20) {
            Writer w; w.hash16(h).bytes(f.hashes.aich);
            pc->send(PROT_EMULE, OP_AICHFILEHASHANS, w.data());
        }
        return;
    }
    case OP_MULTIPACKET:
    case OP_MULTIPACKET_EXT:
        answerMultiPacket(pc, p);
        return;
    default:
        return;   // source requests for complete files, AICH recovery data, ext2: not offered
    }
}

void UploadManager::handleUdpReask(quint32 ip, quint16 port, const QByteArray &payload, bool obfuscated)
{
    Reader r(payload);
    const QByteArray hash = r.hash16();
    if (!r.ok()) return;
    Client *c = nullptr;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it)
        if (!it->uploading && it->ip == ip && it->udpPort == port && m_waiting.contains(it->key)) { c = &it.value(); break; }
    const QByteArray uh = (c && (c->cryptUdp || obfuscated)) ? c->userHash : QByteArray();
    mx::SharedFile f;
    if (!lookupFile(hash, &f)) {
        m_core->sendClientUdp(ip, port, OP_FILENOTFOUND, QByteArray(), uh);
        return;
    }
    if (!c) {
        // unknown re-asker: tell it to come back over TCP when we are (almost) full, else stay silent (eMule)
        if (m_waiting.size() + 50 > kMaxQueue) m_core->sendClientUdp(ip, port, OP_QUEUEFULL, QByteArray(), QByteArray());
        return;
    }
    if (c->fileHash != hash) return;
    c->lastAskMs = m_clock.elapsed();
    Writer w(8);
    if (c->udpVersion > 3) {
        if (m_core->isPartialFile(hash)) w.bytes(m_core->partStatus(hash, f.size)); else w.u16(0);   // complete file
    }
    w.u16(quint16(qMin<qsizetype>(m_waiting.indexOf(c->key) + 1, 0xFFFF)));
    m_core->sendClientUdp(ip, port, OP_REASKACK, w.data(), uh);
    ++m_udpReasks;
}

void UploadManager::onStartUpload(PeerConnection *pc, const QByteArray &hash)
{
    mx::SharedFile f;
    if (!lookupFile(hash, &f)) { sendNoFile(pc, hash); return; }
    Client *c = clientFor(pc, true);
    const qint64 now = m_clock.elapsed();
    c->lastAskMs = now;
    if (c->uploading && c->pc != pc) {
        // [up-1] the session runs on another connection of this client: move it here when that one
        // is gone (a reconnect), else this connection waits its turn like a queued request (one
        // upload slot per client, as in eMule)
        const bool gone = !c->pc || !c->pc->isOpen();
        if (gone && c->fileHash == hash) {
            m_core->log(QStringLiteral("upload to %1 moves to its new connection").arg(pc->describe()));
            c->pc = pc;
            c->pc->uploadRole = true;
            countSentOn(*c);
            pc->send(PROT_EDONKEY, OP_ACCEPTUPLOADREQ);
            return;
        }
        Writer w;
        w.u32(1);
        pc->send(PROT_EDONKEY, OP_QUEUERANK, w.data());
        return;
    }
    if (c->uploading) {
        if (c->fileHash == hash) { pc->send(PROT_EDONKEY, OP_ACCEPTUPLOADREQ); return; }
        endSession(*c, QStringLiteral("peer switched file"), true);
        c = clientFor(pc, true);
    }
    const auto blocked = m_blocked.constFind(c->key);
    if (blocked != m_blocked.constEnd()) {
        if (*blocked > now) { if (c->uploadId.isEmpty()) m_clients.remove(c->key); return; }   // cancelled by us: ignore
        m_blocked.remove(c->key);
    }
    const bool fileChanged = c->fileHash != hash && !c->uploadId.isEmpty();
    c->fileHash = hash;
    c->file = f;
    c->partial = m_core->isPartialFile(hash);
    if (!m_waiting.contains(c->key)) {
        if (m_waiting.size() >= kMaxQueue) { m_clients.remove(c->key); pc->close(QStringLiteral("upload queue full")); return; }
        c->waitingSinceMs = now;
        enqueue(*c);
    } else if (fileChanged) {
        c->reportedRank = -1;   // same queue place, new file name in the row
    }
    process();
    auto it = m_clients.find(pc->info().userHash.size() == 16 ? toHex(pc->info().userHash) : pc->peerKey());
    if (it != m_clients.end() && !it->uploading) {
        const int rank = int(m_waiting.indexOf(it->key)) + 1;
        if (rank > 0) {
            if (pc->info().emuleProtocol) {
                Writer w; w.u16(quint16(qMin(rank, 0xFFFF))).u32(0).u32(0).u16(0);
                pc->send(PROT_EMULE, OP_QUEUERANKING, w.data());
            } else {
                Writer w; w.u32(quint32(rank));
                pc->send(PROT_EDONKEY, OP_QUEUERANK, w.data());
            }
        }
    }
}

void UploadManager::sharedFilesChanged()
{
    QStringList gone;
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        if (it->fileHash.size() != 16 || it->partial) continue;   // a running download is served from its verified parts
        if (!lookupFile(it->fileHash, nullptr)) gone << it.key();
    }
    for (const QString &k : std::as_const(gone)) {
        auto it = m_clients.find(k);
        if (it == m_clients.end()) continue;
        Client &c = *it;
        if (c.pc && c.pc->isOpen() && c.pc->handshakeDone()) sendNoFile(c.pc, c.fileHash);
        if (c.uploading) {
            ++c.counter;   // a block read in flight is dropped
            c.endText = QStringLiteral("Stopped: the file is no longer shared");
            endSession(c, QStringLiteral("file no longer shared"), false);
        } else {
            m_core->log(QStringLiteral("queued client %1 dropped: %2 is no longer shared").arg(c.key, c.file.name));
            dropClient(k);
        }
    }
    if (!gone.isEmpty()) {
        reportQueue();
        process();
    }
}

void UploadManager::slotsFreed()
{
    process();
}

bool UploadManager::slotFree()
{
    if (mx::UploadSlots *p = m_core->slotPool()) return p->available(mx::Net::ED2K) > 0;
    return activeUploads() < m_core->uploadSlots();
}

void UploadManager::reportWaiting()
{
    mx::UploadSlots *p = m_core->slotPool();
    if (!p) return;
    // a client that is connected, or that we can call back (HighID), can start now; a LowID client
    // that is not connected only when it asks again (eMule: every half hour)
    int n = 0;
    for (const QString &k : std::as_const(m_waiting)) {
        auto it = m_clients.constFind(k);
        if (it == m_clients.cend()) continue;
        if ((it->pc && it->pc->isOpen() && it->pc->handshakeDone()) || (!it->lowId && it->ip && it->port)) ++n;
    }
    p->setWaiting(mx::Net::ED2K, n, int(m_core->peerConnectTimeoutMs()) + 10000);
}

void UploadManager::process()
{
    reportWaiting();
    for (int idx = 0; idx < m_waiting.size();) {
        const QString key = m_waiting[idx];
        auto it = m_clients.find(key);
        if (it == m_clients.end()) { m_waiting.removeAt(idx); continue; }
        Client &c = *it;
        if (!c.forced && !slotFree()) { ++idx; continue; }   // [glue-3] forced: "Start Transfer"
        if (c.pc && c.pc->isOpen() && c.pc->handshakeDone()) {
            startSession(c);        // removes from m_waiting
            continue;
        }
        if (!c.lowId && c.ip && c.port && !c.connectingBack) {
            PeerDescriptor d;
            d.ip = c.ip;
            d.port = c.port;
            d.userHash = c.userHash;
            d.cryptOptions = c.cryptOptions;   // [enc-1] was 0x01: obfuscated connect-backs to clients without crypt failed
            c.connectingBack = true;
            c.pc = m_core->connectPeer(d);
            if (!c.pc) { c.connectingBack = false; c.addNextConnect = true; }
            ++idx;
            continue;
        }
        c.addNextConnect = true;
        ++idx;
    }
}

void UploadManager::peerConnected(PeerConnection *pc)
{
    if (pc->info().userHash.size() != 16) return;
    auto it = m_clients.find(toHex(pc->info().userHash));
    if (it == m_clients.end() || it->uploading) return;
    it->pc = pc;
    if (it->connectingBack || it->addNextConnect) {
        it->connectingBack = false;
        m_core->log(QStringLiteral("queued client %1 is back").arg(pc->describe()));
        if (it->forced || slotFree()) startSession(*it);
    }
}

void UploadManager::peerClosed(PeerConnection *pc)
{
    QStringList keys;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it)
        if (it->pc == pc) keys << it.key();
    for (const QString &k : keys) {
        auto it = m_clients.find(k);
        if (it == m_clients.end()) continue;
        Client &c = *it;
        if (c.uploading) { endSession(c, QStringLiteral("connection closed"), false); continue; }
        if (c.connectingBack && !pc->handshakeDone()) {
            // could not reach a queued HighID client: forget it (eMule does the same)
            dropClient(k);
            continue;
        }
        c.pc = nullptr;
        c.connectingBack = false;
    }
    QMetaObject::invokeMethod(this, [this] { process(); }, Qt::QueuedConnection);
}

static bool alreadyCompressed(const QString &name)
{
    static const QStringList exts = {
        QStringLiteral("zip"), QStringLiteral("rar"), QStringLiteral("7z"), QStringLiteral("gz"), QStringLiteral("bz2"),
        QStringLiteral("xz"), QStringLiteral("zst"), QStringLiteral("cab"), QStringLiteral("cbz"), QStringLiteral("cbr"),
        QStringLiteral("ace"), QStringLiteral("mp3"), QStringLiteral("ogg"), QStringLiteral("opus"), QStringLiteral("flac"),
        QStringLiteral("aac"), QStringLiteral("m4a"), QStringLiteral("avi"), QStringLiteral("mkv"), QStringLiteral("mp4"),
        QStringLiteral("m4v"), QStringLiteral("webm"), QStringLiteral("ogm"), QStringLiteral("wmv"), QStringLiteral("mov"),
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"), QStringLiteral("gif"), QStringLiteral("webp"),
        QStringLiteral("squashfs"), QStringLiteral("deb"), QStringLiteral("rpm"),
    };
    return exts.contains(QFileInfo(name).suffix().toLower());
}

void UploadManager::startSession(Client &c)
{
    m_waiting.removeAll(c.key);
    if (mx::UploadSlots *p = m_core->slotPool()) p->acquire(mx::Net::ED2K, c.key, true);   // [net-3] checked by the caller
    c.uploading = true;
    c.addNextConnect = false;
    c.connectingBack = false;
    c.blocks.clear();
    c.sentBlocks.clear();
    c.sessionOver = false;
    c.readPending = false;        // a late result of an earlier session is dropped (counter mismatch)
    c.queuedBytes = 0;
    c.sessionBytes = 0;
    c.sessionStartMs = m_clock.elapsed();
    c.lastRequestMs = m_clock.elapsed();
    c.rateWindowBytes = 0;
    c.rateBps = 0;
    c.counter = ++m_counter;
    c.forced = false;
    c.reportedRank = -1;
    if (c.uploadId.isEmpty())   // [glue-3] a queued client keeps its row id
        c.uploadId = QStringLiteral("ed2k-up-%1-%2").arg(c.key.left(12)).arg(c.counter);
    c.compress = c.pc && c.pc->info().dataCompVersion > 0 && !alreadyCompressed(c.file.name)
                 && m_core->cfg().b(QStringLiteral("ed2k/compressUploads"), true);
    c.pc->uploadRole = true;
    c.pc->setIdleTimeoutMs(120000);
    countSentOn(c);
    c.pc->send(PROT_EDONKEY, OP_ACCEPTUPLOADREQ);
    m_core->log(QStringLiteral("upload slot to %1 for \"%2\"").arg(c.pc->describe(), c.file.name));
    report(c);
}

void UploadManager::countSentOn(Client &c)
{
    if (!c.pc) return;
    const QString key = c.key;
    PeerConnection *pc = c.pc;
    disconnect(pc, &PeerConnection::dataSent, this, nullptr);
    connect(pc, &PeerConnection::dataSent, this, [this, key, pc](qint64 n) {
        auto it = m_clients.find(key);
        if (it == m_clients.end() || !it->uploading) return;
        it->sessionBytes += n;   // what was written to the socket for this session
        it->rateWindowBytes += n;
        m_totalUploaded += n;
        if (it->pc == pc) pumpClient(*it);
    });
}

void UploadManager::endSession(Client &c, const QString &why, bool requeue)
{
    const QString key = c.key;
    if (c.uploading) {
        c.uploading = false;
        if (m_core->ctx().uploadSlots) m_core->ctx().uploadSlots->release(key);   // [net-3]
        c.blocks.clear();
        if (c.pc) {
            c.pc->conn()->clearDataQueue();
            c.pc->disconnect(this);
            if (requeue && c.pc->isOpen()) c.pc->send(PROT_EDONKEY, OP_OUTOFPARTREQS);
            c.pc->uploadRole = false;
        }
        m_core->log(QStringLiteral("upload to %1 ended (%2, %3 sent)").arg(c.pc ? c.pc->describe() : c.key, why, formatBytes(c.sessionBytes)));
        report(c, true);
        c.uploadId.clear();   // [glue-3] a re-queued client gets a new (Queued) row
        c.sessionBytes = 0;
    }
    if (requeue) {
        c.waitingSinceMs = m_clock.elapsed();
        if (!m_waiting.contains(key)) enqueue(c);
    } else {
        dropClient(key);
    }
    QMetaObject::invokeMethod(this, [this] { process(); }, Qt::QueuedConnection);
}

void UploadManager::onRequestParts(PeerConnection *pc, const Packet &p, bool i64)
{
    Client *c = clientFor(pc, false);
    if (!c || !c->uploading || c->pc != pc) return;
    Reader r(p.payload);
    const QByteArray hash = r.hash16();
    qint64 st[3], en[3];
    for (qint64 &v : st) v = i64 ? qint64(r.u64()) : qint64(r.u32());
    for (qint64 &v : en) v = i64 ? qint64(r.u64()) : qint64(r.u32());
    if (!r.ok() || hash != c->fileHash || c->sessionOver) return;
    const QList<bool> parts = c->partial ? m_core->availableParts(hash, c->file.size) : QList<bool>();
    for (int i = 0; i < 3; ++i) {
        if (en[i] <= st[i]) continue;
        if (st[i] < 0 || en[i] > c->file.size || en[i] - st[i] > 3 * EMBLOCKSIZE) continue;
        if (c->partial) {
            // partial file: only blocks inside verified parts
            const int pa = int(st[i] / PARTSIZE), pb = int((en[i] - 1) / PARTSIZE);
            if (pa != pb || pa >= parts.size() || !parts[pa]) continue;
        }
        bool dup = false;
        for (const Block &b : c->blocks) if (b.start == st[i] && b.end == en[i]) { dup = true; break; }
        for (const Block &b : c->sentBlocks) if (b.start == st[i] && b.end == en[i]) { dup = true; break; }
        if (dup || c->blocks.size() >= 12) continue;
        c->blocks.append({st[i], en[i]});
    }
    c->lastRequestMs = m_clock.elapsed();
    pumpClient(*c);
}

void UploadManager::pumpClient(Client &c)
{
    if (!c.uploading || !c.pc || !c.pc->isOpen() || !m_core->ctx().library) return;
    // one block at a time is read + packed on the worker thread; the socket queue stays ≤ 400 KiB
    if (!c.readPending && !c.blocks.isEmpty() && c.pc->conn()->queuedDataBytes() < 400 * 1024) {
        const Block b = c.blocks.takeFirst();
        c.sentBlocks.append(b);
        if (c.sentBlocks.size() > 16) c.sentBlocks.removeFirst();
        c.queuedBytes += b.end - b.start;
        startRead(c, b);
    }
    // session limits (eMule: ~one part or one hour): once reached and others are waiting, accept
    // no further block requests, drain what is queued, then give the slot to the next client
    if (!c.sessionOver && !m_waiting.isEmpty() &&
        (c.queuedBytes >= kSessionMaxBytes || m_clock.elapsed() - c.sessionStartMs > kSessionMaxMs)) {
        c.sessionOver = true;
        c.blocks.clear();
    }
    if (c.sessionOver && !c.readPending && c.pc && c.pc->conn()->queuedDataBytes() == 0)
        endSession(c, QStringLiteral("session limit reached"), true);
}

void UploadManager::startRead(Client &c, const Block &b)
{
    c.readPending = true;
    ReadJob job;
    job.key = c.key;
    job.counter = c.counter;
    job.block = b;
    mx::LibraryAccess *lib = m_core->ctx().library;
    const mx::FileHashes hashes = c.file.hashes;
    const QByteArray fileHash = c.fileHash;
    const bool compress = c.compress;
    QPointer<UploadManager> self(this);
    m_pool.start(QRunnable::create([self, this, lib, hashes, fileHash, compress, job] {
        // worker thread: LibraryAccess::read is thread-safe; the rest is pure computation
        const Block b = job.block;
        const QByteArray data = lib->read(hashes, b.start, b.end - b.start);
        QList<QPair<QByteArray, qint64>> packets;
        const bool ok = data.size() == b.end - b.start;
        bool packed = false;
        if (ok) {
            const bool large = b.end > 0xFFFFFFFFLL;
            QByteArray z;
            if (compress) z = zlibCompress(data, 1);
            if (!z.isEmpty() && z.size() < data.size()) {
                packed = true;
                const int nChunks = int((z.size() + kChunk - 1) / kChunk);
                qint64 accounted = 0;
                for (int i = 0; i < nChunks; ++i) {
                    const QByteArray part = z.mid(qsizetype(i) * kChunk, kChunk);
                    Writer w(part.size() + 32);
                    w.hash16(fileHash);
                    if (large) w.u64(quint64(b.start)); else w.u32(quint32(b.start));
                    w.u32(quint32(z.size()));
                    w.bytes(part);
                    const qint64 share = (i + 1 == nChunks) ? (data.size() - accounted) : (qint64(part.size()) * data.size() / z.size());
                    accounted += share;
                    packets.append({encodePacket(PROT_EMULE, large ? OP_COMPRESSEDPART_I64 : OP_COMPRESSEDPART, w.data()), share});
                }
            } else {
                for (qint64 off = 0; off < data.size(); off += kChunk) {
                    const qint64 n = qMin<qint64>(kChunk, data.size() - off);
                    const qint64 s = b.start + off, e = s + n;
                    Writer w(n + 40);
                    w.hash16(fileHash);
                    if (large) w.u64(quint64(s)).u64(quint64(e)); else w.u32(quint32(s)).u32(quint32(e));
                    w.bytes(data.constData() + off, n);
                    packets.append({large ? encodePacket(PROT_EMULE, OP_SENDINGPART_I64, w.data()) : encodePacket(PROT_EDONKEY, OP_SENDINGPART, w.data()), n});
                }
            }
        }
        // the manager waits for its pool in its destructor, so `this` is alive here
        QMetaObject::invokeMethod(this, [self, job, packets, ok, packed] {
            if (self) self->onReadDone(job, packets, ok, packed);
        }, Qt::QueuedConnection);
    }));
}

void UploadManager::onReadDone(const ReadJob &job, const QList<QPair<QByteArray, qint64>> &packets, bool ok, bool packed)
{
    auto it = m_clients.find(job.key);
    if (it == m_clients.end() || it->counter != job.counter) return;   // session ended meanwhile
    Client &c = *it;
    c.readPending = false;
    if (!c.uploading || !c.pc || !c.pc->isOpen()) return;
    if (!ok) { endSession(c, QStringLiteral("read error at %1").arg(job.block.start), false); return; }
    if (packed) ++m_packedBlocks; else ++m_plainBlocks;
    for (const auto &pk : packets) c.pc->queueData(pk.first, pk.second);
    pumpClient(c);
}

mx::TransferInfo UploadManager::info(const Client &c) const
{
    mx::TransferInfo t;
    t.id = c.uploadId;
    t.isUpload = true;
    t.name = c.file.name;
    t.savePath = c.file.path;
    t.size = c.file.size;
    t.done = c.sessionBytes;
    t.rateUpBps = c.rateBps;
    const int rank = c.uploading ? -1 : int(m_waiting.indexOf(c.key)) + 1;
    t.state = c.uploading ? mx::TransferState::Transferring
                          : rank > 0 ? mx::TransferState::Queued
                          : !c.endText.isEmpty() ? mx::TransferState::Error : mx::TransferState::Completed;
    t.statusText = c.uploading ? QStringLiteral("Uploading")
                   : rank > 0  ? QStringLiteral("Queued (position %1 of %2)").arg(rank).arg(m_waiting.size())
                   : !c.endText.isEmpty() ? c.endText : QStringLiteral("Finished");
    t.priority = c.priority;
    t.queuePosition = rank > 0 ? rank : 0;
    t.hashes = c.file.hashes;
    t.networks = {mx::Net::ED2K};
    t.peer.net = mx::Net::ED2K;
    t.peer.id = c.userHash.size() == 16 ? toHex(c.userHash) : c.key;
    t.peer.name = c.name.isEmpty() ? t.peer.id : c.name;
    t.peer.address = QStringLiteral("%1:%2").arg(ipToString(c.ip)).arg(c.port);
    mx::TransferSource s;
    s.net = mx::Net::ED2K;
    s.user = t.peer;
    s.statusText = t.statusText;
    s.bytes = c.sessionBytes;
    s.rateBps = c.rateBps;
    s.connection = QStringLiteral("eD2k");
    s.client = c.software;
    t.sources = {s};
    t.sourcesTotal = 1;
    t.sourcesActive = c.uploading ? 1 : 0;
    t.added = QDateTime::currentDateTime().addMSecs(-(m_clock.elapsed() - (c.uploading ? c.sessionStartMs : c.waitingSinceMs)));
    return t;
}

void UploadManager::report(Client &c, bool finished)
{
    if (c.uploadId.isEmpty()) return;
    emit m_core->uploadChanged(info(c));
    if (finished) emit m_core->uploadFinished(c.uploadId);
}

void UploadManager::tick()
{
    const qint64 now = m_clock.elapsed();
    QStringList keys = m_clients.keys();
    for (const QString &k : keys) {
        auto it = m_clients.find(k);
        if (it == m_clients.end()) continue;
        Client &c = *it;
        if (c.uploading) {
            c.rateBps = int(c.rateWindowBytes);   // bytes during the last second
            c.rateWindowBytes = 0;
            if (!c.pc || !c.pc->isOpen()) { endSession(c, QStringLiteral("connection lost"), false); continue; }
            if (c.blocks.isEmpty() && c.pc->conn()->queuedDataBytes() == 0 && now - c.lastRequestMs > 45000) {
                endSession(c, QStringLiteral("no requests"), false);
                continue;
            }
            pumpClient(c);
            auto again = m_clients.find(k);
            if (again != m_clients.end() && again->uploading) report(*again);
        } else if (now - c.lastAskMs > kPurgeQueueMs) {
            dropClient(k);
        }
    }
    for (auto it = m_blocked.begin(); it != m_blocked.end();)
        it = it.value() <= now ? m_blocked.erase(it) : std::next(it);
    process();
    reportQueue();
}

// ---------------------------------------------------------------- [glue-3] queue control
void UploadManager::enqueue(Client &c)
{
    if (!m_waiting.contains(c.key)) m_waiting.append(c.key);
    if (c.uploadId.isEmpty())
        c.uploadId = QStringLiteral("ed2k-up-%1-%2").arg(c.key.left(12)).arg(++m_counter);
    c.reportedRank = -1;
    sortWaiting();
    reportQueue();
    reportWaiting();
}

void UploadManager::sortWaiting()
{
    // priority first (high → front), FIFO inside a priority (m_waiting is already in arrival order)
    std::stable_sort(m_waiting.begin(), m_waiting.end(), [this](const QString &a, const QString &b) {
        return m_clients.value(a).priority > m_clients.value(b).priority;
    });
}

void UploadManager::reportQueue()
{
    for (int i = 0; i < m_waiting.size(); ++i) {
        auto it = m_clients.find(m_waiting[i]);
        if (it == m_clients.end() || it->uploading || it->uploadId.isEmpty()) continue;
        if (it->reportedRank == i + 1) continue;
        it->reportedRank = i + 1;
        emit m_core->uploadChanged(info(*it));
    }
}

void UploadManager::dropClient(const QString &key)
{
    m_waiting.removeAll(key);
    reportWaiting();
    auto it = m_clients.find(key);
    if (it == m_clients.end()) return;
    if (it->uploading && m_core->ctx().uploadSlots) m_core->ctx().uploadSlots->release(key);   // [net-3]
    const QString id = it->uploading ? QString() : it->uploadId;
    m_clients.erase(it);
    if (!id.isEmpty()) emit m_core->uploadFinished(id);   // the core removes a row that was only queued
    for (auto &c : m_clients) c.reportedRank = -1;        // positions behind it moved
}

UploadManager::Client *UploadManager::clientByUploadId(const QString &uploadId)
{
    if (uploadId.isEmpty()) return nullptr;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it)
        if (it->uploadId == uploadId) return &it.value();
    return nullptr;
}

QStringList UploadManager::waitingUploadIds() const
{
    QStringList out;
    for (const QString &k : m_waiting) out << m_clients.value(k).uploadId;
    return out;
}

bool UploadManager::startUploadNow(const QString &uploadId)
{
    Client *c = clientByUploadId(uploadId);
    if (!c) return false;
    if (c->uploading) return true;
    if (!m_waiting.contains(c->key)) return false;
    c->forced = true;
    c->priority = qMax(c->priority, 1);
    sortWaiting();
    m_core->log(QStringLiteral("upload slot forced for %1").arg(c->name.isEmpty() ? c->key : c->name));
    process();   // starts it now when connected, else connects back (LowID: when it re-asks)
    reportQueue();
    return true;
}

bool UploadManager::setUploadPriority(const QString &uploadId, int priority)
{
    Client *c = clientByUploadId(uploadId);
    if (!c) return false;
    c->priority = qBound(-1, priority, 1);
    if (!c->uploading) {
        sortWaiting();
        for (auto &o : m_clients) o.reportedRank = -1;
        reportQueue();
    } else {
        report(*c);
    }
    return true;
}

bool UploadManager::cancelUpload(const QString &uploadId)
{
    Client *c = clientByUploadId(uploadId);
    if (!c) return false;
    const QString key = c->key;
    m_blocked.insert(key, m_clock.elapsed() + 10 * 60 * 1000);   // do not take it back right away
    if (c->uploading) {
        c->priority = 0;
        endSession(*c, QStringLiteral("cancelled locally"), false);
    } else {
        dropClient(key);
    }
    return true;
}

} // namespace mx::ed2k
