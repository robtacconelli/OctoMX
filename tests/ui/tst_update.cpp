// SPDX-License-Identifier: GPL-3.0-or-later
// [post-1] Check for Updates against a local fake server (never GitHub): newer / same / older /
// pre-release / draft / malformed / 404 / rate limited (403 + 429) / timeout / unreachable; the
// request headers (Accept, User-Agent, nothing else of ours); the version compare and the release
// note excerpt; the UI: Help menu, About button, the offer box with its three answers, Skip This
// Version remembered, "You have the latest version", quiet automatic check at most once a day,
// and no request at all when the policy forbids it.
// [upd-1] the self-update in the UI, against the same kind of local server with a throwaway key
// and a fake installed package: auto mode (quiet download, Ready box, Restart Now installs at exit
// and starts the new program), ask mode (Update Now, Later installs at exit, one balloon), notify
// mode, a tampered archive, an unsigned release, a folder that cannot be written, a downgrade,
// Skip This Version in the Ready box, the migration of updates/checkAtStart, the Settings radios
// (with tool tips), the About box line and the real boxes.
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QtTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QMessageBox>

#include "uib_helpers.h"
#include "upd1_support.h"
#include "mx/update_install.h"
#include "ui/dialogs/settings_dialog.h"
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QRadioButton>
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "mx/update_check.h"
#include "ui/dialogs/about_dialog.h"
#include "ui/update_ui.h"

Q_DECLARE_METATYPE(mx::UpdateChecker::Result)

using namespace mx;
using namespace mx::ui;

namespace {

// a tiny HTTP/1.1 server on 127.0.0.1: answers by path, records the request heads
class FakeServer : public QObject {
public:
    QTcpServer server;
    QList<QByteArray> requests;
    QHash<QString, QPair<int, QByteArray>> routes;   // path -> (status, body)
    QHash<QString, QByteArray> extraHeaders;         // path -> raw header lines
    QStringList silent;                              // paths that never answer (timeout)

    FakeServer()
    {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, s, [this, s] {
                    QByteArray &buf = m_buf[s];
                    buf += s->readAll();
                    if (!buf.contains("\r\n\r\n"))
                        return;
                    requests << buf;
                    const QString path = QString::fromLatin1(buf.split(' ').value(1));
                    buf.clear();
                    if (silent.contains(path))
                        return;
                    const auto r = routes.value(path, qMakePair(404, QByteArray("{\"message\":\"Not Found\"}")));
                    QByteArray out = "HTTP/1.1 " + QByteArray::number(r.first) + " X\r\nContent-Type: application/json\r\n";
                    out += extraHeaders.value(path);
                    out += "Content-Length: " + QByteArray::number(r.second.size()) + "\r\nConnection: close\r\n\r\n" + r.second;
                    s->write(out);
                    s->disconnectFromHost();
                });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
    QString url(const QString &path) const { return QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path); }

private:
    QHash<QTcpSocket *, QByteArray> m_buf;
};

QByteArray release(const char *tag, bool pre = false, bool draft = false, const char *body = "## What's new\n\n* A light theme\n* Check for updates")
{
    QJsonObject o;
    o.insert(QStringLiteral("tag_name"), QString::fromLatin1(tag));
    o.insert(QStringLiteral("name"), QStringLiteral("OctoMX ") + QString::fromLatin1(tag));
    o.insert(QStringLiteral("body"), QString::fromLatin1(body));
    o.insert(QStringLiteral("html_url"), QStringLiteral("https://github.com/robtacconelli/OctoMX/releases/tag/") + QString::fromLatin1(tag));
    o.insert(QStringLiteral("draft"), draft);
    o.insert(QStringLiteral("prerelease"), pre);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// "4.1.0" -> "v4.2.0" etc.
QByteArray bumped(int dMinor)
{
    QList<int> v = parseVersion(QStringLiteral(MX_VERSION_FULL));
    while (v.size() < 3)
        v << 0;
    v[1] += dMinor;
    if (v[1] < 0) {
        v[1] = 0;
        v[0] -= 1;
    }
    return "v" + QByteArray::number(v[0]) + "." + QByteArray::number(v[1]) + ".0";
}

} // namespace

class TstUpdate : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    FakeServer *m_srv = nullptr;

    UpdateChecker::Result runCheck(const QString &path, int timeoutMs = 5000)
    {
        SimpleSettings s;
        s.setValue(QString(keys::updates::Url), m_srv->url(path));
        UpdateChecker c(&s, QStringLiteral(MX_VERSION_FULL));
        c.setTimeoutMs(timeoutMs);
        QSignalSpy spy(&c, &UpdateChecker::finished);
        c.check();
        if (!spy.wait(timeoutMs + 5000))
            return {};
        return spy.first().first().value<UpdateChecker::Result>();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qRegisterMetaType<UpdateChecker::Result>();
        m_srv = new FakeServer;
        QVERIFY(m_srv->server.isListening());
        m_srv->routes.insert(QStringLiteral("/newer"), {200, release(bumped(1).constData())});
        m_srv->routes.insert(QStringLiteral("/same"), {200, release((QByteArray("v") + MX_VERSION_FULL).constData())});
        m_srv->routes.insert(QStringLiteral("/older"), {200, release(bumped(-1).constData())});
        m_srv->routes.insert(QStringLiteral("/pre"), {200, release(bumped(1).constData(), true)});
        m_srv->routes.insert(QStringLiteral("/pretag"), {200, release((bumped(1) + "-beta1").constData())});
        m_srv->routes.insert(QStringLiteral("/draft"), {200, release(bumped(1).constData(), false, true)});
        m_srv->routes.insert(QStringLiteral("/bad"), {200, QByteArray("{\"tag_name\": ")});
        m_srv->routes.insert(QStringLiteral("/notag"), {200, QByteArray("{\"name\":\"x\"}")});
        m_srv->routes.insert(QStringLiteral("/rl403"), {403, QByteArray("{\"message\":\"API rate limit exceeded\"}")});
        m_srv->extraHeaders.insert(QStringLiteral("/rl403"), "X-RateLimit-Remaining: 0\r\n");
        m_srv->routes.insert(QStringLiteral("/rl429"), {429, QByteArray("{}")});
        m_srv->routes.insert(QStringLiteral("/html"), {200, release(bumped(1).constData(), false, false,
                                                                    "<b>bold</b> [link](https://example.org/x) **strong**\n- one\n- two")});
        m_srv->silent << QStringLiteral("/slow");
        // nothing in this test may leave the machine
        QVERIFY(!qEnvironmentVariableIsSet("MX_UPDATE_REAL"));
    }

    void versions()
    {
        QCOMPARE(parseVersion(QStringLiteral("v4.2")), QList<int>({4, 2}));
        QCOMPARE(parseVersion(QStringLiteral("4.2.1")), QList<int>({4, 2, 1}));
        QCOMPARE(parseVersion(QStringLiteral("V10.0.3")), QList<int>({10, 0, 3}));
        QVERIFY(parseVersion(QStringLiteral("v4.2.0-rc1")).isEmpty());
        QVERIFY(parseVersion(QStringLiteral("latest")).isEmpty());
        QCOMPARE(compareVersions(QStringLiteral("4.1"), QStringLiteral("4.1.0")), 0);
        QCOMPARE(compareVersions(QStringLiteral("4.10"), QStringLiteral("4.9.9")), 1);   // numeric, not text
        QCOMPARE(compareVersions(QStringLiteral("v4.1.1"), QStringLiteral("4.1")), 1);
        QCOMPARE(compareVersions(QStringLiteral("3.99"), QStringLiteral("4.0")), -1);
        QCOMPARE(displayVersion(QStringLiteral("4.2.0")), QStringLiteral("4.2"));
        QCOMPARE(displayVersion(QStringLiteral("4.2.1")), QStringLiteral("4.2.1"));
        QCOMPARE(displayVersion(QStringLiteral("5")), QStringLiteral("5.0"));
        QVERIFY(isPrereleaseTag(QStringLiteral("v4.2.0-beta1")));
        QVERIFY(isPrereleaseTag(QStringLiteral("4.2rc1")));
        QVERIFY(!isPrereleaseTag(QStringLiteral("v4.2.0")));
        // the excerpt is plain text: no HTML, no Markdown markers, links reduced to their text
        const QString ex = releaseNotesExcerpt(QStringLiteral("## Hello\n\n<b>bold</b> [link](https://example.org/x) **strong** `code`\n- one\n* two\n```\nhidden\n```\n"));
        QCOMPARE(ex, QStringLiteral("Hello\nbold link strong code\n* one\n* two"));
        const QString longNotes = QString(QStringLiteral("line\n")).repeated(30);
        QCOMPARE(releaseNotesExcerpt(longNotes, 3).count(QLatin1Char('\n')), 2);
        QVERIFY(releaseNotesExcerpt(QString(900, QLatin1Char('x')), 8, 100).size() <= 104);
        ReleaseInfo r;
        r.htmlUrl = QStringLiteral("https://evil.example.org/x");
        QCOMPARE(releasePageUrl(r), QString::fromLatin1(kReleasesPage));   // only github.com pages are opened
        r.htmlUrl = QStringLiteral("https://github.com/robtacconelli/OctoMX/releases/tag/v9.0.0");
        QCOMPARE(releasePageUrl(r), r.htmlUrl);
    }

    void outcomes()
    {
        UpdateChecker::Result r = runCheck(QStringLiteral("/newer"));
        QCOMPARE(int(r.outcome), int(UpdateChecker::Outcome::Newer));
        QCOMPARE(r.release.tag, QString::fromLatin1(bumped(1)));
        QCOMPARE(int(runCheck(QStringLiteral("/same")).outcome), int(UpdateChecker::Outcome::UpToDate));
        QCOMPARE(int(runCheck(QStringLiteral("/older")).outcome), int(UpdateChecker::Outcome::Older));
        QCOMPARE(int(runCheck(QStringLiteral("/pre")).outcome), int(UpdateChecker::Outcome::NotOffered));
        QCOMPARE(int(runCheck(QStringLiteral("/pretag")).outcome), int(UpdateChecker::Outcome::NotOffered));
        QCOMPARE(int(runCheck(QStringLiteral("/draft")).outcome), int(UpdateChecker::Outcome::NotOffered));
        r = runCheck(QStringLiteral("/bad"));
        QCOMPARE(int(r.outcome), int(UpdateChecker::Outcome::Failed));
        QVERIFY(r.error.contains(QStringLiteral("does not understand")));
        QCOMPARE(int(runCheck(QStringLiteral("/notag")).outcome), int(UpdateChecker::Outcome::Failed));
        r = runCheck(QStringLiteral("/missing"));
        QCOMPARE(r.httpStatus, 404);
        QVERIFY(r.error.contains(QStringLiteral("No published release")));
        r = runCheck(QStringLiteral("/rl403"));
        QVERIFY(r.rateLimited);
        QCOMPARE(int(r.outcome), int(UpdateChecker::Outcome::Failed));
        r = runCheck(QStringLiteral("/rl429"));
        QVERIFY(r.rateLimited);
        r = runCheck(QStringLiteral("/slow"), 400);
        QVERIFY(r.timedOut);
        QCOMPARE(int(r.outcome), int(UpdateChecker::Outcome::Failed));
        // unreachable: a port nobody listens on
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 dead = probe.serverPort();
        probe.close();
        SimpleSettings s;
        s.setValue(QString(keys::updates::Url), QStringLiteral("http://127.0.0.1:%1/latest").arg(dead));
        UpdateChecker c(&s, QStringLiteral(MX_VERSION_FULL));
        c.setTimeoutMs(4000);   // Windows retries a refused connect for a few seconds
        QSignalSpy spy(&c, &UpdateChecker::finished);
        c.check();
        QVERIFY(spy.wait(12000));
        r = spy.first().first().value<UpdateChecker::Result>();
        QCOMPARE(int(r.outcome), int(UpdateChecker::Outcome::Failed));
        QVERIFY2(r.error.contains(QStringLiteral("could not be reached")) || r.timedOut, qPrintable(r.error));
    }

    void requestHeaders()
    {
        m_srv->requests.clear();
        runCheck(QStringLiteral("/same"));
        QCOMPARE(m_srv->requests.size(), 1);
        const QByteArray head = m_srv->requests.first().toLower();
        QVERIFY(head.startsWith("get /same http/1.1"));
        QVERIFY(head.contains("accept: application/vnd.github+json"));
        QVERIFY(head.contains(QByteArray("user-agent: octomx/") + QByteArray(MX_VERSION_FULL)));
        QVERIFY(!head.contains("cookie"));
        QVERIFY(!head.contains("authorization"));
        // the schema default points at GitHub's releases/latest of the project
        QCOMPARE(settingDefault(keys::updates::Url).toString(),
                 QStringLiteral("https://api.github.com/repos/robtacconelli/OctoMX/releases/latest"));
        QVERIFY(findSetting(keys::updates::Url)->hidden);
        QCOMPARE(settingDefault(keys::updates::CheckAtStart).toBool(), true);
    }

    void manualCheckUi()
    {
        uib::App app;
        app.start(m_dir.filePath(QStringLiteral("p1")));
        MainWindow *mw = app.mw;
        UpdateController *uc = UpdateController::of(mw);
        QVERIFY(uc);
        Settings *s = app.core->settings();
        // tests: the default policy only allows a check when the address was set on purpose
        QCOMPARE(int(UpdateController::policy()), int(UpdateController::Policy::IfUrlSet));
        QStringList messages;
        UpdateController::s_messageHook = [&](const QString &t) { messages << t; };
        m_srv->requests.clear();
        mw->checkForUpdates();
        QCOMPARE(messages.size(), 1);
        QVERIFY(messages.last().contains(QStringLiteral("off in this test run")));
        QCOMPARE(m_srv->requests.size(), 0);   // nothing was asked

        // the Help button's menu: About OctoMX... (default) / Check for Updates... / Terms of Use...
        QMenu *hm = mw->helpMenu();
        QCOMPARE(hm->actions().size(), 3);
        QCOMPARE(hm->actions().at(2)->text(), QStringLiteral("Terms of Use..."));
        QCOMPARE(hm->actions().at(0)->text(), QStringLiteral("About OctoMX..."));
        QCOMPARE(hm->actions().at(1)->text(), QStringLiteral("Check for Updates..."));
        QCOMPARE(hm->defaultAction(), hm->actions().at(0));

        // a newer version: the offer box text and the three answers
        s->setValue(QString(keys::updates::Url), m_srv->url(QStringLiteral("/newer")));
        QList<UpdateController::Offer> offers;
        int answer = UpdateController::RemindLater;
        UpdateController::s_offerHook = [&](const UpdateController::Offer &o) {
            offers << o;
            return answer;
        };
        QStringList opened;
        UpdateController::s_openUrlHook = [&](const QString &u) { opened << u; };
        QSignalSpy done(uc, &UpdateController::checkFinished);
        hm->actions().at(1)->trigger();   // Help ▸ Check for Updates...
        QVERIFY(done.wait(5000));
        QCOMPARE(offers.size(), 1);
        const QString want = QStringLiteral("OctoMX %1 is available (you have %2).")
                                 .arg(displayVersion(QString::fromLatin1(bumped(1))), displayVersion(QStringLiteral(MX_VERSION_FULL)));
        QVERIFY2(offers.last().text.startsWith(want), qPrintable(offers.last().text));
        QVERIFY(offers.last().text.contains(QStringLiteral("* A light theme")));
        QVERIFY(!offers.last().text.contains(QChar(0x2014)));
        QCOMPARE(opened.size(), 0);   // Remind Me Later: nothing
        QVERIFY(s->s(keys::updates::SkipVersion).isEmpty());
        answer = UpdateController::OpenPage;
        mw->checkForUpdates();
        QVERIFY(done.wait(5000));
        QCOMPARE(opened, QStringList({QStringLiteral("https://github.com/robtacconelli/OctoMX/releases/tag/") + QString::fromLatin1(bumped(1))}));
        answer = UpdateController::SkipVersion;
        mw->checkForUpdates();
        QVERIFY(done.wait(5000));
        QCOMPARE(s->s(keys::updates::SkipVersion), QString::fromLatin1(bumped(1)).mid(1));
        // a manual check still shows a skipped version (the user asked)
        const int before = int(offers.size());
        answer = UpdateController::RemindLater;
        mw->checkForUpdates();
        QVERIFY(done.wait(5000));
        QCOMPARE(int(offers.size()), before + 1);
        // HTML in the release notes stays plain text
        s->setValue(QString(keys::updates::Url), m_srv->url(QStringLiteral("/html")));
        mw->checkForUpdates();
        QVERIFY(done.wait(5000));
        QVERIFY(offers.last().notes.contains(QStringLiteral("bold link strong")));
        QVERIFY(!offers.last().notes.contains(QLatin1Char('<')));

        // same / older / pre-release: "You have the latest version"; failures: a plain message
        messages.clear();
        for (const char *p : {"/same", "/older", "/pre"}) {
            s->setValue(QString(keys::updates::Url), m_srv->url(QString::fromLatin1(p)));
            mw->checkForUpdates();
            QVERIFY(done.wait(5000));
        }
        QCOMPARE(messages.size(), 3);
        for (const QString &m : std::as_const(messages))
            QVERIFY2(m.startsWith(QStringLiteral("You have the latest version (OctoMX %1)").arg(displayVersion(QStringLiteral(MX_VERSION_FULL)))),
                     qPrintable(m));
        messages.clear();
        for (const char *p : {"/bad", "/missing", "/rl429"}) {
            s->setValue(QString(keys::updates::Url), m_srv->url(QString::fromLatin1(p)));
            mw->checkForUpdates();
            QVERIFY(done.wait(5000));
        }
        QCOMPARE(messages.size(), 3);
        QVERIFY(messages.at(0).startsWith(QStringLiteral("OctoMX could not check for updates.")));
        QVERIFY(messages.at(2).contains(QStringLiteral("try again later")));

        // the About box has the button too
        {
            AboutDialog about(app.core);   // on the stack: no parent
            QSignalSpy req(&about, &AboutDialog::checkForUpdatesRequested);
            auto *b = about.findChild<QAbstractButton *>(QStringLiteral("aboutCheckUpdates"));
            QVERIFY(b);
            QCOMPARE(b->text(), QStringLiteral("Check for Updates..."));
            b->click();
            QCOMPARE(req.count(), 1);
        }

        UpdateController::s_offerHook = {};
        UpdateController::s_messageHook = {};
        UpdateController::s_openUrlHook = {};
        app.stop();
    }

    // the first start after an update from a version before 5.0 (the bridge): one box, "Updated to
    // OctoMX <v> (from 4.5)." with the plain version numbers and nothing else
    void updatedFromPre50()
    {
        clearHooks();
        QList<UpdateController::Offer> offers;
        UpdateController::s_offerHook = [&](const UpdateController::Offer &o) { offers << o; return int(UpdateController::RemindLater); };
        UpdateController::setJustUpdatedForTests(QStringLiteral("4.5.0"), QStringLiteral(MX_VERSION_FULL));
        uib::App app;
        app.start(m_dir.filePath(QStringLiteral("updated45")));
        QTRY_COMPARE_WITH_TIMEOUT(offers.size(), 1, 5000);
        QTest::qWait(1600);
        QCOMPARE(offers.size(), 1);
        QCOMPARE(int(offers.first().kind), int(UpdateController::Kind::Updated));
        QCOMPARE(offers.first().text, QStringLiteral("Updated to OctoMX %1 (from 4.5).\n\nWhat's New shows the release notes in your web browser.")
                                          .arg(displayVersion(QStringLiteral(MX_VERSION_FULL))));
        app.stop();
        clearHooks();
    }

    void realOfferBox()
    {
        // the actual message box: classic, plain text, three buttons, Escape = Remind Me Later
        uib::App app;
        app.start(m_dir.filePath(QStringLiteral("p2")));
        app.core->settings()->setValue(QString(keys::updates::Url), m_srv->url(QStringLiteral("/newer")));
        QStringList opened;
        UpdateController::s_openUrlHook = [&](const QString &u) { opened << u; };
        app.mw->checkForUpdates();
        QMessageBox *box = nullptr;
        QTRY_VERIFY((box = app.mw->findChild<QMessageBox *>(QStringLiteral("updateBox"))) && box->isVisible());
        QCOMPARE(box->windowTitle(), QStringLiteral("OctoMX"));
        QCOMPARE(box->textFormat(), Qt::PlainText);
        QStringList texts;
        for (QAbstractButton *b : box->buttons())
            texts << b->text();
        texts.sort();
        QCOMPARE(texts, QStringList({"Open Download Page", "Remind Me Later", "Skip This Version"}));
        box->findChild<QAbstractButton *>(QStringLiteral("updateSkip"))->click();
        QTRY_VERIFY(!app.mw->findChild<QMessageBox *>(QStringLiteral("updateBox")) ||
                    !app.mw->findChild<QMessageBox *>(QStringLiteral("updateBox"))->isVisible());
        QCOMPARE(app.core->settings()->s(keys::updates::SkipVersion), QString::fromLatin1(bumped(1)).mid(1));
        QVERIFY(opened.isEmpty());
        UpdateController::s_openUrlHook = {};
        app.stop();
    }

    void automaticCheck()
    {
        uib::App app;
        app.start(m_dir.filePath(QStringLiteral("p3")));
        UpdateController *uc = UpdateController::of(app.mw);
        Settings *s = app.core->settings();
        QList<UpdateController::Offer> offers;
        QStringList messages;
        UpdateController::s_offerHook = [&](const UpdateController::Offer &o) {
            offers << o;
            return int(UpdateController::RemindLater);
        };
        UpdateController::s_messageHook = [&](const QString &t) { messages << t; };
        // default address (GitHub) in a test window: never due, nothing sent
        QVERIFY(!uc->startCheckDue());
        s->setValue(QString(keys::updates::Url), m_srv->url(QStringLiteral("/newer")));
        QVERIFY(uc->startCheckDue());
        QSignalSpy done(uc, &UpdateController::checkFinished);
        uc->scheduleStartCheck(0);
        QVERIFY(done.wait(5000));
        QCOMPARE(offers.size(), 1);   // a newer version: the box
        QVERIFY(QDateTime::fromString(s->s(keys::updates::LastCheck), Qt::ISODate).isValid());
        QVERIFY(!uc->startCheckDue());   // at most once a day
        // a day later; the version was skipped: quiet
        s->setValue(QString(keys::updates::LastCheck), QDateTime::currentDateTimeUtc().addSecs(-25 * 3600).toString(Qt::ISODate));
        s->setValue(QString(keys::updates::SkipVersion), QString::fromLatin1(bumped(1)).mid(1));
        QVERIFY(uc->startCheckDue());
        uc->scheduleStartCheck(0);
        QVERIFY(done.wait(5000));
        QCOMPARE(offers.size(), 1);
        // failures and "up to date" are quiet in the automatic check (one log line)
        for (const char *p : {"/same", "/bad", "/rl403"}) {
            s->setValue(QString(keys::updates::LastCheck), QString());
            s->setValue(QString(keys::updates::Url), m_srv->url(QString::fromLatin1(p)));
            uc->scheduleStartCheck(0);
            QVERIFY(done.wait(5000));
        }
        QCOMPARE(offers.size(), 1);
        QCOMPARE(messages.size(), 0);
        // switched off: never due
        s->setValue(QString(keys::updates::LastCheck), QString());
        s->setValue(QString(keys::updates::Mode), QStringLiteral("off"));   // [upd-1] was updates/checkAtStart
        QVERIFY(!uc->startCheckDue());
        s->setValue(QString(keys::updates::Mode), QStringLiteral("auto"));
        // demo mode: never, not even with an address
        UpdateController::setDemoMode(true);
        QVERIFY(!uc->startCheckDue());
        m_srv->requests.clear();
        app.mw->checkForUpdates();
        QVERIFY(messages.last().contains(QStringLiteral("demo mode")));
        QCOMPARE(m_srv->requests.size(), 0);
        UpdateController::setDemoMode(false);
        // policy Off (what the app sets for demo cores) / On (normal profile)
        UpdateController::setPolicy(UpdateController::Policy::Off);
        QVERIFY(!uc->networkAllowed());
        UpdateController::setPolicy(UpdateController::Policy::On);
        QVERIFY(uc->networkAllowed());
        UpdateController::setPolicy(UpdateController::Policy::IfUrlSet);
        UpdateController::s_offerHook = {};
        UpdateController::s_messageHook = {};
        app.stop();
    }

    // ================================================================== [upd-1] self-update
private:
    // an installed package of this version (+ the user's files) and a published newer release
    struct Inst {
        QString prog;
        mx::upd::Layout layout;
        QString newVersion;
        QMap<QString, QByteArray> newFiles;
        mx::upd::Manifest newM;
        QByteArray archive;
    };
    Inst install(const QString &name)
    {
        Inst in;
        const QString root = m_dir.filePath(name);
        QDir(root).removeRecursively();
        in.prog = QDir(root).filePath(QStringLiteral("OctoMX"));
        const QString cur = QStringLiteral(MX_VERSION_FULL);
        upd1::writePackage(in.prog, cur, upd1::packageFiles(cur));
        upd1::writeFile(QDir(in.prog).filePath(QStringLiteral("portable.txt")), "portable\n");
        upd1::writeFile(QDir(in.prog).filePath(QStringLiteral("profile/settings.json")), "{}");
        upd1::writeFile(QDir(in.prog).filePath(QStringLiteral("downloads/a.ogg")), "music");
        const bool win = mx::upd::platformTag() == QLatin1String("win64");
        in.layout = mx::upd::detectLayout(win ? in.prog : QDir(in.prog).filePath(QStringLiteral("bin")));
        in.newVersion = QString::fromLatin1(bumped(1)).mid(1);
        in.newFiles = upd1::packageFiles(in.newVersion, {{QStringLiteral("whatsnew.txt"), "new"}});
        const QString src = QDir(QDir(root).filePath(QStringLiteral("src"))).filePath(mx::upd::packageDirName(in.newVersion));
        in.newM = upd1::writePackage(src, in.newVersion, in.newFiles);
        in.archive = upd1::makeArchive(src);
        return in;
    }
    // /<p>/latest answers the release with its assets on this server
    void serve(const QString &p, const Inst &in, bool sign = true, const QByteArray &served = QByteArray())
    {
        const QString name = mx::upd::archiveName(in.newVersion);
        const QByteArray sums = upd1::sha256Hex(in.archive) + "  " + name.toUtf8() + "\n";
        m_up->routes.insert(p + QStringLiteral("/a/") + name, upd1::Route{200, served.isEmpty() ? in.archive : served, {}, true});
        m_up->routes.insert(p + QStringLiteral("/a/SHA256SUMS"), upd1::Route{200, sums, {}, false});
        m_up->routes.insert(p + QStringLiteral("/a/SHA256SUMS.sig"), upd1::Route{200, m_key.sign(sums), {}, false});
        QList<QPair<QString, QString>> assets = {{name, m_up->url(p + QStringLiteral("/a/") + name)},
                                                 {QStringLiteral("SHA256SUMS"), m_up->url(p + QStringLiteral("/a/SHA256SUMS"))}};
        if (sign)
            assets << qMakePair(QStringLiteral("SHA256SUMS.sig"), m_up->url(p + QStringLiteral("/a/SHA256SUMS.sig")));
        m_up->routes.insert(p + QStringLiteral("/latest"), upd1::Route{200, upd1::releaseJson(QStringLiteral("v") + in.newVersion, assets), {}, false});
    }
    void clearHooks()
    {
        UpdateController::s_offerHook = {};
        UpdateController::s_messageHook = {};
        UpdateController::s_openUrlHook = {};
        UpdateController::s_balloonHook = {};
        UpdateController::s_startHook = {};
        UpdateController::s_quitHook = {};
        UpdateController::resetLayout();
    }
    upd1::Key m_key;
    upd1::Server *m_up = nullptr;

private slots:
    void autoModeRestartNow()
    {
        if (!m_up) {
            m_key = upd1::Key::make(true);
            m_up = new upd1::Server;
        }
        Inst in = install(QStringLiteral("auto"));
        QVERIFY2(in.layout.ok, qPrintable(in.layout.why));
        UpdateController::setLayoutForTests(in.layout);
        UpdateController::setRestartArguments({QStringLiteral("--profile"), QStringLiteral("/p"), QStringLiteral("--minimized")});
        serve(QStringLiteral("/auto"), in);
        uib::App app;
        app.start(m_dir.filePath(QStringLiteral("p-auto")));
        UpdateController *uc = UpdateController::of(app.mw);
        Settings *s = app.core->settings();
        QCOMPARE(uc->mode(), QStringLiteral("auto"));   // the default
        s->setValue(QString(keys::updates::Url), m_up->url(QStringLiteral("/auto/latest")));
        QList<UpdateController::Offer> offers;
        UpdateController::s_offerHook = [&](const UpdateController::Offer &o) {
            offers << o;
            return o.kind == UpdateController::Kind::Ready ? int(UpdateController::RestartNow) : int(UpdateController::RemindLater);
        };
        bool quit = false;
        UpdateController::s_quitHook = [&] { quit = true; };
        QString startedExe;
        QStringList startedArgs;
        UpdateController::s_startHook = [&](const QString &exe, const QStringList &args) {
            startedExe = exe;
            startedArgs = args;
            return true;
        };
        QSignalSpy ready(uc, &UpdateController::updateReady);
        uc->scheduleStartCheck(0);
        QVERIFY(ready.wait(30000));
        // quiet: the first box the user sees is "ready to install"
        QCOMPARE(offers.size(), 1);
        QCOMPARE(int(offers.first().kind), int(UpdateController::Kind::Ready));
        QVERIFY2(offers.first().text.startsWith(QStringLiteral("OctoMX %1 is ready to install").arg(displayVersion(in.newVersion))),
                 qPrintable(offers.first().text));
        QVERIFY(offers.first().text.contains(QStringLiteral("* Updates that install themselves")));
        QVERIFY(offers.first().text.contains(QStringLiteral("signature")));
        QVERIFY(quit);   // Restart Now asked OctoMX to close; nothing is installed while it runs
        QVERIFY(UpdateController::hasPendingInstall());
        QVERIFY(uc->statusLine().contains(QStringLiteral("ready to install")));
        QVERIFY(QFileInfo::exists(mx::upd::stageDirFor(in.prog, in.newVersion)));
        QVERIFY(mx::upd::verifyTree(in.prog, in.layout.manifest));   // the old version is still in place
        app.stop();
        // what main.cpp does after the core has shut down
        QVERIFY(UpdateController::finishOnExit());
        QCOMPARE(startedExe, in.layout.exe);
        QCOMPARE(startedArgs, QStringList({QStringLiteral("--profile"), QStringLiteral("/p"), QStringLiteral("--minimized")}));
        QString why;
        QVERIFY2(mx::upd::verifyTree(in.prog, in.newM, &why), qPrintable(why));
        QCOMPARE(upd1::readFile(QDir(in.prog).filePath(QStringLiteral("portable.txt"))), QByteArray("portable\n"));
        QCOMPARE(upd1::readFile(QDir(in.prog).filePath(QStringLiteral("downloads/a.ogg"))), QByteArray("music"));
        QVERIFY(QFileInfo::exists(QDir(in.prog).filePath(QStringLiteral("profile/settings.json"))));
        // the next start removes the old files and knows it was updated
        const mx::upd::Recovery rec = mx::upd::recoverInstall(in.prog);
        QCOMPARE(int(rec.what), int(mx::upd::Recovery::Cleaned));
        QCOMPARE(rec.to, in.newVersion);
        QVERIFY(!QFileInfo::exists(mx::upd::oldDirFor(in.prog, QStringLiteral(MX_VERSION_FULL))));
        clearHooks();
    }

    void askModeUpdateNowLater()
    {
        Inst in = install(QStringLiteral("ask"));
        UpdateController::setLayoutForTests(in.layout);
        serve(QStringLiteral("/ask"), in);
        uib::App app;
        app.start(m_dir.filePath(QStringLiteral("p-ask")));
        UpdateController *uc = UpdateController::of(app.mw);
        Settings *s = app.core->settings();
        s->setValue(QString(keys::updates::Mode), QStringLiteral("ask"));
        s->setValue(QString(keys::updates::Url), m_up->url(QStringLiteral("/ask/latest")));
        QList<UpdateController::Offer> offers;
        UpdateController::s_offerHook = [&](const UpdateController::Offer &o) {
            offers << o;
            return o.kind == UpdateController::Kind::Ask ? int(UpdateController::UpdateNow) : int(UpdateController::InstallLater);
        };
        QStringList balloons;
        UpdateController::s_balloonHook = [&](const QString &t) { balloons << t; };
        bool started = false;
        UpdateController::s_startHook = [&](const QString &, const QStringList &) { return started = true; };
        QSignalSpy ready(uc, &UpdateController::updateReady);
        QSignalSpy done(uc, &UpdateController::checkFinished);
        uc->scheduleStartCheck(0);   // the check at start: ask mode offers Update Now
        QVERIFY(ready.wait(30000));
        QCOMPARE(offers.size(), 2);
        QCOMPARE(int(offers.at(0).kind), int(UpdateController::Kind::Ask));
        QVERIFY(offers.at(0).text.contains(QStringLiteral("Update Now downloads it")));
        QCOMPARE(int(offers.at(1).kind), int(UpdateController::Kind::Ready));
        QCOMPARE(balloons.size(), 1);
        QVERIFY(balloons.first().contains(QStringLiteral("will be installed when you close OctoMX")));
        // Help > Check for Updates... again: the ready box again, but no second balloon
        app.mw->checkForUpdates();
        QVERIFY(done.wait(5000));
        QCOMPARE(offers.size(), 3);
        QCOMPARE(int(offers.at(2).kind), int(UpdateController::Kind::Ready));
        QCOMPARE(balloons.size(), 1);
        app.stop();
        QVERIFY(!UpdateController::finishOnExit());   // installed, no restart (Later)
        QVERIFY(!started);
        QVERIFY(mx::upd::verifyTree(in.prog, in.newM));
        clearHooks();
    }

    void notifyModeAndRefusals()
    {
        Inst in = install(QStringLiteral("ref"));
        UpdateController::setLayoutForTests(in.layout);
        const QString name = mx::upd::archiveName(in.newVersion);
        uib::App app;
        app.start(m_dir.filePath(QStringLiteral("p-ref")));
        UpdateController *uc = UpdateController::of(app.mw);
        Settings *s = app.core->settings();
        QList<UpdateController::Offer> offers;
        UpdateController::s_offerHook = [&](const UpdateController::Offer &o) {
            offers << o;
            return int(UpdateController::RemindLater);
        };
        QSignalSpy done(uc, &UpdateController::checkFinished);
        QSignalSpy failed(uc, &UpdateController::updateFailed);
        auto check = [&](const QString &p) {
            s->setValue(QString(keys::updates::Url), m_up->url(p + QStringLiteral("/latest")));
            s->setValue(QString(keys::updates::LastCheck), QString());
            uc->scheduleStartCheck(0);
            return done.wait(10000);
        };
        // notify: exactly the 4.1 box, nothing downloaded
        serve(QStringLiteral("/n"), in);
        s->setValue(QString(keys::updates::Mode), QStringLiteral("notify"));
        QVERIFY(check(QStringLiteral("/n")));
        QCOMPARE(int(offers.last().kind), int(UpdateController::Kind::Notify));
        QVERIFY(offers.last().reason.isEmpty());
        QVERIFY(offers.last().text.contains(QStringLiteral("does not download or install anything by itself")));
        QCOMPARE(m_up->count(QStringLiteral("/n/a/SHA256SUMS")), 0);
        s->setValue(QString(keys::updates::Mode), QStringLiteral("auto"));
        // a tampered archive: refused after the download, the download page offered with the reason
        QByteArray bad = in.archive;
        bad[bad.size() / 3] = char(bad[bad.size() / 3] ^ 0x20);
        serve(QStringLiteral("/t"), in, true, bad);
        QVERIFY(check(QStringLiteral("/t")));
        QVERIFY(failed.wait(20000));
        QCOMPARE(int(offers.last().kind), int(UpdateController::Kind::Notify));
        QVERIFY2(offers.last().reason.contains(QStringLiteral("changed or damaged")), qPrintable(offers.last().reason));
        QVERIFY(!UpdateController::hasPendingInstall());
        QVERIFY(mx::upd::verifyTree(in.prog, in.layout.manifest));
        // an unsigned release: never downloaded
        serve(QStringLiteral("/u"), in, false);
        QVERIFY(check(QStringLiteral("/u")));
        QCOMPARE(int(offers.last().kind), int(UpdateController::Kind::Notify));
        QVERIFY(offers.last().reason.contains(QStringLiteral("not signed")));
        QCOMPARE(m_up->count(QStringLiteral("/u/a/") + name), 0);
        // a folder OctoMX cannot write to / not one of our packages
        mx::upd::Layout ro = in.layout;
        ro.ok = false;
        ro.why = QStringLiteral("OctoMX cannot write to its folder C:\\Program Files\\OctoMX (test)");
        UpdateController::setLayoutForTests(ro);
        serve(QStringLiteral("/w"), in);
        QVERIFY(check(QStringLiteral("/w")));
        QVERIFY(offers.last().reason.contains(QStringLiteral("cannot write")));
        QVERIFY(offers.last().text.contains(QStringLiteral("Open Download Page")));
        QCOMPARE(m_up->count(QStringLiteral("/w/a/SHA256SUMS")), 0);
        UpdateController::setLayoutForTests(in.layout);
        // a downgrade offered by the server: "latest version", never downloaded
        const int n = int(offers.size());
        m_up->routes.insert(QStringLiteral("/old/latest"), upd1::Route{200, upd1::releaseJson(QString::fromLatin1(bumped(-1)), {}), {}, false});
        QVERIFY(check(QStringLiteral("/old")));
        QCOMPARE(int(offers.size()), n);
        // ... and a release older or equal is never installed, whatever asks for it
        ReleaseInfo same;
        QVERIFY(parseLatestRelease(upd1::releaseJson(QStringLiteral("v") + QStringLiteral(MX_VERSION_FULL), {{name, m_up->url(QStringLiteral("/n/a/") + name)}}), &same));
        QString why;
        QVERIFY(!uc->canAutoInstall(same, &why));
        QVERIFY(why.contains(QStringLiteral("newer")));
        uc->startUpdate(same, true);
        QVERIFY(failed.count() >= 2);
        QCOMPARE(int(uc->state()), int(UpdateController::State::Idle));
        app.stop();
        QVERIFY(!UpdateController::finishOnExit());
        QVERIFY(mx::upd::verifyTree(in.prog, in.layout.manifest));
        clearHooks();
    }

    void readySkipThisVersion()
    {
        Inst in = install(QStringLiteral("skip"));
        UpdateController::setLayoutForTests(in.layout);
        serve(QStringLiteral("/s"), in);
        uib::App app;
        app.start(m_dir.filePath(QStringLiteral("p-skip")));
        UpdateController *uc = UpdateController::of(app.mw);
        app.core->settings()->setValue(QString(keys::updates::Url), m_up->url(QStringLiteral("/s/latest")));
        UpdateController::s_offerHook = [&](const UpdateController::Offer &) { return int(UpdateController::SkipVersion); };
        QSignalSpy ready(uc, &UpdateController::updateReady);
        uc->scheduleStartCheck(0);
        QVERIFY(ready.wait(30000));
        QCOMPARE(app.core->settings()->s(keys::updates::SkipVersion), in.newVersion);
        QVERIFY(!UpdateController::hasPendingInstall());
        QVERIFY(!QFileInfo::exists(mx::upd::stageDirFor(in.prog, in.newVersion)));
        app.stop();
        QVERIFY(!UpdateController::finishOnExit());
        QVERIFY(mx::upd::verifyTree(in.prog, in.layout.manifest));
        clearHooks();
    }

    void settingsAboutAndBoxes()
    {
        // 4.2 profiles: "Check for a new version at start" off stays off
        const QString prof = m_dir.filePath(QStringLiteral("p-mig"));
        {
            uib::App app;
            app.start(prof);
            app.core->settings()->setValue(QString(keys::updates::CheckAtStart), false);
            app.core->settings()->setValue(QString(keys::updates::ModeMigrated), false);
            app.stop();
        }
        uib::App app;
        app.start(prof);
        QCOMPARE(app.core->settings()->s(keys::updates::Mode), QStringLiteral("off"));
        UpdateController *uc = UpdateController::of(app.mw);
        QCOMPARE(uc->mode(), QStringLiteral("off"));
        QCOMPARE(settingDefault(keys::updates::Mode).toString(), QStringLiteral("auto"));
        // Settings > Miscellaneous > System Integration: four radios, each with a tool tip
        {
            SettingsDialog d(app.core, app.mw);
            QVERIFY(d.showPage(QStringLiteral("misc/system")));
            for (const char *m : {"auto", "ask", "notify", "off"}) {
                auto *r = d.findChild<QRadioButton *>(QStringLiteral("set_updates_mode_") + QString::fromLatin1(m));
                QVERIFY2(r, m);
                QVERIFY2(!r->toolTip().isEmpty(), m);
                QCOMPARE(r->isChecked(), qstrcmp(m, "off") == 0);
            }
            QVERIFY(!d.findChild<QCheckBox *>(QStringLiteral("set_updates_checkAtStart")));
        }
        // the About box: version and what the last check found
        app.core->settings()->setValue(QString(keys::updates::LatestSeen), QString::fromLatin1(bumped(1)).mid(1));
        {
            AboutDialog about(app.core, app.mw);
            auto *l = about.findChild<QLabel *>(QStringLiteral("aboutUpdateStatus"));
            QVERIFY(l);
            QCOMPARE(l->text(), QStringLiteral("OctoMX %1 is available (you have %2).")
                                    .arg(displayVersion(QString::fromLatin1(bumped(1))), displayVersion(QStringLiteral(MX_VERSION_FULL))));
        }
        app.core->settings()->setValue(QString(keys::updates::LatestSeen), QStringLiteral(MX_VERSION_FULL));
        QCOMPARE(uc->statusLine(), QStringLiteral("OctoMX %1 is up to date.").arg(displayVersion(QStringLiteral(MX_VERSION_FULL))));
        // the real boxes: plain text, every button with a tool tip
        ReleaseInfo r;
        QVERIFY(parseLatestRelease(release(bumped(1).constData(), false, false, "<b>bold</b> notes"), &r));
        QStringList opened;
        UpdateController::s_openUrlHook = [&](const QString &u) { opened << u; };
        struct Want { const char *kind, *box; QStringList buttons; };
        const QList<Want> wants = {
            {"ask", "updateBox", {QStringLiteral("updateNow"), QStringLiteral("updateLater"), QStringLiteral("updateSkip")}},
            {"ready", "updateReadyBox", {QStringLiteral("updateRestart"), QStringLiteral("updateInstallLater"), QStringLiteral("updateSkip")}},
            {"progress", "updateProgressBox", {QStringLiteral("updateHide"), QStringLiteral("updateCancel")}},
            {"updated", "updatedBox", {QStringLiteral("updatedWhatsNew"), QStringLiteral("updatedOk")}},
        };
        for (const Want &w : wants) {
            uc->showDemoBox(QString::fromLatin1(w.kind), r);
            QWidget *box = nullptr;
            QTRY_VERIFY((box = app.mw->findChild<QWidget *>(QString::fromLatin1(w.box))) && box->isVisible());
            for (const QString &b : w.buttons) {
                auto *pb = box->findChild<QAbstractButton *>(b);
                QVERIFY2(pb, qPrintable(b));
                QVERIFY2(!pb->toolTip().isEmpty(), qPrintable(b));
            }
            if (auto *t = box->findChild<QLabel *>(QStringLiteral("updateText")); t && qstrcmp(w.kind, "progress") != 0)
                QCOMPARE(t->textFormat(), Qt::PlainText);   // release notes are never interpreted as HTML
            if (qstrcmp(w.kind, "ask") == 0) {
                auto *link = box->findChild<QLabel *>(QStringLiteral("updateLink"));
                QVERIFY(link && !link->toolTip().isEmpty());
                emit link->linkActivated(QStringLiteral("page"));
                QCOMPARE(opened.size(), 1);
            }
            box->findChild<QAbstractButton *>(w.buttons.last())->click();
            QTRY_VERIFY(!app.mw->findChild<QWidget *>(QString::fromLatin1(w.box)) ||
                        !app.mw->findChild<QWidget *>(QString::fromLatin1(w.box))->isVisible());
        }
        QVERIFY(!UpdateController::hasPendingInstall());   // the pictures never install anything
        app.stop();
        clearHooks();
    }

    void cleanupTestCase()
    {
        delete m_srv;
        delete m_up;
#ifdef MX_UPDATE_TEST_HOOKS
        mx::upd::clearTestTrustedKeys();
#endif
    }
};

QTEST_MAIN(TstUpdate)
#include "tst_update.moc"
