// SPDX-License-Identifier: GPL-3.0-or-later
// [bw-1] Speed limits one click away: the Bandwidth page row (two-way binding with Settings ▸
// Bandwidth Throttle, "2.5 MB" input, upload minimum, limit lines), the tray menu Speed Limits ▸
// (presets, Other..., Speed Limits On toggle, schedule state, Resume Schedule), the title text, the
// schedule versus the quick controls, and the values after a restart.
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSystemTrayIcon>

#include "mock/mock_core.h"
#include "mx/bandwidth_limits.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/settings_pages.h"
#include "ui/kit/colors.h"
#include "ui/main_window.h"
#include "ui/os_integration.h"
#include "ui/pages/bandwidth_page.h"
#include "ui/speed_limits.h"
#include "ui/tray.h"

using namespace mx;
using namespace mx::ui;
namespace kb = mx::keys::bandwidth;

class TstBw1 : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    Core *m_core = nullptr;
    MainWindow *m_mw = nullptr;
    Settings *s() const { return m_core->settings(); }
    BandwidthPage *page() const { return qobject_cast<BandwidthPage *>(m_mw->page(QStringLiteral("bandwidth"))); }
    static QAction *find(QMenu *m, const QString &prefix)
    {
        for (QAction *a : m->actions())
            if (a->text().startsWith(prefix))
                return a;
        return nullptr;
    }
    QMenu *speedMenu() const
    {
        TrayController *t = TrayController::of(m_mw);
        if (!t)
            return nullptr;
        QMenu *m = t->trayMenu()->findChild<QMenu *>(QStringLiteral("speedLimitsMenu"));
        if (m)
            emit m->aboutToShow();
        return m;
    }
    // MX_BW1_SHOTS=<dir>: the menus on their own (the window grab of a tray menu at the screen's
    // bottom edge cuts the sub menus off)
    static void saveMenu(QMenu *m, const QString &name)
    {
        const QString dir = qEnvironmentVariable("MX_BW1_SHOTS");
        if (dir.isEmpty() || !m)
            return;
        m->popup(QPoint(20, 20));
        QTest::qWait(80);
        m->grab().save(dir + QLatin1Char('/') + name + QStringLiteral(".png"));
        m->hide();
    }
    void reset()
    {
        s()->setValue(kb::ScheduleEnabled, false);
        s()->setValue(kb::DownLimitKBs, 0);
        s()->setValue(kb::UpLimitKBs, 0);
        s()->setValue(kb::LastDownLimitKBs, 15);
        s()->setValue(kb::LastUpLimitKBs, 24);
        s()->setValue(kb::ScheduleOverrideUntil, QString());
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        TrayController::setTrayAvailableOverride(1);
        MockOptions o;
        o.frozen = true;
        m_core = createMockCoreWithOptions(m_dir.filePath(QStringLiteral("profile")), o);
        Colors::instance()->attach(m_core->settings());
        m_core->start();
        m_mw = new MainWindow(m_core);
        m_mw->resize(1024, 689);
        m_mw->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_mw));
        m_mw->showPage(QStringLiteral("bandwidth"));
        QVERIFY(page() && page()->limitRow());
    }
    void init() { reset(); }

    void parsing()
    {
        QCOMPARE(speed::parseKBs(QStringLiteral("500")), 500);
        QCOMPARE(speed::parseKBs(QStringLiteral("500 KB")), 500);
        QCOMPARE(speed::parseKBs(QStringLiteral("500 kb/s")), 500);
        QCOMPARE(speed::parseKBs(QStringLiteral("2.5 MB")), 2560);
        QCOMPARE(speed::parseKBs(QStringLiteral("2,5 MB/s")), 2560);
        QCOMPARE(speed::parseKBs(QStringLiteral("1 GB")), 1024 * 1024);
        QCOMPARE(speed::parseKBs(QStringLiteral("64000 B")), 63);
        QCOMPARE(speed::parseKBs(QStringLiteral("0")), 0);
        QCOMPARE(speed::parseKBs(QStringLiteral("fast")), -1);
        QCOMPARE(speed::parseKBs(QStringLiteral("-5")), -1);
        QCOMPARE(speed::kbsText(0), QStringLiteral("Unlimited"));
        QCOMPARE(speed::kbsText(500), QStringLiteral("500 KB/s"));
        QCOMPARE(speed::kbsText(2560), QStringLiteral("2.5 MB/s"));
        QCOMPARE(speed::kbsText(1024), QStringLiteral("1 MB/s"));
    }

    // the row and the Settings page show and write the same settings, live, both ways
    void rowAndSettingsInStep()
    {
        SpeedLimitRow *row = page()->limitRow();
        QVERIFY(row->isVisible());
        QVERIFY(!row->downCheck()->isChecked());
        QCOMPARE(row->downSpin()->value(), 15);
        QVERIFY(!row->downCheck()->toolTip().isEmpty() && !row->upSpin()->toolTip().isEmpty());
        QVERIFY(row->upSpin()->toolTip().contains(QStringLiteral("%1 KB/s").arg(bwlimit::kMinUploadKBs)));

        SettingsDialog dlg(m_core, m_mw);
        dlg.showPage(QStringLiteral("throttle"));
        auto *dc = dlg.findChild<QCheckBox *>(PageKit::nameFor(QString(kb::DownLimitKBs)));
        auto *ds = dlg.findChild<QSpinBox *>(PageKit::nameFor(QString(kb::LastDownLimitKBs)));
        auto *us = dlg.findChild<QSpinBox *>(PageKit::nameFor(QString(kb::LastUpLimitKBs)));
        QVERIFY(dc && ds && us);
        QVERIFY(qobject_cast<RateSpinBox *>(ds));

        // row -> settings -> Settings page
        row->downCheck()->click();
        QCOMPARE(s()->i(kb::DownLimitKBs), 15);
        QVERIFY(dc->isChecked());
        row->downSpin()->setValue(500);
        QCOMPARE(s()->i(kb::DownLimitKBs), 500);
        QCOMPARE(ds->value(), 500);
        // Settings page -> row
        ds->setValue(750);
        QCOMPARE(s()->i(kb::DownLimitKBs), 750);
        QCOMPARE(row->downSpin()->value(), 750);
        dc->click();
        QCOMPARE(s()->i(kb::DownLimitKBs), 0);
        QVERIFY(!row->downCheck()->isChecked());
        QCOMPARE(row->downSpin()->value(), 750);   // the value is kept while off
        // "2.5 MB" typed into the row
        QLineEdit *le = row->downSpin()->findChild<QLineEdit *>();
        QVERIFY(le);
        le->selectAll();
        QTest::keyClicks(le, QStringLiteral("2.5 MB"));
        QTest::keyClick(le, Qt::Key_Return);
        QCOMPARE(s()->i(kb::LastDownLimitKBs), 2560);
        QCOMPARE(ds->value(), 2560);
        // the upload minimum: the spin boxes never go below it, a smaller stored value is raised
        QCOMPARE(row->upSpin()->minimum(), bwlimit::kMinUploadKBs);
        QCOMPARE(us->minimum(), bwlimit::kMinUploadKBs);
        s()->setValue(kb::UpLimitKBs, 3);
        QCOMPARE(speed::inForce(s()).upKBs, bwlimit::kMinUploadKBs);
    }

    void limitLinesTitleAndStatus()
    {
        BandwidthPage *p = page();
        QVERIFY(!m_mw->windowTitle().contains(QStringLiteral("limit")));
        QCOMPARE(p->incomingGraph()->limit(), 0);
        speed::setLimit(s(), true, 500);
        speed::setLimit(s(), false, 50);
        QCOMPARE(p->incomingGraph()->limit(), 500 * 1024);
        QCOMPARE(p->outgoingGraph()->limit(), 50 * 1024);
        QTRY_VERIFY2(m_mw->windowTitle().contains(QStringLiteral("DL limit 500 KB/s, UL limit 50 KB/s")), qPrintable(m_mw->windowTitle()));
        const QStringList f = p->statusFields();
        QVERIFY2(f.join(QLatin1Char('|')).contains(QStringLiteral("(limit 500 KB/s)")), qPrintable(f.join(QLatin1Char('|'))));
        QTRY_VERIFY(TrayController::of(m_mw)->trayIcon()->toolTip().contains(QStringLiteral("DL limit")));
        speed::setLimitsOn(s(), false);
        QTRY_VERIFY(!m_mw->windowTitle().contains(QStringLiteral("limit")));
        QCOMPARE(p->incomingGraph()->limit(), 0);
    }

    void trayMenu()
    {
        QMenu *m = speedMenu();
        QVERIFY(m);
        QVERIFY(m->toolTipsVisible());
        QAction *on = find(m, QStringLiteral("Speed Limits On"));
        QAction *down = find(m, QStringLiteral("Download: "));
        QAction *up = find(m, QStringLiteral("Upload: "));
        QVERIFY(on && down && up && down->menu() && up->menu());
        saveMenu(m, QStringLiteral("menu_speed_limits"));
        QCOMPARE(down->text(), QStringLiteral("Download: Unlimited"));
        QVERIFY(!on->isChecked());
        // Download ▸ Unlimited / presets / Other...
        QMenu *dm = down->menu();
        QCOMPARE(dm->actions().first()->text(), QStringLiteral("Unlimited"));
        QVERIFY(dm->actions().first()->isChecked());
        QVERIFY(find(dm, QStringLiteral("Other...")));
        QList<int> presets;
        for (QAction *a : dm->actions())
            if (a->isCheckable() && a->data().toInt() > 0)
                presets << a->data().toInt();
        QVERIFY2(presets.size() >= 3, qPrintable(QString::number(presets.size())));
        QVERIFY(presets.contains(15));   // the last custom value
        for (QAction *a : dm->actions())
            QVERIFY(a->isSeparator() || !a->toolTip().isEmpty());
        // a preset applies at once
        QAction *pick = nullptr;
        for (QAction *a : dm->actions())
            if (a->isCheckable() && a->data().toInt() == presets.last())
                pick = a;
        pick->trigger();
        QCOMPARE(s()->i(kb::DownLimitKBs), presets.last());
        m = speedMenu();
        emit find(m, QStringLiteral("Download: "))->menu()->aboutToShow();
        saveMenu(m, QStringLiteral("menu_speed_limits_on"));
        saveMenu(find(m, QStringLiteral("Download: "))->menu(), QStringLiteral("menu_speed_limits_download"));
        QVERIFY(find(m, QStringLiteral("Download: "))->text().endsWith(speed::kbsText(presets.last())));
        QVERIFY(find(m, QStringLiteral("Speed Limits On"))->isChecked());
        QVERIFY(page()->limitRow()->downCheck()->isChecked());
        // Upload ▸ Other... (2.5 MB typed)
        speed::s_askHook = [](QWidget *, bool d, int) { return d ? -1 : 2560; };
        find(find(m, QStringLiteral("Upload: "))->menu(), QStringLiteral("Other..."))->trigger();
        QCOMPARE(s()->i(kb::UpLimitKBs), 2560);
        speed::s_askHook = nullptr;
        // Speed Limits On: off keeps the values, on brings back the same two
        m = speedMenu();
        find(m, QStringLiteral("Speed Limits On"))->trigger();
        QCOMPARE(s()->i(kb::DownLimitKBs), 0);
        QCOMPARE(s()->i(kb::UpLimitKBs), 0);
        QCOMPARE(s()->i(kb::LastUpLimitKBs), 2560);
        m = speedMenu();
        QVERIFY(!find(m, QStringLiteral("Speed Limits On"))->isChecked());
        find(m, QStringLiteral("Speed Limits On"))->trigger();
        QCOMPARE(s()->i(kb::DownLimitKBs), presets.last());
        QCOMPARE(s()->i(kb::UpLimitKBs), 2560);
        // only the upload limit on: off and on again keeps the download unlimited
        speed::setLimit(s(), true, 0);
        speed::setLimitsOn(s(), false);
        speed::setLimitsOn(s(), true);
        QCOMPARE(s()->i(kb::DownLimitKBs), 0);
        QCOMPARE(s()->i(kb::UpLimitKBs), 2560);
        // Bandwidth Settings... opens the Throttle page
        find(speedMenu(), QStringLiteral("Bandwidth Settings..."))->trigger();
        SettingsDialog *dlg = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (auto *d = qobject_cast<SettingsDialog *>(w); d && d->isVisible())
                dlg = d;
        QVERIFY(dlg);
        dlg->close();
    }

    // the schedule in force, the quick controls during its window, Resume Schedule
    void scheduleAndQuickControls()
    {
        if (QTime::currentTime() >= QTime(23, 58))
            QSKIP("too close to midnight for an always-on window");
        speed::setLimit(s(), true, 400);
        s()->setValue(kb::ScheduleStart, QStringLiteral("00:00"));
        s()->setValue(kb::ScheduleEnd, QStringLiteral("23:59"));
        s()->setValue(kb::ScheduleDays, 127);
        s()->setValue(kb::ScheduleDownKBs, 100);
        s()->setValue(kb::ScheduleUpKBs, 32);
        s()->setValue(kb::ScheduleEnabled, true);
        QCOMPARE(speed::inForce(s()).downKBs, 100);
        QTRY_VERIFY2(m_mw->windowTitle().contains(QStringLiteral("DL limit 100 KB/s, UL limit 32 KB/s (schedule)")), qPrintable(m_mw->windowTitle()));
        QVERIFY(page()->limitRow()->note()->toolTip().startsWith(QStringLiteral("Schedule in force until 23:59")));
        QCOMPARE(page()->incomingGraph()->limit(), 100 * 1024);
        QMenu *m = speedMenu();
        QAction *sched = m->findChild<QAction *>(QStringLiteral("speedLimitsSchedule"));
        QAction *resume = m->findChild<QAction *>(QStringLiteral("speedLimitsResume"));
        QVERIFY(sched && sched->isVisible() && sched->text().contains(QStringLiteral("23:59")));
        saveMenu(m, QStringLiteral("menu_speed_limits_schedule"));
        QVERIFY(resume && !resume->isVisible());
        // the user picks Unlimited in the tray during the window: that wins until 23:59
        find(find(m, QStringLiteral("Download: "))->menu(), QStringLiteral("Unlimited"))->trigger();
        const bwlimit::InForce f = speed::inForce(s());
        QCOMPARE(f.source, bwlimit::InForce::Manual);
        QCOMPARE(f.downKBs, 0);
        QVERIFY(page()->limitRow()->note()->toolTip().startsWith(QStringLiteral("Your limits win until 23:59")));
        m = speedMenu();
        saveMenu(m, QStringLiteral("menu_speed_limits_override"));
        QVERIFY(resume->isVisible());
        resume->trigger();
        QCOMPARE(speed::inForce(s()).source, bwlimit::InForce::Schedule);
        QCOMPARE(speed::inForce(s()).downKBs, 100);
        // turning the limits off with the toggle during the window also wins
        speed::setLimitsOn(s(), false);
        QCOMPARE(speed::inForce(s()).downKBs, 0);
        QCOMPARE(speed::inForce(s()).upKBs, 0);
        s()->setValue(kb::ScheduleEnabled, false);
    }

    void cleanupTestCase()
    {
        delete m_mw;
        m_mw = nullptr;
        m_core->shutdown();
        delete m_core;
    }

    // restart keeps the values (a profile of its own)
    void restartKeepsValues()
    {
        const QString profile = m_dir.filePath(QStringLiteral("restart"));
        {
            Core *c = createMockCoreWithOptions(profile, MockOptions());
            c->start();
            speed::setLimit(c->settings(), true, 2560);
            speed::setLimit(c->settings(), false, 40);
            speed::setLimitsOn(c->settings(), false);   // off, values remembered
            c->settings()->sync();
            c->shutdown();
            delete c;
        }
        Core *c = createMockCoreWithOptions(profile, MockOptions());
        c->start();
        QCOMPARE(c->settings()->i(kb::DownLimitKBs), 0);
        QCOMPARE(c->settings()->i(kb::LastDownLimitKBs), 2560);
        QCOMPARE(c->settings()->i(kb::LastUpLimitKBs), 40);
        speed::setLimitsOn(c->settings(), true);
        QCOMPARE(c->settings()->i(kb::DownLimitKBs), 2560);
        QCOMPARE(c->settings()->i(kb::UpLimitKBs), 40);
        c->shutdown();
        delete c;
    }
};

QTEST_MAIN(TstBw1)
#include "tst_bw1.moc"
