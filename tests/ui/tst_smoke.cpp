// SPDX-License-Identifier: GPL-3.0-or-later
// Smoke test: the main window on the frozen demo core, every page shown, a script exercising the
// driver verbs (search -> double-click -> download, menus, hotlist, float/dock, resize, shot).
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>

#include "mock/mock_core.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/legal_notice.h"
#include "ui/hotlist_pane.h"
#include "ui/kit/colors.h"
#include "ui/kit/list_view.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/main_window.h"
#include "ui/pages/page.h"
#include "ui/pages/search_page.h"
#include "ui/script_driver.h"

using namespace mx;
using namespace mx::ui;

class TstSmoke : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    Core *m_core = nullptr;
    MainWindow *m_mw = nullptr;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        MockOptions o;
        o.frozen = true;
        m_core = createMockCoreWithOptions(m_dir.filePath(QStringLiteral("profile")), o);
        Colors::instance()->attach(m_core->settings());
        m_core->start();
        m_mw = new MainWindow(m_core);
        m_mw->resize(1024, 689);
        m_mw->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_mw));
    }

    void titleAndPages()
    {
        QVERIFY(m_mw->windowTitle().startsWith(QStringLiteral("OctoMX v" MX_VERSION_STRING)));
        QVERIFY(m_mw->windowTitle().contains(QStringLiteral(" UL @ ")));          // demo uploads are running
        QVERIFY(m_mw->windowTitle().contains(QStringLiteral("  <Online")));
        for (const QString &id : MainWindow::pageIds()) {
            m_mw->showPage(id);
            QCOMPARE(m_mw->currentPageId(), id);
            QVERIFY(m_mw->page(id)->isVisible());
            QVERIFY(m_mw->pageButton(id)->isSelected());
            QTest::qWait(20);
        }
        m_mw->setHotlistVisible(true);
        QVERIFY(m_mw->pageButton(QStringLiteral("hotlist"))->isSelected());
        QVERIFY(m_mw->pageButton(MainWindow::pageIds().constLast())->isSelected());   // both selected at once
        m_mw->setHotlistVisible(false);
    }

    // the terms of use: asked once per profile, I Agree only after the check box, Exit refuses
    void termsOfUse()
    {
        Settings *st = m_mw->core()->settings();
        const QVariant before = st->value(QString(keys::internal::TermsAccepted));
        st->setValue(keys::internal::TermsAccepted, 0);
        QVERIFY(!LegalNotice::accepted(st));
        QCOMPARE(LegalNotice::paragraphs().size(), 6);
        QVERIFY(LegalNotice::paragraphs().first().contains(QStringLiteral("legal content")));
        // Exit: nothing is stored
        QTimer::singleShot(0, this, [] {
            auto *dlg = qobject_cast<LegalNotice *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            auto *agree = dlg->findChild<QPushButton *>(QStringLiteral("legalAgree"));
            QVERIFY(agree && !agree->isEnabled());
            // every paragraph is fully visible inside the dialog
            for (QWidget *w : dlg->findChildren<QWidget *>(QRegularExpression(QStringLiteral("^legal"))))
                QVERIFY2(dlg->rect().contains(w->geometry()), qPrintable(w->objectName()));
            if (!qEnvironmentVariableIsEmpty("MX_TERMS_SHOT"))   // for a look at the dialog
                dlg->grab().save(qEnvironmentVariable("MX_TERMS_SHOT"));
            dlg->findChild<QPushButton *>(QStringLiteral("legalExit"))->click();
        });
        QVERIFY(!LegalNotice::ensureAccepted(st));
        QVERIFY(!LegalNotice::accepted(st));
        // tick, I Agree: stored, and not asked again
        QTimer::singleShot(0, this, [] {
            auto *dlg = qobject_cast<LegalNotice *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            auto *agree = dlg->findChild<QPushButton *>(QStringLiteral("legalAgree"));
            dlg->findChild<QCheckBox *>(QStringLiteral("legalCheck"))->setChecked(true);
            QVERIFY(agree->isEnabled());
            agree->click();
        });
        QVERIFY(LegalNotice::ensureAccepted(st));
        QCOMPARE(st->i(keys::internal::TermsAccepted), qint64(LegalNotice::kTermsVersion));
        QVERIFY(LegalNotice::ensureAccepted(st));   // no dialog now (it would block here)
        // at every interface scale the texts and buttons stay inside the dialog, nothing overlaps
        for (double scale : {1.0, 1.5, 2.0, 3.0}) {
            setUiScale(scale);
            LegalNotice d(false);
            QList<QRect> boxes;
            for (QWidget *w : d.findChildren<QWidget *>(QRegularExpression(QStringLiteral("^legal")))) {
                QVERIFY2(d.rect().contains(w->geometry()), qPrintable(w->objectName()));
                for (const QRect &b : std::as_const(boxes))
                    QVERIFY2(!b.intersects(w->geometry()), qPrintable(w->objectName()));
                boxes << w->geometry();
            }
            QCOMPARE(boxes.size(), 6 + 3);   // six paragraphs, the check box, I Agree, Exit
            if (!qEnvironmentVariableIsEmpty("MX_TERMS_SHOT"))
                d.grab().save(qEnvironmentVariable("MX_TERMS_SHOT") + QStringLiteral(".%1.png").arg(int(scale * 100)));
        }
        setUiScale(1.0);
        // the read-only form of Help, Terms of Use...
        LegalNotice ro(true);
        QVERIFY(ro.findChild<QPushButton *>(QStringLiteral("legalClose")));
        QVERIFY(!ro.findChild<QPushButton *>(QStringLiteral("legalAgree")));
        st->setValue(keys::internal::TermsAccepted, before);
    }

    void scriptRun()
    {
        const QString shot = m_dir.filePath(QStringLiteral("smoke.png"));
        const int before = int(m_core->transfers()->downloads().size());
        ScriptDriver d(m_mw);
        d.setLines({
            QStringLiteral("page search"),
            QStringLiteral("type searchText velorine"),
            QStringLiteral("key searchText Return"),
            QStringLiteral("wait 400"),
            QStringLiteral("expect-rows searchResults 100"),
            QStringLiteral("expand-row searchResults 0"),
            QStringLiteral("select-row searchResults 1"),
            QStringLiteral("context-menu searchResults 12"),
            QStringLiteral("wait 50"),
            QStringLiteral("menu-pick Add To Hotlist"),
            QStringLiteral("wait 50"),
            QStringLiteral("dblclick-row searchResults 12"),
            QStringLiteral("wait 50"),
            QStringLiteral("page networks"),
            QStringLiteral("subtab other"),
            QStringLiteral("context-menu serverList 1"),
            QStringLiteral("close-menus"),
            QStringLiteral("hotlist on"),
            QStringLiteral("page transfers"),
            QStringLiteral("context-menu downloadList 0"),
            QStringLiteral("wait 50"),
            QStringLiteral("menu-pick Pause"),
            QStringLiteral("hotlist off"),
            QStringLiteral("float transfers"),
            QStringLiteral("wait 50"),
            QStringLiteral("dock transfers"),
            QStringLiteral("resize 800 600"),
            QStringLiteral("shot ") + shot,
            QStringLiteral("quit"),
        });
        QSignalSpy done(&d, &ScriptDriver::finished);
        d.start(0);
        QVERIFY(done.wait(20000));
        QCOMPARE(done.first().first().toInt(), 0);
        QVERIFY2(d.errors().isEmpty(), qPrintable(d.errors().join(QLatin1Char('\n'))));
        QVERIFY(QFileInfo(shot).size() > 1000);
        QCOMPARE(int(m_core->transfers()->downloads().size()), before + 1);   // the double-click started a download
        bool paused = false;
        for (const TransferInfo &t : m_core->transfers()->downloads())
            paused |= t.state == TransferState::Paused && t.id == QStringLiteral("d1");
        QVERIFY(paused);
        QVERIFY(m_mw->size() == QSize(800, 600));
        QVERIFY(!m_mw->isFloating(QStringLiteral("transfers")));
    }

    void badCommandFails()
    {
        ScriptDriver d(m_mw);
        d.setLines({QStringLiteral("frobnicate 1")});
        QSignalSpy done(&d, &ScriptDriver::finished);
        d.start(0);
        QVERIFY(done.wait(5000));
        QCOMPARE(done.first().first().toInt(), 2);
    }

    void linksAndPersistence()
    {
        const int before = int(m_core->transfers()->downloads().size());
        m_mw->addLinks({QStringLiteral("magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Sintel"),
                        QStringLiteral("not a link")});
        QCOMPARE(int(m_core->transfers()->downloads().size()), before + 1);
        QCOMPARE(m_mw->currentPageId(), QStringLiteral("transfers"));
        m_mw->saveState();
        QVERIFY(!m_core->settings()->value(QStringLiteral("ui/geometry")).toByteArray().isEmpty());
        QCOMPARE(m_core->settings()->s(QStringLiteral("ui/lastPage")), QStringLiteral("transfers"));
    }

    void cleanupTestCase()
    {
        delete m_mw;
        QSignalSpy fin(m_core, &Core::shutdownFinished);
        m_core->shutdown();
        QVERIFY(fin.wait(5000));
        delete m_core;
    }
};

int main(int argc, char **argv)
{
    preApplicationSetup();
    QApplication app(argc, argv);
    installClassicLook(&app);
    TstSmoke t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_smoke.moc"
