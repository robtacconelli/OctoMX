// SPDX-License-Identifier: GPL-3.0-or-later
// The palette object: spec defaults, values from Settings, live change notification.
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>

#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "ui/kit/colors.h"
#include "ui/kit/theme.h"
#include "ui/models/search_model.h"

using namespace mx;
using namespace mx::ui;

class TstColors : public QObject {
    Q_OBJECT
private slots:
    void specDefaults()
    {
        Colors::instance()->attach(nullptr);
        QCOMPARE(color(keys::colors::ListBackground), QColor(0, 0, 0));
        QCOMPARE(color(keys::colors::ListSelectedBackground), QColor(0x80, 0x80, 0x80));
        QCOMPARE(color(keys::colors::CanJoinDownload), QColor(0x80, 0xff, 0xff));
        QCOMPARE(color(keys::colors::CurrentlyDownloading), QColor(0x00, 0x40, 0x80));
        QCOMPARE(color(ckeys::OneSlotAvailable), QColor(0xff, 0x80, 0x40));
        QCOMPARE(color(ckeys::HotlistTemporary), QColor(0xff, 0xff, 0xcc));
        QCOMPARE(color(keys::colors::ProgressBar), QColor(0x00, 0x80, 0xff));
        QVERIFY(Colors::knownKeys().size() > 60);
    }

    void fromSettingsAndLive()
    {
        SimpleSettings s;
        Colors::instance()->attach(&s);
        // with the schema in place every spec default is also the schema default
        for (const QString &k : Colors::knownKeys()) {
            const QVariant v = s.value(k);
            if (v.isValid())
                QVERIFY2(QColor(v.toString()) == Colors::specDefault(k), qPrintable(k + QLatin1String(" schema ") + v.toString()));
        }
        QSignalSpy spy(Colors::instance(), &Colors::changed);
        s.setValue(QString(keys::colors::BitrateHigh), QStringLiteral("#123456"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(color(keys::colors::BitrateHigh), QColor(0x12, 0x34, 0x56));
        QCOMPARE(search::bitrateColor(320), QColor(0x12, 0x34, 0x56));
        s.setValue(QStringLiteral("ui/whatever"), 1);
        QCOMPARE(spy.count(), 1);   // only colours/... keys trigger a repaint
        s.resetToDefaults(QStringLiteral("colors/"));
        QCOMPARE(color(keys::colors::BitrateHigh), QColor(0x00, 0xff, 0x00));
        Colors::instance()->attach(nullptr);
    }

    void speedRules()
    {
        // [qa-1] owner request: modern connection types (original WinMX labels are still understood)
        QCOMPARE(search::connectionRank(QStringLiteral("T3+")), 9);         // -> FTTH
        QCOMPARE(search::connectionRank(QStringLiteral("DSL")), 1);
        QCOMPARE(search::connectionRank(QStringLiteral("128K ISDN")), 1);   // -> DSL
        QCOMPARE(search::connectionRank(QStringLiteral("10 Gbit+")), 12);
        QCOMPARE(search::connectionRank(QStringLiteral("Unknown")), 0);
        QCOMPARE(search::speedColor(search::connectionRank(QStringLiteral("VDSL"))), QColor(0x00, 0xff, 0x00));
        QCOMPARE(search::speedColor(search::connectionRank(QStringLiteral("4G"))), QColor(0xff, 0xff, 0x00));
        QCOMPARE(search::speedColor(search::connectionRank(QStringLiteral("DSL"))), QColor(0xff, 0xff, 0x00));
        QCOMPARE(search::speedColor(0), QColor(0xff, 0x00, 0x00));
        QCOMPARE(search::slotsColor(0), QColor(0xff, 0xff, 0xff));
    }
};

int main(int argc, char **argv)
{
    preApplicationSetup();
    QApplication app(argc, argv);
    installClassicLook(&app);
    TstColors t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_colors.moc"
