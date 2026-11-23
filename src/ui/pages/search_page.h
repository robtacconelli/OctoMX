// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: the Search page (UI_SPEC §3.4 + §8.4).
//
// Row C is the tab strip of open searches ("Open New Search" + one tab per search). Each tab is a
// SearchTab: the fixed ~702-px control block (two keyword lines with the vertical CLEAR button,
// Search, Download, Manually Resume..., Auto Find Sources / Auto Enter Queue, Audio / Video /
// Pictures, file-type / network / connection menu buttons, Search In New Window, Available Files On
// Top, Show Full Paths, Stop, Close, Ping buttons), the colour legend and the result list.
//
// One click: type words, press Enter -> one merged list from all networks; double-click (or Enter)
// on a row -> TransferService::downloadHit.
#pragma once

class QAction;

#include <QtCore/QPointer>
#include <QtCore/QSet>

#include <functional>

#include "mx/types.h"
#include "ui/pages/page.h"

class QLabel;
class QLineEdit;
class QStackedWidget;
class QTimer;
class QCompleter;
class QStringListModel;

namespace mx {
class Settings;
}

namespace mx::ui {

class LegendItem;
class MenuButton;
class MxListView;
class PushButton;
class SearchModel;
class SearchProxy;
class TabStrip;
class VerticalButton;
class StaticText;
class FileDownloadDialog;
class FilterRow;

class SearchTab : public QWidget {
    Q_OBJECT
public:
    SearchTab(mx::Core *core, MainWindow *mw, QWidget *parent = nullptr);
    ~SearchTab() override;

    QString label() const;                       // "\"velorine\" \"\""
    QStringList statusFields() const;
    MxListView *list() const { return m_list; }
    SearchModel *model() const { return m_model; }
    SearchProxy *proxy() const { return m_proxy; }
    quint32 searchId() const { return m_id; }
    bool isRunning() const { return m_running; }
    QLineEdit *keywords() const { return m_edit1; }

    void setTexts(const QString &t1, const QString &t2);
    void startSearch();                          // "Search" button / Enter
    void stopSearch();
    void downloadSelected();
    void downloadIndex(const QModelIndex &viewIndex);
    void showContextMenu(const QModelIndex &viewIndex, const QPoint &globalPos);
    void flushPending();                         // apply queued hits now (tests)
    void manuallyResumeSelected();               // "Manually Resume..." (File Download dialog, UI_SPEC §5.7)
    void ping(bool allResults);                  // Ping On-Screen / All Results -> SearchService::pingHits
    // "Load Incomplete": the next double-clicked hit resumes from this local file (empty = off)
    void setResumeFile(const QString &path);
    QString resumeFile() const { return m_resumeFile; }
    bool isHashSearch() const { return m_isHashSearch; }
    // The search for a pasted link / raw hash / "HASH>" line, or empty for a keyword search.
    static QString hashQueryFor(const QString &t1, const QString &t2);
    static bool looksLikeHashOrLink(const QString &s);
    static QString hashDisplayName(const QString &t1, const QString &t2);   // tab label of a hash search
    static mx::DownloadRequest requestFor(const mx::SearchHit &h);
    // ui/resumedFiles: transfer id -> the local file a manual resume uses in place
    static void rememberResumedFile(mx::Settings *s, const QString &transferId, const QString &file);
    // tests: answer the Yes/No warnings (dangerous / suspected fake) and the File Download dialog
    static std::function<int(const QString &title, const QString &text)> s_askHook;
    static std::function<bool(FileDownloadDialog *)> s_resumeDialogHook;
    // [ui-f] the file-type filter: a code of mx::FileTypeFilter ("any", "audio:min:192", "custom:mkv,srt")
    QString fileTypeCode() const;
    void setFileTypeCode(const QString &code);   // as if chosen (persists, updates the quick buttons)
    // tests: answer "Custom Extensions..." (return false = Cancel; *text = what was typed)
    static std::function<bool(QString *text)> s_customExtensionsHook;
    // [ui-g] the always visible "Filter results:" row above the result list
    FilterRow *filterRow() const { return m_filterRow; }
    // [ui-g] the form uses the free width of a wide window (more quick type buttons, wider fields)
    bool isWideForm() const { return m_wideForm; }
    static constexpr int kWideFormWidth = 975;   // design px of page width from which the form is wide
    // [ui-g] the text above the two keyword fields for a file-type code ("Artist and / or Title" for audio)
    static QString searchLabelFor(const QString &typeCode);
    QString searchLabel() const;

signals:
    void labelChanged();
    void statusChanged();
    void newWindowRequested(const QString &t1, const QString &t2);
    void closeRequested();

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    void showHistoryMenu();   // [ui-e]
    QAction *m_historyAction = nullptr;
    QString localPathFor(const mx::SearchHit &h) const;   // [ui-e] the file on this computer, if any
    void buildControls();
    void onHits(quint32 id, const QList<mx::SearchHit> &hits);
    void onFinished(quint32 id);
    void onRemoved(quint32 id, const QStringList &keys);
    void openBundleDialog(const mx::SearchHit &h);
    void setFileType(const QString &code);
    void applyFileTypeToQuery(mx::SearchQuery &q) const;
    void onFileTypeChosen(const QVariant &data);   // [ui-f]
    void askCustomExtensions(const QString &previousCode);
    void updateFileTypeButton();                   // fitted text, tool tip, pressed quick button
    void updateCompleter();
    void persistFilters();
    void updateTypeButtons();
    bool confirmDownload(const mx::SearchHit &h);
    int ask(const QString &title, const QString &text);
    bool resumeFrom(const mx::SearchHit &h, const QString &file);
    void markDownloading(const QModelIndex &src);

    mx::Core *m_core;
    MainWindow *m_main;
    // controls
    StaticText *m_label;
    VerticalButton *m_clear;
    QLineEdit *m_edit1;
    QLineEdit *m_edit2;
    PushButton *m_search;
    PushButton *m_download;
    PushButton *m_resume;
    MenuButton *m_afs;
    MenuButton *m_aeq;
    PushButton *m_all;                           // [ui-f] All Files
    PushButton *m_audio;
    PushButton *m_video;
    PushButton *m_pictures;
    MenuButton *m_fileType;
    PushButton *m_newWindow;
    PushButton *m_availTop;
    PushButton *m_fullPaths;
    MenuButton *m_networks;
    MenuButton *m_connection;
    PushButton *m_stop;
    PushButton *m_close;
    PushButton *m_pingScreen;
    PushButton *m_pingAll;
    QList<LegendItem *> m_legend;
    QList<PushButton *> m_moreTypes;             // [ui-g] Documents | Software | Archives | Disc Images (wide form)
    FilterRow *m_filterRow = nullptr;            // [ui-g]
    bool m_wideForm = false;
    void updateSearchLabel();
    MxListView *m_list;
    SearchModel *m_model;
    SearchProxy *m_proxy;
    QCompleter *m_completer;
    QStringListModel *m_historyModel;
    // state
    quint32 m_id = 0;
    bool m_running = false;
    bool m_everSearched = false;
    bool m_stopped = false;
    QString m_t1, m_t2;                          // texts of the running/last search
    QList<mx::SearchHit> m_pending;
    QTimer *m_flush;
    QString m_bundleDialogKey;                   // waiting for requestBundleFiles() results
    mx::SearchHit m_bundlePendingHit;            // [qa-1] the hit waiting for its file list
    QTimer *m_bundleTimer = nullptr;             // [qa-1] no answer at all: download every file
    void startWholeBundle(const QString &why);
    QString m_resumeFile;                        // Load Incomplete: resume this file from the next hit
    QSet<QString> m_suspectConfirmed;            // suspected fakes the user chose to download anyway
    bool m_isHashSearch = false;
    QStringList m_customExts;                    // [ui-f] the extensions behind the "custom" entry
    QString m_typeCode = QStringLiteral("any");  // [ui-f] the current file-type code
    QString m_hashName;
};

class SearchPage : public Page {
    Q_OBJECT
public:
    SearchPage(mx::Core *core, MainWindow *mw, QWidget *parent = nullptr);
    QStringList statusFields() const override;
    void activated() override;
    bool selectSubTab(const QString &name) override;

    SearchTab *currentTab() const;
    SearchTab *tab(int i) const;
    int tabCount() const;
    TabStrip *tabStrip() const { return m_tabs; }
    SearchTab *newTab(bool makeCurrent = true);
    void closeTab(SearchTab *t);
    void searchInNewTab(const QString &t1, const QString &t2);
    void saveState() override;

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    void updateLabels();
    TabStrip *m_tabs;
    QStackedWidget *m_stack;
    QList<SearchTab *> m_tabList;
};

} // namespace mx::ui
