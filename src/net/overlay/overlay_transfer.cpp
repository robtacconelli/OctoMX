// SPDX-License-Identifier: GPL-3.0-or-later
// Net::Overlay: direct transfers: OverlaySource (download side) and upload slots / queue.
#include "overlay_transfer.h"

#include "overlay_plugin_impl.h"

#include <QtCore/QJsonArray>
#include <QtCore/QRandomGenerator>
#include <QtCore/QThreadPool>
#include <QtCore/QtEndian>
#include <QtNetwork/QHostAddress>

#include "mx/settings_keys.h"

namespace mx::ovl {

QByteArray makeDataFrame(quint32 sid, quint32 rq, qint64 off, const char *data, int len)
{
    QByteArray f(kDataHeader + len, Qt::Uninitialized);
    char *p = f.data();
    p[0] = 'D';
    p[1] = char(kWireVersion);
    qToBigEndian<quint32>(sid, p + 2);
    qToBigEndian<quint32>(rq, p + 6);
    qToBigEndian<quint64>(static_cast<quint64>(off), p + 10);
    memcpy(p + kDataHeader, data, static_cast<size_t>(len));
    return f;
}

bool parseDataFrame(const QByteArray &f, quint32 *sid, quint32 *rq, qint64 *off, int *payloadPos)
{
    if (f.size() <= kDataHeader || f.size() > kDataHeader + kMaxFramePayload) return false;
    if (f[0] != 'D' || f[1] != char(kWireVersion)) return false;
    *sid = qFromBigEndian<quint32>(f.constData() + 2);
    *rq = qFromBigEndian<quint32>(f.constData() + 6);
    const quint64 o = qFromBigEndian<quint64>(f.constData() + 10);
    if (o > (quint64(1) << 52)) return false;
    *off = static_cast<qint64>(o);
    *payloadPos = kDataHeader;
    return true;
}

// =============================================================================== download side
OverlaySource::OverlaySource(OverlayPlugin *plugin, const QString &downloadId, const FileHashes &h, qint64 size,
                             const SearchSource &src, const QString &userId, const QString &nodeId)
    : RangeSource(nullptr), m_plugin(plugin), m_downloadId(downloadId), m_h(h), m_size(size), m_src(src),
      m_userId(userId), m_nodeId(nodeId)
{
    m_sid = QRandomGenerator::global()->generate() | 1u;
    if (const auto *u = plugin->user(userId)) m_client = u->client;
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &OverlaySource::check);
    m_paceTimer.setSingleShot(true);   // [bw-1]
    m_paceTimer.setInterval(40);
    connect(&m_paceTimer, &QTimer::timeout, this, &OverlaySource::sendPaced);
}

namespace {
// [bw-1] LAN peers are exempt from the limits unless bandwidth/limitLan (direct links only: a
// relayed peer's address is the relay's)
bool lanPeer(OverlayPlugin *p, const QString &nodeId)
{
    RateLimiter *lim = p ? p->ctx().limiter : nullptr;
    if (!lim || !p->node() || p->node()->peerTransport(nodeId) == QLatin1String("relay")) return false;
    for (const RatsPeer &peer : p->node()->peers()) {
        if (peer.id.compare(nodeId, Qt::CaseInsensitive) != 0) continue;
        QString host = peer.address;
        if (host.startsWith(QLatin1Char('['))) host = host.mid(1, host.indexOf(QLatin1Char(']')) - 1);
        else if (host.count(QLatin1Char(':')) == 1) host = host.left(host.indexOf(QLatin1Char(':')));
        return lim->exempt(QHostAddress(host));
    }
    return false;
}
} // namespace

qint64 OverlaySource::preferredRequestSize() const
{
    // [bw-1] with a download limit: requests of about half a second of it, so the paced requests keep
    // the rate smooth (and the budget's grant covers the whole answer)
    const RateLimiter *lim = m_plugin ? m_plugin->ctx().limiter : nullptr;
    const int bps = lim ? lim->limitBps(RateLimiter::Down) : 0;
    return bps > 0 ? qBound(qint64(16 * 1024), qint64(bps / 2), qint64(1024 * 1024)) : qint64(1024 * 1024);
}

void OverlaySource::sendPaced()
{
    // [bw-1] the download limit for the overlay: librats reads the socket itself, so the rate is
    // kept by sending a "get" only once the shared budget granted its bytes (the answer's wire bytes
    // then use that grant instead of being charged again)
    if (!m_plugin || m_closed) { m_unsent.clear(); return; }
    RateLimiter *lim = m_plugin->ctx().limiter;
    const bool limited = lim && lim->limitBps(RateLimiter::Down) > 0 && !lanPeer(m_plugin, m_nodeId);
    while (!m_unsent.empty()) {
        const QJsonObject o = m_unsent.front();
        const quint32 rq = static_cast<quint32>(o.value(QStringLiteral("rq")).toDouble());
        auto it = std::find_if(m_reqs.begin(), m_reqs.end(), [rq](const Req &r) { return r.rq == rq; });
        if (it == m_reqs.end()) { m_unsent.pop_front(); continue; }   // cancelled meanwhile
        if (limited) {
            if (m_downCredit < it->len) m_downCredit += lim->acquire(RateLimiter::Down, it->len - m_downCredit);
            if (m_downCredit < it->len) { m_paceTimer.start(); return; }
            m_downCredit -= it->len;
        }
        it->lastMs = nowMs();
        m_unsent.pop_front();
        m_plugin->sendToNode(m_nodeId, o);
    }
}

OverlaySource::~OverlaySource()
{
    if (m_plugin) m_plugin->unregisterSource(m_sid);
}

QString OverlaySource::stateText() const
{
    // [ovl-2] through a relayed circuit (both ends firewalled, a third user carries the bytes, at most
    // librats' per-circuit cap of 10 Mbit/s): say so in the row
    const bool relayed = m_plugin && m_plugin->node() && m_plugin->node()->peerTransport(m_nodeId) == QLatin1String("relay");
    switch (m_state) {
    case Idle: return tr("Idle");
    case Connecting: return tr("Connecting...");
    case Queued: {
        const QString q = m_queuePos > 0 ? tr("Remotely Queued (%1)").arg(m_queuePos) : tr("Remotely Queued");
        return relayed ? tr("%1, relayed").arg(q) : q;
    }
    case Ready: return relayed ? tr("Waiting (relayed)") : tr("Waiting");
    case Busy: return relayed ? tr("Transferring (relayed)") : tr("Transferring");
    case Failed: return m_error.isEmpty() ? tr("Failed") : m_error;
    case Closed: return tr("Closed");
    }
    return QString();
}

QList<QPair<qint64, qint64>> OverlaySource::available() const
{
    // complete files only for now (partial-file sharing is a later protocol extension)
    if (m_state == Ready || m_state == Busy) return {qMakePair(qint64(0), m_size)};
    return {};
}

void OverlaySource::setState(State s)
{
    if (m_state == s) return;
    const bool availChange = (s == Ready || s == Busy) != (m_state == Ready || m_state == Busy);
    m_state = s;
    Q_EMIT stateChanged();
    if (availChange) Q_EMIT availabilityChanged();
}

void OverlaySource::sendOpen()
{
    if (!m_plugin || m_closed) return;
    m_lastOpenMs = nowMs();
    QJsonObject o;
    o[QStringLiteral("t")] = QStringLiteral("get_open");
    o[QStringLiteral("v")] = kWireVersion;
    o[QStringLiteral("sid")] = static_cast<double>(m_sid);
    o[QStringLiteral("h")] = hashesToJson(m_h);
    o[QStringLiteral("size")] = static_cast<double>(m_size);
    const auto *u = m_plugin->user(m_userId);
    const QStringList addrs = u ? u->addrs : QStringList();
    QPointer<OverlaySource> self(this);
    m_plugin->sendToNode(m_nodeId, o, addrs, [self](bool ok) {
        if (self && !ok && self->m_state == Connecting && self->m_plugin)
            self->fail(self->m_plugin->unreachableText(self->m_userId));
    });
}

void OverlaySource::open()
{
    if (m_closed || !m_plugin) return;
    if (m_nodeId.isEmpty()) {
        fail(tr("User is not online"));
        return;
    }
    m_error.clear();
    setState(Connecting);
    m_timer.start();
    sendOpen();
}

void OverlaySource::request(qint64 offset, qint64 length)
{
    if (!m_plugin || (m_state != Ready && m_state != Busy)) {
        QTimer::singleShot(0, this, [this, offset, length] { Q_EMIT requestFinished(offset, length, false, tr("Not ready")); });
        return;
    }
    if (offset < 0 || length <= 0 || offset + length > m_size || length > kMaxRequest) {
        QTimer::singleShot(0, this, [this, offset, length] { Q_EMIT requestFinished(offset, length, false, tr("Bad range")); });
        return;
    }
    Req r{++m_rqSeq, offset, length, 0, nowMs()};
    m_reqs.push_back(r);
    QJsonObject o;
    o[QStringLiteral("t")] = QStringLiteral("get");
    o[QStringLiteral("v")] = kWireVersion;
    o[QStringLiteral("sid")] = static_cast<double>(m_sid);
    o[QStringLiteral("rq")] = static_cast<double>(r.rq);
    o[QStringLiteral("off")] = static_cast<double>(offset);
    o[QStringLiteral("len")] = static_cast<double>(length);
    m_unsent.push_back(o);   // [bw-1] paced by the download limit
    sendPaced();
    if (m_state != Busy) {
        // Never re-enter the caller: a stateChanged handler that issues more requests would run
        // inside this request() call. Ready → Busy is announced on the next event-loop turn.
        m_state = Busy;
        QTimer::singleShot(0, this, [this] { if (m_state == Busy) Q_EMIT stateChanged(); });
    }
}

void OverlaySource::cancelRequests()
{
    if (m_reqs.empty()) return;
    m_reqs.clear();
    if (m_plugin) {
        QJsonObject o;
        o[QStringLiteral("t")] = QStringLiteral("get_cancel");
        o[QStringLiteral("v")] = kWireVersion;
        o[QStringLiteral("sid")] = static_cast<double>(m_sid);
        m_plugin->sendToNode(m_nodeId, o);
    }
    if (m_state == Busy) setState(Ready);
}

void OverlaySource::close()
{
    if (m_closed) return;
    m_closed = true;
    m_timer.stop();
    m_reqs.clear();
    if (m_plugin) {
        QJsonObject o;
        o[QStringLiteral("t")] = QStringLiteral("get_close");
        o[QStringLiteral("v")] = kWireVersion;
        o[QStringLiteral("sid")] = static_cast<double>(m_sid);
        if (m_plugin->m_peers.contains(m_nodeId)) m_plugin->sendToNode(m_nodeId, o);
        m_plugin->unregisterSource(m_sid);
    }
    setState(Closed);
}

void OverlaySource::onSlot(const QString &st, int pos, const QString &reason, const QStringList &hashSets)
{
    if (m_closed) return;
    // [engine-3] the uploader offers verification data: fetch the part hash set once (also while queued)
    if (!m_hsAsked && m_plugin && m_size >= kEd2kPart && hashSets.contains(QStringLiteral("ed2k-parts"))
        && (st == QLatin1String("ready") || st == QLatin1String("queued"))) {
        m_hsAsked = true;
        QJsonObject o;
        o[QStringLiteral("t")] = QStringLiteral("hs_req");
        o[QStringLiteral("v")] = kWireVersion;
        o[QStringLiteral("sid")] = static_cast<double>(m_sid);
        o[QStringLiteral("kind")] = QStringLiteral("ed2k-parts");
        m_plugin->sendToNode(m_nodeId, o);
    }
    if (st == QLatin1String("ready")) {
        m_queuePos = 0;
        setState(m_reqs.empty() ? Ready : Busy);
    } else if (st == QLatin1String("queued")) {
        const bool changed = pos != m_queuePos;
        m_queuePos = pos;
        if (m_state == Queued && changed) Q_EMIT stateChanged();
        setState(Queued);
    } else if (st == QLatin1String("closed")) {
        // the uploader dropped our slot (idle / cancelled): fail what is pending and queue again
        const auto reqs = m_reqs;
        m_reqs.clear();
        for (const Req &r : reqs) Q_EMIT requestFinished(r.off, r.len, false, tr("Upload slot closed"));
        if (reason == QLatin1String("cancelled")) {
            fail(tr("Upload cancelled by user"));
            return;
        }
        setState(Connecting);
        sendOpen();
    } else {
        fail(st == QLatin1String("missing") ? tr("File not shared") :
             reason == QLatin1String("busy") ? tr("No upload slots") :
             reason == QLatin1String("hotlist") ? tr("Denied: uploads only to users on their Hotlist") : tr("Denied by user"));
    }
}

void OverlaySource::onHashSet(const QJsonObject &o)
{
    if (m_closed || !m_hsAsked) return;
    const QString kind = jStr(o, "kind", 32);
    if (kind != QLatin1String("ed2k-parts") || !jBool(o, "ok", false)) return;
    if (jInt(o, "unit", 0) != kEd2kPart) return;
    const qint64 n = jInt(o, "n", 0);
    const qint64 full = m_size / kEd2kPart + 1, data = (m_size + kEd2kPart - 1) / kEd2kPart;
    if (n <= 0 || n > kMaxHashSetParts || (n != full && n != data)) return;
    const QString hex = jStr(o, "hashes", int(n * 32));
    if (hex.size() != n * 32 || !isHex(hex)) return;
    const QByteArray raw = QByteArray::fromHex(hex.toLatin1());
    QList<QByteArray> parts;
    parts.reserve(int(n));
    for (qint64 i = 0; i < n; ++i) parts << raw.mid(int(i * 16), 16);
    m_hsAsked = true;
    Q_EMIT hashSetReceived(kind, kEd2kPart, parts);
}

void OverlaySource::onData(quint32 rq, qint64 off, const char *bytes, int len)
{
    if (m_reqs.empty() || m_closed) return;
    Req &r = m_reqs.front();
    if (r.rq != rq) return;   // late data of a cancelled request
    if (off != r.off + r.got || r.got + len > r.len) {
        const Req bad = r;
        m_reqs.pop_front();
        Q_EMIT requestFinished(bad.off, bad.len, false, tr("Out-of-order data"));
        return;
    }
    r.got += len;
    r.lastMs = nowMs();
    if (m_plugin && m_plugin->ctx().limiter) {   // [glue-3] wire counted by RatsNode
        if (lanPeer(m_plugin, m_nodeId)) m_plugin->ctx().limiter->accountExempt(RateLimiter::Down, Net::Overlay, 0, len);   // [bw-1] gives back the charge
        else m_plugin->ctx().limiter->accountTraffic(RateLimiter::Down, Net::Overlay, 0, len);
    }
    Q_EMIT data(off, QByteArray(bytes, len));
    if (m_reqs.empty() || m_reqs.front().rq != rq) return;   // a slot connected to data() cancelled us
    if (m_reqs.front().got == m_reqs.front().len) {
        const Req done = m_reqs.front();
        m_reqs.pop_front();
        if (m_reqs.empty() && m_state == Busy) setState(Ready);
        Q_EMIT requestFinished(done.off, done.len, true, QString());
    }
}

void OverlaySource::onGot(quint32 rq, bool ok, const QString &err)
{
    if (ok) return;
    for (auto it = m_reqs.begin(); it != m_reqs.end(); ++it) {
        if (it->rq != rq) continue;
        const Req r = *it;
        m_reqs.erase(it);
        if (m_reqs.empty() && m_state == Busy) setState(Ready);
        Q_EMIT requestFinished(r.off, r.len, false, err);
        if (err == QLatin1String("no slot")) {
            setState(Connecting);
            sendOpen();
        }
        return;
    }
}

void OverlaySource::peerGone()
{
    if (m_closed || m_state == Failed) return;
    if (m_state == Connecting) return;   // the dial in progress may still succeed
    fail(tr("Disconnected"));
}

void OverlaySource::linkMoved()
{
    if (m_closed || m_state == Failed) return;
    if (m_state == Connecting || m_state == Queued) {
        sendOpen();   // the slot answer (or our open) may have been on the old link
        return;
    }
    // outstanding requests may never be answered: give them back to the engine, which asks again
    const auto reqs = m_reqs;
    if (reqs.empty()) return;
    cancelRequests();
    for (const Req &r : reqs) Q_EMIT requestFinished(r.off, r.len, false, tr("Link changed"));
}

void OverlaySource::fail(const QString &why)
{
    if (m_closed) return;
    const auto reqs = m_reqs;
    m_reqs.clear();
    m_error = why;
    m_timer.stop();
    setState(Failed);
    for (const Req &r : reqs) Q_EMIT requestFinished(r.off, r.len, false, why);
}

void OverlaySource::check()
{
    if (!m_plugin || m_closed) return;
    const qint64 now = nowMs();
    // [bw-1] a request still waiting for the download budget has not been sent: no timeout yet
    const bool frontWaiting = !m_reqs.empty() && !m_unsent.empty() &&
                              static_cast<quint32>(m_unsent.front().value(QStringLiteral("rq")).toDouble()) == m_reqs.front().rq;
    if (!m_reqs.empty() && !frontWaiting && now - m_reqs.front().lastMs > m_plugin->timings().requestTimeoutMs) {
        fail(tr("Timed out"));
        return;
    }
    // keep our queue place alive / retry a lost open
    if ((m_state == Queued && now - m_lastOpenMs > 60000) || (m_state == Connecting && now - m_lastOpenMs > 20000))
        sendOpen();
}

// ------------------------------------------------------------------------------- plugin glue
RangeSource *OverlayPlugin::openSource(const QString &downloadId, const FileHashes &h, qint64 size,
                                       const SearchSource &src)
{
    if (src.net != Net::Overlay || size <= 0 || h.isEmpty()) return nullptr;
    const QString uid = src.user.id.toLower();
    if (!isHex(uid, 64) || uid == m_userId) return nullptr;
    QString node = nodeForUser(uid);
    if (node.isEmpty()) node = src.extra.value(QStringLiteral("node")).toString().toLower();
    if (!isHex(node, 64)) return nullptr;
    auto *s = new OverlaySource(this, downloadId, h, size, src, uid, node);
    while (m_sources.contains(s->sid())) {   // vanishingly unlikely
        delete s;
        s = new OverlaySource(this, downloadId, h, size, src, uid, node);
    }
    m_sources.insert(s->sid(), s);
    return s;
}

void OverlayPlugin::unregisterSource(quint32 sid)
{
    m_sources.remove(sid);
}

void OverlayPlugin::onSlot(const QString &nodeId, const QJsonObject &o)
{
    const quint32 sid = static_cast<quint32>(jInt(o, "sid", 0));
    auto src = m_sources.value(sid);
    if (!src || src->nodeId() != nodeId) return;
    QStringList hs;
    for (const QJsonValue &v : jArr(o, "hs", 8))
        if (v.isString() && v.toString().size() <= 32) hs << v.toString();
    src->onSlot(jStr(o, "st", 16), static_cast<int>(qBound(qint64(0), jInt(o, "pos", 0), qint64(100000))),
                jStr(o, "reason", 32), hs);
}

void OverlayPlugin::onHashSetAnswer(const QString &nodeId, const QJsonObject &o)
{
    const quint32 sid = static_cast<quint32>(jInt(o, "sid", 0));
    auto src = m_sources.value(sid);
    if (!src || src->nodeId() != nodeId) return;
    src->onHashSet(o);
}

void OverlayPlugin::onGot(const QString &nodeId, const QJsonObject &o)
{
    const quint32 sid = static_cast<quint32>(jInt(o, "sid", 0));
    auto src = m_sources.value(sid);
    if (!src || src->nodeId() != nodeId) return;
    src->onGot(static_cast<quint32>(jInt(o, "rq", 0)), jBool(o, "ok", false), jStr(o, "err", 64));
}

void OverlayPlugin::onDataFrame(const QString &nodeId, const QByteArray &frame)
{
    quint32 sid = 0, rq = 0;
    qint64 off = 0;
    int pos = 0;
    if (!parseDataFrame(frame, &sid, &rq, &off, &pos)) return;
    auto src = m_sources.value(sid);
    if (!src || src->nodeId() != nodeId) return;
    src->onData(rq, off, frame.constData() + pos, frame.size() - pos);
}

// ================================================================================== serve side
int OverlayPlugin::totalSlots() const
{
    return qBound(0, setting(QString(keys::transfers::MaxUploads), 8).toInt(), 200);
}

int OverlayPlugin::freeSlots() const
{
    int active = 0;
    for (const UploadSession *s : m_uploads) active += s->active ? 1 : 0;
    return qMax(0, totalSlots() - active);
}

namespace {
void sendSlot(OverlayPlugin *p, const QString &node, quint32 sid, const QString &st, int pos = 0,
              const QString &reason = QString(), const QStringList &hashSets = {})
{
    QJsonObject o;
    o[QStringLiteral("t")] = QStringLiteral("slot");
    o[QStringLiteral("v")] = kWireVersion;
    o[QStringLiteral("sid")] = static_cast<double>(sid);
    o[QStringLiteral("st")] = st;
    if (pos > 0) o[QStringLiteral("pos")] = pos;
    if (!reason.isEmpty()) o[QStringLiteral("reason")] = reason;
    if (!hashSets.isEmpty()) o[QStringLiteral("hs")] = QJsonArray::fromStringList(hashSets);   // [engine-3]
    p->sendToNode(node, o);
}
void sendSlot(OverlayPlugin *p, const UploadSession *s, const QString &st, int pos = 0)
{
    sendSlot(p, s->nodeId, s->sid, st, pos, QString(), s->hashSets);
}
void sendGot(OverlayPlugin *p, const QString &node, quint32 sid, quint32 rq, bool ok, const QString &err)
{
    QJsonObject o;
    o[QStringLiteral("t")] = QStringLiteral("got");
    o[QStringLiteral("v")] = kWireVersion;
    o[QStringLiteral("sid")] = static_cast<double>(sid);
    o[QStringLiteral("rq")] = static_cast<double>(rq);
    o[QStringLiteral("ok")] = ok;
    if (!err.isEmpty()) o[QStringLiteral("err")] = err;
    p->sendToNode(node, o);
}
} // namespace

void OverlayPlugin::onGetOpen(const QString &nodeId, const QJsonObject &o)
{
    const QString uid = m_nodeUser.value(nodeId);
    const quint32 sid = static_cast<quint32>(jInt(o, "sid", 0));
    if (sid == 0) return;
    const QString key = nodeId + QLatin1Char('/') + QString::number(sid);
    if (UploadSession *s = m_uploads.value(key)) {   // re-open / keep-alive
        s->lastActivityMs = nowMs();
        if (s->active) sendSlot(this, s, QStringLiteral("ready"));
        else sendSlot(this, s, QStringLiteral("queued"), m_uploadQueue.indexOf(s) + 1);
        return;
    }
    const FileHashes h = hashesFromJson(jObj(o, "h"));
    const qint64 size = jInt(o, "size", 0);
    SharedFile file;
    bool found = false;
    for (const SharedFile &f : m_shared)
        if (f.hashes.matches(h) && (size <= 0 || f.size == size)) { file = f; found = true; break; }
    if (!found && !m_haveSharedList && m_ctx.library && m_ctx.library->findByHash(h, &file))
        found = (size <= 0 || file.size == size) && !file.hashes.conflicts(h);
    if (!found || !m_ctx.library) return sendSlot(this, nodeId, sid, QStringLiteral("missing"));
    const bool hot = m_hotlist.contains(uid);
    if (setting(QString(keys::transfers::UploadsToHotlistOnly), false).toBool() && !hot)
        return sendSlot(this, nodeId, sid, QStringLiteral("denied"), 0, QStringLiteral("hotlist"));

    int active = 0, userActive = 0, userQueued = 0;
    for (const UploadSession *s : m_uploads) {
        active += s->active;
        if (s->userId == uid) (s->active ? userActive : userQueued)++;
    }
    const int maxUp = totalSlots();
    const int perUser = qBound(1, setting(QString(keys::transfers::MaxUploadsPerUser), 2).toInt(), 20);
    const qint64 bypass = setting(QString(keys::queue::SmallFileBypassKB), 128).toLongLong() * 1024;
    auto *s = new UploadSession;
    s->key = key;
    s->uploadId = QStringLiteral("overlay-up-%1").arg(randomId());
    s->nodeId = nodeId;
    s->userId = uid;
    s->userName = userRefFor(uid).name;
    s->sid = sid;
    s->hashes = file.hashes;
    s->size = file.size;
    s->name = file.name;
    s->hotlist = hot;
    if (file.size >= kEd2kPart && !m_ctx.library->ed2kPartHashes(file.hashes).isEmpty())
        s->hashSets << QStringLiteral("ed2k-parts");   // [engine-3] offered in every slot answer
    s->queuedMs = s->lastActivityMs = nowMs();
    s->serial = ++m_uploadSerial;
    const bool small = bypass > 0 && file.size <= bypass;
    if (maxUp > 0 && ((active < maxUp && userActive < perUser) || (small && userActive < perUser))) {
        s->active = true;
        s->startedMs = nowMs();
        m_uploads.insert(key, s);
        sendSlot(this, s, QStringLiteral("ready"));
        reportUpload(s, true);
        return;
    }
    const bool queueOn = setting(QString(keys::queue::Enabled), true).toBool();
    const int maxQueue = setting(QString(keys::queue::MaxLength), 200).toInt();
    const int maxPerUser = setting(QString(keys::queue::MaxPerUser), 10).toInt();
    if (!queueOn || maxUp == 0 || m_uploadQueue.size() >= maxQueue || userQueued >= maxPerUser) {
        delete s;
        return sendSlot(this, nodeId, sid, QStringLiteral("denied"), 0, QStringLiteral("busy"));
    }
    m_uploads.insert(key, s);
    int at = m_uploadQueue.size();
    if (hot && setting(QString(keys::queue::HotlistPriority), true).toBool()) {
        at = 0;
        while (at < m_uploadQueue.size() && m_uploadQueue[at]->hotlist) ++at;
    }
    m_uploadQueue.insert(at, s);
    sendQueuePositions();
    reportUpload(s, true);
}

void OverlayPlugin::sendQueuePositions()
{
    const bool tell = setting(QString(keys::queue::SendPosition), true).toBool();
    for (int i = 0; i < m_uploadQueue.size(); ++i) {
        UploadSession *s = m_uploadQueue[i];
        if (s->lastQueuePos == i + 1) continue;
        s->lastQueuePos = i + 1;
        sendSlot(this, s, QStringLiteral("queued"), tell ? i + 1 : 0);
        reportUpload(s, true);
    }
}

void OverlayPlugin::promoteQueue()
{
    const int maxUp = totalSlots();
    const int perUser = qBound(1, setting(QString(keys::transfers::MaxUploadsPerUser), 2).toInt(), 20);
    int active = 0;
    QHash<QString, int> userActive;
    for (const UploadSession *s : m_uploads)
        if (s->active) { ++active; ++userActive[s->userId]; }
    bool changed = false;
    for (int i = 0; i < m_uploadQueue.size() && active < maxUp;) {
        UploadSession *s = m_uploadQueue[i];
        if (userActive.value(s->userId) >= perUser) { ++i; continue; }
        m_uploadQueue.removeAt(i);
        s->active = true;
        s->startedMs = s->lastActivityMs = nowMs();
        ++active;
        ++userActive[s->userId];
        sendSlot(this, s, QStringLiteral("ready"));
        reportUpload(s, true);
        changed = true;
    }
    if (changed) sendQueuePositions();
}

void OverlayPlugin::onGet(const QString &nodeId, const QJsonObject &o)
{
    const quint32 sid = static_cast<quint32>(jInt(o, "sid", 0));
    const quint32 rq = static_cast<quint32>(jInt(o, "rq", 0));
    UploadSession *s = m_uploads.value(nodeId + QLatin1Char('/') + QString::number(sid));
    if (!s || !s->active) return sendGot(this, nodeId, sid, rq, false, QStringLiteral("no slot"));
    const qint64 off = jInt(o, "off", -1), len = jInt(o, "len", 0);
    if (off < 0 || len <= 0 || len > kMaxRequest || off + len > s->size || s->reqs.size() >= 8)
        return sendGot(this, nodeId, sid, rq, false, QStringLiteral("bad range"));
    s->reqs.push_back(UploadSession::Req{rq, off, len, 0});
    s->lastActivityMs = nowMs();
    pumpUpload(s);
}

// [engine-3] hs_req {sid, kind} → hs {sid, kind, ok, unit, n, hashes:<hex>} for an open/queued session
void OverlayPlugin::onHashSetRequest(const QString &nodeId, const QJsonObject &o)
{
    const quint32 sid = static_cast<quint32>(jInt(o, "sid", 0));
    const QString kind = jStr(o, "kind", 32);
    UploadSession *s = m_uploads.value(nodeId + QLatin1Char('/') + QString::number(sid));
    QJsonObject a;
    a[QStringLiteral("t")] = QStringLiteral("hs");
    a[QStringLiteral("v")] = kWireVersion;
    a[QStringLiteral("sid")] = static_cast<double>(sid);
    a[QStringLiteral("kind")] = kind;
    QList<QByteArray> parts;
    if (s && kind == QLatin1String("ed2k-parts") && s->hashSets.contains(kind) && s->hashSetsServed < 3 && m_ctx.library)
        parts = m_ctx.library->ed2kPartHashes(s->hashes);
    if (s) ++s->hashSetsServed;
    bool ok = !parts.isEmpty() && parts.size() <= kMaxHashSetParts;
    QByteArray hex;
    if (ok) {
        hex.reserve(parts.size() * 32);
        for (const QByteArray &p : std::as_const(parts)) {
            if (p.size() != 16) { ok = false; break; }
            hex += p.toHex();
        }
    }
    a[QStringLiteral("ok")] = ok;
    if (ok) {
        a[QStringLiteral("unit")] = static_cast<double>(kEd2kPart);
        a[QStringLiteral("n")] = static_cast<double>(parts.size());
        a[QStringLiteral("hashes")] = QString::fromLatin1(hex);
    }
    sendToNode(nodeId, a);
}

void OverlayPlugin::onGetClose(const QString &nodeId, const QJsonObject &o, bool cancelOnly)
{
    const quint32 sid = static_cast<quint32>(jInt(o, "sid", 0));
    UploadSession *s = m_uploads.value(nodeId + QLatin1Char('/') + QString::number(sid));
    if (!s) return;
    if (cancelOnly) {
        s->reqs.clear();
        s->buf.clear();
        s->bufPos = 0;
        ++s->serial;   // drop a read in flight
        s->reading = false;
        return;
    }
    closeUpload(s, QStringLiteral("closed by peer"), false);
}

void OverlayPlugin::pumpUpload(UploadSession *s)
{
    if (!s || !s->active || !m_running) return;
    const bool exempt = lanPeer(this, s->nodeId);   // [bw-1] LAN peer while bandwidth/limitLan is off
    for (int guard = 0; guard < 64; ++guard) {
        if (s->reqs.empty()) return;
        UploadSession::Req &r = s->reqs.front();
        if (s->bufPos < s->buf.size()) {
            if (!isWritable(s->nodeId)) return;   // resumed by peerWritable
            int n = qMin(kMaxFramePayload, s->buf.size() - s->bufPos);
            if (m_ctx.limiter && !exempt) {
                n = static_cast<int>(m_ctx.limiter->acquire(RateLimiter::Up, n));
                if (n <= 0) {
                    if (!s->throttled) {
                        s->throttled = true;
                        const QString key = s->key;
                        const quint64 serial = s->serial;
                        QTimer::singleShot(25, this, [this, key, serial] {
                            UploadSession *u = m_uploads.value(key);
                            if (!u || u->serial != serial) return;
                            u->throttled = false;
                            pumpUpload(u);
                        });
                    }
                    return;
                }
            }
            const qint64 off = r.off + r.sent;
            sendDataFrame(s->nodeId, makeDataFrame(s->sid, r.rq, off, s->buf.constData() + s->bufPos, n));
            if (m_ctx.limiter && exempt) m_ctx.limiter->accountExempt(RateLimiter::Up, Net::Overlay, 0, n);   // [bw-1] the frame's wire is not charged
            else if (m_ctx.limiter) m_ctx.limiter->accountTraffic(RateLimiter::Up, Net::Overlay, 0, n);   // [glue-3] wire counted by RatsNode
            s->bufPos += n;
            r.sent += n;
            s->bytesSent += n;
            s->rateWindowBytes += n;
            s->lastActivityMs = nowMs();
            if (r.sent >= r.len) {
                s->reqs.pop_front();
                s->buf.clear();
                s->bufPos = 0;
            }
            continue;
        }
        if (s->reading) return;
        // read the next chunk of the current request off the main thread
        const qint64 off = r.off + r.sent;
        const qint64 n = qMin(kReadChunk, r.len - r.sent);
        s->reading = true;
        s->buf.clear();
        s->bufPos = 0;
        LibraryAccess *lib = m_ctx.library;
        const FileHashes h = s->hashes;
        const QString key = s->key;
        const quint64 serial = s->serial;
        const quint32 rq = r.rq;
        OverlayPlugin *self = this;   // the plugin waits for m_readPool before it dies
        m_readPool->start([self, lib, h, off, n, key, serial, rq] {
            QByteArray d = lib ? lib->read(h, off, n) : QByteArray();
            OverlayPlugin *p = self;
            QMetaObject::invokeMethod(p, [p, d, key, serial, rq, n] {
                UploadSession *u = p->m_uploads.value(key);
                if (!u || u->serial != serial) return;
                u->reading = false;
                if (u->reqs.empty() || u->reqs.front().rq != rq) return p->pumpUpload(u);
                if (d.size() != n) {
                    const quint32 bad = u->reqs.front().rq;
                    u->reqs.pop_front();
                    sendGot(p, u->nodeId, u->sid, bad, false, QStringLiteral("read error"));
                    return p->pumpUpload(u);
                }
                u->buf = d;
                u->bufPos = 0;
#ifdef MX_TEST_HOOKS
                // [engine-3] test hook overlay/testCorruptUploads: flip the served bytes in [start, end)
                if (p->m_corruptEnd > p->m_corruptStart) {
                    const qint64 off0 = u->reqs.front().off + u->reqs.front().sent;
                    const qint64 a = qMax(off0, p->m_corruptStart), b = qMin(off0 + qint64(u->buf.size()), p->m_corruptEnd);
                    for (qint64 i = a; i < b; ++i) u->buf[int(i - off0)] = char(u->buf[int(i - off0)] ^ 0x5a);
                    if (b > a) p->m_corruptedBytes += b - a;
                }
#endif
                p->pumpUpload(u);
            }, Qt::QueuedConnection);
        });
        return;
    }
    // more work left: continue on the next event-loop turn (keeps the GUI responsive)
    const QString key = s->key;
    const quint64 serial = s->serial;
    QTimer::singleShot(0, this, [this, key, serial] {
        UploadSession *u = m_uploads.value(key);
        if (u && u->serial == serial) pumpUpload(u);
    });
}

void OverlayPlugin::reportUpload(UploadSession *s, bool force)
{
    const qint64 now = nowMs();
    if (!force && now - s->lastReportMs < 1000) return;
    s->lastReportMs = now;
    TransferInfo t;
    t.id = s->uploadId;
    t.isUpload = true;
    t.name = s->name;
    t.size = s->size;
    t.done = qMin(s->bytesSent, s->size);
    t.rateUpBps = s->rateBps;
    t.hashes = s->hashes;
    t.networks = {Net::Overlay};
    t.peer = userRefFor(s->userId);
    t.added = QDateTime::fromMSecsSinceEpoch(s->queuedMs);
    const int pos = m_uploadQueue.indexOf(s) + 1;
    t.state = s->active ? TransferState::Transferring : TransferState::Queued;
    t.statusText = s->active ? (s->reqs.empty() ? tr("Waiting") : tr("Uploading")) : tr("Queued (%1)").arg(pos);
    TransferSource ts;
    ts.net = Net::Overlay;
    ts.user = t.peer;
    ts.statusText = t.statusText;
    ts.bytes = s->bytesSent;
    ts.rateBps = s->rateBps;
    if (const UserInfo *u = user(s->userId)) {
        ts.connection = u->conn;
        ts.client = u->client;
    }
    t.sources = {ts};
    t.sourcesTotal = 1;
    t.sourcesActive = s->active ? 1 : 0;
    Q_EMIT uploadChanged(t);
}

void OverlayPlugin::closeUpload(UploadSession *s, const QString &reason, bool notifyPeer)
{
    if (!s) return;
    m_uploads.remove(s->key);
    m_uploadQueue.removeAll(s);
    if (notifyPeer) sendSlot(this, s->nodeId, s->sid, QStringLiteral("closed"), 0, reason);
    qCInfo(lcOverlay) << "upload" << s->name << "to" << s->userName << "closed:" << reason << s->bytesSent << "bytes";
    const QString id = s->uploadId;
    delete s;
    Q_EMIT uploadFinished(id);
    if (m_running) {
        promoteQueue();
        sendQueuePositions();
    }
}

void OverlayPlugin::cancelUpload(const QString &uploadId)
{
    for (UploadSession *s : m_uploads)
        if (s->uploadId == uploadId) return closeUpload(s, QStringLiteral("cancelled"), true);
}

// [glue-2] "Start Transfer" on a queued upload: give it a slot now (beyond the slot limit).
bool OverlayPlugin::startUploadNow(const QString &uploadId)
{
    for (UploadSession *s : std::as_const(m_uploadQueue)) {
        if (s->uploadId != uploadId) continue;
        m_uploadQueue.removeAll(s);
        s->active = true;
        s->startedMs = s->lastActivityMs = nowMs();
        sendSlot(this, s, QStringLiteral("ready"));
        reportUpload(s, true);
        sendQueuePositions();
        return true;
    }
    for (UploadSession *s : std::as_const(m_uploads))
        if (s->uploadId == uploadId) return s->active;   // already running
    return false;
}

// [glue-2] priority of a queued upload = its place in the queue (high → front, low → back).
bool OverlayPlugin::setUploadPriority(const QString &uploadId, int priority)
{
    for (UploadSession *s : std::as_const(m_uploadQueue)) {
        if (s->uploadId != uploadId) continue;
        if (priority == 0) return true;
        m_uploadQueue.removeAll(s);
        if (priority > 0) m_uploadQueue.prepend(s);
        else m_uploadQueue.append(s);
        sendQueuePositions();
        for (UploadSession *q : std::as_const(m_uploadQueue)) reportUpload(q, true);
        return true;
    }
    return false;
}

void OverlayPlugin::dropPeerUploads(const QString &nodeId)
{
    const QList<UploadSession *> all = m_uploads.values();
    for (UploadSession *s : all)
        if (s->nodeId == nodeId) closeUpload(s, QStringLiteral("disconnected"), false);
}

void OverlayPlugin::uploadsTick()
{
    const qint64 now = nowMs();
    const qint64 queueTimeout = qMax<qint64>(1, setting(QString(keys::queue::TimeoutMin), 60).toLongLong()) * 60000;
    const QList<UploadSession *> all = m_uploads.values();
    for (UploadSession *s : all) {
        if (s->rateWindowStart == 0) s->rateWindowStart = now;
        if (now - s->rateWindowStart >= 1000) {
            s->rateBps = static_cast<int>(s->rateWindowBytes * 1000 / qMax<qint64>(1, now - s->rateWindowStart));
            s->rateWindowBytes = 0;
            s->rateWindowStart = now;
        }
        if (s->active && s->reqs.empty() && !s->reading && now - s->lastActivityMs > m_t.uploadIdleMs) {
            closeUpload(s, QStringLiteral("idle"), true);
            continue;
        }
        if (!s->active && now - s->lastActivityMs > qMin<qint64>(queueTimeout, 5 * 60000)) {
            // queued clients re-open every 60 s; silence means they are gone
            closeUpload(s, QStringLiteral("queue timeout"), true);
            continue;
        }
        reportUpload(s, false);
    }
}

} // namespace mx::ovl
