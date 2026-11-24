// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-c] Tray / minimise / close behaviour (UI_SPEC §5.5, §6): tray and system menus, close prompt
// (all three answers, "Don't show this again"), close action tray / exit, the "active downloads"
// confirmation, restore (incl. maximised), Exit/Shutdown When Xfers Stop, Auto - Startup through
// the OS integration (fake backend), Reset All Windows.
// [post-1] the minimise button minimises to the taskbar with every setting; X goes to the tray only
// when misc/closeAction says so (or the user picks it in the prompt); without a tray X exits.
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSystemTrayIcon>

#include "mock/mock_core.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/close_prompt.h"
#include "ui/kit/colors.h"
#include "ui/main_window.h"
#include "ui/os_integration.h"
#include "ui/tray.h"

using namespace mx;
using namespace mx::ui;

class TstTrayClose : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    Core *m_core = nullptr;
    MainWindow *m_mw = nullptr;
    TrayController *m_tray = nullptr;
    FakeOsIntegration *m_os = nullptr;
    int m_promptCalls = 0;

    QStringList menuTexts(QMenu *m) const
    {
        QStringList out;
        for (QAction *a : m->actions())
            out << (a->isSeparator() ? QStringLiteral("-") : a->text());
        return out;
    }
    void showAgain()
    {
        m_tray->restore();
        QTRY_VERIFY(m_mw->isVisible());
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        TrayController::setTrayAvailableOverride(1);   // offscreen has no tray: pretend there is one
        MockOptions o;
        o.frozen = true;
        m_core = createMockCoreWithOptions(m_dir.filePath(QStringLiteral("profile")), o);
        Colors::instance()->attach(m_core->settings());
        m_core->start();
        m_mw = new MainWindow(m_core);
        m_mw->resize(1024, 689);
        m_mw->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_mw));
        m_tray = TrayController::of(m_mw);
        QVERIFY(m_tray);
        m_os = static_cast<FakeOsIntegration *>(OsIntegration::createFake(this));
        m_os->attach(m_core->settings());
        m_tray->setOsIntegration(m_os);
        QVERIFY(m_tray->activeDownloads() > 0);   // the demo core has running downloads
    }

    void menus()
    {
        QCOMPARE(menuTexts(m_tray->trayMenu()),
                 QStringList({"Show", "Hide", "-", "Exit When Xfers Stop", "Shutdown When Xfers Stop", "-", "Reset All Windows",
                              "Color Theme", "Speed Limits", "-", "Auto - Startup", "-", "Exit"}));   // [post-1] Color Theme ▸, [bw-1] Speed Limits ▸
        QCOMPARE(menuTexts(m_tray->systemMenu()),
                 QStringList({"Hide", "Exit", "-", "Exit When Transfers Finish", "Shutdown When Transfers Finish", "-",
                              "Auto - Startup", "-", "Reset All Windows"}));
        emit m_tray->trayMenu()->aboutToShow();
        QCOMPARE(m_tray->trayMenu()->defaultAction()->text(), QStringLiteral("Hide"));   // bold default
        QCOMPARE(m_tray->trayIcon()->toolTip(), m_mw->windowTitle());
    }

    void autoStartup()
    {
        QAction *a = nullptr;
        for (QAction *x : m_tray->trayMenu()->actions())
            if (x->text() == QLatin1String("Auto - Startup"))
                a = x;
        QVERIFY(a && a->isCheckable());
        a->trigger();
        QVERIFY(m_core->settings()->b(keys::misc::StartWithOs));
        QVERIFY(m_os->isAutostart());   // the setting drives the OS integration
        a->trigger();
        QVERIFY(!m_core->settings()->b(keys::misc::StartWithOs));
        QVERIFY(!m_os->isAutostart());
    }

    void promptMinimize()
    {
        QSignalSpy quit(m_mw, &MainWindow::quitRequested);
        m_core->settings()->setValue(keys::misc::CloseAction, QStringLiteral("ask"));
        TrayController::setPromptHook([this](bool downloads, bool *dont) {
            ++m_promptCalls;
            [&] { QVERIFY(downloads); }();
            *dont = true;
            return int(ClosePrompt::MinimizeToTray);
        });
        m_mw->close();
        QCOMPARE(m_promptCalls, 1);
        QVERIFY(!m_mw->isVisible());
        QVERIFY(m_tray->isHiddenToTray());
        QCOMPARE(quit.count(), 0);
        QCOMPARE(m_core->settings()->s(keys::misc::CloseAction), QStringLiteral("tray"));   // don't ask again
        emit m_tray->trayMenu()->aboutToShow();
        QCOMPARE(m_tray->trayMenu()->defaultAction()->text(), QStringLiteral("Show"));
        showAgain();
        // close action "tray": no prompt
        m_mw->close();
        QCOMPARE(m_promptCalls, 1);
        QVERIFY(m_tray->isHiddenToTray());
        showAgain();
    }

    void exitConfirmation()
    {
        QSignalSpy quit(m_mw, &MainWindow::quitRequested);
        m_core->settings()->setValue(keys::misc::CloseAction, QStringLiteral("exit"));
        QString asked;
        TrayController::setConfirmHook([&](const QString &t) {
            asked = t;
            return false;
        });
        m_mw->close();
        QVERIFY(asked.contains(QStringLiteral("active download")));
        QCOMPARE(quit.count(), 0);
        QVERIFY(m_mw->isVisible());
        // without the confirmation setting: exits at once
        m_core->settings()->setValue(keys::misc::ConfirmExit, false);
        asked.clear();
        // (do not really close the window here: ask the controller what it would do)
        QVERIFY(m_tray->handleClose());
        QVERIFY(asked.isEmpty());
        m_core->settings()->setValue(keys::misc::ConfirmExit, true);
        TrayController::setConfirmHook({});   // the hook refers to this function's locals
    }

    void realPromptDialog()
    {
        // the actual dialog: three choices while downloads run, "Exit" quits
        delete m_mw;
        m_mw = new MainWindow(m_core);
        m_mw->resize(1024, 689);
        m_mw->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_mw));
        m_tray = TrayController::of(m_mw);
        m_tray->setOsIntegration(m_os);
        TrayController::setPromptHook({});
        m_core->settings()->setValue(keys::misc::CloseAction, QStringLiteral("ask"));
        QSignalSpy quit(m_mw, &MainWindow::quitRequested);
        bool seen = false;
        QTimer::singleShot(100, this, [&] {
            auto *dlg = qobject_cast<ClosePrompt *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            seen = true;
            QCOMPARE(dlg->windowTitle(), QStringLiteral("OctoMX"));
            // [post-1] Exit first and preselected, the 4.1 wording
            QVERIFY(dlg->findChild<QRadioButton *>(QStringLiteral("closeExit"))->isChecked());   // default
            QCOMPARE(dlg->findChild<QRadioButton *>(QStringLiteral("closeExit"))->text(), QStringLiteral("Exit OctoMX"));
            QCOMPARE(dlg->findChild<QRadioButton *>(QStringLiteral("closeTray"))->text(), QStringLiteral("Keep running in the tray"));
            QVERIFY(!dlg->findChild<QRadioButton *>(QStringLiteral("closeTray"))->isChecked());
            QCOMPARE(dlg->findChild<QAbstractButton *>(QStringLiteral("closeDontShow"))->text(), QStringLiteral("Do not ask again"));
            QVERIFY(dlg->findChild<QRadioButton *>(QStringLiteral("closeWhenDone")));
            dlg->findChild<QRadioButton *>(QStringLiteral("closeExit"))->setChecked(true);
            dlg->findChild<QAbstractButton *>(QStringLiteral("closeOk"))->click();
        });
        m_mw->close();
        QVERIFY(seen);
        QCOMPARE(quit.count(), 1);
        QCOMPARE(m_core->settings()->s(keys::misc::CloseAction), QStringLiteral("ask"));   // not "don't show"
        // a fresh window for the next tests
        delete m_mw;
        m_mw = new MainWindow(m_core);
        m_mw->resize(1024, 689);
        m_mw->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_mw));
        m_tray = TrayController::of(m_mw);
        m_tray->setOsIntegration(m_os);
    }

    // [post-1] the minimise button is an ordinary minimise with every combination of settings,
    // including an old profile that still has misc/minimizeToTray on
    void minimizeStaysInTaskbar()
    {
        TrayController::setPromptHook([this](bool, bool *) {
            ++m_promptCalls;
            return -1;
        });
        const int before = m_promptCalls;
        for (const bool oldKey : {true, false})
            for (const char *action : {"ask", "tray", "exit"}) {
                m_core->settings()->setValue(keys::misc::MinimizeToTray, oldKey);
                m_core->settings()->setValue(keys::misc::CloseAction, QString::fromLatin1(action));
                m_mw->showMinimized();
                QTest::qWait(60);
                QVERIFY2(!m_tray->isHiddenToTray(), action);
                QVERIFY2(m_mw->isVisible(), action);   // a task bar button, not hidden
                QTRY_VERIFY2(m_mw->isMinimized(), action);   // [mac-1] Cocoa minimises with an animation
                m_mw->showNormal();
                QTRY_VERIFY(!m_mw->isMinimized());
#ifdef Q_OS_MACOS
                QTest::qWait(700);   // [mac-1] Cocoa ignores a minimise while the restore animation runs
#endif
            }
        QCOMPARE(m_promptCalls, before);   // a minimise never asks
        TrayController::setPromptHook({});
        m_core->settings()->setValue(keys::misc::CloseAction, QStringLiteral("ask"));
    }

    void oldProfileMinimizeKeyDoesNotHideOnClose()
    {
        // an old profile with minimise-to-tray on and the default close action: X asks, never hides silently
        m_core->settings()->setValue(keys::misc::MinimizeToTray, true);
        m_core->settings()->setValue(keys::misc::CloseAction, QStringLiteral("ask"));
        int asked = 0;
        TrayController::setPromptHook([&](bool, bool *) {
            ++asked;
            return -1;   // the prompt was closed: nothing happens
        });
        m_mw->close();
        QCOMPARE(asked, 1);
        QVERIFY(m_mw->isVisible());
        QVERIFY(!m_tray->isHiddenToTray());
        TrayController::setPromptHook({});
    }

    void noTrayCloseExits()
    {
        // without a system tray every close action exits (confirmation while downloads run first)
        TrayController::setTrayAvailableOverride(0);
        int prompts = 0;
        TrayController::setPromptHook([&](bool, bool *) {
            ++prompts;
            return int(ClosePrompt::KeepRunningInTray);
        });
        int confirms = 0;
        TrayController::setConfirmHook([&](const QString &) {
            ++confirms;
            return false;
        });
        for (const char *action : {"ask", "tray", "exit"}) {
            m_core->settings()->setValue(keys::misc::CloseAction, QString::fromLatin1(action));
            QVERIFY2(!m_tray->handleClose(), action);   // downloads run: asked, answered No
            QVERIFY2(!m_tray->isHiddenToTray(), action);
            QVERIFY2(m_mw->isVisible(), action);
        }
        QCOMPARE(prompts, 0);
        QCOMPARE(confirms, 3);
        m_core->settings()->setValue(keys::misc::ConfirmExit, false);
        for (const char *action : {"ask", "tray", "exit"}) {
            m_core->settings()->setValue(keys::misc::CloseAction, QString::fromLatin1(action));
            QVERIFY2(m_tray->handleClose(), action);   // exits
        }
        m_core->settings()->setValue(keys::misc::ConfirmExit, true);
        // a fresh controller state (handleClose marked it as quitting)
        delete m_mw;
        m_mw = new MainWindow(m_core);
        m_mw->resize(1024, 689);
        m_mw->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_mw));
        m_tray = TrayController::of(m_mw);
        m_tray->setOsIntegration(m_os);
        // "Hide" without a tray minimises to the taskbar
        m_tray->hideToTray();
        QTRY_VERIFY(m_mw->isMinimized());
        QVERIFY(m_mw->isVisible());
        m_mw->showNormal();
        QTRY_VERIFY(!m_mw->isMinimized());
        TrayController::setTrayAvailableOverride(1);
        TrayController::setPromptHook({});
        TrayController::setConfirmHook({});
        m_core->settings()->setValue(keys::misc::CloseAction, QStringLiteral("ask"));
    }

    void hideAndRestoreMaximized()
    {
        m_mw->showMaximized();
        QTRY_VERIFY(m_mw->isMaximized());
        m_tray->hideToTray();   // tray / system menu "Hide"
        QVERIFY(m_tray->isHiddenToTray());
        QVERIFY(!m_mw->isVisible());
        m_tray->restore();   // tray click / second instance
        QTRY_VERIFY(m_mw->isVisible());
        QVERIFY(m_mw->isMaximized());
        m_mw->showNormal();
        QTRY_VERIFY(!m_mw->isMaximized());
    }

    void exitWhenDoneAndShutdown()
    {
        QSignalSpy quit(m_mw, &MainWindow::quitRequested);
        m_core->settings()->setValue(keys::misc::CloseAction, QStringLiteral("ask"));
        TrayController::setPromptHook([](bool, bool *) { return int(ClosePrompt::ExitWhenDone); });
        m_mw->close();
        QVERIFY(m_tray->exitWhenDone());
        QVERIFY(m_tray->isHiddenToTray());
        m_tray->setShutdownWhenDone(true);
        QTest::qWait(1200);
        QCOMPARE(quit.count(), 0);   // downloads still running
        for (const TransferInfo &t : m_core->transfers()->downloads())
            m_core->transfers()->cancel(t.id, false);
        QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 4000);
        QCOMPARE(m_os->shutdowns, 1);
        TrayController::setPromptHook({});
    }

    void resetAllWindows()
    {
        delete m_mw;
        m_mw = new MainWindow(m_core);
        m_mw->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_mw));
        m_tray = TrayController::of(m_mw);
        m_mw->resize(900, 500);
        m_mw->floatPage(QStringLiteral("transfers"));
        QVERIFY(m_mw->isFloating(QStringLiteral("transfers")));
        m_tray->resetAllWindows();
        QVERIFY(!m_mw->isFloating(QStringLiteral("transfers")));
        QCOMPARE(m_mw->size(), QSize(800, 600));
    }

    void cleanupTestCase()
    {
        delete m_mw;
        delete m_core;
    }
};

QTEST_MAIN(TstTrayClose)
#include "tst_tray_close.moc"
