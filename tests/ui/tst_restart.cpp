// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-c] Restart test: start the real executable with a profile under the build tree (demo core),
// change state through the UI with the script driver (settings dialog controls, page, hotlist,
// column width, window size, maximised, floating page), quit, start again and assert that
// everything came back.
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtTest/QtTest>

class TstRestart : public QObject {
    Q_OBJECT
private:
    QString m_dir;

    // runs octomx with a script; returns the exit code (0 = every command passed)
    int run(const QStringList &script, QString *log, const QStringList &extraArgs = {})
    {
        const QString file = m_dir + QStringLiteral("/run.script");
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            return -1;
        f.write((script.join(QLatin1Char('\n')) + QStringLiteral("\nquit\n")).toUtf8());
        f.close();
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        // a 1920x1200 virtual screen (the default 800x600 one would clamp the restored geometry)
        QFile cfg(m_dir + QStringLiteral("/offscreen.json"));
        if (cfg.open(QIODevice::WriteOnly | QIODevice::Truncate))
            cfg.write(R"({ "synchronousWindowSystemEvents": false, "windowFrameMargins": false,
  "screens": [ { "name": "shot", "x": 0, "y": 0, "width": 1920, "height": 1200, "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1 } ] })");
        cfg.close();
        // relative: a "Z:" drive letter would break the ':'-separated platform argument (wine)
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen:configfile=offscreen.json"));
        p.setWorkingDirectory(m_dir);
        env.insert(QStringLiteral("MX_OS_INTEGRATION"), QStringLiteral("fake"));
        env.insert(QStringLiteral("MX_FAKE_TRAY"), QStringLiteral("1"));
        p.setProcessEnvironment(env);
        p.setProcessChannelMode(QProcess::MergedChannels);
        QString exe = QStringLiteral(MX_OCTOMX_EXE);
#ifdef Q_OS_WIN
        if (exe.startsWith(QLatin1Char('/')))
            exe.prepend(QStringLiteral("Z:"));   // built on Linux, run under wine
#endif
        p.start(exe,
                QStringList{QStringLiteral("--profile"), m_dir + QStringLiteral("/profile"), QStringLiteral("--demo"),
                            QStringLiteral("--no-wizard"), QStringLiteral("--script"), file} + extraArgs);
        if (!p.waitForStarted(20000)) {
            *log = QStringLiteral("cannot start %1: %2").arg(exe, p.errorString());
            return -3;
        }
        if (!p.waitForFinished(90000)) {
            p.kill();
            *log = QStringLiteral("timeout: ") + QString::fromUtf8(p.readAll());
            return -2;
        }
        *log = QStringLiteral("exit code %1\n").arg(p.exitCode()) + QString::fromUtf8(p.readAll());
        return p.exitCode();
    }

private slots:
    void initTestCase()
    {
        m_dir = QCoreApplication::applicationDirPath() + QStringLiteral("/tst_restart_work");
        QDir(m_dir).removeRecursively();
        QVERIFY(QDir().mkpath(m_dir + QStringLiteral("/profile")));
    }

    void changeQuitAndRestore()
    {
        QString log;
        // ---- first run: change things through the UI
        int rc = run({
                         // [ui-d] Networks sub-tab, search type, list sort / column order, splitter
                         QStringLiteral("page networks"),
                         QStringLiteral("subtab other"),
                         QStringLiteral("page search"),
                         QStringLiteral("click searchVideo"),
                         QStringLiteral("sort searchResults 1 desc"),
                         QStringLiteral("page transfers"),
                         QStringLiteral("move-column uploadList 2 0"),
                         QStringLiteral("sort uploadList 3 desc"),
                         QStringLiteral("settings appearance"),
                         QStringLiteral("close-dialogs"),
                         QStringLiteral("hotlist on"),
                         // [ui-e] a hand-sized column keeps its share of the list width: sized at the final window size
                         QStringLiteral("resize 900 640"),
                         QStringLiteral("wait 50"),
                         QStringLiteral("set-column downloadList 0 333"),
                         QStringLiteral("set-splitter transfersSplitter 250"),
                         QStringLiteral("settings Queuing"),
                         QStringLiteral("setvalue set_transfers_maxDownloads 33"),
                         QStringLiteral("settings bt"),
                         QStringLiteral("setvalue set_bt_listenPort 7001"),
                         QStringLiteral("setvalue set_bt_dht off"),
                         QStringLiteral("setvalue set_bt_encryption forced"),
                         QStringLiteral("settings appearance/search"),
                         QStringLiteral("setvalue color_search_0 #123456"),
                         QStringLiteral("settings misc"),
                         QStringLiteral("click set_misc_closeAction_tray"),
                         QStringLiteral("settings connection/incoming"),
                         QStringLiteral("setvalue set_connection_tcpPort 16699"),
                         QStringLiteral("settings search"),
                         QStringLiteral("setvalue set_search_autoFindSourcesMin 30"),
                         QStringLiteral("settings library"),
                         QStringLiteral("setvalue set_library_fileTypes Audio (.mp3); Video (.avi)"),
                         QStringLiteral("settings throttle"),
                         QStringLiteral("setvalue set_bandwidth_lastUpLimitKBs 42"),
                         QStringLiteral("setvalue set_bandwidth_upLimitKBs on"),
                         QStringLiteral("settings advanced"),
                         QStringLiteral("setvalue set_misc_logKeepFiles 9"),
                         QStringLiteral("close-dialogs"),
                         QStringLiteral("expect-setting transfers/maxDownloads 33"),
                     },
                     &log);
        QVERIFY2(rc == 0, qPrintable(log.right(2000)));

        // ---- second run: everything is back
        rc = run({
                     QStringLiteral("expect-page transfers"),
                     QStringLiteral("expect-hotlist on"),
                     QStringLiteral("expect-subtab networks 1"),
                     QStringLiteral("expect-choice searchFileType video"),
                     QStringLiteral("expect-setting search/defaultType video"),   // [ui-f] codes
                     QStringLiteral("expect-sort searchResults 1 desc"),
                     QStringLiteral("expect-column-pos uploadList 2 0"),
                     QStringLiteral("expect-sort uploadList 3 desc"),
                     QStringLiteral("expect-splitter transfersSplitter 250"),
                     QStringLiteral("expect-column downloadList 0 333"),
                     QStringLiteral("expect-size 900 640"),
                     QStringLiteral("expect-setting transfers/maxDownloads 33"),
                     QStringLiteral("expect-setting bt/listenPort 7001"),
                     QStringLiteral("expect-setting bt/dht false"),
                     QStringLiteral("expect-setting bt/encryption forced"),
                     QStringLiteral("expect-setting colors/listBackground #123456"),
                     QStringLiteral("expect-setting misc/closeAction tray"),
                     QStringLiteral("expect-setting connection/tcpPort 16699"),
                     QStringLiteral("expect-setting search/autoFindSourcesMin 30"),
                     QStringLiteral("expect-setting library/fileTypes Audio (.mp3); Video (.avi)"),
                     QStringLiteral("expect-setting bandwidth/upLimitKBs 42"),
                     QStringLiteral("expect-setting misc/logKeepFiles 9"),
                     // the dialog opens on the last page and its controls show the values
                     QStringLiteral("settings"),
                     QStringLiteral("expect-settings-page advanced"),
                     QStringLiteral("expect-widget set_misc_logKeepFiles 9"),
                     QStringLiteral("settings Queuing"),
                     QStringLiteral("expect-widget set_transfers_maxDownloads 33"),
                     QStringLiteral("settings bt"),
                     QStringLiteral("expect-widget set_bt_listenPort 7001"),
                     QStringLiteral("expect-widget set_bt_dht off"),
                     QStringLiteral("expect-widget set_bt_encryption forced"),
                     QStringLiteral("settings appearance/search"),
                     QStringLiteral("expect-widget color_search_0 #123456"),
                     QStringLiteral("settings throttle"),
                     QStringLiteral("expect-widget set_bandwidth_upLimitKBs on"),
                     QStringLiteral("expect-widget set_bandwidth_lastUpLimitKBs 42"),
                     QStringLiteral("close-dialogs"),
                     // next: maximised + a floating page
                     QStringLiteral("page search"),
                     QStringLiteral("float bandwidth"),
                     QStringLiteral("move-window float_bandwidth 300 220"),
                     QStringLiteral("resize-window float_bandwidth 610 380"),
                     QStringLiteral("maximize"),
                     QStringLiteral("wait 100"),
                 },
                 &log);
        QVERIFY2(rc == 0, qPrintable(log.right(2000)));

        // ---- third run: maximised state, page and normal size survive too
        rc = run({
                     QStringLiteral("expect-page search"),
                     QStringLiteral("expect-maximized on"),
                     QStringLiteral("expect-size 900 640"),
                     QStringLiteral("wait 100"),
                     QStringLiteral("expect-floating bandwidth"),
                     QStringLiteral("expect-window float_bandwidth 300 220 610 380"),
                 },
                 &log);
        QVERIFY2(rc == 0, qPrintable(log.right(2000)));
    }

    void minimizedStart()
    {
        QString log;
        // --minimized: the window stays hidden in the tray, restore shows it
        const int rc = run({QStringLiteral("expect-visible false"), QStringLiteral("restore"), QStringLiteral("wait 100"),
                            QStringLiteral("expect-visible true")},
                           &log, {QStringLiteral("--minimized")});
        QVERIFY2(rc == 0, qPrintable(log.right(2000)));
    }

    // [post-1] an old profile with minimise-to-tray on: after a restart the minimise button still
    // minimises to the taskbar, "Always start minimized" too; X follows misc/closeAction only
    void minimiseAndCloseAfterRestart()
    {
        QString log;
        int rc = run({QStringLiteral("set misc/minimizeToTray true"), QStringLiteral("set misc/closeAction tray"),
                      QStringLiteral("set misc/startMinimized true"), QStringLiteral("set misc/confirmExit false")},
                     &log);
        QVERIFY2(rc == 0, qPrintable(log.right(2000)));
        rc = run({QStringLiteral("wait 100"), QStringLiteral("expect-visible true"), QStringLiteral("expect-minimized on"),
                  QStringLiteral("expect-hidden-to-tray off"), QStringLiteral("restore"), QStringLiteral("wait 50"),
                  QStringLiteral("expect-minimized off"), QStringLiteral("minimize"), QStringLiteral("wait 100"),
                  QStringLiteral("expect-hidden-to-tray off"), QStringLiteral("expect-visible true"),
                  QStringLiteral("expect-minimized on"), QStringLiteral("restore"), QStringLiteral("wait 50"),
                  // X with "tray": hidden in the tray, the program keeps running
                  QStringLiteral("close-window"), QStringLiteral("wait 100"), QStringLiteral("expect-hidden-to-tray on"),
                  QStringLiteral("expect-visible false"), QStringLiteral("restore"), QStringLiteral("wait 50"),
                  QStringLiteral("expect-visible true"), QStringLiteral("set misc/startMinimized false"),
                  QStringLiteral("set misc/closeAction exit")},
                 &log);
        QVERIFY2(rc == 0, qPrintable(log.right(2000)));
        // X with "exit": the program ends (the rest of the script never runs)
        rc = run({QStringLiteral("expect-minimized off"), QStringLiteral("expect-setting misc/closeAction exit"),
                  QStringLiteral("close-window"), QStringLiteral("wait 3000"), QStringLiteral("echo still-running")},
                 &log);
        QVERIFY2(rc == 0, qPrintable(log.right(2000)));
        QVERIFY2(!log.contains(QStringLiteral("still-running")), qPrintable(log.right(2000)));
    }

    void cleanupTestCase()
    {
        QDir(m_dir).removeRecursively();
    }
};

QTEST_MAIN(TstRestart)
#include "tst_restart.moc"
