// SPDX-License-Identifier: GPL-3.0-or-later
// [safe-1] The waiting part of the safe quit with a stand-in core whose disk drains at a set pace:
// no window when it is quick; after about a second the classic "Closing: saving downloads..." window
// with what is left; it goes away by itself when everything is saved; "Close now" ends the wait and
// tells the core (which then records only what is saved); the X and Esc do not close it.
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPushButton>

#include "mx/services.h"
#include "ui/dialogs/closing_window.h"
#include "ui/kit/widgets.h"

using namespace mx;

namespace {
class StandInCore : public Core {
public:
    qint64 left = 0;
    qint64 perTick = 0;
    bool pendingTorrents = false;
    int begun = 0, closedNow = 0;
    QTimer drain;
    StandInCore()
    {
        drain.setInterval(50);
        QObject::connect(&drain, &QTimer::timeout, this, [this] {
            left = qMax<qint64>(0, left - perTick);
            if (!left) pendingTorrents = false;
        });
    }
    Settings *settings() override { return nullptr; }
    NetworkService *network() override { return nullptr; }
    SearchService *search() override { return nullptr; }
    TransferService *transfers() override { return nullptr; }
    LibraryService *library() override { return nullptr; }
    ChatService *chat() override { return nullptr; }
    UserService *users() override { return nullptr; }
    BandwidthService *bandwidth() override { return nullptr; }
    QString profileDir() const override { return QString(); }
    QString versionString() const override { return QString(); }
    void start() override {}
    void shutdown() override {}
    void beginShutdown() override
    {
        ++begun;
        drain.start();
    }
    bool shutdownBusy() const override { return left > 0 || pendingTorrents; }
    qint64 shutdownBytesLeft() const override { return left; }
    void closeNow() override { ++closedNow; }
};

QWidget *closingWindow()
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w->isVisible() && w->objectName() == QLatin1String("closingWindow")) return w;
    return nullptr;
}
} // namespace

class TstSafeQuit : public QObject {
    Q_OBJECT
private slots:
    void quickNoWindow();
    void slowWindowAppearsAndGoes();
    void closeNowEndsTheWait();
    void xAndEscDoNotClose();
};

void TstSafeQuit::quickNoWindow()
{
    StandInCore c;
    c.left = 1000;
    c.perTick = 1000;
    bool seen = false;
    QTimer probe;
    QObject::connect(&probe, &QTimer::timeout, &probe, [&] { seen |= closingWindow() != nullptr; });
    probe.start(20);
    QVERIFY(ui::waitForSafeQuit(&c, 1000));
    QCOMPARE(c.begun, 1);
    QVERIFY(!seen);
    QCOMPARE(c.closedNow, 0);
}

void TstSafeQuit::slowWindowAppearsAndGoes()
{
    StandInCore c;
    c.left = qint64(60) << 20;
    c.perTick = qint64(2) << 20;   // 40 MB/s: 1.5 s
    c.pendingTorrents = true;
    QStringList figures;
    bool hasCloseNow = false, tooltips = true;
    QTimer probe;
    QObject::connect(&probe, &QTimer::timeout, &probe, [&] {
        if (QWidget *w = closingWindow()) {
            if (auto *f = w->findChild<ui::StaticText *>(QStringLiteral("closingFigure"))) {
                if (figures.isEmpty() || figures.last() != f->text()) figures << f->text();
                tooltips &= !f->toolTip().isEmpty();
            }
            if (auto *b = w->findChild<QPushButton *>(QStringLiteral("closingCloseNow"))) {
                hasCloseNow = b->isVisible() && b->text() == QLatin1String("Close now");
                tooltips &= !b->toolTip().isEmpty();
            }
        }
    });
    probe.start(30);
    QElapsedTimer t;
    t.start();
    QVERIFY(ui::waitForSafeQuit(&c, 1000));
    qInfo("waited %lld ms; the figures shown: %s", t.elapsed(), qPrintable(figures.join(QStringLiteral(" | "))));
    QVERIFY(t.elapsed() >= 1400);
    QVERIFY(hasCloseNow);
    QVERIFY(tooltips);
    QVERIFY2(!figures.isEmpty() && figures.first().contains(QLatin1String("left to write")), qPrintable(figures.join(QLatin1Char('|'))));
    QVERIFY(QTest::qWaitFor([] { return closingWindow() == nullptr; }, 1000));   // gone by itself
    QCOMPARE(c.closedNow, 0);
}

void TstSafeQuit::closeNowEndsTheWait()
{
    StandInCore c;
    c.left = qint64(1) << 30;
    c.perTick = 1 << 20;   // would take a minute
    QTimer::singleShot(1500, &c, [] {
        QWidget *w = closingWindow();
        QVERIFY(w);
        auto *b = w->findChild<QPushButton *>(QStringLiteral("closingCloseNow"));
        QVERIFY(b);
        QTest::mouseClick(b, Qt::LeftButton);
    });
    QElapsedTimer t;
    t.start();
    QVERIFY(!ui::waitForSafeQuit(&c, 1000));
    QVERIFY(t.elapsed() < 5000);
    QCOMPARE(c.closedNow, 1);
    QVERIFY(QTest::qWaitFor([] { return closingWindow() == nullptr; }, 1000));
}

void TstSafeQuit::xAndEscDoNotClose()
{
    StandInCore c;
    c.left = qint64(40) << 20;
    c.perTick = qint64(1) << 20;   // 2 s
    QTimer::singleShot(1300, &c, [] {
        QWidget *w = closingWindow();
        QVERIFY(w);
        w->close();
        QTest::keyClick(w, Qt::Key_Escape);
    });
    bool stillThere = false;
    QTimer::singleShot(1600, &c, [&] { stillThere = closingWindow() != nullptr; });
    QVERIFY(ui::waitForSafeQuit(&c, 1000));
    QVERIFY(stillThere);
    QCOMPARE(c.closedNow, 0);
}

QTEST_MAIN(TstSafeQuit)
#include "tst_safe_quit.moc"
