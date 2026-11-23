// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/settings_binding.h"

#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSignalBlocker>
#include <QtGui/QPainter>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QColorDialog>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFontDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyleOptionButton>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QTreeWidget>

#include <climits>

#include "mx/services.h"
#include "ui/kit/format.h"
#include "ui/kit/colors.h"
#include "ui/kit/draw.h"
#include "ui/kit/scale.h"

namespace mx::ui {

namespace {

// 16-px rows in the white classic lists (Settings ▸ Library "Available Filetypes")
class RowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &i) const override
    {
        return QSize(QStyledItemDelegate::sizeHint(o, i).width(), S(16));
    }
};

// connects any QObject's valueEdited(QVariant) signal to a std::function
class Relay : public QObject {
    Q_OBJECT
public:
    Relay(QObject *parent, std::function<void(const QVariant &)> f) : QObject(parent), m_f(std::move(f)) {}
public slots:
    void relay(const QVariant &v) { m_f(v); }
private:
    std::function<void(const QVariant &)> m_f;
};

SettingsBinder::MessageHook &messageHook()
{
    static SettingsBinder::MessageHook h;
    return h;
}
ColorButton::PickHook &pickHook()
{
    static ColorButton::PickHook h;
    return h;
}
PathField::BrowseHook &browseHook()
{
    static PathField::BrowseHook h;
    return h;
}
StringListEditor::PromptHook &promptHook()
{
    static StringListEditor::PromptHook h;
    return h;
}

QString rangeText(const SettingDef *def)
{
    return QObject::tr("Please enter a number between %1 and %2.").arg(def->min.toString(), def->max.toString());
}

} // namespace

// ================================================================================ SettingsBinder

SettingsBinder::SettingsBinder(mx::Settings *s, QObject *parent) : QObject(parent), m_s(s)
{
    connect(s, &Settings::changed, this, [this](const QString &key, const QVariant &) { onChanged(key); });
}

void SettingsBinder::setMessageHook(const MessageHook &h)
{
    messageHook() = h;
}

SettingsBinder::AskHook &askHook()
{
    static SettingsBinder::AskHook h;
    return h;
}

void SettingsBinder::setAskHook(const AskHook &h)
{
    askHook() = h;
}

int SettingsBinder::ask(QWidget *parent, const QString &title, const QString &text, int buttons, int defaultButton)
{
    if (askHook())
        return askHook()(parent, title, text, buttons, defaultButton);
    return QMessageBox::question(parent, title, text, QMessageBox::StandardButtons(buttons), QMessageBox::StandardButton(defaultButton));
}

void SettingsBinder::message(QWidget *parent, const QString &title, const QString &text)
{
    if (messageHook()) {
        messageHook()(parent, title, text);
        return;
    }
    QMessageBox::warning(parent, title, text);
}

QString SettingsBinder::listToText(const QStringList &l)
{
    return l.join(QStringLiteral("; "));
}

QStringList SettingsBinder::textToList(const QString &t)
{
    QStringList out;
    for (const QString &s : t.split(QLatin1Char(';'))) {
        const QString v = s.trimmed();
        if (!v.isEmpty())
            out << v;
    }
    return out;
}

QString SettingsBinder::mapToText(const QVariantMap &m)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(m)).toJson(QJsonDocument::Compact));
}

bool SettingsBinder::textToMap(const QString &t, QVariantMap *out)
{
    if (t.trimmed().isEmpty()) {
        *out = QVariantMap();
        return true;
    }
    QJsonParseError err;
    const QJsonDocument d = QJsonDocument::fromJson(t.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !d.isObject())
        return false;
    *out = d.object().toVariantMap();
    return true;
}

void SettingsBinder::rememberInitial(const QString &key)
{
    if (!m_initial.contains(key))
        m_initial.insert(key, m_s->value(key));
}

void SettingsBinder::add(const QList<QWidget *> &widgets, const QStringList &keys, const std::function<void()> &load)
{
    Binding b;
    b.keys = keys;
    b.load = load;
    for (QWidget *w : widgets) {
        b.widgets << w;
        if (!w->property(kKeyProp).isValid())
            w->setProperty(kKeyProp, keys.join(QLatin1Char(',')));
    }
    const int idx = int(m_bindings.size());
    m_bindings << b;
    for (const QString &k : keys) {
        m_byKey.insert(k, idx);
        rememberInitial(k);
    }
    load();
}

void SettingsBinder::onChanged(const QString &key)
{
    const QList<int> idx = m_byKey.values(key);
    for (int i : idx)
        m_bindings[i].load();
}

void SettingsBinder::reloadAll()
{
    for (const Binding &b : std::as_const(m_bindings))
        b.load();
}

QStringList SettingsBinder::boundKeys() const
{
    QStringList out = m_byKey.uniqueKeys();
    out.sort();
    return out;
}

QStringList SettingsBinder::keysOf(const QWidget *root) const
{
    QStringList out;
    for (const Binding &b : m_bindings) {
        for (const QPointer<QWidget> &w : b.widgets) {
            if (w && (w == root || root->isAncestorOf(w))) {
                for (const QString &k : b.keys)
                    if (!out.contains(k))
                        out << k;
                break;
            }
        }
    }
    return out;
}

QStringList SettingsBinder::restartPending() const
{
    QStringList out(m_restart.begin(), m_restart.end());
    out.sort();
    return out;
}

void SettingsBinder::resetKeys(const QStringList &keys)
{
    for (const QString &k : keys) {
        rememberInitial(k);
        m_s->setValue(k, QVariant());   // = back to the default (mx::Settings contract)
        const SettingDef *def = findSetting(k);
        if (def && def->restartRequired) {
            if (m_s->value(k) != m_initial.value(k))
                m_restart.insert(k);
            else
                m_restart.remove(k);
        }
    }
}

bool SettingsBinder::commit(const QString &key, const QVariant &value, QWidget *origin)
{
    const SettingDef *def = findSetting(key);
    QVariant v = value;
    if (def) {
        if (def->type == SettingType::Int || def->type == SettingType::Double) {
            bool ok = false;
            const double n = value.toDouble(&ok);
            if (!ok || (def->min.isValid() && n < def->min.toDouble()) || (def->max.isValid() && n > def->max.toDouble())) {
                message(origin ? origin->window() : nullptr, tr("OctoMX"),
                        ok ? rangeText(def) : tr("Please enter a number."));
                onChanged(key);   // show the stored value again
                return false;
            }
        }
        bool ok = false;
        v = coerceSetting(*def, value, &ok);
        if (!ok) {
            message(origin ? origin->window() : nullptr, tr("OctoMX"), tr("\"%1\" is not a valid value for this setting.").arg(value.toString()));
            onChanged(key);
            return false;
        }
    }
    rememberInitial(key);
    if (m_s->value(key) != v)
        m_s->setValue(key, v);
    if (def && def->restartRequired) {
        if (m_s->value(key) != m_initial.value(key))
            m_restart.insert(key);
        else
            m_restart.remove(key);
    }
    emit committed(key, v);
    return true;
}

void SettingsBinder::bindCustom(const QList<QWidget *> &tagged, const QStringList &keys, const std::function<void()> &load)
{
    add(tagged, keys, load);
}

void SettingsBinder::bindRadio(QRadioButton *r, const QString &key, const QVariant &value)
{
    r->setProperty(kValueProp, value);
    r->setAutoExclusive(true);
    add({r}, {key}, [this, r, key, value] {
        QSignalBlocker b(r);
        const QVariant cur = m_s->value(key);
        const bool on = value.typeId() == QMetaType::Bool ? cur.toBool() == value.toBool()
                                                          : cur.toString().compare(value.toString(), Qt::CaseInsensitive) == 0;
        if (on) {
            r->setChecked(true);
        } else if (r->isChecked()) {
            // auto-exclusive radios cannot be unchecked directly
            r->setAutoExclusive(false);
            r->setChecked(false);
            r->setAutoExclusive(true);
        }
    });
    connect(r, &QAbstractButton::toggled, this, [this, r, key, value](bool on) {
        if (on)
            commit(key, value, r);
    });
}

void SettingsBinder::bindValueButton(QAbstractButton *b, const QString &key, const QVariant &value)
{
    b->setProperty(kValueProp, value);
    add({b}, {key}, [] {});
    connect(b, &QAbstractButton::clicked, this, [this, b, key, value] { commit(key, value, b); });
}

QWidget *SettingsBinder::bind(QWidget *c, const QString &key, SettingType fallbackType)
{
    const SettingDef *def = findSetting(key);
    const SettingType t = def ? def->type : fallbackType;

    if (auto *bw = dynamic_cast<BoundWidget *>(c)) {
        const bool uiColor = !def && fallbackType == SettingType::Color;   // UI-only colour (news bar)
        add({c}, {key}, [this, c, bw, key, uiColor] {
            QSignalBlocker b(c);
            QVariant v = m_s->value(key);
            if (!v.isValid() && uiColor)
                v = Colors::instance()->get(key).name();
            bw->setBoundValue(v);
        });
        auto *relay = new Relay(c, [this, c, key](const QVariant &v) { commit(key, v, c); });
        connect(c, SIGNAL(valueEdited(QVariant)), relay, SLOT(relay(QVariant)));
        return c;
    }
    if (auto *mb = qobject_cast<MenuButton *>(c)) {
        add({c}, {key}, [this, mb, key] {
            QSignalBlocker b(mb);
            mb->setCurrentData(m_s->value(key));
        });
        connect(mb, &MenuButton::choiceChanged, this, [this, mb, key](const QVariant &v) { commit(key, v, mb); });
        return c;
    }
    if (auto *cb = qobject_cast<QComboBox *>(c)) {
        if (cb->count() == 0 && def && t == SettingType::Enum) {
            for (int i = 0; i < def->choices.size(); ++i)
                cb->addItem(def->choiceLabels.value(i, def->choices.at(i)), def->choices.at(i));
        }
        add({c}, {key}, [this, cb, key, t] {
            QSignalBlocker b(cb);
            const QVariant v = m_s->value(key);
            int i = cb->findData(v);
            if (i < 0 && t == SettingType::Enum) {
                for (int k = 0; k < cb->count(); ++k)
                    if (cb->itemData(k).toString().compare(v.toString(), Qt::CaseInsensitive) == 0)
                        i = k;
            }
            if (i < 0 && (t == SettingType::Int || t == SettingType::Double)) {
                // a value set elsewhere that the list does not offer: add it, keep the order
                int pos = 0;
                while (pos < cb->count() && cb->itemData(pos).toDouble() < v.toDouble())
                    ++pos;
                cb->insertItem(pos, v.toString(), v);
                i = pos;
            }
            cb->setCurrentIndex(i);
        });
        connect(cb, &QComboBox::activated, this, [this, cb, key](int i) { commit(key, cb->itemData(i), cb); });
        return c;
    }
    if (auto *sb = qobject_cast<QSpinBox *>(c)) {
        int mn = 0, mx = INT_MAX;
        if (def && def->min.isValid())
            mn = int(qBound(qint64(INT_MIN), qint64(def->min.toLongLong()), qint64(INT_MAX)));
        if (def && def->max.isValid())
            mx = int(qBound(qint64(INT_MIN), qint64(def->max.toLongLong()), qint64(INT_MAX)));
        sb->setRange(mn, mx);
        sb->setKeyboardTracking(false);
        add({c}, {key}, [this, sb, key] {
            QSignalBlocker b(sb);
            sb->setValue(m_s->value(key).toInt());
        });
        connect(sb, &QSpinBox::valueChanged, this, [this, sb, key](int v) { commit(key, v, sb); });
        return c;
    }
    if (auto *ds = qobject_cast<QDoubleSpinBox *>(c)) {
        if (def && def->min.isValid())
            ds->setMinimum(def->min.toDouble());
        if (def && def->max.isValid())
            ds->setMaximum(def->max.toDouble());
        ds->setKeyboardTracking(false);
        add({c}, {key}, [this, ds, key] {
            QSignalBlocker b(ds);
            ds->setValue(m_s->value(key).toDouble());
        });
        connect(ds, &QDoubleSpinBox::valueChanged, this, [this, ds, key](double v) { commit(key, v, ds); });
        return c;
    }
    if (auto *le = qobject_cast<QLineEdit *>(c)) {
        auto toText = [t](const QVariant &v) -> QString {
            switch (t) {
            case SettingType::StringList: return listToText(v.toStringList());
            case SettingType::Map: return mapToText(v.toMap());
            case SettingType::Bytes: return v.toByteArray().size() == 1 ? tr("(1 byte)") : tr("(%1 bytes)").arg(v.toByteArray().size());   // [qa-2]
            default: return v.toString();
            }
        };
        if (t == SettingType::Bytes)
            le->setReadOnly(true);
        add({c}, {key}, [this, le, key, toText] {
            QSignalBlocker b(le);
            const QString s = toText(m_s->value(key));
            if (le->text() != s) {
                le->setText(s);
                le->setCursorPosition(0);
            }
        });
        connect(le, &QLineEdit::editingFinished, this, [this, le, key, t, toText] {
            if (le->isReadOnly())
                return;
            if (le->text() == toText(m_s->value(key)))
                return;
            QVariant v;
            switch (t) {
            case SettingType::StringList:
                v = textToList(le->text());
                break;
            case SettingType::Map: {
                QVariantMap m;
                if (!textToMap(le->text(), &m)) {
                    message(le->window(), tr("OctoMX"), tr("This setting must be a JSON object, e.g. {\"name\": true}."));
                    onChanged(key);
                    return;
                }
                v = m;
                break;
            }
            case SettingType::Int:
            case SettingType::Double: {
                bool ok = false;
                const double d = le->text().trimmed().toDouble(&ok);
                v = ok ? QVariant(t == SettingType::Int ? QVariant(qint64(d)) : QVariant(d)) : QVariant(le->text());
                if (ok && t == SettingType::Int && double(qint64(d)) != d)
                    v = le->text();   // "1.5" for an integer -> rejected by commit
                break;
            }
            default:
                v = le->text();
            }
            commit(key, v, le);
        });
        return c;
    }
    if (auto *ab = qobject_cast<QAbstractButton *>(c); ab && ab->isCheckable() && !qobject_cast<QRadioButton *>(c)) {
        add({c}, {key}, [this, ab, key] {
            QSignalBlocker b(ab);
            ab->setChecked(m_s->value(key).toBool());
        });
        connect(ab, &QAbstractButton::toggled, this, [this, ab, key](bool on) { commit(key, on, ab); });
        return c;
    }
    qWarning("SettingsBinder: cannot bind %s to %s", c->metaObject()->className(), qPrintable(key));
    return c;
}

// =================================================================================== ColorButton

ColorButton::ColorButton(const QString &text, QWidget *parent) : PushButton(text, parent)
{
    connect(this, &QPushButton::clicked, this, &ColorButton::pick);
}

void ColorButton::setPickHook(const PickHook &h)
{
    pickHook() = h;
}

void ColorButton::setBoundValue(const QVariant &v)
{
    const QColor c(v.toString());
    if (c != m_color) {
        m_color = c;
        update();
    }
}

void ColorButton::setValueByUser(const QVariant &v)
{
    const QColor c = v.typeId() == QMetaType::QColor ? v.value<QColor>() : QColor(v.toString());
    if (!c.isValid())
        return;
    setBoundValue(c.name());
    emit valueEdited(c.name());
}

void ColorButton::pick()
{
    QColor c;
    if (pickHook())
        c = pickHook()(m_color, window());
    else
        c = QColorDialog::getColor(m_color, window(), tr("Color"));
    if (c.isValid())
        setValueByUser(c);
}

void ColorButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QStyleOptionButton opt;
    initStyleOption(&opt);
    opt.text.clear();
    style()->drawControl(QStyle::CE_PushButton, &opt, &p, this);
    const int shift = isDown() ? uiLine() : 0;
    p.setFont(font());
    p.setPen(isEnabled() ? chrome::Text : chrome::DisabledText);
    const QRect sw(width() - S(8) - S(30) + shift, (height() - S(9)) / 2 + shift, S(30), S(9));
    draw::text(&p, QRect(S(6) + shift, shift, qMax(0, sw.left() - S(10)), height()), Qt::AlignLeft, text());
    p.fillRect(sw, isEnabled() ? m_color : chrome::Face);
    const QColor fc = isEnabled() ? QColor(Qt::black) : QColor(chrome::Shadow);
    draw::edge(&p, sw, fc, fc);
}

// ===================================================================================== PathField

PathField::PathField(const QString &buttonText, bool directory, QWidget *parent) : QWidget(parent), m_dir(directory)
{
    m_text = new StaticText(QString(), this);
    m_text->setObjectName(QStringLiteral("pathText"));
    m_button = new PushButton(buttonText, this);
    m_button->setObjectName(QStringLiteral("pathChange"));
    m_button->setToolTip(directory ? tr("Pick another folder for this setting. The folder in use is shown on the left.")
                                   : tr("Pick another file for this setting. The file in use is shown on the left."));
    connect(m_button, &QPushButton::clicked, this, [this] {
        const QString p = browse(m_path, m_dir, window(), m_filter);
        if (!p.isEmpty())
            setValueByUser(p);
    });
}

void PathField::setBrowseHook(const BrowseHook &h)
{
    browseHook() = h;
}

QString PathField::browse(const QString &current, bool directory, QWidget *parent, const QString &filter)
{
    if (browseHook())
        return browseHook()(current, directory, parent);
    if (directory)
        return QFileDialog::getExistingDirectory(parent, tr("Choose Folder"), current);
    return QFileDialog::getOpenFileName(parent, tr("Choose File"), current, filter);
}

void PathField::resizeEvent(QResizeEvent *)
{
    if (m_manualGeometry)
        return;
    const int bw = S(70);
    m_button->setGeometry(width() - bw, 0, bw, height());
    m_text->setGeometry(0, (height() - S(13)) / 2, qMax(0, width() - bw - S(6)), S(13));
    setBoundValue(m_path);
}

void PathField::setButtonGeometry(const QRect &textRect, const QRect &buttonRect)
{
    m_manualGeometry = true;
    m_text->setGeometry(SR(textRect));   // [ui-e] design pixels
    m_button->setGeometry(SR(buttonRect));
}

void PathField::setBoundValue(const QVariant &v)
{
    m_path = v.toString();
    QString shown = QDir::toNativeSeparators(m_path);
    if (m_dir && !shown.isEmpty() && !shown.endsWith(QDir::separator()))
        shown += QDir::separator();
    const QFontMetrics fm(font());
    m_text->setText(fmt::elideMiddle(shown, fm, qMax(20, m_text->width())));   // [post-1] three ASCII dots
    m_text->setToolTip(shown);
}

void PathField::setValueByUser(const QVariant &v)
{
    setBoundValue(v);
    emit valueEdited(m_path);
}

// ==================================================================================== FontButton

FontButton::FontButton(const QString &text, QWidget *parent) : PushButton(text, parent)
{
    connect(this, &QPushButton::clicked, this, [this] {
        QFont f;
        const QStringList parts = m_value.split(QLatin1Char(','));
        f.setFamily(parts.value(0, QStringLiteral("Tahoma")));
        f.setPointSize(qMax(4, parts.value(1, QStringLiteral("8")).toInt()));
        if (parts.value(2) == QLatin1String("bold"))
            f.setBold(true);
        bool ok = false;
        const QFont chosen = QFontDialog::getFont(&ok, f, window(), tr("Font"));
        if (ok)
            setValueByUser(QStringLiteral("%1,%2%3").arg(chosen.family()).arg(chosen.pointSize()).arg(chosen.bold() ? QStringLiteral(",bold") : QString()));
    });
}

void FontButton::setValueByUser(const QVariant &v)
{
    setBoundValue(v);
    emit valueEdited(m_value);
}

// ============================================================================== StringListEditor

StringListEditor::StringListEditor(const QStringList &defaults, QWidget *parent) : QWidget(parent), m_defaults(defaults)
{
    m_list = new QTreeWidget(this);
    m_list->setObjectName(QStringLiteral("listEditorList"));
    m_list->setHeaderLabels({tr("Name")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->header()->setSectionsClickable(false);
    m_list->header()->setStretchLastSection(true);
    m_list->setItemDelegate(new RowDelegate(m_list));
    auto mk = [this](const QString &t, const char *name) {
        auto *b = new PushButton(t, this);
        b->setObjectName(QString::fromLatin1(name));
        return b;
    };
    m_new = mk(tr("New"), "listNew");
    m_edit = mk(tr("Edit"), "listEdit");
    m_up = mk(tr("Move Up"), "listUp");
    m_defaultsBtn = mk(tr("Add Defaults"), "listDefaults");
    m_remove = mk(tr("Remove"), "listRemove");
    m_down = mk(tr("Move Down"), "listDown");
    m_new->setToolTip(tr("Add a new entry to this list. You type its text in a small box."));
    m_edit->setToolTip(tr("Change the text of the selected entry (double-click does the same)."));
    m_up->setToolTip(tr("Move the selected entry one place up."));
    m_defaultsBtn->setToolTip(tr("Add back the entries the program comes with that are missing from the list. Your own entries stay."));
    m_remove->setToolTip(tr("Delete the selected entry from the list."));
    m_down->setToolTip(tr("Move the selected entry one place down."));
    connect(m_list, &QTreeWidget::currentItemChanged, this, &StringListEditor::updateButtons);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] { m_edit->click(); });
    connect(m_new, &QPushButton::clicked, this, [this] {
        const QString t = prompt(tr("New Filetype"), QString());
        if (t.isEmpty())
            return;
        m_list->addTopLevelItem(new QTreeWidgetItem({t}));
        edited();
    });
    connect(m_edit, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem *it = m_list->currentItem();
        if (!it)
            return;
        const QString t = prompt(tr("Edit Filetype"), it->text(0));
        if (t.isEmpty() || t == it->text(0))
            return;
        it->setText(0, t);
        edited();
    });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        delete m_list->currentItem();
        edited();
    });
    connect(m_up, &QPushButton::clicked, this, [this] {
        const int i = m_list->indexOfTopLevelItem(m_list->currentItem());
        if (i <= 0)
            return;
        QTreeWidgetItem *it = m_list->takeTopLevelItem(i);
        m_list->insertTopLevelItem(i - 1, it);
        m_list->setCurrentItem(it);
        edited();
    });
    connect(m_down, &QPushButton::clicked, this, [this] {
        const int i = m_list->indexOfTopLevelItem(m_list->currentItem());
        if (i < 0 || i >= m_list->topLevelItemCount() - 1)
            return;
        QTreeWidgetItem *it = m_list->takeTopLevelItem(i);
        m_list->insertTopLevelItem(i + 1, it);
        m_list->setCurrentItem(it);
        edited();
    });
    connect(m_defaultsBtn, &QPushButton::clicked, this, [this] {
        QStringList cur = boundValue().toStringList();
        bool any = false;
        for (const QString &d : std::as_const(m_defaults))
            if (!cur.contains(d)) {
                cur << d;
                any = true;
            }
        if (any) {
            setBoundValue(cur);
            edited();
        }
    });
    updateButtons();
}

void StringListEditor::setPromptHook(const PromptHook &h)
{
    promptHook() = h;
}

QString StringListEditor::prompt(const QString &title, const QString &current)
{
    if (promptHook())
        return promptHook()(title, current, window()).trimmed();
    bool ok = false;
    const QString t = QInputDialog::getText(window(), title, tr("Name (extensions in brackets, e.g. Audio (.mp3 .ogg)):"),
                                            QLineEdit::Normal, current, &ok);
    return ok ? t.trimmed() : QString();
}

QVariant StringListEditor::boundValue() const
{
    QStringList out;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
        out << m_list->topLevelItem(i)->text(0);
    return out;
}

void StringListEditor::setBoundValue(const QVariant &v)
{
    const QStringList l = v.toStringList();
    if (l == boundValue().toStringList())
        return;
    const int cur = m_list->indexOfTopLevelItem(m_list->currentItem());
    m_list->clear();
    for (const QString &s : l)
        m_list->addTopLevelItem(new QTreeWidgetItem({s}));
    if (cur >= 0 && cur < m_list->topLevelItemCount())
        m_list->setCurrentItem(m_list->topLevelItem(cur));
    updateButtons();
}

void StringListEditor::setValueByUser(const QVariant &v)
{
    setBoundValue(v);
    edited();
}

void StringListEditor::edited()
{
    updateButtons();
    emit valueEdited(boundValue());
}

void StringListEditor::updateButtons()
{
    const int i = m_list->indexOfTopLevelItem(m_list->currentItem());
    const bool sel = i >= 0;
    m_edit->setEnabled(sel);
    m_remove->setEnabled(sel);
    m_up->setEnabled(sel && i > 0);
    m_down->setEnabled(sel && i < m_list->topLevelItemCount() - 1);
}

void StringListEditor::resizeEvent(QResizeEvent *)
{
    // list on top, two rows of three buttons (75x23, 5-px gaps) below, Settings ▸ Library
    const int w = width(), h = height();
    m_list->setGeometry(0, 0, w, h - S(54));
    const int bw = S(75), bh = S(23), gap = (w - 3 * bw) / 2;
    const int y1 = h - S(50), y2 = h - bh;
    m_new->setGeometry(0, y1, bw, bh);
    m_edit->setGeometry(bw + gap, y1, bw, bh);
    m_up->setGeometry(w - bw, y1, bw, bh);
    m_defaultsBtn->setGeometry(0, y2, bw, bh);
    m_remove->setGeometry(bw + gap, y2, bw, bh);
    m_down->setGeometry(w - bw, y2, bw, bh);
}

} // namespace mx::ui

#include "settings_binding.moc"
