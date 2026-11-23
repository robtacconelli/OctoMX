// SPDX-License-Identifier: GPL-3.0-or-later
// Net::Overlay: private messages (direct / relayed end-to-end encrypted), browse, whois, privacy.
#include "overlay_plugin_impl.h"

#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include "mx/settings_keys.h"

namespace mx::ovl {

namespace {
ChatEvents *ev(QObject *o) { return static_cast<ChatEvents *>(o); }
constexpr int kMaxPmText = 2048;
constexpr int kBrowsePage = 400;
constexpr int kMaxBrowseFiles = 100000;
QByteArray pmAad(const QString &to, const QString &id) { return QStringLiteral("pm\n%1\n%2").arg(to, id).toUtf8(); }
} // namespace

bool OverlayPlugin::allowedBy(const QString &key, const QString &userId) const
{
    const QString v = setting(key, QStringLiteral("everyone")).toString();
    if (v == QLatin1String("nobody")) return false;
    if (v == QLatin1String("hotlist")) return m_hotlist.contains(userId);
    return true;
}

// ------------------------------------------------------------------------- private messages
// [ovl-2] Delivery: direct on a live link, else end-to-end encrypted through the mesh (mx/relay).
// Nodes that announce the "pm_ack" capability answer every PM with a receipt (pm_ack) or a refusal
// (pm_err), on the path the PM came. No receipt in time: a direct PM is relayed too; a relayed one is
// reported "not delivered" and sent again when the user comes back (or every pmRetryMs while online).
QString OverlayPlugin::pmRefusal(const QString &userId) const
{
    const QString v = setting(QString(keys::messages::AllowPrivateMessages), QStringLiteral("everyone")).toString();
    if (v == QLatin1String("nobody")) return QStringLiteral("nobody");
    const bool hot = m_hotlist.contains(userId);
    if (v == QLatin1String("hotlist") && !hot) return QStringLiteral("hotlist");
    if (setting(QString(keys::messages::IgnoreHotlistOnly), false).toBool() && !hot) return QStringLiteral("hotlist");
    return QString();
}

QString OverlayPlugin::refusalText(const QString &name, const QString &reason) const
{
    if (reason == QLatin1String("hotlist"))
        return tr("%1 accepts private messages only from users on their Hotlist.").arg(name);
    if (reason == QLatin1String("restricted"))
        return tr("%1 accepts private messages only from some users (their message settings).").arg(name);
    return tr("%1 is not accepting private messages.").arg(name);
}

void OverlayPlugin::sendPrivateMessage(const UserRef &to, const QString &text)
{
    sendPrivateMessageWithId(to, text, QString());
}

bool OverlayPlugin::sendPrivateMessageWithId(const UserRef &to, const QString &text, const QString &msgIdIn)
{
    const QString uid = to.id.toLower();
    const bool tracked = !msgIdIn.isEmpty();
    auto error = [this, to](const QString &msg) {
        ChatMessage m;
        m.from = to;
        m.kind = ChatKind::Error;
        m.text = msg;
        m.time = QDateTime::currentDateTime();
        Q_EMIT ev(m_events)->messageReceived(m);
    };
    if (!isHex(uid, 64) || to.net != Net::Overlay) {
        error(tr("Not a OctoMX user."));
        return false;
    }
    if (uid == m_userId) {   // [ovl-2] our own row (room list, Hotlist, search result)
        if (tracked) Q_EMIT ev(m_events)->privateMessageStatus(to, msgIdIn, int(PmDelivery::Refused), tr("this is you"));
        else error(tr("This is you: you cannot send yourself a private message."));
        return false;
    }
    const QString t = cleanText(text, kMaxPmText);
    if (t.trimmed().isEmpty()) return false;
    const QString id = tracked && isHex(msgIdIn) && msgIdIn.size() >= 8 && msgIdIn.size() <= 64 ? msgIdIn.toLower() : randomId();
    QJsonObject msg;
    msg[QStringLiteral("t")] = QStringLiteral("pm");
    msg[QStringLiteral("v")] = kWireVersion;
    msg[QStringLiteral("id")] = id;
    msg[QStringLiteral("ts")] = static_cast<double>(nowMs());
    msg[QStringLiteral("name")] = m_displayName;
    msg[QStringLiteral("text")] = t;
    m_pmPartners.insert(uid);
    OutPm p;
    p.id = id;
    p.uid = uid;
    p.to = to;
    p.msg = msg;
    p.createdMs = nowMs();
    p.tracked = tracked;
    if (!tracked) {
        // untracked callers (scripts, auto replies of other cores) get the old behaviour: errors as lines
        const UserInfo *u = user(uid);
        if (!m_running) { error(tr("You are not connected to the OctoMX network.")); return false; }
        if (!u || u->nodeId.isEmpty()) { error(tr("%1 is not online.").arg(to.name.isEmpty() ? uid.left(8) : to.name)); return false; }
    }
    auto it = m_outPms.insert(id, p);
    pmStatus(*it, PmDelivery::Sending);
    pmTry(*it);
    return true;
}

void OverlayPlugin::pmStatus(const OutPm &p, PmDelivery st, const QString &detail)
{
    if (!p.tracked) {
        // nobody follows this message by id: failures become an Error line in the message window
        if (st != PmDelivery::NotDelivered && st != PmDelivery::Refused) return;
        ChatMessage m;
        m.from = p.to;
        m.kind = ChatKind::Error;
        m.time = QDateTime::currentDateTime();
        m.text = st == PmDelivery::Refused ? detail : tr("Not delivered: %1").arg(detail);
        Q_EMIT ev(m_events)->messageReceived(m);
        return;
    }
    Q_EMIT ev(m_events)->privateMessageStatus(p.to, p.id, int(st), detail);
}

bool OverlayPlugin::pmRelay(OutPm &p)
{
    const UserInfo *ui = user(p.uid);
    if (!ui || ui->xpub.size() != 32 || !m_node || m_peers.isEmpty()) return false;
    const QByteArray inner = QJsonDocument(p.msg).toJson(QJsonDocument::Compact);
    const QByteArray box = sealTo(ui->xpub, inner, pmAad(p.uid, p.id));
    if (box.isEmpty()) return false;
    QJsonObject b = newBody("pm");
    b[QStringLiteral("id")] = p.id;
    b[QStringLiteral("to")] = p.uid;
    b[QStringLiteral("box")] = QString::fromLatin1(box.toBase64());
    qCInfo(lcOverlay) << "pm" << p.id.left(8) << "to" << ui->name << "relayed (end-to-end encrypted)";
    // the outer envelope id must be fresh per copy (pub-sub dedup); the inner pm id stays the same
    publishSigned(QString::fromLatin1(kTopicRelay), b);
    return true;
}

void OverlayPlugin::pmTry(OutPm &p)
{
    const qint64 now = nowMs();
    p.lastTryMs = now;
    ++p.tries;
    const UserInfo *u = user(p.uid);
    const QString name = p.to.name.isEmpty() ? (u ? u->name : p.uid.left(8)) : p.to.name;
    auto notDelivered = [this, &p](const QString &why) {
        p.stage = OutPm::WaitUser;
        if (p.reported) return;
        p.reported = true;
        pmStatus(p, PmDelivery::NotDelivered, why);
    };
    if (!m_running) return notDelivered(tr("you are not connected to the OctoMX network"));
    if (!u || u->nodeId.isEmpty() || !u->online)
        return notDelivered(tr("%1 is offline; it is sent when %1 comes back online").arg(name));
    const bool acks = u->has("pm_ack");
    if (m_peers.contains(u->nodeId) && m_peers.value(u->nodeId).hello) {
        sendRaw(u->nodeId, p.msg);
        qCDebug(lcOverlay) << "pm" << p.id.left(8) << "to" << u->name << "sent direct";
        if (!acks) {   // an older node: no receipt will come
            pmStatus(p, PmDelivery::Sent);
            m_outPms.remove(p.id);
            return;
        }
        p.stage = OutPm::WaitDirectAck;
        p.deadlineMs = now + m_t.pmAckMs;
        return;
    }
    // not linked: relay now (the mesh carries it at once), and connect in the background so the next
    // messages, browse and transfers go direct
    if (!u->addrs.isEmpty() && m_node && now - m_reachAskedMs.value(u->nodeId, 0) > 30000)
        for (const QString &a : u->addrs) m_node->connectTo(a);
    if (!pmRelay(p)) {
        if (m_peers.isEmpty()) return notDelivered(tr("you are not connected to any other OctoMX user"));
        return notDelivered(tr("%1 cannot be reached").arg(name));
    }
    if (!acks) {
        pmStatus(p, PmDelivery::Sent);
        m_outPms.remove(p.id);
        return;
    }
    p.stage = OutPm::WaitRelayAck;
    p.deadlineMs = now + m_t.pmRelayAckMs;
}

void OverlayPlugin::pmTick()
{
    const qint64 now = nowMs();
    QStringList drop;
    for (auto it = m_outPms.begin(); it != m_outPms.end(); ++it) {
        OutPm &p = it.value();
        if (now - p.createdMs > m_t.pmGiveUpMs) {
            pmStatus(p, PmDelivery::NotDelivered, tr("gave up after %n minute(s)", nullptr, int(m_t.pmGiveUpMs / 60000)));
            drop << it.key();
            continue;
        }
        if (p.stage == OutPm::WaitDirectAck && now > p.deadlineMs) {
            qCDebug(lcOverlay) << "pm" << p.id.left(8) << "no receipt on the direct link: relaying it too";
            if (pmRelay(p)) {
                p.stage = OutPm::WaitRelayAck;
                p.deadlineMs = now + m_t.pmRelayAckMs;
            } else {
                p.stage = OutPm::WaitRelayAck;
                p.deadlineMs = now;   // fails below
            }
        }
        if (p.stage == OutPm::WaitRelayAck && now > p.deadlineMs) {
            const UserInfo *u = user(p.uid);
            const QString name = u ? u->name : p.to.name;
            p.stage = OutPm::WaitUser;
            if (!p.reported) {
                p.reported = true;
                pmStatus(p, PmDelivery::NotDelivered, tr("no answer from %1; it is sent again when %1 is back").arg(name));
            }
            continue;
        }
        if (p.stage == OutPm::WaitUser && now - p.lastTryMs > m_t.pmRetryMs) {
            const UserInfo *u = user(p.uid);
            if (u && u->online) pmTry(p);
            else p.lastTryMs = now;
        }
    }
    for (const QString &id : drop) m_outPms.remove(id);
}

void OverlayPlugin::pmRetryFor(const QString &userId)
{
    QStringList ids;
    for (auto it = m_outPms.cbegin(); it != m_outPms.cend(); ++it)
        if (it->uid == userId && it->stage == OutPm::WaitUser) ids << it.key();
    std::sort(ids.begin(), ids.end(), [this](const QString &a, const QString &b) {
        return m_outPms.value(a).createdMs < m_outPms.value(b).createdMs;
    });
    for (const QString &id : ids) {
        auto it = m_outPms.find(id);
        if (it != m_outPms.end()) {
            qCDebug(lcOverlay) << "pm" << id.left(8) << "retry: the user is back";
            pmTry(it.value());
        }
    }
}

void OverlayPlugin::onPmAck(const QString &fromUser, const QString &id)
{
    auto it = m_outPms.find(id);
    if (it == m_outPms.end() || it->uid != fromUser) return;
    const bool wasReported = it->reported;
    qCDebug(lcOverlay) << "pm" << id.left(8) << "delivered" << (wasReported ? "(after a retry)" : "");
    pmStatus(*it, PmDelivery::Delivered, wasReported ? tr("delivered now") : QString());
    m_outPms.erase(it);
}

void OverlayPlugin::onPmRefused(const QString &fromUser, const QString &id, const QString &reason)
{
    const UserRef from = userRefFor(fromUser);
    const QString text = refusalText(from.name, reason);
    auto it = m_outPms.find(id);
    if (it != m_outPms.end() && it->uid == fromUser) {
        pmStatus(*it, PmDelivery::Refused, text);
        m_outPms.erase(it);
        return;
    }
    if (isHex(id) && !m_seen.insert(QStringLiteral("pmerr:") + id)) return;   // relayed + direct copy
    ChatMessage m;   // untracked (or older sender path): an Error line in the message window
    m.from = from;
    m.kind = ChatKind::Error;
    m.time = QDateTime::currentDateTime();
    m.text = text;
    Q_EMIT ev(m_events)->messageReceived(m);
}

void OverlayPlugin::publishRelay(const char *type, const QString &toUser, QJsonObject extra)
{
    QJsonObject b = newBody(type);
    for (auto it = extra.begin(); it != extra.end(); ++it) b.insert(it.key(), it.value());
    b[QStringLiteral("to")] = toUser;
    publishSigned(QString::fromLatin1(kTopicRelay), b);
}

void OverlayPlugin::answerPm(const QString &fromUser, const QString &id, const QString &refusal, bool relayed,
                             const QString &nodeId)
{
    QJsonObject a;
    a[QStringLiteral("v")] = kWireVersion;
    a[QStringLiteral("id")] = id;
    if (!refusal.isEmpty()) a[QStringLiteral("reason")] = refusal;
    if (!relayed && !nodeId.isEmpty() && m_peers.contains(nodeId)) {
        a[QStringLiteral("t")] = refusal.isEmpty() ? QStringLiteral("pm_ack") : QStringLiteral("pm_err");
        sendRaw(nodeId, a);
        return;
    }
    // came through the mesh: answer the same way (signed by us; carries no text)
    QJsonObject x;
    x[QStringLiteral("mid")] = id;
    if (!refusal.isEmpty()) x[QStringLiteral("reason")] = refusal;
    publishRelay(refusal.isEmpty() ? "pm_ack" : "pm_err", fromUser, x);
}

void OverlayPlugin::refusePrivateMessage(const ChatMessage &msg, const QString &reason)
{
    if (!msg.msgId.isEmpty() && msg.msgId == m_pmInFlight)
        m_pmInFlightRefusal = reason.isEmpty() ? QStringLiteral("refused") : reason.left(16);
}

void OverlayPlugin::onRelayEnvelope(const QString &fromUser, const QJsonObject &b)
{
    if (jStr(b, "to", 64) != m_userId) return;
    const QString t = jStr(b, "t", 16);
    if (t == QLatin1String("pm_ack")) return onPmAck(fromUser, jStr(b, "mid", 64));
    if (t == QLatin1String("pm_err")) return onPmRefused(fromUser, jStr(b, "mid", 64), jStr(b, "reason", 16));
    if (t == QLatin1String("reach")) return onReachRequest(fromUser, b);
    if (t == QLatin1String("ping")) {
        QJsonObject x;
        x[QStringLiteral("pid")] = jStr(b, "pid", 64);
        publishRelay("pong", fromUser, x);
        return;
    }
    if (t == QLatin1String("pong")) {
        auto it = m_users.find(fromUser);
        if (it != m_users.end() && it->pingSentMs > 0 && jStr(b, "pid", 64) == it->pingId) {
            qCDebug(lcOverlay) << it->name << "answered through the mesh: still online";
            it->pingSentMs = 0;
            it->lastSeenMs = nowMs();
        }
        return;
    }
    if (t != QLatin1String("pm")) return;
    const QString id = jStr(b, "id", 64);
    const QByteArray box = QByteArray::fromBase64(jStr(b, "box", 16000).toLatin1());
    QByteArray plain;
    if (!m_id.open(box, pmAad(m_userId, id), &plain)) {
        qCInfo(lcOverlay) << "relayed pm from" << fromUser.left(8) << "does not decrypt";
        return;
    }
    QJsonObject inner;
    if (!decodeControl(QByteArray("J") + plain, &inner, 32 * 1024)) return;
    if (jStr(inner, "id", 64) != id) return;   // inner and outer must agree
    onPm(fromUser, inner, true);
}

void OverlayPlugin::onPm(const QString &fromUser, const QJsonObject &o, bool relayed, const QString &nodeId)
{
    const QString id = jStr(o, "id", 64);
    if (!isHex(id) || id.size() < 8) return;
    // a copy we saw already (direct + relayed, or a retry after a lost receipt): answer it again
    if (m_pmVerdicts.contains(id)) {
        answerPm(fromUser, id, m_pmVerdicts.value(id), relayed, nodeId);
        return;
    }
    if (!m_seen.insert(QStringLiteral("pm:") + id)) return;
    auto remember = [this](const QString &mid, const QString &verdict) {
        m_pmVerdicts.insert(mid, verdict);
        m_pmVerdictOrder << mid;
        while (m_pmVerdictOrder.size() > 4096) m_pmVerdicts.remove(m_pmVerdictOrder.takeFirst());
    };
    QString refusal = pmRefusal(fromUser);
    if (refusal.isEmpty() && !floodOk(QStringLiteral("pm:") + fromUser)) return;   // flood: no answer
    const QString text = cleanText(jStr(o, "text", kMaxPmText), kMaxPmText);
    if (refusal.isEmpty() && text.isEmpty()) return;
    if (refusal.isEmpty()) {
        ChatMessage m;
        m.from = userRefFor(fromUser);
        const QString name = cleanText(jStr(o, "name", 48), 48);
        if (!name.isEmpty()) m.from.name = name;
        // [ovl-2] `to` stays empty: by the API contract (types.h) only the local echo of a PM *we* sent
        // carries it, and the UI files a message with `to` set as our own (it used to land in a hidden
        // window "to ourselves", so incoming messages seemed lost)
        m.text = text;
        m.kind = ChatKind::Private;
        m.msgId = id;
        const qint64 ts = jInt(o, "ts", 0);
        m.time = ts > 0 && qAbs(ts - nowMs()) < 86400000 ? QDateTime::fromMSecsSinceEpoch(ts) : QDateTime::currentDateTime();
        m_pmPartners.insert(fromUser);
        // the core may refuse it by rules only it knows (refusePrivateMessage, called synchronously)
        m_pmInFlight = id;
        m_pmInFlightRefusal.clear();
        Q_EMIT ev(m_events)->messageReceived(m);
        refusal = m_pmInFlightRefusal;
        m_pmInFlight.clear();
        m_pmInFlightRefusal.clear();
    }
    remember(id, refusal);
    if (!refusal.isEmpty()) qCInfo(lcOverlay) << "pm from" << userRefFor(fromUser).name << "refused:" << refusal;
    // every node answers a refusal (older senders show it as an Error line); receipts only go to
    // senders that asked for them by announcing "pm_ack"
    const UserInfo *u = user(fromUser);
    if (!refusal.isEmpty() || (u && u->has("pm_ack"))) {
        if (relayed && !refusal.isEmpty() && !(u && u->has("pm_ack"))) return;   // older nodes ignore it anyway
        answerPm(fromUser, id, refusal, relayed, nodeId);
    }
}

QString OverlayPlugin::unreachableText(const QString &userId) const
{
    const UserInfo *u = user(userId);
    const QString name = u && !u->name.isEmpty() ? u->name : userRefFor(userId).name;
    if (!m_running) return tr("You are not connected to the OctoMX network");
    if (!u || !u->online) return tr("%1 is offline").arg(name);
    if (m_peers.isEmpty()) return tr("Cannot reach %1: you are not connected to any OctoMX user").arg(name);
    if (u->nodeId.isEmpty())
        return tr("Cannot reach %1 yet: their program has not announced its network address (an older version "
                  "does so every 2 minutes); try again in a moment").arg(name);
    return tr("Cannot reach %1: no direct connection, no connect-back and no relay worked "
              "(when both sides are firewalled, a OctoMX user connected to both who relays is needed)").arg(name);
}

// ------------------------------------------------------------------------------------ browse
namespace {
BrowseEntry entryFromJson(const QJsonObject &f)
{
    BrowseEntry e;
    e.path = cleanText(jStr(f, "p", 1024), 1024);
    if (e.path.isEmpty()) e.path = cleanText(jStr(f, "n", 512), 512);
    e.size = qMax<qint64>(0, jInt(f, "s", 0));
    e.durationSec = static_cast<int>(jInt(f, "d", -1));
    e.bitrateKbps = static_cast<int>(jInt(f, "b", -1));
    e.sampleRateHz = static_cast<int>(jInt(f, "r", -1));
    e.hashes = hashesFromJson(jObj(f, "h"));
    return e;
}
} // namespace

void OverlayPlugin::browse(quint32 requestId, const UserRef &u)
{
    const QString uid = u.id.toLower();
    if (uid == m_userId) {
        // [ovl-2] our own row: the files we share, straight from the list the network sees (the same
        // list the room / presence file count comes from), no round trip
        const QList<SharedFile> files = m_haveSharedList || !m_ctx.library ? m_shared : m_ctx.library->sharedFiles();
        QList<BrowseEntry> entries;
        for (int i = 0; i < qMin<int>(files.size(), kMaxBrowseFiles); ++i) {
            const BrowseEntry e = entryFromJson(fileToJson(files[i], true));
            if (!e.path.isEmpty()) entries << e;
        }
        QTimer::singleShot(0, this, [this, requestId, u, entries] { Q_EMIT browseResults(requestId, u, entries, true); });
        return;
    }
    const UserInfo *info = user(uid);
    if (!m_running || !info || (info->nodeId.isEmpty() && !info->online)) {
        const QString why = unreachableText(uid);
        QTimer::singleShot(0, this, [this, requestId, u, why] { Q_EMIT browseFailed(requestId, u, why); });
        return;
    }
    BrowseReq r;
    r.user = u;
    r.nodeId = info->nodeId;   // may be empty: queued by user until its hello (sendToUser)
    r.rid = randomId();
    r.lastMs = nowMs();
    m_browse.insert(requestId, r);
    sendBrowseRequest(requestId);
}

void OverlayPlugin::sendBrowseRequest(quint32 requestId)
{
    auto it = m_browse.find(requestId);
    if (it == m_browse.end()) return;
    it->lastMs = nowMs();
    QJsonObject q;
    q[QStringLiteral("t")] = QStringLiteral("browse_req");
    q[QStringLiteral("v")] = kWireVersion;
    q[QStringLiteral("rid")] = it->rid;
    sendToUser(it->user.id.toLower(), q, [this, requestId](bool ok) {
        if (ok || !m_browse.contains(requestId)) return;
        const BrowseReq br = m_browse.take(requestId);
        Q_EMIT browseFailed(requestId, br.user, unreachableText(br.user.id.toLower()));
    });
}

void OverlayPlugin::onBrowseRequest(const QString &nodeId, const QJsonObject &o)
{
    const QString uid = m_nodeUser.value(nodeId);
    const QString rid = jStr(o, "rid", 64);
    if (!isHex(rid)) return;
    if (!allowedBy(QString(keys::messages::AllowBrowse), uid)) {
        QJsonObject e;
        e[QStringLiteral("t")] = QStringLiteral("browse_err");
        e[QStringLiteral("v")] = kWireVersion;
        e[QStringLiteral("rid")] = rid;
        // [ovl-2] "hotlist": only users on their Hotlist may browse (older nodes send "refused")
        const bool hot = setting(QString(keys::messages::AllowBrowse), QString()).toString() == QLatin1String("hotlist");
        e[QStringLiteral("reason")] = hot ? QStringLiteral("hotlist") : QStringLiteral("refused");
        qCInfo(lcOverlay) << "browse by" << userRefFor(uid).name << "refused:" << e.value(QStringLiteral("reason")).toString();
        sendRaw(nodeId, e);
        return;
    }
    const QList<SharedFile> files = m_haveSharedList || !m_ctx.library ? m_shared : m_ctx.library->sharedFiles();
    const int total = qMin<int>(files.size(), kMaxBrowseFiles);
    const int pages = qMax(1, (total + kBrowsePage - 1) / kBrowsePage);
    for (int p = 0; p < pages; ++p) {
        QJsonArray arr;
        for (int i = p * kBrowsePage; i < qMin(total, (p + 1) * kBrowsePage); ++i) arr << fileToJson(files[i], true);
        QJsonObject page;
        page[QStringLiteral("t")] = QStringLiteral("browse");
        page[QStringLiteral("v")] = kWireVersion;
        page[QStringLiteral("rid")] = rid;
        page[QStringLiteral("page")] = p;
        page[QStringLiteral("pages")] = pages;
        page[QStringLiteral("total")] = total;
        page[QStringLiteral("files")] = arr;
        sendRaw(nodeId, page);   // compressed ('Z') when large; librats queues + paces it
    }
}

void OverlayPlugin::onBrowse(const QString &nodeId, const QJsonObject &o)
{
    const QString rid = jStr(o, "rid", 64);
    quint32 reqId = 0;
    BrowseReq *req = nullptr;
    const QString fromUser = m_nodeUser.value(nodeId);
    for (auto it = m_browse.begin(); it != m_browse.end(); ++it)
        if (it->rid == rid && (it->nodeId == nodeId || it->user.id.toLower() == fromUser)) {
            reqId = it.key();
            req = &it.value();
            req->nodeId = nodeId;
            break;
        }
    if (!req) return;
    if (jStr(o, "t", 16) == QLatin1String("browse_err")) {
        const UserRef u = req->user;
        m_browse.remove(reqId);
        const QString why = jStr(o, "reason", 16);
        Q_EMIT browseFailed(reqId, u, why == QLatin1String("hotlist")
                                          ? tr("%1 does not allow browsing: only users on their Hotlist may browse").arg(u.name)
                                          : tr("%1 does not allow browsing").arg(u.name));
        return;
    }
    QList<BrowseEntry> entries;
    for (const QJsonValue &v : jArr(o, "files", 1000)) {
        if (!v.isObject()) continue;
        const BrowseEntry e = entryFromJson(v.toObject());
        if (!e.path.isEmpty()) entries << e;
    }
    const int page = static_cast<int>(jInt(o, "page", 0));
    const int pages = static_cast<int>(jInt(o, "pages", 1));
    req->lastMs = nowMs();
    req->got += entries.size();
    const bool finished = page + 1 >= pages;
    const UserRef u = req->user;
    if (finished) m_browse.remove(reqId);
    Q_EMIT browseResults(reqId, u, entries, finished);
}

// ------------------------------------------------------------------------------------- whois
void OverlayPlugin::whois(const UserRef &u)
{
    const QString uid = u.id.toLower();
    auto partial = [this, u, uid](bool reachable) {
        QVariantMap m;
        const UserInfo *i = user(uid);
        m.insert(QStringLiteral("id"), uid);
        m.insert(QStringLiteral("name"), i ? i->name : u.name);
        m.insert(QStringLiteral("online"), i && i->online);
        m.insert(QStringLiteral("reachable"), reachable);
        if (i) {
            m.insert(QStringLiteral("client"), i->client);
            m.insert(QStringLiteral("files"), i->files);
            m.insert(QStringLiteral("connection"), i->conn);
            if (i->sinceMs > 0) m.insert(QStringLiteral("onlineSec"), (nowMs() - i->sinceMs) / 1000);
        }
        Q_EMIT whoisResult(u, m);
    };
    if (uid == m_userId) {   // [ovl-2] ourselves: what others would get, without a round trip
        qint64 bytes = 0;
        for (const SharedFile &f : m_shared) bytes += f.size;
        QStringList rooms;
        for (const Room &r : m_rooms)
            if (r.joined) rooms << r.name;
        QVariantMap m;
        m.insert(QStringLiteral("id"), uid);
        m.insert(QStringLiteral("name"), m_displayName);
        m.insert(QStringLiteral("online"), m_running);
        m.insert(QStringLiteral("reachable"), m_running);
        m.insert(QStringLiteral("client"), (m_ctx.clientName.isEmpty() ? QStringLiteral("OctoMX") : m_ctx.clientName) + QLatin1Char(' ') +
                                               (m_ctx.clientVersion.isEmpty() ? QStringLiteral(MX_VERSION_STRING) : m_ctx.clientVersion));
        m.insert(QStringLiteral("files"), m_shared.size());
        m.insert(QStringLiteral("bytes"), bytes);
        m.insert(QStringLiteral("connection"), connectionLabel());
        m.insert(QStringLiteral("onlineSec"), m_running ? (nowMs() - m_onlineSinceMs) / 1000 : 0);
        m.insert(QStringLiteral("freeSlots"), freeSlots());
        m.insert(QStringLiteral("totalSlots"), totalSlots());
        m.insert(QStringLiteral("rooms"), rooms);
        QTimer::singleShot(0, this, [this, u, m] { Q_EMIT whoisResult(u, m); });
        return;
    }
    if (!m_running || !user(uid)) {
        QTimer::singleShot(0, this, [partial] { partial(false); });
        return;
    }
    WhoisReq w;
    w.user = u;
    w.rid = randomId();
    w.sentMs = nowMs();
    m_whois.insert(w.rid, w);
    QJsonObject q;
    q[QStringLiteral("t")] = QStringLiteral("whois_req");
    q[QStringLiteral("v")] = kWireVersion;
    q[QStringLiteral("rid")] = w.rid;
    const QString rid = w.rid;
    sendToUser(uid, q, [this, rid, partial](bool ok) {
        if (ok || !m_whois.contains(rid)) return;
        m_whois.remove(rid);
        partial(false);
    });
}

void OverlayPlugin::onWhoisRequest(const QString &nodeId, const QJsonObject &o)
{
    const QString rid = jStr(o, "rid", 64);
    if (!isHex(rid)) return;
    QJsonObject a;
    a[QStringLiteral("v")] = kWireVersion;
    a[QStringLiteral("rid")] = rid;
    if (!setting(QString(keys::messages::AllowWhois), true).toBool()) {
        a[QStringLiteral("t")] = QStringLiteral("whois_err");
        sendRaw(nodeId, a);
        return;
    }
    a[QStringLiteral("t")] = QStringLiteral("whois");
    a[QStringLiteral("name")] = m_displayName;
    a[QStringLiteral("client")] = m_ctx.clientName.isEmpty() ? QStringLiteral("OctoMX") : m_ctx.clientName;
    a[QStringLiteral("ver")] = m_ctx.clientVersion.isEmpty() ? QStringLiteral(MX_VERSION_STRING) : m_ctx.clientVersion;
    qint64 bytes = 0;
    for (const SharedFile &f : m_shared) bytes += f.size;
    a[QStringLiteral("files")] = m_shared.size();
    a[QStringLiteral("bytes")] = static_cast<double>(bytes);
    a[QStringLiteral("conn")] = connectionLabel();
    a[QStringLiteral("online")] = static_cast<double>((nowMs() - m_onlineSinceMs) / 1000);
    a[QStringLiteral("free")] = freeSlots();
    a[QStringLiteral("total")] = totalSlots();
    QJsonArray rooms;
    for (const Room &r : m_rooms)
        if (r.joined) rooms << r.name;
    a[QStringLiteral("rooms")] = rooms;
    sendRaw(nodeId, a);
}

void OverlayPlugin::onWhois(const QString &nodeId, const QJsonObject &o, bool error)
{
    const QString rid = jStr(o, "rid", 64);
    auto it = m_whois.find(rid);
    if (it == m_whois.end()) return;
    const QString uid = m_nodeUser.value(nodeId);
    if (uid != it->user.id.toLower()) return;
    const UserRef u = it->user;
    m_whois.erase(it);
    QVariantMap m;
    m.insert(QStringLiteral("id"), uid);
    m.insert(QStringLiteral("online"), true);
    m.insert(QStringLiteral("reachable"), true);
    if (error) {
        m.insert(QStringLiteral("name"), userRefFor(uid).name);
        m.insert(QStringLiteral("error"), tr("User does not answer whois requests"));
        Q_EMIT whoisResult(u, m);
        return;
    }
    m.insert(QStringLiteral("name"), cleanText(jStr(o, "name", 48), 48));
    m.insert(QStringLiteral("client"), cleanText(jStr(o, "client", 32) + QLatin1Char(' ') + jStr(o, "ver", 16), 48));
    m.insert(QStringLiteral("files"), jInt(o, "files", 0));
    m.insert(QStringLiteral("bytes"), jInt(o, "bytes", 0));
    m.insert(QStringLiteral("connection"), cleanText(jStr(o, "conn", 24), 24));
    m.insert(QStringLiteral("onlineSec"), jInt(o, "online", 0));
    m.insert(QStringLiteral("freeSlots"), jInt(o, "free", -1));
    m.insert(QStringLiteral("totalSlots"), jInt(o, "total", -1));
    QStringList rooms;
    for (const QJsonValue &v : jArr(o, "rooms", 50))
        if (v.isString()) rooms << cleanText(v.toString(), 64);
    m.insert(QStringLiteral("rooms"), rooms);
    Q_EMIT whoisResult(u, m);
}

void OverlayPlugin::expireRequests()
{
    const qint64 now = nowMs();
    QList<quint32> dead;
    for (auto it = m_browse.cbegin(); it != m_browse.cend(); ++it)
        if (now - it->lastMs > 30000) dead << it.key();
    for (quint32 id : dead) {
        const BrowseReq r = m_browse.take(id);
        Q_EMIT browseFailed(id, r.user, r.got > 0 ? tr("Browse interrupted: %1 stopped sending the list").arg(r.user.name)
                                                  : tr("Timed out: %1 did not answer the browse request").arg(r.user.name));
    }
    QStringList deadW;
    for (auto it = m_whois.cbegin(); it != m_whois.cend(); ++it)
        if (now - it->sentMs > 15000) deadW << it.key();
    for (const QString &rid : deadW) {
        const WhoisReq w = m_whois.take(rid);
        QVariantMap m;
        m.insert(QStringLiteral("id"), w.user.id);
        m.insert(QStringLiteral("name"), w.user.name);
        m.insert(QStringLiteral("reachable"), false);
        m.insert(QStringLiteral("online"), isUserOnline(w.user));
        Q_EMIT whoisResult(w.user, m);
    }
}

} // namespace mx::ovl
