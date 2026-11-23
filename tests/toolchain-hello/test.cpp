// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest>
class HelloTest : public QObject {
    Q_OBJECT
private slots:
    void arithmetic() { QCOMPARE(6 * 7, 42); }
    void strings() { QVERIFY(QString("OctoMX").startsWith("Octo")); }
};
QTEST_GUILESS_MAIN(HelloTest)
#include "test.moc"
