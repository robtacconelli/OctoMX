// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest/QtTest>

#include "core/json_settings.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QSignalSpy>
#include <QtCore/QTemporaryDir>

using namespace mx;

class TestSettings : public QObject {
    Q_OBJECT
private slots:
    void schemaIsConsistent()
    {
        const QList<SettingDef> &schema = settingsSchema();
        QVERIFY(schema.size() > 200);
        QSet<QString> seen;
        for (const SettingDef &d : schema) {
            QVERIFY2(!seen.contains(d.key), qPrintable(QStringLiteral("duplicate key ") + d.key));
            seen.insert(d.key);
            QVERIFY2(!d.label.isEmpty(), qPrintable(d.key));
            bool ok = false;
            const QVariant c = coerceSetting(d, d.defaultValue, &ok);
            if (d.type == SettingType::Bytes || d.type == SettingType::Map) continue; // empty defaults
            QVERIFY2(ok, qPrintable(QStringLiteral("default does not coerce: ") + d.key));
            QCOMPARE(c, d.defaultValue);
            if (d.type == SettingType::Enum) QVERIFY(d.choices.contains(d.defaultValue.toString()));
            if (d.type == SettingType::Enum && !d.choiceLabels.isEmpty()) QCOMPARE(d.choiceLabels.size(), d.choices.size());
        }
        // every network has an enable flag
        for (int i = int(Net::Unknown) + 1; i <= int(Net::Rats); ++i) {
            const Net n = Net(i);
            QVERIFY2(!settingsPrefixForNet(n).isEmpty(), qPrintable(netName(n)));
            QVERIFY2(findSetting(keys::enabledKey(n)), qPrintable(keys::enabledKey(n)));
        }
        // keys requested by the plugin workers (glue batch 1)
        for (const QString &k : {QString(keys::web::UserAgent), QString(keys::web::ProviderEnabled),
                                 QString(keys::web::AdultKeywords), QString(keys::web::TimeoutSec),
                                 QString(keys::web::ProbeOnStart), QString(keys::web::Trackers),
                                 QString(keys::http::ConnectionsPerSource), QString(keys::http::AllowInsecureRedirect),
                                 QString(keys::http::StallTimeoutSec), QString(keys::http::MaxRetries),
                                 QString(keys::http::RetryBaseMs), QString(keys::rats::Enabled),
                                 QString(keys::rats::Protocol), QString(keys::rats::ListenPort),
                                 QString(keys::rats::DhtPort), QString(keys::rats::SafeSearch),
                                 QString(keys::rats::MaxPeers), QString(keys::bt::AllowMultipleConnectionsPerIp),
                                 QString(keys::bt::ValidateHttpsTrackers)})
            QVERIFY2(findSetting(k), qPrintable(k));
        QCOMPARE(settingDefault(keys::rats::ListenPort).toInt(), 4444);
        QCOMPARE(settingDefault(keys::rats::Protocol).toString(), QStringLiteral("rats-search/4"));
        QCOMPARE(settingDefault(keys::web::TimeoutSec).toInt(), 25);
        QCOMPARE(settingDefault(keys::http::RetryBaseMs).toInt(), 1000);
        QCOMPARE(settingDefault(keys::bt::ValidateHttpsTrackers).toBool(), true);
        QCOMPARE(findSetting(keys::http::StallTimeoutSec)->max.toInt(), 600);
        // a sample of the constants resolve
        for (const QString &k : {QString(keys::identity::ScreenName), QString(keys::bt::SeedLibrary),
                                 QString(keys::colors::ChatBackground), QString(keys::transfers::ShareCompleted),
                                 QString(keys::search::TimeoutSec), QString(keys::misc::CloseAction),
                                 QString(keys::bandwidth::ScheduleDays), QString(keys::ui::LastPage)})
            QVERIFY2(findSetting(k), qPrintable(k));
        QCOMPARE(settingDefault(keys::colors::ListBackground).toString(), QStringLiteral("#000000"));
        QCOMPARE(settingDefault(keys::connection::TcpPort).toInt(), 6699);
        QCOMPARE(connectionTypes().size(), 13);   // [qa-1] owner request: modern connection types
        const QVariantMap dark = colorSchemePreset(false), light = colorSchemePreset(true);
        QCOMPARE(dark.value(QString(keys::colors::ListBackground)).toString(), QStringLiteral("#000000"));
        QCOMPARE(light.value(QString(keys::colors::ListBackground)).toString(), QStringLiteral("#ffffff"));
        QCOMPARE(light.value(QString(keys::colors::ListText)).toString(), QStringLiteral("#000000"));
        QCOMPARE(dark.size(), light.size());
        QVERIFY(dark.size() > 70);
        QCOMPARE(settingsPrefixForNet(Net::Rats), QStringLiteral("rats"));
        QVERIFY(findSetting(keys::enabledKey(Net::Rats)));
    }

    void wildcards()
    {
        const SettingDef *d = findSetting(keys::ui::columnsKey(QStringLiteral("search")));
        QVERIFY(d);
        QCOMPARE(d->type, SettingType::Map);
        QVERIFY(findSetting(keys::ui::splitterKey(QStringLiteral("transfers"))));
        QVERIFY(!findSetting(QStringLiteral("ui/columns/")));
        QVERIFY(!findSetting(QStringLiteral("no/such/key")));
    }

    void coercion()
    {
        const SettingDef *maxDl = findSetting(keys::transfers::MaxDownloads);
        bool ok = false;
        QCOMPARE(coerceSetting(*maxDl, QStringLiteral("25"), &ok).toInt(), 25);
        QVERIFY(ok);
        QCOMPARE(coerceSetting(*maxDl, 100000, &ok).toInt(), maxDl->max.toInt());   // clamped
        QCOMPARE(coerceSetting(*maxDl, -5, &ok).toInt(), maxDl->min.toInt());
        QCOMPARE(coerceSetting(*maxDl, 7.6, &ok).toInt(), 8);
        coerceSetting(*maxDl, QStringLiteral("abc"), &ok);
        QVERIFY(!ok);

        const SettingDef *b = findSetting(keys::misc::StartMinimized);
        QCOMPARE(coerceSetting(*b, QStringLiteral("yes"), &ok).toBool(), true);
        QCOMPARE(coerceSetting(*b, QStringLiteral("0"), &ok).toBool(), false);
        QCOMPARE(coerceSetting(*b, 1, &ok).toBool(), true);

        const SettingDef *e = findSetting(keys::bt::Encryption);
        QCOMPARE(coerceSetting(*e, QStringLiteral("FORCED"), &ok).toString(), QStringLiteral("forced"));
        QCOMPARE(coerceSetting(*e, 2, &ok).toString(), QStringLiteral("disabled"));
        coerceSetting(*e, QStringLiteral("bogus"), &ok);
        QVERIFY(!ok);

        const SettingDef *c = findSetting(keys::colors::ChatText);
        QCOMPARE(coerceSetting(*c, QStringLiteral("FF8800"), &ok).toString(), QStringLiteral("#ff8800"));
        QCOMPARE(coerceSetting(*c, QStringLiteral("#abc"), &ok).toString(), QStringLiteral("#aabbcc"));
        coerceSetting(*c, QStringLiteral("#12345g"), &ok);
        QVERIFY(!ok);

        const SettingDef *d = findSetting(keys::bt::SeedRatioLimit);
        QCOMPARE(coerceSetting(*d, QStringLiteral("1.5"), &ok).toDouble(), 1.5);
        QCOMPARE(coerceSetting(*d, 1e9, &ok).toDouble(), d->max.toDouble());

        const SettingDef *sl = findSetting(keys::bt::ExtraTrackers);
        QCOMPARE(coerceSetting(*sl, QStringLiteral("udp://a"), &ok).toStringList(), QStringList{QStringLiteral("udp://a")});

        const SettingDef *bytes = findSetting(keys::ui::Geometry);
        const QByteArray raw("\x01\x02\xff", 3);
        QCOMPARE(coerceSetting(*bytes, QString::fromLatin1(raw.toBase64()), &ok).toByteArray(), raw);
        QCOMPARE(settingToJson(bytes, raw).toString(), QString::fromLatin1(raw.toBase64()));
    }

    void simpleSettings()
    {
        SimpleSettings s;
        QSignalSpy spy(&s, &Settings::changed);
        QCOMPARE(s.i(keys::transfers::MaxDownloads), 40);
        s.setValue(keys::transfers::MaxDownloads, 12);
        QCOMPARE(s.i(keys::transfers::MaxDownloads), 12);
        QCOMPARE(spy.count(), 1);
        s.setValue(keys::transfers::MaxDownloads, 12);          // unchanged: no signal
        QCOMPARE(spy.count(), 1);
        s.setValue(keys::transfers::MaxDownloads, QStringLiteral("junk")); // rejected
        QCOMPARE(s.i(keys::transfers::MaxDownloads), 12);
        QCOMPARE(spy.count(), 1);
        s.setValue(keys::transfers::MaxDownloads, 99999);        // clamped
        QCOMPARE(s.i(keys::transfers::MaxDownloads), 500);
        QCOMPARE(spy.last().at(1).toInt(), 500);
        s.setValue(QStringLiteral("x/unknown"), QStringLiteral("kept"));
        QCOMPARE(s.s(QStringLiteral("x/unknown")), QStringLiteral("kept"));
        QVERIFY(s.keys(QStringLiteral("x/")).contains(QStringLiteral("x/unknown")));
        QVERIFY(s.keys(QStringLiteral("bt/")).contains(QString(keys::bt::Dht)));

        // runtime default for empty paths
        s.setRuntimeDefault(keys::transfers::IncomingDir, QStringLiteral("/data/in"));
        QCOMPARE(s.s(keys::transfers::IncomingDir), QStringLiteral("/data/in"));
        s.setValue(keys::transfers::IncomingDir, QString());
        QCOMPARE(s.s(keys::transfers::IncomingDir), QStringLiteral("/data/in"));
        s.setValue(keys::transfers::IncomingDir, QStringLiteral("/mine"));
        QCOMPARE(s.s(keys::transfers::IncomingDir), QStringLiteral("/mine"));

        spy.clear();
        s.resetToDefaults(QStringLiteral("transfers/"));
        QCOMPARE(s.i(keys::transfers::MaxDownloads), 40);
        QCOMPARE(s.s(keys::transfers::IncomingDir), QStringLiteral("/data/in"));
        QCOMPARE(spy.count(), 2);
        QCOMPARE(s.s(QStringLiteral("x/unknown")), QStringLiteral("kept"));
    }

    void jsonRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("settings.json"));
        {
            JsonSettings s(path);
            QVERIFY(s.load());
            s.setValue(keys::identity::ScreenName, QStringLiteral("Quill233_25471"));
            s.setValue(keys::bandwidth::UpLimitKBs, 40);
            s.setValue(keys::ui::Geometry, QByteArray("\x00\x01geom", 6));
            s.setValue(keys::ui::columnsKey(QStringLiteral("search")),
                       QVariantMap{{QStringLiteral("widths"), QVariantList{100, 50}}});
            s.setValue(keys::bt::ExtraTrackers, QStringList{QStringLiteral("udp://x:1")});
            s.sync();
        }
        // unknown key written by a newer version is preserved
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            QCOMPARE(o.value(QString(keys::identity::ScreenName)).toString(), QStringLiteral("Quill233_25471"));
            o.insert(QStringLiteral("future/feature"), 42);
            o.insert(QString(keys::transfers::MaxUploads), 100000); // out of range on disk → clamped on load
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QJsonDocument(o).toJson());
        }
        {
            JsonSettings s(path);
            QVERIFY(s.load());
            QCOMPARE(s.s(keys::identity::ScreenName), QStringLiteral("Quill233_25471"));
            QCOMPARE(s.i(keys::bandwidth::UpLimitKBs), 40);
            QCOMPARE(s.value(keys::ui::Geometry).toByteArray(), QByteArray("\x00\x01geom", 6));
            QCOMPARE(s.value(keys::ui::columnsKey(QStringLiteral("search"))).toMap().value(QStringLiteral("widths")).toList().size(), 2);
            QCOMPARE(s.sl(keys::bt::ExtraTrackers), QStringList{QStringLiteral("udp://x:1")});
            QCOMPARE(s.i(keys::transfers::MaxUploads), 500);
            QCOMPARE(s.i(QStringLiteral("future/feature")), 42);
            s.setValue(keys::misc::Sounds, false);
            s.sync();
        }
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            QCOMPARE(o.value(QStringLiteral("future/feature")).toInt(), 42);
            QCOMPARE(o.value(QString(keys::misc::Sounds)).toBool(true), false);
        }
    }

    void debouncedSave()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("settings.json"));
        JsonSettings s(path);
        s.setSaveDelay(100);
        s.load();
        s.setValue(keys::search::MaxResults, 500);
        QVERIFY(!QFile::exists(path));            // not written synchronously
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path), 2000);
        JsonSettings other(path);
        other.load();
        QCOMPARE(other.i(keys::search::MaxResults), 500);
    }

    void corruptFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("settings.json"));
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("{ this is not json");
        }
        JsonSettings s(path);
        QVERIFY(!s.load());
        QVERIFY(QFile::exists(path + QStringLiteral(".bad")));
        QCOMPARE(s.i(keys::transfers::MaxDownloads), 40);
    }
};

QTEST_GUILESS_MAIN(TestSettings)
#include "tst_settings.moc"
