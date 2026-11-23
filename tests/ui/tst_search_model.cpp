// SPDX-License-Identifier: GPL-3.0-or-later
// Search results model: grouping, original WinMX colour rules, sorting, live updates keep the selection,
// rows merged away by the core (hitsRemoved) disappear without disturbing the rest.
#include <QtCore/QItemSelectionModel>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>

#include "mx/settings_keys.h"
#include "ui/kit/colors.h"
#include "ui/kit/format.h"
#include "ui/kit/theme.h"
#include "ui/models/search_model.h"

using namespace mx;
using namespace mx::ui;

namespace {
SearchSource src(const QString &user, const QString &conn, int freeSlots, int total, int queue = -1)
{
    SearchSource s;
    s.net = Net::Overlay;
    s.user.net = Net::Overlay;
    s.user.id = user;
    s.user.name = user;
    s.connection = conn;
    s.freeSlots = freeSlots;
    s.totalSlots = total;
    if (queue >= 0)
        s.extra.insert(QStringLiteral("queue"), queue);
    return s;
}

SearchHit hit(const QString &key, const QString &name, qint64 size, int kbps, QList<SearchSource> sources)
{
    SearchHit h;
    h.key = key;
    h.fileName = name;
    h.size = size;
    h.bitrateKbps = kbps;
    h.durationSec = 274;
    h.sampleRateHz = 44100;
    h.sources = sources;
    return h;
}

QString segText(const QVariant &v)
{
    QString t;
    for (const Segment &s : segmentsFrom(v))
        t += s.text;
    return t;
}
} // namespace

class TstSearchModel : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { fmt::setGroupSeparator(QLatin1Char(' ')); }

    void grouping()
    {
        SearchModel m;
        m.applyHits({hit(QStringLiteral("a"), QStringLiteral("Velorine - Beside The Mill Pond.mp3"), 4388571, 128,
                         {src(QStringLiteral("u1"), QStringLiteral("VDSL"), 1, 3), src(QStringLiteral("u2"), QStringLiteral("Cable"), 0, 4, 14),
                          src(QStringLiteral("u3"), QStringLiteral("4G"), 0, 2, 17)}),
                     hit(QStringLiteral("b"), QStringLiteral("Velorine - King Small Hours.mp3"), 5861376, 160,
                         {src(QStringLiteral("Quill233_21212"), QStringLiteral("T1"), 9, 12)})});
        QCOMPARE(m.rowCount(), 2);
        const QModelIndex g = m.index(0, 0);
        QCOMPARE(m.rowCount(g), 3);                    // group row: sources are children
        QCOMPARE(m.rowCount(m.index(1, 0)), 0);        // single source: no children
        // [ui-e] human-readable size, exact count in the tooltip and with "Show exact byte counts"
        QCOMPARE(m.index(0, search::Size).data().toString(), QStringLiteral("4.19 MB"));
        QCOMPARE(m.index(0, search::Size).data(Qt::ToolTipRole).toString(), QStringLiteral("4 388 571 bytes"));
        QCOMPARE(m.headerData(search::Size, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("Size"));
        fmt::setExactSizes(true);
        QCOMPARE(m.index(0, search::Size).data().toString(), QStringLiteral("4 388 571"));
        QCOMPARE(m.headerData(search::Size, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("Bytes"));
        fmt::setExactSizes(false);
        // [ui-e] the Network / Peers columns
        QCOMPARE(m.index(0, search::Network).data().toString(), QStringLiteral("OctoMX"));
        QCOMPARE(m.index(0, search::Peers).data().toString(), QStringLiteral("3"));
        QCOMPARE(m.index(0, search::Peers).data(SortRole).toInt(), 3);
        QCOMPARE(m.index(0, search::Network, g).data().toString(), QStringLiteral("OctoMX: u1"));
        QCOMPARE(m.index(0, search::Peers, g).data().toString(), QStringLiteral("1"));
        QCOMPARE(m.index(0, search::Time).data().toString(), QStringLiteral("4:34"));
        QCOMPARE(m.index(0, search::User).data().toString(), QStringLiteral("3 users"));
        QCOMPARE(segText(m.index(0, search::Speed).data(SegmentsRole)), QStringLiteral("2 + 1"));
        QCOMPARE(segText(m.index(0, search::Status).data(SegmentsRole)), QStringLiteral("1 + 0 available users"));
        QCOMPARE(m.index(1, search::User).data().toString(), QStringLiteral("Quill233_21212"));
        QVERIFY(m.index(1, search::Status).data().toString().startsWith(QStringLiteral("9 of 12 available")));
        // child rows repeat the file data and carry their own user / speed / status
        const QModelIndex c = m.index(1, search::Status, g);
        // [ui-e] the source's status names its declared line type
        QCOMPARE(c.data().toString(), QStringLiteral("14 in queue (0 of 4 available), Cable"));
        QCOMPARE(m.index(1, search::File, g).data().toString(), QStringLiteral("Velorine - Beside The Mill Pond.mp3"));
        // a child row downloads from that source only
        QCOMPARE(m.hitFor(m.index(2, 0, g)).sources.size(), 1);
        QCOMPARE(m.hitFor(m.index(2, 0, g)).sources.first().user.name, QStringLiteral("u3"));
        QCOMPARE(m.hitFor(g).sources.size(), 3);
    }

    void noneAvailableGroup()
    {
        SearchModel m;
        m.applyHits({hit(QStringLiteral("q"), QStringLiteral("x.mp3"), 1, 128,
                         {src(QStringLiteral("u1"), QStringLiteral("DSL"), 0, 3, 17), src(QStringLiteral("u2"), QStringLiteral("DSL"), 0, 3, 14)})});
        QCOMPARE(m.index(0, search::Status).data().toString(), QStringLiteral("14 17 in lowest queues"));
    }

    void colourRules()
    {
        SearchModel m;
        SearchSource partial = src(QStringLiteral("p"), QStringLiteral("Unknown"), 1, 4);
        partial.extra.insert(QStringLiteral("partialPercent"), 25);
        SearchHit join = hit(QStringLiteral("j"), QStringLiteral("join.mp3"), 1000, 205, {src(QStringLiteral("x"), QStringLiteral("Cable"), 4, 6)});
        join.canJoinRunningDownload = true;
        m.applyHits({hit(QStringLiteral("hi"), QStringLiteral("hi.mp3"), 1, 192, {src(QStringLiteral("a"), QStringLiteral("FTTB"), 2, 3)}),
                     hit(QStringLiteral("mid"), QStringLiteral("mid.mp3"), 1, 128, {src(QStringLiteral("b"), QStringLiteral("128K ISDN"), 1, 4)}),
                     hit(QStringLiteral("lo"), QStringLiteral("lo.mp3"), 1, 96, {src(QStringLiteral("c"), QStringLiteral("Unknown"), 0, 3, 8)}),
                     hit(QStringLiteral("part"), QStringLiteral("part.mp3"), 109878998, 320, {partial}), join});
        auto fg = [&](int row, int col) { return m.index(row, col).data(Qt::ForegroundRole).value<QColor>(); };
        QCOMPARE(fg(0, search::Bitrate), QColor(0x00, 0xff, 0x00));
        QCOMPARE(fg(1, search::Bitrate), QColor(0xff, 0xff, 0x00));
        QCOMPARE(fg(2, search::Bitrate), QColor(0xff, 0x00, 0x00));
        QCOMPARE(fg(0, search::Speed), QColor(0x00, 0xff, 0x00));
        QCOMPARE(fg(1, search::Speed), QColor(0xff, 0xff, 0x00));
        QCOMPARE(fg(2, search::Speed), QColor(0xff, 0x00, 0x00));
        QCOMPARE(fg(0, search::Status), QColor(0x00, 0xff, 0x00));    // 2 free
        QCOMPARE(fg(1, search::Status), QColor(0xff, 0x80, 0x40));    // exactly 1 free
        QCOMPARE(fg(2, search::Status), QColor(0xff, 0xff, 0xff));    // queued
        QCOMPARE(m.index(3, search::Bytes).data().toString(), QStringLiteral("25% of 105 MB"));
        QCOMPARE(fg(3, search::Bytes), QColor(0xff, 0x80, 0x40));
        // "can be added to running multi-point download": file/bytes/time/freq/user cyan, bitrate keeps its colour
        QCOMPARE(fg(4, search::File), QColor(0x80, 0xff, 0xff));
        QCOMPARE(fg(4, search::User), QColor(0x80, 0xff, 0xff));
        QCOMPARE(fg(4, search::Bitrate), QColor(0x00, 0xff, 0x00));
        QCOMPARE(m.index(0, search::Bytes).data(Qt::TextAlignmentRole).toInt() & Qt::AlignRight, int(Qt::AlignRight));
    }

    void sortingAndAvailableOnTop()
    {
        SearchModel m;
        SearchProxy p;
        p.setSourceModel(&m);
        m.applyHits({hit(QStringLiteral("1"), QStringLiteral("b.mp3"), 300, 128, {src(QStringLiteral("a"), QStringLiteral("T1"), 0, 3, 5)}),
                     hit(QStringLiteral("2"), QStringLiteral("a.mp3"), 100, 128, {src(QStringLiteral("b"), QStringLiteral("T1"), 2, 3)}),
                     hit(QStringLiteral("3"), QStringLiteral("c.mp3"), 200, 128, {src(QStringLiteral("c"), QStringLiteral("T1"), 0, 3, 2)})});
        p.sort(search::Bytes, Qt::AscendingOrder);
        QCOMPARE(p.index(0, search::File).data().toString(), QStringLiteral("a.mp3"));
        QCOMPARE(p.index(2, search::File).data().toString(), QStringLiteral("b.mp3"));
        p.sort(search::File, Qt::DescendingOrder);
        QCOMPARE(p.index(0, search::File).data().toString(), QStringLiteral("c.mp3"));
        p.sort(search::Bytes, Qt::DescendingOrder);
        p.setAvailableOnTop(true);
        QCOMPARE(p.index(0, search::File).data().toString(), QStringLiteral("a.mp3"));   // the only one with a free slot
        // connection filter
        m.applyHits({hit(QStringLiteral("4"), QStringLiteral("slow.mp3"), 50, 128, {src(QStringLiteral("d"), QStringLiteral("56K"), 1, 1)})});
        QCOMPARE(p.rowCount(), 4);
        // [qa-1] modern list: the old T1 sources count as DSL, 56K as DSL too -> filter at 4G removes all;
        // at DSL keeps all; "Unknown" is below DSL
        p.setMinConnectionRank(connectionSpeedRank(QStringLiteral("DSL")));
        QCOMPARE(p.rowCount(), 4);
        m.applyHits({hit(QStringLiteral("5"), QStringLiteral("unk.mp3"), 60, 128, {src(QStringLiteral("e"), QStringLiteral("Unknown"), 1, 1)}),
                     hit(QStringLiteral("6"), QStringLiteral("ftth.mp3"), 70, 128, {src(QStringLiteral("f"), QStringLiteral("FTTH"), 1, 1)})});
        QCOMPARE(p.rowCount(), 5);   // unk.mp3 filtered
        p.setMinConnectionRank(connectionSpeedRank(QStringLiteral("Cable")));
        QCOMPARE(p.rowCount(), 1);   // only FTTH is Cable or faster
        p.setMinConnectionRank(0);
        QCOMPARE(p.rowCount(), 6);
        p.setMinConnectionRank(connectionSpeedRank(QStringLiteral("VDSL")));
        QCOMPARE(p.rowCount(), 1);
        p.setMinConnectionRank(0);
        QCOMPARE(p.rowCount(), 6);
    }

    void updatesKeepSelection()
    {
        SearchModel m;
        SearchProxy p;
        p.setSourceModel(&m);
        p.sort(search::Bytes, Qt::AscendingOrder);
        QList<SearchHit> hits;
        for (int i = 0; i < 50; ++i)
            hits << hit(QStringLiteral("k%1").arg(i), QStringLiteral("f%1.mp3").arg(i), 1000 + i * 10, 128,
                        {src(QStringLiteral("u%1").arg(i), QStringLiteral("DSL"), 1, 2)});
        m.applyHits(hits);
        QItemSelectionModel sel(&p);
        const QModelIndex target = p.index(10, 0);
        QCOMPARE(target.data().toString(), QStringLiteral("f10.mp3"));
        sel.select(target, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        // more sources for a selected hit (becomes a group), new hits that sort before it
        SearchHit grown = hits[10];
        grown.sources << src(QStringLiteral("extra"), QStringLiteral("T1"), 0, 3, 4);
        QList<SearchHit> batch{grown};
        for (int i = 0; i < 5; ++i)
            batch << hit(QStringLiteral("n%1").arg(i), QStringLiteral("new%1.mp3").arg(i), 5 + i, 128,
                         {src(QStringLiteral("v"), QStringLiteral("DSL"), 1, 2)});
        QCOMPARE(m.applyHits(batch), 5);
        QCOMPARE(m.rowCount(), 55);
        const QModelIndexList rows = sel.selectedRows(0);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().data().toString(), QStringLiteral("f10.mp3"));
        QCOMPARE(rows.first().row(), 15);                    // moved down by the 5 new smaller files
        QCOMPARE(p.rowCount(rows.first()), 2);               // now a 2-source group
        QCOMPARE(m.index(m.rowOfKey(QStringLiteral("k10")), search::User).data().toString(), QStringLiteral("2 users"));
    }

    void removedRowsKeepOthers()
    {
        SearchModel m;
        SearchProxy p;
        p.setSourceModel(&m);
        QList<SearchHit> hits;
        for (int i = 0; i < 10; ++i)
            hits << hit(QStringLiteral("k%1").arg(i), QStringLiteral("f%1.mp3").arg(i), 1000 + i, 128,
                        {src(QStringLiteral("u%1").arg(i), QStringLiteral("DSL"), 1, 2)});
        m.applyHits(hits);
        QItemSelectionModel sel(&p);
        sel.select(p.index(7, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        // the core found k3 == k8: k3 disappears, k8 grows in the following hitsChanged
        QCOMPARE(m.removeHits({QStringLiteral("k3"), QStringLiteral("unknown")}), 1);
        QCOMPARE(m.rowCount(), 9);
        QCOMPARE(m.rowOfKey(QStringLiteral("k3")), -1);
        QCOMPARE(m.rowOfKey(QStringLiteral("k9")), 8);       // key index rebuilt
        QCOMPARE(sel.selectedRows(0).size(), 1);
        QCOMPARE(sel.selectedRows(0).first().data().toString(), QStringLiteral("f7.mp3"));
        SearchHit merged = hits[8];
        merged.sources << hits[3].sources;
        m.applyHits({merged});
        QCOMPARE(m.rowCount(), 9);
        QCOMPARE(m.rowCount(m.index(m.rowOfKey(QStringLiteral("k8")), 0)), 2);
    }

    // [ui-e] Network = the networks that have the file; index-only hits name the index; Peers = one
    // number, unknown ("?") last in both sort orders
    void networkAndPeers()
    {
        SearchModel m;
        SearchProxy p;
        p.setSourceModel(&m);
        SearchSource lime;
        lime.net = Net::BitTorrent;
        lime.connection = QStringLiteral("LimeTorrents");
        lime.extra.insert(QStringLiteral("providerName"), QStringLiteral("LimeTorrents"));
        SearchSource ed;
        ed.net = Net::ED2K;
        ed.seeds = 4;
        ed.peers = 6;
        SearchSource kad = ed;
        kad.net = Net::Kad;
        kad.peers = 2;
        SearchSource gn = src(QStringLiteral("g1"), QStringLiteral("Cable"), 1, 2);
        gn.net = Net::Gnutella;
        gn.user.net = Net::Gnutella;
        SearchSource bt;
        bt.net = Net::BitTorrent;
        bt.connection = QStringLiteral("DHT");
        bt.seeds = 10;
        bt.peers = 5;
        SearchHit indexOnly = hit(QStringLiteral("i"), QStringLiteral("idx.mkv"), 0, -1, {lime});
        SearchHit edkad = hit(QStringLiteral("e"), QStringLiteral("edkad.avi"), 734003200, -1, {ed, kad});
        SearchHit btg = hit(QStringLiteral("b"), QStringLiteral("btg.iso"), 2000, -1, {bt, gn});
        SearchHit core = hit(QStringLiteral("c"), QStringLiteral("core.iso"), 3000, -1, {bt});
        core.totalPeers = 999;   // the core's number wins
        m.applyHits({indexOnly, edkad, btg, core});
        auto row = [&](const QString &key) { return m.rowOfKey(key); };
        QCOMPARE(m.index(row("i"), search::Network).data().toString(), QStringLiteral("LimeTorrents (BT)"));
        QCOMPARE(m.index(row("i"), search::Peers).data().toString(), QStringLiteral("?"));
        QCOMPARE(m.index(row("i"), search::Size).data().toString(), QStringLiteral("?"));
        QCOMPARE(m.index(row("e"), search::Network).data().toString(), QStringLiteral("eD2k+Kad"));
        QCOMPARE(m.index(row("e"), search::Peers).data().toString(), QStringLiteral("10"));   // eD2k 6 + Kad 4
        QCOMPARE(m.index(row("e"), search::Size).data().toString(), QStringLiteral("700 MB"));
        QCOMPARE(m.index(row("b"), search::Network).data().toString(), QStringLiteral("BT+Gnutella"));
        QCOMPARE(m.index(row("b"), search::Peers).data().toString(), QStringLiteral("16"));
        QCOMPARE(m.index(row("c"), search::Peers).data().toString(), QStringLiteral("999"));
        QCOMPARE(m.index(row("b"), search::Peers).data(Qt::TextAlignmentRole).toInt() & Qt::AlignRight, int(Qt::AlignRight));
        p.sort(search::Peers, Qt::DescendingOrder);
        QCOMPARE(p.index(0, search::File).data().toString(), QStringLiteral("core.iso"));
        QCOMPARE(p.index(3, search::File).data().toString(), QStringLiteral("idx.mkv"));
        p.sort(search::Peers, Qt::AscendingOrder);
        QCOMPARE(p.index(0, search::File).data().toString(), QStringLiteral("edkad.avi"));
        QCOMPARE(p.index(3, search::File).data().toString(), QStringLiteral("idx.mkv"));   // unknown last
        p.sort(search::Size, Qt::AscendingOrder);
        QCOMPARE(p.index(3, search::File).data().toString(), QStringLiteral("idx.mkv"));   // unknown size last
    }

    void torrentGroup()
    {
        SearchModel m;
        SearchSource dht;
        dht.net = Net::BitTorrent;
        dht.connection = QStringLiteral("DHT");
        dht.seeds = 1412;
        dht.peers = 88;
        SearchSource tr = dht;
        tr.connection = QStringLiteral("tracker");
        tr.seeds = 700;
        tr.peers = 40;
        SearchHit h = hit(QStringLiteral("t"), QStringLiteral("debian.iso"), 790626304, -1, {dht, tr});
        m.applyHits({h});
        QCOMPARE(m.index(0, search::User).data().toString(), QStringLiteral("1500 users"));
        QCOMPARE(segText(m.index(0, search::Speed).data(SegmentsRole)), QStringLiteral("1412 + 88"));
        QCOMPARE(m.index(0, search::Status).data().toString(), QStringLiteral("1412 seeds, 88 peers"));
        QCOMPARE(m.index(0, search::Network).data().toString(), QStringLiteral("BT"));
        QCOMPARE(m.index(0, search::Peers).data().toString(), QStringLiteral("1 500"));
        QCOMPARE(m.index(0, search::Bitrate).data().toString(), QString());
    }
};

int main(int argc, char **argv)
{
    preApplicationSetup();
    QApplication app(argc, argv);
    installClassicLook(&app);
    TstSearchModel t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_search_model.moc"
