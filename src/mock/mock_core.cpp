// SPDX-License-Identifier: GPL-3.0-or-later
#include "mock/mock_core.h"

#include <climits>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaMethod>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>

#include "mock/mock_data.h"
#include "mx/download_order.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "mx/trash.h"

namespace mx {

namespace {

// ----------------------------------------------------------------------------------- settings
class MockSettings : public SimpleSettings {
public:
    MockSettings(const QString &profileDir, QObject *parent) : SimpleSettings(parent), m_dir(profileDir)
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(500);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] { sync(); });
        if (!m_dir.isEmpty()) {
            QFile f(QDir(m_dir).filePath(QStringLiteral("settings.json")));
            if (f.open(QIODevice::ReadOnly)) {
                const QVariantMap json = QJsonDocument::fromJson(f.readAll()).object().toVariantMap();
                QVariantMap vals;
                for (auto it = json.begin(); it != json.end(); ++it)
                    vals.insert(it.key(), settingFromJson(findSetting(it.key()), it.value()));
                loadValues(vals);
            }
        }
    }
    void sync() override
    {
        m_timer.stop();
        if (m_dir.isEmpty())
            return;
        QDir().mkpath(m_dir);
        QVariantMap out;
        const QVariantMap vals = storedValues();
        for (auto it = vals.begin(); it != vals.end(); ++it)
            out.insert(it.key(), settingToJson(findSetting(it.key()), it.value()));
        QSaveFile f(QDir(m_dir).filePath(QStringLiteral("settings.json")));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(QJsonObject::fromVariantMap(out)).toJson(QJsonDocument::Indented));
            f.commit();
        }
    }

protected:
    void scheduleSave() override
    {
        if (!m_dir.isEmpty())
            m_timer.start();
    }

private:
    QString m_dir;
    QTimer m_timer;
};

// ----------------------------------------------------------------------------------- network
class MockNetwork : public NetworkService {
public:
    MockNetwork(Settings *s, bool frozen, QObject *parent) : NetworkService(parent), m_settings(s), m_frozen(frozen)
    {
        m_nets = mock::networks();
        m_servers = mock::servers();
        if (m_settings->s(keys::identity::ScreenName).isEmpty())
            m_settings->setValue(keys::identity::ScreenName, QStringLiteral("OctoMXUser_60512"));
        // server messages: the MOTD of the connected eD2k server is there from the start; without
        // --demo-frozen a new line arrives every 40 s
        const QDateTime t0 = frozen ? QDateTime(QDate(2026, 11, 20), QTime(18, 0), QTimeZone::UTC) : QDateTime::currentDateTimeUtc();
        for (const ServerEntry &e : std::as_const(m_servers))
            if (e.net == Net::ED2K && e.state == ConnState::Connected) {
                const char *motd[] = {"Welcome to %1!", "This server does not index copyrighted files.",
                                      "Server version 17.15 - 512341 users, 98123456 files",
                                      "Please share what you download.  Your LowID/HighID status: HighID"};
                int k = 0;
                for (const char *line : motd)
                    m_messages << ServerMessage{Net::ED2K, e.id, e.name,
                                                QString::fromLatin1(line).replace(QLatin1String("%1"), e.name), t0.addSecs(k++)};
                break;
            }
        if (!frozen) {
            auto *t = new QTimer(this);
            t->setInterval(40000);
            connect(t, &QTimer::timeout, this, [this] {
                for (const ServerEntry &e : std::as_const(m_servers))
                    if (e.net == Net::ED2K && e.state == ConnState::Connected) {
                        postMessage(ServerMessage{Net::ED2K, e.id, e.name,
                                                  tr("Server load: %1 users online").arg(500000 + int(QDateTime::currentSecsSinceEpoch() % 30000)),
                                                  QDateTime::currentDateTimeUtc()});
                        break;
                    }
            });
            t->start();
        }
    }
    void postMessage(const ServerMessage &m)
    {
        m_messages << m;
        while (m_messages.size() > 200)
            m_messages.removeFirst();
        emit serverMessageReceived(m);
    }
    QList<ServerMessage> serverMessages(Net n) const override
    {
        QList<ServerMessage> out;
        for (const ServerMessage &m : m_messages)
            if (n == Net::Unknown || m.net == n)
                out << m;
        return out;
    }
    bool editServer(Net n, const QString &id, const QString &address, const QString &name) override
    {
        for (ServerEntry &e : m_servers)
            if (e.net == n && e.id == id) {
                e.address = address;
                e.id = address;
                if (!name.isEmpty())
                    e.name = name;
                emit serversChanged(n);
                return true;
            }
        return false;
    }
    int importServerList(Net n, const QString &path) override
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return -1;
        const QByteArray data = f.readAll();
        QList<QPair<QString, QString>> found;   // address, name
        if (!data.isEmpty() && (quint8(data.at(0)) == 0x0E || quint8(data.at(0)) == 0xE0)) {
            if (!parseServerMet(data, &found))
                return -1;
        } else {
            for (const QByteArray &raw : data.split('\n')) {
                const QString line = QString::fromUtf8(raw).trimmed();
                if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                    continue;
                if (line.startsWith(QLatin1String("ed2k://|server|"), Qt::CaseInsensitive)) {
                    const QStringList p = line.split(QLatin1Char('|'));
                    if (p.size() > 3)
                        found << qMakePair(p.at(2) + QLatin1Char(':') + p.at(3), QString());
                    continue;
                }
                const QString addr = line.section(QLatin1Char(' '), 0, 0);
                if (!addr.contains(QLatin1Char(':')))
                    return -1;   // not a server list
                found << qMakePair(addr, line.section(QLatin1Char(' '), 1));
            }
        }
        int added = 0;
        for (const auto &a : found) {
            bool known = false;
            for (const ServerEntry &e : std::as_const(m_servers))
                known = known || (e.net == n && e.id == a.first);
            if (known)
                continue;
            ServerEntry e;
            e.net = n;
            e.id = e.address = a.first;
            e.name = a.second.isEmpty() ? a.first : a.second;
            m_servers << e;
            ++added;
        }
        if (added)
            emit serversChanged(n);
        return added;
    }
    // eD2k server.met: 0x0E/0xE0, uint32 count, per server: uint32 ip, uint16 port, uint32 tag count, tags
    static bool parseServerMet(const QByteArray &d, QList<QPair<QString, QString>> *out)
    {
        int p = 1;
        auto need = [&](int n) { return p + n <= d.size(); };
        auto u8 = [&]() { return quint8(d.at(p++)); };
        auto u16 = [&]() { quint16 v = quint8(d.at(p)) | quint8(d.at(p + 1)) << 8; p += 2; return v; };
        auto u32 = [&]() { quint32 v = quint32(quint8(d.at(p))) | quint32(quint8(d.at(p + 1))) << 8 |
                                       quint32(quint8(d.at(p + 2))) << 16 | quint32(quint8(d.at(p + 3))) << 24; p += 4; return v; };
        if (!need(4))
            return false;
        const quint32 count = u32();
        for (quint32 i = 0; i < count; ++i) {
            if (!need(10))
                return false;
            const quint32 ip = u32();
            const quint16 port = u16();
            const quint32 tags = u32();
            QString name;
            for (quint32 t = 0; t < tags; ++t) {
                if (!need(1))
                    return false;
                quint8 type = u8();
                quint8 id = 0;
                if (type & 0x80) {
                    type &= 0x7f;
                    if (!need(1))
                        return false;
                    id = u8();
                } else {
                    if (!need(2))
                        return false;
                    const quint16 nl = u16();
                    if (!need(nl))
                        return false;
                    if (nl == 1)
                        id = quint8(d.at(p));
                    p += nl;
                }
                int len = 0;
                if (type == 2) {
                    if (!need(2))
                        return false;
                    len = u16();
                } else if (type >= 0x11 && type <= 0x20) {
                    len = type - 0x10;
                } else if (type == 3 || type == 4) {
                    len = 4;
                } else if (type == 8) {
                    len = 1;
                } else if (type == 9) {
                    len = 2;
                } else {
                    return false;
                }
                if (!need(len))
                    return false;
                if (id == 0x01 && (type == 2 || type >= 0x11))
                    name = QString::fromUtf8(d.mid(p, len));
                p += len;
            }
            const QString addr = QStringLiteral("%1.%2.%3.%4:%5").arg(ip & 0xff).arg((ip >> 8) & 0xff).arg((ip >> 16) & 0xff)
                                     .arg(ip >> 24).arg(port);
            *out << qMakePair(addr, name);
        }
        return true;
    }
    void pingUser(const UserRef &u) override
    {
        // overlay / eD2k / Gnutella peers answer; swarm sources and unknown users do not
        const int ms = (u.net == Net::BitTorrent || u.net == Net::Unknown) ? -1 : 18 + int(qHash(u.key()) % 260);
        QTimer::singleShot(m_frozen ? 0 : 400, this, [this, u, ms] { emit userPinged(u, ms); });
    }
    QList<NetworkStatus> networks() const override { return m_nets; }
    NetworkStatus status(Net n) const override
    {
        for (const NetworkStatus &s : m_nets)
            if (s.net == n)
                return s;
        return NetworkStatus();
    }
    ConnState overallState() const override
    {
        bool connecting = false;
        for (const NetworkStatus &s : m_nets) {
            if (s.state == ConnState::Connected)
                return ConnState::Connected;
            if (s.state == ConnState::Connecting)
                connecting = true;
        }
        return connecting ? ConnState::Connecting : ConnState::Disconnected;
    }
    QString overallHeadline() const override
    {
        switch (overallState()) {
        case ConnState::Connected: return tr("Ready");
        case ConnState::Connecting: return tr("Finding network connections");
        default: return tr("Not connected");
        }
    }
    QString overallDetail() const override
    {
        switch (overallState()) {
        case ConnState::Connected: return tr("You can search and download now.");
        case ConnState::Connecting: return tr("Connecting: searching and downloading work in a moment...");
        default: return tr("Click Connect to join the network.");
        }
    }
    // per-network traffic (bandwidth model); no signal, the Bandwidth page reads it every second
    void setRates(Net n, int down, int up)
    {
        for (NetworkStatus &s : m_nets)
            if (s.net == n) {
                s.rateDownBps = down;
                s.rateUpBps = up;
            }
    }
    void setState(Net n, ConnState st, const QString &text)
    {
        for (NetworkStatus &s : m_nets)
            if (s.net == n) {
                s.state = st;
                s.statusText = text;
                emit networkChanged(s);
            }
        emit overallChanged();
        emit summaryChanged();
    }
    void connectAll() override
    {
        const QList<NetworkStatus> base = mock::networks();
        for (NetworkStatus &s : m_nets)
            if (isEnabled(s.net))
                setState(s.net, ConnState::Connecting, tr("Connecting..."));
        auto finish = [this, base] {
            for (const NetworkStatus &b : base)
                if (isEnabled(b.net))
                    setState(b.net, b.state, b.statusText);
        };
        if (m_frozen)
            finish();
        else
            QTimer::singleShot(1500, this, finish);
    }
    void disconnectAll() override
    {
        for (NetworkStatus &s : m_nets)
            setState(s.net, isEnabled(s.net) ? ConnState::Disconnected : ConnState::Disabled, tr("Not connected"));
    }
    void setEnabled(Net n, bool on) override
    {
        m_settings->setValue(keys::enabledKey(n), on);
        setState(n, on ? ConnState::Disconnected : ConnState::Disabled, on ? tr("Not connected") : tr("Disabled"));
    }
    bool isEnabled(Net n) const override
    {
        const QString k = keys::enabledKey(n);
        return k.isEmpty() ? false : m_settings->value(k).toBool();
    }
    void connectNetwork(Net n) override
    {
        setState(n, ConnState::Connecting, tr("Connecting..."));
        const NetworkStatus b = [n] {
            for (const NetworkStatus &s : mock::networks())
                if (s.net == n)
                    return s;
            return NetworkStatus();
        }();
        if (m_frozen)
            setState(n, b.state, b.statusText);
        else
            QTimer::singleShot(1200, this, [this, n, b] { setState(n, b.state == ConnState::Connecting ? ConnState::Connected : b.state, b.statusText); });
    }
    void disconnectNetwork(Net n) override { setState(n, ConnState::Disconnected, tr("Not connected")); }
    QList<ServerEntry> servers(Net n) const override
    {
        if (n == Net::Unknown)
            return m_servers;
        QList<ServerEntry> l;
        for (const ServerEntry &e : m_servers)
            if (e.net == n)
                l << e;
        return l;
    }
    void addServer(Net n, const QString &address, const QString &name) override
    {
        ServerEntry e;
        e.net = n;
        e.id = address;
        e.address = address;
        e.name = name.isEmpty() ? address : name;
        e.isStatic = true;
        m_servers << e;
        emit serversChanged(n);
    }
    void removeServer(Net n, const QString &id) override
    {
        for (int i = 0; i < m_servers.size(); ++i)
            if (m_servers[i].net == n && m_servers[i].id == id) {
                m_servers.removeAt(i);
                break;
            }
        emit serversChanged(n);
    }
    void setServerState(Net n, const QString &id, ConnState st)
    {
        for (ServerEntry &e : m_servers)
            if (e.net == n && e.id == id)
                e.state = st;
        emit serversChanged(n);
    }
    void connectServer(Net n, const QString &id) override
    {
        setServerState(n, id, ConnState::Connecting);
        if (m_frozen)
            setServerState(n, id, ConnState::Connected);
        else
            QTimer::singleShot(1000, this, [this, n, id] { setServerState(n, id, ConnState::Connected); });
    }
    void disconnectServer(Net n, const QString &id) override { setServerState(n, id, ConnState::Disconnected); }
    QString screenName() const override { return m_settings->s(keys::identity::ScreenName); }
    void setScreenName(const QString &name) override
    {
        m_settings->setValue(keys::identity::ScreenName, name);
        emit screenNameChanged(name);
    }
    // [ui-e] the engine-4 fields: S= is the secondary connections, the known sources stay separate
    NetworkSummary summary() const override
    {
        NetworkSummary s = NetworkService::summary();
        s.secondary = s.sources;
        s.knownSources = s.sources;
        return s;
    }
    QString titleSummary() const override
    {
        switch (overallState()) {
        case ConnState::Connected:
            return m_settings->b(keys::identity::PrimaryConnection) ? QStringLiteral("Online P=5 S=12 spm=4284")
                                                                     : QStringLiteral("Online 52:39");
        case ConnState::Connecting: return QStringLiteral("Connecting");
        default: return QStringLiteral("Offline");
        }
    }

private:
    Settings *m_settings;
    bool m_frozen;
    QList<NetworkStatus> m_nets;
    QList<ServerEntry> m_servers;
    QList<ServerMessage> m_messages;
};

// ----------------------------------------------------------------------------------- search
class MockSearch : public SearchService {
public:
    MockSearch(Settings *st, bool frozen, QObject *parent) : SearchService(parent), m_settings(st), m_frozen(frozen)
    {
        m_history = {QStringLiteral("velorine"), QStringLiteral("linux"), QStringLiteral("gooseberry"),
                     QStringLiteral("big buck bunny")};
    }
    quint32 startSearch(SearchQuery q) override
    {
        q.id = ++m_next;
        QList<int> batch;
        QList<SearchHit> hits = mock::searchHits(q, &batch);
        // network / type filters
        QList<SearchHit> filtered;
        QList<int> fb;
        for (int i = 0; i < hits.size(); ++i) {
            SearchHit h = hits.at(i);
            if (!hitMatchesFileType(q, h))   // [ui-f] the core's rule (type, extensions, bitrate)
                continue;
            // search hygiene settings, like the core: flag (or not), and hide flagged hits on request
            if (!h.suspect.isEmpty() && !m_settings->b(keys::search::FlagSuspects))
                h.suspect.clear();
            if (!h.suspect.isEmpty() && m_settings->b(keys::search::HideSuspects))
                continue;
            if (!q.networks.isEmpty()) {
                QList<SearchSource> keep;
                for (const SearchSource &s : h.sources)
                    if (q.networks.contains(s.net))
                        keep << s;
                if (keep.isEmpty())
                    continue;
                h.sources = keep;
            }
            filtered << h;
            fb << batch.value(i);
        }
        Search s;
        s.query = q;
        s.running = true;
        m_searches.insert(q.id, s);
        const QString text = q.text.simplified();
        if (!text.isEmpty()) {
            m_history.removeAll(text);
            m_history.prepend(text);
        }
        const quint32 id = q.id;
        if (m_frozen) {
            QTimer::singleShot(0, this, [this, id, filtered] { deliver(id, filtered, true); });
        } else {
            for (int b = 0; b < 4; ++b) {
                QList<SearchHit> part;
                for (int i = 0; i < filtered.size(); ++i)
                    if (fb.value(i) == b)
                        part << filtered.at(i);
                QTimer::singleShot(400 + b * 700, this, [this, id, part, b] { deliver(id, part, b == 3); });
            }
            // a late update merging one more source into the first hit (exercises the update path)
            if (!filtered.isEmpty()) {
                SearchHit upd = filtered.first();
                SearchSource extra = upd.sources.first();
                extra.user = mock::user(Net::Kad, QStringLiteral("kadpeer_%1").arg(id));
                extra.net = Net::Kad;
                extra.connection = QStringLiteral("DSL");
                upd.sources << extra;
                QTimer::singleShot(3200, this, [this, id, upd] { deliver(id, {upd}, false); });
            }
            // once per search: two rows turn out to be the same file
            if (filtered.size() > 20) {
                const QString gone = filtered.at(filtered.size() - 1).key;
                const QString keep = filtered.at(filtered.size() - 2).key;
                QTimer::singleShot(3600, this, [this, id, gone, keep] { mergeRows(id, gone, keep); });
            }
        }
        return id;
    }
    void deliver(quint32 id, const QList<SearchHit> &part, bool last)
    {
        auto it = m_searches.find(id);
        if (it == m_searches.end() || !it->running)
            return;
        for (const SearchHit &h : part) {
            bool found = false;
            for (SearchHit &e : it->hits)
                if (e.key == h.key) {
                    e = h;
                    found = true;
                    break;
                }
            if (!found)
                it->hits << h;
        }
        if (!part.isEmpty())
            emit hitsChanged(id, part);
        emit searchProgress(id, Net::Overlay, tr("%1 results").arg(it->hits.size()));
        if (last) {
            it->running = false;
            emit searchFinished(id);
        }
    }
    void requestBundleFiles(quint32 id, const QString &hitKey) override
    {
        auto it = m_searches.find(id);
        if (it == m_searches.end())
            return;
        for (SearchHit &h : it->hits) {
            if (h.key != hitKey || !h.isBundle)
                continue;
            if (h.files.isEmpty()) {
                for (int k = 0; k < 5; ++k)
                    h.files << BundleFile{QStringLiteral("%1/part%2.bin").arg(h.fileName).arg(k + 1), h.size / 5, k};
                h.files << BundleFile{QStringLiteral(".pad/12345"), 12345, 5};   // hidden by the UI
            }
            const SearchHit copy = h;
            if (m_frozen)
                emit hitsChanged(id, {copy});
            else
                QTimer::singleShot(300, this, [this, id, copy] { emit hitsChanged(id, {copy}); });
            return;
        }
    }
    // the core merged two rows: `gone` is absorbed into `survivor` (exercises hitsRemoved)
    void mergeRows(quint32 id, const QString &gone, const QString &survivorKey)
    {
        auto it = m_searches.find(id);
        if (it == m_searches.end())
            return;
        SearchHit absorbed, survivor;
        int si = -1;
        for (int i = 0; i < it->hits.size(); ++i) {
            if (it->hits[i].key == gone)
                absorbed = it->hits[i];
            if (it->hits[i].key == survivorKey)
                si = i;
        }
        if (absorbed.key.isEmpty() || si < 0)
            return;
        it->hits[si].sources << absorbed.sources;
        survivor = it->hits[si];
        for (int i = 0; i < it->hits.size(); ++i)
            if (it->hits[i].key == gone) {
                it->hits.removeAt(i);
                break;
            }
        emit hitsRemoved(id, {gone});
        emit hitsChanged(id, {survivor});
    }
    void stopSearch(quint32 id) override
    {
        auto it = m_searches.find(id);
        if (it != m_searches.end() && it->running) {
            it->running = false;
            emit searchFinished(id);
        }
    }
    void closeSearch(quint32 id) override { m_searches.remove(id); }
    // Ping: overlay / eD2k / Gnutella / Kad users answer with a stable pseudo round trip, every
    // 7th of them times out ("?"), swarm sources (BitTorrent) cannot be pinged.
    void pingHits(quint32 id, const QStringList &hitKeys) override
    {
        auto it = m_searches.find(id);
        if (it == m_searches.end())
            return;
        QList<SearchHit> changed;
        for (SearchHit &h : it->hits) {
            if (!hitKeys.contains(h.key))
                continue;
            for (SearchSource &src : h.sources) {
                const uint hsh = qHash(src.user.key() + h.key);
                src.pingMs = (src.net == Net::BitTorrent || hsh % 7 == 0) ? -1 : int(12 + hsh % 480);
                src.extra.insert(QStringLiteral("pinged"), true);
            }
            changed << h;
        }
        if (changed.isEmpty())
            return;
        if (m_frozen)
            emit hitsChanged(id, changed);
        else
            QTimer::singleShot(600, this, [this, id, changed] {
                if (m_searches.contains(id))
                    emit hitsChanged(id, changed);
            });
    }
    // Search by hash: one row for the file; its sources grow as "networks answer"
    quint32 searchHash(const QString &linkOrHash) override
    {
        SearchQuery q;
        q.id = ++m_next;
        q.text = linkOrHash;
        SearchHit h;
        h.queryId = q.id;
        h.fileName = QStringLiteral("file");
        QString hash = linkOrHash;
        if (linkOrHash.startsWith(QLatin1String("ed2k://"), Qt::CaseInsensitive)) {
            const QStringList p = linkOrHash.split(QLatin1Char('|'));
            h.fileName = QUrl::fromPercentEncoding(p.value(2).toUtf8());
            h.size = p.value(3).toLongLong();
            hash = p.value(4);
            h.hashes.ed2k = QByteArray::fromHex(hash.toLatin1());
        } else if (linkOrHash.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)) {
            const QUrl u(linkOrHash);
            for (const QString &kv : u.query(QUrl::FullyDecoded).split(QLatin1Char('&'))) {
                if (kv.startsWith(QLatin1String("dn=")))
                    h.fileName = kv.mid(3);
                else if (kv.startsWith(QLatin1String("xl=")))
                    h.size = kv.mid(3).toLongLong();
                else if (kv.startsWith(QLatin1String("xt=urn:btih:"), Qt::CaseInsensitive)) {
                    hash = kv.mid(12);
                    h.hashes.btih = QByteArray::fromHex(hash.toLatin1());
                }
            }
        } else {
            hash = linkOrHash.section(QLatin1Char(':'), -1);
            h.fileName = tr("Hash %1").arg(hash.left(12));
            if (hash.size() == 32)
                h.hashes.ed2k = QByteArray::fromHex(hash.toLatin1());
            else
                h.hashes.sha1 = QByteArray::fromHex(hash.toLatin1());
        }
        h.key = QStringLiteral("hash:") + hash.toLower();
        if (h.size <= 0)
            h.size = 4404019;
        h.type = mediaTypeForFileName(h.fileName);
        const struct { Net net; const char *user; const char *conn; } srcs[] = {
            {Net::ED2K, "eMule_Hans", "VDSL"}, {Net::Kad, "kadpeer_70707", "Cable"}, {Net::Gnutella, "198.51.100.61:6346", "FTTH"}};
        QList<SearchSource> all;
        for (const auto &x : srcs) {
            SearchSource src;
            src.net = x.net;
            src.user = mock::user(x.net, QString::fromLatin1(x.user));
            src.connection = QString::fromLatin1(x.conn);
            src.freeSlots = 1;
            src.totalSlots = 2;
            all << src;
        }
        Search srch;
        srch.query = q;
        srch.running = true;
        m_searches.insert(q.id, srch);
        const quint32 id = q.id;
        if (m_frozen) {
            h.sources = all;
            QTimer::singleShot(0, this, [this, id, h] { deliver(id, {h}, true); });
        } else {
            for (int k = 0; k < all.size(); ++k) {
                SearchHit part = h;
                part.sources = all.mid(0, k + 1);
                QTimer::singleShot(500 + 900 * k, this, [this, id, part, k, n = int(all.size())] { deliver(id, {part}, k == n - 1); });
            }
        }
        return id;
    }
    bool isRunning(quint32 id) const override { return m_searches.value(id).running; }
    QList<SearchHit> hits(quint32 id) const override { return m_searches.value(id).hits; }
    QStringList history() const override { return m_history; }
    void clearHistory() override { m_history.clear(); }

private:
    struct Search {
        SearchQuery query;
        QList<SearchHit> hits;
        bool running = false;
    };
    Settings *m_settings;
    bool m_frozen;
    quint32 m_next = 0;
    QHash<quint32, Search> m_searches;
    QStringList m_history;
};

// ----------------------------------------------------------------------------------- transfers
class MockTransfers : public TransferService {
public:
    MockTransfers(Settings *s, const QString &profile, bool frozen, QObject *parent)
        : TransferService(parent), m_settings(s), m_profile(profile), m_frozen(frozen)
    {
        m_downloads = mock::downloads();
        for (const TransferInfo &t : std::as_const(m_downloads)) {   // [ui-i] the maps of the demo downloads
            const mock::PieceSim sim = mock::demoPieces(t);
            if (sim.isValid())
                m_pieces.insert(t.id, sim);
        }
        m_uploads = mock::uploads();
        // [ui-e] the demo downloads live in the profile's incoming folder; the completed ones exist there
        // as small placeholder files, so Open File / Open Containing Folder work in the demo
        if (!profile.isEmpty()) {
            const QString in = incomingDir();
            for (TransferInfo &t : m_downloads) {
                t.savePath = QDir(in).filePath(t.name);
                if ((t.state == TransferState::Completed || t.state == TransferState::Seeding) &&
                    QDir(in).absolutePath().startsWith(QDir(profile).absolutePath()) && QDir().mkpath(in)) {
                    QFile f(t.savePath);
                    if (!f.exists() && f.open(QIODevice::WriteOnly))
                        f.write("OctoMX demo placeholder\n");
                }
            }
        }
        int pos = 0;
        for (TransferInfo &u : m_uploads)
            if (u.state == TransferState::Queued)
                u.queuePosition = ++pos;
        if (!frozen) {
            auto *t = new QTimer(this);
            t->setInterval(1000);
            connect(t, &QTimer::timeout, this, [this] { tick(); });
            t->start();
        }
    }
    TransferInfo *find(const QString &id)
    {
        for (TransferInfo &t : m_downloads)
            if (t.id == id)
                return &t;
        for (TransferInfo &t : m_uploads)
            if (t.id == id)
                return &t;
        return nullptr;
    }
    QString addDownload(TransferInfo t)
    {
        t.id = QStringLiteral("n%1").arg(++m_next);
        t.state = TransferState::FindingSources;
        t.statusText = QStringLiteral("Finding sources...");
        t.added = QDateTime::currentDateTimeUtc();
        t.savePath = QDir(incomingDir()).filePath(t.name);
        m_downloads << t;
        emit transferAdded(t);
        const QString id = t.id;
        auto start = [this, id] {
            if (TransferInfo *x = find(id)) {
                if (x->state != TransferState::FindingSources)
                    return;
                x->state = TransferState::Transferring;
                x->statusText.clear();
                x->rateDownBps = 25000 + int(qHash(id) % 40000);
                for (TransferSource &s : x->sources)
                    s.statusText.clear();
                emit transferChanged(*x);
            }
        };
        if (m_frozen)
            start();
        else
            QTimer::singleShot(2000, this, start);
        return id;
    }
    QString download(const DownloadRequest &req) override
    {
        for (const TransferInfo &t : m_downloads)
            if (!req.hashes.isEmpty() && t.hashes.matches(req.hashes))
                return t.id;   // join the running download
        TransferInfo t;
        t.name = req.name.isEmpty() ? QStringLiteral("download") : req.name;
        t.size = req.size;
        t.hashes = req.hashes;
        for (const SearchSource &s : req.sources) {
            TransferSource ts;
            ts.net = s.net;
            ts.user = s.user.isValid() ? s.user : mock::user(s.net, s.connection.isEmpty() ? netName(s.net) : s.connection);
            ts.connection = s.connection;
            ts.statusText = QStringLiteral("Waiting for Network Reply...");
            if (s.net != Net::BitTorrent)
                ts.id = QStringLiteral("s%1").arg(++m_nextSource);
            t.sources << ts;
            if (!t.networks.contains(s.net))
                t.networks << s.net;
        }
        t.sourcesTotal = t.sources.size();
        return addDownload(t);
    }
    QString downloadHit(const SearchHit &hit, const QString &saveDir) override
    {
        DownloadRequest r;
        r.name = hit.fileName;
        r.size = hit.size;
        r.hashes = hit.hashes;
        r.sources = hit.sources;
        r.saveDir = saveDir;
        return download(r);
    }
    QString downloadUri(const QString &uri, const QString &saveDir) override
    {
        TransferInfo t;
        QString name = uri;
        const QUrl u(uri);
        if (uri.startsWith(QLatin1String("magnet:"))) {
            const QString q = u.query(QUrl::FullyDecoded);
            for (const QString &kv : q.split(QLatin1Char('&')))
                if (kv.startsWith(QLatin1String("dn=")))
                    name = kv.mid(3);
            if (name == uri)
                name = QStringLiteral("magnet link");
            t.isBundle = false;
        } else if (uri.startsWith(QLatin1String("ed2k://"))) {
            const QStringList parts = uri.split(QLatin1Char('|'));
            if (parts.size() > 3) {
                name = QUrl::fromPercentEncoding(parts.at(2).toUtf8());
                t.size = parts.at(3).toLongLong();
            }
        } else {
            name = QFileInfo(u.isLocalFile() ? u.toLocalFile() : uri).fileName();
            if (name.isEmpty())
                name = uri;
        }
        if (name.endsWith(QLatin1String(".torrent")))
            name.chop(8);
        t.name = name;
        Q_UNUSED(saveDir);
        return addDownload(t);
    }
    QList<TransferInfo> downloads() const override { return m_downloads; }
    QList<TransferInfo> uploads() const override { return m_uploads; }
    TransferInfo transfer(const QString &id) const override
    {
        for (const TransferInfo &t : m_downloads)
            if (t.id == id)
                return t;
        for (const TransferInfo &t : m_uploads)
            if (t.id == id)
                return t;
        return TransferInfo();
    }
    void setState(const QString &id, TransferState st, const QString &text)
    {
        if (TransferInfo *t = find(id)) {
            t->state = st;
            t->statusText = text;
            if (st != TransferState::Transferring)
                t->rateDownBps = 0;
            emit transferChanged(*t);
        }
    }
    void pause(const QString &id) override { setState(id, TransferState::Paused, QStringLiteral("Paused")); }
    void resume(const QString &id) override
    {
        if (TransferInfo *t = find(id)) {
            if (t->state != TransferState::Paused && t->state != TransferState::Error && t->state != TransferState::TimedOut)
                return;
            t->state = TransferState::Transferring;
            t->statusText.clear();
            t->rateDownBps = 30000;
            emit transferChanged(*t);
        }
    }
    void pauseAll() override
    {
        for (const TransferInfo &t : m_downloads)
            if (t.state == TransferState::Transferring || t.state == TransferState::Queued)
                pause(t.id);
    }
    void resumeAll() override
    {
        for (const TransferInfo &t : m_downloads)
            if (t.state == TransferState::Paused)
                resume(t.id);
    }
    void cancel(const QString &id, bool deleteData) override
    {
        for (int i = 0; i < m_downloads.size(); ++i)
            if (m_downloads[i].id == id) {
                m_downloads.removeAt(i);
                // a manual resume used its file in place: "delete data" deletes it (only files inside
                // the profile, so a demo can never delete anything else)
                const QString file = m_resumedFrom.take(id);
                if (deleteData && !file.isEmpty() && !m_profile.isEmpty() &&
                    QFileInfo(file).absoluteFilePath().startsWith(QDir(m_profile).absolutePath() + QLatin1Char('/')))
                    QFile::remove(file);
                emit transferRemoved(id, false);
                return;
            }
    }
    // ---- [ui-j] removing with or without the file. The demo's data: the placeholder files of the
    // finished demo downloads in the profile's incoming folder, a file resumed in place; anything
    // outside the profile is never touched (reported as not deleted).
    QString dataPathOf(const TransferInfo &t) const
    {
        const bool finished = t.state == TransferState::Completed || t.state == TransferState::Seeding;
        if (finished)
            return t.savePath;
        const QString own = m_resumedFrom.value(t.id);
        return own.isEmpty() ? QDir(incompleteDir()).filePath(QStringLiteral("__INCOMPLETE___") + t.name) : own;
    }
    bool insideProfile(const QString &path) const
    {
        return !m_profile.isEmpty() && QFileInfo(path).absoluteFilePath().startsWith(QDir(m_profile).absolutePath() + QLatin1Char('/'));
    }
    DownloadDiskInfo diskInfo(const QString &id) const override
    {
        DownloadDiskInfo di;
        const TransferInfo *t = nullptr;
        for (const TransferInfo &x : m_downloads)
            if (x.id == id)
                t = &x;
        if (!t)
            return di;
        di.known = true;
        di.finished = t->state == TransferState::Completed || t->state == TransferState::Seeding;
        di.torrent = t->networks.contains(Net::BitTorrent) && t->networks.size() == 1;
        di.total = t->size;
        const QString p = dataPathOf(*t);
        if (!p.isEmpty() && (di.finished || t->done > 0 || QFileInfo::exists(p))) {
            di.paths << p;
            di.bytes = di.finished ? t->size : qMax<qint64>(t->done, QFileInfo(p).size());
            di.folder = QFileInfo(p).absolutePath();
        }
        if (!di.finished && m_resumedFrom.contains(id))
            di.inPlaceFile = m_resumedFrom.value(id);
        di.trash = trash::availableForAll(di.paths);
        return di;
    }
    void removeDownload(const QString &id, FileRemoval mode) override
    {
        const DownloadDiskInfo di = diskInfo(id);
        QString name;
        for (const TransferInfo &x : std::as_const(m_downloads))
            if (x.id == id)
                name = x.name;
        for (int i = 0; i < m_downloads.size(); ++i)
            if (m_downloads[i].id == id) {
                m_downloads.removeAt(i);
                m_resumedFrom.remove(id);
                emit transferRemoved(id, false);
                break;
            }
        if (mode == FileRemoval::Keep || !di.known)
            return;
        QStringList mine, outside;
        for (const QString &p : di.paths)
            (insideProfile(p) ? mine : outside) << p;
        trash::Result r = trash::dispose(mine, mode == FileRemoval::TrashOrDelete);
        for (const QString &p : std::as_const(outside))
            if (QFileInfo::exists(p)) {
                r.failed << p;
                r.errors << QStringLiteral("the demo core does not touch files outside its profile");
            }
        QTimer::singleShot(0, this, [this, id, name, r] { emit downloadFilesDeleted(id, name, r.trashed, r.deleted, r.failed, r.errors); });
    }
    // ---- glue-2 per-transfer actions
    void findMoreSources(const QString &id) override
    {
        TransferInfo *t = find(id);
        if (!t || t->isUpload)
            return;
        // one more source "answers" (a Kad peer), the others are asked again
        TransferSource s;
        s.net = Net::Kad;
        s.user = mock::user(Net::Kad, QStringLiteral("kadpeer_%1").arg(10000 + ++m_nextSource));
        s.connection = QStringLiteral("DSL");
        s.statusText = QStringLiteral("Waiting for Network Reply...");
        s.id = QStringLiteral("s%1").arg(m_nextSource);
        t->sources << s;
        t->sourcesTotal = qMax(t->sourcesTotal + 1, int(t->sources.size()));
        if (!t->networks.contains(Net::Kad))
            t->networks << Net::Kad;
        if (t->state == TransferState::Error || t->state == TransferState::TimedOut || t->state == TransferState::FindingSources) {
            t->state = TransferState::Connecting;
            t->statusText = QStringLiteral("Connecting to Remote User");
        }
        emit transferChanged(*t);
    }
    void enterQueue(const QString &id) override
    {
        TransferInfo *t = find(id);
        if (!t || t->isUpload)
            return;
        // reconnect every known source now: queued / timed-out ones get a fresh queue place
        int rank = 3;
        for (TransferSource &s : t->sources) {
            if (s.statusText.startsWith(QLatin1String("Busy")) || s.statusText.startsWith(QLatin1String("Timed")) ||
                s.statusText.startsWith(QLatin1String("Remotely Queued")))
                s.statusText = QStringLiteral("Remotely Queued %1").arg(rank++);
        }
        if (t->state == TransferState::Error || t->state == TransferState::TimedOut) {
            t->state = TransferState::RemotelyQueued;
            t->statusText = QStringLiteral("Remotely Queued 3");
        }
        emit transferChanged(*t);
    }
    void cancelSource(const QString &id, const QString &sourceId) override
    {
        TransferInfo *t = find(id);
        if (!t || sourceId.isEmpty())
            return;
        for (int k = 0; k < t->sources.size(); ++k)
            if (t->sources[k].id == sourceId) {
                t->sources.removeAt(k);
                emit transferChanged(*t);
                return;
            }
    }
    void setSourcePriority(const QString &id, const QString &sourceId, int priority) override
    {
        if (TransferInfo *t = find(id))
            for (TransferSource &s : t->sources)
                if (s.id == sourceId && !sourceId.isEmpty()) {
                    s.priority = qBound(-1, priority, 1);
                    emit transferChanged(*t);
                    return;
                }
    }
    bool startUploadNow(const QString &id) override
    {
        TransferInfo *t = find(id);
        // eD2k / overlay queues can grant a slot now; a Gnutella queue entry (PARQ) cannot
        if (!t || !t->isUpload || t->state != TransferState::Queued || t->peer.net == Net::Gnutella)
            return false;
        t->state = TransferState::Transferring;
        t->statusText.clear();
        t->queuePosition = 0;
        t->rateUpBps = 7200;
        emit transferChanged(*t);
        return true;
    }
    bool setUploadPriority(const QString &id, int priority) override
    {
        TransferInfo *t = find(id);
        if (!t || !t->isUpload || t->peer.net == Net::Gnutella)
            return false;
        t->priority = qBound(-1, priority, 1);
        emit transferChanged(*t);
        return true;
    }
    // ---- manual resume
    QString incompleteDir() const override
    {
        const QString s = m_settings->s(keys::transfers::IncompleteDir);
        if (!s.isEmpty())
            return s;
        return QDir(m_profile.isEmpty() ? QDir::tempPath() : m_profile).filePath(QStringLiteral("incomplete"));
    }
    static PartialFile partial(const QString &path, const QString &name, qint64 size, const QDateTime &modified, const QString &owner)
    {
        PartialFile p;
        p.path = path;
        p.name = name;
        p.size = size;
        p.modified = modified;
        p.downloadId = owner;
        return p;
    }
    QList<PartialFile> partialFiles() const override
    {
        QList<PartialFile> out;
        // real files in the incomplete folder (so a demo can resume a file of its own)
        const QFileInfoList real = QDir(incompleteDir()).entryInfoList(QDir::Files, QDir::Name);
        for (const QFileInfo &fi : real)
            out << partial(fi.absoluteFilePath(), fi.fileName(), fi.size(), fi.lastModified(), ownerOf(fi.absoluteFilePath()));
        // the demo's own: two left over from an "earlier session" and the part file of download d2
        const QString dir = incompleteDir();
        const QDateTime t = QDateTime(QDate(2026, 11, 19), QTime(21, 14), QTimeZone::UTC);
        const struct { const char *name; qint64 size; const char *owner; } demo[] = {
            {"__INCOMPLETE___Velorine - Velvet Engine.mp3", 1048576, ""},
            {"__INCOMPLETE___The Gooseberry Big Band - Rooftop Pigeon Stomp.mp3", 0, ""},
            {"__INCOMPLETE___Velorine - The Best Of Velorine - Entire Album.mp3", 51246858, "d2"},
        };
        for (const auto &d : demo) {
            const QString path = QDir(dir).filePath(QString::fromLatin1(d.name));
            if (QFileInfo::exists(path))
                continue;
            QString owner = QString::fromLatin1(d.owner);
            if (!owner.isEmpty() && !find(owner))
                owner.clear();
            if (owner.isEmpty())
                owner = ownerOf(path);
            out << partial(path, QString::fromLatin1(d.name), d.size, t, owner);
        }
        return out;
    }
    QString resumeDownload(const DownloadRequest &req, const QString &existingFile) override
    {
        if (existingFile.isEmpty() || !ownerOf(existingFile).isEmpty())
            return {};   // already used by a download
        qint64 have = -1;
        const QFileInfo fi(existingFile);
        if (fi.exists())
            have = fi.size();
        else
            for (const PartialFile &p : partialFiles())
                if (p.path == existingFile)
                    have = p.size;
        if (have < 0 || (req.size > 0 && have > req.size))
            return {};   // unreadable / larger than the download
        TransferInfo t;
        t.name = req.name.isEmpty() ? fi.fileName() : req.name;
        t.size = req.size > 0 ? req.size : qMax<qint64>(have, 1);
        t.done = have;
        t.hashes = req.hashes;
        for (const SearchSource &src : req.sources) {
            TransferSource ts;
            ts.net = src.net;
            ts.user = src.user.isValid() ? src.user : mock::user(src.net, netName(src.net));
            ts.connection = src.connection;
            ts.statusText = QStringLiteral("Waiting for Network Reply...");
            if (src.net != Net::BitTorrent)
                ts.id = QStringLiteral("s%1").arg(++m_nextSource);
            t.sources << ts;
            if (!t.networks.contains(src.net))
                t.networks << src.net;
        }
        const QString id = addDownload(t);
        m_resumedFrom.insert(id, existingFile);
        return id;
    }
    bool setDownloadTarget(const QString &id, const QString &dir, const QString &name) override
    {
        TransferInfo *t = find(id);
        if (!t || t->isUpload || transfersFinished(*t) || (!t->hashes.btih.isEmpty() && t->state == TransferState::Transferring))
            return false;
        const QString n = name.isEmpty() ? QFileInfo(t->savePath).fileName() : name;
        t->savePath = QDir(dir.isEmpty() ? QFileInfo(t->savePath).absolutePath() : dir).filePath(n.isEmpty() ? t->name : n);
        if (!name.isEmpty())
            t->name = name;
        emit transferChanged(*t);
        return true;
    }
    void retry(const QString &id) override
    {
        if (TransferInfo *t = find(id)) {
            t->state = TransferState::FindingSources;
            t->statusText = QStringLiteral("Finding sources...");
            emit transferChanged(*t);
            const QString tid = id;
            auto again = [this, tid] {
                if (TransferInfo *x = find(tid); x && x->state == TransferState::FindingSources) {
                    x->state = TransferState::Transferring;
                    x->statusText.clear();
                    x->rateDownBps = 20000;
                    emit transferChanged(*x);
                }
            };
            if (m_frozen)
                again();
            else
                QTimer::singleShot(1500, this, again);
        }
    }
    void setPriority(const QString &id, int priority) override
    {
        if (TransferInfo *t = find(id)) {
            t->priority = priority;
            emit transferChanged(*t);
        }
    }
    void clearCompleted() override
    {
        for (int i = m_downloads.size() - 1; i >= 0; --i)
            if (m_downloads[i].state == TransferState::Completed || m_downloads[i].state == TransferState::Cancelled) {
                const QString id = m_downloads[i].id;
                m_downloads.removeAt(i);
                emit transferRemoved(id, false);
            }
        for (int i = m_uploads.size() - 1; i >= 0; --i)
            if (m_uploads[i].state == TransferState::Completed || m_uploads[i].state == TransferState::Cancelled ||
                m_uploads[i].state == TransferState::TimedOut) {
                const QString id = m_uploads[i].id;
                m_uploads.removeAt(i);
                emit transferRemoved(id, true);
            }
    }
    void clearUpload(const QString &id) override
    {
        for (int i = 0; i < m_uploads.size(); ++i)
            if (m_uploads[i].id == id) {
                m_uploads.removeAt(i);
                emit transferRemoved(id, true);
                return;
            }
    }
    // [qa-1] per-download AFS / AEQ (engine-4 API): -1 = back to the global setting
    void setAutoFindSources(const QString &id, int minutes) override
    {
        if (TransferInfo *t = find(id)) {
            t->autoFindSourcesMin = minutes < 0 ? m_settings->i(keys::search::AutoFindSourcesMin) : minutes;
            emit transferChanged(*t);
        }
    }
    void setAutoEnterQueue(const QString &id, int n) override
    {
        if (TransferInfo *t = find(id)) {
            t->autoEnterQueue = n < 0 ? m_settings->i(keys::search::AutoEnterQueue) : n;
            emit transferChanged(*t);
        }
    }
    // [dl-1] download order flags and preview (demo: the bytes received count as the file's head)
    void setSequentialDownload(const QString &id, bool on) override
    {
        if (TransferInfo *t = find(id)) {
            t->sequential = on;
            t->orderFollowedBy = (t->sequential || t->firstLastFirst) ? t->networks : QList<Net>();
            emit transferChanged(*t);
        }
    }
    void setFirstLastPiecesFirst(const QString &id, bool on) override
    {
        if (TransferInfo *t = find(id)) {
            t->firstLastFirst = on;
            t->orderFollowedBy = (t->sequential || t->firstLastFirst) ? t->networks : QList<Net>();
            emit transferChanged(*t);
        }
    }
    static PreviewFile demoPreview(int index, const QString &name, qint64 size, qint64 done, qint64 tail = -1)
    {
        PreviewFile pf;
        pf.index = index;
        pf.name = name;
        pf.size = size;
        pf.done = done;
        pf.headBytes = tail >= 0 ? done - tail : done;
        pf.tailBytes = tail >= 0 ? tail : (done >= size ? size : 0);
        preview::Mp4Layout mp4;
        mp4.decided = done > 0;
        mp4.tailFrom = 0;
        const preview::Readiness r = preview::evaluate(name, size, pf.headBytes, pf.tailBytes, mp4);
        pf.media = r.media;
        pf.ready = r.ready;
        pf.reason = r.reason;
        return pf;
    }
    PreviewInfo previewInfo(const QString &id) const override
    {
        PreviewInfo info;
        for (const TransferInfo &t : m_downloads) {
            if (t.id != id)
                continue;
            info.supported = true;
            if (t.isBundle) {
                // demo: the bundle's progress spread over its files, a quarter of it at each file's end
                for (const BundleFile &b : t.files) {
                    if (!b.wanted || !preview::isMediaFile(b.path))
                        continue;
                    const qint64 done = b.done >= 0 ? b.done : (t.size > 0 ? qint64(double(t.done) / double(t.size) * double(b.size)) : 0);
                    info.files << demoPreview(b.index, b.path, b.size, done, done / 4);
                }
            } else {
                info.files << demoPreview(-1, t.name, t.size, t.done);
            }
        }
        return info;
    }
    QString preparePreview(const QString &id, int fileIndex, QString *error) override
    {
        const PreviewInfo info = previewInfo(id);
        const TransferInfo *t = nullptr;
        for (const TransferInfo &x : m_downloads)
            if (x.id == id)
                t = &x;
        for (const PreviewFile &pf : info.files) {
            if (pf.index != fileIndex && !(fileIndex < 0 && info.files.size() == 1))
                continue;
            if (!pf.ready) {
                if (error)
                    *error = pf.reason;
                return QString();
            }
            QString path = t ? t->savePath : QString();
            if (t && t->isBundle)
                path = QDir(t->savePath).filePath(pf.name);
            // a small placeholder inside the profile, so a player (or the launch log) has a file
            if (!m_profile.isEmpty() && QDir(path).absolutePath().startsWith(QDir(m_profile).absolutePath())) {
                QDir().mkpath(QFileInfo(path).absolutePath());
                QFile f(path);
                if (!f.exists() && f.open(QIODevice::WriteOnly))
                    f.write("OctoMX demo placeholder\n");
            }
            return path;
        }
        if (error)
            *error = tr("Preview: not an audio or video file");
        return QString();
    }
    void cancelUpload(const QString &id) override
    {
        if (TransferInfo *t = find(id)) {
            t->state = TransferState::Cancelled;
            t->statusText = QStringLiteral("Cancelled");
            t->rateUpBps = 0;
            emit transferChanged(*t);
        }
    }
    void setWantedFiles(const QString &id, const QList<int> &indexes) override
    {
        if (TransferInfo *t = find(id)) {
            qint64 size = 0;
            for (const BundleFile &f : t->files)
                if (indexes.isEmpty() || indexes.contains(f.index))
                    size += f.size;
            if (size > 0)
                t->size = size;
            t->done = qMin(t->done, t->size);
            emit transferChanged(*t);
        }
    }
    void openFile(const QString &id) override { Q_UNUSED(id); }
    void openFolder(const QString &id) override { Q_UNUSED(id); }
    QString incomingDir() const override
    {
        const QString s = m_settings->s(keys::transfers::IncomingDir);
        if (!s.isEmpty())
            return s;
        return QDir(m_profile.isEmpty() ? QDir::tempPath() : m_profile).filePath(QStringLiteral("incoming"));
    }

private:
    void tick()
    {
        ++m_tick;
        for (TransferInfo &t : m_downloads) {
            auto sim = m_pieces.find(t.id);   // [ui-i]
            if (t.state != TransferState::Transferring) {
                // paused / queued: the map keeps what is there, nothing is arriving any more
                if (sim != m_pieces.end() && t.pieceMap.contains(char(1))) {
                    sim->setRunning(false);
                    sim->apply(t);
                    emit transferChanged(t);
                }
                continue;
            }
            const int wobble = int((m_tick * 7919 + qHash(t.id)) % 9000) - 4500;
            t.rateDownBps = qMax(1000, t.rateDownBps + wobble / 4);
            if (sim != m_pieces.end()) {
                sim->advance(t.rateDownBps);
                sim->apply(t);
            } else {
                t.done = qMin(t.size > 0 ? t.size : t.done + t.rateDownBps, t.done + t.rateDownBps);
            }
            if (t.size <= 0)
                t.size = 50 * 1024 * 1024;
            int sum = 0;
            for (TransferSource &s : t.sources)
                if (s.rateBps > 0) {
                    s.rateBps = qMax(500, s.rateBps + wobble / 8);
                    s.bytes += s.rateBps;
                    sum += s.rateBps;
                }
            if (sum > 0)
                t.rateDownBps = sum;
            t.etaSec = t.rateDownBps > 0 ? int((t.size - t.done) / t.rateDownBps) : -1;
            if (t.done >= t.size) {
                t.done = t.size;
                t.state = TransferState::Completed;
                t.statusText = QStringLiteral("Complete");
                t.rateDownBps = 0;
                t.etaSec = -1;
                t.completed = QDateTime::currentDateTimeUtc();
                for (TransferSource &s : t.sources) {
                    s.rateBps = 0;
                    s.statusText = QStringLiteral("Complete");
                }
                emit transferChanged(t);
                emit downloadCompleted(t);
                continue;
            }
            emit transferChanged(t);
        }
        for (TransferInfo &t : m_uploads) {
            if (t.state != TransferState::Transferring)
                continue;
            t.done = qMin(t.size, t.done + t.rateUpBps);
            if (t.done >= t.size) {
                t.state = TransferState::Completed;
                t.statusText = QStringLiteral("Complete");
                t.rateUpBps = 0;
            }
            t.etaSec = t.rateUpBps > 0 ? int((t.size - t.done) / t.rateUpBps) : -1;
            emit transferChanged(t);
        }
    }
    static bool transfersFinished(const TransferInfo &t)
    {
        return t.state == TransferState::Completed || t.state == TransferState::Seeding || t.state == TransferState::Cancelled;
    }
    QString ownerOf(const QString &file) const
    {
        for (auto it = m_resumedFrom.constBegin(); it != m_resumedFrom.constEnd(); ++it)
            if (it.value() == file)
                return it.key();
        return {};
    }
    TransferInfo *find(const QString &id) const { return const_cast<MockTransfers *>(this)->find(id); }
    Settings *m_settings;
    QString m_profile;
    bool m_frozen;
    QList<TransferInfo> m_downloads;
    QHash<QString, mock::PieceSim> m_pieces;   // [ui-i] maps of the demo downloads
    QList<TransferInfo> m_uploads;
    QHash<QString, QString> m_resumedFrom;   // download id -> the file it resumed from (used in place)
    int m_nextSource = 100;
    int m_next = 0;
    qint64 m_tick = 0;
};

// ----------------------------------------------------------------------------------- library
// Shared folders persist in the profile ("mock/libraryFolders", "mock/libraryExcluded"). The two
// demo roots get canned files; any real directory that is shared is scanned for real (in small
// chunks on the event loop, capped) so "Share Folder" in --demo shows the user's own files.
// Without --demo-frozen new files start unhashed and a simulated hasher works through them,
// reporting scanProgress like the real Library.
class MockLibrary : public LibraryService {
public:
    MockLibrary(Settings *s, bool frozen, QObject *parent) : LibraryService(parent), m_settings(s), m_frozen(frozen)
    {
        m_excluded = m_settings->value(QStringLiteral("mock/libraryExcluded")).toStringList();
        m_excludedFolders = m_settings->value(QStringLiteral("mock/libraryExcludedFolders")).toStringList();
        const QVariant stored = m_settings->value(QStringLiteral("mock/libraryFolders"));
        QList<SharedFolder> folders;
        if (stored.isValid() && !stored.isNull()) {
            for (const QVariant &v : stored.toList()) {
                const QVariantMap m = v.toMap();
                SharedFolder f;
                f.path = m.value(QStringLiteral("path")).toString();
                f.recursive = m.value(QStringLiteral("recursive"), true).toBool();
                if (!f.path.isEmpty())
                    folders << f;
            }
        } else {
            folders = mock::sharedFolders();
        }
        for (const SharedFolder &f : std::as_const(folders)) {
            SharedFolder c = f;
            c.fileCount = 0;
            c.totalBytes = 0;
            m_folders << c;
            loadFolder(c, false);
        }
        if (!frozen) {
            // a few files still waiting for the hasher at start-up (shows the progress)
            for (int i = int(m_files.size()) - 1, n = 0; i >= 0 && n < 12; --i, ++n)
                m_files[i].hashed = false;
            m_hashTimer.setInterval(350);
            QObject::connect(&m_hashTimer, &QTimer::timeout, this, [this] { hashSome(); });
            m_hashTimer.start();
        }
        m_scanTimer.setInterval(0);
        QObject::connect(&m_scanTimer, &QTimer::timeout, this, [this] { scanSome(); });
    }
    // ---- glue-3: file-type filter, excluded sub folders, media folders (like the core Library)
    QStringList sharedTypes() const override   // [ui-f] legacy ids converted like the core Library
    {
        return normalizeSharedTypeIds(m_settings->sl(keys::library::SharedTypes), m_settings->b(keys::library::ShareUnlistedTypes));
    }
    void setSharedTypes(const QStringList &ids) override
    {
        m_settings->setValue(keys::library::SharedTypes, normalizeSharedTypeIds(ids, m_settings->b(keys::library::ShareUnlistedTypes)));
        emit filesChanged();
    }
    QString otherExtensions() const override { return m_settings->s(keys::library::OtherExtensions); }
    void setOtherExtensions(const QString &ext) override
    {
        m_settings->setValue(keys::library::OtherExtensions, ext);
        emit filesChanged();
    }
    void excludeFolder(const QString &path, bool excluded) override
    {
        const QString p = QDir::cleanPath(QDir::fromNativeSeparators(path));
        if (excluded == m_excludedFolders.contains(p))
            return;
        if (excluded)
            m_excludedFolders << p;
        else
            m_excludedFolders.removeAll(p);
        m_settings->setValue(QStringLiteral("mock/libraryExcludedFolders"), m_excludedFolders);
        emit filesChanged();
    }
    QStringList excludedFolders() const override { return m_excludedFolders; }
    QStringList suggestMediaFolders() const override
    {
        QStringList out;
        for (auto loc : {QStandardPaths::MusicLocation, QStandardPaths::MoviesLocation, QStandardPaths::PicturesLocation,
                         QStandardPaths::DownloadLocation}) {
            const QString p = QStandardPaths::writableLocation(loc);
            if (!p.isEmpty() && QFileInfo(p).isDir() && QDir(p) != QDir::home() && !out.contains(p))
                out << p;
        }
        return out;
    }
    bool visible(const SharedFile &f) const
    {
        const QString p = QDir::fromNativeSeparators(f.path);
        for (const QString &d : m_excludedFolders)
            if (p.startsWith(d + QLatin1Char('/')))
                return false;
        return fileMatchesSharedTypes(f.name.isEmpty() ? QFileInfo(f.path).fileName() : f.name, sharedTypes(), otherExtensions(),
                                      m_settings->b(keys::library::ShareUnlistedTypes));
    }
    QList<SharedFile> shown() const
    {
        QList<SharedFile> l;
        for (const SharedFile &f : m_files)
            if (visible(f))
                l << f;
        return l;
    }
    QList<SharedFolder> folders() const override
    {
        QList<SharedFolder> l = m_folders;
        for (SharedFolder &f : l) {
            for (const SharedFile &x : shown())
                if (under(x.path, f)) {
                    ++f.fileCount;
                    f.totalBytes += x.size;
                }
        }
        return l;
    }
    void addFolder(const QString &path, bool recursive) override
    {
        const QString p = QDir::cleanPath(QDir::fromNativeSeparators(path));
        for (SharedFolder &f : m_folders)
            if (QDir::cleanPath(QDir::fromNativeSeparators(f.path)) == p) {
                f.recursive = recursive;
                persist();
                emit foldersChanged();
                return;
            }
        SharedFolder f;
        f.path = path;
        f.recursive = recursive;
        m_folders << f;
        persist();
        emit foldersChanged();
        loadFolder(f, true);
    }
    void removeFolder(const QString &path) override
    {
        const QString p = QDir::cleanPath(QDir::fromNativeSeparators(path));
        for (int i = 0; i < m_folders.size(); ++i)
            if (QDir::cleanPath(QDir::fromNativeSeparators(m_folders[i].path)) == p) {
                const SharedFolder f = m_folders.takeAt(i);
                QList<SharedFile> keep;
                for (const SharedFile &x : std::as_const(m_files))
                    if (!under(x.path, f) || coveredByOther(x.path))
                        keep << x;
                m_files = keep;
                for (int k = int(m_pending.size()) - 1; k >= 0; --k)
                    if (m_pending[k].root.path == f.path)
                        m_pending.removeAt(k);
                break;
            }
        persist();
        emit foldersChanged();
        emit filesChanged();
    }
    void rescan() override
    {
        // re-read the real folders; the canned ones stay as they are
        QList<SharedFile> keep;
        for (const SharedFile &x : std::as_const(m_files)) {
            bool real = false;
            for (const SharedFolder &f : std::as_const(m_folders))
                if (under(x.path, f) && QFileInfo(f.path).isDir())
                    real = true;
            if (!real)
                keep << x;
        }
        m_files = keep;
        for (const SharedFolder &f : std::as_const(m_folders))
            if (QFileInfo(f.path).isDir())
                loadFolder(f, true);
        emit filesChanged();
        if (m_pending.isEmpty())
            emit scanProgress(int(m_files.size()), int(m_files.size()), QString());
    }
    QList<SharedFile> files(const QString &folder) const override
    {
        if (folder.isEmpty())
            return shown();
        SharedFolder f;
        f.path = folder;
        QList<SharedFile> l;
        for (const SharedFile &x : shown())
            if (under(x.path, f))
                l << x;
        return l;
    }
    int fileCount() const override { return int(shown().size()); }
    qint64 totalBytes() const override
    {
        qint64 s = 0;
        for (const SharedFile &f : shown())
            s += f.size;
        return s;
    }
    bool isScanning() const override { return !m_pending.isEmpty() || unhashed() > 0; }
    QString scanStatus() const override
    {
        if (!m_pending.isEmpty())
            return tr("Scanning %1").arg(QDir::toNativeSeparators(m_pending.first().root.path));
        const int u = unhashed();
        if (u > 0)
            return tr("Hashing %1 of %2").arg(int(m_files.size()) - u + 1).arg(m_files.size());
        return QString();
    }
    void forgetPaths(const QStringList &paths) override   // [ui-j] deleted from the disk
    {
        QList<SharedFile> keep;
        for (const SharedFile &x : std::as_const(m_files)) {
            const QString fp = QDir::cleanPath(QDir::fromNativeSeparators(x.path));
            bool gone = false;
            for (const QString &p : paths) {
                const QString c = QDir::cleanPath(QDir::fromNativeSeparators(p));
                gone = gone || fp == c || fp.startsWith(c + QLatin1Char('/'));
            }
            if (!gone)
                keep << x;
        }
        if (keep.size() == m_files.size())
            return;
        m_files = keep;
        emit filesChanged();
        emit foldersChanged();
    }
    void unshareFile(const QString &path) override
    {
        for (int i = 0; i < m_files.size(); ++i)
            if (m_files[i].path == path) {
                m_files.removeAt(i);
                if (!m_excluded.contains(path))
                    m_excluded << path;
                m_settings->setValue(QStringLiteral("mock/libraryExcluded"), m_excluded);
                emit filesChanged();
                return;
            }
    }

private:
    struct Pending {
        SharedFolder root;
        QStringList dirs;     // directories still to list
    };
    static bool under(const QString &file, const SharedFolder &f)
    {
        const QString fp = QDir::fromNativeSeparators(file);
        QString root = QDir::fromNativeSeparators(f.path);
        if (!root.endsWith(QLatin1Char('/')))
            root += QLatin1Char('/');
        if (!fp.startsWith(root))
            return false;
        return f.recursive || !fp.mid(root.size()).contains(QLatin1Char('/'));
    }
    bool coveredByOther(const QString &file) const
    {
        for (const SharedFolder &f : m_folders)
            if (under(file, f))
                return true;
        return false;
    }
    int unhashed() const
    {
        int n = 0;
        for (const SharedFile &f : m_files)
            if (!f.hashed)
                ++n;
        return n;
    }
    void persist()
    {
        QVariantList l;
        for (const SharedFolder &f : std::as_const(m_folders))
            l << QVariantMap{{QStringLiteral("path"), f.path}, {QStringLiteral("recursive"), f.recursive}};
        m_settings->setValue(QStringLiteral("mock/libraryFolders"), l);
    }
    void addFile(const SharedFile &f)
    {
        if (m_excluded.contains(f.path))
            return;
        for (const SharedFile &x : std::as_const(m_files))
            if (x.path == f.path)
                return;
        m_files << f;
    }
    void loadFolder(const SharedFolder &f, bool announce)
    {
        if (f.path == mock::sharedRoot() || f.path == mock::downloadsRoot()) {
            for (const SharedFile &x : mock::demoFilesFor(f.path))
                if (under(x.path, f))
                    addFile(x);
            if (announce)
                emit filesChanged();
            return;
        }
        if (!QFileInfo(f.path).isDir())
            return;
        Pending p;
        p.root = f;
        p.dirs << f.path;
        m_pending << p;
        if (m_frozen) {
            while (!m_pending.isEmpty())
                scanSome();
        } else if (!m_scanTimer.isActive()) {
            m_scanTimer.start();
        }
    }
    void scanSome()
    {
        if (m_pending.isEmpty()) {
            m_scanTimer.stop();
            return;
        }
        Pending &p = m_pending.first();
        int budget = 200;
        while (budget > 0 && !p.dirs.isEmpty() && m_files.size() < 20000) {
            const QString dir = p.dirs.takeFirst();
            const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QFileInfo &fi : entries) {
                --budget;
                if (fi.isDir()) {
                    if (p.root.recursive && !fi.isSymLink())
                        p.dirs << fi.absoluteFilePath();
                    continue;
                }
                SharedFile f;
                f.path = QDir::toNativeSeparators(fi.absoluteFilePath());
                f.name = fi.fileName();
                f.size = fi.size();
                f.mtime = fi.lastModified();
                f.type = mediaTypeForFileName(f.name);
                f.hashed = m_frozen;
                addFile(f);
            }
        }
        const bool done = p.dirs.isEmpty() || m_files.size() >= 20000;
        if (done)
            m_pending.removeFirst();
        emit filesChanged();
        if (m_pending.isEmpty()) {
            m_scanTimer.stop();
            if (!m_frozen && !m_hashTimer.isActive())
                m_hashTimer.start();
        }
    }
    void hashSome()
    {
        int total = int(m_files.size()), done = 0;
        QString current;
        int budget = 3;
        for (SharedFile &f : m_files) {
            if (!f.hashed && budget > 0) {
                f.hashed = true;
                f.hashes.ed2k = QCryptographicHash::hash(f.path.toUtf8(), QCryptographicHash::Md4);
                current = f.path;
                --budget;
            }
            if (f.hashed)
                ++done;
        }
        if (current.isEmpty()) {
            m_hashTimer.stop();
            return;
        }
        emit scanProgress(done, total, current);
        if (done >= total || done % 9 == 0)
            emit filesChanged();
    }

    Settings *m_settings;
    bool m_frozen;
    QList<SharedFolder> m_folders;
    QList<SharedFile> m_files;
    QStringList m_excluded;
    QStringList m_excludedFolders;
    QList<Pending> m_pending;
    QTimer m_scanTimer;
    QTimer m_hashTimer;
};

// ----------------------------------------------------------------------------------- chat
class MockChat : public ChatService {
public:
    MockChat(Settings *s, bool frozen, QObject *parent) : ChatService(parent), m_settings(s), m_frozen(frozen)
    {
        m_rooms = mock::rooms();
        // a second chat network (demo fiction) so List Channels shows its network choice
        const struct { const char *name; const char *topic; int users; } gnut[] = {
            {"#gnutella-help", "Gnutella client help", 37}, {"#linux-isos", "Distribution torrents and mirrors", 112},
            {"#creative-commons", "Free films and music", 58}};
        for (const auto &g : gnut) {
            ChatRoomInfo r;
            r.id = QStringLiteral("gnut:") + QString::fromLatin1(g.name);
            r.name = QString::fromLatin1(g.name);
            r.topic = QString::fromLatin1(g.topic);
            r.users = g.users;
            r.limit = 500;
            r.net = Net::Gnutella;
            m_rooms << r;
        }
        const QVariant favs = m_settings->value(QStringLiteral("mock/chatFavourites"));
        m_favs = favs.isValid() && !favs.isNull()
                     ? favs.toStringList()
                     : QStringList{mock::helpRoomName(), m_rooms.value(3).name, m_rooms.value(11).name};
        if (!frozen) {
            auto *t = new QTimer(this);
            t->setInterval(2500);
            connect(t, &QTimer::timeout, this, [this] { chatter(); });
            t->start();
        }
    }
    void refreshRoomList() override
    {
        if (m_frozen)
            emit roomListChanged();
        else
            QTimer::singleShot(300, this, [this] { emit roomListChanged(); });
    }
    QList<Net> chatNetworks() const override { return {Net::Overlay, Net::Gnutella}; }
    void refreshRoomList(Net net) override
    {
        ++m_refreshes[int(net)];
        refreshRoomList();
    }
    int refreshCount(Net net) const { return m_refreshes.value(int(net)); }
    QList<ChatRoomInfo> roomList() const override { return m_rooms; }
    void joinRoom(const QString &roomIdOrName) override
    {
        ChatRoomInfo r;
        bool found = false;
        for (const ChatRoomInfo &x : m_rooms)
            if (x.id == roomIdOrName || x.name == roomIdOrName || x.name.startsWith(roomIdOrName + QLatin1Char('_'))) {
                r = x;
                found = true;
            }
        if (!found) {
            r.name = roomIdOrName + QStringLiteral("_0A1B2C3D4E5F");
            r.id = r.name;
            r.topic = tr("Welcome to your new room");
            r.users = 1;
            r.limit = 40;
            m_rooms << r;
        }
        for (const ChatRoomInfo &j : m_joined)
            if (j.id == r.id)
                return;
        m_joined << r;
        QList<ChatUser> users = mock::roomUsers(r.id);
        ChatUser me;
        me.user = mock::user(Net::Overlay, m_settings->s(keys::identity::ScreenName));
        me.connection = QStringLiteral("DSL");
        me.files = 74;
        me.rank = found ? 0 : 2;   // the creator of a new room hosts it
        users << me;
        m_roomUsers.insert(r.id, users);
        m_backlog.insert(r.id, mock::roomBacklog(r));
        emit roomJoined(r);
    }
    void leaveRoom(const QString &roomId) override
    {
        for (int i = 0; i < m_joined.size(); ++i)
            if (m_joined[i].id == roomId) {
                m_joined.removeAt(i);
                emit roomLeft(roomId);
                return;
            }
    }
    QList<ChatRoomInfo> joinedRooms() const override { return m_joined; }
    QList<ChatUser> roomUsers(const QString &roomId) const override { return m_roomUsers.value(roomId); }
    QList<ChatMessage> backlog(const QString &roomId) const override { return m_backlog.value(roomId); }
    void post(const ChatMessage &m)
    {
        if (!m.roomId.isEmpty())
            m_backlog[m.roomId] << m;
        emit messageReceived(m);
    }
    void sendMessage(const QString &roomId, const QString &text) override
    {
        ChatMessage m;
        m.roomId = roomId;
        m.from = mock::user(Net::Overlay, m_settings->s(keys::identity::ScreenName));
        m.time = QDateTime::currentDateTimeUtc();
        const QString word = text.section(QLatin1Char(' '), 0, 0).toLower();
        const QString arg = text.section(QLatin1Char(' '), 1).trimmed();
        auto sys = [this, roomId](const QString &t, ChatKind k = ChatKind::System) {
            ChatMessage r;
            r.roomId = roomId;
            r.kind = k;
            r.text = t;
            r.time = QDateTime::currentDateTimeUtc();
            post(r);
        };
        if (word == QLatin1String("/me") || word == QLatin1String("/action") || word == QLatin1String("/emote")) {
            m.kind = ChatKind::Action;
            m.text = arg;
        } else if (word == QLatin1String("/topic")) {
            if (arg.isEmpty()) {
                for (const ChatRoomInfo &r : std::as_const(m_joined))
                    if (r.id == roomId)
                        sys(tr("Topic: %1").arg(r.topic), ChatKind::Topic);
            } else {
                setTopic(roomId, arg);
            }
            return;
        } else if (word == QLatin1String("/clr") || word == QLatin1String("/clear") || word == QLatin1String("/cls")) {
            m_backlog[roomId].clear();
            emit backlogCleared(roomId);
            return;
        } else if (word == QLatin1String("/join") || word == QLatin1String("/j")) {
            if (!arg.isEmpty())
                joinRoom(arg);
            return;
        } else if (word == QLatin1String("/part") || word == QLatin1String("/leave")) {
            leaveRoom(arg.isEmpty() ? roomId : arg);
            return;
        } else if (word == QLatin1String("/msg") || word == QLatin1String("/pm") || word == QLatin1String("/m")) {
            const QString who = arg.section(QLatin1Char(' '), 0, 0);
            const QString msg = arg.section(QLatin1Char(' '), 1);
            if (!who.isEmpty() && !msg.isEmpty())
                sendPrivateMessage(mock::user(Net::Overlay, who), msg);
            return;
        } else if (word == QLatin1String("/whois") || word == QLatin1String("/w")) {
            const QVariantMap info = mock::whois(mock::user(Net::Overlay, arg));
            sys(tr("Whois %1:").arg(arg));
            for (auto it = info.constBegin(); it != info.constEnd(); ++it)
                sys(QStringLiteral("  %1: %2").arg(it.key(), it.value().toString()));
            return;
        } else if (word == QLatin1String("/ignore") || word == QLatin1String("/unignore")) {
            if (m_users && !arg.isEmpty())
                m_users->ignore(mock::user(Net::Overlay, arg), word == QLatin1String("/ignore"));
            sys(word == QLatin1String("/ignore") ? tr("%1 is now ignored").arg(arg) : tr("%1 is no longer ignored").arg(arg));
            return;
        } else if (word == QLatin1String("/help") || word == QLatin1String("/?")) {
            sys(tr("Commands: /me /topic /join /part /msg /whois /ignore /unignore /clear /help; other /commands go to the host"));
            return;
        } else if (word.startsWith(QLatin1Char('/')) && !word.startsWith(QLatin1String("//"))) {
            sys(tr("%1: you do not have access to this command in this channel").arg(word), ChatKind::Error);
            return;
        } else {
            m.text = text.startsWith(QLatin1String("//")) ? text.mid(1) : text;
        }
        post(m);
    }
    void sendPrivateMessage(const UserRef &to, const QString &text) override
    {
        ChatMessage m;
        m.kind = ChatKind::Private;
        m.from = mock::user(Net::Overlay, m_settings->s(keys::identity::ScreenName));
        m.to = to;
        m.text = text;
        m.time = QDateTime::currentDateTimeUtc();
        emit messageReceived(m);
        ChatMessage r;
        r.kind = ChatKind::Private;
        r.from = to;
        r.text = tr("hi! (auto reply from the demo)");
        r.time = m.time;
        if (m_frozen)
            emit messageReceived(r);
        else
            QTimer::singleShot(1500, this, [this, r] { emit messageReceived(r); });
    }
    void setTopic(const QString &roomId, const QString &topic) override
    {
        for (ChatRoomInfo &r : m_joined)
            if (r.id == roomId) {
                r.topic = topic;
                ChatMessage m;
                m.roomId = roomId;
                m.kind = ChatKind::Topic;
                m.text = tr("Topic changed to: %1").arg(topic);
                m.time = QDateTime::currentDateTimeUtc();
                post(m);
                emit roomUpdated(r);
            }
    }
    QStringList favouriteRooms() const override { return m_favs; }
    void setFavouriteRoom(const QString &room, bool fav) override
    {
        m_favs.removeAll(room);
        if (fav)
            m_favs << room;
        m_settings->setValue(QStringLiteral("mock/chatFavourites"), m_favs);
    }

private:
    void chatter()
    {
        ++m_tick;
        if (m_tick == 10 && !m_pmSent) {
            // one incoming private message per demo session (opens a window, adds to Temporary)
            m_pmSent = true;
            ChatMessage r;
            r.kind = ChatKind::Private;
            r.from = mock::user(Net::Overlay, QStringLiteral("helping_holly_88120"));
            r.text = tr("hey! are you the one sharing the Gooseberry Big Band collection?");
            r.time = QDateTime::currentDateTimeUtc();
            emit messageReceived(r);
        }
        if (m_joined.isEmpty())
            return;
        ChatRoomInfo &r = m_joined[m_tick % m_joined.size()];
        QList<ChatUser> &users = m_roomUsers[r.id];
        if (users.isEmpty())
            return;
        ChatMessage m;
        m.roomId = r.id;
        m.time = QDateTime::currentDateTimeUtc();
        if (m_tick % 5 == 0) {
            // somebody joins or leaves
            static const char *conns[] = {"VDSL", "Cable", "4G", "FTTB"};
            if (m_tick % 10 == 0 && users.size() > 5) {
                const ChatUser gone = users.takeAt(int(m_tick % (users.size() - 3)) + 3);
                m.kind = ChatKind::Part;
                m.from = gone.user;
                m.text = tr("%1 has left").arg(gone.user.name);
            } else {
                ChatUser u;
                u.user = mock::user(Net::Overlay, QStringLiteral("newbie%1_%2").arg(m_tick).arg(10000 + m_tick * 37));
                u.files = (m_tick * 13) % 500;
                u.connection = QString::fromLatin1(conns[m_tick % 4]);
                users << u;
                m.kind = ChatKind::Join;
                m.from = u.user;
                m.text = tr("%1 (%2 %3 files) has entered").arg(u.user.name, u.connection).arg(u.files);
            }
            r.users = int(users.size());
            post(m);
            emit roomUsersChanged(r.id);
            emit roomUpdated(r);
            return;
        }
        const QStringList lines = mock::chatLines();
        m.from = users.at(m_tick % users.size()).user;
        m.text = lines.at(m_tick % lines.size());
        if (m_tick % 7 == 0) {
            m.kind = ChatKind::Action;
            m.text = QStringLiteral("is listening to Velorine - Velvet Engine");
        }
        post(m);
    }

public:
    void setUsers(UserService *u) { m_users = u; }

private:
    UserService *m_users = nullptr;
    bool m_pmSent = false;
    QHash<int, int> m_refreshes;
    Settings *m_settings;
    bool m_frozen;
    QList<ChatRoomInfo> m_rooms;
    QList<ChatRoomInfo> m_joined;
    QHash<QString, QList<ChatUser>> m_roomUsers;
    QHash<QString, QList<ChatMessage>> m_backlog;
    QStringList m_favs;
    int m_tick = 0;
};

// ----------------------------------------------------------------------------------- users
class MockUsers : public UserService {
public:
    MockUsers(Settings *s, bool frozen, QObject *parent) : UserService(parent), m_settings(s), m_frozen(frozen)
    {
        m_hotlist = mock::hotlist();
        // the ignore list persists in the profile ("net|name" entries)
        const QVariant stored = s->value(QStringLiteral("mock/ignored"));
        if (stored.isValid() && !stored.isNull()) {
            for (const QString &e : stored.toStringList())
                m_ignored << mock::user(Net(e.section(QLatin1Char('|'), 0, 0).toInt()), e.section(QLatin1Char('|'), 1));
        } else {
            m_ignored = mock::ignored();
        }
        connect(this, &UserService::ignoredChanged, this, [this] {
            QStringList l;
            for (const UserRef &u : std::as_const(m_ignored))
                l << QStringLiteral("%1|%2").arg(int(u.net)).arg(u.name);
            m_settings->setValue(QStringLiteral("mock/ignored"), l);
        });
    }
    QList<HotlistEntry> hotlist() const override { return m_hotlist; }
    bool setOnline(const QString &name, bool online)   // [ui-g] tests
    {
        for (HotlistEntry &e : m_hotlist)
            if (e.user.name == name) {
                e.online = online;
                emit hotlistChanged();
                return true;
            }
        return false;
    }
    void addToHotlist(const UserRef &u) override
    {
        for (const HotlistEntry &e : m_hotlist)
            if (e.user.key() == u.key())
                return;
        HotlistEntry e;
        e.user = u;
        e.online = true;
        m_hotlist << e;
        emit hotlistChanged();
    }
    void removeFromHotlist(const UserRef &u) override
    {
        for (int i = 0; i < m_hotlist.size(); ++i)
            if (m_hotlist[i].user.key() == u.key()) {
                m_hotlist.removeAt(i);
                emit hotlistChanged();
                return;
            }
    }
    void setNote(const UserRef &u, const QString &note) override
    {
        for (HotlistEntry &e : m_hotlist)
            if (e.user.key() == u.key())
                e.note = note;
        emit hotlistChanged();
    }
    QList<UserRef> ignored() const override { return m_ignored; }
    void ignore(const UserRef &u, bool on) override
    {
        for (int i = 0; i < m_ignored.size(); ++i)
            if (m_ignored[i].key() == u.key()) {
                if (!on) {
                    m_ignored.removeAt(i);
                    emit ignoredChanged();
                }
                return;
            }
        if (on) {
            m_ignored << u;
            emit ignoredChanged();
        }
    }
    bool isIgnored(const UserRef &u) const override
    {
        for (const UserRef &x : m_ignored)
            if (x.key() == u.key())
                return true;
        return false;
    }
    quint32 browse(const UserRef &u) override
    {
        const quint32 id = ++m_next;
        const QList<BrowseEntry> all = mock::browseFiles(u);
        if (m_frozen) {
            QTimer::singleShot(0, this, [this, id, u, all] { emit browseResults(id, u, all, true); });
        } else {
            const int half = int(all.size()) / 2;
            QTimer::singleShot(500, this, [this, id, u, all, half] { emit browseResults(id, u, all.mid(0, half), false); });
            QTimer::singleShot(1200, this, [this, id, u, all, half] { emit browseResults(id, u, all.mid(half), true); });
        }
        return id;
    }
    void cancelBrowse(quint32) override {}
    void whois(const UserRef &u) override
    {
        const QVariantMap m = mock::whois(u);
        if (m_frozen)
            QTimer::singleShot(0, this, [this, u, m] { emit whoisResult(u, m); });
        else
            QTimer::singleShot(400, this, [this, u, m] { emit whoisResult(u, m); });
    }
    quint32 requestWhois(const UserRef &u) override
    {
        const quint32 id = ++m_whoisId;
        const QVariantMap m = mock::whois(u);
        QTimer::singleShot(m_frozen ? 0 : 400, this, [this, id, u, m] {
            emit whoisReply(id, u, m);
            emit whoisResult(u, m);
        });
        return id;
    }

private:
    Settings *m_settings;
    bool m_frozen;
    QList<HotlistEntry> m_hotlist;
    QList<UserRef> m_ignored;
    quint32 m_next = 0;
    quint32 m_whoisId = 0;
};

// ----------------------------------------------------------------------------------- bandwidth
// Totals = protocol overhead per network (mock::overheadAt) + the payload of the demo transfers.
// The per-network rates are written into the MockNetwork statuses so the Bandwidth page can split
// the totals into its coloured series like it does with the real core.
class MockBandwidth : public BandwidthService {
public:
    // Every sample carries the per-network totals and the payload part (like the real core). The
    // 20 minutes before the start are in history() from the beginning (the Bandwidth page draws that
    // backlog when it opens); live seconds follow every second unless frozen.
    MockBandwidth(bool frozen, MockNetwork *net, TransferService *tr, QObject *parent)
        : BandwidthService(parent), m_net(net), m_tr(tr)
    {
        // a fixed "now" when frozen so the graph is reproducible
        m_t0 = frozen ? qint64(1800000000000LL) : QDateTime::currentMSecsSinceEpoch();
        for (int i = 0; i < kHistory; ++i)
            store(compute(i, m_t0 - (kHistory - i) * 1000, mock::payloadShape(kHistory - i, i), false));
        m_t = kHistory;
        compute(m_t, m_t0, 1.0, true);   // today's per-network rates into the NetworkStatus list
        if (!frozen) {
            auto *t = new QTimer(this);
            t->setInterval(1000);
            connect(t, &QTimer::timeout, this, [this] { push(compute(++m_t, QDateTime::currentMSecsSinceEpoch(), 1.0, true)); });
            t->start();
        }
    }
    QList<BandwidthSample> history(int seconds) const override { return m_hist.mid(qMax(0, int(m_hist.size()) - seconds)); }
    BandwidthSample current() const override { return m_hist.isEmpty() ? BandwidthSample() : m_hist.last(); }
    qint64 totalDownloaded() const override { return 123456789 + m_down; }
    qint64 totalUploaded() const override { return 98765432 + m_up; }
    qint64 allTimeDownloaded() const override { return 98765432100LL + m_down; }
    qint64 allTimeUploaded() const override { return 45678901234LL + m_up; }

private:
    static constexpr int kHistory = 1200;
    void store(const BandwidthSample &s)
    {
        m_hist << s;
        if (m_hist.size() > 3600)
            m_hist.removeFirst();
        m_down += s.downBps;
        m_up += s.upBps;
    }
    void push(const BandwidthSample &s)
    {
        store(s);
        emit sample(s);
    }
    BandwidthSample compute(qint64 t, qint64 timeMs, double payloadFactor, bool apply)
    {
        QHash<int, qint64> payDown, payUp;
        qint64 down = 0, up = 0;
        auto account = [](QHash<int, qint64> &h, const TransferInfo &x, qint64 rate) {
            if (rate <= 0)
                return;
            QList<Net> nets = x.networks;
            if (nets.isEmpty() && x.peer.net != Net::Unknown)
                nets << x.peer.net;
            for (Net n : std::as_const(nets))
                h[int(n)] += rate / nets.size();
        };
        // network rates carry today's payload share (what the UI subtracts), the totals the
        // payload of that second
        for (const TransferInfo &x : m_tr->downloads()) {
            down += qint64(x.rateDownBps * payloadFactor);
            account(payDown, x, x.rateDownBps);
            up += qint64(x.rateUpBps * payloadFactor);
            account(payUp, x, x.rateUpBps);
        }
        for (const TransferInfo &x : m_tr->uploads()) {
            up += qint64(x.rateUpBps * payloadFactor);
            account(payUp, x, x.rateUpBps);
        }
        BandwidthSample s;
        s.payloadDownBps = int(qMin<qint64>(down, INT_MAX));
        s.payloadUpBps = int(qMin<qint64>(up, INT_MAX));
        // per network: its protocol overhead + its share of the payload of that second
        for (const mock::NetRate &r : mock::overheadAt(t)) {
            down += r.down;
            up += r.up;
            const int nd = int(r.down + payDown.value(int(r.net)) * payloadFactor);
            const int nu = int(r.up + payUp.value(int(r.net)) * payloadFactor);
            if (nd > 0)
                s.netDownBps.insert(r.net, nd);
            if (nu > 0)
                s.netUpBps.insert(r.net, nu);
            if (apply)
                m_net->setRates(r.net, int(r.down + payDown.value(int(r.net))), int(r.up + payUp.value(int(r.net))));
        }
        s.timeMs = timeMs;
        s.downBps = int(qMin<qint64>(down, INT_MAX));
        s.upBps = int(qMin<qint64>(up, INT_MAX));
        return s;
    }
    MockNetwork *m_net;
    TransferService *m_tr;
    QList<BandwidthSample> m_hist;
    qint64 m_t0 = 0, m_t = 0, m_down = 0, m_up = 0;
};

// ----------------------------------------------------------------------------------- connection test [diag-1]
// A realistic canned report: BitTorrent reachable, eD2k firewalled (LowID) because its port
// mapping failed (router answered 500), Kad TCP firewalled, one torrent with stale index numbers.
class MockDiagnostics : public DiagnosticsService {
public:
    MockDiagnostics(Settings *s, TransferService *tr, bool frozen, QObject *parent)
        : DiagnosticsService(parent), m_settings(s), m_tr(tr), m_frozen(frozen)
    {
        m_timer.setInterval(frozen ? 40 : 250);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            static const char *steps[] = {"Checking adapters and ports...", "Asking the router to forward the ports...",
                                          "Looking up 18 host names...", "Testing UDP: tracker handshake and DHT ping...",
                                          "Checking web access to servers and indexes...", "Waiting for the networks' own reachability checks..."};
            m_percent = qMin(100, m_percent + 10);
            m_step = QLatin1String(steps[qMin(5, m_percent / 18)]);
            emit testProgress(m_percent, m_step);
            emit reportChanged();
            if (m_percent >= 100) {
                m_timer.stop();
                m_running = false;
                m_lastTest = now();
                m_step = QStringLiteral("Done");
                emit testFinished(false);
                emit reportChanged();
            }
        });
    }
    DiagReport report() const override
    {
        DiagReport r;
        r.generated = now();
        r.version = QStringLiteral(MX_VERSION_STRING);
        r.os = QStringLiteral("Windows 11 (demo)");
        r.logFile = QStringLiteral("C:\\Users\\demo\\AppData\\Roaming\\OctoMX\\logs\\octomx.log");
        r.testRunning = m_running;
        r.testPercent = m_percent;
        r.testStep = m_step;
        r.lastTest = m_lastTest;
        auto it = [](const char *id, const char *check, DiagLevel lv, const QString &result, const QString &details = QString(),
                     const QString &explain = QString()) {
            DiagItem i;
            i.id = QLatin1String(id);
            i.check = QLatin1String(check);
            i.level = lv;
            i.result = result;
            i.details = details;
            i.explain = explain;
            return i;
        };
        const QString fwd = QStringLiteral("In your router, forward TCP port 4662 and UDP port 4672 to 192.168.1.23.");
        DiagGroup g;
        g.id = QStringLiteral("general");
        g.title = QStringLiteral("General");
        g.level = DiagLevel::Limited;
        g.result = QStringLiteral("Windows Firewall: On, no rule for OctoMX");
        g.items << it("general.adapter.eth", "Adapter Ethernet", DiagLevel::Ok, QStringLiteral("192.168.1.23"), QStringLiteral("default route"))
                << it("general.adapter.wg", "Adapter WireGuard Tunnel", DiagLevel::Ok, QStringLiteral("10.8.0.2"), QStringLiteral("VPN"),
                      QStringLiteral("A VPN adapter (WireGuard Tunnel) is present but the internet route does not use it."))
                << it("general.route", "Internet route", DiagLevel::Ok, QStringLiteral("Through Ethernet (192.168.1.23)"), QStringLiteral("gateway 192.168.1.1"))
                << it("general.proxy", "Proxy", DiagLevel::Ok, QStringLiteral("None"))
                << it("general.bind", "Bind interface", DiagLevel::Ok, QStringLiteral("All adapters"))
                << it("general.router", "Router (UPnP)", DiagLevel::Ok, QStringLiteral("Home Router (Example HR-1000)"), QStringLiteral("at 192.168.1.1"))
                << it("general.wan", "Router internet address", DiagLevel::Ok, QStringLiteral("203.0.113.45"), QStringLiteral("UPnP"))
                << it("general.seen", "Seen from the internet as", DiagLevel::Ok, QStringLiteral("203.0.113.45"),
                      QStringLiteral("203.0.113.45 (BitTorrent peers and trackers), 203.0.113.45 (eD2k server eMule Security No1)"))
                << it("general.nat", "NAT", DiagLevel::Ok, QStringLiteral("One router, public address"), QString(),
                      QStringLiteral("Your router has the public address 203.0.113.45, port forwarding can work."))
                << it("general.limit.down", "Download limit", DiagLevel::Ok, QStringLiteral("Unlimited"))
                << it("general.limit.up", "Upload limit", DiagLevel::Ok, QStringLiteral("64 KB/s"))
                << it("general.slots", "Download slots", DiagLevel::Ok, QStringLiteral("5 active of 5, torrents 5 at once"), QStringLiteral("sources per download 400"),
                      QStringLiteral("More torrents than \"Active torrent downloads\" wait as \"Queued by BitTorrent\" until one finishes or is paused."))
                << it("general.firewall", "Windows Firewall", DiagLevel::Limited, QStringLiteral("On, no rule for OctoMX"), QStringLiteral("Private"),
                      QStringLiteral("Windows Firewall has no rule for OctoMX, so incoming connections are blocked: allow it in Windows Security ▸ Firewall & network protection ▸ Allow an app through firewall."));
        r.groups << g;
        auto net = [&](Net n, DiagLevel lv, const QString &result, const QList<DiagItem> &items, const QString &explain = QString()) {
            DiagGroup x;
            x.id = QStringLiteral("net.") + netName(n);
            x.title = netName(n);
            x.net = n;
            x.level = lv;
            x.result = result;
            x.items = items;
            x.explain = explain;
            r.groups << x;
        };
        const QString checked = m_lastTest.isValid() ? QStringLiteral("Answers (38 ms)") : QStringLiteral("Not tested");
        net(Net::BitTorrent, DiagLevel::Ok, QStringLiteral("Works, reachable from the internet"),
            {it("bittorrent.state", "State", DiagLevel::Ok, QStringLiteral("Connected"), QStringLiteral("DHT: 412 nodes, 3 torrents")),
             it("bittorrent.port.tcp.6881", "Listen TCP 6881", DiagLevel::Ok, QStringLiteral("Listening"), QStringLiteral("peers")),
             it("bittorrent.port.udp.6881", "Listen UDP 6881", DiagLevel::Ok, QStringLiteral("Listening"), QStringLiteral("DHT and uTP peers")),
             it("bittorrent.map.tcp.6881", "Port mapping TCP 6881", DiagLevel::Ok, QStringLiteral("Mapped by UPnP"), QStringLiteral("2 min ago, lease until 19:58")),
             it("bittorrent.map.udp.6881", "Port mapping UDP 6881", DiagLevel::Ok, QStringLiteral("Mapped by UPnP"), QStringLiteral("2 min ago, lease until 19:58")),
             it("bittorrent.reach.tcp", "Reachable from the internet (TCP)", DiagLevel::Ok, QStringLiteral("Yes"), QStringLiteral("14 incoming peer connections in the last 15 min")),
             it("bittorrent.reach.udp", "Reachable from the internet (UDP)", DiagLevel::Ok, QStringLiteral("Yes"), QStringLiteral("3 incoming uTP connections in the last 15 min")),
             it("bittorrent.dns", "Name lookups (DNS)", m_lastTest.isValid() ? DiagLevel::Ok : DiagLevel::Unknown,
                m_lastTest.isValid() ? QStringLiteral("8 of 8 resolve") : QStringLiteral("Not tested"), QStringLiteral("8 host names")),
             it("bittorrent.dht", "DHT", DiagLevel::Ok, QStringLiteral("412 nodes"), QStringLiteral("18 342 answers received")),
             it("bittorrent.udptracker.tracker.opentrackr.org:1337", "UDP tracker tracker.opentrackr.org:1337",
                m_lastTest.isValid() ? DiagLevel::Ok : DiagLevel::Unknown, checked)});
        net(Net::ED2K, DiagLevel::Limited, QStringLiteral("Works, not reachable from the internet"),
            {it("ed2k.state", "State", DiagLevel::Ok, QStringLiteral("Connected"), QStringLiteral("eMule Security No1, LowID")),
             it("ed2k.port.tcp.4662", "Listen TCP 4662", DiagLevel::Ok, QStringLiteral("Listening"), QStringLiteral("eD2k peers")),
             it("ed2k.port.udp.4672", "Listen UDP 4672", DiagLevel::Ok, QStringLiteral("Listening"), QStringLiteral("eD2k client UDP and Kad")),
             it("ed2k.map.tcp.4662", "Port mapping TCP 4662", DiagLevel::Limited, QStringLiteral("Failed"), QStringLiteral("UPnP: 500 Internal Server Error (UPnP error 718: ConflictInMappingEntry)"),
                QStringLiteral("The router did not forward this port automatically (500 Internal Server Error). Enable UPnP in the router, or forward it by hand: In your router, forward TCP port 4662 to 192.168.1.23.")),
             it("ed2k.map.udp.4672", "Port mapping UDP 4672", DiagLevel::Ok, QStringLiteral("Mapped by UPnP"), QStringLiteral("2 min ago")),
             it("ed2k.reach.tcp", "Reachable from the internet (TCP)", DiagLevel::Limited, QStringLiteral("No"),
                QStringLiteral("server eMule Security No1 assigned a LowID (1234567): it could not connect back to TCP port 4662"),
                QStringLiteral("Others cannot connect to you over TCP on eD2k: downloads work, but with fewer sources (two firewalled users cannot connect to each other). ") + fwd),
             it("ed2k.reach.udp", "Reachable from the internet (UDP)", DiagLevel::Unknown, QStringLiteral("Unknown")),
             it("ed2k.id", "Client ID", DiagLevel::Limited, QStringLiteral("LowID"), QStringLiteral("1234567 on eMule Security No1"),
                QStringLiteral("A LowID means the server could not open a connection to your TCP port 4662. You can still download, but not from other LowID users, and you get fewer sources."))},
            fwd);
        net(Net::Kad, DiagLevel::Limited, QStringLiteral("Works, not reachable from the internet"),
            {it("kad.state", "State", DiagLevel::Ok, QStringLiteral("Connected"), QStringLiteral("1203 contacts, firewalled")),
             it("kad.reach.tcp", "Reachable from the internet (TCP)", DiagLevel::Limited, QStringLiteral("No"), QStringLiteral("Kad firewall check: 4 nodes answered, none could connect back"), fwd),
             it("kad.reach.udp", "Reachable from the internet (UDP)", DiagLevel::Ok, QStringLiteral("Yes"), QStringLiteral("Kad UDP firewall test packet arrived")),
             it("kad.dht", "Kad contacts", DiagLevel::Ok, QStringLiteral("1203 nodes"), QStringLiteral("5 120 answers received"))},
            fwd);
        net(Net::Gnutella, DiagLevel::Unknown, QStringLiteral("Connecting..."),
            {it("gnutella.state", "State", DiagLevel::Unknown, QStringLiteral("Connecting..."), QStringLiteral("2 of 3 ultrapeers")),
             it("gnutella.reach.tcp", "Reachable from the internet (TCP)", DiagLevel::Unknown, QStringLiteral("Unknown"), QStringLiteral("waiting for the connect-back"))});
        net(Net::Overlay, DiagLevel::Ok, QStringLiteral("Works, reachable from the internet"),
            {it("octomx.state", "State", DiagLevel::Ok, QStringLiteral("Connected"), QStringLiteral("5 peers")),
             it("octomx.reach.tcp", "Reachable from the internet (TCP)", DiagLevel::Ok, QStringLiteral("Yes"), QStringLiteral("other OctoMX users connected to us over TCP"))});
        // [set-1] one index that does not answer: the demo shows every level of the Connection Test colours
        net(Net::Web, DiagLevel::Limited, QStringLiteral("Works, one index does not answer"),
            {it("web.state", "State", DiagLevel::Ok, QStringLiteral("Connected"), QStringLiteral("3 index sources")),
             it("web.index.3", "Index 3 (demo)", DiagLevel::Broken, QStringLiteral("Not answering"), QStringLiteral("timed out after 20 s"))});
        for (const TransferInfo &t : m_tr->downloads()) {
            if (t.diagnosis.isEmpty()) continue;
            DiagGroup d;
            d.id = diagGroupIdForDownload(t.id);
            d.title = t.name;
            d.downloadId = t.id;
            d.level = DiagLevel(t.diagnosisLevel);
            d.result = t.diagnosis;
            d.items << it("dl.diagnosis", "Diagnosis", d.level, t.diagnosis);
            if (t.id == QLatin1String("d8"))
                d.items << it("dl.index", "Index numbers", DiagLevel::Unknown, QStringLiteral("192 seeds, 295 peers"), QStringLiteral("reported by BitSearch"))
                        << it("dl.known", "Peers known", DiagLevel::Unknown, QStringLiteral("0 peers (0 seeds)"))
                        << it("dl.dht", "DHT lookup", DiagLevel::Limited, QStringLiteral("3 lookups returned 0 peers"), QStringLiteral("412 nodes in the routing table"))
                        << it("dl.tracker", "Tracker tracker.opentrackr.org", DiagLevel::Ok, QStringLiteral("Works, 0 peers"));
            else if (t.id == QLatin1String("d3"))
                d.items << it("dl.known", "Peers known", DiagLevel::Ok, QStringLiteral("80 peers (31 seeds)"), QStringLiteral("from trackers 64, DHT 22"))
                        << it("dl.connect", "Connections", DiagLevel::Ok, QStringLiteral("2 connected (1 seed), 61 attempts"),
                              QStringLiteral("failed: 44 timed out, 9 refused, 4 handshake, 0 banned, 2 other"));
            r.groups << d;
        }
        r.level = DiagLevel::Limited;
        r.summary = QStringLiteral("Works, but you cannot be reached from the internet on eD2k, Kad (slower, fewer sources).");
        r.logLines << QStringLiteral("2026-11-20 18:00:03.512 W mx.diag: port mapping: UPnP TCP 4662 failed (eD2k): 500 Internal Server Error (UPnP error 718: ConflictInMappingEntry)")
                   << QStringLiteral("2026-11-20 18:00:07.120 W mx.ed2k: connected to eMule Security No1, LowID (ID 1234567)");
        if (!m_portCheck.isEmpty()) {
            for (DiagGroup &x : r.groups) {
                if (x.net == Net::BitTorrent)
                    x.items << it("bittorrent.portcheck", "Port check (portcheck.transmissionbt.com)", DiagLevel::Ok, QStringLiteral("TCP 6881 open"));
                if (x.net == Net::ED2K)
                    x.items << it("ed2k.portcheck", "Port check (portcheck.transmissionbt.com)", DiagLevel::Limited, QStringLiteral("TCP 4662 closed"));
            }
        }
        return r;
    }
    void startTest() override
    {
        if (m_running) return;
        m_running = true;
        m_percent = 0;
        m_step = QStringLiteral("Checking adapters and ports...");
        emit testProgress(0, m_step);
        m_timer.start();
    }
    void cancelTest() override
    {
        if (!m_running) return;
        m_timer.stop();
        m_running = false;
        m_step = QStringLiteral("Cancelled");
        emit testFinished(true);
        emit reportChanged();
    }
    bool isTesting() const override { return m_running; }
    QString portCheckService() const override
    {
        const QString t = m_settings->s(QString(keys::diag::PortCheckUrl));
        return t.contains(QLatin1String("%1")) ? QUrl(QString(t).replace(QLatin1String("%1"), QStringLiteral("1"))).host() : QString();
    }
    void checkPortsOnline() override
    {
        QTimer::singleShot(m_frozen ? 0 : 300, this, [this] {
            m_portCheck = QStringLiteral("done");
            emit reportChanged();
        });
    }
    QString downloadDiagnosis(const QString &id) const override { return m_tr->transfer(id).diagnosis; }

private:
    QDateTime now() const
    {
        return m_frozen ? QDateTime(QDate(2026, 11, 20), QTime(18, 5), QTimeZone::UTC) : QDateTime::currentDateTime();
    }
    Settings *m_settings;
    TransferService *m_tr;
    bool m_frozen;
    QTimer m_timer;
    bool m_running = false;
    int m_percent = 0;
    QString m_step;
    QDateTime m_lastTest;
    QString m_portCheck;
};

// ----------------------------------------------------------------------------------- core
class MockCore : public Core {
public:
    MockCore(const QString &profile, const MockOptions &opt, QObject *parent) : Core(parent), m_profile(profile), m_opt(opt)
    {
        registerMetaTypes();
        m_settings = new MockSettings(profile, this);
        m_settings->setRuntimeDefault(QString(keys::transfers::IncomingDir),
                                      QDir(profile.isEmpty() ? QDir::tempPath() : profile).filePath(QStringLiteral("incoming")));
        m_network = new MockNetwork(m_settings, opt.frozen, this);
        m_search = new MockSearch(m_settings, opt.frozen, this);
        m_transfers = new MockTransfers(m_settings, profile, opt.frozen, this);
        m_library = new MockLibrary(m_settings, opt.frozen, this);
        m_chat = new MockChat(m_settings, opt.frozen, this);
        m_users = new MockUsers(m_settings, opt.frozen, this);
        m_chat->setUsers(m_users);
        m_bandwidth = new MockBandwidth(opt.frozen, m_network, m_transfers, this);
        m_diag = new MockDiagnostics(m_settings, m_transfers, opt.frozen, this);   // [diag-1]
    }
    Settings *settings() override { return m_settings; }
    NetworkService *network() override { return m_network; }
    SearchService *search() override { return m_search; }
    TransferService *transfers() override { return m_transfers; }
    LibraryService *library() override { return m_library; }
    ChatService *chat() override { return m_chat; }
    UserService *users() override { return m_users; }
    BandwidthService *bandwidth() override { return m_bandwidth; }
    DiagnosticsService *diagnostics() override { return m_diag; }
    QString profileDir() const override { return m_profile; }
    QString versionString() const override { return QStringLiteral(MX_VERSION_STRING); }
    void start() override { emit statusMessage(tr("Demo mode - no network access")); }
    void shutdown() override
    {
        m_settings->sync();
        QTimer::singleShot(50, this, [this] { emit shutdownFinished(); });
    }
    bool isMock() const override { return true; }

private:
    QString m_profile;
    MockOptions m_opt;
    MockSettings *m_settings;
    MockNetwork *m_network;
    MockSearch *m_search;
    MockTransfers *m_transfers;
    MockLibrary *m_library;
    MockChat *m_chat;
    MockUsers *m_users;
    MockBandwidth *m_bandwidth;
    MockDiagnostics *m_diag;
};

} // namespace

Core *createMockCoreWithOptions(const QString &profileDir, const MockOptions &opt, QObject *parent)
{
    return new MockCore(profileDir, opt, parent);
}

bool mockSetHotlistOnline(Core *core, const QString &name, bool online)
{
    auto *u = core ? dynamic_cast<MockUsers *>(core->users()) : nullptr;
    return u && u->setOnline(name, online);
}

Core *createMockCore(const QString &profileDir, QObject *parent)
{
    MockOptions o;
    o.frozen = qEnvironmentVariableIntValue("MX_DEMO_FROZEN") != 0;
    return createMockCoreWithOptions(profileDir, o, parent);
}

} // namespace mx
