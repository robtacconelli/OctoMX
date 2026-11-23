// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX tests: reusable fake network plugins (header-only; add this header to the test target's
// sources so AUTOMOC processes it). Used by the core tests and meant for the engine tests too.
//
//   auto *p = new mx::test::FakePlugin(mx::Net::ED2K, mx::NetworkPlugin::CanSearch | mx::NetworkPlugin::CanServe);
//   p->cannedHits << mx::test::FakePlugin::makeHit("song.mp3", 5'000'000, hashes);   // answered to every search
//   p->setRangeData(hashes, bytes);   // openSource() then returns a FakeRangeSource serving these bytes
//
// Every call is recorded (initCount, startCount, searches, sharedLists, ...) so tests can assert on it.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include "mx/plugin.h"

namespace mx::test {

// Serves byte ranges of an in-memory file. Goes Connecting → Ready shortly after open().
class FakeRangeSource : public RangeSource {
    Q_OBJECT
public:
    FakeRangeSource(const SearchSource &desc, const QByteArray &data, QObject *parent = nullptr)
        : RangeSource(parent), m_desc(desc), m_data(data) {}

    int openDelayMs = 0;
    int chunkSize = 64 * 1024;       // data() chunk size
    bool corrupt = false;            // flip a byte in every delivered chunk
    bool failRequests = false;
    QList<QPair<qint64, qint64>> requests;

    SearchSource descriptor() const override { return m_desc; }
    State state() const override { return m_state; }
    QString stateText() const override { return m_state == Ready ? QStringLiteral("Ready") : QStringLiteral("Connecting..."); }
    QString clientName() const override { return QStringLiteral("FakeClient 1.0"); }
    QList<QPair<qint64, qint64>> available() const override { return {{0, m_data.size()}}; }
    int maxPendingRequests() const override { return 2; }
    void open() override
    {
        setState(Connecting);
        QTimer::singleShot(openDelayMs, this, [this] { if (m_state == Connecting) setState(Ready); });
    }
    void request(qint64 offset, qint64 length) override
    {
        requests << qMakePair(offset, length);
        QTimer::singleShot(0, this, [this, offset, length] {
            if (m_state == Closed) return;
            if (failRequests || offset < 0 || offset + length > m_data.size()) {
                emit requestFinished(offset, length, false, QStringLiteral("range not available"));
                return;
            }
            for (qint64 o = offset; o < offset + length; o += chunkSize) {
                QByteArray part = m_data.mid(o, qMin<qint64>(chunkSize, offset + length - o));
                if (corrupt && !part.isEmpty()) part[0] = char(part.at(0) ^ 0x5a);
                emit data(o, part);
            }
            emit requestFinished(offset, length, true, QString());
        });
    }
    void cancelRequests() override {}
    void close() override { setState(Closed); }

private:
    void setState(State s) { if (m_state != s) { m_state = s; emit stateChanged(); } }
    SearchSource m_desc;
    QByteArray m_data;
    State m_state = Idle;
};

class FakePlugin : public NetworkPlugin {
    Q_OBJECT
public:
    FakePlugin(Net net, int caps, QObject *parent = nullptr)
        : NetworkPlugin(parent), m_net(net), m_caps(caps)
    {
        st.net = net;
        st.state = ConnState::Disconnected;
    }

    // ---- behaviour knobs
    UserRef me;                      // [ovl-2] localUser(): the network's own identity (invalid: none)
    UserRef localUser() const override { return me; }
    QList<SearchHit> cannedHits;     // returned for every search (queryId filled in)
    bool autoRespond = true;         // answer searches automatically
    bool sendDone = true;            // ... and send searchDone afterwards
    int respondDelayMs = 0;
    bool connectOnStart = true;      // start() → Connected (else stays Connecting)
    int peersWhenConnected = 3;
    NetworkStatus st;
    QList<ServerEntry> serverList;

    // ---- recorded calls
    int initCount = 0, startCount = 0, stopCount = 0;
    PluginContext ctx;
    QList<SearchQuery> searches;
    QList<quint32> cancelledSearches;
    QList<QList<SharedFile>> sharedLists;
    QStringList findSourcesCalls;    // download ids
    QStringList stoppedFindSources;
    QList<QPair<quint32, UserRef>> browseCalls;
    QList<UserRef> whoisCalls;
    QStringList cancelledUploads;
    QStringList addedServers, removedServers, connectedServers;
    QList<QPair<FileHashes, qint64>> hashLinkLookups;
    QList<FileHashes> publishedLinks;
    QList<QPointer<FakeRangeSource>> openedSources;
    QList<QList<UserRef>> hotlists;          // setHotlist() calls
    QList<FileHashes> bundleRequests;        // requestBundleFiles() calls
    QHash<QByteArray, QList<BundleFile>> bundleAnswers;   // btih → files, answered asynchronously

    void setRangeData(const FileHashes &h, const QByteArray &bytes) { m_ranges.append({h, bytes}); }

    static SearchHit makeHit(const QString &name, qint64 size, const FileHashes &h = FileHashes(),
                             Net net = Net::Unknown, const QString &user = QString())
    {
        SearchHit hit;
        hit.fileName = name;
        hit.size = size;
        hit.hashes = h;
        SearchSource s;
        s.net = net;
        if (!user.isEmpty()) {
            s.user.net = net;
            s.user.id = user;
            s.user.name = user;
        }
        s.uri = QStringLiteral("fake://%1/%2").arg(user, name);
        hit.sources << s;
        return hit;
    }

    // ---- emit helpers
    void setConnState(ConnState s, const QString &text = QString())
    {
        st.state = s;
        st.statusText = text.isEmpty() ? (s == ConnState::Connected ? QStringLiteral("Connected") : QString()) : text;
        st.peers = s == ConnState::Connected ? peersWhenConnected : 0;
        emit statusChanged(st);
    }
    void emitHits(quint32 id, const QList<SearchHit> &hits) { emit searchHits(id, hits); }
    void emitDone(quint32 id) { emit searchDone(id); }
    void emitUpload(const TransferInfo &t) { emit uploadChanged(t); }
    void emitUploadFinished(const QString &id) { emit uploadFinished(id); }
    void emitBundleFiles(const FileHashes &h, const QList<BundleFile> &files) { emit bundleFiles(h, files); }

    // ---- NetworkPlugin
    Net net() const override { return m_net; }
    int capabilities() const override { return m_caps; }
    void init(const PluginContext &c) override { ctx = c; ++initCount; }
    void start() override
    {
        ++startCount;
        setConnState(connectOnStart ? ConnState::Connected : ConnState::Connecting);
    }
    void stop() override
    {
        ++stopCount;
        setConnState(ConnState::Disconnected);
    }
    NetworkStatus status() const override { return st; }

    QList<ServerEntry> servers() const override { return serverList; }
    void addServer(const QString &address, const QString &name) override
    {
        addedServers << address;
        ServerEntry e;
        e.net = m_net;
        e.id = address;
        e.address = address;
        e.name = name;
        serverList << e;
        emit serversChanged();
    }
    void removeServer(const QString &id) override
    {
        removedServers << id;
        serverList.removeIf([&](const ServerEntry &e) { return e.id == id; });
        emit serversChanged();
    }
    void connectServer(const QString &id) override { connectedServers << id; }

    void search(const SearchQuery &q) override
    {
        searches << q;
        if (!autoRespond) return;
        QTimer::singleShot(respondDelayMs, this, [this, q] {
            QList<SearchHit> hits = cannedHits;
            for (SearchHit &h : hits) h.queryId = q.id;
            if (!hits.isEmpty()) emit searchHits(q.id, hits);
            if (sendDone) emit searchDone(q.id);
        });
    }
    void cancelSearch(quint32 queryId) override { cancelledSearches << queryId; }
    void findSources(const QString &downloadId, const FileHashes &, qint64, const QString &) override
    { findSourcesCalls << downloadId; }
    void stopFindSources(const QString &downloadId) override { stoppedFindSources << downloadId; }
    RangeSource *openSource(const QString &, const FileHashes &h, qint64, const SearchSource &src) override
    {
        for (const auto &r : std::as_const(m_ranges)) {
            if (r.first.matches(h)) {
                auto *s = new FakeRangeSource(src, r.second);
                openedSources << s;
                return s;
            }
        }
        return nullptr;
    }
    void setSharedFiles(const QList<SharedFile> &files) override { sharedLists << files; }
    void lookupHashLinks(const FileHashes &known, qint64 size) override { hashLinkLookups << qMakePair(known, size); }
    void publishHashLinks(const FileHashes &all, qint64, const QString &) override { publishedLinks << all; }
    void browse(quint32 requestId, const UserRef &u) override { browseCalls << qMakePair(requestId, u); }
    void whois(const UserRef &u) override { whoisCalls << u; }
    void cancelUpload(const QString &uploadId) override { cancelledUploads << uploadId; }
    void setHotlist(const QList<UserRef> &users) override { hotlists << users; }
    void requestBundleFiles(const FileHashes &h) override
    {
        bundleRequests << h;
        if (!bundleAnswers.contains(h.btih)) return;
        const QList<BundleFile> files = bundleAnswers.value(h.btih);
        QTimer::singleShot(0, this, [this, h, files] { emit bundleFiles(h, files); });
    }

private:
    Net m_net;
    int m_caps;
    QList<QPair<FileHashes, QByteArray>> m_ranges;
};

// A fake chat network (overlay-like): FakePlugin + ChatBackend.
class FakeChatPlugin : public FakePlugin, public ChatBackend {
    Q_OBJECT
public:
    explicit FakeChatPlugin(QObject *parent = nullptr)
        : FakePlugin(Net::Overlay, CanChat | CanBrowse | CanSearch, parent), events(new ChatEvents(this)) {}

    ChatEvents *events;
    QList<ChatRoomInfo> rooms;                       // room list
    QHash<QString, QList<ChatUser>> users;           // room id → users
    QStringList joinCalls, leaveCalls;
    struct Sent { QString roomId; QString text; ChatKind kind; };
    QList<Sent> sentRoom;
    QList<QPair<UserRef, QString>> sentPrivate;
    QList<QPair<QString, QString>> topics;
    int refreshCount = 0;

    // simulate traffic
    void receive(const QString &roomId, const UserRef &from, const QString &text, ChatKind kind = ChatKind::Message)
    {
        ChatMessage m;
        m.roomId = roomId;
        m.from = from;
        m.text = text;
        m.kind = kind;
        m.time = QDateTime::currentDateTime();
        emit events->messageReceived(m);
    }
    static UserRef user(const QString &name)
    {
        UserRef u;
        u.net = Net::Overlay;
        u.id = QStringLiteral("id-") + name;
        u.name = name;
        return u;
    }

    void refreshRoomList() override { ++refreshCount; emit events->roomListChanged(); }
    QList<ChatRoomInfo> roomList() const override { return rooms; }
    void joinRoom(const QString &roomIdOrName) override
    {
        joinCalls << roomIdOrName;
        ChatRoomInfo r;
        r.id = roomIdOrName;
        r.name = roomIdOrName;
        r.users = users.value(r.id).size();
        QTimer::singleShot(0, events, [this, r] { emit events->roomJoined(r); });
    }
    void leaveRoom(const QString &roomId) override
    {
        leaveCalls << roomId;
        QTimer::singleShot(0, events, [this, roomId] { emit events->roomLeft(roomId); });
    }
    QList<ChatUser> roomUsers(const QString &roomId) const override { return users.value(roomId); }
    void sendRoomMessage(const QString &roomId, const QString &text, ChatKind kind) override
    { sentRoom << Sent{roomId, text, kind}; }
    void sendPrivateMessage(const UserRef &to, const QString &text) override { sentPrivate << qMakePair(to, text); }
    void setTopic(const QString &roomId, const QString &topic) override { topics << qMakePair(roomId, topic); }
    void setMotd(const QString &roomId, const QString &motd) override { motds << qMakePair(roomId, motd); }
    QList<QPair<QString, QString>> motds;
    QObject *chatEvents() override { return events; }
};

} // namespace mx::test
