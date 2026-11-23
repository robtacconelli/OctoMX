// SPDX-License-Identifier: GPL-3.0-or-later
// [post-1] Owner request for 4.1: "remove the [em dash] from all texts, I prefer the comma". Fails when a
// user-visible text contains an em dash (U+2014), an en dash (U+2013) or the ellipsis character
// (U+2026, the UI writes three ASCII dots): every settings schema label and choice label, the
// texts of every widget reachable on every page and in the main dialogs and menus (buttons, labels,
// tool / status tips, window titles, combo items, menu entries, list headers and demo rows), and the
// generated Connection Test report. tools/check-no-dashes.sh checks the source tree itself.
#include <QtTest/QtTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QTreeView>

#include "uib_helpers.h"
#include "mx/diagnostics.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/about_dialog.h"
#include "ui/dialogs/close_prompt.h"
#include "ui/dialogs/connection_test.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/setup_wizard.h"
#include "ui/kit/widgets.h"
#include "ui/os_integration.h"
#include "ui/tray.h"
#include "ui/update_ui.h"

using namespace mx;
using namespace mx::ui;

namespace {

QString why(const QString &t)
{
    if (t.contains(QChar(0x2014)))
        return QStringLiteral("em dash");
    if (t.contains(QChar(0x2013)))
        return QStringLiteral("en dash");
    if (t.contains(QChar(0x2026)))
        return QStringLiteral("ellipsis character");
    return QString();
}

struct Collector {
    QStringList bad;
    int texts = 0;
    void check(const QString &where, const QString &t)
    {
        if (t.isEmpty())
            return;
        ++texts;
        const QString w = why(t);
        if (!w.isEmpty())
            bad << QStringLiteral("%1: %2: %3").arg(where, w, t.left(80));
    }
    void menu(const QString &where, QMenu *m, int depth = 0)
    {
        if (!m || depth > 4)
            return;
        emit m->aboutToShow();   // menus that fill themselves on show
        check(where + QStringLiteral(" title"), m->title());
        for (QAction *a : m->actions()) {
            check(where, a->text());
            check(where + QStringLiteral(" tip"), a->toolTip());
            check(where + QStringLiteral(" status"), a->statusTip());
            if (a->menu())
                menu(where + QStringLiteral(" > ") + a->text(), a->menu(), depth + 1);
        }
    }
    void model(const QString &where, QAbstractItemModel *m)
    {
        if (!m)
            return;
        for (int c = 0; c < m->columnCount(); ++c)
            check(where + QStringLiteral(" header"), m->headerData(c, Qt::Horizontal).toString());
        std::function<void(const QModelIndex &, int)> rows = [&](const QModelIndex &parent, int depth) {
            for (int r = 0; r < qMin(m->rowCount(parent), 40); ++r)
                for (int c = 0; c < m->columnCount(parent); ++c) {
                    const QModelIndex i = m->index(r, c, parent);
                    check(where, i.data(Qt::DisplayRole).toString());
                    check(where + QStringLiteral(" tip"), i.data(Qt::ToolTipRole).toString());
                    if (c == 0 && depth < 2 && m->hasChildren(i))
                        rows(i, depth + 1);
                }
        };
        rows(QModelIndex(), 0);
    }
    void widget(const QString &where, QWidget *root)
    {
        QList<QWidget *> all = root->findChildren<QWidget *>();
        all.prepend(root);
        for (QWidget *w : std::as_const(all)) {
            const QString n = where + QLatin1Char('/') + (w->objectName().isEmpty() ? QString::fromLatin1(w->metaObject()->className()) : w->objectName());
            if (w->isWindow())
                check(n + QStringLiteral(" title"), w->windowTitle());
            check(n + QStringLiteral(" tip"), w->toolTip());
            check(n + QStringLiteral(" status"), w->statusTip());
            check(n + QStringLiteral(" whatsthis"), w->whatsThis());
            if (auto *b = qobject_cast<QAbstractButton *>(w))
                check(n, b->text());
            if (auto *l = qobject_cast<QLabel *>(w))
                check(n, l->text());
            if (auto *g = qobject_cast<QGroupBox *>(w))
                check(n, g->title());
            if (auto *e = qobject_cast<QLineEdit *>(w))
                check(n + QStringLiteral(" placeholder"), e->placeholderText());
            if (auto *wl = dynamic_cast<WrapLabel *>(w))
                check(n, wl->text());
            if (auto *st = dynamic_cast<StaticText *>(w))
                check(n, st->text());
            if (auto *cb = qobject_cast<QComboBox *>(w))
                for (int i = 0; i < cb->count(); ++i)
                    check(n, cb->itemText(i));
            if (auto *mb = qobject_cast<QPushButton *>(w); mb && mb->menu())
                menu(n + QStringLiteral(" menu"), mb->menu());
            if (auto *tv = qobject_cast<QAbstractItemView *>(w))
                model(n, tv->model());
            if (auto *te = qobject_cast<QPlainTextEdit *>(w); te && te->objectName() != QLatin1String("aboutLicenses"))
                check(n, te->toPlainText());   // (the licences box holds third-party licence texts)
        }
    }
};

} // namespace

class TstNoDashes : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        TrayController::setTrayAvailableOverride(1);
    }

    void schemaTexts()
    {
        Collector c;
        for (const SettingDef &d : settingsSchema()) {
            c.check(d.key, d.label);
            for (const QString &l : d.choiceLabels)
                c.check(d.key + QStringLiteral(" choice"), l);
            if (d.type == SettingType::String && d.defaultValue.typeId() == QMetaType::QString)
                c.check(d.key + QStringLiteral(" default"), d.defaultValue.toString());
        }
        for (const QString &t : connectionTypes())
            c.check(QStringLiteral("connection type"), connectionTypeLabel(t));
        QVERIFY(c.texts > 400);
        QVERIFY2(c.bad.isEmpty(), qPrintable(c.bad.join(QLatin1Char('\n'))));
    }

    void everyPageDialogAndMenu()
    {
        uib::App app;
        app.start(m_dir.filePath(QStringLiteral("p1")));
        MainWindow *mw = app.mw;
        Collector c;
        // every page (with a search and a chat room open so their tabs exist)
        mw->startSearch(QStringLiteral("sintel"));
        QTest::qWait(300);
        for (const QString &id : MainWindow::pageIds()) {
            mw->showPage(id);
            QTest::qWait(30);
        }
        mw->showPage(QStringLiteral("chat"));
        c.widget(QStringLiteral("main"), mw);
        // menus: tray, system, Help, Settings button
        TrayController *tray = TrayController::of(mw);
        QVERIFY(tray);
        c.menu(QStringLiteral("tray"), tray->trayMenu());
        c.menu(QStringLiteral("system"), tray->systemMenu());
        c.menu(QStringLiteral("help"), mw->helpMenu());
        c.menu(QStringLiteral("settings button"), mw->settingsButtonMenu());
        // every Settings page
        mw->openSettings();
        auto *dlg = mw->findChild<SettingsDialog *>();
        QVERIFY(dlg);
        int pages = 0;
        for (QTreeView *tree : dlg->findChildren<QTreeView *>()) {
            std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
                for (int r = 0; r < tree->model()->rowCount(parent); ++r) {
                    const QModelIndex i = tree->model()->index(r, 0, parent);
                    tree->setCurrentIndex(i);
                    QTest::qWait(5);
                    ++pages;
                    walk(i);
                }
            };
            walk(QModelIndex());
        }
        QVERIFY2(pages > 30, qPrintable(QString::number(pages)));
        c.widget(QStringLiteral("settings"), dlg);
        dlg->close();
        // dialogs
        {
            AboutDialog about(app.core);
            c.widget(QStringLiteral("about"), &about);
        }
        for (bool downloads : {false, true}) {
            ClosePrompt cp(downloads);
            c.widget(QStringLiteral("close prompt"), &cp);
        }
        {
            SetupWizard wiz(app.core);
            for (int i = 0; i < 12; ++i) {
                c.widget(QStringLiteral("wizard"), &wiz);
                wiz.next();
                QTest::qWait(5);
            }
        }
        ConnectionTestDialog *ct = ConnectionTestDialog::open(app.core, mw);
        QTest::qWait(100);
        c.widget(QStringLiteral("connection test"), ct);
        ct->close();
        // the update box texts
        ReleaseInfo r;
        r.tag = QStringLiteral("v9.1.0");
        r.version = QStringLiteral("9.1.0");
        r.body = QStringLiteral("## Changes\n* one\n* two");
        c.check(QStringLiteral("update box"), UpdateController::offerText(r, QStringLiteral("4.1.0")));
        // the generated Connection Test report
        if (DiagnosticsService *d = app.core->diagnostics()) {
            c.check(QStringLiteral("report"), diagReportText(d->report(), true));
            c.check(QStringLiteral("report"), diagReportText(d->report(), false));
        }
        QVERIFY2(c.texts > 1500, qPrintable(QString::number(c.texts)));
        QVERIFY2(c.bad.isEmpty(), qPrintable(c.bad.join(QLatin1Char('\n'))));
        app.stop();
    }
};

QTEST_MAIN(TstNoDashes)
#include "tst_nodashes.moc"
