// SPDX-License-Identifier: GPL-3.0-or-later
// Performance sanity: 10 000 search results (with groups) in the sorted list view, live updates
// of 1 000 of them, scrolling/painting; 10 000 transfers. Generous limits so the test is stable
// on a loaded machine; the measured times are printed.
#include <QtCore/QElapsedTimer>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QScrollBar>

#include "ui/kit/list_view.h"
#include "ui/kit/theme.h"
#include "ui/models/search_model.h"
#include "ui/models/transfer_model.h"

using namespace mx;
using namespace mx::ui;

namespace {
SearchHit mkHit(int i, int nsrc)
{
    SearchHit h;
    h.key = QStringLiteral("k%1").arg(i);
    h.fileName = QStringLiteral("Artist %1 - Title %2.mp3").arg(i % 97).arg(i);
    h.size = 1000000 + qint64(i) * 7919 % 9000000;
    h.bitrateKbps = (i % 3) ? 128 : 192;
    h.durationSec = 100 + i % 300;
    h.sampleRateHz = 44100;
    for (int k = 0; k < nsrc; ++k) {
        SearchSource s;
        s.net = Net::Overlay;
        s.user.net = Net::Overlay;
        s.user.id = s.user.name = QStringLiteral("user%1_%2").arg(i).arg(k);
        s.connection = (k % 2) ? QStringLiteral("DSL") : QStringLiteral("56K");
        s.freeSlots = (i + k) % 3;
        s.totalSlots = 4;
        h.sources << s;
    }
    return h;
}
} // namespace

class TstPerf : public QObject {
    Q_OBJECT
private slots:
    void search10k()
    {
        SearchModel m;
        SearchProxy p;
        p.setSourceModel(&m);
        MxListView v;
        v.setTreeMode(true);
        v.setModel(&p);
        v.resize(1000, 600);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        v.sortByColumn(search::Bytes, Qt::DescendingOrder);

        QElapsedTimer t;
        t.start();
        for (int b = 0; b < 20; ++b) {            // arrives in 20 batches of 500
            QList<SearchHit> batch;
            for (int i = b * 500; i < (b + 1) * 500; ++i)
                batch << mkHit(i, (i % 10 == 0) ? 4 : 1);
            m.applyHits(batch);
        }
        const qint64 insertMs = t.elapsed();
        QCOMPARE(m.rowCount(), 10000);
        QCOMPARE(p.rowCount(), 10000);

        t.restart();
        QList<SearchHit> upd;
        for (int i = 0; i < 10000; i += 10)
            upd << mkHit(i, 6);                   // 1 000 groups grow
        m.applyHits(upd);
        const qint64 updateMs = t.elapsed();

        t.restart();
        v.expandAll();
        for (int k = 0; k < 20; ++k) {
            v.verticalScrollBar()->setValue(v.verticalScrollBar()->maximum() * k / 20);
            v.viewport()->repaint();
        }
        const qint64 paintMs = t.elapsed();
        qInfo("10k search rows: insert %lld ms, 1000 updates %lld ms, 20 scroll+paint %lld ms", insertMs, updateMs, paintMs);
        QVERIFY2(insertMs < 8000, "inserting 10 000 results too slow");
        QVERIFY2(updateMs < 4000, "updating 1 000 results too slow");
        QVERIFY2(paintMs < 4000, "painting too slow");
    }

    void transfers10k()
    {
        TransferModel m(nullptr, false);
        MxListView v;
        v.setTreeMode(true);
        v.setModel(&m);
        v.resize(1000, 400);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 10000; ++i) {
            TransferInfo ti;
            ti.id = QStringLiteral("t%1").arg(i);
            ti.name = QStringLiteral("file %1.bin").arg(i);
            ti.size = 1000000;
            ti.done = i * 97 % 1000000;
            ti.state = TransferState::Transferring;
            m.addTransfer(ti);
        }
        for (int i = 0; i < 10000; i += 7) {
            TransferInfo ti = m.transferAt(i);
            ti.done += 1000;
            m.updateTransfer(ti);
        }
        v.viewport()->repaint();
        const qint64 ms = t.elapsed();
        qInfo("10k transfers + 1429 updates + paint: %lld ms", ms);
        QVERIFY2(ms < 8000, "transfers too slow");
    }
};

int main(int argc, char **argv)
{
    preApplicationSetup();
    QApplication app(argc, argv);
    installClassicLook(&app);
    TstPerf t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_perf.moc"
