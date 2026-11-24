// SPDX-License-Identifier: GPL-3.0-or-later
// [safe-1] The real program (octomx, the real core, the HTTP network) downloads a file from a local
// throttled HTTP server and is stopped in the middle; started again it finishes the file, which
// must have the right hash:
//   sigterm   SIGTERM: the safe quit runs (log, exit code 0, the running mark is gone)
//   sigkill   SIGKILL: the next start says once that OctoMX was not closed properly
//   slowquit  the script's quit (the same path as the menu, the window, the tray and the update
//             install) while the disk is slow: the "Closing: saving downloads..." window is shown,
//             the quit waits until everything is written and goes away by itself
//   closenow  the same, with "Close now" pressed in that window: nothing recorded is lost
// Linux and macOS only (Windows has no SIGTERM; its console handler and end-session path are in
// src/app/main.cpp and mx/quit_signals.cpp).
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtTest/QTest>

#include "test_server.h"

#ifndef Q_OS_WIN
#include <csignal>
#endif

namespace {
const qint64 kSize = qint64(48) << 20;
const QByteArray kPath = "/f/safe-app.bin";

QString root()
{
    const QString d = QCoreApplication::applicationDirPath() + QStringLiteral("/safe-app-data");
    QDir().mkpath(d);
    return d;
}

QByteArray readAll(const QString &p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
} // namespace

class TstSafeApp : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void sigterm() { scenario(QStringLiteral("sigterm")); }
    void sigkill() { scenario(QStringLiteral("sigkill")); }
    void slowquit() { scenario(QStringLiteral("slowquit")); }
    void closenow() { scenario(QStringLiteral("closenow")); }

private:
    void scenario(const QString &how);
    bool startApp(QProcess &p, const QString &profile, const QStringList &script, const QStringList &links,
                  const QProcessEnvironment &extra = QProcessEnvironment());
    void writeSettings(const QString &profile);
    std::unique_ptr<mxtest::TestServer> m_srv;
    QByteArray m_want;
};

void TstSafeApp::initTestCase()
{
#ifdef Q_OS_WIN
    QSKIP("SIGTERM / SIGKILL: Linux and macOS");
#endif
    QVERIFY(QFileInfo::exists(QStringLiteral(MX_OCTOMX_EXE)));
    QDir(root()).removeRecursively();
    m_srv = std::make_unique<mxtest::TestServer>();
    m_srv->start();
    QVERIFY(QTest::qWaitFor([&] { return m_srv->port() > 0; }, 5000));
    m_want = QCryptographicHash::hash(mxtest::generated(mxtest::seedFor(kPath, 0), 0, kSize), QCryptographicHash::Sha1);
}

void TstSafeApp::cleanupTestCase()
{
    if (m_srv) {
        m_srv->quit();
        m_srv->wait(5000);
        m_srv.reset();
    }
    if (!qEnvironmentVariableIsSet("MX_SAFE_APP_KEEP")) QDir(root()).removeRecursively();   // kept: logs and closing.png
}

void TstSafeApp::writeSettings(const QString &profile)
{
    QDir().mkpath(profile);
    QFile f(profile + QStringLiteral("/settings.json"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QStringLiteral(R"({ "bt/enabled": false, "ed2k/enabled": false, "kad/enabled": false, "gnutella/enabled": false,
  "http/enabled": true, "overlay/enabled": false, "web/enabled": false, "rats/enabled": false,
  "connection/upnp": false, "connection/natpmp": false, "misc/logLevel": "debug",
  "transfers/incomingDir": "%1/incoming", "transfers/incompleteDir": "%1/incomplete",
  "internal/wizardDone": true, "internal/termsAccepted": 1 })").arg(profile).toUtf8());
}

bool TstSafeApp::startApp(QProcess &p, const QString &profile, const QStringList &script, const QStringList &links,
                          const QProcessEnvironment &extra)
{
    const QString s = profile + QStringLiteral("/run.script");
    QFile f(s);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(script.join(QLatin1Char('\n')).toUtf8() + '\n');
    f.close();
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.insert(QStringLiteral("MX_OVERLAY_DISCOVERY"), QStringLiteral("0"));
    env.insert(QStringLiteral("HOME"), profile + QStringLiteral("/home"));
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), profile + QStringLiteral("/home/.config"));
    env.insert(QStringLiteral("XDG_DATA_HOME"), profile + QStringLiteral("/home/.local/share"));
    for (const QString &k : extra.keys()) env.insert(k, extra.value(k));
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.setStandardOutputFile(profile + QStringLiteral("/stdout.txt"), QIODevice::Append);
    p.start(QStringLiteral(MX_OCTOMX_EXE), QStringList{QStringLiteral("--profile"), profile, QStringLiteral("--script"), s} + links);
    return p.waitForStarted(10000);
}

void TstSafeApp::scenario(const QString &how)
{
    const QString profile = root() + QLatin1Char('/') + how;
    QDir(profile).removeRecursively();
    writeSettings(profile);
    const QString url = QString::fromLatin1(m_srv->base() + kPath) + QStringLiteral("?size=%1&rate=%2").arg(kSize).arg(4 << 20);
    const QString log = profile + QStringLiteral("/logs/octomx.log");
    const QString incoming = profile + QStringLiteral("/incoming/safe-app.bin");
    auto partBytes = [&] {
        qint64 n = 0;
        for (const QFileInfo &fi : QDir(profile + QStringLiteral("/incomplete")).entryInfoList({QStringLiteral("*.part")}, QDir::Files)) n += fi.size();
        return n;
    };
    QProcessEnvironment extra;
    QStringList first = {QStringLiteral("wait 600000")};
    if (how == QLatin1String("slowquit") || how == QLatin1String("closenow")) {
        extra.insert(QStringLiteral("MX_TEST_SLOW_WRITE_MS"), QStringLiteral("60"));   // a slow disk: writes pile up
        extra.insert(QStringLiteral("MX_TEST_CLOSING_SHOT"), profile + QStringLiteral("/closing.png"));
        if (how == QLatin1String("closenow")) extra.insert(QStringLiteral("MX_TEST_CLOSE_NOW_MS"), QStringLiteral("1500"));
        first = {QStringLiteral("wait 6000"), QStringLiteral("quit")};
    }
    QProcess a;
    QVERIFY(startApp(a, profile, first, {url}, extra));
    QVERIFY2(QTest::qWaitFor([&] { return partBytes() >= (qint64(8) << 20); }, 60000), "the download did not start");
    if (how == QLatin1String("sigterm")) ::kill(pid_t(a.processId()), SIGTERM);   // the program this test started
    else if (how == QLatin1String("sigkill")) ::kill(pid_t(a.processId()), SIGKILL);
    QVERIFY(a.waitForFinished(180000));
    const QString text = QString::fromUtf8(readAll(log));
    const QString out = QString::fromUtf8(readAll(profile + QStringLiteral("/stdout.txt")));
    if (how == QLatin1String("sigkill")) {
        QCOMPARE(a.exitStatus(), QProcess::CrashExit);
        QVERIFY(QFileInfo::exists(profile + QStringLiteral("/octomx.running")));
    } else {
        QCOMPARE(a.exitStatus(), QProcess::NormalExit);
        QCOMPARE(a.exitCode(), 0);
        QVERIFY2(text.contains(QLatin1String("engine shut down")), qPrintable(text.right(2000)));
        QVERIFY(!QFileInfo::exists(profile + QStringLiteral("/octomx.running")));
    }
    if (how == QLatin1String("sigterm")) QVERIFY2(out.contains(QLatin1String("SIGTERM: closing safely")) || text.contains(QLatin1String("SIGTERM")), qPrintable(out.right(1500)));
    if (how == QLatin1String("slowquit")) {
        QVERIFY2(text.contains(QLatin1String("every download is saved")), qPrintable(text.right(2000)));
        QVERIFY2(QFileInfo::exists(profile + QStringLiteral("/closing.png")), "the closing window was not shown");
    }
    if (how == QLatin1String("closenow"))
        QVERIFY2(out.contains(QLatin1String("Close now")) || text.contains(QLatin1String("Close now")), qPrintable(out.right(1500)));
    qInfo("%s: stopped with %lld bytes in the part file", qPrintable(how), partBytes());
    // started again: it finishes the file
    QProcess b;
    QVERIFY(startApp(b, profile, {QStringLiteral("wait 600000")}, {}));
    QVERIFY2(QTest::qWaitFor([&] { return QFileInfo::exists(incoming) && QFileInfo(incoming).size() == kSize; }, 120000),
             qPrintable(QString::fromUtf8(readAll(log)).right(1500)));
    QTest::qWait(500);
    const QString text2 = QString::fromUtf8(readAll(log));
    if (how == QLatin1String("sigkill")) QVERIFY(text2.contains(QLatin1String("was not closed properly")));
    ::kill(pid_t(b.processId()), SIGTERM);
    QVERIFY(b.waitForFinished(60000));
    QCOMPARE(QCryptographicHash::hash(readAll(incoming), QCryptographicHash::Sha1).toHex(), m_want.toHex());
    QCOMPARE(QDir(profile + QStringLiteral("/incoming")).entryList(QDir::Files | QDir::Hidden), QStringList{QStringLiteral("safe-app.bin")});
}

QTEST_GUILESS_MAIN(TstSafeApp)
#include "tst_safe_app.moc"
