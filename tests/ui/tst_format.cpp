// SPDX-License-Identifier: GPL-3.0-or-later
// Number / time formatting and "..." elision exactly as the original WinMX draws them.
#include <QtGui/QFontMetrics>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>

#include "ui/kit/format.h"
#include "ui/kit/theme.h"

using namespace mx::ui;

class TstFormat : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { fmt::setGroupSeparator(QLatin1Char(' ')); }

    void numbers()
    {
        QCOMPARE(fmt::number(0), QStringLiteral("0"));
        QCOMPARE(fmt::number(999), QStringLiteral("999"));
        QCOMPARE(fmt::number(1000), QStringLiteral("1 000"));
        QCOMPARE(fmt::number(4388571), QStringLiteral("4 388 571"));
        QCOMPARE(fmt::number(106764288), QStringLiteral("106 764 288"));
        QCOMPARE(fmt::number(-12345), QStringLiteral("-12 345"));
        fmt::setGroupSeparator(QLatin1Char(','));
        QCOMPARE(fmt::number(5792882), QStringLiteral("5,792,882"));
        fmt::setGroupSeparator(QLatin1Char(' '));
    }

    void durations()
    {
        QCOMPARE(fmt::duration(274), QStringLiteral("4:34"));
        QCOMPARE(fmt::duration(4448), QStringLiteral("1:14:08"));
        QCOMPARE(fmt::duration(44), QStringLiteral("0:44"));
        QCOMPARE(fmt::duration(-1), QString());
        QCOMPARE(fmt::eta(3730), QStringLiteral("1:02:10"));
    }

    void sizesAndRates()
    {
        QCOMPARE(fmt::bytesOf(583408, 6110000), QStringLiteral("583 408 of 6 110 000"));
        QCOMPARE(fmt::partialOf(25, 109878998), QStringLiteral("25% of 109 878 998"));
        QCOMPARE(fmt::percent(49, 100), QStringLiteral("49%"));
        QCOMPARE(fmt::percentInt(5, 0), 0);
        QCOMPARE(fmt::percentInt(200, 100), 100);
        QCOMPARE(fmt::rateKBs(6666), QStringLiteral("6.51"));
        QCOMPARE(fmt::rateKBs(0), QStringLiteral("0.00"));
        QCOMPARE(fmt::bitrate(128), QStringLiteral("128"));
        QCOMPARE(fmt::bitrate(-1), QString());
        QCOMPARE(fmt::ping(-1), QStringLiteral("?"));
    }

    void elide()
    {
        const QFontMetrics fm(normalFont());
        const QString s = QStringLiteral("Quill233_21212");
        QCOMPARE(fmt::elide(s, fm, 1000), s);
        const QString e = fmt::elide(s, fm, fm.horizontalAdvance(s) - 5);
        QVERIFY(e.endsWith(QLatin1String("...")));
        QVERIFY(!e.contains(QChar(0x2026)));
        QVERIFY(fm.horizontalAdvance(e) <= fm.horizontalAdvance(s) - 5);
        QCOMPARE(fmt::elide(s, fm, 0), QString());
    }
};

int main(int argc, char **argv)
{
    preApplicationSetup();
    QApplication app(argc, argv);
    installClassicLook(&app);
    TstFormat t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_format.moc"
