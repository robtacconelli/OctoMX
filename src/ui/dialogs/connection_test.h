// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: "Connection Test" [diag-1] (UI_SPEC 8.11): ports, port mapping, reachability and the
// "why is this download slow" diagnosis, from mx::DiagnosticsService. Classic gray window, black
// tree list (Check | Result | Details) grouped by General / network / download, coloured green
// (fine), yellow (works with limits), red (broken), white (unknown), gray (off). The box below the
// list explains the selected row and says what to do. Buttons: Test Now (cancellable, progress on
// top), Check Ports Online, Copy Report, Save Report..., Open Log Folder, Close; "Hide my IP
// addresses in the report" (diag/hideAddresses). One window per application (open()).
// Script verbs: open-conntest [transferId], conntest-test, conntest-select <rowId | first-download>,
// conntest-online, conntest-save <file>.
#pragma once

#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtWidgets/QDialog>

#include <functional>

#include "mx/diagnostics.h"

class QCheckBox;
class QPlainTextEdit;
class QProgressBar;
class QStandardItemModel;
class QStandardItem;

namespace mx {
class Core;
}

namespace mx::ui {

class MxListView;
class PushButton;
class StaticText;

class ConnectionTestDialog : public QDialog {
    Q_OBJECT
public:
    ConnectionTestDialog(mx::Core *core, QWidget *parent = nullptr);
    ~ConnectionTestDialog() override;

    // Show the one Connection Test window (created on demand), raised; with a transfer id the
    // download's group is selected and scrolled to ("Why Is This Slow?").
    static ConnectionTestDialog *open(mx::Core *core, QWidget *parent, const QString &downloadId = QString());
    static ConnectionTestDialog *instance();

    void selectDownload(const QString &transferId);
    bool selectRow(const QString &id);        // group or item id
    QString reportText() const;               // what Copy Report copies
    MxListView *list() const { return m_list; }
    QStandardItemModel *model() const { return m_model; }
    QString explanation() const;
    QString summaryText() const;
    void refresh();

    // tests: replace the save file dialog / the folder opener
    static std::function<QString(QWidget *, const QString &suggested)> s_saveHook;
    static std::function<void(const QString &path)> s_openFolderHook;

protected:
    void resizeEvent(QResizeEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    void onSelection();
    void onTestProgress(int percent, const QString &step);
    void onTestFinished(bool cancelled);
    void testNow();
    void copyReport();
    void saveReport();
    void openLogFolder();
    void checkOnline();
    void updateButtons();
    QColor levelColor(mx::DiagLevel l) const;
    QList<QStandardItem *> makeRow(const QString &id, const QString &check, const QString &result, const QString &details,
                                   mx::DiagLevel level, const QString &explain, bool group);

    mx::Core *m_core;
    mx::DiagReport m_report;
    StaticText *m_summary;
    QWidget *m_lamp;
    StaticText *m_progressText;
    QProgressBar *m_progress;
    MxListView *m_list;
    QStandardItemModel *m_model;
    QPlainTextEdit *m_explain;
    QCheckBox *m_hide;
    mx::AddressMasker m_windowMask;   // [ui-h] the same masking in the window while m_hide is ticked
    PushButton *m_test, *m_online, *m_copy, *m_save, *m_logs, *m_close;
    QString m_selected;      // id of the selected row (kept across refreshes)
    QSet<QString> m_collapsed;
    bool m_restored = false;
    bool m_columnsSet = false;
};

} // namespace mx::ui
