// SPDX-License-Identifier: GPL-3.0-or-later
// ui-b: the secondary windows (Ignored Users, Browse, Whois, Private Message) and the Hotlist
// actions that open them: actions reach UserService / TransferService / ChatService, lists
// update on signals, one window per user, windows are deleted on close (no leaks), geometry persists.
#include <QtCore/QPointer>
#include <QtWidgets/QPushButton>

#include "uib_helpers.h"
#include "ui/dialogs/browse_window.h"
#include "ui/dialogs/user_windows.h"
#include "ui/hotlist_pane.h"
#include "ui/kit/list_view.h"
#include "ui/kit/status_bar.h"
#include "ui/models/browse_model.h"
#include "ui/models/chat_models.h"
#include "ui/models/hotlist_model.h"
#include "ui/pages/chat_view.h"

using namespace mx;
using namespace mx::ui;

class TstUibWindows : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    uib::App m_app;
    QString profile() const { return m_dir.filePath(QStringLiteral("profile")); }

    template <class W>
    QList<W *> windows() const
    {
        return m_app.mw->findChildren<W *>(QString(), Qt::FindDirectChildrenOnly);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_app.start(profile());
    }
    void cleanupTestCase() { m_app.stop(); }

    void ignoredWindow()
    {
        const int before = uib::visibleTopLevels();
        m_app.mw->findChild<QAbstractButton *>(QStringLiteral("btnIgnored"))->click();
        auto *w = m_app.mw->findChild<IgnoredWindow *>();
        QVERIFY(w);
        QVERIFY(w->isVisible());
        QCOMPARE(w->windowTitle(), QStringLiteral("Ignored Users"));
        const int n = int(m_app.core->users()->ignored().size());
        QCOMPARE(w->model()->userCount(), n);
        QCOMPARE(w->statusBar()->text(), QStringLiteral("%1 users in list").arg(n));
        // Whois / Remove disabled without a selection
        auto *remove = w->findChild<QPushButton *>(QStringLiteral("ignoredRemove"));
        QVERIFY(!remove->isEnabled());
        w->list()->selectionModel()->select(w->model()->index(0), QItemSelectionModel::Select);
        QVERIFY(remove->isEnabled());
        remove->click();
        QCOMPARE(m_app.core->users()->ignored().size(), n - 1);
        QCOMPARE(w->model()->userCount(), n - 1);   // updated from ignoredChanged
        // a second click on Ignored raises the same window
        m_app.mw->openIgnored();
        QCOMPARE(windows<IgnoredWindow>().size(), 1);
        // geometry persists
        w->resize(400, 300);
        QPointer<IgnoredWindow> guard(w);
        w->close();
        QTRY_VERIFY(guard.isNull());
        QCOMPARE(uib::visibleTopLevels(), before);
        m_app.mw->openIgnored();
        w = m_app.mw->findChild<IgnoredWindow *>();
        QCOMPARE(w->size(), QSize(400, 300));
        w->close();
    }

    void browseWindow()
    {
        const UserRef u = uib::user(QStringLiteral("Pim605_12345"));
        QSignalSpy spy(m_app.core->users(), &UserService::browseResults);
        m_app.mw->openBrowse(u);
        auto *w = m_app.mw->findChild<BrowseWindow *>();
        QVERIFY(w);
        QCOMPARE(w->windowTitle(), QStringLiteral("Pim605_12345 on OctoMX"));
        QTRY_VERIFY(!w->isLoading());
        QCOMPARE(w->model()->entryCount(), 303);
        QVERIFY(w->statusBar()->text().startsWith(QStringLiteral("Finished downloading list> 303 files (")));
        QVERIFY(w->model()->folderCount() >= 5);
        // folders are expanded: row 1 is a file
        QVERIFY(w->list()->isExpanded(w->list()->model()->index(0, 0)));
        // legend colours from the running demo downloads
        bool canJoin = false, incomplete = false;
        for (int i = 0; i < w->model()->entryCount(); ++i) {
            const QString p = w->model()->entry(i).path;
            if (p.endsWith(QStringLiteral("The Best Of Velorine - Entire Album.mp3")))
                canJoin = true;
            if (p.endsWith(QStringLiteral("Velorine - Marigold.mp3")))
                incomplete = true;
        }
        QVERIFY(canJoin && incomplete);
        // one window per user
        m_app.mw->openBrowse(u);
        QCOMPARE(windows<BrowseWindow>().size(), 1);
        // download the first file -> TransferService
        const int before = int(m_app.core->transfers()->downloads().size());
        const QModelIndex fileRow = w->list()->model()->index(0, 0, w->list()->model()->index(0, 0));
        w->list()->selectRow(fileRow);
        w->downloadSelected();
        QCOMPARE(m_app.core->transfers()->downloads().size(), before + 1);
        const TransferInfo t = m_app.core->transfers()->downloads().last();
        QCOMPARE(t.name, BrowseModel::fileNameOf(w->model()->entryAt(w->list()->mapToSource(fileRow))->path));
        // Show Full Paths flattens the list
        w->findChild<QPushButton *>(QStringLiteral("browseFullPaths"))->click();
        QCOMPARE(w->list()->model()->rowCount(), 303);
        QPointer<BrowseWindow> guard(w);
        w->close();
        QTRY_VERIFY(guard.isNull());
    }

    void browseColoursForSourceUser()
    {
        // zqvtra662_44120 is a source of the running Velorine download -> "currently downloading"
        m_app.mw->openBrowse(uib::user(QStringLiteral("zqvtra662_44120")));
        auto *w = m_app.mw->findChild<BrowseWindow *>();
        QTRY_VERIFY(!w->isLoading());
        w->findChild<QPushButton *>(QStringLiteral("browseFullPaths"))->click();
        bool blue = false;
        for (int r = 0; r < w->list()->model()->rowCount(); ++r) {
            const QModelIndex i = w->list()->model()->index(r, 0);
            if (i.data().toString().endsWith(QStringLiteral("The Best Of Velorine - Entire Album.mp3")))
                blue = i.data(Qt::ForegroundRole).value<QColor>() == color(keys::colors::CurrentlyDownloading);
        }
        QVERIFY(blue);
        w->close();
    }

    void whoisWindow()
    {
        const UserRef u = uib::user(QStringLiteral("Twirl836_24616"));
        m_app.mw->openWhois(u);
        auto *w = m_app.mw->findChild<WhoisWindow *>();
        QVERIFY(w);
        QCOMPARE(w->windowTitle(), QStringLiteral("Whois: Twirl836_24616"));
        QTRY_VERIFY(w->hasResult());
        const auto lines = w->view()->lines();
        QCOMPARE(lines.value(0).first, QStringLiteral("User:"));
        QCOMPARE(lines.value(0).second, QStringLiteral("Twirl836_24616"));
        bool conn = false, online = false;
        for (const auto &l : lines) {
            conn |= l.first == QStringLiteral("Connection:") && l.second == QStringLiteral("VDSL");   // [qa-1] modern list in the mock
            online |= l.first == QStringLiteral("Online:") && l.second.startsWith(QStringLiteral("2 hours 13 minutes"));
        }
        QVERIFY(conn && online);
        // "Send Message" opens the PM window
        w->findChild<QPushButton *>(QStringLiteral("whoisSendMessage"))->click();
        QVERIFY(m_app.mw->findChild<PrivateMessageWindow *>());
        m_app.mw->findChild<PrivateMessageWindow *>()->close();
        w->close();
        QTest::qWait(10);
    }

    void whoisFormatRealCoreKeys()
    {
        QString err;
        const UserRef u = uib::user(QStringLiteral("peer"));
        const auto rows = WhoisWindow::format(u, {{QStringLiteral("name"), QStringLiteral("peer")},
                                                  {QStringLiteral("online"), false},
                                                  {QStringLiteral("files"), 12},
                                                  {QStringLiteral("error"), QStringLiteral("No whois service")}},
                                              &err);
        QCOMPARE(err, QStringLiteral("No whois service"));
        QCOMPARE(rows.value(0).second, QStringLiteral("peer"));
        bool status = false;
        for (const auto &r : rows)
            status |= r.first == QStringLiteral("Status:") && r.second == QStringLiteral("Offline");
        QVERIFY(status);
    }

    void privateMessages()
    {
        const UserRef u = uib::user(QStringLiteral("pmbuddy_33333"));
        m_app.mw->openPrivateMessage(u);
        auto *w = m_app.mw->findChild<PrivateMessageWindow *>();
        QVERIFY(w);
        QCOMPARE(w->windowTitle(), QStringLiteral("pmbuddy_33333 - Private Message"));
        QSignalSpy sent(m_app.core->chat(), &ChatService::messageReceived);
        w->submit(QStringLiteral("hi holly"));
        QVERIFY(sent.count() >= 1);
        QTRY_VERIFY(w->output()->plainText().contains(QStringLiteral("> hi holly")));
        QTRY_VERIFY(w->output()->plainText().contains(QStringLiteral("<pmbuddy_33333> hi!")));
        // temporary hotlist entry
        bool temp = false;
        HotlistModel *hm = m_app.mw->hotlistPane()->model();
        for (int r = 0; r < hm->rowCount(); ++r)
            temp |= hm->kind(r) == HotlistModel::TemporaryRow && hm->user(r).key() == u.key();
        QVERIFY(temp);
        w->close();
        QTRY_VERIFY(windows<PrivateMessageWindow>().isEmpty());
        // reopening shows the conversation of the session
        m_app.mw->openPrivateMessage(u);
        w = m_app.mw->findChild<PrivateMessageWindow *>();
        QVERIFY(w->output()->plainText().contains(QStringLiteral("hi holly")));
        w->close();
        QTRY_VERIFY(windows<PrivateMessageWindow>().isEmpty());
        // an incoming message from somebody new opens a window by itself
        m_app.core->chat()->sendPrivateMessage(uib::user(QStringLiteral("newfriend_11111")), QStringLiteral("ping"));
        QTRY_COMPARE(windows<PrivateMessageWindow>().size(), 1);
        QCOMPARE(windows<PrivateMessageWindow>().value(0)->user().name, QStringLiteral("newfriend_11111"));
        windows<PrivateMessageWindow>().value(0)->close();
        QTRY_VERIFY(windows<PrivateMessageWindow>().isEmpty());
    }

    void hotlistActions()
    {
        m_app.mw->setHotlistVisible(true);
        HotlistPane *hp = m_app.mw->hotlistPane();
        HotlistModel *hm = hp->model();
        int row = -1;
        for (int r = 0; r < hm->rowCount(); ++r)
            if (hm->kind(r) == HotlistModel::OnlineRow) {
                row = r;
                break;
            }
        QVERIFY(row >= 0);
        const UserRef u = hm->user(row);
        auto pick = [&](const QString &text) {
            hp->showContextMenu(row, hp->mapToGlobal(QPoint(10, 40)));
            QAction *a = uib::menuAction(uib::openMenu(), text);
            QVERIFY2(a, qPrintable(text));
            a->trigger();
            uib::closeMenus();
        };
        pick(QStringLiteral("Browse"));
        QCOMPARE(windows<BrowseWindow>().size(), 1);
        QCOMPARE(windows<BrowseWindow>().value(0)->user().key(), u.key());
        pick(QStringLiteral("Whois"));
        QCOMPARE(windows<WhoisWindow>().size(), 1);
        pick(QStringLiteral("Send Message"));
        QCOMPARE(windows<PrivateMessageWindow>().size(), 1);
        pick(QStringLiteral("Ignore"));
        QVERIFY(m_app.core->users()->isIgnored(u));
        const int before = int(m_app.core->users()->hotlist().size());
        pick(QStringLiteral("Remove"));
        QCOMPARE(m_app.core->users()->hotlist().size(), before - 1);
        // everything closes and is deleted
        for (QWidget *w : m_app.mw->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
            if (w->isWindow() && w->isVisible())
                w->close();
        QTRY_VERIFY(windows<BrowseWindow>().isEmpty() && windows<WhoisWindow>().isEmpty() &&
                    windows<PrivateMessageWindow>().isEmpty());
    }

    void manyWindowsNoLeak()
    {
        const int before = int(m_app.mw->findChildren<QWidget *>().size());
        for (int i = 0; i < 20; ++i) {
            const UserRef u = uib::user(QStringLiteral("user%1_00000").arg(i));
            m_app.mw->openBrowse(u);
            m_app.mw->openWhois(u);
            m_app.mw->openPrivateMessage(u);
        }
        QCOMPARE(windows<BrowseWindow>().size(), 20);
        QTest::qWait(50);
        for (QWidget *w : m_app.mw->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
            if (w->isWindow() && w->isVisible())
                w->close();
        QTRY_COMPARE(int(m_app.mw->findChildren<QWidget *>().size()), before);
    }
};

QTEST_MAIN(TstUibWindows)
#include "tst_uib_windows.moc"
