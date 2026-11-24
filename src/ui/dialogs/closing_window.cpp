// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/closing_window.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>

#include "mx/services.h"
#include "ui/kit/icons.h"
#include "ui/kit/scale.h"
#include "ui/kit/widgets.h"

namespace mx::ui {

namespace {
QString megabytes(qint64 n)
{
    return QStringLiteral("%1 MB").arg(double(n) / (1 << 20), 0, 'f', n < (qint64(10) << 20) ? 1 : 0);
}
} // namespace

// the classic small message window: a line of text, a figure, one button (Win32 metrics, no new chrome)
ClosingWindow::ClosingWindow(QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("closingWindow"));
    setWindowTitle(tr("OctoMX"));
    setWindowIcon(appIcon());
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setFixedSize(SS(330, 112));
    auto *text = new WrapLabel(tr("Closing: saving downloads..."), this);
    text->setObjectName(QStringLiteral("closingText"));
    text->setToolTip(tr("OctoMX writes every download to the disk before it closes, so nothing is lost or damaged."));
    text->setGeometry(SR(16, 16, 298, 16));
    m_figure = new StaticText(QString(), this);
    m_figure->setObjectName(QStringLiteral("closingFigure"));
    m_figure->setToolTip(tr("How much is still being written to the disk."));
    m_figure->setGeometry(SR(16, 40, 298, 16));
    auto *now = new PushButton(tr("Close now"), this);
    now->setObjectName(QStringLiteral("closingCloseNow"));
    now->setToolTip(tr("Close without waiting. What is already saved is kept; what is not saved yet is downloaded again "
                       "the next time OctoMX runs."));
    now->setGeometry(SR(127, 75, 75, 23));
    connect(now, &QPushButton::clicked, this, &ClosingWindow::closeNowClicked);
    setLeft(-1, -1);
}

void ClosingWindow::setLeft(qint64 bytesLeft, qint64 bytesAtStart)
{
    if (bytesLeft < 0) {
        m_figure->setText(tr("Saving..."));
    } else if (bytesLeft == 0) {
        m_figure->setText(tr("Saving the torrent state..."));
    } else if (bytesAtStart > 0) {
        const int pct = int(qBound<qint64>(qint64(0), (bytesAtStart - qMin(bytesLeft, bytesAtStart)) * 100 / bytesAtStart, qint64(99)));
        m_figure->setText(tr("%1 % saved, %2 left to write").arg(pct).arg(megabytes(bytesLeft)));
    } else {
        m_figure->setText(tr("%1 left to write").arg(megabytes(bytesLeft)));
    }
}

void ClosingWindow::closeEvent(QCloseEvent *e)
{
    e->ignore();
}

bool waitForSafeQuit(Core *core, int showAfterMs, int capMs)
{
    if (!core) return true;
    core->beginShutdown();
    if (!core->shutdownBusy()) return true;
    QElapsedTimer t;
    t.start();
    const qint64 atStart = core->shutdownBytesLeft();
    bool closedNow = false;
    QPointer<ClosingWindow> win;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(100);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!core->shutdownBusy()) {
            loop.quit();
            return;
        }
        if (t.elapsed() >= capMs) {   // a hung disk: the last resort
            qWarning("octomx: the disk did not finish in %d s: closing without waiting", capMs / 1000);
            closedNow = true;
            core->closeNow();
            loop.quit();
            return;
        }
        if (!win && t.elapsed() >= showAfterMs) {
            win = new ClosingWindow;
            QObject::connect(win, &ClosingWindow::closeNowClicked, &loop, [&] {
                qInfo("octomx: Close now: %lld bytes not saved yet are downloaded again", core->shutdownBytesLeft());
                closedNow = true;
                core->closeNow();
                loop.quit();
            });
            win->show();
            win->raise();
            win->setLeft(core->shutdownBytesLeft(), atStart);
            // test seams (tst_safe_app): a picture of the window, "Close now" pressed after n ms
            if (const QString shot = qEnvironmentVariable("MX_TEST_CLOSING_SHOT"); !shot.isEmpty())
                QTimer::singleShot(300, win, [w = win.data(), shot] { w->grab().save(shot); });
            if (const int ms = qEnvironmentVariableIntValue("MX_TEST_CLOSE_NOW_MS"); ms > 0)
                QTimer::singleShot(ms, win, [w = win.data()] { emit w->closeNowClicked(); });
        }
        if (win) win->setLeft(core->shutdownBytesLeft(), atStart);
    });
    poll.start();
    loop.exec();
    poll.stop();
    qInfo("octomx: safe quit: waited %lld ms for the disk%s", t.elapsed(), closedNow ? " (Close now)" : "");
    delete win.data();
    return !closedNow;
}

} // namespace mx::ui
