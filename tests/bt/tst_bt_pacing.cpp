// SPDX-License-Identifier: GPL-3.0-or-later
// [bw-2] The rule for what libtorrent keeps in flight under a speed limit (bt_pacing.h), and that
// without a limit libtorrent's own defaults are back. Pure functions, no network. The running
// session (a limit changed while transferring, switched off again) is covered by tst_bt_limits.
#include <QtTest/QTest>

#include <libtorrent/settings_pack.hpp>

#include "bt_common.h"
#include "bt_pacing.h"

using namespace mx::bt;
namespace lt = libtorrent;

class TstBtPacing : public QObject {
    Q_OBJECT
private slots:
    void queueScalesWithTheLimit()
    {
        QCOMPARE(pacedQueueBlocks(0, 1, 500), -1);                     // no limit: libtorrent decides
        QCOMPARE(pacedQueueBlocks(300 * 1024, 1, 500), 2);             // 0.12 s of 300 KiB/s = 2.25 blocks
        QCOMPARE(pacedQueueBlocks(1000 * 1024, 1, 500), 8);            // 7.5 blocks
        QCOMPARE(pacedQueueBlocks(5000 * 1024, 1, 500), 38);           // 37.5 blocks: 600 KB in flight
        QCOMPARE(pacedQueueBlocks(5000 * 1024, 4, 500), 9);            // split among the peers
        QCOMPARE(pacedQueueBlocks(300 * 1024, 30, 500), kMinQueueBlocks);   // never below 2 blocks
        QCOMPARE(pacedQueueBlocks(1 << 30, 1, 250), 250);              // never above the peer's own limit
        // the in-flight time follows the round trip
        QCOMPARE(pacedQueueBlocks(2000 * 1024, 1, 500, 0.24), 30);
        QCOMPARE(pacedSendBuffer(0, 1, 500 * 1024), -1);
        QCOMPARE(pacedSendBuffer(50 * 1024, 1, 500 * 1024), 16 * 1024);     // at least one block
        QCOMPARE(pacedSendBuffer(5000 * 1024, 2, 500 * 1024), int(0.12 * 5000 * 1024 / 2));
    }

    void inflightFollowsTheRoundTrip()
    {
        const double b = 2000 * 1024;
        QVERIFY(adaptInflight(0.12, 0.7 * b, b) > 0.12);   // under the budget: the round trip limits, longer pipe
        QVERIFY(adaptInflight(0.3, b, b) < 0.3);           // at the budget: look for a shorter one
        QCOMPARE(adaptInflight(0.2, 0.95 * b, b), 0.2);    // in between: keep it
        QCOMPARE(adaptInflight(kInflightMaxSec, 0.1 * b, b), kInflightMaxSec);
        QCOMPARE(adaptInflight(kInflightMinSec, b, b), kInflightMinSec);
        // a peer 0.2 s away (the pipe must hold 0.2 s of the budget): it settles near 0.2 s and the
        // rate stays above 90 % on average
        double k = kInflightSec, sum = 0;
        for (int i = 0; i < 60; ++i) {
            const double rate = qMin(1.0, k / 0.2) * b;
            if (i >= 20) sum += rate;
            k = adaptInflight(k, rate, b);
        }
        QVERIFY2(k >= 0.17 && k <= 0.3, qPrintable(QString::number(k)));
        QVERIFY2(sum / 40 > 0.9 * b, qPrintable(QString::number(sum / 40 / b)));
    }

    void defaultsWithoutALimit()
    {
        const lt::settings_pack def = lt::default_settings();
        lt::settings_pack off;
        applyRatePacing(off, 0, 0, 1);
        for (int key : {int(lt::settings_pack::tick_interval), int(lt::settings_pack::request_queue_time),
                        int(lt::settings_pack::max_out_request_queue), int(lt::settings_pack::send_buffer_watermark)})
            QCOMPARE(off.get_int(key), def.get_int(key));
        lt::settings_pack on;
        applyRatePacing(on, 300 * 1024, 50 * 1024, 2);
        QCOMPARE(on.get_int(lt::settings_pack::tick_interval), 100);
        QCOMPARE(on.get_int(lt::settings_pack::request_queue_time), 1);
        QCOMPARE(on.get_int(lt::settings_pack::max_out_request_queue), def.get_int(lt::settings_pack::max_out_request_queue));
        QCOMPARE(on.get_int(lt::settings_pack::send_buffer_watermark), 16 * 1024);
        // an upload limit alone: the tick is short, the download queue is libtorrent's
        lt::settings_pack up;
        applyRatePacing(up, 0, 50 * 1024, 1);
        QCOMPARE(up.get_int(lt::settings_pack::tick_interval), 100);
        QCOMPARE(up.get_int(lt::settings_pack::request_queue_time), def.get_int(lt::settings_pack::request_queue_time));
    }
};

QTEST_GUILESS_MAIN(TstBtPacing)
#include "tst_bt_pacing.moc"
