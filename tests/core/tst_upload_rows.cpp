// SPDX-License-Identifier: GPL-3.0-or-later
// [up-1] The end of an upload row tells the truth: "Complete" only when the whole file went out,
// "Finished" for an upload that ended normally with a part of it (one range of a multi-source
// download, a Gnutella / eD2k request), the plugin's own last word when it gave one ("Stopped by
// the other side: ...", "Cancelled by remote (...)", "Connection closed"), no rate after the end.
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include "core/upload_tracker.h"
#include "fake_plugin.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

using namespace mx;
using namespace mx::test;

class TstUploadRows : public QObject {
    Q_OBJECT
    SimpleSettings settings;
    TransferInfo row(const QString &id, qint64 size, qint64 done)
    {
        TransferInfo t;
        t.id = id;
        t.name = QStringLiteral("generated-test-file.bin");
        t.size = size;
        t.done = done;
        t.state = TransferState::Transferring;
        t.statusText = QStringLiteral("Uploading");
        t.rateUpBps = 250 * 1024;
        return t;
    }
    TransferInfo end(UploadTracker &tr, const QString &id)
    {
        for (const TransferInfo &t : tr.uploads())
            if (t.id.endsWith(id)) return t;
        return {};
    }

private slots:
    void initTestCase()
    {
        registerMetaTypes();
        settings.setValue(keys::transfers::AutoClearUploads, false);
    }

    void wholeFileIsComplete()
    {
        UploadTracker tr(&settings);
        FakePlugin p(Net::Overlay, NetworkPlugin::CanServe);
        tr.attach(&p);
        p.emitUpload(row(QStringLiteral("w"), 1000, 1000));
        p.emitUploadFinished(QStringLiteral("w"));
        const TransferInfo t = end(tr, QStringLiteral("w"));
        QCOMPARE(t.state, TransferState::Completed);
        QCOMPARE(t.statusText, QStringLiteral("Complete"));
        QCOMPARE(t.rateUpBps, 0);
    }

    // the owner's row: 10 MB of 2.7 GB, then the plugin said nothing more: not "Complete"
    void partEndedIsFinished()
    {
        UploadTracker tr(&settings);
        FakePlugin p(Net::Overlay, NetworkPlugin::CanServe);
        tr.attach(&p);
        p.emitUpload(row(QStringLiteral("p"), 2710LL * 1000 * 1000, 10LL * 1000 * 1000));
        p.emitUploadFinished(QStringLiteral("p"));
        const TransferInfo t = end(tr, QStringLiteral("p"));
        QCOMPARE(t.statusText, QStringLiteral("Finished"));
        QCOMPARE(t.done, 10LL * 1000 * 1000);
        QCOMPARE(t.rateUpBps, 0);
        // a plugin that reported "Complete" for a part is corrected too
        TransferInfo c = row(QStringLiteral("c"), 5000, 100);
        p.emitUpload(c);
        c.state = TransferState::Completed;
        c.statusText = QStringLiteral("Complete");
        p.emitUpload(c);
        p.emitUploadFinished(QStringLiteral("c"));
        QCOMPARE(end(tr, QStringLiteral("c")).statusText, QStringLiteral("Finished"));
    }

    void pluginsLastWordKept()
    {
        UploadTracker tr(&settings);
        FakePlugin p(Net::Overlay, NetworkPlugin::CanServe);
        tr.attach(&p);
        const QList<QPair<TransferState, QString>> words{
            {TransferState::Error, QStringLiteral("Stopped by the other side: File Mismatch: a block failed the check")},
            {TransferState::Cancelled, QStringLiteral("Cancelled by remote (download paused)")},
            {TransferState::Completed, QStringLiteral("Finished")},
            {TransferState::Error, QStringLiteral("Connection closed")},
            {TransferState::Error, QStringLiteral("Disconnected")}};
        int n = 0;
        for (const auto &w : words) {
            const QString id = QStringLiteral("k%1").arg(++n);
            TransferInfo t = row(id, 2000000, 500000);
            p.emitUpload(t);
            t.state = w.first;
            t.statusText = w.second;
            t.rateUpBps = 0;
            p.emitUpload(t);
            p.emitUploadFinished(id);
            const TransferInfo e = end(tr, id);
            QCOMPARE(e.statusText, w.second);
            QCOMPARE(e.state, w.first);
            QCOMPARE(e.rateUpBps, 0);
            QCOMPARE(e.done, qint64(500000));
        }
    }
};

QTEST_MAIN(TstUploadRows)
#include "tst_upload_rows.moc"
