// SPDX-License-Identifier: GPL-3.0-or-later
// [str-1] Settings ▸ Other Networks ▸ Streaming API with a real core: off by default (nothing listens),
// every control with a tool tip, switched on (it listens, a token is made, the address shown), Copy and
// New token, a port another program holds (said plainly, no other port taken), the LAN switch; and a
// stream in the Transfers window with its "Streaming: ..." status.
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <QtGui/QClipboard>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>

#include "core/engine/download.h"
#include "core/engine/engine.h"
#include "core/real_core.h"
#include "engine_fakes.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/kit/colors.h"
#include "ui/kit/list_view.h"
#include "ui/kit/widgets.h"
#include "ui/main_window.h"

using namespace mx;
using namespace mx::ui;

namespace {
quint16 freePort()
{
    QTcpServer s;
    s.listen(QHostAddress::LocalHost, 0);
    return s.serverPort();
}
bool listens(quint16 port)
{
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, port);
    (void)QTest::qWaitFor([&] { return s.state() == QAbstractSocket::ConnectedState || s.state() == QAbstractSocket::UnconnectedState; }, 3000);
    return s.state() == QAbstractSocket::ConnectedState;
}
} // namespace

class TstStr1 : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    RealCore *m_core = nullptr;
    MainWindow *m_mw = nullptr;
    etest::TestPlugin *m_net = nullptr;
    quint16 m_port = 0;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_net = new etest::TestPlugin(Net::ED2K, NetworkPlugin::CanFetchRanges | NetworkPlugin::CanSearch);
        m_core = static_cast<RealCore *>(createRealCore(m_dir.filePath(QStringLiteral("profile")), {m_net}));
        m_port = freePort();
        m_core->settings()->setValue(keys::api::Port, int(m_port));
        m_core->settings()->setValue(keys::transfers::IncomingDir, m_dir.filePath(QStringLiteral("incoming")));
        m_core->settings()->setValue(keys::transfers::IncompleteDir, m_dir.filePath(QStringLiteral("incomplete")));
        m_core->settings()->setValue(keys::transfers::MinFreeSpaceMB, 0);
        Colors::instance()->attach(m_core->settings());
        m_core->start();
        m_mw = new MainWindow(m_core);
        m_mw->resize(1024, 689);
        m_mw->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_mw));
    }
    void cleanupTestCase()
    {
        delete m_mw;
        m_core->shutdown();
        delete m_core;
        Colors::instance()->attach(nullptr);
    }

    void settingsPage()
    {
        SettingsDialog d(m_core, m_mw);
        d.show();
        QVERIFY(d.showPage(QStringLiteral("api")));
        QCOMPARE(d.pageLabel(QStringLiteral("api")), QStringLiteral("Streaming API"));
        QWidget *page = d.findChild<QWidget *>(QStringLiteral("settingsPage_api"));
        QVERIFY(page);
        auto *on = page->findChild<QCheckBox *>(QStringLiteral("set_api_enabled"));
        auto *lan = page->findChild<QCheckBox *>(QStringLiteral("set_api_allowLan"));
        auto *keep = page->findChild<QCheckBox *>(QStringLiteral("set_api_keepStreams"));
        auto *port = page->findChild<QSpinBox *>(QStringLiteral("set_api_port"));
        auto *idle = page->findChild<QSpinBox *>(QStringLiteral("set_api_idleMin"));
        auto *limit = page->findChild<QSpinBox *>(QStringLiteral("set_api_cacheLimitMB"));
        auto *token = page->findChild<QLineEdit *>(QStringLiteral("set_api_token"));
        auto *base = page->findChild<QLineEdit *>(QStringLiteral("apiBaseAddress"));
        auto *state = page->findChild<WrapLabel *>(QStringLiteral("apiState"));
        auto *copy = page->findChild<QPushButton *>(QStringLiteral("apiCopyToken"));
        auto *renew = page->findChild<QPushButton *>(QStringLiteral("apiNewToken"));
        QVERIFY(on && lan && keep && port && idle && limit && token && base && state && copy && renew);
        // every control explains itself
        for (QWidget *w : page->findChildren<QWidget *>())
            if ((qobject_cast<QAbstractButton *>(w) || qobject_cast<QAbstractSpinBox *>(w) || qobject_cast<QLineEdit *>(w))
                && !qobject_cast<QAbstractSpinBox *>(w->parentWidget()))   // the edit inside a spin box is the spin box
                QVERIFY2(!w->toolTip().isEmpty(), qPrintable(w->objectName()));
        QVERIFY(lan->toolTip().contains(QStringLiteral("192.168")));
        // off by default; the defaults on the page
        QVERIFY(!on->isChecked());
        QVERIFY(!lan->isChecked());
        QVERIFY(!keep->isChecked());
        QCOMPARE(idle->value(), 240);
        QCOMPARE(limit->value(), 4096);
        QCOMPARE(state->text(), QStringLiteral("Off: nothing listens."));
        QVERIFY(token->text().isEmpty());
        QVERIFY(!listens(m_port));
        // on: it listens at once, a token was made, the address is shown
        on->click();
        QVERIFY(QTest::qWaitFor([&] { return listens(m_port); }, 3000));
        QVERIFY(QTest::qWaitFor([&] { return state->text().startsWith(QStringLiteral("Listening on 127.0.0.1")); }, 3000));
        QCOMPARE(token->text().size(), 48);
        QCOMPARE(base->text(), QStringLiteral("http://127.0.0.1:%1/api/v1/").arg(m_port));
        QVERIFY(token->isReadOnly());
        const QString picture = QStringLiteral(MX_STR1_SHOTS) + QStringLiteral("/settings-streaming-api.png");
        QDir().mkpath(QFileInfo(picture).absolutePath());
        d.grab().save(picture);
        // Copy, New token
        copy->click();
        QCOMPARE(QGuiApplication::clipboard()->text(), token->text());
        const QString old = token->text();
        renew->click();
        QVERIFY(QTest::qWaitFor([&] { return token->text() != old && token->text().size() == 48; }, 2000));
        QCOMPARE(m_core->settings()->s(keys::api::Token), token->text());
        // a port another program holds: said plainly, no other port taken
        QTcpServer other;
        QVERIFY(other.listen(QHostAddress::LocalHost, 0));
        port->setValue(other.serverPort());
        port->editingFinished();
        QVERIFY(QTest::qWaitFor([&] { return state->text().contains(QStringLiteral("in use")); }, 3000));
        QVERIFY(state->text().contains(QString::number(other.serverPort())));
        QVERIFY(!m_core->streamApiState().listening);
        d.grab().save(QStringLiteral(MX_STR1_SHOTS) + QStringLiteral("/settings-streaming-api-port-in-use.png"));
        port->setValue(m_port);
        port->editingFinished();
        QVERIFY(QTest::qWaitFor([&] { return listens(m_port); }, 3000));
        // the LAN switch reaches the core at once
        lan->click();
        QVERIFY(m_core->settings()->b(keys::api::AllowLan));
        lan->click();
        QVERIFY(!m_core->settings()->b(keys::api::AllowLan));
        // off again
        on->click();
        QVERIFY(QTest::qWaitFor([&] { return !listens(m_port); }, 3000));
        QCOMPARE(state->text(), QStringLiteral("Off: nothing listens."));
    }

    // a stream shows in the Transfers window as a download with an honest status
    void transfersRow()
    {
        const QByteArray data = etest::randomBytes(2 * 1024 * 1024, 5);
        etest::SourceConfig c;
        c.data = data;
        c.bytesPerSec = 200 * 1000;
        const SearchSource src = m_net->addSource(QStringLiteral("test-ed2k://row"), c, QStringLiteral("198.51.100.90:4662"));
        DownloadRequest r;
        r.name = QStringLiteral("Big Buck Bunny row.mkv");
        r.size = data.size();
        r.hashes.ed2k = QByteArray(16, '\x11');
        r.sources = {src};
        m_core->engine()->setNextStream(-1, m_core->engine()->streamCacheRoot());
        const QString id = m_core->engine()->download(r);
        m_core->engine()->clearNextStream();
        QVERIFY(!id.isEmpty());
        m_core->engine()->downloadObject(id)->setStreamNote(QStringLiteral("Streaming: idle, no player (the cache goes in 4 h 0 min)"));
        auto *list = m_mw->findChild<MxListView *>(QStringLiteral("downloadList"));
        QVERIFY(list);
        bool found = false;
        QVERIFY(QTest::qWaitFor([&] {
            for (int i = 0; i < list->visibleRowCount(); ++i) {
                const QModelIndex v = list->visibleRow(i);
                if (v.parent().isValid() || !v.data().toString().contains(QStringLiteral("Big Buck Bunny row"))) continue;
                for (int col = 0; col < list->model()->columnCount(); ++col)
                    if (v.siblingAtColumn(col).data().toString().startsWith(QStringLiteral("Streaming:"))) found = true;
            }
            return found;
        }, 5000));
        m_mw->grab().save(QStringLiteral(MX_STR1_SHOTS) + QStringLiteral("/transfers-stream-row.png"));
        // it can be cancelled there: its cache goes, never to the trash
        m_core->transfers()->cancel(id, true);
        QVERIFY(QTest::qWaitFor([&] { return m_core->transfers()->transfer(id).id.isEmpty(); }, 3000));
    }
};

QTEST_MAIN(TstStr1)
#include "tst_str1.moc"
