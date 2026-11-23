// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-c] Settings dialog tests: every schema key reachable (bound on a page or listed on Advanced),
// UI -> Settings -> reopen round trip for every bound control, validation, restart notice,
// openSettings(page) mapping, Reset Defaults, live colours, the Setup Wizard paths.
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTreeWidget>

#include "mock/mock_core.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/settings_binding.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/setup_wizard.h"
#include "ui/kit/colors.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/main_window.h"

using namespace mx;
using namespace mx::ui;

namespace {

template <class D> auto hiddenOf(const D &d, int) -> decltype(bool(d.hidden)) { return d.hidden; }
template <class D> bool hiddenOf(const D &, long) { return false; }

QStringList keysOf(const QWidget *w)
{
    return w->property(kKeyProp).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
}

QString text(const QVariant &v)
{
    if (v.typeId() == QMetaType::QStringList)
        return SettingsBinder::listToText(v.toStringList());
    if (v.typeId() == QMetaType::QVariantMap)
        return SettingsBinder::mapToText(v.toMap());
    if (v.typeId() == QMetaType::Bool)
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return v.toString();
}

bool sameValue(const QVariant &a, const QVariant &b)
{
    if (a.typeId() == QMetaType::Bool || b.typeId() == QMetaType::Bool)
        return a.toBool() == b.toBool();
    bool na = false, nb = false;
    const double da = a.toDouble(&na), db = b.toDouble(&nb);
    if (na && nb && a.typeId() != QMetaType::QString)
        return qFuzzyCompare(da + 1, db + 1);
    return text(a).compare(text(b), Qt::CaseInsensitive) == 0;
}

// all bound widgets of the dialog (every page, including Advanced)
QList<QWidget *> boundWidgets(SettingsDialog *d)
{
    QList<QWidget *> out;
    for (QWidget *w : d->findChildren<QWidget *>())
        if (w->property(kKeyProp).isValid() && !keysOf(w).isEmpty())
            out << w;
    return out;
}

} // namespace

class TstSettingsDialog : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    Core *m_core = nullptr;
    MainWindow *m_mw = nullptr;
    QStringList m_messages;
    int m_askAnswer = QMessageBox::No;
    int m_seq = 0;

    SettingsDialog *open(const QString &page = QString())
    {
        m_mw->openSettings(page);
        auto *d = m_mw->findChild<SettingsDialog *>();
        return d;
    }
    void closeDialog(SettingsDialog *d)
    {
        QPointer<SettingsDialog> p(d);
        d->close();
        QTRY_VERIFY(p.isNull());   // deleted (deleteLater) -> the next open is a fresh dialog
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        MockOptions o;
        o.frozen = true;
        m_core = createMockCoreWithOptions(m_dir.filePath(QStringLiteral("profile")), o);
        Colors::instance()->attach(m_core->settings());
        m_core->start();
        m_mw = new MainWindow(m_core);
        m_mw->resize(1024, 689);
        m_mw->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_mw));
        SettingsBinder::setMessageHook([this](QWidget *, const QString &, const QString &t) { m_messages << t; });
        SettingsBinder::setAskHook([this](QWidget *, const QString &, const QString &, int, int) { return m_askAnswer; });
        ColorButton::setPickHook([this](const QColor &c, QWidget *) {
            ++m_seq;
            return QColor((c.red() + 37 + m_seq) % 256, (c.green() + 91) % 256, (c.blue() + 13) % 256);
        });
        PathField::setBrowseHook([this](const QString &, bool dir, QWidget *) {
            const QString p = m_dir.filePath(QStringLiteral("pick%1%2").arg(++m_seq).arg(dir ? QString() : QStringLiteral(".wav")));
            if (dir)
                QDir().mkpath(p);
            return p;
        });
        StringListEditor::setPromptHook([this](const QString &, const QString &, QWidget *) {
            return QStringLiteral("Test Type %1 (.t%1)").arg(++m_seq);
        });
    }

    void sizeAndChrome()
    {
        SettingsDialog *d = open();
        QVERIFY(d);
        QCOMPARE(d->size(), QSize(527, 351));   // = 533 x 376 outer with the classic frame
        QCOMPARE(d->windowTitle(), QStringLiteral("Settings"));
        QVERIFY(d->findChild<QAbstractButton *>(QStringLiteral("settingsWizard")));
        QVERIFY(d->findChild<QAbstractButton *>(QStringLiteral("settingsClose")));
        // the original tree (relabelled per UI_SPEC §8.9) in the original order
        const QStringList original = {QStringLiteral("Appearance"), QStringLiteral("Internet Connection"),
                                      QStringLiteral("OctoMX Peer Network"), QStringLiteral("Other Networks"),
                                      QStringLiteral("Incoming Msgs & Privacy"), QStringLiteral("Library"),
                                      QStringLiteral("Channels"), QStringLiteral("Search"), QStringLiteral("File Transfers"),
                                      QStringLiteral("Bandwidth Throttle"), QStringLiteral("File Launching"),
                                      QStringLiteral("Miscellaneous")};
        QStringList top;
        for (int i = 0; i < d->tree()->topLevelItemCount(); ++i)
            top << d->tree()->topLevelItem(i)->text(0);
        QCOMPARE(top.mid(0, original.size()), original);
        QCOMPARE(top.last(), QStringLiteral("Advanced"));
        // [ui-h] "In/Out UDP Packets" and "Host Name Resolution" are gone: nothing read their settings
        for (const char *sub : {"Outgoing TCP Connections", "Incoming TCP Connections", "Queuing", "Channel Window",
                                "Bandwidth Window"})
            QVERIFY2(!d->resolvePage(QString::fromLatin1(sub)).isEmpty(), sub);
        for (const char *gone : {"In/Out UDP Packets", "Host Name Resolution"})
            QVERIFY2(d->resolvePage(QString::fromLatin1(gone)).isEmpty(), gone);
        closeDialog(d);
    }

    // every key of mx::settingsSchema() is reachable: bound on a page or listed on Advanced
    void completeness()
    {
        SettingsDialog *d = open();
        QSet<QString> onPages, onAdvanced;
        QWidget *adv = d->pageWidget(QStringLiteral("advanced"));
        QVERIFY(adv);
        for (QWidget *w : boundWidgets(d)) {
            for (const QString &k : keysOf(w))
                (adv->isAncestorOf(w) ? onAdvanced : onPages).insert(k);
        }
        int schemaKeys = 0, pageCount = 0, advCount = 0;
        QStringList missing;
        for (const SettingDef &def : settingsSchema()) {
            if (hiddenOf(def, 0))
                continue;   // internal switches are deliberately not shown
            ++schemaKeys;
            const bool p = onPages.contains(def.key), a = onAdvanced.contains(def.key);
            if (p)
                ++pageCount;
            if (a)
                ++advCount;
            if (!p && !a)
                missing << def.key;
            QVERIFY2(!(p && a), qPrintable(def.key + QStringLiteral(" is on a page AND on Advanced")));
        }
        QVERIFY2(missing.isEmpty(), qPrintable(QStringLiteral("not reachable: ") + missing.join(QStringLiteral(", "))));
        // data-driven: the Advanced page lists exactly the schema keys the pages do not bind
        QCOMPARE(advCount, schemaKeys - pageCount);
        QCOMPARE(d->advancedKeys().size(), advCount);
        qInfo("schema keys: %d, bound on pages: %d, on Advanced: %d (extra UI-side keys bound: %d)", schemaKeys, pageCount,
              advCount, int(onPages.size()) - pageCount);
        closeDialog(d);
    }

    // UI control -> Settings -> reopen -> control shows it, for every bound control (every type)
    void roundTrip()
    {
        Settings *s = m_core->settings();
        SettingsDialog *d = open();
        QSet<QString> classes;
        int mutated = 0;
        for (QWidget *w : boundWidgets(d)) {
            const QStringList keys = keysOf(w);
            const QString key = keys.first();
            const SettingDef *def = findSetting(key);
            const SettingType type = def ? def->type : SettingType::String;
            const QString cls = QString::fromLatin1(w->metaObject()->className());
            const QString what = QStringLiteral("%1 (%2)").arg(key, cls);
            QVariant want;   // the value the UI action must store in `key`
            if (key.endsWith(QLatin1String("/*")))
                continue;   // a family counter (display only)
            if (auto *bw = dynamic_cast<BoundWidget *>(w)) {
                if (auto *cb = qobject_cast<ColorButton *>(w)) {
                    cb->click();   // through the colour dialog hook
                    want = cb->color().name();
                } else if (auto *pf = qobject_cast<PathField *>(w)) {
                    pf->button()->click();
                    want = pf->boundValue();
                } else if (auto *le = qobject_cast<StringListEditor *>(w)) {
                    le->findChild<QAbstractButton *>(QStringLiteral("listNew"))->click();
                    want = le->boundValue();
                    QVERIFY(want.toStringList().last().startsWith(QStringLiteral("Test Type")));
                } else {
                    // generic: change the value according to its type
                    QVariant v = bw->boundValue();
                    if (type == SettingType::Int)
                        v = v.toInt() ^ 1;
                    else if (type == SettingType::Map) {
                        QVariantMap m = v.toMap();
                        QVariantList l = m.value(QStringLiteral("entries")).toList();
                        l << QVariantMap{{QStringLiteral("title"), QStringLiteral("Test %1").arg(++m_seq)}, {QStringLiteral("ext"), QStringLiteral("t")}};
                        m.insert(QStringLiteral("entries"), l);
                        v = m;
                    } else
                        v = QStringLiteral("Test,%1").arg(++m_seq);
                    bw->setValueByUser(v);
                    want = bw->boundValue();
                }
                classes << QStringLiteral("BoundWidget:") + cls;
            } else if (auto *mb = qobject_cast<MenuButton *>(w)) {
                QAction *pick = nullptr;
                for (QAction *a : mb->menu()->findChildren<QAction *>())
                    if (a->isCheckable() && a->data().isValid() && a->data() != mb->currentData())
                        pick = a;
                QVERIFY2(pick, qPrintable(what));
                pick->trigger();
                want = pick->data();
                classes << QStringLiteral("MenuButton");
            } else if (auto *r = qobject_cast<QRadioButton *>(w)) {
                if (r->isChecked())
                    continue;   // another radio of the group is tested
                r->setChecked(true);
                want = r->property(kValueProp);
                classes << QStringLiteral("QRadioButton");
            } else if (auto *cb = qobject_cast<QComboBox *>(w)) {
                const int i = (cb->currentIndex() + 1) % cb->count();
                cb->setCurrentIndex(i);
                emit cb->activated(i);
                want = cb->itemData(i);
                classes << QStringLiteral("QComboBox");
            } else if (auto *sb = qobject_cast<QSpinBox *>(w)) {
                const int v = sb->value() < sb->maximum() ? sb->value() + 1 : sb->value() - 1;
                sb->setValue(v);
                want = v;
                classes << QStringLiteral("QSpinBox");
            } else if (auto *ds = qobject_cast<QDoubleSpinBox *>(w)) {
                const double v = ds->value() < ds->maximum() ? ds->value() + 0.5 : ds->value() - 0.5;
                ds->setValue(v);
                want = ds->value();
                classes << QStringLiteral("QDoubleSpinBox");
            } else if (auto *le = qobject_cast<QLineEdit *>(w)) {
                if (le->isReadOnly())
                    continue;   // Bytes (window geometry): shown, not edited
                QString t;
                if (keys.contains(QLatin1String("connection/proxyHost"))) {
                    t = QStringLiteral("10.0.0.%1:8081").arg(++m_seq % 200);
                    le->setText(t);
                    emit le->editingFinished();
                    QCOMPARE(s->s(QStringLiteral("connection/proxyHost")), t.section(QLatin1Char(':'), 0, 0));
                    QCOMPARE(s->i(QStringLiteral("connection/proxyPort")), 8081);
                    ++mutated;
                    classes << QStringLiteral("QLineEdit(composite)");
                    continue;
                }
                if (!le->inputMask().isEmpty())
                    t = QStringLiteral("07:%1").arg(10 + (++m_seq % 40));
                else if (type == SettingType::Int) {
                    const qint64 mn = def->min.toLongLong(), mx = def->max.toLongLong();
                    qint64 v = s->i64(key) + 1;
                    if (v > mx)
                        v = mn;
                    t = QString::number(v);
                } else if (type == SettingType::Double)
                    t = QString::number(s->value(key).toDouble() + 0.5);
                else if (type == SettingType::StringList)
                    t = QStringLiteral("a%1; b%1").arg(++m_seq);
                else if (type == SettingType::Map)
                    t = QStringLiteral("{\"test%1\":true}").arg(++m_seq);
                else if (type == SettingType::Color)
                    t = QStringLiteral("#12345%1").arg(++m_seq % 10);
                else if (type == SettingType::Enum)
                    t = def->choices.last();
                else if (type == SettingType::Path)
                    t = m_dir.filePath(QStringLiteral("p%1").arg(++m_seq));
                else
                    t = QStringLiteral("rt%1").arg(++m_seq);
                le->setText(t);
                emit le->editingFinished();
                want = type == SettingType::StringList ? QVariant(SettingsBinder::textToList(t))
                       : type == SettingType::Map      ? QVariant(t)
                                                       : QVariant(t);
                if (type == SettingType::Map) {
                    QVariantMap m;
                    QVERIFY(SettingsBinder::textToMap(t, &m));
                    want = m;
                }
                classes << QStringLiteral("QLineEdit");
                if (keys.contains(QLatin1String("connection/proxyAuth")))
                    QVERIFY(s->b(QStringLiteral("connection/proxyAuth")));
            } else if (auto *ab = qobject_cast<QAbstractButton *>(w); ab && ab->isCheckable()) {
                ab->setChecked(!ab->isChecked());
                // composites (UPnP + NAT-PMP, bandwidth limit on/off): every key follows the box
                for (const QString &k : keys)
                    QVERIFY2(s->value(k).toBool() == ab->isChecked(), qPrintable(what + QStringLiteral(" -> ") + k));
                ++mutated;
                classes << QStringLiteral("checkable ") + cls;
                continue;
            } else if (auto *ab = qobject_cast<QAbstractButton *>(w); ab && w->property(kValueProp).isValid()) {
                ab->click();
                want = w->property(kValueProp);
                classes << QStringLiteral("value button");
            } else if (qobject_cast<StaticText *>(w)) {
                continue;   // display of a composite (alert text)
            } else {
                QFAIL(qPrintable(QStringLiteral("no round-trip rule for bound control ") + what));
            }
            QVERIFY2(sameValue(s->value(key), want),
                     qPrintable(QStringLiteral("%1: stored '%2', expected '%3'").arg(what, text(s->value(key)), text(want))));
            ++mutated;
        }
        QVERIFY2(m_messages.isEmpty() || !m_messages.join(QString()).contains(QStringLiteral("valid")), qPrintable(m_messages.join(QStringLiteral(" | "))));
        m_messages.clear();
        qInfo("round trip: %d controls changed; control kinds: %s", mutated, qPrintable(QStringList(classes.values()).join(QStringLiteral(", "))));
        QVERIFY(mutated > 300);
        // restart-only settings changed above (ports, proxy ...) are announced on close
        QVERIFY(!d->restartMessage().isEmpty());
        closeDialog(d);
        QVERIFY(!m_messages.isEmpty());
        QVERIFY(m_messages.last().startsWith(QStringLiteral("THESE SETTINGS TAKE EFFECT WHEN OCTOMX STARTS AGAIN")));
        m_messages.clear();

        // reopen: every control shows what Settings holds
        d = open();
        int checked = 0;
        for (QWidget *w : boundWidgets(d)) {
            const QStringList keys = keysOf(w);
            const QString key = keys.first();
            if (key.endsWith(QLatin1String("/*")) || qobject_cast<StaticText *>(w))
                continue;
            const QVariant cur = s->value(key);
            const QString what = QStringLiteral("%1 (%2)").arg(key, QString::fromLatin1(w->metaObject()->className()));
            bool ok = true;
            QString shown;
            if (auto *bw = dynamic_cast<BoundWidget *>(w)) {
                const QVariant v = bw->boundValue();
                ok = qobject_cast<ColorButton *>(w) ? QColor(v.toString()) == QColor(cur.toString()) : sameValue(v, cur);
                shown = text(v);
            } else if (auto *mb = qobject_cast<MenuButton *>(w)) {
                ok = sameValue(mb->currentData(), cur);
                shown = text(mb->currentData());
            } else if (auto *r = qobject_cast<QRadioButton *>(w)) {
                ok = r->isChecked() == sameValue(r->property(kValueProp), cur);
                shown = r->isChecked() ? QStringLiteral("checked") : QStringLiteral("unchecked");
            } else if (auto *cb = qobject_cast<QComboBox *>(w)) {
                ok = sameValue(cb->currentData(), cur);
                shown = text(cb->currentData());
            } else if (auto *sb = qobject_cast<QSpinBox *>(w)) {
                ok = sb->value() == cur.toInt();
                shown = QString::number(sb->value());
            } else if (auto *ds = qobject_cast<QDoubleSpinBox *>(w)) {
                ok = qFuzzyCompare(ds->value() + 1, cur.toDouble() + 1);
                shown = QString::number(ds->value());
            } else if (auto *le = qobject_cast<QLineEdit *>(w)) {
                if (le->isReadOnly() || keys.size() > 1)
                    continue;
                const SettingDef *def = findSetting(key);
                const SettingType t = def ? def->type : SettingType::String;
                shown = le->text();
                ok = t == SettingType::StringList ? SettingsBinder::textToList(shown) == cur.toStringList()
                     : t == SettingType::Map      ? SettingsBinder::mapToText(cur.toMap()) == shown
                                                  : sameValue(shown, cur);
            } else if (auto *ab = qobject_cast<QAbstractButton *>(w); ab && ab->isCheckable()) {
                ok = ab->isChecked() == cur.toBool();
                shown = ab->isChecked() ? QStringLiteral("on") : QStringLiteral("off");
            } else {
                continue;   // value buttons have no state of their own
            }
            QVERIFY2(ok, qPrintable(QStringLiteral("after reopen %1 shows '%2', Settings has '%3'").arg(what, shown, text(cur))));
            ++checked;
        }
        qInfo("after reopen: %d controls show the stored value", checked);
        QVERIFY(checked > 300);
        closeDialog(d);
        m_messages.clear();
        m_core->settings()->resetToDefaults();
    }

    void validation()
    {
        Settings *s = m_core->settings();
        SettingsDialog *d = open(QStringLiteral("connection/incoming"));
        auto *port = d->findChild<QLineEdit *>(QStringLiteral("set_connection_tcpPort"));
        QVERIFY(port);
        const int before = s->i(keys::connection::TcpPort);
        port->setText(QStringLiteral("70000"));
        emit port->editingFinished();
        QCOMPARE(s->i(keys::connection::TcpPort), before);
        QCOMPARE(m_messages.size(), 1);
        QVERIFY(m_messages.last().contains(QStringLiteral("between 0 and 65535")));
        QCOMPARE(port->text(), QString::number(before));   // the stored value is shown again
        port->setText(QStringLiteral("abc"));
        emit port->editingFinished();
        QCOMPARE(m_messages.size(), 2);
        QCOMPARE(s->i(keys::connection::TcpPort), before);
        port->setText(QStringLiteral("16699"));
        emit port->editingFinished();
        QCOMPARE(s->i(keys::connection::TcpPort), 16699);
        // spin boxes take their range from the schema
        auto *sb = d->findChild<QSpinBox *>(QStringLiteral("set_transfers_maxDownloads"));
        QVERIFY(sb);
        QCOMPARE(sb->minimum(), int(findSetting(QStringLiteral("transfers/maxDownloads"))->min.toInt()));
        QCOMPARE(sb->maximum(), int(findSetting(QStringLiteral("transfers/maxDownloads"))->max.toInt()));
        m_messages.clear();
        QVERIFY(d->restartMessage().contains(QStringLiteral("Incoming TCP port")));
        closeDialog(d);
        QCOMPARE(m_messages.size(), 1);
        m_messages.clear();
        s->resetToDefaults();
    }

    void openSettingsPages()
    {
        const struct { const char *ask; const char *id; } cases[] = {
            {"Queuing", "transfers/queuing"}, {"File Transfers", "transfers"}, {"Incoming Msgs & Privacy", "messages"},
            {"OpenNap Networks", "other"}, {"bt", "bt"}, {"Bandwidth Throttle", "throttle"}, {"advanced", "advanced"},
            {"Channel Window", "appearance/channel"}};
        for (const auto &c : cases) {
            m_mw->openSettings(QString::fromLatin1(c.ask));
            auto *d = m_mw->findChild<SettingsDialog *>();
            QVERIFY(d && d->isVisible());
            QCOMPARE(d->currentPageId(), QString::fromLatin1(c.id));
            QVERIFY(d->pageWidget(QString::fromLatin1(c.id))->isVisible());
        }
        // still one dialog (modeless, raised again)
        QCOMPARE(m_mw->findChildren<SettingsDialog *>().size(), 1);
        // the Settings button opens it; the last page is remembered
        auto *d = m_mw->findChild<SettingsDialog *>();
        closeDialog(d);
        m_mw->findChild<QAbstractButton *>(QStringLiteral("btnSettings"))->click();
        d = m_mw->findChild<SettingsDialog *>();
        QVERIFY(d && d->isVisible());
        QCOMPARE(d->currentPageId(), QStringLiteral("appearance/channel"));
        closeDialog(d);
    }

    void resetDefaults()
    {
        // [ui-h] the OctoMX Peer Network page has no settings of its own left: File Transfers' button
        Settings *s = m_core->settings();
        s->setValue(keys::transfers::ClearCompleteDownloadsSec, 80);
        s->setValue(keys::transfers::AutoClearCompleted, true);
        SettingsDialog *d = open(QStringLiteral("transfers"));
        d->findChild<QAbstractButton *>(QStringLiteral("transfersResetDefaults"))->click();
        QCOMPARE(s->i(keys::transfers::ClearCompleteDownloadsSec), 300);
        QCOMPARE(s->b(keys::transfers::AutoClearCompleted), false);
        auto *spin = d->findChild<QSpinBox *>(QStringLiteral("set_transfers_clearCompleteDownloadsSec"));
        QCOMPARE(spin->value(), 300);
        closeDialog(d);
    }

    // [enc-1] Settings, Gnutella: Encryption (TLS), Prefer by default, like BitTorrent's choice
    void gnutellaEncryption()
    {
        Settings *s = m_core->settings();
        QCOMPARE(s->s(keys::gnutella::Encryption), QStringLiteral("enabled"));
        SettingsDialog *d = open(QStringLiteral("gnutella"));
        auto *c = d->pageWidget(QStringLiteral("gnutella"))->findChild<QComboBox *>(QStringLiteral("set_gnutella_encryption"));
        QVERIFY(c);
        QCOMPARE(c->count(), 3);
        QCOMPARE(c->currentText(), QStringLiteral("Prefer encryption"));
        QCOMPARE(c->itemText(1), QStringLiteral("Require encryption"));
        QCOMPARE(c->itemText(2), QStringLiteral("Disable encryption"));
        QVERIFY(c->toolTip().contains(QLatin1String("not anonymity")));
        c->setCurrentIndex(1);
        emit c->activated(1);                                // what a user's choice does
        QTRY_COMPARE(s->s(keys::gnutella::Encryption), QStringLiteral("forced"));
        c->setCurrentIndex(0);
        emit c->activated(0);
        QTRY_COMPARE(s->s(keys::gnutella::Encryption), QStringLiteral("enabled"));
        closeDialog(d);
    }

    void colorsApplyLive()
    {
        SettingsDialog *d = open(QStringLiteral("appearance/search"));
        auto *b = d->pageWidget(QStringLiteral("appearance/search"))->findChild<ColorButton *>();
        QVERIFY(b);
        const QString key = keysOf(b).first();
        QSignalSpy spy(Colors::instance(), &Colors::changed);
        b->click();
        QVERIFY(spy.count() >= 1);
        QCOMPARE(Colors::instance()->get(key), b->color());
        // presets: dark = the schema defaults, light = colorSchemePreset(true)
        d->showPage(QStringLiteral("appearance"));
        d->findChild<QAbstractButton *>(QStringLiteral("appearanceLight"))->click();
        QCOMPARE(m_core->settings()->s(keys::colors::ListBackground), colorSchemePreset(true).value(QStringLiteral("colors/listBackground")).toString());
        d->findChild<QAbstractButton *>(QStringLiteral("appearanceDark"))->click();
        QCOMPARE(Colors::instance()->get(QStringLiteral("colors/listBackground")), QColor(Qt::black));
        QCOMPARE(Colors::instance()->get(key), Colors::specDefault(key));
        closeDialog(d);
    }

    void wizardRecommended()
    {
        Settings *s = m_core->settings();
        s->setValue(keys::gnutella::Enabled, false);
        s->setValue(keys::bt::Enabled, false);
        s->setValue(keys::connection::Upnp, false);
        s->setValue(keys::misc::AssociateTorrent, false);
        SetupWizard w(m_core, m_mw);
        w.show();
        QVERIFY(w.recommended());
        QVERIFY(w.button(QWizard::FinishButton)->isEnabled());   // one click from the first page
        w.button(QWizard::FinishButton)->click();
        QVERIFY(s->b(keys::gnutella::Enabled) && s->b(keys::bt::Enabled) && s->b(keys::ed2k::Enabled) && s->b(keys::kad::Enabled) &&
                s->b(keys::overlay::Enabled) && s->b(keys::http::Enabled) && s->b(keys::web::Enabled) && s->b(keys::rats::Enabled));
        QVERIFY(s->b(keys::connection::Upnp) && s->b(keys::connection::NatPmp) && s->b(keys::connection::AcceptIncoming));
        QVERIFY(s->b(keys::misc::AssociateTorrent) && s->b(keys::misc::AssociateMagnet) && s->b(keys::misc::AssociateEd2k));
        QVERIFY(s->b(keys::internal::WizardDone));
        QVERIFY(s->b(keys::network::ConnectOnStart));
    }

    void wizardCustom()
    {
        Settings *s = m_core->settings();
        SetupWizard::setPortTester([](int port, bool udp) {
            return SetupWizard::PortTest{port != 1234, QStringLiteral("%1 %2").arg(udp ? "udp" : "tcp").arg(port)};
        });
        SetupWizard w(m_core, m_mw);
        w.show();
        w.findChild<QRadioButton *>(QStringLiteral("wizCustom"))->setChecked(true);
        w.next();
        QCOMPARE(w.currentId(), int(SetupWizard::ScreenName));
        w.findChild<QLineEdit *>(QStringLiteral("wizName"))->setText(QStringLiteral("Tester"));
        w.next();
        w.findChild<QRadioButton *>(QStringLiteral("wizConn4"))->setChecked(true);   // Cable ([qa-1] modern list)
        w.next();
        QCOMPARE(w.currentId(), int(SetupWizard::TcpPort));
        auto *tcp = w.findChild<QLineEdit *>(QStringLiteral("wizTcpPort"));
        tcp->setText(QStringLiteral("1234"));
        w.findChild<QAbstractButton *>(QStringLiteral("wizTcpTest"))->click();
        QCOMPARE(w.findChild<WrapLabel *>(QStringLiteral("wizTcpResult"))->text(), QStringLiteral("tcp 1234"));
        tcp->setText(QStringLiteral("16000"));
        w.next();   // [ui-h] no UDP page any more: Folders follows
        QVERIFY(!w.findChild<QLineEdit *>(QStringLiteral("wizUdpPort")));
        w.findChild<PathField *>(QStringLiteral("wizIncoming"))->setValueByUser(m_dir.filePath(QStringLiteral("in")));
        w.next();
        w.findChild<QCheckBox *>(QStringLiteral("wizNet_gnutella"))->setChecked(false);
        w.findChild<QCheckBox *>(QStringLiteral("wizAssoc"))->setChecked(false);
        w.next();
        QCOMPARE(w.currentId(), int(SetupWizard::Finish));
        QVERIFY(w.findChild<WrapLabel *>(QStringLiteral("wizSummary"))->text().contains(QStringLiteral("16000 (TCP and UDP)")));
        w.button(QWizard::FinishButton)->click();
        QCOMPARE(s->s(keys::identity::ScreenName), QStringLiteral("Tester"));
        QCOMPARE(s->s(keys::identity::Connection), QStringLiteral("Cable"));
        QCOMPARE(s->i(keys::connection::TcpPort), 16000);
        QCOMPARE(s->s(keys::transfers::IncomingDir), m_dir.filePath(QStringLiteral("in")));
        QVERIFY(!s->b(keys::gnutella::Enabled));
        QVERIFY(s->b(keys::bt::Enabled));
        QVERIFY(!s->b(keys::misc::AssociateMagnet));
        s->resetToDefaults();
    }

    void wizardFromSettings()
    {
        Settings *s = m_core->settings();
        s->setValue(keys::transfers::MaxDownloads, 7);
        SettingsDialog *d = open();
        m_askAnswer = QMessageBox::Cancel;
        d->findChild<QAbstractButton *>(QStringLiteral("settingsWizard"))->click();
        QTest::qWait(50);
        QVERIFY(!d->findChild<SetupWizard *>());
        m_askAnswer = QMessageBox::Yes;   // "Clear All Current Settings Before Running Setup Wizard?"
        d->findChild<QAbstractButton *>(QStringLiteral("settingsWizard"))->click();
        QTRY_VERIFY(d->findChild<SetupWizard *>());
        QCOMPARE(s->i(keys::transfers::MaxDownloads), 40);   // cleared
        auto *w = d->findChild<SetupWizard *>();
        QVERIFY(w->isVisible());
        w->reject();
        closeDialog(d);
        m_messages.clear();
    }

    void cleanupTestCase()
    {
        delete m_mw;
        delete m_core;
    }
};

QTEST_MAIN(TstSettingsDialog)
#include "tst_settings_dialog.moc"
