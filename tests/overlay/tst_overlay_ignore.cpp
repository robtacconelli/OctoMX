// SPDX-License-Identifier: GPL-3.0-or-later
// [net-3] The ignore list on the OctoMX network, between two real nodes over 127.0.0.1: a user I
// ignore (by key, or added by name only) gets no browse list, no whois details, no search answer and
// no upload ("Not available"); ignoring someone ends what they are downloading from me, with the
// reason on my upload row; taking them off the list gives everything back. The core's UserManager
// hands the list to every network at start and on each change.
#include <QtTest/QSignalSpy>
#include <QtTest/QtTest>

#include "core/network_manager.h"
#include "core/user_manager.h"
#include "ovl_test_support.h"

using namespace mx::ovl;
using namespace ovltest;

namespace {

mx::SearchSource sourceFor(Instance &uploader)
{
    mx::SearchSource s;
    s.net = mx::Net::Overlay;
    s.user = uploader.me();
    s.extra.insert(QStringLiteral("node"), uploader.plugin->nodeId());
    return s;
}

void drop(mx::RangeSource *s)
{
    if (!s) return;
    s->close();
    s->deleteLater();
}

// a network that only records the ignore lists it is given (the core side of the hook)
class RecordingPlugin : public mx::NetworkPlugin {
public:
    using mx::NetworkPlugin::NetworkPlugin;
    mx::Net net() const override { return mx::Net::Overlay; }
    int capabilities() const override { return CanChat; }
    void init(const mx::PluginContext &) override {}
    void start() override {}
    void stop() override {}
    mx::NetworkStatus status() const override { return {}; }
    void setIgnored(const QList<mx::UserRef> &users) override { lists << users; }
    QList<QList<mx::UserRef>> lists;
};

} // namespace

class TstOverlayIgnore : public QObject {
    Q_OBJECT
    std::unique_ptr<Instance> A, B;   // A asks, B owns the files and ignores A
    mx::SharedFile file;

    // what A gets from B now
    struct Seen {
        bool browseOk = false, browseRefused = false, whoisDetails = false, whoisError = false, searchHit = false;
        bool sourceReady = false;
        QString sourceText;
    };
    Seen ask(quint32 tag)
    {
        Seen s;
        QSignalSpy res(A->plugin, &mx::NetworkPlugin::browseResults);
        QSignalSpy fail(A->plugin, &mx::NetworkPlugin::browseFailed);
        A->plugin->browse(tag, B->me());
        waitFor([&] { return !fail.isEmpty() || (!res.isEmpty() && res.last().at(3).toBool()); }, 15000);
        s.browseOk = !res.isEmpty();
        s.browseRefused = !fail.isEmpty();
        QSignalSpy who(A->plugin, &mx::NetworkPlugin::whoisResult);
        A->plugin->whois(B->me());
        waitFor([&] { return !who.isEmpty(); }, 20000);
        if (!who.isEmpty()) {
            const QVariantMap m = who.first().at(1).toMap();
            s.whoisError = m.contains(QStringLiteral("error"));
            s.whoisDetails = m.contains(QStringLiteral("files")) && !s.whoisError;
        }
        QSignalSpy hits(A->plugin, &mx::NetworkPlugin::searchHits);
        QSignalSpy done(A->plugin, &mx::NetworkPlugin::searchDone);
        mx::SearchQuery q;
        q.id = tag;
        q.text = QStringLiteral("ignored lighthouse");
        A->plugin->search(q);
        waitFor([&] { return done.count() == 1; }, 15000);
        for (const auto &a : hits)
            if (a.at(0).toUInt() == tag && !a.at(1).value<QList<mx::SearchHit>>().isEmpty()) s.searchHit = true;
        mx::FileHashes h;
        h.sha1 = file.hashes.sha1;
        mx::RangeSource *src = A->plugin->openSource(QStringLiteral("ign-%1").arg(tag), h, file.size, sourceFor(*B));
        if (src) {
            src->open();
            waitFor([&] { return src->state() == mx::RangeSource::Failed || src->state() == mx::RangeSource::Ready; }, 15000);
            s.sourceReady = src->state() == mx::RangeSource::Ready;
            s.sourceText = src->stateText();
            drop(src);
        }
        QTest::qWait(300);
        return s;
    }

private slots:
    void initTestCase()
    {
        mx::registerMetaTypes();
        A = std::make_unique<Instance>(QStringLiteral("Asker"));
        B = std::make_unique<Instance>(QStringLiteral("Owner"));
        file = B->library.addGenerated(B->profile, QStringLiteral("Ignored Lighthouse.bin"), 3 * 1024 * 1024, 7);
        A->start();
        B->start();
        B->setShared();
        QVERIFY(link(*A, *B));
        QTest::qWait(1500);   // the search mesh
    }

    void ignoredUserGetsNothing()
    {
        // not ignored: browse, whois, search and a download slot work
        const Seen before = ask(1);
        QVERIFY(before.browseOk);
        QVERIFY(before.whoisDetails);
        QVERIFY(before.searchHit);
        QVERIFY2(before.sourceReady, qPrintable(before.sourceText));
        // ignored by key
        B->plugin->setIgnored({A->me()});
        const Seen ignored = ask(2);
        QVERIFY(!ignored.browseOk);
        QVERIFY(ignored.browseRefused);
        QVERIFY(!ignored.whoisDetails);
        QVERIFY(ignored.whoisError);
        QVERIFY(!ignored.searchHit);
        QVERIFY(!ignored.sourceReady);
        QVERIFY2(ignored.sourceText.contains(QStringLiteral("Not available")), qPrintable(ignored.sourceText));
        // ignored by name only (as "/ignore Asker_12345" adds it): the same
        mx::UserRef byName;
        byName.net = mx::Net::Unknown;
        byName.name = A->me().name.toUpper();
        byName.id = byName.name;
        B->plugin->setIgnored({byName});
        const Seen named = ask(3);
        QVERIFY(named.browseRefused);
        QVERIFY(named.whoisError);
        QVERIFY(!named.searchHit);
        QVERIFY(!named.sourceReady);
        // another user's entry leaves A alone
        mx::UserRef other;
        other.net = mx::Net::Overlay;
        other.id = QString(64, QLatin1Char('c'));
        other.name = QStringLiteral("Someone_00000");
        B->plugin->setIgnored({other});
        const Seen after = ask(4);
        QVERIFY(after.browseOk);
        QVERIFY(after.whoisDetails);
        QVERIFY(after.searchHit);
        QVERIFY2(after.sourceReady, qPrintable(after.sourceText));
    }

    // ignoring a user who is downloading from me ends that upload, with the reason on my row
    void ignoringEndsTheirUpload()
    {
        B->plugin->setIgnored({});
        QSignalSpy upChanged(B->plugin, &mx::NetworkPlugin::uploadChanged);
        QSignalSpy upFinished(B->plugin, &mx::NetworkPlugin::uploadFinished);
        mx::FileHashes h;
        h.sha1 = file.hashes.sha1;
        mx::RangeSource *src = A->plugin->openSource(QStringLiteral("ign-run"), h, file.size, sourceFor(*B));
        QVERIFY(src);
        src->open();
        QVERIFY(waitFor([&] { return src->state() == mx::RangeSource::Ready; }, 15000));
        src->request(0, 64 * 1024);
        QVERIFY(waitFor([&] {
            for (const auto &a : upChanged)
                if (a.at(0).value<mx::TransferInfo>().state == mx::TransferState::Transferring) return true;
            return false;
        }, 10000));
        B->plugin->setIgnored({A->me()});
        QVERIFY(waitFor([&] { return !upFinished.isEmpty(); }, 5000));
        const mx::TransferInfo last = upChanged.last().at(0).value<mx::TransferInfo>();
        QCOMPARE(last.state, mx::TransferState::Cancelled);
        QCOMPARE(last.statusText, QStringLiteral("Stopped: the user is on your ignore list"));
        QVERIFY(waitFor([&] { return src->state() == mx::RangeSource::Failed; }, 15000));
        QVERIFY2(src->stateText().contains(QStringLiteral("Not available")), qPrintable(src->stateText()));
        drop(src);
        B->plugin->setIgnored({});
    }

    // the core: UserManager hands the ignore list to every network at start and on each change
    void coreHandsTheListToTheNetworks()
    {
        mx::SimpleSettings s;
        auto *rec = new RecordingPlugin;
        mx::NetworkManager net(&s, {rec});
        mx::UserManager users(&s, &net, nullptr);
        users.start();
        QCOMPARE(rec->lists.size(), 1);
        QVERIFY(rec->lists.last().isEmpty());
        mx::UserRef u;
        u.net = mx::Net::Overlay;
        u.id = QString(64, QLatin1Char('d'));
        u.name = QStringLiteral("Pest_11111");
        users.ignore(u, true);
        QCOMPARE(rec->lists.size(), 2);
        QCOMPARE(rec->lists.last().size(), 1);
        QCOMPARE(rec->lists.last().first().id, u.id);
        users.ignore(u, false);
        QCOMPARE(rec->lists.size(), 3);
        QVERIFY(rec->lists.last().isEmpty());
    }

    void cleanupTestCase()
    {
        A.reset();
        B.reset();
    }
};

QTEST_GUILESS_MAIN(TstOverlayIgnore)
#include "tst_overlay_ignore.moc"
