// SPDX-License-Identifier: GPL-3.0-or-later
// ui-b: the Bandwidth page, samples from BandwidthService split into the original WinMX series,
// axis scaling, status text, live updates, clean resizing, splitter persistence.
#include <QtWidgets/QSplitter>

#include "uib_helpers.h"
#include "ui/kit/format.h"
#include "ui/pages/bandwidth_page.h"

using namespace mx;
using namespace mx::ui;

class TstUibBandwidth : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    uib::App m_app;
    BandwidthPage *page() const { return qobject_cast<BandwidthPage *>(m_app.mw->page(QStringLiteral("bandwidth"))); }
    QString profile() const { return m_dir.filePath(QStringLiteral("profile")); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_app.start(profile());
        m_app.mw->showPage(QStringLiteral("bandwidth"));
    }
    void cleanupTestCase() { m_app.stop(); }

    void niceSteps()
    {
        // classic axis in bytes per second ("Show exact byte counts")
        fmt::setExactSizes(true);
        QCOMPARE(bw::niceStep(16800), 2000);    // reference top graph: 18000 in 2000 steps
        QCOMPARE(bw::niceStep(27000), 5000);    // reference bottom graph: 30000 in 5000 steps
        QCOMPARE(bw::niceStep(0), 200);
        QCOMPARE(bw::niceStep(480000), 100000);
        fmt::setExactSizes(false);
        // [ui-e] default: KB/s / MB/s steps (1024-based)
        QCOMPARE(bw::niceStep(16800), 2 * 1024);
        QCOMPARE(bw::niceStep(480000), 100 * 1024);
        QCOMPARE(bw::niceStep(5 * 1024 * 1024), 1024 * 1024);
    }

    void historyIsSplitIntoSeries()
    {
        BandwidthPage *p = page();
        // the demo replays its history through sample() once the page listens
        QTRY_VERIFY(p->incomingPoints().size() >= 1200);
        const QList<BandwidthSample> hist = m_app.core->bandwidth()->history(3600);
        QCOMPARE(p->incomingPoints().size(), hist.size());
        // with a 1-second average every point keeps its sample's total exactly
        m_app.core->settings()->setValue(keys::bandwidth::GraphAverageSec, 1);
        const bw::Point last = bw::split(m_app.core, hist.last(), false);
        QCOMPARE(last.total(), hist.last().upBps);
        QVERIFY(last.v[bw::Overlay] > 3000);     // the orange "WPN Primary" band
        QVERIFY(last.v[bw::Packets] > 0);
        QVERIFY(last.v[bw::Uploads] > 10000);    // the yellow uploads on top
        QCOMPARE(last.v[bw::Other], 0);          // everything explained
        const bw::Point in = bw::split(m_app.core, hist.last(), true);
        QCOMPARE(in.total(), hist.last().downBps);
        QVERIFY(in.v[bw::Downloads] > 100000);
        // status bar = the latest displayed totals
        QVERIFY(p->statusFields().value(0).startsWith(QStringLiteral("Incoming (top) ")));
        QVERIFY(p->statusFields().value(1).contains(QStringLiteral("Outgoing (bottom) ")));
    }

    void axisCoversVisiblePeak()
    {
        BandwidthGraph *g = page()->outgoingGraph();
        const QVector<bw::Point> &pts = page()->outgoingPoints();
        int peak = 0;
        for (int i = qMax(0, int(pts.size()) - g->plotRect().width()); i < pts.size(); ++i)
            peak = qMax(peak, pts.at(i).total());
        QVERIFY(g->axisMax() >= peak);
        QVERIFY(g->axisMax() / g->axisStep() <= 9);
        QCOMPARE(g->plotRect().left() - g->contentsRect().left(), 48);
    }

    void resizesCleanly()
    {
        for (const QSize &s : {QSize(700, 450), QSize(1280, 900), QSize(1024, 689)}) {
            m_app.mw->resize(s);
            QTest::qWait(20);
            BandwidthGraph *g = page()->incomingGraph();
            QVERIFY(g->width() > 600);
            QVERIFY(!g->grab().isNull());
            QVERIFY(page()->outgoingGraph()->geometry().bottom() < page()->height());
        }
    }

    void splitterPersists()
    {
        page()->splitter()->setSizes({200, 400});
        emit page()->splitter()->splitterMoved(200, 1);
        const int top = page()->splitter()->sizes().value(0);
        m_app.stop();
        m_app.start(profile());
        m_app.mw->showPage(QStringLiteral("bandwidth"));
        QTest::qWait(30);
        QCOMPARE(page()->splitter()->sizes().value(0), top);
    }

    void liveSamples()
    {
        // a non-frozen core emits one sample per second
        uib::App live;
        QTemporaryDir d;
        live.start(d.filePath(QStringLiteral("p")), false);
        auto *p = qobject_cast<BandwidthPage *>(live.mw->page(QStringLiteral("bandwidth")));
        QTRY_VERIFY(p->incomingPoints().size() >= 1200);
        const int n = int(p->incomingPoints().size());
        QTRY_VERIFY_WITH_TIMEOUT(p->incomingPoints().size() > n, 3000);
        live.stop();
    }
};

QTEST_MAIN(TstUibBandwidth)
#include "tst_uib_bandwidth.moc"
