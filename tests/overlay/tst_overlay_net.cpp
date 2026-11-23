// SPDX-License-Identifier: GPL-3.0-or-later
// [ovl-2] The OctoMX network between real nodes: private messages with delivery receipts, refusals,
// retries; presence (bye, lost link, mesh ping, relayed timeout, coming back); browse and transfers by
// connect-back and through a relayed circuit; Hotlist-only uploads and Hotlist priority; an older (4.2)
// node. Every node is a real OverlayPlugin with its own librats node on real sockets (127.0.0.1); the
// topologies are built by explicit dials: direct (A-B), relayed (A-C-B) and firewalled (a node that
// refuses direct inbound links, as behind a home router without a forwarded port).
#include "ovl_test_support.h"

#include <QtCore/QJsonArray>

using namespace mx::ovl;
using namespace ovltest;

namespace {

struct Opts {
    bool firewalled = false;
    bool relay = true;   // overlay/relay: carry relayed circuits for others
};

OverlayTimings fastTimings()
{
    OverlayTimings t;
    t.presenceMs = 3000;
    t.presenceTimeoutMs = 8000;
    t.presenceGraceMs = 2000;
    t.pingWaitMs = 2000;
    t.pmAckMs = 1500;
    t.pmRelayAckMs = 4000;
    t.pmRetryMs = 3000;
    t.heartbeatMs = 1000;
    t.memberTimeoutMs = 3500;
    t.directoryMs = 1000;
    t.historyDelayMs = 1200;
    t.searchQuietMs = 2500;
    t.searchMaxMs = 8000;
    t.sourcesRepeatMs = 3000;
    t.dialWaitMs = 9000;
    return t;
}

std::unique_ptr<Instance> node(const QString &name, Opts o = {}, const QString &profile = QString())
{
    auto i = std::make_unique<Instance>(name, profile);
    OverlayPlugin::TestOptions to;
    to.dht = to.mdns = to.reconnect = to.pex = false;
    to.listenPort = 0;
    // a firewalled node: direct inbound links are refused, its announced addresses lead nowhere (here:
    // none are announced, as dials to a NAT without a forwarded port never complete) and no hole punch
    // opens it: only connect-back and relayed circuits reach it
    to.firewalled = o.firewalled;
    to.advertise = !o.firewalled;
    i->plugin->setTestOptions(to);
    i->plugin->setTimings(fastTimings());
    i->settings.setValue(QString(mx::keys::overlay::Relay), o.relay);
    return i;
}

struct Status {
    QString id;
    mx::PmDelivery st;
    QString detail;
};
QList<Status> statuses(const QSignalSpy &spy, const QString &id)
{
    QList<Status> out;
    for (const auto &a : spy)
        if (a.at(1).toString() == id) out << Status{id, mx::PmDelivery(a.at(2).toInt()), a.at(3).toString()};
    return out;
}
bool hasStatus(const QSignalSpy &spy, const QString &id, mx::PmDelivery st)
{
    for (const Status &s : statuses(spy, id))
        if (s.st == st) return true;
    return false;
}
QString detailOf(const QSignalSpy &spy, const QString &id, mx::PmDelivery st)
{
    for (const Status &s : statuses(spy, id))
        if (s.st == st) return s.detail;
    return QString();
}
QList<mx::ChatMessage> pms(const QSignalSpy &spy, const QString &text = QString())
{
    QList<mx::ChatMessage> out;
    for (const auto &a : spy) {
        const auto m = a.first().value<mx::ChatMessage>();
        if (m.kind == mx::ChatKind::Private && (text.isEmpty() || m.text == text)) out << m;
    }
    return out;
}
int offlineEvents(const QSignalSpy &spy, const QString &userId)
{
    int n = 0;
    for (const auto &a : spy)
        if (a.at(0).value<mx::UserRef>().id == userId && !a.at(1).toBool()) ++n;
    return n;
}
mx::SearchSource sourceFor(Instance &uploader)
{
    mx::SearchSource s;
    s.net = mx::Net::Overlay;
    s.user = uploader.me();
    s.extra.insert(QStringLiteral("node"), uploader.plugin->nodeId());
    return s;
}
bool roomHas(Instance &i, const QString &room, const QString &userId)
{
    for (const auto &u : i.chat->roomUsers(room))
        if (u.user.id == userId) return true;
    return false;
}
// simulate a crash: the librats node goes away without bye / part
void crash(Instance &i) { i.plugin->node()->stop(); }

// Drives a RangeSource like the engine: sequential, pipelined, hashing on the fly.
class Fetcher : public QObject {
public:
    mx::RangeSource *src;
    qint64 size, next = 0, done = 0;
    int pending = 0;
    QCryptographicHash sha1{QCryptographicHash::Sha1};
    bool failed = false, sawRelayed = false;
    QString error;
    Fetcher(mx::RangeSource *s, qint64 sz) : src(s), size(sz)
    {
        connect(src, &mx::RangeSource::data, this, [this](qint64, const QByteArray &b) {
            sha1.addData(b);
            if (src->stateText().contains(QStringLiteral("relayed"))) sawRelayed = true;
        });
        connect(src, &mx::RangeSource::requestFinished, this, [this](qint64, qint64 len, bool ok, const QString &e) {
            --pending;
            if (!ok) { failed = true; error = e; return; }
            done += len;
            pump();
        });
        connect(src, &mx::RangeSource::stateChanged, this, [this] {
            if (src->state() == mx::RangeSource::Failed) { failed = true; error = src->stateText(); }
            pump();
        });
    }
    void pump()
    {
        if (failed) return;
        if (src->state() != mx::RangeSource::Ready && src->state() != mx::RangeSource::Busy) return;
        while (pending < src->maxPendingRequests() && next < size) {
            const qint64 len = qMin(src->preferredRequestSize(), size - next);
            ++pending;
            const qint64 off = next;
            next += len;
            src->request(off, len);
        }
    }
    bool complete() const { return done == size; }
};

const QString kId1 = QStringLiteral("00000000000000a1");

} // namespace

class TstOverlayNet : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { mx::registerMetaTypes(); }

    // ---------------------------------------------------------------- private messages
    void pmDirectReceipt()
    {
        auto A = node(QStringLiteral("Alice")), B = node(QStringLiteral("Bob"));
        A->start();
        B->start();
        QVERIFY(link(*A, *B));
        QSignalSpy st(A->events, &mx::ChatEvents::privateMessageStatus);
        QSignalSpy rxB(B->events, &mx::ChatEvents::messageReceived);
        QVERIFY(A->chat->sendPrivateMessageWithId(B->me(), QStringLiteral("hello Bob"), kId1));
        QVERIFY(hasStatus(st, kId1, mx::PmDelivery::Sending));
        QVERIFY(waitFor([&] { return pms(rxB).size() == 1; }));
        const auto m = pms(rxB).first();
        QCOMPARE(m.text, QStringLiteral("hello Bob"));
        QCOMPARE(m.msgId, kId1);
        QVERIFY(!m.to.isValid());                 // the UI files it as Alice's message, not as Bob's own
        QCOMPARE(m.from.id, A->me().id);
        QVERIFY(waitFor([&] { return hasStatus(st, kId1, mx::PmDelivery::Delivered); }, 5000));
        QVERIFY(!hasStatus(st, kId1, mx::PmDelivery::NotDelivered));
    }

    void pmRelayedReceipt()
    {
        // A and B know each other only through C; B is firewalled (A cannot dial it)
        auto A = node(QStringLiteral("Alice")), B = node(QStringLiteral("Bob"), {true}), C = node(QStringLiteral("Carol"));
        A->start();
        B->start();
        C->start();
        QVERIFY(link(*C, *A));
        QVERIFY(link(*C, *B));
        QVERIFY2(waitFor([&] { return A->plugin->isUserOnline(B->me()) && B->plugin->isUserOnline(A->me()); }, 15000),
                 "presence did not travel through Carol");
        QSignalSpy stA(A->events, &mx::ChatEvents::privateMessageStatus);
        QSignalSpy rxB(B->events, &mx::ChatEvents::messageReceived);
        QSignalSpy rxC(C->events, &mx::ChatEvents::messageReceived);
        QVERIFY(A->chat->sendPrivateMessageWithId(B->me(), QStringLiteral("through Carol"), kId1));
        QVERIFY2(waitFor([&] { return pms(rxB).size() == 1; }, 8000), "relayed PM lost");
        QVERIFY(waitFor([&] { return hasStatus(stA, kId1, mx::PmDelivery::Delivered); }, 8000));
        QCOMPARE(pms(rxC).size(), 0);   // Carol carried it but cannot read it
        QVERIFY(!A->plugin->node()->isConnected(B->plugin->nodeId()) ||
                A->plugin->node()->peerTransport(B->plugin->nodeId()) == QLatin1String("relay"));
        // and back
        QSignalSpy stB(B->events, &mx::ChatEvents::privateMessageStatus);
        QSignalSpy rxA(A->events, &mx::ChatEvents::messageReceived);
        const QString id2 = QStringLiteral("00000000000000b2");
        QVERIFY(B->chat->sendPrivateMessageWithId(A->me(), QStringLiteral("back to Alice"), id2));
        QVERIFY(waitFor([&] { return pms(rxA).size() == 1; }, 8000));
        QVERIFY(waitFor([&] { return hasStatus(stB, id2, mx::PmDelivery::Delivered); }, 8000));
    }

    void pmRefusals()
    {
        // direct A-B, C linked to B only: A -> C goes through B
        auto A = node(QStringLiteral("Alice")), B = node(QStringLiteral("Bob")), C = node(QStringLiteral("Carol"), {true});
        A->start();
        B->start();
        C->start();
        QVERIFY(link(*A, *B));
        QVERIFY(link(*B, *C));
        QVERIFY(waitFor([&] { return A->plugin->isUserOnline(C->me()) && C->plugin->isUserOnline(A->me()); }, 15000));
        QSignalSpy st(A->events, &mx::ChatEvents::privateMessageStatus);
        QSignalSpy rxB(B->events, &mx::ChatEvents::messageReceived);
        // "Hotlist users only", direct: refused with a polite reason, then accepted once on the Hotlist
        B->settings.setValue(QString(mx::keys::messages::AllowPrivateMessages), QStringLiteral("hotlist"));
        QVERIFY(A->chat->sendPrivateMessageWithId(B->me(), QStringLiteral("may I?"), QStringLiteral("00000000000000c1")));
        QVERIFY(waitFor([&] { return hasStatus(st, QStringLiteral("00000000000000c1"), mx::PmDelivery::Refused); }));
        QVERIFY(detailOf(st, QStringLiteral("00000000000000c1"), mx::PmDelivery::Refused).contains(QStringLiteral("Hotlist")));
        QCOMPARE(pms(rxB).size(), 0);
        B->plugin->setHotlist({A->me()});
        QVERIFY(A->chat->sendPrivateMessageWithId(B->me(), QStringLiteral("now?"), QStringLiteral("00000000000000c2")));
        QVERIFY(waitFor([&] { return hasStatus(st, QStringLiteral("00000000000000c2"), mx::PmDelivery::Delivered); }));
        QCOMPARE(pms(rxB, QStringLiteral("now?")).size(), 1);
        // relayed and refused: Carol accepts messages from nobody
        C->settings.setValue(QString(mx::keys::messages::AllowPrivateMessages), QStringLiteral("nobody"));
        QVERIFY(A->chat->sendPrivateMessageWithId(C->me(), QStringLiteral("hi Carol"), QStringLiteral("00000000000000c3")));
        QVERIFY(waitFor([&] { return hasStatus(st, QStringLiteral("00000000000000c3"), mx::PmDelivery::Refused); }, 8000));
        QVERIFY(detailOf(st, QStringLiteral("00000000000000c3"), mx::PmDelivery::Refused).contains(QStringLiteral("not accepting")));
        // the core refuses by a rule only it knows ("only from the same room"): the sender is told
        C->settings.setValue(QString(mx::keys::messages::AllowPrivateMessages), QStringLiteral("everyone"));
        QObject ctx;
        connect(C->events, &mx::ChatEvents::messageReceived, &ctx, [&](const mx::ChatMessage &m) {
            if (m.kind == mx::ChatKind::Private) C->chat->refusePrivateMessage(m, QStringLiteral("restricted"));
        });
        QVERIFY(A->chat->sendPrivateMessageWithId(C->me(), QStringLiteral("hi again"), QStringLiteral("00000000000000c4")));
        QVERIFY(waitFor([&] { return hasStatus(st, QStringLiteral("00000000000000c4"), mx::PmDelivery::Refused); }, 8000));
        QVERIFY(detailOf(st, QStringLiteral("00000000000000c4"), mx::PmDelivery::Refused).contains(QStringLiteral("only from some users")));
    }

    void pmOfflineThenBack()
    {
        auto A = node(QStringLiteral("Alice"));
        const QString bProfile = freshDir(QStringLiteral("BobKeep"));
        auto B = node(QStringLiteral("Bob"), {}, bProfile);
        A->start();
        B->start();
        QVERIFY(link(*A, *B));
        const mx::UserRef bob = B->me();
        QSignalSpy pres(A->events, &mx::ChatEvents::userPresence);
        // Bob quits normally: his bye marks him offline at once (it used to be ignored from a direct peer)
        QElapsedTimer t;
        t.start();
        B->plugin->stop();
        QVERIFY2(waitFor([&] { return !A->plugin->isUserOnline(bob); }, 3000), "bye from a direct peer ignored");
        qInfo("offline %lld ms after a normal quit", t.elapsed());
        // a message to the offline user: "not delivered" at once, kept for later
        QSignalSpy st(A->events, &mx::ChatEvents::privateMessageStatus);
        QVERIFY(A->chat->sendPrivateMessageWithId(bob, QStringLiteral("are you back?"), kId1));
        QVERIFY(waitFor([&] { return hasStatus(st, kId1, mx::PmDelivery::NotDelivered); }, 1000));
        QVERIFY(detailOf(st, kId1, mx::PmDelivery::NotDelivered).contains(QStringLiteral("offline")));
        // Bob comes back (same identity): online within seconds, and the message goes out by itself
        QVERIFY(RatsNode::waitAllStopped(5000));
        B.reset();
        B = node(QStringLiteral("Bob"), {}, bProfile);
        QSignalSpy rxB(B->events, &mx::ChatEvents::messageReceived);
        t.restart();
        B->start();
        QVERIFY(link(*A, *B));
        QVERIFY(waitFor([&] { return A->plugin->isUserOnline(bob); }, 3000));
        qInfo("online again %lld ms after the restart", t.elapsed());
        QVERIFY2(waitFor([&] { return pms(rxB).size() == 1; }, 8000), "queued PM not retried when the user came back");
        QVERIFY(waitFor([&] { return hasStatus(st, kId1, mx::PmDelivery::Delivered); }, 5000));
        QCOMPARE(detailOf(st, kId1, mx::PmDelivery::Delivered), QStringLiteral("delivered now"));
        QTest::qWait(1500);
        QCOMPARE(pms(rxB).size(), 1);   // exactly once
        B.reset();
        QDir(bProfile).removeRecursively();
    }

    void pmNoPathFailsAtOnce()
    {
        auto A = node(QStringLiteral("Alice")), B = node(QStringLiteral("Bob"));
        A->start();
        B->start();
        QVERIFY(link(*A, *B));
        const mx::UserRef bob = B->me();
        crash(*B);   // the only link goes: Alice is alone
        QVERIFY(waitFor([&] { return !A->plugin->isUserOnline(bob); }, 3000));
        QSignalSpy st(A->events, &mx::ChatEvents::privateMessageStatus);
        QVERIFY(A->chat->sendPrivateMessageWithId(bob, QStringLiteral("anyone?"), kId1));
        QVERIFY(hasStatus(st, kId1, mx::PmDelivery::NotDelivered));   // synchronously: never silence
        B->plugin->stop();
    }

    // ---------------------------------------------------------------- presence
    void presenceCrashDirect()
    {
        // A-B direct, both also linked to C. B crashes: A's ping through C goes unanswered -> offline in
        // seconds, and the room user list agrees at the same moment
        auto A = node(QStringLiteral("Alice")), B = node(QStringLiteral("Bob")), C = node(QStringLiteral("Carol"));
        A->start();
        B->start();
        C->start();
        QVERIFY(link(*A, *B));
        QVERIFY(link(*A, *C));
        QVERIFY(link(*B, *C));
        const QString room = QStringLiteral("presence check");
        A->chat->joinRoom(room);
        B->chat->joinRoom(room);
        QVERIFY(waitFor([&] { return roomHas(*A, room, B->me().id); }, 8000));
        const mx::UserRef bob = B->me();
        QSignalSpy pres(A->events, &mx::ChatEvents::userPresence);
        QSignalSpy rx(A->events, &mx::ChatEvents::messageReceived);
        QElapsedTimer t;
        t.start();
        crash(*B);
        QVERIFY2(waitFor([&] { return !A->plugin->isUserOnline(bob); }, 6000), "crashed direct peer stays online");
        qInfo("offline %lld ms after a crash (mesh ping unanswered)", t.elapsed());
        QVERIFY(!roomHas(*A, room, bob.id));
        bool partLine = false;
        for (const auto &a : rx) {
            const auto m = a.first().value<mx::ChatMessage>();
            if (m.kind == mx::ChatKind::Part && m.text.contains(QStringLiteral("connection lost"))) partLine = true;
        }
        QVERIFY(partLine);
        B->plugin->stop();
    }

    void linkLostButStillReachable()
    {
        // the direct A-B link drops, but both are still linked to C: B answers the ping and stays online
        auto A = node(QStringLiteral("Alice")), B = node(QStringLiteral("Bob")), C = node(QStringLiteral("Carol"));
        A->start();
        B->start();
        C->start();
        QVERIFY(link(*A, *B));
        QVERIFY(link(*A, *C));
        QVERIFY(link(*B, *C));
        QVERIFY(waitFor([&] { return A->plugin->isUserOnline(B->me()); }));
        QSignalSpy pres(A->events, &mx::ChatEvents::userPresence);
        A->plugin->node()->disconnectPeer(B->plugin->nodeId());
        QVERIFY(waitFor([&] { return !A->plugin->node()->isConnected(B->plugin->nodeId()); }, 3000));
        QTest::qWait(fastTimings().pingWaitMs + 2000);
        QVERIFY(A->plugin->isUserOnline(B->me()));
        QCOMPARE(offlineEvents(pres, B->me().id), 0);
    }

    void relayedPresenceTimeout()
    {
        // A-C-B: B is only known through the mesh. B crashes: A has no link to lose, so B times out after
        // 2 x his announce interval + grace (here 2 x 3 s + 2 s; with the defaults 2 x 60 s + 30 s)
        auto A = node(QStringLiteral("Alice")), B = node(QStringLiteral("Bob")), C = node(QStringLiteral("Carol"));
        A->start();
        B->start();
        C->start();
        QVERIFY(link(*C, *A));
        QVERIFY(link(*C, *B));
        QVERIFY(waitFor([&] { return A->plugin->isUserOnline(B->me()); }, 15000));
        const mx::UserRef bob = B->me();
        QElapsedTimer t;
        t.start();
        crash(*B);
        QVERIFY2(waitFor([&] { return !A->plugin->isUserOnline(bob); }, 15000), "relayed user never timed out");
        const qint64 ms = t.elapsed();
        qInfo("relayed user offline %lld ms after a crash", ms);
        QVERIFY(ms <= 2 * 3000 + 2000 + 3500);
        // and Carol, who had the direct link, knew within seconds
        QVERIFY(!C->plugin->isUserOnline(bob));
        B->plugin->stop();
    }

    // ---------------------------------------------------------------- browse / transfer reachability
    void browseFirewalledByConnectBack()
    {
        // B is firewalled: A's dial fails, A asks B through C to connect back
        auto A = node(QStringLiteral("Alice")), B = node(QStringLiteral("Bob"), {true}), C = node(QStringLiteral("Carol"));
        A->start();
        B->start();
        C->start();
        QVERIFY(link(*C, *A));
        QVERIFY(link(*C, *B));
        QVERIFY(waitFor([&] { return A->plugin->isUserOnline(B->me()); }, 15000));
        B->library.addGenerated(B->profile, QStringLiteral("connect-back sample.bin"), 4096, 11);
        B->setShared();
        QSignalSpy res(A->plugin, &mx::NetworkPlugin::browseResults);
        QSignalSpy fail(A->plugin, &mx::NetworkPlugin::browseFailed);
        QElapsedTimer t;
        t.start();
        A->plugin->browse(41, B->me());
        QVERIFY2(waitFor([&] { return (!res.isEmpty() && res.last().at(3).toBool()) || !fail.isEmpty(); }, 15000), "no answer");
        QVERIFY2(fail.isEmpty(), fail.isEmpty() ? "" : qPrintable(fail.first().at(2).toString()));
        qInfo("browse of a firewalled user: %lld ms, link %s", t.elapsed(),
              qPrintable(A->plugin->node()->peerTransport(B->plugin->nodeId())));
        QCOMPARE(res.last().at(2).value<QList<mx::BrowseEntry>>().size(), 1);
    }

    void browseAndTransferBothFirewalled()
    {
        // A and B both firewalled, both linked to C who relays: browse and download through a circuit
        auto A = node(QStringLiteral("Alice"), {true}), B = node(QStringLiteral("Bob"), {true}), C = node(QStringLiteral("Carol"));
        A->start();
        B->start();
        C->start();
        QVERIFY(link(*C, *A));
        QVERIFY(link(*C, *B));
        QVERIFY(waitFor([&] { return A->plugin->isUserOnline(B->me()); }, 15000));
        const mx::SharedFile f = B->library.addGenerated(B->profile, QStringLiteral("relayed-2MB.bin"), 2 * 1024 * 1024 + 7, 21);
        B->setShared();
        QSignalSpy res(A->plugin, &mx::NetworkPlugin::browseResults);
        QSignalSpy fail(A->plugin, &mx::NetworkPlugin::browseFailed);
        QElapsedTimer t;
        t.start();
        A->plugin->browse(42, B->me());
        QVERIFY2(waitFor([&] { return (!res.isEmpty() && res.last().at(3).toBool()) || !fail.isEmpty(); }, 15000), "no answer");
        QVERIFY2(fail.isEmpty(), fail.isEmpty() ? "" : qPrintable(fail.first().at(2).toString()));
        QCOMPARE(A->plugin->node()->peerTransport(B->plugin->nodeId()), QStringLiteral("relay"));
        qInfo("browse through Carol's relay: %lld ms", t.elapsed());
        mx::FileHashes h;
        h.sha1 = f.hashes.sha1;
        auto *src = A->plugin->openSource(QStringLiteral("dl-relay"), h, f.size, sourceFor(*B));
        QVERIFY(src);
        Fetcher fe(src, f.size);
        t.restart();
        src->open();
        QVERIFY(waitFor([&] { return fe.complete() || fe.failed; }, 30000));
        QVERIFY2(!fe.failed, qPrintable(fe.error));
        QCOMPARE(fe.sha1.result(), f.hashes.sha1);
        QVERIFY(fe.sawRelayed);   // the row says "Transferring (relayed)"
        qInfo("2 MB through the relay in %lld ms", t.elapsed());
        src->close();
        delete src;
    }

    void bothFirewalledNoRelay()
    {
        // nobody relays: browse fails with a clear reason; private messages still go through the mesh
        auto A = node(QStringLiteral("Alice"), {true}), B = node(QStringLiteral("Bob"), {true}),
             C = node(QStringLiteral("Carol"), {false, false});
        A->start();
        B->start();
        C->start();
        QVERIFY(link(*C, *A));
        QVERIFY(link(*C, *B));
        QVERIFY(waitFor([&] { return A->plugin->isUserOnline(B->me()); }, 15000));
        QSignalSpy fail(A->plugin, &mx::NetworkPlugin::browseFailed);
        QElapsedTimer t;
        t.start();
        A->plugin->browse(43, B->me());
        QVERIFY(waitFor([&] { return !fail.isEmpty(); }, fastTimings().dialWaitMs + 5000));
        const QString why = fail.first().at(2).toString();
        qInfo("browse failed after %lld ms: %s", t.elapsed(), qPrintable(why));
        QVERIFY(why.contains(QStringLiteral("Cannot reach")));
        QVERIFY(why.contains(QStringLiteral("firewalled")));
        QSignalSpy st(A->events, &mx::ChatEvents::privateMessageStatus);
        QVERIFY(A->chat->sendPrivateMessageWithId(B->me(), QStringLiteral("still here"), kId1));
        QVERIFY(waitFor([&] { return hasStatus(st, kId1, mx::PmDelivery::Delivered); }, 8000));
    }

    void browseHotlistOnly()
    {
        auto A = node(QStringLiteral("Alice")), B = node(QStringLiteral("Bob")), C = node(QStringLiteral("Carol"));
        A->start();
        B->start();
        C->start();
        QVERIFY(link(*A, *B));
        QVERIFY(link(*C, *B));
        B->library.addGenerated(B->profile, QStringLiteral("hotlist browse.bin"), 1000, 3);
        B->setShared();
        B->settings.setValue(QString(mx::keys::messages::AllowBrowse), QStringLiteral("hotlist"));
        B->plugin->setHotlist({A->me()});
        QSignalSpy resA(A->plugin, &mx::NetworkPlugin::browseResults);
        QSignalSpy failC(C->plugin, &mx::NetworkPlugin::browseFailed);
        A->plugin->browse(51, B->me());
        C->plugin->browse(52, B->me());
        QVERIFY(waitFor([&] { return !resA.isEmpty() && resA.last().at(3).toBool(); }));
        QVERIFY(waitFor([&] { return !failC.isEmpty(); }));
        QVERIFY(failC.first().at(2).toString().contains(QStringLiteral("only users on their Hotlist")));
    }

    // ---------------------------------------------------------------- ourselves, rooms, one NAT
    void browseWhoisMessageSelf()
    {
        // the owner's report: "Browse" on the own row of a room said "User is not online"
        auto A = node(QStringLiteral("Alice")), B = node(QStringLiteral("Bob"));
        A->start();
        B->start();
        QVERIFY(link(*A, *B));
        for (int i = 0; i < 29; ++i) A->library.addGenerated(A->profile, QStringLiteral("own %1.bin").arg(i), 100 + i, uint(i));
        A->setShared();
        const QString room = QStringLiteral("self check");
        A->chat->joinRoom(room);
        QVERIFY(waitFor([&] { return !A->chat->roomUsers(room).isEmpty(); }));
        mx::ChatUser own;
        for (const auto &u : A->chat->roomUsers(room))
            if (u.user.id == A->me().id) own = u;
        QCOMPARE(own.user.name, A->me().name);
        QCOMPARE(own.files, 29);
        QSignalSpy res(A->plugin, &mx::NetworkPlugin::browseResults);
        QSignalSpy fail(A->plugin, &mx::NetworkPlugin::browseFailed);
        A->plugin->browse(61, own.user);
        QVERIFY(waitFor([&] { return !res.isEmpty() || !fail.isEmpty(); }, 2000));
        QCOMPARE(fail.count(), 0);
        QVERIFY(res.last().at(3).toBool());
        QCOMPARE(res.last().at(2).value<QList<mx::BrowseEntry>>().size(), own.files);   // agrees with the room list
        QSignalSpy who(A->plugin, &mx::NetworkPlugin::whoisResult);
        A->plugin->whois(own.user);
        QVERIFY(waitFor([&] { return !who.isEmpty(); }, 2000));
        QCOMPARE(who.first().at(1).toMap().value(QStringLiteral("files")).toInt(), 29);
        QCOMPARE(who.first().at(1).toMap().value(QStringLiteral("name")).toString(), A->me().name);
        QSignalSpy st(A->events, &mx::ChatEvents::privateMessageStatus);
        QVERIFY(!A->chat->sendPrivateMessageWithId(own.user, QStringLiteral("me?"), kId1));
        QVERIFY(hasStatus(st, kId1, mx::PmDelivery::Refused));
        QCOMPARE(detailOf(st, kId1, mx::PmDelivery::Refused), QStringLiteral("this is you"));
    }

    void browseRoomMemberThroughMesh()
    {
        // B is seen in a room through C (firewalled, no direct link): browse from the room row works and
        // returns as many files as the room list shows
        auto A = node(QStringLiteral("Alice")), B = node(QStringLiteral("Bob"), {true}), C = node(QStringLiteral("Carol"));
        A->start();
        B->start();
        C->start();
        QVERIFY(link(*C, *A));
        QVERIFY(link(*C, *B));
        for (int i = 0; i < 7; ++i) B->library.addGenerated(B->profile, QStringLiteral("room file %1.bin").arg(i), 50 + i, uint(i));
        B->setShared();
        const QString room = QStringLiteral("browse from the room");
        A->chat->joinRoom(room);
        B->chat->joinRoom(room);
        mx::ChatUser bob;
        QVERIFY(waitFor([&] {
            for (const auto &u : A->chat->roomUsers(room))
                if (u.user.id == B->me().id && u.files == 7) { bob = u; return true; }
            return false;
        }, 10000));
        QVERIFY(A->plugin->isUserOnline(bob.user));   // a room heartbeat is presence
        QSignalSpy res(A->plugin, &mx::NetworkPlugin::browseResults);
        QSignalSpy fail(A->plugin, &mx::NetworkPlugin::browseFailed);
        A->plugin->browse(62, bob.user);
        QVERIFY(waitFor([&] { return (!res.isEmpty() && res.last().at(3).toBool()) || !fail.isEmpty(); }, 15000));
        QVERIFY2(fail.isEmpty(), fail.isEmpty() ? "" : qPrintable(fail.first().at(2).toString()));
        QCOMPARE(res.last().at(2).value<QList<mx::BrowseEntry>>().size(), bob.files);
    }

    void twoPcsBehindOneNat()
    {
        // two PCs at home: both announce the shared public address first (which, without hairpinning,
        // leads nowhere: a documentation address here) and their LAN address after it; they meet through
        // an outside user C. Presence, a PM, browse and a download must work between them, directly.
        auto A = node(QStringLiteral("Alice")), B = node(QStringLiteral("Bob")), C = node(QStringLiteral("Carol"));
        for (Instance *i : {A.get(), B.get()}) {
            OverlayPlugin::TestOptions to;
            to.dht = to.mdns = to.reconnect = to.pex = false;
            to.listenPort = 0;
            to.publicFirst = QStringLiteral("192.0.2.77:6699");
            i->plugin->setTestOptions(to);
        }
        A->start();
        B->start();
        C->start();
        QVERIFY(link(*C, *A));
        QVERIFY(link(*C, *B));
        QVERIFY(waitFor([&] { return A->plugin->isUserOnline(B->me()) && B->plugin->isUserOnline(A->me()); }, 15000));
        QSignalSpy st(A->events, &mx::ChatEvents::privateMessageStatus);
        QVERIFY(A->chat->sendPrivateMessageWithId(B->me(), QStringLiteral("same house"), kId1));
        QVERIFY(waitFor([&] { return hasStatus(st, kId1, mx::PmDelivery::Delivered); }, 8000));
        const mx::SharedFile f = B->library.addGenerated(B->profile, QStringLiteral("lan.bin"), 500000, 31);
        B->setShared();
        QSignalSpy res(A->plugin, &mx::NetworkPlugin::browseResults);
        QSignalSpy fail(A->plugin, &mx::NetworkPlugin::browseFailed);
        A->plugin->browse(63, B->me());
        QVERIFY(waitFor([&] { return (!res.isEmpty() && res.last().at(3).toBool()) || !fail.isEmpty(); }, 15000));
        QVERIFY2(fail.isEmpty(), fail.isEmpty() ? "" : qPrintable(fail.first().at(2).toString()));
        const QString tr = A->plugin->node()->peerTransport(B->plugin->nodeId());
        qInfo("link between the two PCs: %s", qPrintable(tr));
        QVERIFY(!tr.isEmpty() && tr != QLatin1String("relay"));   // the LAN address was tried and used
        mx::FileHashes h;
        h.sha1 = f.hashes.sha1;
        auto *src = A->plugin->openSource(QStringLiteral("dl-lan"), h, f.size, sourceFor(*B));
        Fetcher fe(src, f.size);
        src->open();
        QVERIFY(waitFor([&] { return fe.complete() || fe.failed; }, 15000));
        QVERIFY2(!fe.failed, qPrintable(fe.error));
        QCOMPARE(fe.sha1.result(), f.hashes.sha1);
        src->close();
        delete src;
    }

    void silentPeerDropped()
    {
        // a peer that crashed without closing its link (power cut, kill -9): librats would keep the UDP
        // link for 45 s; our liveness pings drop it and the user goes offline
        auto A = node(QStringLiteral("Alice"));
        OverlayTimings t = fastTimings();
        t.linkIdlePingMs = 800;
        t.linkDeadMs = 2500;
        A->plugin->setTimings(t);
        A->start();
        FakePeer mute;
        mute.answerPings = false;
        const QString dir = freshDir(QStringLiteral("mute"));
        QVERIFY(mute.start(dir));
        mute.node.connectTo(A->addr());
        QString aNode;
        QVERIFY(waitFor([&] {
            for (const auto &p : mute.node.peers()) aNode = p.id;
            return !aNode.isEmpty();
        }));
        mute.hello(aNode);
        const mx::UserRef m{mx::Net::Overlay, mute.userId(), mute.name, QString()};
        QVERIFY(waitFor([&] { return A->plugin->isUserOnline(m); }));
        QElapsedTimer el;
        el.start();
        QVERIFY(waitFor([&] { return !A->plugin->isUserOnline(m); }, 8000));
        qInfo("silent peer offline after %lld ms", el.elapsed());
        bool pinged = false;
        for (const auto &o : mute.received) pinged |= o.value(QStringLiteral("t")).toString() == QLatin1String("ping");
        QVERIFY(pinged);
        A->plugin->stop();
        mute.node.stop();
        QDir(dir).removeRecursively();
    }

    // ---------------------------------------------------------------- uploads and the Hotlist
    void uploadsToHotlistOnly()
    {
        auto A = node(QStringLiteral("Alice")), B = node(QStringLiteral("Bob")), C = node(QStringLiteral("Carol"));
        A->start();
        B->start();
        C->start();
        QVERIFY(link(*A, *B));
        QVERIFY(link(*C, *B));
        const mx::SharedFile f = B->library.addGenerated(B->profile, QStringLiteral("hotlist-only.bin"), 300000, 5);
        B->setShared();
        B->settings.setValue(QString(mx::keys::transfers::UploadsToHotlistOnly), true);
        B->plugin->setHotlist({A->me()});
        mx::FileHashes h;
        h.sha1 = f.hashes.sha1;
        auto *sa = A->plugin->openSource(QStringLiteral("dl-ha"), h, f.size, sourceFor(*B));
        auto *sc = C->plugin->openSource(QStringLiteral("dl-hc"), h, f.size, sourceFor(*B));
        Fetcher fa(sa, f.size), fc(sc, f.size);
        sa->open();
        sc->open();
        QVERIFY(waitFor([&] { return fa.complete() || fa.failed; }, 15000));
        QVERIFY2(!fa.failed, qPrintable(fa.error));
        QCOMPARE(fa.sha1.result(), f.hashes.sha1);
        QVERIFY(waitFor([&] { return sc->state() == mx::RangeSource::Failed; }, 8000));
        QVERIFY(sc->stateText().contains(QStringLiteral("Hotlist")));
        sa->close();
        sc->close();
        delete sa;
        delete sc;
    }

    void hotlistPriorityInQueue()
    {
        // one upload slot, D holds it; C (not on the Hotlist) queues first, then A (on the Hotlist):
        // A goes ahead of C
        auto A = node(QStringLiteral("Alice")), B = node(QStringLiteral("Bob")), C = node(QStringLiteral("Carol")),
             D = node(QStringLiteral("Dave"));
        for (Instance *i : {A.get(), B.get(), C.get(), D.get()}) i->start();
        QVERIFY(link(*A, *B));
        QVERIFY(link(*C, *B));
        QVERIFY(link(*D, *B));
        const mx::SharedFile f = B->library.addGenerated(B->profile, QStringLiteral("queue.bin"), 200000, 9);
        B->setShared();
        B->settings.setValue(QString(mx::keys::transfers::MaxUploads), 1);
        B->settings.setValue(QString(mx::keys::queue::SmallFileBypassKB), 0);
        B->settings.setValue(QString(mx::keys::queue::HotlistPriority), true);
        B->plugin->setHotlist({A->me()});
        mx::FileHashes h;
        h.sha1 = f.hashes.sha1;
        auto *sd = D->plugin->openSource(QStringLiteral("q-d"), h, f.size, sourceFor(*B));
        auto *sc = C->plugin->openSource(QStringLiteral("q-c"), h, f.size, sourceFor(*B));
        auto *sa = A->plugin->openSource(QStringLiteral("q-a"), h, f.size, sourceFor(*B));
        sd->open();
        QVERIFY(waitFor([&] { return sd->state() == mx::RangeSource::Ready; }));
        sc->open();
        QVERIFY(waitFor([&] { return sc->state() == mx::RangeSource::Queued; }));
        QCOMPARE(sc->stateText(), QStringLiteral("Remotely Queued (1)"));
        sa->open();
        QVERIFY(waitFor([&] { return sa->state() == mx::RangeSource::Queued; }));
        QCOMPARE(sa->stateText(), QStringLiteral("Remotely Queued (1)"));
        QVERIFY(waitFor([&] { return sc->stateText() == QStringLiteral("Remotely Queued (2)"); }, 5000));
        // D finishes: the Hotlist user gets the slot
        sd->close();
        QVERIFY(waitFor([&] { return sa->state() == mx::RangeSource::Ready; }, 5000));
        QCOMPARE(sc->state(), mx::RangeSource::Queued);
        for (auto *s : {sa, sc, sd}) {
            s->close();
            delete s;
        }
    }

    // ---------------------------------------------------------------- an older (4.2) node
    void olderNodeCompatibility()
    {
        // A 4.2 node sends a hello without "caps", knows no receipts, no reach, no ping. It must still
        // exchange private messages (in both directions) and get refusals; our PM to it shows "Sent".
        auto A = node(QStringLiteral("Alice"));
        A->start();
        FakePeer old;
        old.name = QStringLiteral("Oldie_42042");
        const QString dir = freshDir(QStringLiteral("old42"));
        QVERIFY(old.start(dir));
        old.node.connectTo(A->addr());
        QString aNode;
        QVERIFY(waitFor([&] {
            for (const auto &p : old.node.peers()) aNode = p.id;
            return !aNode.isEmpty();
        }));
        old.hello(aNode);
        QVERIFY(waitFor([&] { return A->plugin->connectedUsers() == 1; }));
        mx::UserRef oldie{mx::Net::Overlay, old.userId(), old.name, QString()};
        // our tracked PM: delivered on the wire, status "Sent" (no receipt can come)
        QSignalSpy st(A->events, &mx::ChatEvents::privateMessageStatus);
        QVERIFY(A->chat->sendPrivateMessageWithId(oldie, QStringLiteral("hi old friend"), kId1));
        QVERIFY(waitFor([&] {
            for (const auto &o : old.received)
                if (o.value(QStringLiteral("t")).toString() == QLatin1String("pm") &&
                    o.value(QStringLiteral("text")).toString() == QLatin1String("hi old friend")) return true;
            return false;
        }));
        QVERIFY(hasStatus(st, kId1, mx::PmDelivery::Sent));
        QVERIFY(!hasStatus(st, kId1, mx::PmDelivery::NotDelivered));
        // its PM (4.2 format) arrives; it gets no receipt (it would not understand one)
        QSignalSpy rx(A->events, &mx::ChatEvents::messageReceived);
        QJsonObject pm{{QStringLiteral("t"), QStringLiteral("pm")}, {QStringLiteral("v"), 1},
                       {QStringLiteral("id"), QStringLiteral("42424242424242aa")},
                       {QStringLiteral("ts"), double(QDateTime::currentMSecsSinceEpoch())},
                       {QStringLiteral("name"), old.name}, {QStringLiteral("text"), QStringLiteral("hello from 4.2")}};
        old.send(aNode, pm);
        QVERIFY(waitFor([&] { return pms(rx, QStringLiteral("hello from 4.2")).size() == 1; }));
        QTest::qWait(300);
        for (const auto &o : old.received) QVERIFY(o.value(QStringLiteral("t")).toString() != QLatin1String("pm_ack"));
        // refused: the old node gets the pm_err it knows (its own generic text)
        A->settings.setValue(QString(mx::keys::messages::AllowPrivateMessages), QStringLiteral("hotlist"));
        pm[QStringLiteral("id")] = QStringLiteral("42424242424242bb");
        old.send(aNode, pm);
        QVERIFY(waitFor([&] {
            for (const auto &o : old.received)
                if (o.value(QStringLiteral("t")).toString() == QLatin1String("pm_err") &&
                    o.value(QStringLiteral("id")).toString() == QLatin1String("42424242424242bb")) return true;
            return false;
        }));
        A->plugin->stop();
        old.node.stop();
        QDir(dir).removeRecursively();
    }

    void cleanupTestCase() { QVERIFY(RatsNode::waitAllStopped(8000)); }
};

QTEST_GUILESS_MAIN(TstOverlayNet)
#include "tst_overlay_net.moc"
