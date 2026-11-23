// SPDX-License-Identifier: GPL-3.0-or-later
// [qa-2] regression tests for what the independent check of the modernised UI found:
//   * the Transfers menu of a completed download has Open File / Open Containing Folder once
//   * Explorer's /select command line (quoted: commas, spaces, "&", non-ASCII), FileManager1 URIs
//   * the interface scale never makes dialogs larger than the screen; dialogs / secondary windows
//     are kept on the work area
//   * scaled captions shrink a little instead of being cut; the schedule's day boxes do not overlap
//   * every kind of search hit either starts a Transfers row at once or says why
//   * wording: user-visible strings have no em/en dashes, no ellipsis character (U+2026) and no glyph the UI font
//     lacks (the "▸" in two messages drew as a box)
//   * small format fixes (exact-mode rate with a decimal comma, elision never splits a surrogate pair)
#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QAbstractProxyModel>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtGui/QRawFont>
#include <QtGui/QScreen>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QStyleOptionComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStyle>

#include "uib_helpers.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/about_dialog.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/setup_wizard.h"
#include "ui/kit/classic_style.h"
#include "ui/kit/format.h"
#include "ui/kit/launcher.h"
#include "ui/kit/list_view.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/models/search_model.h"
#include "ui/pages/search_page.h"
#include "ui/pages/transfers_page.h"
#include "ui/models/transfer_model.h"

using namespace mx;
using namespace mx::ui;

namespace {
QStringList actionTexts(QMenu *m)
{
    QStringList out;
    for (QAction *a : m->actions())
        out << (a->isSeparator() ? QStringLiteral("-") : a->text().remove(QLatin1Char('&')));
    return out;
}

QString writeFile(const QString &path, const QByteArray &data = QByteArrayLiteral("x"))
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
    return path;
}
} // namespace

class TstQa2 : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    uib::App m_app;
    QString profile() const { return m_dir.filePath(QStringLiteral("p")); }

    TransfersPage *transfers() { return qobject_cast<TransfersPage *>(m_app.mw->page(QStringLiteral("transfers"))); }
    QModelIndex downloadRow(const QString &name)
    {
        MxListView *list = transfers()->downloadList();
        for (int i = 0; i < list->visibleRowCount(); ++i)
            if (list->visibleRow(i).data().toString() == name)
                return list->visibleRow(i);
        return {};
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        fmt::setGroupSeparator(QLatin1Char(' '));
        m_app.start(profile());
    }
    void cleanupTestCase() { m_app.stop(); }

    // brief item 6: a completed download's menu had Open File / Open Containing Folder at the top AND
    // Open / Play / Open Containing Folder at the bottom (all doing the same)
    void completedDownloadMenuHasNoDuplicates()
    {
        m_app.mw->showPage(QStringLiteral("transfers"));
        TransfersPage *tp = transfers();
        const QModelIndex done = downloadRow(QStringLiteral("Big Buck Bunny 1080p.mkv"));
        QVERIFY(done.isValid());
        tp->downloadList()->selectRow(done);
        tp->showDownloadMenu(done, QPoint(5, 5));
        QTest::qWait(10);
        QMenu *m = uib::openMenu();
        QVERIFY(m);
        const QStringList t = actionTexts(m);
        QCOMPARE(t.value(0), QStringLiteral("Open File"));
        QCOMPARE(t.value(1), QStringLiteral("Open Containing Folder"));
        QCOMPARE(m->defaultAction()->text(), QStringLiteral("Open File"));
        QCOMPARE(t.count(QStringLiteral("Open Containing Folder")), 1);
        QVERIFY(!t.contains(QStringLiteral("Open")));
        QVERIFY(!t.contains(QStringLiteral("Play")));
        QVERIFY(!t.contains(QStringLiteral("Start")));
        QVERIFY(t.last() != QLatin1String("-"));   // no dangling separator at the end
        uib::closeMenus();

        // a download that is not complete keeps the classic greyed Open / Play and its folder entry
        QModelIndex running;
        for (const TransferInfo &x : m_app.core->transfers()->downloads())
            if (!transfers::isFinished(x) && !running.isValid())
                running = downloadRow(x.name);
        QVERIFY(running.isValid());
        tp->downloadList()->selectRow(running);
        tp->showDownloadMenu(running, QPoint(5, 5));
        QTest::qWait(10);
        m = uib::openMenu();
        QVERIFY(m);
        const QStringList r = actionTexts(m);
        QVERIFY(!r.contains(QStringLiteral("Open File")));
        QCOMPARE(r.count(QStringLiteral("Open Containing Folder")), 1);
        QAction *open = uib::menuAction(m, QStringLiteral("Open"));
        QVERIFY(open && !open->isEnabled());
        uib::closeMenus();
    }

    // brief item 3: the launcher's Explorer call (checked through the command hook, nothing starts)
    void explorerSelectCommandLine()
    {
        QCOMPARE(launcher::explorerSelectArguments(QStringLiteral("C:/Downloads/Sintel, 2010 & co.mkv")),
                 QStringLiteral("/select,\"") + QDir::toNativeSeparators(QStringLiteral("C:/Downloads/Sintel, 2010 & co.mkv")) +
                     QStringLiteral("\""));
        // a quote cannot end the GVariant string of the FileManager1 call
        const QString items = launcher::fileManagerItems(QStringLiteral("/tmp/it's here.mkv"));
        QVERIFY(items.startsWith(QStringLiteral("['file:")));
        QCOMPARE(items.count(QLatin1Char('\'')), 2);

        struct Call { QString program; QStringList args; QString native; };
        QList<Call> calls;
        launcher::setCommandHook([&calls](const QString &p, const QStringList &a, const QString &n) {
            calls << Call{p, a, n};
            return true;
        });
        const QString dir = m_dir.filePath(QStringLiteral("Big Buck Bunny & friends"));
        QVERIFY(QDir().mkpath(dir));
        const QStringList names = {QStringLiteral("Sintel, 2010 (4K).mkv"), QStringLiteral("big buck bunny's trailer.ogv"),
                                   QStringLiteral("Città d'arte, ünïcödé 日本.mp4"), QStringLiteral("a&b #1 %20.iso")};
        for (const QString &n : names) {
            const QString f = writeFile(dir + QLatin1Char('/') + n);
            calls.clear();
            QString why;
            QVERIFY2(launcher::showInFolder(f, &why), qPrintable(why));
            QVERIFY(!calls.isEmpty());
            const Call c = calls.first();
            const QString abs = QFileInfo(f).absoluteFilePath();
#ifdef Q_OS_WIN
            qInfo("explorer.exe %s", qPrintable(c.native));   // evidence in the log
            QCOMPARE(c.program, QStringLiteral("explorer.exe"));
            QCOMPARE(c.native, QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(abs)));
            QVERIFY(!c.native.mid(9).contains(QLatin1Char('/')));
#else
            if (c.program == QLatin1String("url")) {   // no session bus: the folder opens
                QCOMPARE(QUrl::fromEncoded(c.args.value(0).toUtf8()).toLocalFile(), QFileInfo(abs).absolutePath());
            } else {
                QVERIFY(c.program.endsWith(QLatin1String("gdbus")));
                QCOMPARE(c.args.value(c.args.size() - 2), launcher::fileManagerItems(abs));
            }
#endif
            // Open File hands the exact file to the desktop (a "#" or "%20" in the name survives)
            calls.clear();
            QVERIFY(launcher::openFile(nullptr, m_app.core->settings(), f, &why));
            QCOMPARE(calls.value(0).program, QStringLiteral("url"));
            QCOMPARE(QUrl::fromEncoded(calls.value(0).args.value(0).toUtf8()).toLocalFile(), abs);
        }
        // a bundle (folder) opens itself; a file deleted meanwhile shows its folder
        calls.clear();
        QVERIFY(launcher::showInFolder(dir));
        QCOMPARE(calls.value(0).program, QStringLiteral("url"));
        QCOMPARE(QUrl::fromEncoded(calls.value(0).args.value(0).toUtf8()).toLocalFile(), QFileInfo(dir).absoluteFilePath());
        calls.clear();
        QVERIFY(launcher::showInFolder(dir + QStringLiteral("/gone.avi")));
        QCOMPARE(QUrl::fromEncoded(calls.value(0).args.value(0).toUtf8()).toLocalFile(), QFileInfo(dir).absoluteFilePath());
        // a folder that is gone too: a reason, nothing started
        calls.clear();
        QString why;
        QVERIFY(!launcher::showInFolder(m_dir.filePath(QStringLiteral("nope/gone.avi")), &why));
        QVERIFY(why.contains(QStringLiteral("does not exist")));
        QVERIFY(calls.isEmpty());
        launcher::setCommandHook({});
    }

    // brief item 1: dialogs larger than the screen (800x600 at 175 %: Settings 922x614 at -61,-7)
    void scaleNeverExceedsTheScreen()
    {
        QCOMPARE(maxScaleForArea(QSize(800, 600)), 1.0);    // the 682-px button band at 125 % is 853 px
        QCOMPARE(maxScaleForArea(QSize(900, 600)), 1.25);
        QCOMPARE(maxScaleForArea(QSize(1024, 728)), 1.25);
        QCOMPARE(maxScaleForArea(QSize(1280, 680)), 1.5);
        QCOMPARE(maxScaleForArea(QSize(1366, 728)), 1.75);
        QCOMPARE(maxScaleForArea(QSize(1920, 1040)), 2.5);
        QCOMPARE(maxScaleForArea(QSize(2560, 1400)), 3.0);
        QCOMPARE(maxScaleForArea(QSize(640, 480)), 1.0);
        // the setting asks for 300 %, the (800x800 offscreen) screen holds less
        const double before = uiScale();
        qputenv("MX_UI_SCALE", "300");
        applyUiScale(nullptr);
        qunsetenv("MX_UI_SCALE");
        QCOMPARE(requestedUiScale(), 3.0);
        QCOMPARE(uiScale(), qMin(3.0, maxScaleFor(nullptr)));   // < 3.0 on the 800x800 offscreen screen
        QVERIFY(autoScale() <= maxScaleFor(nullptr));
        setUiScale(before);
        // kLargestDialog really is the largest fixed dialog (title bar included) at every scale
        for (double sc : {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0}) {
            setUiScale(sc);
            const QSize limit = SS(kLargestDialog) - SS(0, 30);
            SettingsDialog settings(m_app.core, m_app.mw);
            SetupWizard wizard(m_app.core);
            AboutDialog about(m_app.core);
            for (QWidget *d : {static_cast<QWidget *>(&settings), static_cast<QWidget *>(&wizard), static_cast<QWidget *>(&about)})
                QVERIFY2(d->width() <= limit.width() && d->height() <= limit.height(),
                         qPrintable(QStringLiteral("%1 at %2: %3x%4 > %5x%6").arg(d->objectName()).arg(sc).arg(d->width())
                                        .arg(d->height()).arg(limit.width()).arg(limit.height())));
        }
        setUiScale(before);
        // placing: inside stays, over an edge comes back, larger than the area keeps its title bar on it
        const QRect area(0, 0, 800, 560);
        QCOMPARE(placeOnScreen(QRect(10, 10, 300, 200), area), QPoint(10, 10));
        QCOMPARE(placeOnScreen(QRect(700, 500, 300, 200), area), QPoint(500, 360));
        QCOMPARE(placeOnScreen(QRect(-60, -7, 300, 200), area), QPoint(0, 0));
        QCOMPARE(placeOnScreen(QRect(-127, -51, 1054, 702), area), QPoint(0, 0));
        // the guard: a dialog shown off the screen comes back on it; a resizable secondary window
        // larger than the work area is made to fit
        const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
        QDialog dlg(m_app.mw);
        dlg.setFixedSize(300, 200);
        dlg.move(avail.right() + 200, avail.bottom() + 100);
        dlg.show();
        QTest::qWait(20);
        const QRect fg = dlg.frameGeometry();
        QVERIFY2(avail.contains(fg), qPrintable(QStringLiteral("frame %1,%2 %3x%4 in %5,%6 %7x%8").arg(fg.x()).arg(fg.y()).arg(fg.width())
                                                     .arg(fg.height()).arg(avail.x()).arg(avail.y()).arg(avail.width()).arg(avail.height())));
        dlg.hide();
        QWidget win;
        win.setWindowFlag(Qt::Window);
        win.setObjectName(QStringLiteral("someWindow"));
        win.resize(avail.width() + 400, avail.height() + 300);
        win.move(avail.topLeft() + QPoint(50, 60));
        win.show();
        QTest::qWait(20);
        QVERIFY(avail.contains(win.frameGeometry()));
        win.hide();
    }

    // above 100 % a caption that does not fit gets a slightly smaller font instead of being cut
    void scaledCaptionsShrinkInsteadOfClipping()
    {
        const double before = uiScale();
        QFont f = normalFont();
        setUiScale(1.0);
        QCOMPARE(fitFont(f, QStringLiteral("minutes (0 = never)"), 10), f);   // 100 %: never touched
        setUiScale(1.75);
        f.setPixelSize(20);
        const QString text = QStringLiteral("Limit incoming bandwidth");
        const int w = QFontMetrics(f).horizontalAdvance(text);
        QCOMPARE(fitFont(f, text, w + 5), f);                                  // fits: unchanged
        const QFont g = fitFont(f, text, w - 8);
        QVERIFY(g.pixelSize() < 20 && g.pixelSize() >= 17);
        QVERIFY(QFontMetrics(g).horizontalAdvance(text) <= w - 8);
        QCOMPARE(fitFont(f, text, w / 2).pixelSize(), 17);                     // at most 15 % smaller
        // a wrapped text one line too long for its box keeps every line
        WrapLabel label(QStringLiteral("A line with very little upload speed (an old ADSL line with 128 kbps, for example) "
                                       "usually does better with a SECONDARY connection to the OctoMX Peer Network."));
        QFont lf = f;
        lf.setPixelSize(20);
        label.setFont(lf);
        label.resize(400, 10);
        const int lines = int(label.wrappedLines().size());
        label.resize(400, (lines - 1) * label.linePitch() + QFontMetrics(lf).height() - 4);
        QFont used;
        int pitch = 0;
        QStringList drawn;
        QVERIFY(label.fitText(&used, &pitch, &drawn));
        QVERIFY(used.pixelSize() <= 20);
        QCOMPARE(drawn.join(QLatin1Char(' ')).simplified(), label.text().simplified());
        setUiScale(before);
    }

    // the combo box arrow button was a fixed 16-px sliver at every scale
    void comboArrowScales()
    {
        const double before = uiScale();
        ClassicStyle style;
        QComboBox cb;
        cb.addItem(QStringLiteral("Automatic (250 %)"));
        for (double sc : {1.0, 1.75, 2.5}) {
            setUiScale(sc);
            cb.resize(S(150), S(21));
            QStyleOptionComboBox opt;
            opt.initFrom(&cb);
            opt.rect = cb.rect();
            opt.frame = true;
            const QRect arrow = style.subControlRect(QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxArrow, &cb);
            const QRect field = style.subControlRect(QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, &cb);
            if (sc == 1.0)
                QCOMPARE(arrow.width(), 16);   // 100 %: Qt's classic geometry, untouched
            else
                QCOMPARE(arrow.width(), S(16));
            QVERIFY(field.right() < arrow.left());
            QVERIFY(cb.rect().contains(arrow));
        }
        setUiScale(before);
    }

    // Settings ▸ Bandwidth Schedule: equal 41-px slots cut "Wed" at 100 % and overlapped at 175 %
    void scheduleDaysDoNotOverlap()
    {
        SettingsDialog d(m_app.core, m_app.mw);
        QVERIFY(d.showPage(QStringLiteral("throttle/schedule")));
        d.show();
        QTest::qWait(20);
        QCheckBox *prev = nullptr;
        for (int i = 0; i < 7; ++i) {
            auto *c = d.findChild<QCheckBox *>(QStringLiteral("day%1").arg(i));
            QVERIFY(c);
            const int need = c->style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, c) +
                             c->style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing, nullptr, c) +
                             c->fontMetrics().horizontalAdvance(c->text());
            QVERIFY2(c->width() >= need, qPrintable(QStringLiteral("%1: %2 < %3").arg(c->text()).arg(c->width()).arg(need)));
            if (prev)
                QVERIFY(prev->geometry().right() < c->geometry().left());
            QVERIFY(c->parentWidget()->rect().contains(c->geometry()));
            prev = c;
        }
        d.close();
    }

    // the "looks like a fake" question was a 178-px box that cut its lines off (its width came from
    // line 5 of the text, "looks like a fake:")
    void fakeWarningShowsItsWholeText()
    {
        m_app.mw->showPage(QStringLiteral("search"));
        auto *page = qobject_cast<SearchPage *>(m_app.mw->page(QStringLiteral("search")));
        SearchTab *tab = page->newTab(true);
        MxListView *list = tab->findChild<MxListView *>(QStringLiteral("searchResults"));
        SearchModel *model = nullptr;
        for (auto *proxy = qobject_cast<QAbstractProxyModel *>(list->model()); !model && proxy;
             proxy = qobject_cast<QAbstractProxyModel *>(proxy->sourceModel()))
            model = qobject_cast<SearchModel *>(proxy->sourceModel());
        QVERIFY(model);
        model->setFlagSuspects(true);
        SearchHit h;
        h.fileName = QStringLiteral("Sintel.2010.1080p.BluRay.mkv");
        h.key = QStringLiteral("fake:1");
        h.size = 1341;
        h.suspect = QStringLiteral("1080p video of 1 341 bytes (too small for a video)");
        h.hashes.ed2k = QByteArray(16, '\x22');
        SearchSource src;
        src.net = Net::ED2K;
        src.user = uib::user(QStringLiteral("someone"), Net::ED2K);
        h.sources << src;
        model->applyHits({h});
        QTest::qWait(10);
        int labelWidth = 0, needed = 0;
        QTimer::singleShot(100, this, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                if (auto *label = box->findChild<QLabel *>(QStringLiteral("qt_msgbox_label"))) {
                    labelWidth = label->width();
                    for (const QString &line : box->text().split(QLatin1Char('\n')))
                        needed = qMax(needed, label->fontMetrics().horizontalAdvance(line));
                }
                box->reject();
            }
        });
        tab->downloadIndex(list->model()->index(0, 0));
        QVERIFY(needed > 0);
        QVERIFY2(labelWidth >= needed, qPrintable(QStringLiteral("%1 < %2").arg(labelWidth).arg(needed)));
        page->closeTab(tab);
    }

    // brief item 3 / ui-e item 6: every kind of hit starts a Transfers row at once or says why
    void everyHitDownloadsOrSaysWhy()
    {
        m_app.mw->showPage(QStringLiteral("search"));
        auto *page = qobject_cast<SearchPage *>(m_app.mw->page(QStringLiteral("search")));
        SearchTab *tab = page->currentTab();
        SearchTab::s_askHook = [](const QString &, const QString &) { return int(QMessageBox::Yes); };
        // accept the bundle file chooser when it comes up (modal)
        QTimer chooser;
        chooser.setInterval(50);
        connect(&chooser, &QTimer::timeout, this, [] {
            if (auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                d->accept();
        });
        chooser.start();
        tab->setTexts(QStringLiteral("linux"), QString());
        tab->startSearch();
        QTest::qWait(900);
        tab->flushPending();
        MxListView *list = tab->findChild<MxListView *>(QStringLiteral("searchResults"));
        QVERIFY(list);
        auto *model = qobject_cast<SearchModel *>(const_cast<QAbstractItemModel *>(list->mapToSource(list->model()->index(0, 0)).model()));
        QVERIFY(model);
        // the kinds the real core produces that the demo search does not: an index-only torrent
        // (LimeTorrents, no file list), a hit with neither source nor link, an OctoMX overlay hit
        SearchHit idx;
        idx.fileName = QStringLiteral("Sintel 4K (index only)");
        idx.size = 1 << 30;
        idx.isBundle = true;
        idx.hashes.btih = QByteArray(20, '\x11');
        idx.key = QStringLiteral("btih:1111");
        idx.indexes = {QStringLiteral("LimeTorrents")};
        idx.networks = {Net::BitTorrent};
        SearchSource web;
        web.net = Net::BitTorrent;
        web.connection = QStringLiteral("LimeTorrents");
        idx.sources << web;
        SearchHit none;
        none.fileName = QStringLiteral("nothing to fetch.iso");
        none.key = QStringLiteral("none:1");
        none.indexes = {QStringLiteral("BT4G")};
        SearchHit overlay;
        overlay.fileName = QStringLiteral("Big Buck Bunny 480p.avi");
        overlay.size = 123456789;
        overlay.key = QStringLiteral("ovl:1");
        SearchSource os;
        os.net = Net::Overlay;
        os.user = uib::user(QStringLiteral("bunnyfan_1234"));
        os.connection = QStringLiteral("FTTH");
        os.freeSlots = 1;
        os.totalSlots = 2;
        overlay.sources << os;
        model->applyHits({idx, none, overlay});
        QTest::qWait(20);
        QSet<QString> kinds;
        int rows = 0;
        for (int r = 0; r < list->model()->rowCount(); ++r) {
            const QModelIndex vi = list->model()->index(r, 0);
            const QModelIndex src = list->mapToSource(vi);
            const SearchHit h = model->hitFor(src);
            kinds << search::networkText(h);
            const int before = int(m_app.core->transfers()->downloads().size());
            const QString noticeBefore = m_app.mw->notice();
            m_app.mw->showNotice(QString());
            tab->downloadIndex(vi);
            QTest::qWait(10);
            const int after = int(m_app.core->transfers()->downloads().size());
            const QString notice = m_app.mw->notice();
            QVERIFY2(after > before || !notice.isEmpty(),
                     qPrintable(QStringLiteral("%1 (%2): no Transfers row and no message").arg(h.fileName, search::networkText(h))));
            if (h.key == QLatin1String("none:1"))
                QVERIFY(notice.contains(QStringLiteral("no source")));
            Q_UNUSED(noticeBefore)
            ++rows;
        }
        chooser.stop();
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->objectName() == QLatin1String("downloadProblem"))
                w->close();
        SearchTab::s_askHook = nullptr;
        QVERIFY(rows >= 13);
        QVERIFY2(kinds.size() >= 4, qPrintable(QStringList(kinds.values()).join(QStringLiteral(", "))));
    }

    // brief item 4: 5 000 results stay responsive; the filter keeps working with sorting and expanded rows
    void fiveThousandResultsStayResponsive()
    {
        m_app.mw->showPage(QStringLiteral("search"));
        auto *page = qobject_cast<SearchPage *>(m_app.mw->page(QStringLiteral("search")));
        SearchTab *tab = page->newTab(true);
        MxListView *list = tab->findChild<MxListView *>(QStringLiteral("searchResults"));
        auto *model = qobject_cast<SearchModel *>(list->model());
        for (auto *proxy = qobject_cast<QAbstractProxyModel *>(list->model()); !model && proxy;
             proxy = qobject_cast<QAbstractProxyModel *>(proxy->sourceModel()))
            model = qobject_cast<SearchModel *>(proxy->sourceModel());
        QVERIFY(model);
        QList<SearchHit> hits;
        for (int i = 0; i < 5000; ++i) {
            SearchHit h;
            h.fileName = QStringLiteral("%1 sample file %2.%3").arg(i % 3 ? QStringLiteral("debian") : QStringLiteral("sintel"))
                             .arg(i).arg(i % 2 ? QStringLiteral("iso") : QStringLiteral("mkv"));
            h.key = QStringLiteral("k:%1").arg(i);
            h.size = i % 11 ? qint64(i) * 104729 : 0;   // some unknown sizes
            h.totalPeers = i % 7 ? (i * 37) % 1000 : -1;  // some unknown peer counts
            for (int k = 0; k < 1 + i % 3; ++k) {
                SearchSource s;
                s.net = k ? Net::ED2K : Net::BitTorrent;
                s.user = uib::user(QStringLiteral("user%1_%2").arg(i).arg(k), s.net);
                s.seeds = i % 50;
                h.sources << s;
            }
            hits << h;
        }
        QElapsedTimer t;
        t.start();
        model->applyHits(hits);
        QTest::qWait(1);
        const qint64 insertMs = t.restart();
        list->sortByColumn(search::Peers, Qt::DescendingOrder);
        QTest::qWait(1);
        const qint64 sortMs = t.restart();
        list->setFilterText(QStringLiteral("sintel mkv"));
        const qint64 filterMs = t.restart();
        list->sortByColumn(search::Size, Qt::AscendingOrder);
        QTest::qWait(200);   // the refilter after a layout change is debounced
        const qint64 resortMs = t.restart();
        qInfo("5000 results: insert %lld ms, sort by Peers %lld ms, filter %lld ms, sort by Size while filtered %lld ms",
              insertMs, sortMs, filterMs, resortMs);
        QVERIFY2(sortMs < 1500 && filterMs < 1500, "sorting / filtering 5 000 results too slow");
        // every visible row matches the filter after the re-sort; the count is right
        int shown = 0;
        for (int r = 0; r < list->model()->rowCount(); ++r) {
            if (list->isRowHidden(r, QModelIndex()))
                continue;
            ++shown;
            const QString name = list->model()->index(r, 0).data().toString();
            QVERIFY2(name.contains(QStringLiteral("sintel")) && name.contains(QStringLiteral("mkv")), qPrintable(name));
        }
        QCOMPARE(shown, list->filterMatches());
        QCOMPARE(shown, 834);   // i % 3 == 0 and i even
        // unknown peers sort last both ways, also with the filter on
        list->sortByColumn(search::Peers, Qt::AscendingOrder);
        QTest::qWait(200);
        int lastKnown = -1, firstUnknown = INT_MAX;
        for (int r = 0; r < list->model()->rowCount(); ++r) {
            const QString p = list->model()->index(r, search::Peers).data().toString();
            if (p == QLatin1String("?"))
                firstUnknown = qMin(firstUnknown, r);
            else
                lastKnown = r;
        }
        QVERIFY(lastKnown < firstUnknown);
        // a child row's text (a source's user name) finds its file, shown with its parent
        list->setFilterText(QStringLiteral("user5_2"));
        QTest::qWait(10);
        QCOMPARE(list->filterMatches(), 1);
        list->setFilterText(QStringLiteral("4999.iso"));
        QTest::qWait(10);
        QCOMPARE(list->filterMatches(), 1);
        list->setFilterText(QString());
        QCOMPARE(list->filterMatches(), 5000);
        list->hideFilter();
        page->closeTab(tab);
    }

    // brief item 7: plain strings the UI font can draw
    void userVisibleStringsFollowTheWordingRules()
    {
        QString root = QStringLiteral(MX_SOURCE_DIR);
        if (!QFileInfo::exists(root + QStringLiteral("/src/ui")) && QFileInfo::exists(QStringLiteral("Z:") + root + QStringLiteral("/src/ui")))
            root = QStringLiteral("Z:") + root;   // wine: the Unix tree is drive Z:
        if (!QFileInfo::exists(root + QStringLiteral("/src/ui")))
            QSKIP("source tree not reachable");
        const QRawFont font = QRawFont::fromFont(normalFont());
        QStringList bad;
        int seen = 0;
        // tr("...") and QObject::tr("...") with adjacent literals ("a" "b")
        static const QRegularExpression call(QStringLiteral("\\btr\\(\\s*((?:\"(?:[^\"\\\\]|\\\\.)*\"\\s*)+)"));
        static const QRegularExpression lit(QStringLiteral("\"((?:[^\"\\\\]|\\\\.)*)\""));
        for (const QString &sub : {QStringLiteral("/src/ui"), QStringLiteral("/src/app")}) {
            QDirIterator it(root + sub, {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString path = it.next();
                QFile f(path);
                QVERIFY(f.open(QIODevice::ReadOnly));
                const QString src = QString::fromUtf8(f.readAll());
                auto m = call.globalMatch(src);
                while (m.hasNext()) {
                    const auto mm = m.next();
                    QString text;
                    auto l = lit.globalMatch(mm.captured(1));
                    while (l.hasNext())
                        text += l.next().captured(1);
                    ++seen;
                    QString why;
                    if (text.contains(QChar(0x2014)) || text.contains(QChar(0x2013)))
                        why = QStringLiteral("dash");
                    else if (text.contains(QStringLiteral("%n")))   // a plain "file(s)" phrase is classic WinMX
                        why = QStringLiteral("%n plural (no translator is loaded: \"3 file(s)\")");
                    else if (text.contains(QChar(0x2026)))
                        why = QStringLiteral("ellipsis character (use ...)");
                    else
                        for (const QChar c : std::as_const(text))
                            if (c.unicode() > 0x7f && !font.supportsCharacter(c)) {
                                why = QStringLiteral("U+%1 not in the UI font").arg(int(c.unicode()), 4, 16, QLatin1Char('0'));
                                break;
                            }
                    if (!why.isEmpty())
                        bad << QStringLiteral("%1: %2: %3").arg(QFileInfo(path).fileName(), why, text.left(60));
                }
            }
        }
        QVERIFY2(seen > 1000, qPrintable(QString::number(seen)));   // the scan really read the strings
        QVERIFY2(bad.isEmpty(), qPrintable(bad.join(QLatin1Char('\n'))));
    }

    void formatFixes()
    {
        // exact byte counts with "1.234.567" grouping: the rate uses the decimal comma too
        qputenv("MX_DIGIT_GROUPING", "dot");
        fmt::setExactSizes(true);
        QCOMPARE(fmt::rate(6666), QStringLiteral("6,51"));
        fmt::setExactSizes(false);
        QCOMPARE(fmt::rate(6666), QStringLiteral("6,51 KB/s"));
        qunsetenv("MX_DIGIT_GROUPING");
        QCOMPARE(fmt::rateKBs(6666), QStringLiteral("6.51"));
        // elision never leaves half of a surrogate pair
        const QString emoji = QStringLiteral("Sintel ") + QString::fromUcs4(U"\U0001F3AC\U0001F3AC\U0001F3AC\U0001F3AC") +
                              QStringLiteral(" final cut.mkv");
        const QFontMetrics fm(normalFont());
        for (int w = 20; w < fm.horizontalAdvance(emoji); ++w) {
            const QString e = fmt::elide(emoji, fm, w);
            if (e.size() > 3)
                QVERIFY2(!e.at(e.size() - 4).isHighSurrogate(), qPrintable(QString::number(w)));
        }
    }
};

QTEST_MAIN(TstQa2)
#include "tst_qa2.moc"
