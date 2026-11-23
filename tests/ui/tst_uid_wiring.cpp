// SPDX-License-Identifier: GPL-3.0-or-later
// ui-d: every control wired to the glue-2 / glue-3 service API, on the frozen demo core,
// Search (ping, hash search, suspects, canonical settings keys, Manually Resume), Transfers (Find
// Sources Now, Enter Queue / Retry, per-source cancel + priority, upload Start Transfer + priority,
// Change Destination, Load Incomplete, the delete warning of a resumed file), Networks (edit,
// import .MET text + binary server.met, server messages window, summary numbers), title bar,
// user ping, Bandwidth (detailed samples), Shared Files (types, excluded folders), Chat (chat
// networks), Whois (request ids), the settings migration and the session-only warning switch.
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>

#include "uib_helpers.h"
#include "ui/dialogs/file_download_dialog.h"
#include "ui/dialogs/server_messages.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/share_dialogs.h"
#include "ui/dialogs/user_windows.h"
#include "ui/kit/list_view.h"
#include "ui/kit/status_bar.h"
#include "ui/models/chat_models.h"
#include "ui/kit/session.h"
#include "ui/kit/widgets.h"
#include "ui/models/search_model.h"
#include "ui/models/server_model.h"
#include "ui/models/transfer_model.h"
#include "ui/pages/bandwidth_page.h"
#include "ui/pages/chat_page.h"
#include "ui/pages/networks_page.h"
#include "ui/pages/search_page.h"
#include "ui/pages/shared_page.h"
#include "ui/pages/transfers_page.h"
#include "mx/settings_keys.h"

using namespace mx;
using namespace mx::ui;

namespace {
QModelIndex viewRowOf(MxListView *l, const QModelIndex &src)
{
    return l->mapFromSource(src);
}
} // namespace

class TstUidWiring : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    uib::App m_app;
    QStringList m_asked;   // titles+texts of the Yes/No questions the pages asked
    int m_answer = QMessageBox::Yes;

    QString profile() const { return m_dir.filePath(QStringLiteral("profile")); }
    SearchPage *search() const { return qobject_cast<SearchPage *>(m_app.mw->page(QStringLiteral("search"))); }
    TransfersPage *transfers() const { return qobject_cast<TransfersPage *>(m_app.mw->page(QStringLiteral("transfers"))); }
    NetworksPage *networks() const { return qobject_cast<NetworksPage *>(m_app.mw->page(QStringLiteral("networks"))); }
    Settings *st() const { return m_app.core->settings(); }

    SearchTab *runSearch(const QString &t1, const QString &t2 = QString())
    {
        m_app.mw->showPage(QStringLiteral("search"));
        SearchTab *t = search()->currentTab();
        t->setTexts(t1, t2);
        t->startSearch();
        QTest::qWait(20);
        t->flushPending();
        return t;
    }
    int downloadRowOf(const QString &id) const { return transfers()->downloadModel()->rowOfId(id); }
    TransferInfo download(const QString &id) const { return m_app.core->transfers()->transfer(id); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        // a profile from before the clean-up: the retired private keys must be migrated
        QDir().mkpath(profile());
        QFile f(QDir(profile()).filePath(QStringLiteral("settings.json")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        QJsonObject o;
        o.insert(QStringLiteral("ui/searchFileType"), QStringLiteral("video"));
        o.insert(QStringLiteral("ui/searchAutoFindSources"), 25);
        o.insert(QStringLiteral("ui/searchAutoEnterQueue"), 40);
        o.insert(QStringLiteral("ui/suppressDangerWarning"), true);
        o.insert(QStringLiteral("ui/searchConnectionFilter"), QStringLiteral("8"));
        f.write(QJsonDocument(o).toJson());
        f.close();
        Session::instance()->reset();
        SearchTab::s_askHook = [this](const QString &title, const QString &text) {
            m_asked << title + QLatin1Char('|') + text;
            return m_answer;
        };
        TransfersPage::s_askHook = SearchTab::s_askHook;
        m_app.start(profile());
    }
    void cleanupTestCase()
    {
        m_app.stop();
        SearchTab::s_askHook = nullptr;
        TransfersPage::s_askHook = nullptr;
    }

    // ------------------------------------------------------------------ settings keys
    void legacyKeysMigrated()
    {
        QCOMPARE(st()->s(keys::search::DefaultType), QStringLiteral("video"));   // [ui-f] codes
        QCOMPARE(st()->i(keys::search::AutoFindSourcesMin), 25);
        QCOMPARE(st()->i(keys::search::AutoEnterQueue), 40);
        QCOMPARE(st()->s(keys::search::DefaultConnection), QStringLiteral("DSL or faster"));
        // the old keys are gone (unknown keys read as invalid), the session switch was not carried over
        for (const char *k : {"ui/searchFileType", "ui/searchAutoFindSources", "ui/searchAutoEnterQueue", "ui/suppressDangerWarning"}) {
            QVERIFY2(!findSetting(QString::fromLatin1(k)), k);
            QVERIFY2(!st()->value(QString::fromLatin1(k)).isValid(), k);
        }
        QCOMPARE(st()->s(keys::ui::SearchConnectionFilter), QString());
        QVERIFY(!Session::instance()->suppressDangerousWarning());
        // the Search page shows them
        SearchTab *t = search()->currentTab();
        QCOMPARE(t->findChild<MenuButton *>(QStringLiteral("searchFileType"))->currentData().toString(), QStringLiteral("video"));
        QCOMPARE(t->findChild<MenuButton *>(QStringLiteral("searchAfs"))->text(), QStringLiteral("Auto Find Sources - 25m"));
        QCOMPARE(t->findChild<MenuButton *>(QStringLiteral("searchAeq"))->text(), QStringLiteral("Auto Enter Queue - 40"));
        // [qa-1] the old rank 8 ("DSL or faster") maps onto the modern list's DSL (speed rank 1)
        QCOMPARE(t->findChild<MenuButton *>(QStringLiteral("searchConnection"))->currentData().toInt(), 1);
        QCOMPARE(t->proxy()->minConnectionRank(), 1);
        // a second migration run changes nothing
        migrateLegacySettings(st());
        QCOMPARE(st()->s(keys::search::DefaultType), QStringLiteral("video"));
    }

    void searchPageWritesCanonicalKeys()
    {
        SearchTab *t = search()->currentTab();
        auto *type = t->findChild<MenuButton *>(QStringLiteral("searchFileType"));
        auto *afs = t->findChild<MenuButton *>(QStringLiteral("searchAfs"));
        auto *conn = t->findChild<MenuButton *>(QStringLiteral("searchConnection"));
        // the type buttons: captions and types from search/typeButton*
        st()->setValue(QStringLiteral("search/typeButtonAudioLabel"), QStringLiteral("Music"));
        st()->setValue(keys::search::TypeButtonAudio, QStringLiteral("OGG Audio - Any Bitrate"));
        auto *audio = t->findChild<PushButton *>(QStringLiteral("searchAudio"));
        QCOMPARE(audio->text(), QStringLiteral("Music"));
        audio->click();
        // [ui-f] the old "OGG Audio" entry is gone: its nearest is all audio
        QCOMPARE(type->currentData().toString(), QStringLiteral("audio"));
        QCOMPARE(st()->s(keys::search::DefaultType), QStringLiteral("audio"));
        QVERIFY(audio->isChecked());
        QCOMPARE(st()->s(keys::ui::SearchType), QStringLiteral("audio"));
        // menu choices write the canonical keys
        for (QAction *a : afs->menu()->actions())
            if (a->data().toInt() == 15)
                a->trigger();
        QCOMPARE(st()->i(keys::search::AutoFindSourcesMin), 15);
        QCOMPARE(st()->i(keys::transfers::SourceSearchIntervalMin), 15);
        for (QAction *a : conn->menu()->actions())
            if (a->data().toInt() == 0 && !a->isSeparator())
                a->trigger();
        QCOMPARE(st()->s(keys::search::DefaultConnection), QStringLiteral("Any Connection"));
        // ... and follow the Settings dialog (the Transfers page shares AFS / AEQ)
        st()->setValue(keys::search::AutoEnterQueue, 9);
        QCOMPARE(t->findChild<MenuButton *>(QStringLiteral("searchAeq"))->text(), QStringLiteral("Auto Enter Queue - 9"));
        QCOMPARE(transfers()->findChild<MenuButton *>(QStringLiteral("dlAeq"))->text(), QStringLiteral("Auto Enter Queue - 9"));
        st()->setValue(keys::search::DefaultType, QStringLiteral("Any File / Bitrate"));
        QCOMPARE(type->currentData().toString(), QStringLiteral("any"));
        st()->setValue(QStringLiteral("search/typeButtonAudioLabel"), QStringLiteral("Audio"));
        st()->setValue(keys::search::TypeButtonAudio, QStringLiteral("MP3 Audio - Any Bitrate"));
    }

    void dangerousWarningSessionAndPermanent()
    {
        SearchTab *t = runSearch(QStringLiteral("anything goes"));
        int exe = -1;
        for (int r = 0; r < t->model()->hitCount(); ++r)
            if (t->model()->hitAt(r).fileName.endsWith(QLatin1String(".zip")) && t->model()->hitAt(r).suspect.isEmpty())
                exe = r;
        QVERIFY(exe >= 0);
        const QModelIndex vi = viewRowOf(t->list(), t->model()->index(exe, 0));
        m_asked.clear();
        m_answer = QMessageBox::No;
        t->downloadIndex(vi);
        QCOMPARE(m_asked.size(), 1);
        QVERIFY(m_asked.first().contains(QStringLiteral("This is a program or script")));
        // "this session only" lives in memory: the Settings dialog check box toggles it
        m_app.mw->openSettings(QStringLiteral("File Transfers"));
        auto *dlg = m_app.mw->findChild<SettingsDialog *>();
        QVERIFY(dlg);
        auto *box = dlg->findChild<QCheckBox *>(QStringLiteral("sessionSuppressDangerWarning"));
        QVERIFY(box);
        box->setChecked(true);
        QVERIFY(Session::instance()->suppressDangerousWarning());
        QVERIFY(!st()->value(QStringLiteral("ui/suppressDangerWarning")).isValid());   // never written
        m_asked.clear();
        t->downloadIndex(vi);
        QCOMPARE(m_asked.size(), 0);
        box->setChecked(false);
        dlg->close();
        // the permanent switch
        st()->setValue(keys::transfers::SuppressDangerousWarning, true);
        t->downloadIndex(vi);
        QCOMPARE(m_asked.size(), 0);
        st()->setValue(keys::transfers::SuppressDangerousWarning, false);
        m_answer = QMessageBox::Yes;
    }

    // ------------------------------------------------------------------ search
    void suspectHitsFlagged()
    {
        SearchTab *t = runSearch(QStringLiteral("big buck bunny"));
        QList<int> suspects;
        for (int r = 0; r < t->model()->hitCount(); ++r)
            if (!t->model()->hitAt(r).suspect.isEmpty())
                suspects << r;
        QCOMPARE(suspects.size(), 3);
        const int r = suspects.first();
        const QModelIndex src = t->model()->index(r, search::File);
        QCOMPARE(src.data(Qt::ForegroundRole).value<QColor>(), color(ckeys::SuspectText));
        QVERIFY(src.data(Qt::ToolTipRole).toString().contains(QStringLiteral("Suspected fake: ")));
        QVERIFY(t->model()->index(r, search::Status).data(Qt::ToolTipRole).toString().contains(t->model()->hitAt(r).suspect));
        // selecting the row names the reason in the status bar
        const QModelIndex vi = viewRowOf(t->list(), src);
        t->list()->selectionModel()->setCurrentIndex(vi, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QVERIFY(t->statusFields().last().startsWith(QStringLiteral("Suspected fake: ")));
        // a warning before downloading it, once per file
        m_asked.clear();
        m_answer = QMessageBox::Yes;
        Session::instance()->setSuppressDangerousWarning(true);   // the .exe fake would ask twice otherwise
        t->downloadIndex(vi);
        QCOMPARE(m_asked.size(), 1);
        QVERIFY(m_asked.first().contains(QStringLiteral("looks like a fake")));
        QVERIFY(m_asked.first().contains(t->model()->hitAt(r).suspect));
        t->downloadIndex(vi);
        QCOMPARE(m_asked.size(), 1);   // not again
        Session::instance()->setSuppressDangerousWarning(false);
        // flagging off: normal colour, no tooltip line, no warning
        st()->setValue(keys::search::FlagSuspects, false);
        QVERIFY(src.data(Qt::ForegroundRole).value<QColor>() != color(ckeys::SuspectText));
        QVERIFY(!src.data(Qt::ToolTipRole).toString().contains(QStringLiteral("Suspected fake")));
        st()->setValue(keys::search::FlagSuspects, true);
        // hiding: the rows leave the view at once
        const int before = t->proxy()->rowCount();
        st()->setValue(keys::search::HideSuspects, true);
        QCOMPARE(t->proxy()->rowCount(), before - 3);
        st()->setValue(keys::search::HideSuspects, false);
        QCOMPARE(t->proxy()->rowCount(), before);
    }

    void pingButtonsFillPingColumn()
    {
        SearchTab *t = runSearch(QStringLiteral("velorine"));
        QVERIFY(t->model()->hitCount() > 50);
        t->findChild<PushButton *>(QStringLiteral("searchPingAll"))->click();
        QTest::qWait(20);
        t->flushPending();
        int numbers = 0, unknown = 0;
        for (int r = 0; r < t->model()->hitCount(); ++r) {
            const SearchHit &h = t->model()->hitAt(r);
            for (const SearchSource &s : h.sources) {
                QVERIFY(s.extra.value(QStringLiteral("pinged")).toBool());
                (s.pingMs >= 0 ? numbers : unknown)++;
            }
        }
        QVERIFY(numbers > 20);
        QVERIFY(unknown > 0);
        // single-source rows show the ms (or "?") in the Ping column
        for (int r = 0; r < t->model()->hitCount(); ++r) {
            const SearchHit &h = t->model()->hitAt(r);
            if (h.sources.size() != 1)
                continue;
            const QString shown = t->model()->index(r, search::Ping).data().toString();
            QCOMPARE(shown, h.sources.first().pingMs >= 0 ? QString::number(h.sources.first().pingMs) : QStringLiteral("?"));
        }
    }

    void hashSearch_data()
    {
        QTest::addColumn<QString>("t1");
        QTest::addColumn<QString>("t2");
        QTest::addColumn<QString>("name");
        QTest::newRow("magnet") << QStringLiteral("magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Sintel.mkv&xl=1234")
                                << QString() << QStringLiteral("Sintel.mkv");
        QTest::newRow("ed2k") << QStringLiteral("ed2k://|file|Big_Buck_Bunny.ogv|4404019|0123456789ABCDEF0123456789ABCDEF|/")
                              << QString() << QStringLiteral("Big_Buck_Bunny.ogv");
        QTest::newRow("HASH> line") << QStringLiteral("Trailer") << QStringLiteral("HASH>0123456789abcdef0123456789abcdef -4404019")
                                    << QStringLiteral("Trailer");
        QTest::newRow("raw sha1") << QStringLiteral("urn:sha1:OXAM7KQMFB5QUXFE3EZP3GYJLYVB556T") << QString()
                                  << QStringLiteral("Hash OXAM7KQMFB5Q");
    }
    void hashSearch()
    {
        QFETCH(QString, t1);
        QFETCH(QString, t2);
        QFETCH(QString, name);
        SearchTab *t = runSearch(t1, t2);
        QVERIFY(t->isHashSearch());
        QCOMPARE(t->model()->hitCount(), 1);
        QCOMPARE(t->model()->hitAt(0).fileName, name);
        QCOMPARE(t->model()->hitAt(0).sources.size(), 3);   // one row, sources from three networks
        if (!t2.isEmpty() || !t1.startsWith(QLatin1String("urn:")))
            QCOMPARE(t->label(), QStringLiteral("\"%1 with HASH\"").arg(name));
    }
    void hashDetection()
    {
        QVERIFY(SearchTab::looksLikeHashOrLink(QStringLiteral("0123456789abcdef0123456789abcdef")));
        QVERIFY(SearchTab::looksLikeHashOrLink(QStringLiteral("btih:0123456789abcdef0123456789abcdef01234567")));
        QVERIFY(SearchTab::looksLikeHashOrLink(QStringLiteral("tth:ABCDEFGHIJKLMNOPQRSTUVWXYZ234567ABCDEFG")));
        QVERIFY(!SearchTab::looksLikeHashOrLink(QStringLiteral("big buck bunny")));
        QVERIFY(!SearchTab::looksLikeHashOrLink(QStringLiteral("abcdefghabcdefghabcdefghabcdefgh")));   // a word, no digit
        QCOMPARE(SearchTab::hashQueryFor(QStringLiteral("Name"), QStringLiteral("HASH>0123456789abcdef0123456789abcdef -77")),
                 QStringLiteral("ed2k://|file|Name|77|0123456789abcdef0123456789abcdef|/"));
        QCOMPARE(SearchTab::hashQueryFor(QStringLiteral("big buck bunny"), QString()), QString());
    }

    void manuallyResumeFromPartialFile()
    {
        SearchTab *t = runSearch(QStringLiteral("anything else"));
        int row = -1;
        for (int r = 0; r < t->model()->hitCount() && row < 0; ++r)
            if (t->model()->hitAt(r).fileName.endsWith(QLatin1String(".mp3")) && t->model()->hitAt(r).size > 2000000)
                row = r;
        QVERIFY(row >= 0);
        const SearchHit h = t->model()->hitAt(row);
        t->list()->selectionModel()->select(viewRowOf(t->list(), t->model()->index(row, 0)),
                                            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        int candidates = 0;
        SearchTab::s_resumeDialogHook = [&candidates](FileDownloadDialog *d) {
            candidates = d->candidateCount();
            d->choose(2, 0);   // the first (enabled) incomplete file
            d->accept();
            return d->result() == QDialog::Accepted;
        };
        const int before = int(m_app.core->transfers()->downloads().size());
        t->manuallyResumeSelected();
        SearchTab::s_resumeDialogHook = nullptr;
        QCOMPARE(candidates, 3);   // two stray incomplete files + one owned by a download (greyed)
        const QList<TransferInfo> dls = m_app.core->transfers()->downloads();
        QCOMPARE(int(dls.size()), before + 1);
        const TransferInfo n = dls.last();
        QCOMPARE(n.name, h.fileName);
        QCOMPARE(n.done, qint64(1048576));   // the incomplete file's data counts as received
        const QVariantMap resumed = st()->value(QStringLiteral("ui/resumedFiles")).toMap();
        QVERIFY(resumed.value(n.id).toString().endsWith(QStringLiteral("__INCOMPLETE___Velorine - Velvet Engine.mp3")));
        // the file is used in place: Delete File names it first, "No" keeps the download
        m_asked.clear();
        m_answer = QMessageBox::No;
        transfers()->showDownloadMenu(viewRowOf(transfers()->downloadList(),
                                                transfers()->downloadModel()->index(downloadRowOf(n.id), 0)),
                                      QPoint(10, 10));
        QTest::qWait(20);
        QAction *del = uib::menuAction(uib::openMenu(), QStringLiteral("Delete File"));
        QVERIFY(del);
        del->trigger();
        uib::closeMenus();
        QCOMPARE(m_asked.size(), 1);
        QVERIFY(m_asked.first().contains(QStringLiteral("__INCOMPLETE___Velorine - Velvet Engine.mp3")));
        QVERIFY(m_asked.first().contains(QStringLiteral("in place")));
        QVERIFY(downloadRowOf(n.id) >= 0);
        m_answer = QMessageBox::Yes;
    }

    // ------------------------------------------------------------------ transfers
    void findSourcesAndEnterQueue()
    {
        m_app.mw->showPage(QStringLiteral("transfers"));
        TransfersPage *p = transfers();
        MxListView *l = p->downloadList();
        auto selectFile = [&](const QString &id) {
            l->selectionModel()->select(viewRowOf(l, p->downloadModel()->index(downloadRowOf(id), 0)),
                                        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        };
        selectFile(QStringLiteral("d1"));
        const int before = int(download(QStringLiteral("d1")).sources.size());
        p->findChild<PushButton *>(QStringLiteral("dlFindSources"))->click();
        QCOMPARE(int(download(QStringLiteral("d1")).sources.size()), before + 1);
        // Enter Queue on an errored download revives it into the remote queue
        selectFile(QStringLiteral("d7"));
        QCOMPARE(download(QStringLiteral("d7")).state, TransferState::Error);
        p->showDownloadMenu(viewRowOf(l, p->downloadModel()->index(downloadRowOf(QStringLiteral("d7")), 0)), QPoint(5, 5));
        QTest::qWait(10);
        QAction *eq = uib::menuAction(uib::openMenu(), QStringLiteral("Enter Queue"));
        QVERIFY(eq);
        eq->trigger();
        uib::closeMenus();
        QCOMPARE(download(QStringLiteral("d7")).state, TransferState::RemotelyQueued);
    }

    void perSourceCancelAndPriority()
    {
        TransfersPage *p = transfers();
        MxListView *l = p->downloadList();
        TransferModel *m = p->downloadModel();
        const QModelIndex file = m->index(downloadRowOf(QStringLiteral("d2")), 0);
        QCOMPARE(m->rowCount(file), 4);
        l->expand(viewRowOf(l, file));
        const QModelIndex src2 = m->index(1, 0, file);   // Bobwib234 (eD2k)
        const QString sid = m->sourceFor(src2).id;
        QVERIFY(!sid.isEmpty());
        // priority: the user row's Bandwidth Priority menu -> setSourcePriority
        p->showDownloadMenu(viewRowOf(l, src2), QPoint(5, 5));
        QTest::qWait(10);
        QMenu *menu = uib::openMenu();
        QAction *bp = uib::menuAction(menu, QStringLiteral("Bandwidth Priority"));
        QVERIFY(bp && bp->isEnabled() && bp->menu());
        QAction *x2 = uib::menuAction(bp->menu(), QStringLiteral("x4"));
        QVERIFY(x2);
        x2->trigger();
        uib::closeMenus();
        int prio = 0;
        for (const TransferSource &s : download(QStringLiteral("d2")).sources)
            if (s.id == sid)
                prio = s.priority;
        QCOMPARE(prio, 1);
        // Cancel with a user row selected drops only that source
        l->selectionModel()->select(viewRowOf(l, m->index(1, 0, m->index(downloadRowOf(QStringLiteral("d2")), 0))),
                                    QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        p->findChild<PushButton *>(QStringLiteral("dlCancel"))->click();
        const TransferInfo d2 = download(QStringLiteral("d2"));
        QCOMPARE(d2.state, TransferState::Transferring);   // the download goes on
        QCOMPARE(int(d2.sources.size()), 3);
        for (const TransferSource &s : d2.sources)
            QVERIFY(s.id != sid);
        // a BitTorrent peer row cannot be addressed: its source actions are disabled
        const QModelIndex d3 = m->index(downloadRowOf(QStringLiteral("d3")), 0);
        p->showDownloadMenu(viewRowOf(l, m->index(0, 0, d3)), QPoint(5, 5));
        QTest::qWait(10);
        menu = uib::openMenu();
        QVERIFY(!uib::menuAction(menu, QStringLiteral("Cancel"))->isEnabled());
        QVERIFY(!uib::menuAction(menu, QStringLiteral("Bandwidth Priority"))->isEnabled());
        uib::closeMenus();
    }

    void uploadStartAndPriority()
    {
        TransfersPage *p = transfers();
        MxListView *l = p->uploadList();
        TransferModel *m = p->uploadModel();
        auto menuFor = [&](const QString &id) {
            p->showUploadMenu(viewRowOf(l, m->index(m->rowOfId(id), 0)), QPoint(5, 5));
            QTest::qWait(10);
            return uib::openMenu();
        };
        // a queued eD2k client: Start Transfer gives it a slot now
        QMenu *menu = menuFor(QStringLiteral("u6"));
        QAction *start = uib::menuAction(menu, QStringLiteral("Start Transfer"));
        QVERIFY(start && start->isEnabled());
        start->trigger();
        uib::closeMenus();
        QCOMPARE(m_app.core->transfers()->transfer(QStringLiteral("u6")).state, TransferState::Transferring);
        // priority
        menu = menuFor(QStringLiteral("u3"));
        QAction *bp = uib::menuAction(menu, QStringLiteral("Bandwidth Priority"));
        QVERIFY(bp->isEnabled());
        uib::menuAction(bp->menu(), QStringLiteral("30%"))->trigger();
        uib::closeMenus();
        QCOMPARE(m_app.core->transfers()->transfer(QStringLiteral("u3")).priority, -1);
        menu = menuFor(QStringLiteral("u3"));
        QVERIFY(uib::menuAction(uib::menuAction(menu, QStringLiteral("Bandwidth Priority"))->menu(), QStringLiteral("50%"))->isChecked());
        uib::closeMenus();
        // a Gnutella queue entry cannot be started early: a note in the status bar, nothing else
        menu = menuFor(QStringLiteral("u5"));
        uib::menuAction(menu, QStringLiteral("Start Transfer"))->trigger();
        uib::closeMenus();
        QCOMPARE(m_app.core->transfers()->transfer(QStringLiteral("u5")).state, TransferState::Queued);
        QVERIFY(p->statusFields().join(QLatin1Char(' ')).contains(QStringLiteral("cannot start this queued upload")));
        // a running upload is not "Start"-able
        menu = menuFor(QStringLiteral("u2"));
        QVERIFY(!uib::menuAction(menu, QStringLiteral("Start Transfer"))->isEnabled());
        uib::closeMenus();
    }

    void changeDestination()
    {
        TransfersPage *p = transfers();
        const QString dir = m_dir.filePath(QStringLiteral("elsewhere"));
        TransfersPage::s_dirHook = [dir](QWidget *, const QString &) { return dir; };
        p->showDownloadMenu(viewRowOf(p->downloadList(), p->downloadModel()->index(downloadRowOf(QStringLiteral("d4")), 0)),
                            QPoint(5, 5));
        QTest::qWait(10);
        uib::menuAction(uib::openMenu(), QStringLiteral("Change Destination..."))->trigger();
        uib::closeMenus();
        TransfersPage::s_dirHook = nullptr;
        QCOMPARE(QFileInfo(download(QStringLiteral("d4")).savePath).absolutePath(), QFileInfo(dir).absoluteFilePath());
    }

    void loadIncompleteSearchesAndResumes()
    {
        // a real incomplete file in the profile's incomplete folder
        const QString inc = m_app.core->transfers()->incompleteDir();
        QDir().mkpath(inc);
        const QString file = QDir(inc).filePath(QStringLiteral("__INCOMPLETE___Velorine - Is It A Rumour.mp3"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray(300000, 'x'));
        }
        int rows = 0;
        TransfersPage::s_loadIncompleteHook = [&rows, file](LoadIncompleteDialog *d) {
            rows = d->rowCount();
            for (int i = 0; i < d->rowCount(); ++i) {
                d->selectRow(i);
                if (d->chosenFile() == file)
                    break;
            }
            return true;
        };
        transfers()->findChild<PushButton *>(QStringLiteral("dlLoadIncomplete"))->click();
        TransfersPage::s_loadIncompleteHook = nullptr;
        QCOMPARE(rows, 2);   // the real file + the demo's stray file (Velvet Engine / Best Of are in use)
        // no link given: a search for the name, in "resume" mode
        QCOMPARE(m_app.mw->currentPageId(), QStringLiteral("search"));
        SearchTab *t = search()->currentTab();
        QCOMPARE(t->resumeFile(), file);
        QVERIFY(t->statusFields().join(QLatin1Char(' ')).contains(QStringLiteral("to resume")));
        QTest::qWait(20);
        t->flushPending();
        int row = -1;
        for (int r = 0; r < t->model()->hitCount() && row < 0; ++r)
            if (t->model()->hitAt(r).size > 300000 && t->model()->hitAt(r).suspect.isEmpty())
                row = r;
        QVERIFY(row >= 0);
        const int before = int(m_app.core->transfers()->downloads().size());
        t->downloadIndex(viewRowOf(t->list(), t->model()->index(row, 0)));
        QCOMPARE(int(m_app.core->transfers()->downloads().size()), before + 1);
        QCOMPARE(m_app.core->transfers()->downloads().last().done, qint64(300000));
        QVERIFY(t->resumeFile().isEmpty());
        // deleting its data deletes the file (after the warning)
        const QString id = m_app.core->transfers()->downloads().last().id;
        m_answer = QMessageBox::Yes;
        m_asked.clear();
        m_app.mw->showPage(QStringLiteral("transfers"));
        transfers()->showDownloadMenu(viewRowOf(transfers()->downloadList(),
                                                transfers()->downloadModel()->index(downloadRowOf(id), 0)),
                                      QPoint(5, 5));
        QTest::qWait(10);
        uib::menuAction(uib::openMenu(), QStringLiteral("Delete File"))->trigger();
        uib::closeMenus();
        QCOMPARE(m_asked.size(), 1);
        QVERIFY(m_asked.first().contains(QDir::toNativeSeparators(file)));
        QVERIFY(!QFileInfo::exists(file));
        QVERIFY(!st()->value(QStringLiteral("ui/resumedFiles")).toMap().contains(id));
    }

    // ------------------------------------------------------------------ networks / title
    void titleAndStatusFromSummary()
    {
        st()->setValue(keys::identity::PrimaryConnection, true);
        QVERIFY(m_app.mw->titleText().contains(QStringLiteral("<Online P=5 S=12 spm=4284>")));
        QVERIFY(m_app.mw->titleText().contains(QStringLiteral("nets online")));
        m_app.mw->showPage(QStringLiteral("networks"));
        const QStringList f = networks()->statusFields();
        QCOMPARE(f.value(1), QStringLiteral("5 Primary Connections"));
        QCOMPARE(f.value(2), QStringLiteral("12 Incoming Secondary Connections"));
        QCOMPARE(f.value(3), QStringLiteral("4284 / 4284 SPPM"));
        st()->setValue(keys::identity::PrimaryConnection, false);
        QVERIFY(m_app.mw->titleText().contains(QStringLiteral("<Online 0:")));
    }

    void editAndImportServers()
    {
        NetworksPage *p = networks();
        p->selectSubTab(QStringLiteral("other"));
        NetworkService *net = m_app.core->network();
        QVERIFY(net->editServer(Net::ED2K, QStringLiteral("203.0.113.5:4232"), QStringLiteral("203.0.113.6:4242"), QStringLiteral("Renamed")));
        bool found = false;
        for (const ServerEntry &e : net->servers(Net::ED2K))
            found = found || (e.address == QStringLiteral("203.0.113.6:4242") && e.name == QStringLiteral("Renamed"));
        QVERIFY(found);
        // text list
        const QString txt = m_dir.filePath(QStringLiteral("servers.txt"));
        {
            QFile f(txt);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("# my list\n192.0.2.200:4661 Test One\ned2k://|server|192.0.2.201|4242|/\n");
        }
        // binary server.met with two servers (one named)
        const QString met = m_dir.filePath(QStringLiteral("server.met"));
        {
            QByteArray d;
            d.append(char(0x0E));
            auto u32 = [&d](quint32 v) { for (int i = 0; i < 4; ++i) d.append(char((v >> (8 * i)) & 0xff)); };
            auto u16 = [&d](quint16 v) { d.append(char(v & 0xff)); d.append(char(v >> 8)); };
            u32(2);
            u32(0x0A0200C6);   // 198.0.2.10 little endian bytes c6 00 02 0a
            u16(4661);
            u32(1);
            d.append(char(0x02));      // string tag, old style name
            u16(1);
            d.append(char(0x01));      // ST_SERVERNAME
            u16(8);
            d.append("Met Test");
            u32(0x0B0200C6);
            u16(4662);
            u32(0);
            QFile f(met);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(d);
        }
        const QString junk = m_dir.filePath(QStringLiteral("junk.bin"));
        {
            QFile f(junk);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("this is not a list");
        }
        QString next;
        NetworksPage::s_importHook = [&next](QWidget *) { return next; };
        next = txt;
        p->importList();
        QCOMPARE(p->lastImportMessage(), QStringLiteral("2 servers imported."));
        next = met;
        p->importList();
        QCOMPARE(p->lastImportMessage(), QStringLiteral("2 servers imported."));
        next = junk;
        p->importList();
        QVERIFY(p->lastImportMessage().contains(QStringLiteral("not a server list")));
        NetworksPage::s_importHook = nullptr;
        QStringList names;
        for (const ServerEntry &e : net->servers(Net::ED2K))
            names << e.name + QLatin1Char('@') + e.address;
        QVERIFY(names.contains(QStringLiteral("Met Test@198.0.2.10:4661")));
        QVERIFY(names.contains(QStringLiteral("Test One@192.0.2.200:4661")));
    }

    void serverMessagesWindow()
    {
        NetworksPage *p = networks();
        p->viewServerMessages(Net::ED2K, QStringLiteral("45.82.80.155:5687"));
        QTest::qWait(10);
        auto *w = m_app.mw->findChild<ServerMessagesWindow *>();
        QVERIFY(w && w->isVisible());
        QCOMPARE(w->lineCount(), 4);
        QVERIFY(w->plainText().contains(QStringLiteral("Welcome to eMule Security No1!")));
        QVERIFY(w->windowTitle().contains(QStringLiteral("eMule Security No1")));
        // live messages of this server are appended, others are not
        NetworkService *net = m_app.core->network();
        emit net->serverMessageReceived(ServerMessage{Net::ED2K, QStringLiteral("45.82.80.155:5687"), QStringLiteral("eMule Security No1"),
                                                      QStringLiteral("Live line"), QDateTime::currentDateTimeUtc()});
        emit net->serverMessageReceived(ServerMessage{Net::ED2K, QStringLiteral("other:1"), QStringLiteral("Other"),
                                                      QStringLiteral("Not mine"), QDateTime::currentDateTimeUtc()});
        QCOMPARE(w->lineCount(), 5);
        QVERIFY(w->plainText().contains(QStringLiteral("Live line")));
        QVERIFY(!w->plainText().contains(QStringLiteral("Not mine")));
        // one window per server
        p->viewServerMessages(Net::ED2K, QStringLiteral("45.82.80.155:5687"));
        QCOMPARE(m_app.mw->findChildren<ServerMessagesWindow *>().size(), 1);
        // the context menu entry opens it too
        w->close();
        QTest::qWait(10);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(st()->value(keys::ui::dialogKey(QStringLiteral("serverMessages"))).isValid());
    }

    void pingUserShowsAnswer()
    {
        m_app.mw->pingUser(uib::user(QStringLiteral("Quill233_21212")));
        QTRY_VERIFY(m_app.mw->notice().startsWith(QStringLiteral("Ping reply from Quill233_21212: ")));
        m_app.mw->pingUser(uib::user(QStringLiteral("DHT"), Net::BitTorrent));
        QTRY_COMPARE(m_app.mw->notice(), QStringLiteral("No ping reply from DHT"));
    }

    // ------------------------------------------------------------------ bandwidth
    void bandwidthUsesDetailedSamples()
    {
        BandwidthSample s;
        s.downBps = 100000;
        s.payloadDownBps = 60000;
        s.netDownBps.insert(Net::Kad, 3000);
        s.netDownBps.insert(Net::Overlay, 5000);
        s.netDownBps.insert(Net::ED2K, 2000);
        QVERIFY(bw::isDetailed(s));
        const bw::Point p = bw::split(m_app.core, s, true);
        QCOMPARE(p.total(), 100000);
        QCOMPARE(p.v[bw::Downloads], 60000);
        QVERIFY(p.v[bw::Packets] > 0 && p.v[bw::Packets] <= 3000);
        QVERIFY(p.v[bw::Overlay] > 0 && p.v[bw::Overlay] <= 5000);
        QVERIFY(p.v[bw::Other] >= 30000);   // traffic of no reported network
        // the page drew the history backlog on open, split from the samples' own numbers
        auto *page = qobject_cast<BandwidthPage *>(m_app.mw->page(QStringLiteral("bandwidth")));
        QVERIFY(page->incomingPoints().size() >= 1200);
        const QList<BandwidthSample> hist = m_app.core->bandwidth()->history(3600);
        QVERIFY(bw::isDetailed(hist.first()));
        int withOverlay = 0;
        for (const bw::Point &pt : page->outgoingPoints())
            withOverlay += pt.v[bw::Overlay] > 0 ? 1 : 0;
        QVERIFY(withOverlay > 1000);
    }

    // ------------------------------------------------------------------ shared / chat / whois
    void sharedFileTypesAndExcludedFolder()
    {
        LibraryService *lib = m_app.core->library();
        const int all = lib->fileCount();
        auto *page = qobject_cast<SharedPage *>(m_app.mw->page(QStringLiteral("shared")));
        SharedFileTypesDialog dlg(lib, st(), page);
        dlg.findChild<QCheckBox *>(QStringLiteral("type_audio"))->setChecked(false);   // [ui-f] the table's groups
        auto *unlisted = dlg.findChild<QCheckBox *>(QStringLiteral("shareUnlistedTypes"));
        QVERIFY(unlisted && unlisted->isChecked());
        unlisted->setChecked(false);
        dlg.accept();
        QVERIFY(!st()->b(keys::library::ShareUnlistedTypes));
        st()->setValue(keys::library::ShareUnlistedTypes, true);
        QVERIFY(!lib->sharedTypes().contains(QStringLiteral("audio")));
        QVERIFY(lib->fileCount() < all);
        lib->setSharedTypes(sharedTypeIds());
        QCOMPARE(lib->fileCount(), all);
        // un-sharing a sub folder excludes the folder (not file by file)
        QString sub;
        for (const SharedFile &f : lib->files()) {
            const QString dir = QFileInfo(QDir::fromNativeSeparators(f.path)).absolutePath();
            bool root = false;
            for (const SharedFolder &r : lib->folders())
                root = root || QDir::cleanPath(QDir::fromNativeSeparators(r.path)) == dir;
            if (!root) {
                sub = dir;
                break;
            }
        }
        QVERIFY(!sub.isEmpty());
        QString err;
        QVERIFY(page->scriptCommand(QStringLiteral("unshare-folder"), {sub}, &err));
        QVERIFY(lib->excludedFolders().contains(QDir::cleanPath(sub)));
        QVERIFY(lib->fileCount() < all);
        lib->excludeFolder(sub, false);
        QCOMPARE(lib->fileCount(), all);
    }

    void listChannelsCoversChatNetworks()
    {
        m_app.mw->showPage(QStringLiteral("chat"));
        auto *chat = qobject_cast<ChatPage *>(m_app.mw->page(QStringLiteral("chat")));
        QTest::qWait(20);
        for (QDialog *d : m_app.mw->findChildren<QDialog *>())
            if (d->objectName() == QLatin1String("chatWarningDialog"))
                d->accept();
        chat->showListChannelsMenu();
        QTest::qWait(10);
        QMenu *menu = uib::openMenu();
        QVERIFY(menu);
        QCOMPARE(menu->actions().size(), 2);
        QCOMPARE(menu->actions().at(0)->text(), QStringLiteral("OctoMX Peer Network"));
        QCOMPARE(menu->actions().at(1)->text(), QStringLiteral("Gnutella"));
        menu->actions().at(1)->trigger();
        uib::closeMenus();
        QVERIFY(chat->channelList());
        QCOMPARE(chat->channelList()->network(), Net::Gnutella);
        QTRY_COMPARE(chat->channelList()->model()->roomCount(), 3);
        QCOMPARE(chat->tabs()->tabText(0), QStringLiteral("Gnutella"));
        chat->showChannelListFor(Net::Overlay);
        QVERIFY(chat->channelList()->model()->roomCount() > 100);
    }

    void whoisByRequestId()
    {
        m_app.mw->openWhois(uib::user(QStringLiteral("zqvtra662_44120")));
        auto *w = m_app.mw->findChild<WhoisWindow *>();
        QVERIFY(w);
        QTRY_VERIFY(w->hasResult());
        w->close();
    }

    // ------------------------------------------------------------------ persistence of the sub-tab
    void networksSubTabPersists()
    {
        networks()->selectSubTab(QStringLiteral("other"));
        QCOMPARE(st()->value(QStringLiteral("ui/networksSubTab")).toInt(), 1);
    }
};

int main(int argc, char **argv)
{
    preApplicationSetup();
    QApplication app(argc, argv);
    installClassicLook(&app);
    TstUidWiring t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_uid_wiring.moc"
