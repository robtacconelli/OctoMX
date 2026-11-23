// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] Connection Test window with the demo core: rows and colours, the explanation box, Copy /
// Save Report (addresses masked), Test Now progress and cancel, "Why Is This Slow?" selection, the
// entry points (Networks page both tabs, Settings ▸ Internet Connection, Transfers menu and status
// tooltip), and the layout at 100 % (fits 800 x 600) and 200 %.
#include <QtCore/QTemporaryDir>
#include <QtGui/QClipboard>
#include <QtGui/QStandardItemModel>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>

#include "uib_helpers.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/connection_test.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/kit/colors.h"
#include "ui/kit/list_view.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"
#include "ui/models/transfer_model.h"
#include "ui/pages/networks_page.h"

using namespace mx;
using namespace mx::ui;

namespace {
constexpr int kIdRole = 0x0100 + 200 + 61;   // DiagIdRole (UserRoleBase + 61)

QModelIndex rowById(QStandardItemModel *m, const QString &id)
{
    const QModelIndexList l = m->match(m->index(0, 0), kIdRole, id, 1, Qt::MatchExactly | Qt::MatchRecursive);
    return l.isEmpty() ? QModelIndex() : l.first();
}

QColor fg(QStandardItemModel *m, const QString &id)
{
    return rowById(m, id).data(Qt::ForegroundRole).value<QColor>();
}

void noOverlap(QWidget *dlg)
{
    QList<QWidget *> ws;
    for (const char *n : {"diagTestNow", "diagCheckOnline", "diagCopy", "diagSave", "diagLogs", "diagClose"})
        ws << dlg->findChild<QWidget *>(QLatin1String(n));
    for (int i = 0; i < ws.size(); ++i) {
        QVERIFY(ws[i]);
        QVERIFY2(dlg->rect().contains(ws[i]->geometry()), qPrintable(ws[i]->objectName()));
        for (int j = i + 1; j < ws.size(); ++j) QVERIFY(!ws[i]->geometry().intersects(ws[j]->geometry()));
    }
    auto *list = dlg->findChild<QWidget *>(QStringLiteral("diagList"));
    auto *explain = dlg->findChild<QWidget *>(QStringLiteral("diagExplain"));
    auto *hide = dlg->findChild<QWidget *>(QStringLiteral("diagHideAddresses"));
    QVERIFY(!list->geometry().intersects(explain->geometry()));
    QVERIFY(!explain->geometry().intersects(hide->geometry()));
    QVERIFY(!hide->geometry().intersects(ws.first()->geometry()));
    QVERIFY(list->height() > 100);
}
} // namespace

class TstConnTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    uib::App m_app;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_app.start(m_dir.filePath(QStringLiteral("p")));
    }
    void cleanupTestCase()
    {
        if (ConnectionTestDialog::instance()) delete ConnectionTestDialog::instance();
        m_app.stop();
    }

    void rowsAndColours()
    {
        ConnectionTestDialog *d = ConnectionTestDialog::open(m_app.core, m_app.mw);
        QVERIFY(d);
        QVERIFY(QTest::qWaitForWindowExposed(d));
        QStandardItemModel *m = d->model();
        // General, BitTorrent, eD2k, Kad, Gnutella, OctoMX, Web + three downloads
        QCOMPARE(m->rowCount(), 10);
        QCOMPARE(m->columnCount(), 3);
        QCOMPARE(m->headerData(0, Qt::Horizontal).toString(), QStringLiteral("Check"));
        QCOMPARE(m->headerData(1, Qt::Horizontal).toString(), QStringLiteral("Result"));
        QCOMPARE(m->headerData(2, Qt::Horizontal).toString(), QStringLiteral("Details"));
        QCOMPARE(fg(m, QStringLiteral("bittorrent.reach.tcp")), color(keys::colors::PingGood));
        QCOMPARE(fg(m, QStringLiteral("ed2k.reach.tcp")), color(keys::colors::PingMedium));
        QCOMPARE(fg(m, QStringLiteral("ed2k.map.tcp.4662")), color(keys::colors::PingMedium));
        QCOMPARE(fg(m, QStringLiteral("gnutella.reach.tcp")), color(keys::colors::ListText));
        QCOMPARE(rowById(m, QStringLiteral("ed2k.map.tcp.4662")).siblingAtColumn(2).data().toString().contains(QLatin1String("500 Internal Server Error")), true);
        QVERIFY(d->summaryText().contains(QLatin1String("cannot be reached from the internet on eD2k, Kad")));
        QVERIFY(rowById(m, QStringLiteral("general.firewall")).data(Qt::ToolTipRole).toString().contains(QLatin1String("Allow an app")));
        // the box explains the selected row and says what to do
        QVERIFY(d->selectRow(QStringLiteral("ed2k.reach.tcp")));
        QVERIFY(d->explanation().contains(QLatin1String("In your router, forward TCP port 4662 and UDP port 4672 to 192.168.1.23.")));
        QVERIFY(d->selectRow(QStringLiteral("general.firewall")));
        QVERIFY(d->explanation().contains(QLatin1String("Windows Firewall has no rule for OctoMX")));
        // wording: commas, ASCII dots, no em dash
        for (int r = 0; r < m->rowCount(); ++r) {
            const QModelIndex g = m->index(r, 0);
            for (int c = 0; c < m->rowCount(g); ++c)
                for (int col = 0; col < 3; ++col)
                    QVERIFY(!m->index(c, col, g).data().toString().contains(QChar(0x2014)));
        }
        // one window only
        QCOMPARE(ConnectionTestDialog::open(m_app.core, m_app.mw), d);
    }

    void copyAndSave()
    {
        ConnectionTestDialog *d = ConnectionTestDialog::open(m_app.core, m_app.mw);
        auto *hide = d->findChild<QCheckBox *>(QStringLiteral("diagHideAddresses"));
        QVERIFY(hide->isChecked());   // default on
        d->findChild<QPushButton *>(QStringLiteral("diagCopy"))->click();
        const QString text = QGuiApplication::clipboard()->text();
        QVERIFY(text.startsWith(QLatin1String("OctoMX connection report")));
        QVERIFY(text.contains(QLatin1String("== eD2k: [LIMITED]")));
        QVERIFY(text.contains(QLatin1String("Recent warnings and errors")));
        QVERIFY(!text.contains(QLatin1String("203.0.113.45")));
        QVERIFY(text.contains(QLatin1String("ext-1")));
        QVERIFY(text.contains(QLatin1String("192.168.1.23")));   // private addresses stay
        hide->setChecked(false);
        QVERIFY(!m_app.core->settings()->b(keys::diag::HideAddresses));
        QVERIFY(d->reportText().contains(QLatin1String("203.0.113.45")));
        hide->setChecked(true);
        const QString path = m_dir.filePath(QStringLiteral("report.txt"));
        ConnectionTestDialog::s_saveHook = [path](QWidget *, const QString &suggested) {
            return suggested.contains(QLatin1String("octomx-connection-report-")) ? path : QString();
        };
        d->findChild<QPushButton *>(QStringLiteral("diagSave"))->click();
        ConnectionTestDialog::s_saveHook = nullptr;
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("ext-1"));
        QString opened;
        ConnectionTestDialog::s_openFolderHook = [&opened](const QString &p) { opened = p; };
        d->findChild<QPushButton *>(QStringLiteral("diagLogs"))->click();
        ConnectionTestDialog::s_openFolderHook = nullptr;
        QVERIFY(opened.endsWith(QLatin1String("octomx.log")));
    }

    void testNowProgressAndCancel()
    {
        ConnectionTestDialog *d = ConnectionTestDialog::open(m_app.core, m_app.mw);
        auto *test = d->findChild<QPushButton *>(QStringLiteral("diagTestNow"));
        auto *bar = d->findChild<QProgressBar *>(QStringLiteral("diagProgress"));
        QSignalSpy prog(m_app.core->diagnostics(), &DiagnosticsService::testProgress);
        QSignalSpy fin(m_app.core->diagnostics(), &DiagnosticsService::testFinished);
        test->click();
        QCOMPARE(test->text(), QStringLiteral("Cancel Test"));
        QVERIFY(bar->isVisible());
        QTRY_VERIFY(bar->value() >= 30);
        QVERIFY(fin.wait(10000));
        QCOMPARE(fin.first().first().toBool(), false);
        QVERIFY(prog.size() >= 5);
        QCOMPARE(test->text(), QStringLiteral("Test Now"));
        QVERIFY(!bar->isVisible());
        // after the test the DNS row is green
        QCOMPARE(fg(d->model(), QStringLiteral("bittorrent.dns")), color(keys::colors::PingGood));
        // cancel
        fin.clear();
        test->click();
        QTRY_VERIFY(bar->value() >= 10);
        test->click();
        QCOMPARE(fin.size(), 1);
        QCOMPARE(fin.first().first().toBool(), true);
        QCOMPARE(test->text(), QStringLiteral("Test Now"));
        // Check Ports Online names the service and adds the checker rows
        auto *online = d->findChild<QPushButton *>(QStringLiteral("diagCheckOnline"));
        QVERIFY(online->toolTip().contains(QLatin1String("portcheck.transmissionbt.com")));
        online->click();
        QTRY_VERIFY(rowById(d->model(), QStringLiteral("ed2k.portcheck")).isValid());
    }

    void whyIsThisSlow()
    {
        ConnectionTestDialog *d = ConnectionTestDialog::open(m_app.core, m_app.mw, QStringLiteral("d8"));
        const QModelIndex cur = d->list()->mapToSource(d->list()->currentIndex());
        QCOMPARE(cur.siblingAtColumn(0).data(kIdRole).toString(), QStringLiteral("dl.d8"));
        QVERIFY(d->explanation().contains(QLatin1String("index numbers are probably out of date")));
        // the status tooltip of the parent row carries the sentence
        TransferModel model(m_app.core->transfers(), false, m_app.core->settings());
        bool found = false;
        for (int r = 0; r < model.rowCount(); ++r) {
            const QModelIndex i = model.index(r, transfers::DStatus);
            if (model.index(r, 0).data().toString().startsWith(QLatin1String("debian-13"))) {
                QVERIFY(i.data(Qt::ToolTipRole).toString().contains(QLatin1String("Only 2 of 80 peers accept connections")));
                found = true;
            }
        }
        QVERIFY(found);
    }

    void entryPoints()
    {
        delete ConnectionTestDialog::instance();
        QVERIFY(!ConnectionTestDialog::instance());
        m_app.mw->showPage(QStringLiteral("networks"));
        auto *b1 = m_app.mw->findChild<QPushButton *>(QStringLiteral("btnConnectionTest"));
        auto *b2 = m_app.mw->findChild<QPushButton *>(QStringLiteral("btnConnectionTest2"));
        QVERIFY(b1 && b2);
        QCOMPARE(b1->text(), QStringLiteral("Connection Test..."));
        b1->click();
        QVERIFY(ConnectionTestDialog::instance());
        delete ConnectionTestDialog::instance();
        b2->click();
        QVERIFY(ConnectionTestDialog::instance());
        delete ConnectionTestDialog::instance();
        SettingsDialog sd(m_app.core, m_app.mw);
        sd.showPage(QStringLiteral("connection"));
        auto *b3 = sd.findChild<QPushButton *>(QStringLiteral("set_connectionTest"));
        QVERIFY(b3);
        b3->click();
        QVERIFY(ConnectionTestDialog::instance());
        delete ConnectionTestDialog::instance();
    }

    void layout100and200()
    {
        for (double scale : {1.0, 2.0}) {
            setUiScale(scale);
            installClassicLook(qApp);
            {
                ConnectionTestDialog d(m_app.core);
                d.show();
                QVERIFY(QTest::qWaitForWindowExposed(&d));
                if (scale == 1.0) {
                    QVERIFY(d.width() <= 800 && d.height() <= 600);   // fits an 800 x 600 screen
                    d.resize(d.minimumSize());
                    QTest::qWait(20);
                    noOverlap(&d);
                    d.resize(760, 540);
                    QTest::qWait(20);
                } else {
                    d.resize(S(760), S(540));
                    QTest::qWait(20);
                }
                noOverlap(&d);
                // rows are as tall as the font needs, the header readable
                QVERIFY(d.list()->rowHeight() >= int(14 * scale));
                const QImage img = d.grab().toImage();
                img.save(QStringLiteral("conntest_%1.png").arg(int(scale * 100)));
            }
        }
        setUiScale(1.0);
        installClassicLook(qApp);
    }
};

QTEST_MAIN(TstConnTest)
#include "tst_conntest.moc"
