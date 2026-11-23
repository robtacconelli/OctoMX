// SPDX-License-Identifier: GPL-3.0-or-later
// [qa-1] Fidelity regression tests for differences found against the reference screenshots.
#include <QtCore/QCryptographicHash>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtGui/QFontDatabase>
#include <QtGui/QPainter>
#include <QtTest/QtTest>

#include "mx/settings_keys.h"
#include "ui/models/search_model.h"
#include "uib_helpers.h"

using namespace mx;
using namespace mx::ui;

// a text sample rendered with the UI font (normal + bold), as a hash of the pixels
static QByteArray renderHash()
{
    QImage img(400, 60, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setPen(Qt::black);
    p.setFont(normalFont());
    p.drawText(2, 14, QStringLiteral("Velorine - Beside The Mill Pond.mp3  4 388 571  J1 Transfers Queuing..."));
    p.setFont(classicFont(12, true));
    p.drawText(2, 40, QStringLiteral("Ready - 6 networks online  OctoMX Help"));
    p.end();
    return QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(img.constBits()), int(img.sizeInBytes())),
                                    QCryptographicHash::Sha1).toHex();
}

class TestQa1Fidelity : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { installClassicLook(qApp); }

    // ---- owner request: modern connection types (intentional deviation from the original WinMX)
    void connectionTypeHelpers()
    {
        const QStringList want{QStringLiteral("Unknown"), QStringLiteral("DSL"), QStringLiteral("VDSL"), QStringLiteral("FTTC"),
                               QStringLiteral("Cable"), QStringLiteral("4G"), QStringLiteral("5G"), QStringLiteral("Satellite"),
                               QStringLiteral("FTTB"), QStringLiteral("FTTH"), QStringLiteral("2.5 Gbit"), QStringLiteral("5 Gbit"),
                               QStringLiteral("10 Gbit+")};
        QCOMPARE(connectionTypes(), want);
        QCOMPARE(connectionTypeLabel(QStringLiteral("DSL")), QStringLiteral("DSL (up to 24 Mbit)"));
        QCOMPARE(connectionTypeLabel(QStringLiteral("FTTH")), QStringLiteral("FTTH / Gbit (300-1000 Mbit)"));
        QCOMPARE(connectionTypeLabel(QStringLiteral("10 Gbit+")), QStringLiteral("10 Gbit or faster (10000+ Mbit)"));
        // speed order: Unknown < DSL < 4G < VDSL < Satellite < FTTC < Cable < 5G < FTTB < FTTH < 2.5 < 5 < 10
        const QStringList bySpeed{QStringLiteral("Unknown"), QStringLiteral("DSL"), QStringLiteral("4G"), QStringLiteral("VDSL"),
                                  QStringLiteral("Satellite"), QStringLiteral("FTTC"), QStringLiteral("Cable"), QStringLiteral("5G"),
                                  QStringLiteral("FTTB"), QStringLiteral("FTTH"), QStringLiteral("2.5 Gbit"), QStringLiteral("5 Gbit"),
                                  QStringLiteral("10 Gbit+")};
        for (int i = 0; i < bySpeed.size(); ++i) {
            QCOMPARE(connectionSpeedRank(bySpeed.at(i)), i);
            QCOMPARE(connectionTypeForSpeedRank(i), bySpeed.at(i));
            QCOMPARE(connectionSpeedRank(connectionTypeLabel(bySpeed.at(i))), i);   // full labels too
        }
        QCOMPARE(connectionSpeedRank(QStringLiteral("Warp drive")), 0);   // unknown label: Unknown
        QVERIFY(!isConnectionType(QStringLiteral("Warp drive")));
        QCOMPARE(search::connectionText(QStringLiteral("Warp\ndrive 9000 Tbit/s!!")), QStringLiteral("Warpdrive 9000 ..."));
    }

    void connectionTypeMigration()
    {
        for (const char *old : {"14.4K", "28.8K", "33.6K", "56K", "64K ISDN", "128K ISDN", "T1"})
            QCOMPARE(migrateConnectionType(QString::fromLatin1(old)), QStringLiteral("DSL"));
        QCOMPARE(migrateConnectionType(QStringLiteral("Cable")), QStringLiteral("Cable"));
        QCOMPARE(migrateConnectionType(QStringLiteral("DSL")), QStringLiteral("DSL"));
        QCOMPARE(migrateConnectionType(QStringLiteral("T3 or Higher")), QStringLiteral("FTTH"));
        QCOMPARE(migrateConnectionType(QStringLiteral("T3 or faster")), QStringLiteral("FTTH"));
        // stored values of the old schema are coerced on read
        const SettingDef *def = findSetting(QString(keys::identity::Connection));
        QVERIFY(def);
        QCOMPARE(coerceSetting(*def, QStringLiteral("T3 or Higher")).toString(), QStringLiteral("FTTH"));
        QCOMPARE(coerceSetting(*def, QStringLiteral("56K")).toString(), QStringLiteral("DSL"));
        QCOMPARE(settingDefault(QString(keys::identity::Connection)).toString(), QStringLiteral("DSL"));
        QCOMPARE(coerceSetting(*findSetting(QString(keys::queue::ChangedSpeed)), QStringLiteral("14.4K")).toString(), QStringLiteral("DSL"));
        // a profile written by the old version: identity/connection = "T3 or Higher", old search filter text
        QTemporaryDir dir;
        QFile f(dir.filePath(QStringLiteral("settings.json")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"({"identity/connection":"T3 or Higher","queue/changedSpeed":"56K","search/defaultConnection":"T3 or faster"})");
        f.close();
        uib::App app;
        app.start(dir.path());
        QCOMPARE(app.core->settings()->s(keys::identity::Connection), QStringLiteral("FTTH"));
        QCOMPARE(app.core->settings()->s(keys::queue::ChangedSpeed), QStringLiteral("DSL"));
        QCOMPARE(app.core->settings()->s(keys::search::DefaultConnection), QStringLiteral("FTTH or faster"));
        app.stop();
    }

    // helper mode for fontFolderRendersIdentically (run as a child process)
    void printFontHash()
    {
        if (qEnvironmentVariableIsEmpty("QA1_CHILD"))
            QSKIP("child-process helper");
        // written to a file: under wine a child's stdout/stderr does not reach the pipe
        QFile out(qEnvironmentVariable("QA1_OUT"));
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(QStringLiteral("FONTHASH %1 %2\n").arg(uiFontSource(), QString::fromLatin1(renderHash())).toUtf8());
    }

    // Linux used to lay out multi-line labels with a 12-px pitch (FreeType leading -1); Windows: 13
    void lineSpacingIsThirteen()
    {
        const QFontMetrics fm(normalFont());
        QCOMPARE(fm.ascent(), 11);
        QCOMPARE(fm.descent(), 2);
        QCOMPARE(fm.height(), 13);
        QCOMPARE(fm.lineSpacing(), 13);
    }

    // the glyphs "1" and "J" follow genuine Windows Tahoma 8 pt (search_results_322_native.gif)
    void glyphsOneAndJ()
    {
        if (uiFontFamily() != QLatin1String("OctoMX Tahoma"))
            QSKIP("system Tahoma in use");
        const QFontMetrics fm(normalFont());
        QCOMPARE(fm.horizontalAdvance(QStringLiteral("J")), 5);
        // sits on the baseline (no descender) and has a top bar: ink rows 12..19 for baseline 20
        QImage img(20, 30, QImage::Format_RGB32);
        img.fill(Qt::white);
        QPainter p(&img);
        p.setPen(Qt::black);
        p.setFont(normalFont());
        p.drawText(2, 20, QStringLiteral("J"));
        p.end();
        int top = 99, bottom = -1, topWidth = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                if (qGray(img.pixel(x, y)) < 128) {
                    top = qMin(top, y);
                    bottom = qMax(bottom, y);
                }
        for (int x = 0; x < img.width(); ++x)
            topWidth += qGray(img.pixel(x, top)) < 128;
        QCOMPARE(bottom, 19);
        QCOMPARE(top, 12);
        QCOMPARE(topWidth, 3);
        QCOMPARE(fm.horizontalAdvance(QStringLiteral("1")), 6);
    }

    // a "fonts" folder of the installation is used instead of the embedded copy, with identical pixels
    void fontFolderRendersIdentically()
    {
        if (!qEnvironmentVariableIsEmpty("QA1_CHILD"))
            QSKIP("in the child");
        QCOMPARE(uiFontSource(), QStringLiteral(":/mx/fonts"));   // test binaries have no fonts folder
        const QByteArray embedded = renderHash();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QFile::copy(QStringLiteral(":/mx/fonts/tahoma.ttf"), dir.filePath(QStringLiteral("tahoma.ttf"))));
        QVERIFY(QFile::copy(QStringLiteral(":/mx/fonts/tahomabd.ttf"), dir.filePath(QStringLiteral("tahomabd.ttf"))));
        QProcess child;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QA1_CHILD"), QStringLiteral("1"));
        env.insert(QStringLiteral("MX_FONT_DIR"), dir.path());
        env.insert(QStringLiteral("QA1_OUT"), dir.filePath(QStringLiteral("hash.txt")));
        child.setProcessEnvironment(env);
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("printFontHash")});
        QVERIFY(child.waitForFinished(30000));
        QFile res(dir.filePath(QStringLiteral("hash.txt")));
        QVERIFY(res.open(QIODevice::ReadOnly));
        const QString out = QString::fromUtf8(res.readAll());
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("FONTHASH (\\S+) ([0-9a-f]+)")).match(out);
        QVERIFY2(m.hasMatch(), qPrintable(out.left(400)));
        QCOMPARE(QDir::cleanPath(m.captured(1)), QDir::cleanPath(dir.path()));
        QCOMPARE(m.captured(2).toLatin1(), embedded);
    }
};

QTEST_MAIN(TestQa1Fidelity)
#include "tst_qa1_fidelity.moc"
