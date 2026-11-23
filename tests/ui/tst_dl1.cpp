// SPDX-License-Identifier: GPL-3.0-or-later
// [dl-1] Transfers: "Download In Sequential Order" / "Download First And Last Pieces First" (menu,
// multi-selection, marks, tool tips, Add Link), Preview of a partial audio / video file (enablement,
// the first-time "Preview With" dialog, the remembered choice, the command line per choice with
// spaces and non-ASCII names, refusals, the multi-file chooser) and the Settings pages. The demo
// core; the launcher hook replaces every real start (no player is ever run).
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QMenu>
#include <QtWidgets/QRadioButton>

#include "uib_helpers.h"
#include "mx/download_order.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/preview_dialog.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/simple_dialogs.h"
#include "ui/kit/launcher.h"
#include "ui/kit/list_view.h"
#include "ui/models/transfer_model.h"
#include "ui/pages/transfers_page.h"

using namespace mx;
using namespace mx::ui;

namespace {
QAction *action(QMenu *m, const QString &name)
{
    if (!m) return nullptr;
    for (QAction *a : m->actions())
        if (a->objectName() == name) return a;
    return nullptr;
}
} // namespace

class TstDl1 : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    uib::App m_app;
    QStringList m_launched;
    QString profile() const { return m_dir.filePath(QStringLiteral("p")); }

    MxListView *downloads() const { return m_app.mw->findChild<MxListView *>(QStringLiteral("downloadList")); }
    QModelIndex rowOf(const QString &namePart) const
    {
        MxListView *l = downloads();
        for (int i = 0; i < l->visibleRowCount(); ++i) {
            const QModelIndex v = l->visibleRow(i);
            if (!v.parent().isValid() && v.data().toString().contains(namePart)) return v;
        }
        return {};
    }
    QString idOf(const QString &namePart) const
    {
        for (const TransferInfo &t : m_app.core->transfers()->downloads())
            if (t.name.contains(namePart)) return t.id;
        return {};
    }
    TransferInfo info(const QString &namePart) const { return m_app.core->transfers()->transfer(idOf(namePart)); }
    QMenu *menuFor(const QStringList &names)
    {
        MxListView *l = downloads();
        l->clearSelection();
        QModelIndex first;
        for (const QString &n : names) {
            const QModelIndex v = rowOf(n);
            if (!v.isValid()) return nullptr;
            if (!first.isValid()) first = v;
            l->selectionModel()->select(v, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
        emit l->contextMenuAt(first, l->viewport()->mapToGlobal(QPoint(30, 30)));
        return uib::openMenu();
    }
    void closeMenus()
    {
        for (QWidget *w : QApplication::topLevelWidgets())
            if (auto *m = qobject_cast<QMenu *>(w); m && m->isVisible()) m->close();
        QTest::qWait(10);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qunsetenv("MX_LAUNCH_LOG");
        m_app.start(profile());
        m_app.mw->showPage(QStringLiteral("transfers"));
        QTest::qWait(30);
        launcher::setHook([this](const QString &action, const QString &path) {
            m_launched << action + QLatin1Char('|') + path;
            return true;
        });
        TransfersPage::s_messageHook = [this](const QString &why) { m_launched << QStringLiteral("message|") + why; };
    }
    void cleanupTestCase()
    {
        launcher::setHook({});
        launcher::setCommandHook({});
        TransfersPage::s_messageHook = {};
        PreviewWithDialog::s_execHook = {};
        m_app.stop();
    }

    void orderEntriesInTheMenu()
    {
        QMenu *m = menuFor({QStringLiteral("Best Of Velorine")});
        QVERIFY(m);
        QAction *seq = action(m, QStringLiteral("actSequential"));
        QAction *fl = action(m, QStringLiteral("actFirstLast"));
        QVERIFY(seq && fl);
        QCOMPARE(seq->text(), QStringLiteral("Download In Sequential Order"));
        QCOMPARE(fl->text(), QStringLiteral("Download First And Last Pieces First"));
        QVERIFY(seq->isCheckable() && !seq->isChecked() && seq->isEnabled());
        QVERIFY(!seq->toolTip().isEmpty() && !fl->toolTip().isEmpty());
        seq->trigger();
        closeMenus();
        QVERIFY(info(QStringLiteral("Best Of Velorine")).sequential);
        // the mark after the name and the order in the status tool tip
        QTRY_VERIFY(rowOf(QStringLiteral("Best Of Velorine")).data().toString().endsWith(QStringLiteral("[Seq]")));
        const QModelIndex status = rowOf(QStringLiteral("Best Of Velorine")).siblingAtColumn(4);
        QVERIFY2(status.data(Qt::ToolTipRole).toString().contains(QStringLiteral("Download order: in sequential order.")),
                 qPrintable(status.data(Qt::ToolTipRole).toString()));
        m = menuFor({QStringLiteral("Best Of Velorine")});
        QVERIFY(action(m, QStringLiteral("actSequential"))->isChecked());
        action(m, QStringLiteral("actSequential"))->trigger();
        closeMenus();
        QVERIFY(!info(QStringLiteral("Best Of Velorine")).sequential);
        QTRY_VERIFY(!rowOf(QStringLiteral("Best Of Velorine")).data().toString().contains(QLatin1Char('[')));
    }

    void multiSelection()
    {
        const QStringList names = {QStringLiteral("Best Of Velorine"), QStringLiteral("Mind The Morning Tide"), QStringLiteral("Sintel")};
        QMenu *m = menuFor(names);
        QVERIFY(m);
        QVERIFY(!action(m, QStringLiteral("actFirstLast"))->isChecked());
        action(m, QStringLiteral("actFirstLast"))->trigger();
        closeMenus();
        for (const QString &n : names) QVERIFY2(info(n).firstLastFirst, qPrintable(n));
        QTRY_VERIFY(rowOf(QStringLiteral("Sintel")).data().toString().endsWith(QStringLiteral("[F+L]")));
        // one of them off: the entry is unchecked (not all), a click switches it on for all
        m_app.core->transfers()->setFirstLastPiecesFirst(idOf(QStringLiteral("Mind The Morning Tide")), false);
        m = menuFor(names);
        QVERIFY(!action(m, QStringLiteral("actFirstLast"))->isChecked());
        action(m, QStringLiteral("actFirstLast"))->trigger();
        closeMenus();
        for (const QString &n : names) QVERIFY(info(n).firstLastFirst);
        m = menuFor(names);
        QVERIFY(action(m, QStringLiteral("actFirstLast"))->isChecked());
        action(m, QStringLiteral("actFirstLast"))->trigger();
        closeMenus();
        for (const QString &n : names) QVERIFY(!info(n).firstLastFirst);
        // a completed download has no order to change
        m = menuFor({QStringLiteral("Big Buck Bunny")});
        QVERIFY(m);
        QVERIFY(!action(m, QStringLiteral("actSequential"))->isEnabled());
        closeMenus();
    }

    void previewEnablement()
    {
        // 48 % of an mp3 in the demo: Preview
        QMenu *m = menuFor({QStringLiteral("Best Of Velorine")});
        QAction *p = action(m, QStringLiteral("actPreview"));
        QVERIFY(p);
        QCOMPARE(p->text(), QStringLiteral("Preview"));
        QVERIFY(p->isEnabled() && !p->toolTip().isEmpty());
        closeMenus();
        // nothing yet: the entry says why and offers the two flags
        m = menuFor({QStringLiteral("Sintel")});
        p = action(m, QStringLiteral("actPreview"));
        QVERIFY(p);
        QCOMPARE(p->text(), QStringLiteral("Preview: needs the beginning of the file"));
        QVERIFY(p->isEnabled());
        p->trigger();
        closeMenus();
        QVERIFY(info(QStringLiteral("Sintel")).sequential && info(QStringLiteral("Sintel")).firstLastFirst);
        m = menuFor({QStringLiteral("Sintel")});
        QVERIFY(!action(m, QStringLiteral("actPreview"))->isEnabled());   // both on already: nothing more to offer
        closeMenus();
        m_app.core->transfers()->setSequentialDownload(idOf(QStringLiteral("Sintel")), false);
        m_app.core->transfers()->setFirstLastPiecesFirst(idOf(QStringLiteral("Sintel")), false);
        // not audio / video: no Preview at all
        m = menuFor({QStringLiteral("debian-13")});
        QVERIFY(m && !action(m, QStringLiteral("actPreview")));
        closeMenus();
        // switched off in Settings, File Launching, Launch Options
        m_app.core->settings()->setValue(QString(keys::launch::PreviewIncomplete), false);
        m = menuFor({QStringLiteral("Best Of Velorine")});
        QVERIFY(!action(m, QStringLiteral("actPreview")));
        closeMenus();
        m_app.core->settings()->setValue(QString(keys::launch::PreviewIncomplete), true);
    }

    void firstTimeDialogAndRememberedChoice()
    {
        m_app.core->settings()->setValue(QString(keys::launch::PreviewPlayer), QString());
        int asked = 0;
        PreviewWithDialog::s_execHook = [&asked](PreviewWithDialog *d) {
            ++asked;
            // the dialog: both choices, the program field, Browse..., Remember my choice; all with tool tips
            for (const char *n : {"pwSystem", "pwProgram", "pwPath", "pwBrowse", "pwRemember", "dlgOk", "dlgCancel"}) {
                QWidget *w = d->findChild<QWidget *>(QString::fromLatin1(n));
                if (!w || w->toolTip().isEmpty()) return int(QDialog::Rejected);
            }
            d->setChoice(QStringLiteral("system"));
            d->setRemember(true);
            return int(QDialog::Accepted);
        };
        m_launched.clear();
        QMenu *m = menuFor({QStringLiteral("Best Of Velorine")});
        action(m, QStringLiteral("actPreview"))->trigger();
        closeMenus();
        QCOMPARE(asked, 1);
        QCOMPARE(m_launched.size(), 1);
        QVERIFY2(m_launched.first().startsWith(QStringLiteral("preview|")) &&
                     m_launched.first().endsWith(QStringLiteral("Velorine - The Best Of Velorine - Entire Album.mp3")),
                 qPrintable(m_launched.first()));
        QCOMPARE(m_app.core->settings()->s(keys::launch::PreviewPlayer), QStringLiteral("system"));
        // remembered: no question the second time
        m = menuFor({QStringLiteral("Best Of Velorine")});
        action(m, QStringLiteral("actPreview"))->trigger();
        closeMenus();
        QCOMPARE(asked, 1);
        QCOMPARE(m_launched.size(), 2);
        // "Remember my choice" off: used once, asked again next time
        m_app.core->settings()->setValue(QString(keys::launch::PreviewPlayer), QString());
        PreviewWithDialog::s_execHook = [&asked](PreviewWithDialog *d) {
            ++asked;
            d->setChoice(QStringLiteral("system"));
            d->setRemember(false);
            return int(QDialog::Accepted);
        };
        m = menuFor({QStringLiteral("Best Of Velorine")});
        action(m, QStringLiteral("actPreview"))->trigger();
        closeMenus();
        QCOMPARE(asked, 2);
        QCOMPARE(m_app.core->settings()->s(keys::launch::PreviewPlayer), QString());
        // Cancel: nothing starts
        PreviewWithDialog::s_execHook = [](PreviewWithDialog *) { return int(QDialog::Rejected); };
        const int before = m_launched.size();
        m = menuFor({QStringLiteral("Best Of Velorine")});
        action(m, QStringLiteral("actPreview"))->trigger();
        closeMenus();
        QCOMPARE(m_launched.size(), before);
        PreviewWithDialog::s_execHook = {};
    }

    void commandLinePerChoice()
    {
        // a fake player (never run: the command hook takes the start) and media files with spaces and
        // non-ASCII letters in their names
        const QString dir = m_dir.filePath(QStringLiteral("Vid\u00e9os d\u00e9mo"));
        QVERIFY(QDir().mkpath(dir));
#ifdef Q_OS_WIN
        const QString player = QDir(dir).filePath(QStringLiteral("my player.exe"));
#else
        const QString player = QDir(dir).filePath(QStringLiteral("my player"));
#endif
        {
            QFile f(player);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\nexit 0\n");
            f.close();
            f.setPermissions(f.permissions() | QFile::ExeOwner | QFile::ExeUser);
        }
        const QString media = QDir(dir).filePath(QStringLiteral("\u00c4rger \u00fcber Sintel (2010).mkv"));
        {
            QFile f(media);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("demo");
        }
        launcher::setHook({});   // the real conversions, only the start replaced
        QStringList calls;
        launcher::setCommandHook([&calls](const QString &program, const QStringList &args, const QString &nativeArgs) {
            calls << program << args.join(QLatin1Char('\x01')) << nativeArgs;
            return true;
        });
        QString why;
        QVERIFY2(launcher::previewFile(nullptr, media, QStringLiteral("program"), player, &why), qPrintable(why));
        QCOMPARE(calls.size(), 3);
        QCOMPARE(QFileInfo(calls.at(0)).absoluteFilePath(), QFileInfo(player).absoluteFilePath());
        QCOMPARE(calls.at(1), QDir::toNativeSeparators(QFileInfo(media).absoluteFilePath()));   // one argument, as is
        QCOMPARE(calls.at(2), QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(QFileInfo(media).absoluteFilePath())));
        QCOMPARE(launcher::playerNativeArguments(QStringLiteral("C:/D\u00fcsseldorf/a b, c & d.mkv")),
                 QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(QFileInfo(QStringLiteral("C:/D\u00fcsseldorf/a b, c & d.mkv")).absoluteFilePath())));
        // the system's default player: the file:// URL of the file, percent-encoded
        calls.clear();
        QVERIFY(launcher::previewFile(nullptr, media, QStringLiteral("system"), QString(), &why));
        QCOMPARE(calls.at(0), QStringLiteral("url"));
        QCOMPARE(QUrl::fromEncoded(calls.at(1).toUtf8()).toLocalFile(), QFileInfo(media).absoluteFilePath());
        QVERIFY(calls.at(1).contains(QStringLiteral("%C3%84rger%20%C3%BCber")));
        // refusals: not audio / video, the player being the file itself, a missing player
        calls.clear();
        const QString exe = QDir(dir).filePath(QStringLiteral("setup.exe"));
        { QFile f(exe); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("MZ"); }
        QVERIFY(!launcher::previewFile(nullptr, exe, QStringLiteral("system"), QString(), &why));
        QCOMPARE(why, QStringLiteral("Only audio and video files can be previewed."));
        QVERIFY(!launcher::previewFile(nullptr, media, QStringLiteral("program"), media, &why));
        QVERIFY(!launcher::previewFile(nullptr, media, QStringLiteral("program"), dir + QStringLiteral("/nothing.exe"), &why));
        QVERIFY(why.contains(QStringLiteral("was not found")));
        QVERIFY(calls.isEmpty());
        // through the Transfers menu with the remembered program
        m_app.core->settings()->setValue(QString(keys::launch::PreviewPlayer), QStringLiteral("program"));
        m_app.core->settings()->setValue(QString(keys::launch::PreviewProgram), player);
        QMenu *m = menuFor({QStringLiteral("Best Of Velorine")});
        action(m, QStringLiteral("actPreview"))->trigger();
        closeMenus();
        QCOMPARE(calls.size(), 3);
        QVERIFY(calls.at(1).endsWith(QStringLiteral("Velorine - The Best Of Velorine - Entire Album.mp3")));
        // a completed video: Open File stays the system default, Open With Preview Player uses the player
        calls.clear();
        m = menuFor({QStringLiteral("Big Buck Bunny")});
        QVERIFY(action(m, QStringLiteral("actOpenFile")));
        QAction *withPlayer = action(m, QStringLiteral("actOpenWithPreviewPlayer"));
        QVERIFY(withPlayer && !withPlayer->toolTip().isEmpty());
        withPlayer->trigger();
        closeMenus();
        QCOMPARE(calls.size(), 3);
        QVERIFY(calls.at(1).endsWith(QStringLiteral("Big Buck Bunny 1080p.mkv")));
        launcher::setCommandHook({});
        launcher::setHook([this](const QString &action, const QString &path) {
            m_launched << action + QLatin1Char('|') + path;
            return true;
        });
        m_app.core->settings()->setValue(QString(keys::launch::PreviewPlayer), QString());
    }

    void playersFoundAreOffered()
    {
        launcher::setPlayersHook([] { return QList<launcher::Player>{{QStringLiteral("mpv"), QStringLiteral("/opt/players/mpv")}}; });
        PreviewWithDialog d(QStringLiteral("a.mkv"), QString(), QString());
        auto *path = d.findChild<QComboBox *>(QStringLiteral("pwPath"));
        QVERIFY(path);
        QCOMPARE(path->count(), 1);
        QCOMPARE(path->itemData(0).toString(), QStringLiteral("/opt/players/mpv"));
        QCOMPARE(d.choice(), QStringLiteral("system"));
        launcher::setPlayersHook({});
    }

    void multiFileChooser()
    {
        // the demo bundle: Elephants Dream (avi, avi, mov, flac, txt): the media files with their progress
        const QString id = idOf(QStringLiteral("Elephants Dream"));
        const PreviewInfo pi = m_app.core->transfers()->previewInfo(id);
        QCOMPARE(pi.files.size(), 4);   // README.txt is not offered
        PreviewFileDialog d(QStringLiteral("Elephants Dream (all formats)"), pi.files);
        QCOMPARE(d.rowCount(), 4);
        QVERIFY(d.chosenIndex() >= 0);   // the first ready one is preselected
        int readyRows = 0;
        for (const PreviewFile &f : pi.files) readyRows += f.ready ? 1 : 0;
        QVERIFY(readyRows > 0 && readyRows < 4);
        int chosen = -3;
        PreviewFileDialog::s_execHook = [&chosen](PreviewFileDialog *dlg) {
            dlg->selectRow(1);
            chosen = dlg->chosenIndex();
            return int(QDialog::Accepted);
        };
        m_app.core->settings()->setValue(QString(keys::launch::PreviewPlayer), QStringLiteral("system"));
        m_launched.clear();
        QMenu *m = menuFor({QStringLiteral("Elephants Dream")});
        QAction *p = action(m, QStringLiteral("actPreview"));
        QVERIFY(p);
        QCOMPARE(p->text(), QStringLiteral("Preview..."));
        p->trigger();
        closeMenus();
        QCOMPARE(chosen, pi.files.at(1).index);
        QCOMPARE(m_launched.size(), 1);
        QVERIFY2(m_launched.first().endsWith(QFileInfo(pi.files.at(1).name).fileName()), qPrintable(m_launched.first()));
        PreviewFileDialog::s_execHook = {};
        m_app.core->settings()->setValue(QString(keys::launch::PreviewPlayer), QString());
    }

    void addLinkDialogFlags()
    {
        m_app.core->settings()->setValue(QString(keys::transfers::FirstLastDefault), true);
        bool sawDefaults = false;
        TransfersPage::s_addLinkHook = [&sawDefaults](AddLinkDialog *d) {
            sawDefaults = !d->sequential() && d->firstLast();
            d->setText(QStringLiteral("magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Sintel.trailer.mkv"));
            d->setOrder(true, true);
            return true;
        };
        auto *page = m_app.mw->findChild<TransfersPage *>();
        QVERIFY(page);
        page->pasteLinks();
        QVERIFY(sawDefaults);
        const TransferInfo t = info(QStringLiteral("Sintel.trailer"));
        QVERIFY(!t.id.isEmpty());
        QVERIFY(t.sequential && t.firstLastFirst);
        TransfersPage::s_addLinkHook = {};
        m_app.core->settings()->setValue(QString(keys::transfers::FirstLastDefault), false);
    }

    void settingsPages()
    {
        SettingsDialog sd(m_app.core, m_app.mw);
        QVERIFY(sd.pageIds().contains(QStringLiteral("launching/preview")));
        sd.showPage(QStringLiteral("transfers/downloads"));
        auto *seq = sd.findChild<QCheckBox *>(QStringLiteral("set_transfers_sequentialDefault"));
        auto *fl = sd.findChild<QCheckBox *>(QStringLiteral("set_transfers_firstLastDefault"));
        QVERIFY(seq && fl && !seq->toolTip().isEmpty() && !fl->toolTip().isEmpty());
        QVERIFY(!seq->isChecked() && !fl->isChecked());   // qBittorrent: both off
        sd.showPage(QStringLiteral("launching/preview"));
        auto *prog = sd.findChild<QRadioButton *>(QStringLiteral("set_launch_previewPlayer_program"));
        auto *ask = sd.findChild<QRadioButton *>(QStringLiteral("set_launch_previewPlayer_"));
        QVERIFY(prog && ask);
        QVERIFY(ask->isChecked());
        QVERIFY(!prog->toolTip().isEmpty());
    }
};

QTEST_MAIN(TstDl1)
#include "tst_dl1.moc"
