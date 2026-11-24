// SPDX-License-Identifier: GPL-3.0-or-later
// Net::Overlay: direct transfers: OverlaySource (download side) and upload slots / queue.
#include "overlay_transfer.h"

#include "overlay_plugin_impl.h"

#include <QtCore/QCryptographicHash>
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

// [up-1] partial-file maps: [[start, end], ...], sorted, not overlapping, inside the file
QJsonArray haveToJson(const QList<QPair<qint64, qint64>> &have)
{
    QJsonArray a;
    for (const auto &r : have) {
        if (a.size() >= kMaxHaveRanges) break;
        a.append(QJsonArray{static_cast<double>(r.first), static_cast<double>(r.second)});
    }
    return a;
}

bool haveFromJson(const QJsonArray &a, qint64 size, QList<QPair<qint64, qint64>> *out)
{
    out->clear();
    if (a.size() > kMaxHaveRanges) return false;
    qint64 last = 0;
    for (const QJsonValue &v : a) {
        if (!v.isArray() || v.toArray().size() != 2) return false;
        const double ds = v.toArray().at(0).toDouble(-1), de = v.toArray().at(1).toDouble(-1);
        const qint64 b = static_cast<qint64>(ds), e = static_cast<qint64>(de);
        if (double(b) != ds || double(e) != de || b < last || e <= b || e > size) return false;
        if (!out->isEmpty() && out->last().second == b) out->last().second = e;
        else out->append(qMakePair(b, e));
        last = e;
    }
    return true;
}

bool haveContains(const QList<QPair<qint64, qint64>> &have, qint64 off, qint64 len)
{
    for (const auto &r : have)
        if (r.first <= off && off + len <= r.second) return true;
    return false;
}

namespace {
void haveRemove(QList<QPair<qint64, qint64>> *have, qint64 a, qint64 b)
{
    QList<QPair<qint64, qint64>> out;
    for (const auto &r : std::as_const(*have)) {
        if (r.second <= a || r.first >= b) { out << r; continue; }
        if (r.first < a) out << qMakePair(r.first, a);
        if (r.second > b) out << qMakePair(b, r.second);
    }
    *have = out;
}
} // namespace

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
    // [bw-1] with a download limit: requests of a part of it, so the paced requests keep the rate
    // smooth (and the budget's grant covers the whole answer). [bw-2] an eighth of a second (was half
    // a second): the grant is collected before the request goes out and the answer comes at once, so
    // a request is a step of that size in the rate
    const RateLimiter *lim = m_plugin ? m_plugin->ctx().limiter : nullptr;
    const int bps = lim ? lim->limitBps(RateLimiter::Down) : 0;
    return bps > 0 ? qBound(qint64(16 * 1024), qint64(bps / 8), qint64(1024 * 1024)) : qint64(1024 * 1024);
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
            if (m_downCredit < it->len) m_downCredit += lim->acquireFor(RateLimiter::Down, Net::Overlay, it->len - m_downCredit);
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
    // [up-1] a source still downloading the file offers what its slot / "have" said it holds
    if (m_state == Ready || m_state == Busy) return m_partial ? m_have : QList<QPair<qint64, qint64>>{qMakePair(qint64(0), m_size)};
    return {};
}

bool OverlaySource::readHave(const QJsonObject &o)
{
    if (!o.contains(QStringLiteral("have"))) return false;
    QList<QPair<qint64, qint64>> have;
    if (!haveFromJson(o.value(QStringLiteral("have")).toArray(), m_size, &have)) have.clear();   // malformed: nothing
    const bool changed = !m_partial || have != m_have;
    m_partial = true;
    m_have = have;
    if (changed && (m_state == Ready || m_state == Busy)) Q_EMIT availabilityChanged();
    return true;
}

void OverlaySource::onHave(const QJsonObject &o)
{
    if (m_closed) return;
    if (jBool(o, "full", false)) {   // the uploader's copy is complete now
        const bool changed = m_partial;
        m_partial = false;
        m_have.clear();
        if (changed && (m_state == Ready || m_state == Busy)) Q_EMIT availabilityChanged();
        return;
    }
    readHave(o);
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
    o[QStringLiteral("part")] = true;   // [up-1] we understand "have" maps (partial files)
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
    if (m_partial && !haveContains(m_have, offset, length)) {   // [up-1] never ask for what it does not hold
        QTimer::singleShot(0, this, [this, offset, length] { Q_EMIT requestFinished(offset, length, false, rangeNotAvailableError()); });
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
        // [up-1] why: the uploader's row says so ("Finished", "Cancelled by remote", "Stopped by the
        // other side: ..."); older uploaders ignore the fields
        o[QStringLiteral("reason")] = m_closeCode.isEmpty() ? QStringLiteral("dropped") : m_closeCode;
        if (!m_closeText.isEmpty()) o[QStringLiteral("why")] = m_closeText.left(160);
        if (m_plugin->m_peers.contains(m_nodeId)) {
            m_plugin->sendUrgent(m_nodeId, o);   // [up-1] does not wait behind data we send them
            m_plugin->sendToNode(m_nodeId, o);   // the reliable copy (a second one finds no session)
        }
        m_plugin->unregisterSource(m_sid);
    }
    setState(Closed);
}

void OverlaySource::setCloseReason(const QString &code, const QString &text)
{
    m_closeCode = code.left(16);
    m_closeText = text;
}

void OverlaySource::onSlot(const QString &st, int pos, const QString &reason, const QStringList &hashSets, const QJsonObject &msg)
{
    if (m_closed) return;
    // [up-1] a slot of a file the uploader is still downloading carries its map; none = complete
    if (st == QLatin1String("ready") || st == QLatin1String("queued")) {
        if (!readHave(msg) && m_partial) {
            m_partial = false;
            m_have.clear();
            if (m_state == Ready || m_state == Busy) Q_EMIT availabilityChanged();
        }
    }
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
        fail(st == QLatin1String("missing") ? (reason == QLatin1String("unavailable") ? tr("Not available") : tr("File not shared")) :
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
        if (err == QLatin1String("not available")) {
            // [up-1] the uploader does not hold that range: forget it, ask elsewhere; no fault
            haveRemove(&m_have, r.off, r.off + r.len);
            m_partial = true;
            Q_EMIT availabilityChanged();
            Q_EMIT requestFinished(r.off, r.len, false, rangeNotAvailableError());
            return;
        }
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
                jStr(o, "reason", 32), hs, o);
}

void OverlayPlugin::onHave(const QString &nodeId, const QJsonObject &o)
{
    const quint32 sid = static_cast<quint32>(jInt(o, "sid", 0));
    auto src = m_sources.value(sid);
    if (!src || src->nodeId() != nodeId) return;
    src->onHave(o);
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
    if (m_slots) return m_slots->total();   // [net-3] one total over the queued-upload networks
    return qBound(0, setting(QString(keys::transfers::MaxUploads), 8).toInt(), 200);
}

int OverlayPlugin::freeSlots() const
{
    if (m_slots) return qMax(0, m_slots->total() - m_slots->used());
    int active = 0;
    for (const UploadSession *s : m_uploads) active += s->active ? 1 : 0;
    return qMax(0, totalSlots() - active);
}

bool OverlayPlugin::slotFree()
{
    if (m_slots) return m_slots->available(Net::Overlay) > 0;
    int active = 0;
    for (const UploadSession *s : m_uploads) active += s->active ? 1 : 0;
    return active < totalSlots();
}

void OverlayPlugin::takeSlot(UploadSession *s, bool forced)
{
    if (m_slots) m_slots->acquire(Net::Overlay, s->uploadId, forced);
}

void OverlayPlugin::giveBackSlot(UploadSession *s)
{
    if (m_slots) m_slots->release(s->uploadId);
}

// the queued requests that could take a slot now (their user is below the per-user limit)
void OverlayPlugin::reportWaiting()
{
    if (!m_slots) return;
    const int perUser = qBound(1, setting(QString(keys::transfers::MaxUploadsPerUser), 2).toInt(), 100);
    QHash<QString, int> userActive;
    for (const UploadSession *s : m_uploads)
        if (s->active) ++userActive[s->userId];
    int n = 0;
    for (const UploadSession *s : m_uploadQueue)
        if (userActive.value(s->userId) < perUser) ++n;
    m_slots->setWaiting(Net::Overlay, n, 5000);   // a push network: it takes a free slot at once
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
    QJsonObject o;
    o[QStringLiteral("t")] = QStringLiteral("slot");
    o[QStringLiteral("v")] = kWireVersion;
    o[QStringLiteral("sid")] = static_cast<double>(s->sid);
    o[QStringLiteral("st")] = st;
    if (pos > 0) o[QStringLiteral("pos")] = pos;
    if (!s->hashSets.isEmpty()) o[QStringLiteral("hs")] = QJsonArray::fromStringList(s->hashSets);   // [engine-3]
    if (s->partial) o[QStringLiteral("have")] = haveToJson(s->have);   // [up-1] what a running download holds
    p->sendToNode(s->nodeId, o);
}
qint64 haveTotal(const QList<QPair<qint64, qint64>> &have)
{
    qint64 n = 0;
    for (const auto &r : have) n += r.second - r.first;
    return n;
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
    // [net-3] an ignored user gets no upload and no queue place: "not available", as for a file we do not have
    if (isIgnoredUser(uid)) return sendSlot(this, nodeId, sid, QStringLiteral("missing"), 0, QStringLiteral("unavailable"));
    const FileHashes h = hashesFromJson(jObj(o, "h"));
    const qint64 size = jInt(o, "size", 0);
    SharedFile file;
    bool found = false;
    for (const SharedFile &f : m_shared)
        if (f.hashes.matches(h) && (size <= 0 || f.size == size)) { file = f; found = true; break; }
    // [up-1] a file we are still downloading: its verified ranges, by its trusted identity. Only to
    // clients that understand "have" maps (older ones would ask for ranges we do not hold)
    QList<QPair<qint64, qint64>> have;
    bool partial = false;
    if (!found && m_ctx.library && setting(QString(keys::library::ShareIncomplete), true).toBool()
        && m_ctx.library->partialRanges(h, &have) && m_ctx.library->findByHash(h, &file)
        && (size <= 0 || file.size == size) && !file.hashes.conflicts(h)) {   // the library matched it
        if (!jBool(o, "part", false)) return sendSlot(this, nodeId, sid, QStringLiteral("missing"));
        found = partial = true;
    }
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
    const int perUser = qBound(1, setting(QString(keys::transfers::MaxUploadsPerUser), 2).toInt(), 100);
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
    s->partial = partial;
    s->path = file.path;
    s->have = have;
    s->haveSentTotal = haveTotal(have);   // the slot answers carry it
    s->lastHaveMs = nowMs();
    // [engine-3] offered in every slot answer; [up-1] only a set that reduces to the file's ed2k hash
    if (file.size >= kEd2kPart && !servableEd2kSet(file.hashes, file.size).isEmpty())
        s->hashSets << QStringLiteral("ed2k-parts");
    s->queuedMs = s->lastActivityMs = nowMs();
    s->serial = ++m_uploadSerial;
    const bool small = bypass > 0 && file.size <= bypass;
    Q_UNUSED(active)
    const bool room = maxUp > 0 && slotFree();
    if (maxUp > 0 && userActive < perUser && (room || small)) {
        s->active = true;
        s->startedMs = nowMs();
        takeSlot(s, !room);   // [net-3] the small file bypass goes beyond the total
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
    reportWaiting();
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
    const int perUser = qBound(1, setting(QString(keys::transfers::MaxUploadsPerUser), 2).toInt(), 100);
    int active = 0;
    QHash<QString, int> userActive;
    for (const UploadSession *s : m_uploads)
        if (s->active) { ++active; ++userActive[s->userId]; }
    bool changed = false;
    reportWaiting();   // [net-3] the pool weighs who waits before it says whether a slot is ours
    for (int i = 0; i < m_uploadQueue.size() && (m_slots ? maxUp > 0 && m_slots->available(Net::Overlay) > 0 : active < maxUp);) {
        UploadSession *s = m_uploadQueue[i];
        if (userActive.value(s->userId) >= perUser) { ++i; continue; }
        m_uploadQueue.removeAt(i);
        takeSlot(s, false);
        s->active = true;
        s->startedMs = s->lastActivityMs = nowMs();
        ++active;
        ++userActive[s->userId];
        sendSlot(this, s, QStringLiteral("ready"));
        reportUpload(s, true);
        changed = true;
    }
    if (changed) sendQueuePositions();
    reportWaiting();
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
    if (s->partial) {
        refreshPartial(s, !haveContains(s->have, off, len));   // what it holds now (grown, or less after a dropped set)
        if (s->partial && !haveContains(s->have, off, len))
            return sendGot(this, nodeId, sid, rq, false, QStringLiteral("not available"));   // [up-1]
    }
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
        parts = servableEd2kSet(s->hashes, s->size);   // [up-1] verified against the file's ed2k hash
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
        s->rateWindowBytes = 0;   // [up-1] nothing more is sent: the row's rate says so at once
        s->rateBps = 0;
        reportUpload(s, true);
        return;
    }
    // [up-1] the downloader says why it stopped (5.2 and older send no reason)
    const QString code = jStr(o, "reason", 16);
    const QString why = cleanText(jStr(o, "why", 160), 160);
    TransferState st = TransferState::Completed;
    QString text = tr("Finished");
    if (code == QLatin1String("banned")) {
        st = TransferState::Error;
        text = why.isEmpty() ? tr("Stopped by the other side: it refused our data") : tr("Stopped by the other side: %1").arg(why);
    } else if (code == QLatin1String("paused")) {
        st = TransferState::Cancelled;
        text = tr("Cancelled by remote (download paused)");
    } else if (code == QLatin1String("cancelled")) {
        st = TransferState::Cancelled;
        text = tr("Cancelled by remote (download cancelled)");
    } else if (code == QLatin1String("quit")) {
        st = TransferState::Cancelled;
        text = tr("Cancelled by remote (program closed)");
    } else if (code == QLatin1String("dropped")) {
        st = TransferState::Cancelled;
        text = tr("Cancelled by remote");
    }
    closeUpload(s, code.isEmpty() ? QStringLiteral("closed by peer") : QStringLiteral("closed by peer: ") + code
                       + (why.isEmpty() ? QString() : QStringLiteral(" (") + why + QLatin1Char(')')),
                false, st, text);
}

// [up-1] A hash set is offered and served only when it is known to be right: it must reduce to the
// file's ed2k hash (eD2k rule: one part = the hash itself, else MD4 over the part hashes, with the
// empty part of an exact multiple). The library hands out only complete files' sets and the trusted
// set of a running download; this checks it once more, so a node never serves a set it cannot vouch for.
QList<QByteArray> OverlayPlugin::servableEd2kSet(const FileHashes &h, qint64 size) const
{
    if (!m_ctx.library || h.ed2k.size() != 16 || size < kEd2kPart) return {};
    QList<QByteArray> parts = m_ctx.library->ed2kPartHashes(h);
    const qint64 data = (size + kEd2kPart - 1) / kEd2kPart, full = size / kEd2kPart + 1;
    if (parts.size() == data && full != data) parts << QCryptographicHash::hash(QByteArray(), QCryptographicHash::Md4);
    if (parts.size() != full || parts.size() > kMaxHashSetParts) return {};
    QCryptographicHash md4(QCryptographicHash::Md4);
    for (const QByteArray &p : std::as_const(parts)) {
        if (p.size() != 16) return {};
        md4.addData(p);
    }
    const QByteArray root = parts.size() == 1 ? parts.first() : md4.result();
    if (root != h.ed2k) {
        qCWarning(lcOverlay) << "not offering an ed2k part hash set that does not reduce to the file's ed2k hash";
        return {};
    }
    return parts;
}

// [up-1] a session of a file we are still downloading: look at the verified ranges again and tell
// the downloader when they grew (at most every 5 s unless forced), or that the file is complete now
void OverlayPlugin::refreshPartial(UploadSession *s, bool force)
{
    if (!s || !s->partial || !m_ctx.library) return;
    QList<QPair<qint64, qint64>> have;
    const qint64 now = nowMs();
    QJsonObject o;
    o[QStringLiteral("t")] = QStringLiteral("have");
    o[QStringLiteral("v")] = kWireVersion;
    o[QStringLiteral("sid")] = static_cast<double>(s->sid);
    if (!m_ctx.library->partialRanges(s->hashes, &have)) {
        // complete now: a library file, found by its hashes or, for a torrent (the library knows a
        // file by its own hashes, not by the torrent's), by the place it was downloaded to
        SharedFile f;
        bool found = m_ctx.library->findByHash(s->hashes, &f) && f.size == s->size && !f.hashes.conflicts(s->hashes);
        for (int i = 0; !found && !s->path.isEmpty() && i < m_shared.size(); ++i)
            if (m_shared.at(i).path == s->path && m_shared.at(i).size == s->size) { f = m_shared.at(i); found = true; }
        if (found) {
            FileHashes all = f.hashes;
            if (!all.conflicts(s->hashes)) all.mergeFrom(s->hashes);
            s->hashes = all;      // reads go to the library file from now on
            s->partial = false;
            s->have.clear();
            o[QStringLiteral("full")] = true;
            sendToNode(s->nodeId, o);
        }
        return;   // not shared yet (being hashed) or gone (cancelled): requests are "not available"
    }
    s->have = have;   // what onGet() serves from now on
    const qint64 total = haveTotal(have);
    if (total == s->haveSentTotal) return;
    if (!force && now - s->lastHaveMs < 5000) return;
    s->lastHaveMs = now;
    s->haveSentTotal = total;
    o[QStringLiteral("have")] = haveToJson(have);
    sendToNode(s->nodeId, o);
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
                n = static_cast<int>(m_ctx.limiter->acquireFor(RateLimiter::Up, Net::Overlay, n));
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
                    // [up-1] a file still downloading may hold less than a moment ago: not a fault
                    sendGot(p, u->nodeId, u->sid, bad, false, u->partial ? QStringLiteral("not available") : QStringLiteral("read error"));
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
    reportFinal(s, TransferState::Transferring, QString());
}

// endText empty: the running state (slot / queue); else the final state and text of a closed upload
void OverlayPlugin::reportFinal(UploadSession *s, TransferState endState, const QString &endText)
{
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
    if (!endText.isEmpty()) {
        t.state = endState;
        t.statusText = endText;
        t.rateUpBps = 0;
    }
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

void OverlayPlugin::closeUpload(UploadSession *s, const QString &reason, bool notifyPeer, TransferState endState,
                                const QString &endText)
{
    if (!s) return;
    m_uploads.remove(s->key);
    m_uploadQueue.removeAll(s);
    giveBackSlot(s);   // [net-3]
    reportWaiting();
    if (notifyPeer) sendSlot(this, s->nodeId, s->sid, QStringLiteral("closed"), 0, reason);
    qCInfo(lcOverlay) << "upload" << s->name << "to" << s->userName << "closed:" << reason << s->bytesSent << "bytes";
    // [up-1] the row's last word: how it ended, no rate any more, the bytes really handed to the link
    // (the core's tracker says "Complete" only when that is the whole file)
    if (s->active || s->bytesSent > 0) {
        s->active = false;
        s->reqs.clear();
        s->rateBps = 0;
        s->rateWindowBytes = 0;
        m_uploadQueue.removeAll(s);
        reportFinal(s, endState, endText.isEmpty() ? tr("Finished") : endText);
    }
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
        if (s->uploadId == uploadId) return closeUpload(s, QStringLiteral("cancelled"), true, TransferState::Cancelled, tr("Cancelled"));
}

// [glue-2] "Start Transfer" on a queued upload: give it a slot now (beyond the slot limit).
bool OverlayPlugin::startUploadNow(const QString &uploadId)
{
    for (UploadSession *s : std::as_const(m_uploadQueue)) {
        if (s->uploadId != uploadId) continue;
        m_uploadQueue.removeAll(s);
        s->active = true;
        takeSlot(s, true);   // [net-3] beyond the total, counted
        reportWaiting();
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
        if (s->nodeId == nodeId) closeUpload(s, QStringLiteral("disconnected"), false, TransferState::Error, tr("Disconnected"));
}

void OverlayPlugin::uploadsTick()
{
    const qint64 now = nowMs();
    // [set-1] "Drop queued users after ... minutes of inactivity": the time set (it was capped at 5
    // minutes); at least 2 minutes, so a waiting client that asks again every minute keeps its place.
    // MX_TEST_MINUTE_MS shortens the minute in the tests.
    bool testMinute = false;
    const int t = qEnvironmentVariableIntValue("MX_TEST_MINUTE_MS", &testMinute);
    const qint64 minute = testMinute && t > 0 ? qint64(t) : qint64(60000);
    const qint64 queueTimeout = qMax<qint64>(2, setting(QString(keys::queue::TimeoutMin), 60).toLongLong()) * minute;
    const QList<UploadSession *> all = m_uploads.values();
    for (UploadSession *s : all) {
        if (s->rateWindowStart == 0) s->rateWindowStart = now;
        if (now - s->rateWindowStart >= 1000) {
            s->rateBps = static_cast<int>(s->rateWindowBytes * 1000 / qMax<qint64>(1, now - s->rateWindowStart));
            s->rateWindowBytes = 0;
            s->rateWindowStart = now;
        }
        if (s->active && s->reqs.empty() && !s->reading && now - s->lastActivityMs > m_t.uploadIdleMs) {
            closeUpload(s, QStringLiteral("idle"), true);   // the downloader stopped asking: "Finished"
            continue;
        }
        if (s->partial) refreshPartial(s, false);   // [up-1]
        if (!s->active && now - s->lastActivityMs > queueTimeout) {
            // queued clients re-open every 60 s; silence means they are gone
            closeUpload(s, QStringLiteral("queue timeout"), true);
            continue;
        }
        reportUpload(s, false);
    }
}

} // namespace mx::ovl
