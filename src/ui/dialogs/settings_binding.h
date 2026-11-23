// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: Settings dialog plumbing [ui-c]: binds controls to mx::Settings keys.
//
// Every control on a Settings page is bound through SettingsBinder. A bound widget carries the
// dynamic property "mxKey" (the key; composites list several keys separated by ','), radios and
// value buttons also "mxValue" (the value they select). Tests and the Advanced page rely on these
// properties: a schema key that no widget carries is shown on the Advanced page automatically.
//
// Semantics (as in the original WinMX): a change is written to Settings the moment the user makes it (check box
// toggled, radio picked, spin box stepped or edited, text edit finished). The binder validates
// against the schema (range, enum) before writing and refuses bad values with a classic message
// box. It also follows Settings::changed, so a control always shows the current value, even when
// the same key is bound twice (colour pages) or changed elsewhere (Networks page, tray menu).
#pragma once

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QVariant>
#include <QtWidgets/QWidget>

#include <functional>

#include "mx/settings_keys.h"
#include "ui/kit/widgets.h"

class QAbstractButton;
class QLabel;
class QRadioButton;
class QTreeWidget;

namespace mx {
class Settings;
}

namespace mx::ui {

inline constexpr char kKeyProp[] = "mxKey";            // QString: bound key(s), ',' separated
inline constexpr char kValueProp[] = "mxValue";        // radio / value button: value it selects
inline constexpr char kAdvancedProp[] = "mxAdvanced";  // true on editors of the Advanced page

// Interface of custom bound controls (colour button, folder field, list editor, ...). Such a widget
// must also have a Qt signal `valueEdited(QVariant)` emitted when the user changes the value.
class BoundWidget {
public:
    virtual ~BoundWidget() = default;
    virtual QVariant boundValue() const = 0;             // the value the control shows
    virtual void setBoundValue(const QVariant &v) = 0;   // show v (no signal)
    virtual void setValueByUser(const QVariant &v) = 0;  // as if the user had picked v (emits)
};

class SettingsBinder : public QObject {
    Q_OBJECT
public:
    explicit SettingsBinder(mx::Settings *s, QObject *parent = nullptr);
    mx::Settings *settings() const { return m_s; }

    // Bind a standard control (QCheckBox / checkable button, QSpinBox, QDoubleSpinBox, QComboBox,
    // QLineEdit, MenuButton) or a BoundWidget to `key`. The value type comes from the schema;
    // keys outside the schema use `fallbackType`.
    QWidget *bind(QWidget *control, const QString &key, SettingType fallbackType = SettingType::String);
    // A radio button that selects `value` for `key` (all radios of a key form one group).
    void bindRadio(QRadioButton *r, const QString &key, const QVariant &value);
    // A push button that writes `value` when clicked (original WinMX "Quiet" / "Use System Beep").
    void bindValueButton(QAbstractButton *b, const QString &key, const QVariant &value);
    // A composite: `load` refreshes the widgets from Settings; the widgets call commit() themselves.
    void bindCustom(const QList<QWidget *> &tagged, const QStringList &keys, const std::function<void()> &load);

    // Validate and write (returns false and tells the user if the value is out of range).
    bool commit(const QString &key, const QVariant &value, QWidget *origin = nullptr);
    void reloadAll();
    void resetKeys(const QStringList &keys);            // "Reset Defaults" buttons
    QStringList boundKeys() const;                      // every key some control is bound to
    QStringList keysOf(const QWidget *pageRoot) const;  // keys bound inside a page
    // keys with restartRequired whose value differs from the value when the binder was created
    QStringList restartPending() const;
    void clearRestartPending() { m_restart.clear(); }

    // message boxes go through this (tests replace it); default: classic QMessageBox::warning
    using MessageHook = std::function<void(QWidget *parent, const QString &title, const QString &text)>;
    static void setMessageHook(const MessageHook &h);
    static void message(QWidget *parent, const QString &title, const QString &text);
    // Yes/No(/Cancel) questions (QMessageBox::StandardButton values); tests replace it
    using AskHook = std::function<int(QWidget *parent, const QString &title, const QString &text, int buttons, int defaultButton)>;
    static void setAskHook(const AskHook &h);
    static int ask(QWidget *parent, const QString &title, const QString &text, int buttons, int defaultButton);

    // value <-> text helpers shared with the Advanced page
    static QString listToText(const QStringList &l);
    static QStringList textToList(const QString &t);
    static QString mapToText(const QVariantMap &m);
    static bool textToMap(const QString &t, QVariantMap *out);

signals:
    void committed(const QString &key, const QVariant &value);

private:
    struct Binding {
        QStringList keys;
        std::function<void()> load;
        QList<QPointer<QWidget>> widgets;
    };
    void add(const QList<QWidget *> &widgets, const QStringList &keys, const std::function<void()> &load);
    void onChanged(const QString &key);
    void rememberInitial(const QString &key);

    mx::Settings *m_s;
    QList<Binding> m_bindings;
    QMultiHash<QString, int> m_byKey;
    QHash<QString, QVariant> m_initial;
    QSet<QString> m_restart;
    bool m_committing = false;
};

// ------------------------------------------------------------------ custom bound controls

// Push button with the element name at the left and a 30x9 colour swatch at the right
// (Settings ▸ Appearance sub-pages). Click = colour picker; the colour applies live.
class ColorButton : public PushButton, public BoundWidget {
    Q_OBJECT
public:
    explicit ColorButton(const QString &text, QWidget *parent = nullptr);
    QColor color() const { return m_color; }
    QVariant boundValue() const override { return m_color.name(); }
    void setBoundValue(const QVariant &v) override;
    void setValueByUser(const QVariant &v) override;
    // tests replace the colour dialog
    using PickHook = std::function<QColor(const QColor &current, QWidget *parent)>;
    static void setPickHook(const PickHook &h);
signals:
    void valueEdited(const QVariant &v);
protected:
    void paintEvent(QPaintEvent *) override;
private:
    void pick();
    QColor m_color;
};

// A folder (or file) shown as text with a "Change..." push button (Incoming Files Folder).
class PathField : public QWidget, public BoundWidget {
    Q_OBJECT
public:
    PathField(const QString &buttonText, bool directory, QWidget *parent = nullptr);
    QVariant boundValue() const override { return m_path; }
    void setBoundValue(const QVariant &v) override;
    void setValueByUser(const QVariant &v) override;
    void setButtonGeometry(const QRect &textRect, const QRect &buttonRect);
    void setFilter(const QString &f) { m_filter = f; }
    PushButton *button() const { return m_button; }
    using BrowseHook = std::function<QString(const QString &current, bool directory, QWidget *parent)>;
    static void setBrowseHook(const BrowseHook &h);
    static QString browse(const QString &current, bool directory, QWidget *parent, const QString &filter = QString());
signals:
    void valueEdited(const QVariant &v);
protected:
    void resizeEvent(QResizeEvent *) override;
private:
    QString m_path;
    QString m_filter;
    bool m_dir;
    bool m_manualGeometry = false;
    StaticText *m_text;
    PushButton *m_button;
};

// A push button that edits a font setting ("Change Normal Font...").
class FontButton : public PushButton, public BoundWidget {
    Q_OBJECT
public:
    explicit FontButton(const QString &text, QWidget *parent = nullptr);
    QVariant boundValue() const override { return m_value; }
    void setBoundValue(const QVariant &v) override { m_value = v.toString(); setToolTip(m_value); }
    void setValueByUser(const QVariant &v) override;
signals:
    void valueEdited(const QVariant &v);
private:
    QString m_value;
};

// White classic list with a "Name" header and New / Edit / Move Up / Add Defaults / Remove /
// Move Down buttons (Settings ▸ Library "Available Filetypes"); value = QStringList.
class StringListEditor : public QWidget, public BoundWidget {
    Q_OBJECT
public:
    explicit StringListEditor(const QStringList &defaults, QWidget *parent = nullptr);
    QVariant boundValue() const override;
    void setBoundValue(const QVariant &v) override;
    void setValueByUser(const QVariant &v) override;
    QTreeWidget *list() const { return m_list; }
    // tests replace the item text prompt
    using PromptHook = std::function<QString(const QString &title, const QString &current, QWidget *parent)>;
    static void setPromptHook(const PromptHook &h);
signals:
    void valueEdited(const QVariant &v);
protected:
    void resizeEvent(QResizeEvent *) override;
private:
    void edited();
    void updateButtons();
    QString prompt(const QString &title, const QString &current);
    QStringList m_defaults;
    QTreeWidget *m_list;
    PushButton *m_new, *m_edit, *m_up, *m_defaultsBtn, *m_remove, *m_down;
};

} // namespace mx::ui
