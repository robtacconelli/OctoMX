// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_hostcache.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QSaveFile>
#include <algorithm>

namespace mx::gnut {

qint64 nowSecs() { return QDateTime::currentSecsSinceEpoch(); }

double HostEntry::score(qint64 now) const
{
    double s = 0;
    if (isStatic) s += 50;
    if (lastSuccess > 0) {
        const double ageH = double(now - lastSuccess) / 3600.0;
        s += 100.0 / (1.0 + ageH / 24.0);          // worked recently: strong preference
        s += qMin(successes, 20) * 2.0;
    }
    if (lastSeen > 0) s += 20.0 / (1.0 + double(now - lastSeen) / 3600.0);
    s -= failures * 15.0;
    if (source == QLatin1String("x-try") || source == QLatin1String("uhc")) s += 5;
    if (tls) s += 5;                               // [enc-1] prefer hosts that encrypt
    return s;
}

static QJsonObject hostToJson(const HostEntry &h)
{
    QJsonObject o;
    o[QStringLiteral("addr")] = h.addr.toString();
    if (!h.hostName.isEmpty()) o[QStringLiteral("name")] = h.hostName;
    o[QStringLiteral("added")] = h.added;
    o[QStringLiteral("seen")] = h.lastSeen;
    o[QStringLiteral("attempt")] = h.lastAttempt;
    o[QStringLiteral("success")] = h.lastSuccess;
    o[QStringLiteral("ok")] = h.successes;
    o[QStringLiteral("fail")] = h.failures;
    o[QStringLiteral("failTotal")] = h.totalFailures;
    o[QStringLiteral("retry")] = h.retryAfter;
    if (!h.vendor.isEmpty()) o[QStringLiteral("vendor")] = h.vendor;
    o[QStringLiteral("src")] = h.source;
    if (h.isStatic) o[QStringLiteral("static")] = true;
    if (h.tls) o[QStringLiteral("tls")] = true;
    return o;
}

static QJsonObject cacheToJson(const CacheEntry &c)
{
    QJsonObject o;
    o[QStringLiteral("url")] = c.url;
    o[QStringLiteral("attempt")] = c.lastAttempt;
    o[QStringLiteral("success")] = c.lastSuccess;
    o[QStringLiteral("ok")] = c.successes;
    o[QStringLiteral("fail")] = c.failures;
    return o;
}

bool HostCache::load()
{
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = f.read(8 * 1024 * 1024);
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    const QJsonArray hosts = root.value(QStringLiteral("hosts")).toArray();
    for (const QJsonValue &v : hosts) {
        const QJsonObject o = v.toObject();
        HostEntry h;
        h.addr = HostPort::parse(o.value(QStringLiteral("addr")).toString());
        if (!h.addr.isValid()) continue;
        h.hostName = o.value(QStringLiteral("name")).toString();
        h.added = qint64(o.value(QStringLiteral("added")).toDouble());
        h.lastSeen = qint64(o.value(QStringLiteral("seen")).toDouble());
        h.lastAttempt = qint64(o.value(QStringLiteral("attempt")).toDouble());
        h.lastSuccess = qint64(o.value(QStringLiteral("success")).toDouble());
        h.successes = o.value(QStringLiteral("ok")).toInt();
        h.failures = o.value(QStringLiteral("fail")).toInt();
        h.totalFailures = o.value(QStringLiteral("failTotal")).toInt();
        h.retryAfter = qint64(o.value(QStringLiteral("retry")).toDouble());
        h.vendor = o.value(QStringLiteral("vendor")).toString().left(100);
        h.source = o.value(QStringLiteral("src")).toString().left(20);
        h.isStatic = o.value(QStringLiteral("static")).toBool();
        h.tls = o.value(QStringLiteral("tls")).toBool();
        m_hosts.insert(h.addr.key(), h);
    }
    const auto loadCaches = [&](const char *key, QList<CacheEntry> &list) {
        const QJsonArray a = root.value(QLatin1String(key)).toArray();
        for (const QJsonValue &v : a) {
            const QJsonObject o = v.toObject();
            const QString url = o.value(QStringLiteral("url")).toString();
            if (url.isEmpty() || url.size() > 300) continue;
            auto it = std::find_if(list.begin(), list.end(), [&](const CacheEntry &c) { return c.url == url; });
            CacheEntry *c = nullptr;
            if (it == list.end()) { list << CacheEntry(); c = &list.last(); c->url = url; }
            else c = &*it;
            c->lastAttempt = qint64(o.value(QStringLiteral("attempt")).toDouble());
            c->lastSuccess = qint64(o.value(QStringLiteral("success")).toDouble());
            c->successes = o.value(QStringLiteral("ok")).toInt();
            c->failures = o.value(QStringLiteral("fail")).toInt();
            if (list.size() > 100) break;
        }
    };
    loadCaches("gwcs", m_gwcs);
    loadCaches("uhcs", m_uhcs);
    trim();
    m_dirty = false;
    return true;
}

bool HostCache::save() const
{
    if (m_path.isEmpty()) return false;
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QJsonArray hosts;
    QList<HostEntry> list = m_hosts.values();
    const qint64 now = nowSecs();
    std::sort(list.begin(), list.end(), [now](const HostEntry &a, const HostEntry &b) { return a.score(now) > b.score(now); });
    for (const HostEntry &h : list) hosts.append(hostToJson(h));
    QJsonArray g, u;
    for (const CacheEntry &c : m_gwcs) g.append(cacheToJson(c));
    for (const CacheEntry &c : m_uhcs) u.append(cacheToJson(c));
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("hosts")] = hosts;
    root[QStringLiteral("gwcs")] = g;
    root[QStringLiteral("uhcs")] = u;
    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    const bool ok = f.commit();
    if (ok) m_dirty = false;
    return ok;
}

bool HostCache::add(const HostPort &h, const QString &source, qint64 now)
{
    if (!h.isValid() || isBogonIp(h.ip)) return false;
    if (!now) now = nowSecs();
    auto it = m_hosts.find(h.key());
    m_dirty = true;
    if (it != m_hosts.end()) {
        it->lastSeen = now;
        return false;
    }
    HostEntry e;
    e.addr = h;
    e.added = now;
    e.lastSeen = now;
    e.source = source;
    m_hosts.insert(h.key(), e);
    if (m_hosts.size() > MaxHosts + 200) trim();
    return true;
}

void HostCache::addStatic(const HostPort &h, const QString &name)
{
    if (!h.isValid()) return;
    add(h, QStringLiteral("static"));
    HostEntry &e = m_hosts[h.key()];
    e.isStatic = true;
    e.source = QStringLiteral("static");
    if (!name.isEmpty()) e.hostName = name;
    e.retryAfter = 0;
    m_dirty = true;
}

bool HostCache::remove(const QString &id)
{
    const HostPort h = HostPort::parse(id);
    m_dirty = true;
    return m_hosts.remove(h.key()) > 0;
}

void HostCache::markAttempt(const HostPort &h, qint64 now)
{
    auto it = m_hosts.find(h.key());
    if (it == m_hosts.end()) return;
    it->lastAttempt = now ? now : nowSecs();
    m_dirty = true;
}

void HostCache::markSuccess(const HostPort &h, const QString &vendor, qint64 now)
{
    if (!now) now = nowSecs();
    auto it = m_hosts.find(h.key());
    if (it == m_hosts.end()) {
        add(h, QStringLiteral("connected"), now);
        it = m_hosts.find(h.key());
        if (it == m_hosts.end()) return;
    }
    it->lastSuccess = now;
    it->lastSeen = now;
    it->successes++;
    it->failures = 0;
    it->retryAfter = 0;
    if (!vendor.isEmpty()) it->vendor = vendor.left(100);
    m_dirty = true;
}

void HostCache::markFailure(const HostPort &h, int retryAfterSec, qint64 now)
{
    if (!now) now = nowSecs();
    auto it = m_hosts.find(h.key());
    if (it == m_hosts.end()) return;
    it->failures++;
    it->totalFailures++;
    // exponential back-off: 2 min, 4, 8 ... capped at 12 h; a server-given Retry-After wins if longer
    const qint64 backoff = qMin<qint64>(120LL << qMin(it->failures - 1, 9), 12 * 3600);
    it->retryAfter = now + qMax<qint64>(backoff, retryAfterSec);
    m_dirty = true;
    if (!it->isStatic && it->lastSuccess == 0 && it->failures >= 4) m_hosts.erase(it);
}

void HostCache::markUseless(const HostPort &h)
{
    auto it = m_hosts.find(h.key());
    if (it == m_hosts.end() || it->isStatic) return;
    m_hosts.erase(it);
    m_dirty = true;
}

void HostCache::setTls(const HostPort &h, bool tls)
{
    auto it = m_hosts.find(h.key());
    if (it == m_hosts.end() || it->tls == tls) return;
    it->tls = tls;
    m_dirty = true;
}

bool HostCache::supportsTls(const HostPort &h) const
{
    auto it = m_hosts.constFind(h.key());
    return it != m_hosts.constEnd() && it->tls;
}

int HostCache::tlsCount() const
{
    int n = 0;
    for (const HostEntry &e : m_hosts) if (e.tls) ++n;
    return n;
}

QList<HostPort> HostCache::pick(int n, const QSet<quint64> &exclude, qint64 now) const
{
    if (!now) now = nowSecs();
    QList<const HostEntry *> cands;
    for (const HostEntry &e : m_hosts)
        if (!exclude.contains(e.addr.key()) && e.retryAfter <= now) cands << &e;
    // score with a little randomness so that equal hosts rotate
    QList<QPair<double, const HostEntry *>> scored;
    for (const HostEntry *e : cands) scored << qMakePair(e->score(now) + QRandomGenerator::global()->bounded(10.0), e);
    std::sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    QList<HostPort> out;
    for (const auto &s : scored) {
        if (out.size() >= n) break;
        out << s.second->addr;
    }
    return out;
}

QList<HostPort> HostCache::goodHosts(int n) const
{
    QList<const HostEntry *> l;
    for (const HostEntry &e : m_hosts) if (e.lastSuccess > 0 || e.failures == 0) l << &e;
    std::sort(l.begin(), l.end(), [](const HostEntry *a, const HostEntry *b) {
        return qMax(a->lastSuccess, a->lastSeen) > qMax(b->lastSuccess, b->lastSeen);
    });
    QList<HostPort> out;
    for (const HostEntry *e : l) { if (out.size() >= n) break; if (isRoutableIp(e->addr.ip)) out << e->addr; }
    return out;
}

const HostEntry *HostCache::find(const HostPort &h) const
{
    auto it = m_hosts.constFind(h.key());
    return it == m_hosts.constEnd() ? nullptr : &*it;
}

int HostCache::usableCount(qint64 now) const
{
    if (!now) now = nowSecs();
    int n = 0;
    for (const HostEntry &e : m_hosts) if (e.retryAfter <= now) ++n;
    return n;
}

void HostCache::trim()
{
    if (m_hosts.size() <= MaxHosts) return;
    const qint64 now = nowSecs();
    QList<HostEntry> l = m_hosts.values();
    std::sort(l.begin(), l.end(), [now](const HostEntry &a, const HostEntry &b) { return a.score(now) > b.score(now); });
    m_hosts.clear();
    for (const HostEntry &e : l) {
        if (m_hosts.size() >= MaxHosts && !e.isStatic) continue;
        m_hosts.insert(e.addr.key(), e);
    }
    m_dirty = true;
}

void HostCache::setBuiltinCaches(const QStringList &gwcs, const QStringList &uhcs)
{
    for (const QString &u : gwcs) addGwc(u);
    for (const QString &u : uhcs) addUhc(u);
    for (CacheEntry &c : m_gwcs) c.builtin = gwcs.contains(c.url);
    for (CacheEntry &c : m_uhcs) c.builtin = uhcs.contains(c.url);
}

static void addCache(QList<CacheEntry> &list, const QString &url)
{
    if (url.isEmpty() || url.size() > 300 || list.size() >= 100) return;
    for (const CacheEntry &c : list) if (c.url == url) return;
    CacheEntry c;
    c.url = url;
    list << c;
}
void HostCache::addGwc(const QString &url) { addCache(m_gwcs, url.trimmed()); m_dirty = true; }
void HostCache::addUhc(const QString &hp) { addCache(m_uhcs, hp.trimmed()); m_dirty = true; }

static CacheEntry *nextCache(QList<CacheEntry> &list, qint64 now, int minInterval)
{
    CacheEntry *best = nullptr;
    double bestScore = -1e9;
    for (int i = 0; i < list.size(); ++i) {
        CacheEntry &c = list[i];
        if (c.lastAttempt && now - c.lastAttempt < minInterval) continue;
        if (c.failures >= 3 && now - c.lastAttempt < 6 * 3600) continue;   // dead for now
        // earlier (built-in, best known) entries first; a little randomness to rotate equals
        double s = c.successes * 2.0 - c.failures * 5.0 + double(list.size() - i) * 0.3 +
                   QRandomGenerator::global()->bounded(1.0);
        if (c.lastSuccess) s += 10;
        if (s > bestScore) { bestScore = s; best = &c; }
    }
    return best;
}
CacheEntry *HostCache::nextGwc(qint64 now, int minIntervalSec) { return nextCache(m_gwcs, now, minIntervalSec); }
CacheEntry *HostCache::nextUhc(qint64 now, int minIntervalSec) { return nextCache(m_uhcs, now, minIntervalSec); }

} // namespace mx::gnut
