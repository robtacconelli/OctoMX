// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: Settings ▸ Advanced [ui-c]: every schema key that no other page binds, listed
// generically (description, key, typed editor, "Reset"). Built from mx::settingsSchema() after the
// other pages, so a key added to the schema later appears here without any UI change.
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>

#include <climits>

#include "mx/services.h"
#include "ui/dialogs/settings_pages.h"
#include "ui/kit/colors.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"

namespace mx::ui {

namespace {
// [qa-2] "%n value(s)" printed "(s)" literally (no translator is loaded)
QString valuesStoredText(int n)
{
    return n == 1 ? QObject::tr("1 value stored") : QObject::tr("%1 values stored").arg(n);
}
} // namespace


namespace {

constexpr int kRowH = 50;

// Entries flagged hidden in the schema (internal test switches) are not listed. SettingDef has no
// `hidden` member yet; this picks it up automatically once it exists.
template <class D> auto hiddenOf(const D &d, int) -> decltype(bool(d.hidden)) { return d.hidden; }
template <class D> bool hiddenOf(const D &, long) { return false; }

class AdvancedList : public QWidget {
public:
    struct Row {
        QString key;
        QString search;   // lower-case key + label for the filter
        QList<QWidget *> widgets;
        QList<int> dy;    // vertical offset of each widget inside the row
    };
    using QWidget::QWidget;
    QList<Row> rows;
    void filter(const QString &text)
    {
        const QString t = text.trimmed().toLower();
        int y = 4;
        for (const Row &r : std::as_const(rows)) {
            const bool show = t.isEmpty() || r.search.contains(t);
            for (int i = 0; i < r.widgets.size(); ++i) {
                QWidget *w = r.widgets.at(i);
                w->setVisible(show);
                if (show)
                    w->move(w->x(), S(y + r.dy.at(i)));
            }
            if (show)
                y += kRowH;
        }
        resize(width(), S(y + 4));
    }
};

QWidget *editorFor(SettingsBinder *b, const SettingDef &def, QWidget *parent)
{
    QWidget *ed = nullptr;
    switch (def.type) {
    case SettingType::Bool:
        ed = new QCheckBox(QObject::tr("On"), parent);
        break;
    case SettingType::Int: {
        const bool fits = (!def.min.isValid() || def.min.toLongLong() >= INT_MIN) && (!def.max.isValid() || def.max.toLongLong() <= INT_MAX);
        if (fits)
            ed = new QSpinBox(parent);
        else
            ed = new QLineEdit(parent);
        break;
    }
    case SettingType::Double: {
        auto *ds = new QDoubleSpinBox(parent);
        ds->setDecimals(2);
        ed = ds;
        break;
    }
    case SettingType::Enum:
        ed = new QComboBox(parent);
        break;
    case SettingType::Color:
        ed = new ColorButton(QObject::tr("Color"), parent);
        break;
    default:   // String, Path, StringList, Bytes, Map
        ed = new QLineEdit(parent);
        break;
    }
    ed->setObjectName(PageKit::nameFor(def.key));
    ed->setProperty(kAdvancedProp, true);
    b->bind(ed, def.key);
    return ed;
}

} // namespace

void buildAdvancedPage(SettingsDialog *d)
{
    // what the other pages bind (computed before this page exists)
    const QStringList bound = d->pageKeys();
    PageKit k(d, d->addPage(QStringLiteral("advanced"), QObject::tr("Advanced")));
    k.label(QObject::tr("Find:"), 215, 13, 30);
    auto *find = k.place(new QLineEdit, 246, 9, 274, 20);
    find->setObjectName(QStringLiteral("advancedFind"));
    find->setToolTip(QObject::tr("Shows only the settings whose name or key contains this text."));
    find->setClearButtonEnabled(true);

    auto *area = k.place(new QScrollArea, 215, 34, 306, 289);
    area->setObjectName(QStringLiteral("advancedArea"));
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    area->setWidgetResizable(false);
    auto *list = new AdvancedList;
    list->setObjectName(QStringLiteral("advancedList"));
    list->setAutoFillBackground(true);
    const int w = 306 - 4 - 16;
    list->resize(S(w), S(10));

    for (const SettingDef &def : settingsSchema()) {
        if (bound.contains(def.key) || hiddenOf(def, 0))
            continue;
        AdvancedList::Row r;
        r.key = def.key;
        r.search = (def.key + QLatin1Char(' ') + def.label).toLower();
        auto *desc = new StaticText(def.label.isEmpty() ? def.key : def.label + (def.restartRequired ? QStringLiteral(" *") : QString()), list);
        desc->setGeometry(SR(4, 0, w - 8, 13));
        auto *key = new StaticText(def.key, list);
        key->setGeometry(SR(4, 0, w - 8, 13));
        QPalette pal = key->palette();
        pal.setColor(QPalette::WindowText, chrome::Shadow);
        key->setPalette(pal);
        r.widgets << desc << key;
        r.dy << 0 << 13;
        auto *reset = new PushButton(QObject::tr("Reset"), list);
        reset->setObjectName(QStringLiteral("reset_") + PageKit::nameFor(def.key));
        reset->setGeometry(SR(w - 52, 0, 48, 20));
        reset->setToolTip(def.key.endsWith(QLatin1String("/*"))
                              ? QObject::tr("Forget every stored value of this group, so the program uses its defaults again.")
                              : QObject::tr("Put this setting back to its default value."));
        if (def.key.endsWith(QLatin1String("/*"))) {
            // a family of keys (column widths, splitters ...): show how many are stored, reset them all
            const QString prefix = def.key.left(def.key.size() - 1);
            auto *info = new StaticText(QString(), list);
            info->setGeometry(SR(4, 0, w - 64, 20));
            info->setObjectName(PageKit::nameFor(def.key));
            info->setProperty(kAdvancedProp, true);
            Settings *s = d->core()->settings();
            d->binder()->bindCustom({info}, {def.key}, [info, s, prefix] {
                info->setText(valuesStoredText(int(s->keys(prefix).size())));
            });
            QObject::connect(s, &Settings::changed, info, [info, s, prefix](const QString &key) {
                if (key.startsWith(prefix))
                    info->setText(valuesStoredText(int(s->keys(prefix).size())));
            });
            QObject::connect(reset, &QPushButton::clicked, d, [s, prefix] {
                for (const QString &key : s->keys(prefix))
                    s->setValue(key, QVariant());
            });
            r.widgets << info;
        } else {
            QWidget *ed = editorFor(d->binder(), def, list);
            // [ui-h] every editor explains itself: the tip of its key (what it does, the default)
            if (def.type == SettingType::Bool)
                ed->setToolTip(QObject::tr("Ticked turns on: %1").arg(settingTip(def.key)));
            else
                ed->setToolTip(settingTip(def.key) + QLatin1Char(' ') + QObject::tr("Key: %1").arg(def.key));
            ed->setGeometry(SR(4, 0, def.type == SettingType::Bool ? 60 : (def.type == SettingType::Int || def.type == SettingType::Double) ? 90 : w - 64, 20));
            if (def.type == SettingType::Color)
                ed->setGeometry(SR(4, 0, 120, 18));
            const QString keyName = def.key;
            QObject::connect(reset, &QPushButton::clicked, d, [d, keyName] { d->binder()->resetKeys({keyName}); });
            r.widgets << ed;
        }
        r.widgets << reset;
        r.dy << 27 << 27;
        list->rows << r;
    }
    list->filter(QString());
    area->setWidget(list);
    k.label((list->rows.size() == 1 ? QObject::tr("1 setting not on the other pages  (* = after a restart)")
                              : QObject::tr("%1 settings not on the other pages  (* = after a restart)").arg(list->rows.size())),
            215, 328, 306);
    QObject::connect(find, &QLineEdit::textChanged, list, [list, area](const QString &t) {
        list->filter(t);
        area->verticalScrollBar()->setValue(0);
    });
}

} // namespace mx::ui
