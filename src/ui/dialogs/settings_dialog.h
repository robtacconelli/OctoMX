// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the Settings dialog (UI_SPEC §7, §8.9) [ui-c].
//
// Modeless, fixed client size 527 x 351 (= the original's 533 x 376 outer window with the classic
// caption and dialog frame). Left: "Setup Wizard..." button, the category tree, "Close". Right:
// the page of the selected category. Changes apply immediately (the original had no OK / Cancel /
// Apply: only "Close"); settings that need a restart are announced when the dialog is closed.
//
// Pages are identified by an id ("transfers/queuing") and their tree label ("Queuing");
// showPage() accepts either. The original pages are reproduced with their layouts; the pages for
// the new networks and options use the same visual vocabulary. The "Advanced" page lists every
// schema key that no other page binds (data-driven: a key added to the schema later shows up there
// without any UI change).
#pragma once

#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtWidgets/QDialog>

class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace mx {
class Core;
}

namespace mx::ui {

class MainWindow;
class PushButton;
class SettingsBinder;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    static constexpr int kClientW = 527;
    static constexpr int kClientH = 351;
    static constexpr int kPageX = 212;      // left edge of the page area (client coordinates)

    SettingsDialog(mx::Core *core, MainWindow *mw, QWidget *parent = nullptr);
    ~SettingsDialog() override;

    mx::Core *core() const { return m_core; }
    MainWindow *mainWindow() const { return m_mw; }
    SettingsBinder *binder() const { return m_binder; }
    QTreeWidget *tree() const { return m_tree; }

    // id or tree label (case-insensitive); false if unknown
    bool showPage(const QString &idOrLabel);
    QString currentPageId() const;
    QStringList pageIds() const { return m_order; }
    QString pageLabel(const QString &id) const;
    QWidget *pageWidget(const QString &id) const { return m_pages.value(id); }
    QString resolvePage(const QString &idOrLabel) const;

    QStringList pageKeys() const;        // keys bound on the regular pages
    QStringList advancedKeys() const;    // keys listed on the Advanced page

    // page construction (used by the page builders in settings_pages*.cpp)
    QWidget *addPage(const QString &id, const QString &label, const QString &parentId = QString());

    // flush the focused editor, tell about restart-only settings; called by Close / Esc / X
    void finish();

    // message shown when settings that need a restart were changed (tests read it)
    QString restartMessage() const;

signals:
    void wizardRequested();
    void pageShown(const QString &id);

protected:
    void closeEvent(QCloseEvent *) override;
    void reject() override;
    void keyPressEvent(QKeyEvent *) override;

private:
    mx::Core *m_core;
    MainWindow *m_mw;
    SettingsBinder *m_binder;
    QTreeWidget *m_tree;
    QStackedWidget *m_stack;
    PushButton *m_wizardBtn;
    PushButton *m_closeBtn;
    QHash<QString, QWidget *> m_pages;
    QHash<QString, QTreeWidgetItem *> m_items;
    QHash<QString, QString> m_labels;
    QStringList m_order;
    QList<QTreeWidgetItem *> m_autoExpanded;   // opened by showPage(), closed again by the next one
    bool m_finishing = false;
};

} // namespace mx::ui
