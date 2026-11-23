// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-h] behaviour tests for the settings that were dead before 4.5 and now work: message alert
// sounds (never played: the sound hook records them), the chat warning's "do not show again",
// expanded search groups and new downloads, switching to Transfers, the double-click action, the File
// Launching entries and Launch Options programs, always on top, tray notifications and taskbar flashing,
// the remembered Settings page, grid lines and the list colours, the chat user list (colours, Speed
// column at every scale), the reported connection type while busy, the Library page.
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QStandardItemModel>
#include <QtTest/QSignalSpy>
#include <QtTest/QtTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTreeWidget>

#include "mx/diagnostics.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/chat_warning_dialog.h"
#include "ui/dialogs/connection_test.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/share_dialogs.h"
#include "ui/dialogs/user_windows.h"
#include "ui/kit/colors.h"
#include "ui/kit/icons.h"
#include "ui/kit/launcher.h"
#include "ui/kit/list_view.h"
#include "ui/kit/scale.h"
#include "ui/kit/session.h"
#include "ui/kit/sound.h"
#include "ui/main_window.h"
#include "ui/models/chat_models.h"
#include "ui/models/search_model.h"
#include "ui/pages/chat_page.h"
#include "ui/pages/chat_view.h"
#include "ui/pages/search_page.h"
#include "ui/pages/transfers_page.h"
#include "uib_helpers.h"

using namespace mx;
using namespace mx::ui;

class TstUih : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    uib::App m_app;
    QStringList m_sounds;

    Settings *st() const { return m_app.core->settings(); }
    TransfersPage *transfers() const { return qobject_cast<TransfersPage *>(m_app.mw->page(QStringLiteral("transfers"))); }
    MxListView *downloadList() const { return transfers()->findChild<MxListView *>(QStringLiteral("downloadList")); }
    QModelIndex downloadRow(const QString &name) const
    {
        MxListView *list = downloadList();
        for (int i = 0; i < list->visibleRowCount(); ++i) {
            const QModelIndex r = list->visibleRow(i);
            if (r.data().toString().startsWith(name))
                return r;
        }
        return {};
    }
    QString file(const QString &name, const QByteArray &data = "x")
    {
        const QString p = m_dir.filePath(name);
        QFile f(p);
        if (f.open(QIODevice::WriteOnly))
            f.write(data);
        return p;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_app.start(m_dir.filePath(QStringLiteral("profile")));
        sound::setHook([this](const QString &what) { m_sounds << what; });
    }
    void cleanupTestCase()
    {
        sound::setHook({});
        launcher::setHook({});
        m_app.stop();
    }

    // ------------------------------------------------------------------ alert sounds
    void soundChoices()
    {
        // what each event plays, as Settings say (messages/alertMode, alertWav, soundOnMessage,
        // hotlist/soundOnline, misc/soundDownloadComplete, misc/sounds)
        QVERIFY(sound::suppressed());   // offscreen + MX_DEMO_FROZEN: nothing can reach the speakers
        st()->setValue(keys::messages::SoundOnMessage, true);
        st()->setValue(keys::messages::AlertMode, QStringLiteral("quiet"));
        QCOMPARE(sound::choiceFor(st(), sound::Event::Message), QString());
        st()->setValue(keys::messages::AlertMode, QStringLiteral("beep"));
        QCOMPARE(sound::choiceFor(st(), sound::Event::Message), QStringLiteral("beep"));
        st()->setValue(keys::messages::AlertMode, QStringLiteral("sound"));
        QCOMPARE(sound::choiceFor(st(), sound::Event::Message), QStringLiteral("sound"));
        const QString wav = file(QStringLiteral("alert.wav"), sound::builtinWav());
        st()->setValue(keys::messages::AlertWav, wav);
        st()->setValue(keys::messages::AlertMode, QStringLiteral("wav"));
        QCOMPARE(sound::choiceFor(st(), sound::Event::Message), wav);
        st()->setValue(keys::messages::AlertWav, m_dir.filePath(QStringLiteral("gone.wav")));
        QCOMPARE(sound::choiceFor(st(), sound::Event::Message), QStringLiteral("sound"));   // a lost file: the chime
        st()->setValue(keys::messages::SoundOnMessage, false);
        QCOMPARE(sound::choiceFor(st(), sound::Event::Message), QString());
        st()->setValue(keys::messages::SoundOnMessage, true);
        // Hotlist: its own switch, then the message alert
        st()->setValue(keys::messages::AlertMode, QStringLiteral("beep"));
        st()->setValue(keys::hotlist::SoundOnline, false);
        QCOMPARE(sound::choiceFor(st(), sound::Event::HotlistOnline), QString());
        st()->setValue(keys::hotlist::SoundOnline, true);
        QCOMPARE(sound::choiceFor(st(), sound::Event::HotlistOnline), QStringLiteral("beep"));
        // download complete: the chime
        st()->setValue(keys::misc::SoundDownloadComplete, true);
        QCOMPARE(sound::choiceFor(st(), sound::Event::DownloadDone), QStringLiteral("sound"));
        st()->setValue(keys::misc::SoundDownloadComplete, false);
        QCOMPARE(sound::choiceFor(st(), sound::Event::DownloadDone), QString());
        // "Play sounds" off: nothing at all
        st()->setValue(keys::misc::SoundDownloadComplete, true);
        st()->setValue(keys::misc::Sounds, false);
        for (sound::Event e : {sound::Event::Message, sound::Event::HotlistOnline, sound::Event::DownloadDone})
            QCOMPARE(sound::choiceFor(st(), e), QString());
        st()->setValue(keys::misc::Sounds, true);
        // the chime is a valid 16-bit mono PCM WAVE file of about 0.2 s
        const QByteArray w = sound::builtinWav();
        QVERIFY(w.startsWith("RIFF") && w.mid(8, 8) == "WAVEfmt ");
        QVERIFY(w.size() > 7000 && w.size() < 12000);
        st()->resetToDefaults(QStringLiteral("messages/"));
        st()->resetToDefaults(QStringLiteral("hotlist/"));
    }

    void privateMessageAlert()
    {
        // an incoming private message plays the Incoming Message Alert (was: a beep whatever the mode)
        st()->setValue(keys::messages::AlertMode, QStringLiteral("sound"));
        m_sounds.clear();
        const UserRef u = uib::user(QStringLiteral("alertbuddy_44444"));
        m_app.mw->openPrivateMessage(u);
        auto *w = m_app.mw->findChild<PrivateMessageWindow *>();
        QVERIFY(w);
        w->submit(QStringLiteral("hello"));   // the demo user answers "hi!"
        QTRY_VERIFY(m_sounds.contains(QStringLiteral("sound")));
        // Quiet: nothing
        st()->setValue(keys::messages::AlertMode, QStringLiteral("quiet"));
        m_sounds.clear();
        w->submit(QStringLiteral("again"));
        QTRY_VERIFY(w->output()->plainText().count(QStringLiteral("hi!")) >= 2);
        QVERIFY(m_sounds.isEmpty());
        w->close();
        // the Test button on the Settings page plays the chosen alert
        st()->setValue(keys::messages::AlertMode, QStringLiteral("beep"));
        SettingsDialog d(m_app.core, m_app.mw);
        QVERIFY(d.showPage(QStringLiteral("messages")));
        m_sounds.clear();
        d.findChild<QAbstractButton *>(QStringLiteral("set_messages_alertTest"))->click();
        QCOMPARE(m_sounds, QStringList({QStringLiteral("beep")}));
        d.findChild<QAbstractButton *>(QStringLiteral("set_messages_alertMode_sound"))->click();
        QCOMPARE(st()->s(keys::messages::AlertMode), QStringLiteral("sound"));
        st()->resetToDefaults(QStringLiteral("messages/"));
    }

    void downloadAndHotlistSounds()
    {
        QStringList told;
        MainWindow::s_notifyHook = [&told](const QString &t) { told << t; };
        TransferInfo t;
        t.id = QStringLiteral("uih-done");
        t.name = QStringLiteral("done.ogg");
        m_sounds.clear();
        st()->setValue(keys::misc::SoundDownloadComplete, true);
        m_app.mw->notifyDownloadDone(t);
        QCOMPARE(m_sounds, QStringList({QStringLiteral("sound")}));
        st()->setValue(keys::misc::SoundDownloadComplete, false);
        m_sounds.clear();
        m_app.mw->notifyDownloadDone(t);
        QVERIFY(m_sounds.isEmpty());
        // Hotlist user online: the message alert when hotlist/soundOnline is on
        st()->setValue(keys::messages::AlertMode, QStringLiteral("beep"));
        st()->setValue(keys::hotlist::SoundOnline, true);
        m_app.mw->notifyHotlistOnline(QStringLiteral("friend_55555"));
        QCOMPARE(m_sounds, QStringList({QStringLiteral("beep")}));
        st()->setValue(keys::hotlist::SoundOnline, false);
        m_sounds.clear();
        m_app.mw->notifyHotlistOnline(QStringLiteral("friend_55555"));
        QVERIFY(m_sounds.isEmpty());
        MainWindow::s_notifyHook = {};
        st()->resetToDefaults(QStringLiteral("messages/"));
        st()->resetToDefaults(QStringLiteral("hotlist/"));
        st()->setValue(keys::misc::SoundDownloadComplete, true);
    }

    void trayBalloonsAndFlash()
    {
        QStringList told;
        int flashed = 0;
        MainWindow::s_notifyHook = [&told](const QString &t) { told << t; };
        MainWindow::s_flashHook = [&flashed] { ++flashed; };
        TransferInfo t;
        t.id = QStringLiteral("uih-tray");
        t.name = QStringLiteral("tray.ogg");
        st()->setValue(keys::misc::TrayBalloons, true);
        st()->setValue(keys::misc::FlashTaskbar, true);
        m_app.mw->notifyDownloadDone(t);
        QCOMPARE(told.size(), 1);
        const bool active = m_app.mw->isActiveWindow();
        QCOMPARE(flashed, active ? 0 : 1);   // only while OctoMX is in the background
        // "Show notifications in the system tray" off: no balloon, the status bar still says it
        st()->setValue(keys::misc::TrayBalloons, false);
        m_app.mw->notifyDownloadDone(t);
        m_app.mw->notifyHotlistOnline(QStringLiteral("friend_66666"));
        QCOMPARE(told.size(), 1);
        QVERIFY(m_app.mw->notice().contains(QStringLiteral("friend_66666")));
        // "Flash the taskbar button on events" off
        st()->setValue(keys::misc::FlashTaskbar, false);
        flashed = 0;
        m_app.mw->notifyDownloadDone(t);
        QCOMPARE(flashed, 0);
        MainWindow::s_notifyHook = {};
        MainWindow::s_flashHook = {};
        st()->setValue(keys::misc::TrayBalloons, true);
        st()->setValue(keys::misc::FlashTaskbar, true);
    }

    // ------------------------------------------------------------------ chat warning
    void chatWarningDoNotShowAgain()
    {
        QVERIFY(!st()->b(keys::ui::ChatWarningDisabled));
        m_app.mw->showPage(QStringLiteral("chat"));
        ChatWarningDialog *dlg = nullptr;
        QTRY_VERIFY((dlg = m_app.mw->findChild<ChatWarningDialog *>()) != nullptr);
        QVERIFY(dlg->doNotShowBox() && !dlg->doNotShowBox()->isChecked());
        QVERIFY(!dlg->doNotShowBox()->toolTip().isEmpty());
        dlg->doNotShowBox()->setChecked(true);
        dlg->findChild<QAbstractButton *>(QStringLiteral("chatWarningContinue"))->click();
        QTRY_VERIFY(st()->b(keys::ui::ChatWarningDisabled));   // ChatPage::maybeWarn reads it at every start
        m_app.mw->showPage(QStringLiteral("networks"));
        // Continue without the box ticked stores nothing
        st()->setValue(keys::ui::ChatWarningDisabled, false);
        ChatWarningDialog plain(m_app.mw);
        QVERIFY(!plain.doNotShowAgain());
        st()->setValue(keys::ui::ChatWarningDisabled, true);
    }

    // ------------------------------------------------------------------ search / transfers
    void groupsExpanded()
    {
        auto *page = qobject_cast<SearchPage *>(m_app.mw->page(QStringLiteral("search")));
        auto run = [&]() {
            m_app.mw->showPage(QStringLiteral("search"));
            SearchTab *t = page->currentTab();
            t->setTexts(QStringLiteral("debian"), QString());
            t->startSearch();
            QTest::qWait(30);
            t->flushPending();
            QTest::qWait(30);
            return t;
        };
        auto groups = [](SearchTab *t, int *expanded) {
            int n = 0;
            *expanded = 0;
            for (int r = 0; r < t->proxy()->rowCount(); ++r) {
                const QModelIndex vi = t->proxy()->index(r, 0);
                const QModelIndex src = t->list()->mapToSource(vi);
                if (t->model()->hitAt(src.row()).isBundle || t->proxy()->rowCount(vi) < 2)
                    continue;
                ++n;
                if (t->list()->isExpanded(vi))
                    ++*expanded;
            }
            return n;
        };
        st()->setValue(keys::search::GroupsExpanded, true);
        SearchTab *t = run();
        int expanded = 0;
        const int n = groups(t, &expanded);
        QVERIFY2(n > 0, "the demo search has results with several sources");
        QCOMPARE(expanded, n);
        st()->setValue(keys::search::GroupsExpanded, false);
        t = run();
        QCOMPARE(groups(t, &expanded), n);
        QCOMPARE(expanded, 0);
    }

    // the sources of a download are folded away by default; a profile from the time "expanded" was the
    // default loses the stored value once, a later choice of the user is kept
    void sourcesCollapsedByDefault()
    {
        const QString marker = QStringLiteral("ui/settingsMigrated");
        QCOMPARE(st()->defaultValue(keys::transfers::NewDownloadsExpanded).toBool(), false);
        const QVariant markerBefore = st()->value(marker);
        st()->setValue(marker, 1);
        st()->setValue(keys::transfers::NewDownloadsExpanded, true);
        migrateLegacySettings(st());
        QCOMPARE(st()->b(keys::transfers::NewDownloadsExpanded), false);
        QCOMPARE(st()->value(marker).toInt(), 2);
        st()->setValue(keys::transfers::NewDownloadsExpanded, true);   // the user ticks the box again
        migrateLegacySettings(st());
        QCOMPARE(st()->b(keys::transfers::NewDownloadsExpanded), true);
        st()->setValue(keys::transfers::NewDownloadsExpanded, QVariant());
        QCOMPARE(st()->b(keys::transfers::NewDownloadsExpanded), false);
        // with the default nothing in the download list is expanded, new downloads included
        QVERIFY(!m_app.core->transfers()->downloadUri(QStringLiteral("magnet:?xt=urn:btih:%1&dn=uih-default.iso").arg(QString(40, QLatin1Char('c')))).isEmpty());
        QModelIndex r;
        QTRY_VERIFY((r = downloadRow(QStringLiteral("uih-default"))).isValid());
        QTest::qWait(300);
        auto *list = downloadList();
        for (int i = 0; i < list->model()->rowCount(); ++i)
            QVERIFY2(!list->isExpanded(list->model()->index(i, 0)), qPrintable(list->model()->index(i, 0).data().toString()));
        Q_UNUSED(markerBefore);
    }

    // Networks, Other Protocol Networks: the servers and peers of every network are folded away at start
    void networkRowsCollapsedByDefault()
    {
        auto *list = m_app.mw->findChild<MxListView *>(QStringLiteral("serverList"));
        QVERIFY(list);
        QVERIFY(list->model()->rowCount() > 0);
        int withChildren = 0;
        for (int i = 0; i < list->model()->rowCount(); ++i) {
            const QModelIndex idx = list->model()->index(i, 0);
            withChildren += list->model()->rowCount(idx) > 0 ? 1 : 0;
            QVERIFY2(!list->isExpanded(idx), qPrintable(idx.data().toString()));
        }
        QVERIFY(withChildren > 0);   // the demo networks have servers / peers to hide
        // a row the user opens stays open
        const QModelIndex first = list->model()->index(0, 0);
        list->expand(first);
        QTest::qWait(2500);          // the demo refreshes its networks meanwhile
        QVERIFY(list->isExpanded(list->model()->index(0, 0)));
        list->collapse(list->model()->index(0, 0));
    }

    void newDownloadsExpandedAndSwitch()
    {
        m_app.mw->showPage(QStringLiteral("networks"));
        QTest::qWait(2100);   // downloads restored at start never switch pages (2 s grace)
        st()->setValue(keys::transfers::SwitchToTransfers, true);
        st()->setValue(keys::transfers::NewDownloadsExpanded, false);
        // a download started like Search / Browse do (Add Link and pasted links always show Transfers)
        QVERIFY(!m_app.core->transfers()->downloadUri(QStringLiteral("magnet:?xt=urn:btih:%1&dn=uih-collapsed.iso").arg(QString(40, QLatin1Char('a')))).isEmpty());
        QTRY_COMPARE(m_app.mw->currentPageId(), QStringLiteral("transfers"));
        QModelIndex r;
        QTRY_VERIFY((r = downloadRow(QStringLiteral("uih-collapsed"))).isValid());
        QVERIFY(!downloadList()->isExpanded(r));
        // expanded when the box is ticked (off by default)
        st()->setValue(keys::transfers::NewDownloadsExpanded, true);
        m_app.core->transfers()->downloadUri(QStringLiteral("magnet:?xt=urn:btih:%1&dn=uih-expanded.iso").arg(QString(40, QLatin1Char('b'))));
        QTRY_VERIFY((r = downloadRow(QStringLiteral("uih-expanded"))).isValid());
        QVERIFY(downloadList()->isExpanded(r) || downloadList()->model()->rowCount(r) == 0);
        // switching off: the page stays where it is
        st()->setValue(keys::transfers::SwitchToTransfers, false);
        m_app.mw->showPage(QStringLiteral("networks"));
        m_app.core->transfers()->downloadUri(QStringLiteral("magnet:?xt=urn:btih:%1&dn=uih-noswitch.iso").arg(QString(40, QLatin1Char('c'))));
        QTest::qWait(100);
        QCOMPARE(m_app.mw->currentPageId(), QStringLiteral("networks"));
        st()->resetToDefaults(QStringLiteral("transfers/"));
    }

    void doubleClickAction()
    {
        QStringList calls;
        launcher::setHook([&calls](const QString &action, const QString &path) {
            calls << action + QLatin1Char(' ') + QFileInfo(path).fileName();
            return true;
        });
        m_app.mw->showPage(QStringLiteral("transfers"));
        const QModelIndex done = downloadRow(QStringLiteral("Big Buck Bunny 1080p.mkv"));
        QVERIFY(done.isValid());
        st()->setValue(keys::launch::DoubleClickAction, QStringLiteral("open"));
        emit downloadList()->rowActivated(done);
        st()->setValue(keys::launch::DoubleClickAction, QStringLiteral("openFolder"));
        emit downloadList()->rowActivated(done);
        st()->setValue(keys::launch::DoubleClickAction, QStringLiteral("nothing"));
        emit downloadList()->rowActivated(done);
        QCOMPARE(calls, QStringList({QStringLiteral("open Big Buck Bunny 1080p.mkv"), QStringLiteral("show Big Buck Bunny 1080p.mkv")}));
        launcher::setHook({});
        st()->resetToDefaults(QStringLiteral("launch/"));
    }

    void launchEntriesAndCommands()
    {
        // Settings > Launch Options: a program per type when the system default is off
        const QString song = file(QStringLiteral("tune.mp3"));
        QStringList calls;
        launcher::setHook([&calls](const QString &action, const QString &path) {
            calls << action + QLatin1Char(' ') + path;
            return true;
        });
        QVERIFY(launcher::userCommand(st(), song).isEmpty());   // default: the system's program
        st()->setValue(keys::launch::UseOsDefault, false);
        st()->setValue(keys::launch::AudioCommand, QStringLiteral("myplayer --enqueue %1"));
        QCOMPARE(launcher::userCommand(st(), song), QStringList({QStringLiteral("myplayer"), QStringLiteral("--enqueue"), QDir::toNativeSeparators(song)}));
        QVERIFY(launcher::openFile(nullptr, st(), song));
        QCOMPARE(calls, QStringList({QStringLiteral("command myplayer --enqueue ") + QDir::toNativeSeparators(song)}));
        st()->setValue(keys::launch::AudioCommand, QStringLiteral("otherplayer"));   // no %1: the file goes last
        QCOMPARE(launcher::userCommand(st(), song).last(), QDir::toNativeSeparators(song));
        // the File Launching entries: an entry with a program is offered for its extensions
        QVariantList entries;
        entries << QVariantMap{{QStringLiteral("title"), QStringLiteral("Enqueue In Player")}, {QStringLiteral("ext"), QStringLiteral("mp3 ogg")},
                               {QStringLiteral("action"), QStringLiteral("enqueue")}, {QStringLiteral("command"), QStringLiteral("myplayer")},
                               {QStringLiteral("params"), QStringLiteral("--add \"%path%\"")}, {QStringLiteral("workdir"), QString()}}
                << QVariantMap{{QStringLiteral("title"), QStringLiteral("Open")}, {QStringLiteral("ext"), QStringLiteral("*")},
                               {QStringLiteral("action"), QStringLiteral("open")}, {QStringLiteral("command"), QString()},
                               {QStringLiteral("params"), QStringLiteral("%path%")}, {QStringLiteral("workdir"), QString()}};
        st()->setValue(keys::launch::Entries, QVariantMap{{QStringLiteral("entries"), entries}});
        const QList<QVariantMap> forSong = launcher::menuEntries(st(), song);
        QCOMPARE(forSong.size(), 1);   // "Open" without a program is Open File: never twice
        QCOMPARE(forSong.first().value(QStringLiteral("title")).toString(), QStringLiteral("Enqueue In Player"));
        QVERIFY(launcher::menuEntries(st(), m_dir.filePath(QStringLiteral("movie.mkv"))).isEmpty());
        QCOMPARE(launcher::entryCommand(forSong.first(), song),
                 QStringList({QStringLiteral("myplayer"), QStringLiteral("--add"), QDir::toNativeSeparators(song)}));
        calls.clear();
        QVERIFY(launcher::runEntry(nullptr, st(), forSong.first(), song));
        QCOMPARE(calls.size(), 1);
        QVERIFY(calls.first().startsWith(QStringLiteral("entry Enqueue In Player myplayer --add")));
        launcher::setHook({});
        st()->resetToDefaults(QStringLiteral("launch/"));
    }

    // ------------------------------------------------------------------ window / settings dialog
    void alwaysOnTop()
    {
        QVERIFY(!(m_app.mw->windowFlags() & Qt::WindowStaysOnTopHint));
        st()->setValue(keys::misc::AlwaysOnTop, true);
        QVERIFY(m_app.mw->windowFlags() & Qt::WindowStaysOnTopHint);
        QVERIFY(m_app.mw->isVisible());
        st()->setValue(keys::misc::AlwaysOnTop, false);
        QVERIFY(!(m_app.mw->windowFlags() & Qt::WindowStaysOnTopHint));
        QVERIFY(m_app.mw->isVisible());
    }

    void settingsPageRemembered()
    {
        m_app.mw->openSettings(QStringLiteral("search"));
        auto *d = m_app.mw->findChild<SettingsDialog *>();
        QVERIFY(d);
        QCOMPARE(st()->s(keys::ui::SettingsPage), QStringLiteral("search"));
        d->showPage(QStringLiteral("throttle"));
        QCOMPARE(st()->s(keys::ui::SettingsPage), QStringLiteral("throttle"));
        d->finish();
        d->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(!m_app.mw->findChild<SettingsDialog *>() || !m_app.mw->findChild<SettingsDialog *>()->isVisible());
        m_app.mw->openSettings(QString());   // the Settings button
        d = m_app.mw->findChild<SettingsDialog *>();
        QVERIFY(d && d->isVisible());
        QCOMPARE(d->currentPageId(), QStringLiteral("throttle"));
        d->finish();
        d->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void libraryPageOpensSharedTypes()
    {
        SettingsDialog d(m_app.core, m_app.mw);
        QVERIFY(d.showPage(QStringLiteral("library")));
        QVERIFY(!d.pageWidget(QStringLiteral("library"))->findChild<QTreeWidget *>());   // the dead editor is gone
        auto *b = d.findChild<QAbstractButton *>(QStringLiteral("libraryShareTypes"));
        QVERIFY(b && !b->toolTip().isEmpty());
        b->click();
        SharedFileTypesDialog *types = nullptr;
        QTRY_VERIFY((types = d.findChild<SharedFileTypesDialog *>()) != nullptr);
        types->close();
    }

    // ------------------------------------------------------------------ list colours / grid
    void gridHeaderSelectedColours()
    {
        m_app.mw->showPage(QStringLiteral("transfers"));
        MxListView *list = downloadList();
        list->selectionModel()->clear();
        const QColor grid(0x12, 0xab, 0x34), selText(0xfe, 0x01, 0xfd), head(0x40, 0x10, 0x90);
        st()->setValue(keys::colors::ListGrid, grid.name());
        st()->setValue(keys::colors::ListSelectedText, selText.name());
        st()->setValue(keys::colors::ListHeaderText, head.name());
        QTRY_COMPARE(list->header()->palette().color(QPalette::ButtonText), head);
        auto count = [list](const QColor &c) {
            const QImage img = list->viewport()->grab().toImage();
            int n = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (QColor(img.pixel(x, y)) == c)
                        ++n;
            return n;
        };
        QCOMPARE(count(grid), 0);
        st()->setValue(keys::ui::ShowGrid, true);
        QVERIFY(Colors::instance()->showGrid());
        QTRY_VERIFY(count(grid) > 100);
        st()->setValue(keys::ui::ShowGrid, false);
        QTRY_COMPARE(count(grid), 0);
        // a selected row draws its plain text in the Selected Text colour
        // a selected row draws its plain text in the Selected Text colour (state colours stay, as in the original WinMX)
        QStandardItemModel plain;
        for (int r = 0; r < 3; ++r)
            plain.appendRow({new QStandardItem(QStringLiteral("plain row text %1").arg(r)), new QStandardItem(QStringLiteral("more"))});
        MxListView own;
        own.setModel(&plain);
        own.resize(300, 100);
        own.show();
        QVERIFY(QTest::qWaitForWindowExposed(&own));
        auto countIn = [](QWidget *w, const QColor &c) {   // pixels of about that colour (smoothed text too)
            const QImage img = w->grab().toImage();
            int n = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const QColor p(img.pixel(x, y));
                    n += qAbs(p.red() - c.red()) + qAbs(p.green() - c.green()) + qAbs(p.blue() - c.blue()) < 90;
                }
            return n;
        };
        QCOMPARE(countIn(own.viewport(), selText), 0);
        own.selectRow(own.visibleRow(1));
        QTest::qWait(30);
        if (qEnvironmentVariableIsSet("MX_UIH_SHOTS"))
            own.viewport()->grab().save(qEnvironmentVariable("MX_UIH_SHOTS") + QStringLiteral("/selected_text.png"));
        QTRY_VERIFY(countIn(own.viewport(), selText) > 5);
        list->selectionModel()->clear();
        st()->resetToDefaults(QStringLiteral("colors/"));
    }

    void chatUserListColours()
    {
        ChatUserModel m;
        ChatUser admin, voiced, plain;
        admin.user = uib::user(QStringLiteral("op_10001"));
        admin.rank = 2;
        voiced.user = uib::user(QStringLiteral("voice_10002"));
        voiced.rank = 1;
        plain.user = uib::user(QStringLiteral("plain_10003"));
        m.setUsers({admin, voiced, plain});
        st()->setValue(keys::colors::ChatUserAdmin, QStringLiteral("#123456"));
        st()->setValue(keys::colors::ChatUserVoiced, QStringLiteral("#654321"));
        QCOMPARE(m.index(0, ChatUserModel::User).data(Qt::ForegroundRole).value<QColor>(), QColor(QStringLiteral("#123456")));
        QCOMPARE(m.index(1, ChatUserModel::User).data(Qt::ForegroundRole).value<QColor>(), QColor(QStringLiteral("#654321")));
        QCOMPARE(m.index(2, ChatUserModel::User).data(Qt::ForegroundRole).value<QColor>(), Colors::instance()->get(keys::colors::ChatUserListText));
        // the user list background
        auto *chat = qobject_cast<ChatPage *>(m_app.mw->page(QStringLiteral("chat")));
        m_app.mw->showPage(QStringLiteral("chat"));
        chat->join(QStringLiteral("Linux Users"));
        ChatRoomView *room = nullptr;
        QTRY_VERIFY((room = chat->currentRoom()) != nullptr);
        QCOMPARE(room->userList()->backgroundKey(), QString(keys::colors::ChatUserListBackground));
        st()->setValue(keys::colors::ChatUserListBackground, QStringLiteral("#203040"));
        QTRY_COMPARE(room->userList()->palette().color(QPalette::Base), QColor(QStringLiteral("#203040")));
        st()->resetToDefaults(QStringLiteral("colors/"));
        m_app.mw->showPage(QStringLiteral("networks"));
    }

    // the Speed column shows every connection label whole ("Unknown" was cut to "Unknowr"), at every
    // scale and at 800 x 600; the other columns keep their minimum too
    void chatUserListSpeedColumn_data()
    {
        QTest::addColumn<double>("scale");
        QTest::addColumn<QSize>("size");
        for (double sc : {1.0, 1.5, 2.0, 2.5})
            QTest::addRow("%d%%", int(sc * 100)) << sc << QSize(1024, 689);
        QTest::addRow("800x600") << 1.0 << QSize(800, 600);
    }
    void chatUserListSpeedColumn()
    {
        QFETCH(double, scale);
        QFETCH(QSize, size);
        setUiScale(scale);
        uib::App app;
        app.start(m_dir.filePath(QStringLiteral("chat%1-%2").arg(int(scale * 100)).arg(size.width())));
        app.core->settings()->setValue(keys::ui::ChatWarningDisabled, true);
        app.mw->resize(size.width() * scale, size.height() * scale);
        auto *chat = qobject_cast<ChatPage *>(app.mw->page(QStringLiteral("chat")));
        app.mw->showPage(QStringLiteral("chat"));
        chat->join(QStringLiteral("Linux Users"));
        ChatRoomView *room = nullptr;
        QTRY_VERIFY((room = chat->currentRoom()) != nullptr);
        QTest::qWait(50);
        MxListView *users = room->userList();
        QVERIFY(users->model()->rowCount() > 0);
        // squeeze the splitter as far as it goes: the list keeps the width its columns need
        if (auto *split = room->findChild<QSplitter *>(QStringLiteral("chatRoomSplitter")))
            split->setSizes({split->width(), 1});
        QTest::qWait(50);
        users->relayoutColumns();
        const QFontMetrics fm(users->font());
        int widest = 0;
        for (const QString &t : connectionTypes())
            widest = qMax(widest, fm.horizontalAdvance(search::connectionText(t)));
        QVERIFY(widest >= fm.horizontalAdvance(QStringLiteral("Unknown")));
        const int speedW = users->header()->sectionSize(ChatUserModel::Speed);
        QVERIFY2(speedW >= widest + S(4), qPrintable(QStringLiteral("Speed %1 px < text %2 px").arg(speedW).arg(widest)));
        QVERIFY(users->header()->sectionSize(ChatUserModel::Files) >= fm.horizontalAdvance(QStringLiteral("99999")));
        QVERIFY(users->columnsFillViewport());
        QVERIFY(users->width() >= users->minimumListWidth());
        if (qEnvironmentVariableIsSet("MX_UIH_SHOTS")) {
            const QString dir = qEnvironmentVariable("MX_UIH_SHOTS");
            QDir().mkpath(dir);
            app.mw->grab().save(QStringLiteral("%1/chat_users_%2_%3.png").arg(dir).arg(int(scale * 100)).arg(size.width()));
            // the other narrow lists: the Hotlist pane and the channel list
            app.mw->setHotlistVisible(true);
            chat->selectSubTab(QStringLiteral("list"));
            QTest::qWait(50);
            app.mw->grab().save(QStringLiteral("%1/hotlist_channels_%2_%3.png").arg(dir).arg(int(scale * 100)).arg(size.width()));
            app.mw->setHotlistVisible(false);
        }
        app.stop();
        setUiScale(1.0);
    }

    // ------------------------------------------------------------------ Connection Test window
    void connectionTestHidesAddressesInTheWindow()
    {
        // diag/hideAddresses: the window masks public addresses too (it masked only the report before)
        auto publicIn = [](QStandardItemModel *m) {
            static const QRegularExpression ip(QStringLiteral("\\b\\d{1,3}(\\.\\d{1,3}){3}\\b"));
            int n = 0;
            std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
                for (int r = 0; r < m->rowCount(parent); ++r)
                    for (int c = 0; c < m->columnCount(parent); ++c) {
                        const QModelIndex i = m->index(r, c, parent);
                        auto it = ip.globalMatch(i.data().toString() + QLatin1Char(' ') + i.data(Qt::ToolTipRole).toString());
                        while (it.hasNext())
                            n += classifyAddress(it.next().captured(0)) == AddressKind::Public;
                        if (c == 0)
                            walk(i);
                    }
            };
            walk(QModelIndex());
            return n;
        };
        st()->setValue(keys::diag::HideAddresses, false);
        ConnectionTestDialog d(m_app.core);
        auto *model = qobject_cast<QStandardItemModel *>(d.findChild<MxListView *>(QStringLiteral("diagList"))->model());
        QVERIFY(model);
        auto *hide = d.findChild<QCheckBox *>(QStringLiteral("diagHideAddresses"));
        QVERIFY(hide && hide->text().contains(QStringLiteral("window")));
        const int shown = publicIn(model);
        hide->setChecked(true);
        QVERIFY(st()->b(keys::diag::HideAddresses));
        QCOMPARE(publicIn(model), 0);
        if (shown == 0)
            qInfo("the demo report shows no public address: masking checked only for the empty case");
        st()->setValue(keys::diag::HideAddresses, true);
    }

    // screenshots of the Settings pages this task changed, the chat warning and the Connection Test
    // (only with MX_UIH_SHOTS=<dir>; the look is checked by eye, the behaviour by the tests above)
    void shots()
    {
        const QString dir = qEnvironmentVariable("MX_UIH_SHOTS");
        if (dir.isEmpty())
            QSKIP("MX_UIH_SHOTS not set");
        QDir().mkpath(dir);
        SettingsDialog d(m_app.core, m_app.mw);
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        for (const char *id : {"connection", "connection/options", "wpn", "wpn/overlay", "other", "ed2k", "web", "messages",
                               "messages/private", "messages/autoreply", "library", "channels/options", "search/options",
                               "transfers", "transfers/queuing", "transfers/downloads", "throttle", "throttle/schedule",
                               "launching/options", "misc", "misc/system", "misc/interface", "appearance/channel",
                               "appearance/library", "appearance/search", "appearance/private", "advanced"}) {
            QVERIFY2(d.showPage(QString::fromLatin1(id)), id);
            QTest::qWait(20);
            d.grab().save(QStringLiteral("%1/settings_%2.png").arg(dir, QString::fromLatin1(id).replace(QLatin1Char('/'), QLatin1Char('_'))));
        }
        ChatWarningDialog w(m_app.mw);
        w.show();
        QTest::qWait(20);
        w.grab().save(dir + QStringLiteral("/chat_warning.png"));
        st()->setValue(keys::diag::HideAddresses, true);
        ConnectionTestDialog ct(m_app.core);
        ct.show();
        QTest::qWait(50);
        ct.grab().save(dir + QStringLiteral("/connection_test_hidden.png"));
        QLabel info;
        info.setPixmap(informationPixmap(48));
        info.grab().save(dir + QStringLiteral("/information_icon.png"));
    }

    // ------------------------------------------------------------------ Queuing: reported speed
    void reportedConnectionType()
    {
        // Settings > Queuing "When there are 2 or more uploads, change reported connection speed to"
        // (the overlay announces connectionLabel(), which uses this with its running uploads)
        QCOMPARE(mx::reportedConnectionType(QStringLiteral("FTTH"), true, 2, QStringLiteral("DSL"), 1), QStringLiteral("FTTH"));
        QCOMPARE(mx::reportedConnectionType(QStringLiteral("FTTH"), true, 2, QStringLiteral("DSL"), 2), QStringLiteral("DSL"));
        QCOMPARE(mx::reportedConnectionType(QStringLiteral("FTTH"), false, 2, QStringLiteral("DSL"), 5), QStringLiteral("FTTH"));
        QCOMPARE(mx::reportedConnectionType(QStringLiteral("FTTH"), true, 0, QStringLiteral("DSL"), 5), QStringLiteral("FTTH"));
        // the values the overlay plugin reads (queue/changeSpeedWhenBusy, changeSpeedThreshold, changedSpeed)
        st()->setValue(keys::queue::ChangeSpeedWhenBusy, true);
        st()->setValue(keys::queue::ChangeSpeedThreshold, 3);
        st()->setValue(keys::queue::ChangedSpeed, QStringLiteral("4G"));
        auto reported = [this](int uploads) {
            return mx::reportedConnectionType(st()->s(keys::identity::Connection), st()->b(keys::queue::ChangeSpeedWhenBusy),
                                              st()->i(keys::queue::ChangeSpeedThreshold), st()->s(keys::queue::ChangedSpeed), uploads);
        };
        QCOMPARE(reported(2), st()->s(keys::identity::Connection));
        QCOMPARE(reported(3), QStringLiteral("4G"));
        st()->resetToDefaults(QStringLiteral("queue/"));
    }
};

QTEST_MAIN(TstUih)
#include "tst_uih.moc"
