// SPDX-License-Identifier: GPL-3.0-or-later
// [logo-1] The OctoMX application icon (the round MX badge with a pink octopus hugging the letters):
// drawn at every size the program and its resources use, transparent around a round badge, crisp
// pixel art (no partly transparent pixels), green and pink in sensible proportions, the same image on
// every call, and handed to the window title bar and the tray at the exact sizes (nothing scaled).
// With MX_LOGO_SHOTS=<dir> the tray and window icon pixmaps are also written there (contact sheet).
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QSystemTrayIcon>

#include <cmath>

#include "mock/mock_core.h"
#include "ui/kit/colors.h"
#include "ui/kit/icons.h"
#include "ui/main_window.h"
#include "ui/os_integration.h"
#include "ui/tray.h"

using namespace mx;
using namespace mx::ui;

namespace {

const QList<int> kSizes = {16, 20, 22, 24, 28, 32, 36, 40, 44, 48, 64, 96, 128, 256, 512};

bool isGreen(QRgb p) { return qGreen(p) > 100 && qRed(p) < 80 && qBlue(p) < 80; }
bool isPink(QRgb p) { return qRed(p) > 150 && qBlue(p) > 80 && qGreen(p) < qRed(p) - 60; }

QImage argb(const QImage &i) { return i.convertToFormat(QImage::Format_ARGB32); }

} // namespace

class TstLogo : public QObject {
    Q_OBJECT
private slots:
    void everySize_data()
    {
        QTest::addColumn<int>("size");
        for (int s : kSizes)
            QTest::addRow("%d px", s) << s;
    }
    void everySize()
    {
        QFETCH(int, size);
        const QImage img = argb(iconImage(IconId::App, size));
        QCOMPARE(img.size(), QSize(size, size));
        // transparent corners, and nothing outside the round badge: it sits well in a task bar
        for (QPoint c : {QPoint(0, 0), QPoint(size - 1, 0), QPoint(0, size - 1), QPoint(size - 1, size - 1)})
            QCOMPARE(qAlpha(img.pixel(c)), 0);
        int opaque = 0, green = 0, pink = 0;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const QRgb p = img.pixel(x, y);
                QVERIFY2(qAlpha(p) == 0 || qAlpha(p) == 255, "a partly transparent pixel: blurred");
                if (!qAlpha(p))
                    continue;
                ++opaque;
                QVERIFY2(std::hypot(x + 0.5 - size / 2.0, y + 0.5 - size / 2.0) <= size / 2.0, "a pixel outside the badge");
                green += isGreen(p);
                pink += isPink(p);
            }
        QVERIFY(opaque > size * size * 7 / 10);
        const double g = double(green) / opaque, k = double(pink) / opaque;
        const QByteArray what = QByteArray::number(g, 'f', 3) + " green, " + QByteArray::number(k, 'f', 3) + " pink";
        QVERIFY2(g > 0.12 && g < 0.35, what.constData());   // the MX
        QVERIFY2(k > 0.08 && k < 0.35, what.constData());   // the octopus
        QVERIFY2(g / k > 0.6 && k / g > 0.4, what.constData());   // neither drowns the other
    }

    void sameOnEveryCall()
    {
        for (int s : kSizes)
            QCOMPARE(argb(iconImage(IconId::App, s)), argb(iconImage(IconId::App, s)));
    }

    // the icon the program hands to the window title bar and to the system tray: the drawn image at
    // each size the system asks for, not a scaled copy of another size
    void windowAndTrayIcons()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TrayController::setTrayAvailableOverride(1);   // offscreen has no tray: pretend there is one
        MockOptions o;
        o.frozen = true;
        Core *core = createMockCoreWithOptions(dir.filePath(QStringLiteral("profile")), o);
        Colors::instance()->attach(core->settings());
        core->start();
        auto *mw = new MainWindow(core);
        mw->show();
        QCoreApplication::processEvents();
        auto *tray = mw->findChild<QSystemTrayIcon *>(QStringLiteral("trayIcon"));
        QVERIFY(tray);
        const QString shots = qEnvironmentVariable("MX_LOGO_SHOTS");
        if (!shots.isEmpty())
            QDir().mkpath(shots);
        for (int s : {16, 20, 22, 24, 32, 40, 48}) {
            const QImage want = argb(iconImage(IconId::App, s));
            const QImage t = argb(tray->icon().pixmap(QSize(s, s)).toImage());
            const QImage w = argb(mw->windowIcon().pixmap(QSize(s, s)).toImage());
            QCOMPARE(t, want);
            QCOMPARE(w, want);
            if (!shots.isEmpty()) {
                t.save(QDir(shots).filePath(QStringLiteral("tray-%1.png").arg(s)));
                w.save(QDir(shots).filePath(QStringLiteral("window-%1.png").arg(s)));
            }
        }
        delete mw;
        delete core;
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    TstLogo t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_logo.moc"
