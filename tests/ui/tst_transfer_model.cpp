// SPDX-License-Identifier: GPL-3.0-or-later
// Transfers model: file rows with source children, status colours, progress role, in-place
// updates (children added/removed) keep the selection, summary line.
#include <QtCore/QItemSelectionModel>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>

#include "ui/kit/format.h"
#include "ui/kit/theme.h"
#include "ui/models/roles.h"
#include "ui/models/transfer_model.h"

using namespace mx;
using namespace mx::ui;
using namespace mx::ui::transfers;

namespace {
TransferInfo dl(const QString &id, TransferState st, const QString &status, qint64 done = 0, qint64 size = 1000)
{
    TransferInfo t;
    t.id = id;
    t.name = id + QStringLiteral(".mp3");
    t.state = st;
    t.statusText = status;
    t.done = done;
    t.size = size;
    return t;
}
TransferSource srcRow(const QString &user, const QString &status, int rate = 0)
{
    TransferSource s;
    s.net = Net::Overlay;
    s.user.net = Net::Overlay;
    s.user.id = user;
    s.user.name = user;
    s.connection = QStringLiteral("DSL");
    s.statusText = status;
    s.rateBps = rate;
    return s;
}
} // namespace

class TstTransferModel : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { fmt::setGroupSeparator(QLatin1Char(' ')); }

    void rowsAndColours()
    {
        TransferModel m(nullptr, false);
        TransferInfo a = dl(QStringLiteral("a"), TransferState::FindingSources, QStringLiteral("Finding sources..."), 0, 5792882);
        a.sources << srcRow(QStringLiteral("Quill233_21212"), QStringLiteral("Waiting for Network Reply..."));
        TransferInfo b = dl(QStringLiteral("b"), TransferState::Transferring, QString(), 490, 1000);
        b.rateDownBps = 6666;
        b.etaSec = 44;
        b.sources << srcRow(QStringLiteral("x"), QString(), 6666) << srcRow(QStringLiteral("y"), QStringLiteral("Remotely Queued 7"))
                  << srcRow(QStringLiteral("z"), QStringLiteral("Busy (4 Users in Queue)"));
        m.addTransfer(a);
        m.addTransfer(b);
        m.addTransfer(dl(QStringLiteral("c"), TransferState::Completed, QStringLiteral("Complete"), 1000, 1000));
        m.addTransfer(dl(QStringLiteral("d"), TransferState::Queued, QStringLiteral("Locally Queued")));
        m.addTransfer(dl(QStringLiteral("e"), TransferState::Error, QStringLiteral("File/User Unavailable")));
        QCOMPARE(m.rowCount(), 5);
        QCOMPARE(m.rowCount(m.index(1, 0)), 3);
        auto fg = [&](const QModelIndex &i) { return i.data(Qt::ForegroundRole).value<QColor>(); };
        // file row without live source is red, its status keeps the status colour (yellow)
        QCOMPARE(fg(m.index(0, DFile)), QColor(0xff, 0x00, 0x00));
        QCOMPARE(fg(m.index(0, DStatus)), QColor(0xff, 0xff, 0x00));
        QCOMPARE(m.index(0, DBytes).data().toString(), QStringLiteral("0 of 5.52 MB"));   // [ui-e]
        QCOMPARE(fg(m.index(1, DFile)), QColor(0x00, 0xff, 0x00));
        QCOMPARE(m.index(1, DStatus).data(ProgressRole).toInt(), 49);
        QCOMPARE(m.index(1, DRate).data().toString(), QStringLiteral("6.51 KB/s"));
        QCOMPARE(m.index(1, DTimeLeft).data().toString(), QStringLiteral("44 s"));
        QCOMPARE(m.index(1, DUser).data().toString().left(7), QStringLiteral("3 users"));
        QCOMPARE(fg(m.index(2, DStatus)), QColor(0x00, 0x80, 0xff));   // Complete: blue
        QCOMPARE(fg(m.index(3, DStatus)), QColor(0x40, 0x60, 0xff));   // Locally Queued
        QCOMPARE(fg(m.index(4, DStatus)), QColor(0xff, 0x00, 0x00));
        const QModelIndex p = m.index(1, 0);
        QCOMPARE(fg(m.index(1, DStatus, p)), QColor(0xc0, 0x80, 0xff));   // Remotely Queued: purple
        QCOMPARE(fg(m.index(2, DStatus, p)), QColor(0xff, 0x80, 0xff));   // Busy: pink
        QCOMPARE(m.index(0, DUser, p).data().toString(), QStringLiteral("x"));
        QVERIFY(m.isSourceRow(m.index(0, 0, p)));
        QCOMPARE(m.transferFor(m.index(0, 0, p)).id, QStringLiteral("b"));
        QCOMPARE(m.summaryText(), QStringLiteral("1 download from 1 user at 6.51 KB/s total"));
        // [ui-e] "Show exact byte counts": the classic texts
        fmt::setExactSizes(true);
        QCOMPARE(m.index(0, DBytes).data().toString(), QStringLiteral("0 of 5 792 882"));
        QCOMPARE(m.index(1, DRate).data().toString(), QStringLiteral("6.51"));
        QCOMPARE(m.index(1, DTimeLeft).data().toString(), QStringLiteral("0:44"));
        QCOMPARE(m.summaryText(), QStringLiteral("1 download from 1 user at 6 666 bytes per second total"));
        fmt::setExactSizes(false);
    }

    void updatesKeepSelection()
    {
        TransferModel m(nullptr, false);
        for (int i = 0; i < 20; ++i)
            m.addTransfer(dl(QStringLiteral("t%1").arg(i), TransferState::Transferring, QString(), i, 100));
        QItemSelectionModel sel(&m);
        sel.select(m.index(12, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        TransferInfo t = m.transferAt(12);
        t.done = 80;
        t.sources << srcRow(QStringLiteral("a"), QString(), 10) << srcRow(QStringLiteral("b"), QString(), 20);
        m.updateTransfer(t);
        QCOMPARE(m.rowCount(m.index(12, 0)), 2);
        t.sources.removeLast();
        m.updateTransfer(t);
        QCOMPARE(m.rowCount(m.index(12, 0)), 1);
        m.removeTransfer(QStringLiteral("t3"));
        QCOMPARE(m.rowCount(), 19);
        QCOMPARE(sel.selectedRows(0).size(), 1);
        QCOMPARE(m.transferFor(sel.selectedRows(0).first()).id, QStringLiteral("t12"));
        QCOMPARE(sel.selectedRows(0).first().siblingAtColumn(DStatus).data(ProgressRole).toInt(), 80);
        QCOMPARE(m.rowOfId(QStringLiteral("t19")), 18);
    }

    void uploads()
    {
        TransferModel m(nullptr, true);
        TransferInfo u = dl(QStringLiteral("u"), TransferState::TimedOut, QStringLiteral("Timed out (Waiting for incoming connection)"));
        u.isUpload = true;
        u.peer.name = QStringLiteral("peer");
        m.addTransfer(u);
        QCOMPARE(m.rowCount(m.index(0, 0)), 0);
        QCOMPARE(m.index(0, UUser).data().toString(), QStringLiteral("peer"));
        QCOMPARE(m.index(0, UStatus).data(Qt::ForegroundRole).value<QColor>(), QColor(0xff, 0x00, 0x00));
        QVERIFY(m.index(0, UStatus).data(OverflowRole).toBool());
        QCOMPARE(m.headerData(1, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("User"));
    }
};

int main(int argc, char **argv)
{
    preApplicationSetup();
    QApplication app(argc, argv);
    installClassicLook(&app);
    TstTransferModel t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_transfer_model.moc"
