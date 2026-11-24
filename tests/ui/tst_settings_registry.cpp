// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-h] Keeps the settings honest: every setting the user can see (a Settings page or the Advanced
// page = every schema entry that is not hidden) has an entry in settings_registry.h that names the
// behaviour test proving it, or the code that reads it, or why it is internal state. A key that
// nothing reads is hidden in the schema (its value stays readable for old profiles) and must then not
// come back on a Settings page. Every key bound on a page has a hand-written tool tip.
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>

#include "mock/mock_core.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/settings_binding.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/settings_pages.h"
#include "ui/kit/colors.h"
#include "settings_registry.h"

using namespace mx;
using namespace mx::ui;

namespace {

QString source(const QString &rel)
{
    QFile f(QStringLiteral(MX_SOURCE_DIR) + QLatin1Char('/') + rel);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// key -> "ns::Name" from settings_keys.h, key -> "ckeys::Alias" from colors.h
QHash<QString, QStringList> spellings()
{
    QHash<QString, QStringList> out;
    static const QRegularExpression ns(QStringLiteral("^\\s*namespace\\s+(\\w+)\\s*\\{"));
    static const QRegularExpression k(QStringLiteral("inline constexpr K (\\w+)\\{\"([^\"]+)\"\\}"));
    QString current;
    for (const QString &line : source(QStringLiteral("src/api/mx/settings_keys.h")).split(QLatin1Char('\n'))) {
        const auto n = ns.match(line);
        if (n.hasMatch())
            current = n.captured(1);
        const auto m = k.match(line);
        if (m.hasMatch())
            out[m.captured(2)] << current + QStringLiteral("::") + m.captured(1);
    }
    for (const QString &line : source(QStringLiteral("src/ui/kit/colors.h")).split(QLatin1Char('\n'))) {
        const auto m = k.match(line);
        if (m.hasMatch())
            out[m.captured(2)] << QStringLiteral("ckeys::") + m.captured(1);
    }
    return out;
}

} // namespace

class TstSettingsRegistry : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

private slots:
    void everyVisibleKeyHasAProof()
    {
        QSet<QString> visible, hidden;
        for (const SettingDef &d : settingsSchema())
            (d.hidden ? hidden : visible).insert(d.key);
        QSet<QString> listed;
        QStringList errors;
        for (const mxreg::Proof &p : mxreg::kProofs) {
            const QString key = QString::fromLatin1(p.key);
            listed.insert(key);
            if (hidden.contains(key))
                errors << QStringLiteral("%1: hidden in the schema (off the UI) but still in the registry").arg(key);
            else if (!visible.contains(key))
                errors << QStringLiteral("%1: not in the schema any more").arg(key);
        }
        for (const QString &k : std::as_const(visible))
            if (!listed.contains(k))
                errors << QStringLiteral("%1: visible (Settings or Advanced page) but no proof in settings_registry.h; "
                                         "add the test that shows what it does, or hide it in the schema").arg(k);
        errors.sort();
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QVERIFY(visible.size() > 300);
    }

    void proofsPointAtRealCode()
    {
        const QHash<QString, QStringList> names = spellings();
        QHash<QString, QString> cache;
        auto text = [&cache](const QString &rel) -> const QString & {
            auto it = cache.find(rel);
            if (it == cache.end())
                it = cache.insert(rel, source(rel));
            return it.value();
        };
        static const QRegularExpression selfRef(QStringLiteral("(settings_schema\\.cpp|settings_keys\\.h|dialogs/settings_|settings_tips|src/mock/)"));
        QStringList errors;
        int tests = 0, readers = 0, internal = 0;
        for (const mxreg::Proof &p : mxreg::kProofs) {
            const QString key = QString::fromLatin1(p.key), kind = QString::fromLatin1(p.kind);
            const QString file = QString::fromLatin1(p.file), what = QString::fromLatin1(p.what);
            QStringList refs = names.value(key);
            refs << QLatin1Char('"') + key + QLatin1Char('"');
            if (key.endsWith(QLatin1String("/*")))
                refs << QLatin1Char('"') + key.left(key.size() - 1);
            if (kind == QLatin1String("internal")) {
                ++internal;
                if (QString::fromLatin1(p.why).trimmed().isEmpty())
                    errors << QStringLiteral("%1: internal without a reason").arg(key);
                if (file.isEmpty())
                    continue;
            }
            const QString &t = text(file);
            if (t.isEmpty()) {
                errors << QStringLiteral("%1: %2 not found").arg(key, file);
                continue;
            }
            if (!t.contains(what)) {
                errors << QStringLiteral("%1: %2 does not contain \"%3\" any more").arg(key, file, what);
                continue;
            }
            if (kind == QLatin1String("test")) {
                ++tests;
                if (!file.startsWith(QLatin1String("tests/")) || !t.contains(QStringLiteral("void %1(").arg(what)))
                    errors << QStringLiteral("%1: %2 is not a test function in %3").arg(key, what, file);
                bool mentions = false;
                for (const QString &r : std::as_const(refs))
                    mentions |= t.contains(r);
                if (!mentions)
                    errors << QStringLiteral("%1: the test file %2 never names the key").arg(key, file);
            } else if (kind == QLatin1String("reader") || kind == QLatin1String("internal")) {
                readers += kind == QLatin1String("reader");
                if (!file.startsWith(QLatin1String("src/")) || selfRef.match(file).hasMatch())
                    errors << QStringLiteral("%1: %2 is not a reader (the schema, the Settings pages and the demo core do not count)").arg(key, file);
                bool isRef = false;
                for (const QString &r : std::as_const(refs))
                    isRef |= what == r || what.startsWith(r);
                if (!isRef && !what.endsWith(QLatin1String("Key(")))
                    errors << QStringLiteral("%1: \"%2\" in %3 is not a reference to the key").arg(key, what, file);
            } else {
                errors << QStringLiteral("%1: unknown kind %2").arg(key, kind);
            }
        }
        qInfo("registry: %d behaviour tests, %d readers (static smoke check), %d internal", tests, readers, internal);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    }

    // [set-1] the settings of these groups are proven by a behaviour test each (no "reader" smoke
    // check), never by a test that only stores, migrates or round-trips the value
    void ownedGroupsHaveBehaviourTests()
    {
        static const QStringList groups = {"identity", "ui", "colors", "chat", "messages", "hotlist", "library", "search",
                                           "transfers", "queue", "launch", "misc", "diag", "updates", "other"};
        // test functions that store, migrate, coerce or reset a value: they prove no effect
        static const QStringList weak = {"schemaIsConsistent", "coercion", "jsonRoundTrip", "debouncedSave", "roundTrip",
                                         "resetDefaults", "hiddenSchemaEntries", "schemaHasTheBatch", "specDefaults",
                                         "legacyKeysMigrated", "profileMigratedOnce", "connectionTypeMigration",
                                         "searchPageWritesCanonicalKeys", "defaultSummaryParsesTitle", "wizardRecommended",
                                         "connectionDefaultsFollowTheType", "firstRun", "poll", "create", "moveTorrentStorage"};
        QStringList errors;
        int n = 0;
        for (const mxreg::Proof &p : mxreg::kProofs) {
            const QString key = QString::fromLatin1(p.key), kind = QString::fromLatin1(p.kind);
            if (!groups.contains(key.section(QLatin1Char('/'), 0, 0)) || kind == QLatin1String("internal"))
                continue;
            ++n;
            if (kind != QLatin1String("test"))
                errors << QStringLiteral("%1: only a reader proof; add a behaviour test").arg(key);
            else if (weak.contains(QString::fromLatin1(p.what)))
                errors << QStringLiteral("%1: %2 only stores or migrates the value").arg(key, QString::fromLatin1(p.what));
        }
        errors.sort();
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QVERIFY(n > 240);
    }

    // a key taken off the UI stays off it; every key of a page has a hand-written tool tip
    void hiddenKeysStayOffThePages()
    {
        QVERIFY(m_dir.isValid());
        Core *core = createMockCore(m_dir.filePath(QStringLiteral("p")));
        Colors::instance()->attach(core->settings());
        {
            SettingsDialog d(core, nullptr);
            QStringList errors;
            QSet<QString> bound;
            QHash<QString, QStringList> tips;   // key -> tool tips of the widgets bound to it
            for (QWidget *w : d.findChildren<QWidget *>()) {
                const QString keys = w->property(kKeyProp).toString();
                if (keys.isEmpty())
                    continue;
                const bool advanced = w->property(kAdvancedProp).toBool();
                for (const QString &k : keys.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                    const SettingDef *def = findSetting(k);
                    if (def && def->hidden)
                        errors << QStringLiteral("%1 (%2) is hidden in the schema but bound on a Settings page").arg(k, w->objectName());
                    if (!advanced) {
                        bound.insert(k);
                        tips[k] << w->toolTip();
                    }
                }
            }
            // a hand-written tip in settings_tips.cpp, or one the page sets itself (colour buttons, the
            // speed limit rows); never only the schema label the fallback makes of it
            for (const QString &k : std::as_const(bound)) {
                if (hasSettingTip(k))
                    continue;
                const QString fallback = settingTip(k);   // the schema label + the default
                for (const QString &t : tips.value(k))
                    if (t.trimmed().isEmpty() || t == fallback)
                        errors << QStringLiteral("%1: bound on a Settings page with no tool tip of its own (add one to settings_tips.cpp)").arg(k);
            }
            errors.sort();
            QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
            QVERIFY(bound.size() > 200);
            // MX_REGISTRY_TABLE=<file>: key <tab> Settings page(s) <tab> kind <tab> file <tab> what (status table)
            const QString out = qEnvironmentVariable("MX_REGISTRY_TABLE");
            if (!out.isEmpty()) {
                QHash<QString, QStringList> where;
                for (const QString &id : d.pageIds())
                    for (const QString &k : d.binder()->keysOf(d.pageWidget(id)))
                        if (id != QLatin1String("advanced") && !where[k].contains(id))
                            where[k] << id;
                QFile f(out);
                QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
                for (const mxreg::Proof &p : mxreg::kProofs) {
                    const QString k = QString::fromLatin1(p.key);
                    const QString w = where.value(k).isEmpty() ? QStringLiteral("advanced") : where.value(k).join(QLatin1Char(' '));
                    f.write(QStringLiteral("%1\t%2\t%3\t%4\t%5\n").arg(k, w, QString::fromLatin1(p.kind), QString::fromLatin1(p.file),
                                                                  QString::fromLatin1(p.what)).toUtf8());
                }
            }
        }
        Colors::instance()->attach(nullptr);
        delete core;
    }
};

QTEST_MAIN(TstSettingsRegistry)
#include "tst_settings_registry.moc"
