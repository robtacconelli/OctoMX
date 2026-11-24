// SPDX-License-Identifier: GPL-3.0-or-later
// [ver-1] Transfers: "Verify File" and "Restart Download..." in the context menu of a download: the
// entries and their enabled state per download state (with the tool tip that says why not), the
// notice with the outcome, the question of Restart (default "No"; No changes nothing; it names what
// is thrown away), multi-selection, and the script driver verbs. The demo core.
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

#include "uib_helpers.h"
#include "mx/trash.h"
#include "ui/kit/list_view.h"
#include "ui/pages/transfers_page.h"
#include "ui/script_driver.h"

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

class TstVer1 : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    uib::App m_app;
    QStringList m_asked;
    int m_answer = QMessageBox::No;

    MxListView *downloads() const { return m_app.mw->findChild<MxListView *>(QStringLiteral("downloadList")); }
    TransfersPage *page() const { return m_app.mw->findChild<TransfersPage *>(); }
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
    QString notice() const { return page()->statusFields().join(QLatin1Char('|')); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY2(trash::redirected(), "MX_OS_ROOT must point into the build tree");
        m_app.start(m_dir.filePath(QStringLiteral("p")));
        m_app.mw->showPage(QStringLiteral("transfers"));
        QTest::qWait(30);
        TransfersPage::s_askHook = [this](const QString &title, const QString &text) {
            m_asked << title + QLatin1Char('|') + text;
            return m_answer;
        };
    }
    void cleanupTestCase()
    {
        TransfersPage::s_askHook = {};
        m_app.stop();
    }

    // the two entries, enabled per state, each with a tool tip
    void entriesPerState()
    {
        struct Want { const char *name; bool verify; };
        const Want wants[] = {
            {"Big Buck Bunny", true},           // finished, still in the list
            {"Best Of Velorine", true},         // running: it pauses for the check by itself
            {"Elephants Dream", true},          // paused
            {"Marigold", true},                 // stopped with an error
            {"Sintel", false},                  // queued, nothing received yet
            {"Moonlit Ferry", false},           // finding sources, nothing received yet
        };
        for (const Want &w : wants) {
            QMenu *m = menuFor({QString::fromLatin1(w.name)});
            QVERIFY2(m, w.name);
            QAction *v = action(m, QStringLiteral("actVerifyFile"));
            QAction *r = action(m, QStringLiteral("actRestartDownload"));
            QVERIFY(v && r);
            QCOMPARE(v->text(), QStringLiteral("Verify File"));
            QCOMPARE(r->text(), QStringLiteral("Restart Download..."));
            QCOMPARE(v->isEnabled(), w.verify);
            QVERIFY2(r->isEnabled(), w.name);   // offered for every download
            QVERIFY(!v->toolTip().isEmpty() && !r->toolTip().isEmpty());
            if (!w.verify) QVERIFY2(v->toolTip().contains(QStringLiteral("Nothing of it has been received yet")), qPrintable(v->toolTip()));
            else QVERIFY2(v->toolTip().contains(QStringLiteral("only damaged parts are downloaded again")), qPrintable(v->toolTip()));
            QVERIFY2(r->toolTip().contains(QStringLiteral("from the beginning")), qPrintable(r->toolTip()));
            closeMenus();
        }
    }

    // Verify File: the row says Verifying, then the answer is the page's notice and the row's text
    void verifyNotice()
    {
        QMenu *m = menuFor({QStringLiteral("Big Buck Bunny")});
        action(m, QStringLiteral("actVerifyFile"))->trigger();
        closeMenus();
        QCOMPARE(info(QStringLiteral("Big Buck Bunny")).state, TransferState::Verifying);
        m = menuFor({QStringLiteral("Big Buck Bunny")});
        QVERIFY(!action(m, QStringLiteral("actVerifyFile"))->isEnabled());   // one check at a time
        QVERIFY(action(m, QStringLiteral("actVerifyFile"))->toolTip().contains(QStringLiteral("being verified now")));
        closeMenus();
        QTRY_VERIFY_WITH_TIMEOUT(notice().contains(QStringLiteral("Verified: 'Big Buck Bunny 1080p.mkv' is intact.")), 5000);
        QCOMPARE(info(QStringLiteral("Big Buck Bunny")).state, TransferState::Completed);
        QCOMPARE(info(QStringLiteral("Big Buck Bunny")).statusText, QStringLiteral("Complete, verified intact"));
    }

    // several selected downloads: every one that can be checked is checked, the answers together
    void multiSelection()
    {
        QTest::qWait(8200);   // the earlier notice is gone
        QMenu *m = menuFor({QStringLiteral("Marigold"), QStringLiteral("Sintel"), QStringLiteral("Best Of Velorine")});
        QAction *v = action(m, QStringLiteral("actVerifyFile"));
        QVERIFY(v && v->isEnabled());
        v->trigger();
        closeMenus();
        QCOMPARE(info(QStringLiteral("Marigold")).state, TransferState::Verifying);
        QCOMPARE(info(QStringLiteral("Best Of Velorine")).state, TransferState::Verifying);
        QCOMPARE(info(QStringLiteral("Sintel")).state, TransferState::Queued);
        QTRY_VERIFY_WITH_TIMEOUT(notice().contains(QStringLiteral("'Velorine - Marigold.mp3'")) &&
                                     notice().contains(QStringLiteral("'Velorine - The Best Of Velorine")), 5000);
        QCOMPARE(info(QStringLiteral("Marigold")).state, TransferState::Error);
    }

    // Restart Download...: the question (default No) names what goes; No changes nothing
    void restartQuestionNo()
    {
        const TransferInfo before = info(QStringLiteral("Big Buck Bunny"));
        QVERIFY(QFile::exists(before.savePath));
        m_asked.clear();
        m_answer = QMessageBox::No;
        QMenu *m = menuFor({QStringLiteral("Big Buck Bunny")});
        action(m, QStringLiteral("actRestartDownload"))->trigger();
        closeMenus();
        QCOMPARE(m_asked.size(), 1);
        QVERIFY2(m_asked.first().contains(QStringLiteral("the finished file goes to the %1").arg(trash::placeName())), qPrintable(m_asked.first()));
        QVERIFY(m_asked.first().startsWith(QStringLiteral("Restart Download|Restart \"Big Buck Bunny 1080p.mkv\" from the beginning?")));
        const TransferInfo after = info(QStringLiteral("Big Buck Bunny"));
        QCOMPARE(after.state, before.state);
        QCOMPARE(after.done, before.done);
        QVERIFY(QFile::exists(before.savePath));
        // a partial one: the bytes received so far
        m_asked.clear();
        m = menuFor({QStringLiteral("Marigold")});
        action(m, QStringLiteral("actRestartDownload"))->trigger();
        closeMenus();
        QCOMPARE(m_asked.size(), 1);
        QVERIFY2(m_asked.first().contains(QStringLiteral("nothing was received yet")) || m_asked.first().contains(QStringLiteral("received so far")),
                 qPrintable(m_asked.first()));
        QCOMPARE(info(QStringLiteral("Marigold")).state, TransferState::Error);
    }

    // the real box: "No" is its default button
    void restartBoxDefaultsToNo()
    {
        TransfersPage::s_askHook = {};
        bool sawBox = false, defaultNo = false;
        QTimer::singleShot(200, this, [&] {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                auto *box = qobject_cast<QMessageBox *>(w);
                if (!box || box->objectName() != QStringLiteral("restartQuestion")) continue;
                sawBox = true;
                defaultNo = box->defaultButton() == box->button(QMessageBox::No);
                QTest::keyClick(box, Qt::Key_Return);   // Enter = the default = No
            }
        });
        QMenu *m = menuFor({QStringLiteral("Big Buck Bunny")});
        action(m, QStringLiteral("actRestartDownload"))->trigger();
        closeMenus();
        QTRY_VERIFY(sawBox);
        QVERIFY(defaultNo);
        QCOMPARE(info(QStringLiteral("Big Buck Bunny")).state, TransferState::Completed);
        TransfersPage::s_askHook = [this](const QString &title, const QString &text) {
            m_asked << title + QLatin1Char('|') + text;
            return m_answer;
        };
    }

    // Yes: the finished file goes to the (test) trash, the download starts again from zero
    void restartYes()
    {
        QTest::qWait(8200);
        const TransferInfo before = info(QStringLiteral("Big Buck Bunny"));
        m_answer = QMessageBox::Yes;
        QMenu *m = menuFor({QStringLiteral("Big Buck Bunny")});
        action(m, QStringLiteral("actRestartDownload"))->trigger();
        closeMenus();
        m_answer = QMessageBox::No;
        const TransferInfo after = info(QStringLiteral("Big Buck Bunny"));
        QCOMPARE(after.id, before.id);
        QCOMPARE(after.done, qint64(0));
        QVERIFY(after.state != TransferState::Completed && after.state != TransferState::Paused);
        QVERIFY(!QFile::exists(before.savePath));
        QTRY_VERIFY_WITH_TIMEOUT(notice().contains(QStringLiteral("'Big Buck Bunny 1080p.mkv' starts again from zero")), 3000);
    }

    // the script driver verbs: verify-file <name>, restart-download <name> yes|no
    void scriptVerbs()
    {
        QTest::qWait(8200);
        ScriptDriver drv(m_app.mw);
        drv.setLines({QStringLiteral("page transfers"), QStringLiteral("verify-file Elephants Dream"), QStringLiteral("wait 600"),
                      QStringLiteral("restart-download Marigold no"), QStringLiteral("verify-file Sintel")});
        QSignalSpy done(&drv, &ScriptDriver::finished);
        drv.start(10);
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty(), 10000);
        // the last line fails: nothing of Sintel can be checked
        QCOMPARE(drv.errors().size(), 1);
        QVERIFY2(drv.errors().first().contains(QStringLiteral("cannot be verified now")), qPrintable(drv.errors().first()));
        QVERIFY(notice().contains(QStringLiteral("Elephants Dream")));
        QCOMPARE(info(QStringLiteral("Marigold")).state, TransferState::Error);   // "no": nothing changed
        ScriptDriver drv2(m_app.mw);
        drv2.setLines({QStringLiteral("restart-download Marigold yes")});
        QSignalSpy done2(&drv2, &ScriptDriver::finished);
        drv2.start(10);
        QTRY_VERIFY_WITH_TIMEOUT(!done2.isEmpty(), 10000);
        QVERIFY(drv2.errors().isEmpty());
        QCOMPARE(info(QStringLiteral("Marigold")).done, qint64(0));
    }
};

QTEST_MAIN(TstVer1)
#include "tst_ver1.moc"
