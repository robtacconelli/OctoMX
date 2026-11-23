// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the Transfers page (UI_SPEC §3.6 + §8.7): download buttons (2 centred rows),
// download list (file rows + source children), the inline "<n> downloads from ..." line, a
// splitter, the upload buttons and the upload list. Every button and context-menu entry goes to
// mx::TransferService. Links can be pasted (Ctrl+V on the list, "Paste Link..."), opened
// (Load Incomplete / Open Torrent File...) or dropped onto the window.
#pragma once

#include <functional>

#include "mx/types.h"
#include "ui/pages/page.h"

class QLabel;
class QMenu;
class QSplitter;
class QTimer;

namespace mx::ui {

class MenuButton;
class MxListView;
class PushButton;
class TransferModel;
class StaticText;
class LoadIncompleteDialog;
class AddLinkDialog;

class TransfersPage : public Page {
    Q_OBJECT
public:
    TransfersPage(mx::Core *core, MainWindow *mw, QWidget *parent = nullptr);
    QStringList statusFields() const override;
    void saveState() override;

    MxListView *downloadList() const { return m_down; }
    MxListView *uploadList() const { return m_up; }
    TransferModel *downloadModel() const { return m_downModel; }
    TransferModel *uploadModel() const { return m_upModel; }
    void showDownloadMenu(const QModelIndex &viewIndex, const QPoint &globalPos);
    void showUploadMenu(const QModelIndex &viewIndex, const QPoint &globalPos);
    void pasteLinks();
    void openTorrentFile();
    void loadIncomplete();                     // "Load Incomplete": resume an incomplete file on disk
    // tests: answer Yes/No questions, the folder chooser and the Load Incomplete dialog
    static std::function<int(const QString &title, const QString &text)> s_askHook;
    static std::function<QString(QWidget *, const QString &dir)> s_dirHook;
    // [ui-e] replaces the message box of an open / show-in-folder that failed (tests)
    static std::function<void(const QString &why)> s_messageHook;
    static std::function<bool(LoadIncompleteDialog *)> s_loadIncompleteHook;
    static std::function<bool(AddLinkDialog *)> s_addLinkHook;   // [dl-1] tests: fill + accept the Add Link dialog

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    QWidget *buildDownloadHalf();
    QWidget *buildUploadHalf();
    void layoutDownloadHalf();
    void layoutUploadHalf();
    QList<mx::TransferInfo> selectedDownloads() const;   // file rows (or the parents of source rows)
    QList<mx::TransferInfo> selectedUploads() const;
    void addAfsAeqMenus(QMenu *menu, const mx::TransferInfo &t);   // [qa-1] per download
    // [dl-1] download order flags and Preview
    void addOrderEntries(QMenu *menu, const mx::TransferInfo &t, bool finished);
    void addPreviewEntry(QMenu *menu, const mx::TransferInfo &t);
    void previewTransfer(const mx::TransferInfo &t, int fileIndex);
    void cancelSelected();
    void clearSelected();
    void retrySelected();
    void findSourcesSelected();
    bool cancelSelectedSources();
    void changeDestination(const mx::TransferInfo &t);
    void startUpload(const mx::TransferInfo &t);
    void showNotice(const QString &text);
    int ask(const QString &title, const QString &text, int icon);
    void deleteFile(const mx::TransferInfo &t);
    // [ui-e] completed downloads: open the file (a bundle: its folder) / show it in the file manager
    void addLaunchEntries(QMenu *menu, const mx::TransferInfo &t, bool finished);   // [ui-h]
    void openTransfer(const mx::TransferInfo &t);
    void showTransferInFolder(const mx::TransferInfo &t);
    void launchFailed(const QString &why);
    void searchAlternates(const mx::TransferInfo &t);
    void selectBundleFiles(const mx::TransferInfo &t);
    void updateSummary();
    void updateButtons();

    QSplitter *m_split;
    QWidget *m_downHalf;
    QWidget *m_upHalf;
    QList<PushButton *> m_rowC, m_rowD, m_upRow;
    MenuButton *m_afs;
    MenuButton *m_aeq;
    PushButton *m_retry;
    MxListView *m_down;
    MxListView *m_up;
    TransferModel *m_downModel;
    TransferModel *m_upModel;
    StaticText *m_summary;
    QTimer *m_summaryTimer;
    QString m_notice;                          // transient status-bar note (8 s)
    bool m_splitRestored = false;
};

} // namespace mx::ui
