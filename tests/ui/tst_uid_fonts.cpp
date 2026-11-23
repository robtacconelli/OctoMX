// SPDX-License-Identifier: GPL-3.0-or-later
// ui-d: Settings ▸ Appearance fonts really change the fonts, live, Normal = application font
// (chrome, menus), Dynamic = the black lists (row heights follow), Large = headlines / news bar;
// the defaults are exactly the built-in Tahoma 8 pt (11 px, no antialiasing) faces.
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>

#include "uib_helpers.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/kit/list_view.h"
#include "mx/settings_keys.h"

using namespace mx;
using namespace mx::ui;

class TstUidFonts : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    uib::App m_app;
    MxListView *downloads() const { return m_app.mw->findChild<MxListView *>(QStringLiteral("downloadList")); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_app.start(m_dir.filePath(QStringLiteral("p")));
    }
    void cleanupTestCase() { m_app.stop(); }

    void defaultsAreTheBuiltInFaces()
    {
        QCOMPARE(normalFont().pixelSize(), 11);
        QCOMPARE(QApplication::font().pixelSize(), 11);
        QVERIFY(normalFont().styleStrategy() & QFont::NoAntialias);
        QCOMPARE(largeFont().pixelSize(), 12);
        QVERIFY(largeFont().bold());
        QCOMPARE(downloads()->rowHeight(), 14);
        QCOMPARE(downloads()->header()->height(), 17);
        QCOMPARE(downloads()->font().pixelSize(), 11);
    }

    void dynamicFontResizesListsLive()
    {
        m_app.core->settings()->setValue(keys::ui::FontDynamic, QStringLiteral("Tahoma,12"));
        QCOMPARE(dynamicFont().pixelSize(), 16);
        QVERIFY(dynamicFont().styleStrategy() & QFont::NoAntialias);
        MxListView *l = downloads();
        QCOMPARE(l->font().pixelSize(), 16);
        const int extra = QFontMetrics(dynamicFont()).height() - 13;
        QVERIFY(extra > 0);
        QCOMPARE(l->rowHeight(), 14 + extra);
        // the rows really are taller (the view re-laid out)
        const QModelIndex first = l->model()->index(0, 0);
        QCOMPARE(l->visualRect(first).height(), 14 + extra);
        // a list created now (a new search tab) starts with the right height too
        MxListView fresh;
        QCOMPARE(fresh.rowHeight(), 14 + extra);
        fresh.setRowHeight(16);
        QCOMPARE(fresh.rowHeight(), 16 + extra);
        // the chrome keeps the normal font
        QCOMPARE(QApplication::font().pixelSize(), 11);
        QCOMPARE(l->header()->height(), 17);
    }

    void normalFontChangesChromeAndMenus()
    {
        m_app.core->settings()->setValue(keys::ui::FontNormal, QStringLiteral("Tahoma,10,bold"));
        QCOMPARE(normalFont().pixelSize(), 13);
        QVERIFY(normalFont().bold());
        QCOMPARE(QApplication::font().pixelSize(), 13);
        // list headers use it (taller), rows keep the dynamic font
        MxListView *l = downloads();
        QCOMPARE(l->header()->font().pixelSize(), 13);
        QCOMPARE(l->header()->height(), MxListView::headerHeightFor(normalFont()));
        QCOMPARE(l->font().pixelSize(), 16);
        // a menu opened now uses it
        auto *menu = new QMenu(m_app.mw);
        menu->addAction(QStringLiteral("x"));
        menu->popup(QPoint(10, 10));
        QTest::qWait(10);
        QCOMPARE(menu->font().pixelSize(), 13);
        menu->close();
        delete menu;
    }

    void largeFontChangesHeadline()
    {
        m_app.core->settings()->setValue(keys::ui::FontLarge, QStringLiteral("Arial,14"));
        QCOMPARE(largeFont().pixelSize(), 19);
        auto *headline = m_app.mw->findChild<QLabel *>(QStringLiteral("statusHeadline"));
        QVERIFY(headline);
        QCOMPARE(headline->font().pixelSize(), 19);
    }

    void resetRestoresExactDefaults()
    {
        Settings *s = m_app.core->settings();
        s->setValue(keys::ui::FontNormal, s->defaultValue(keys::ui::FontNormal));
        s->setValue(keys::ui::FontDynamic, s->defaultValue(keys::ui::FontDynamic));
        s->setValue(keys::ui::FontLarge, s->defaultValue(keys::ui::FontLarge));
        defaultsAreTheBuiltInFaces();
        QCOMPARE(m_app.mw->findChild<QLabel *>(QStringLiteral("statusHeadline"))->font().pixelSize(), 12);
    }

    void parseFormats()
    {
        const QFont fb = normalFont();
        QCOMPARE(Fonts::parse(QStringLiteral("Tahoma,8"), fb).pixelSize(), 11);
        QCOMPARE(Fonts::parse(QStringLiteral("Verdana,9,bold,italic"), fb).pixelSize(), 12);
        QVERIFY(Fonts::parse(QStringLiteral("Verdana,9,bold,italic"), fb).italic());
        QCOMPARE(Fonts::parse(QString(), fb), fb);
        QFont q(QStringLiteral("Arial"));
        q.setPointSize(10);
        QCOMPARE(Fonts::parse(q.toString(), fb).pixelSize(), 13);
    }
};

int main(int argc, char **argv)
{
    preApplicationSetup();
    QApplication app(argc, argv);
    installClassicLook(&app);
    TstUidFonts t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_uid_fonts.moc"
