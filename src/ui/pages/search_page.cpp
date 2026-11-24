// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/pages/search_page.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtGui/QClipboard>
#include <QtCore/QRegularExpression>
#include <QtGui/QShortcut>
#include <QtGui/QGuiApplication>
#include <QtCore/QStringListModel>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QCompleter>
#include <QtGui/QPainter>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QStyle>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStackedWidget>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/dialogs/simple_dialogs.h"
#include "ui/dialogs/file_download_dialog.h"
#include "ui/kit/tips.h"
#include "ui/kit/colors.h"
#include "ui/kit/file_types.h"
#include "ui/kit/list_view.h"
#include "ui/kit/session.h"
#include "ui/kit/widgets.h"
#include "ui/kit/launcher.h"
#include "ui/kit/links.h"
#include "ui/kit/scale.h"
#include "ui/kit/text_filter.h"
#include "ui/main_window.h"
#include "ui/models/search_model.h"

namespace mx::ui {

namespace {
// file-type buttons: caption key (a UI-side setting, editable in Settings), type key, default caption
const struct { const char *labelKey; const char *typeKey; const char *def; } kTypeButtons[] = {
    {"search/typeButtonAudioLabel", "search/typeButtonAudio", QT_TR_NOOP("Audio")},
    {"search/typeButtonVideoLabel", "search/typeButtonVideo", QT_TR_NOOP("Video")},
    {"search/typeButtonPicturesLabel", "search/typeButtonPictures", QT_TR_NOOP("Pictures")},
};

// links that download directly (no hash to look up): http(s) URLs and .torrent files
bool looksLikeDirectLink(const QString &s)
{
    return ((s.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) ||
             s.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) && !s.contains(QLatin1Char(' '))) ||
           (s.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive) && QFileInfo(s).isFile());
}

// [set-1] one list for every warning (download from Search or Browse, opening a file)
bool dangerous(const QString &name)
{
    return launcher::isDangerousFile(name);
}

// Keyword edit: Enter starts the search (the completer must not swallow it).
class KeywordEdit : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    std::function<void()> onEnter;
    std::function<void()> onHistory;   // [ui-e] Alt+Down / F4: the recent searches menu

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        if (onHistory && (e->key() == Qt::Key_F4 || (e->key() == Qt::Key_Down && (e->modifiers() & Qt::AltModifier)))) {
            onHistory();
            return;
        }
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && onEnter) {
            if (completer() && completer()->popup() && completer()->popup()->isVisible())
                completer()->popup()->hide();
            onEnter();
            return;
        }
        QLineEdit::keyPressEvent(e);
    }
};
} // namespace

// ------------------------------------------------------------------------------------ SearchTab

SearchTab::SearchTab(mx::Core *core, MainWindow *mw, QWidget *parent) : QWidget(parent), m_core(core), m_main(mw)
{
    setObjectName(QStringLiteral("searchTab"));
    buildControls();

    m_model = new SearchModel(this);
    // [set-1] a new tab shows the paths when the setting is on (the button was latched, the list
    // showed names only)
    m_model->setShowFullPaths(m_fullPaths->isChecked());
    m_proxy = new SearchProxy(this);
    m_proxy->setSourceModel(m_model);
    m_list = new MxListView(this);
    m_list->setObjectName(QStringLiteral("searchResults"));
    m_list->setTreeMode(true);
    m_list->setModel(m_proxy);
    // [ui-g] the always visible results filter (was the hidden Ctrl+F bar): name, network, status, user
    m_filterRow = new FilterRow(tr("Filter results:"), QStringLiteral("searchFilter"), this);
    m_list->setFilterColumns({search::File, search::User, search::Network, search::Status});
    m_filterRow->attach(m_list);
    connect(m_list, &MxListView::filterApplied, this, &SearchTab::statusChanged);
    // [qa-1] UI_SPEC 3.4: File = "rest, >= 358", Status / Server 87 (search_idle_354b4: at 1024 px File is
    // 486 px and the columns end ~30 px before the frame, room kept for the scroll bar)
    // [ui-e] File | Size | Time | Bitrate | Freq | Network | Peers | Status (+ the classic User / Speed /
    // Ping, hidden until chosen from the header menu); File takes the slack of a wider window
    m_list->setDefaultColumnWidths({358, 80, 49, 46, 41, 112, 52, 150, 80, 47, 47});
    m_list->setDefaultHiddenColumns({search::User, search::Speed, search::Ping});
    m_list->setFillColumns({search::File, search::Status});
    m_list->setFillWeights({4, 1});
    // [ui-g] no reserved strip for the scroll bar any more: the columns always end at the list's edge
    m_list->setStateKey(core->settings(), QStringLiteral("search"));
    m_model->setFlagSuspects(core->settings()->b(keys::search::FlagSuspects));
    m_proxy->setHideSuspects(core->settings()->b(keys::search::HideSuspects));
    m_proxy->setMinConnectionRank(m_connection->currentData().toInt());   // search/defaultConnection
    connect(m_list, &MxListView::rowActivated, this, &SearchTab::downloadIndex);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &SearchTab::statusChanged);
    connect(m_list, &MxListView::contextMenuAt, this, &SearchTab::showContextMenu);
    // [ui-e] Ctrl+V anywhere on the page outside the text boxes: a pasted magnet: / ed2k:// / http link or
    // .torrent path starts downloading (in the keyword box a link still searches by hash)
    auto *paste = new QShortcut(QKeySequence::Paste, this, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
    connect(paste, &QShortcut::activated, this, [this] {
        const QString text = QGuiApplication::clipboard()->text().trimmed();
        QStringList links;
        for (const QString &l : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts))
            links << l.trimmed();
        if (links.isEmpty()) {
            m_main->showNotice(tr("The clipboard holds no link to download."));
            return;
        }
        m_main->addLinks(links);
    });

    m_flush = new QTimer(this);
    m_flush->setSingleShot(true);
    m_flush->setInterval(250);
    connect(m_flush, &QTimer::timeout, this, &SearchTab::flushPending);

    SearchService *s = core->search();
    connect(s, &SearchService::hitsChanged, this, &SearchTab::onHits);
    connect(s, &SearchService::searchFinished, this, &SearchTab::onFinished);
    connect(s, &SearchService::hitsRemoved, this, &SearchTab::onRemoved);
    // [qa-1] a download action must create a Transfers row or say why not: when the file list of a
    // bundle (e.g. an index-only torrent from a web index) cannot be fetched, download every file
    m_bundleTimer = new QTimer(this);
    m_bundleTimer->setSingleShot(true);
    m_bundleTimer->setInterval(30000);
    connect(m_bundleTimer, &QTimer::timeout, this, [this] { startWholeBundle(tr("no answer")); });
    connect(s, &SearchService::bundleFilesUnavailable, this, [this](quint32 id, const QString &key, const QString &reason) {
        if (id == m_id && !m_bundleDialogKey.isEmpty() && key == m_bundleDialogKey)
            startWholeBundle(reason);
    });
    // [ui-h] Settings > Search "Groups of duplicate results expanded by default": a result that several
    // users have (two or more source rows) opens by itself; bundles (file lists) stay closed
    auto expandGroups = [this](const QModelIndex &parent, int first, int last) {
        if (!m_core->settings()->b(keys::search::GroupsExpanded))
            return;
        const QModelIndex top = parent.isValid() ? parent : QModelIndex();
        for (int r = parent.isValid() ? 0 : first; r <= (parent.isValid() ? 0 : last); ++r) {
            const QModelIndex vi = parent.isValid() ? top : m_proxy->index(r, 0);
            const QModelIndex src = m_list->mapToSource(vi);
            if (!src.isValid() || src.parent().isValid() || m_list->isExpanded(vi))
                continue;
            const SearchHit &h = m_model->hitAt(src.row());
            if (!h.isBundle && m_proxy->rowCount(vi) >= 2)
                m_list->expand(vi);
        }
    };
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, expandGroups);
    connect(m_proxy, &QAbstractItemModel::modelReset, this, [this, expandGroups] {
        if (m_proxy->rowCount() > 0)
            expandGroups(QModelIndex(), 0, m_proxy->rowCount() - 1);
    });
    connect(m_proxy, &QAbstractItemModel::layoutChanged, this, [this, expandGroups] {
        if (m_proxy->rowCount() > 0)
            expandGroups(QModelIndex(), 0, m_proxy->rowCount() - 1);
    });
    // expanding a bundle row whose file list is unknown asks the core for it
    connect(m_list, &QTreeView::expanded, this, [this](const QModelIndex &vi) {
        const QModelIndex src = m_list->mapToSource(vi);
        if (!src.isValid() || src.parent().isValid())
            return;
        const SearchHit &h = m_model->hitAt(src.row());
        if (h.isBundle && h.files.isEmpty() && m_id)
            m_core->search()->requestBundleFiles(m_id, h.key);
    });
}

SearchTab::~SearchTab()
{
    if (m_id)
        m_core->search()->closeSearch(m_id);
}

void SearchTab::buildControls()
{
    Settings *st = m_core->settings();
    m_label = new StaticText(tr("Artist and / or Title"), this);
    m_clear = new VerticalButton(tr("CLEAR"), this);
    m_clear->setObjectName(QStringLiteral("searchClear"));
    auto *e1 = new KeywordEdit(this);
    auto *e2 = new KeywordEdit(this);
    e1->onEnter = [this] { startSearch(); };
    e2->onEnter = [this] { startSearch(); };
    m_edit1 = e1;
    m_edit2 = e2;
    m_edit1->setTextMargins(-1, 0, 0, 0);
    m_edit2->setTextMargins(-1, 0, 0, 0);
    m_edit1->setObjectName(QStringLiteral("searchText"));
    m_edit2->setObjectName(QStringLiteral("searchText2"));
    connect(m_clear, &QAbstractButton::clicked, this, [this] {
        m_edit1->clear();
        m_edit2->clear();
        if (m_list)
            m_list->hideFilter();   // [ui-g] the results filter too
        m_edit1->setFocus();
    });
    m_clear->setToolTip(tr("Empty both search fields and the results filter, to type a new search"));

    // [ui-e] recent searches: a drop-down arrow in the keyword box (and Alt+Down), "Clear Search History"
    {
        const int sz = S(16);
        QPixmap arrow(sz, sz);
        arrow.fill(Qt::transparent);
        QPainter pa(&arrow);
        const int w = (S(7) | 1), h = w / 2 + 1, x0 = (sz - w) / 2, y0 = (sz - h) / 2 + 1;
        for (int i = 0; i < h; ++i)
            pa.fillRect(QRect(x0 + i, y0 + i, w - 2 * i, 1), chrome::Text);
        pa.end();
        m_historyAction = m_edit1->addAction(QIcon(arrow), QLineEdit::TrailingPosition);
        m_historyAction->setObjectName(QStringLiteral("searchHistoryButton"));
        m_historyAction->setToolTip(tr("Recent searches"));
        connect(m_historyAction, &QAction::triggered, this, &SearchTab::showHistoryMenu);
        e1->onHistory = [this] { showHistoryMenu(); };
        m_historyAction->setVisible(m_core->settings()->b(keys::ui::SearchHistoryMenu));
        connect(m_core->settings(), &Settings::changed, this, [this](const QString &key, const QVariant &v) {
            if (key == QLatin1String(keys::ui::SearchHistoryMenu))
                m_historyAction->setVisible(v.toBool());
        });
    }

    m_historyModel = new QStringListModel(this);
    m_completer = new QCompleter(m_historyModel, this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    updateCompleter();

    auto mk = [this](const QString &text, const char *name) {
        auto *b = new PushButton(text, this);
        b->setObjectName(QString::fromLatin1(name));
        return b;
    };
    m_search = mk(tr("Search"), "searchGo");
    m_download = mk(tr("Download"), "searchDownload");
    m_resume = mk(tr("Manually Resume..."), "searchManualResume");
    m_newWindow = mk(tr("Search In New Window"), "searchNewWindow");
    m_availTop = mk(tr("Available Files On Top"), "searchAvailOnTop");
    m_fullPaths = mk(tr("Show Full Paths"), "searchFullPaths");
    m_stop = mk(tr("Stop"), "searchStop");
    m_close = mk(tr("Close"), "searchClose");
    m_pingScreen = mk(tr("Ping On-Screen Results"), "searchPingScreen");
    m_pingAll = mk(tr("Ping All Results"), "searchPingAll");
    m_all = mk(tr("All"), "searchAll");   // [ui-f] All Files, first of the quick buttons
    m_audio = mk(tr("Audio"), "searchAudio");
    m_video = mk(tr("Video"), "searchVideo");
    m_pictures = mk(tr("Pictures"), "searchPictures");
    // [ui-g] more quick types on a wide window (the owner: "add also archive and cd-image")
    m_moreTypes << mk(tr("Documents"), "searchDocuments") << mk(tr("Software"), "searchSoftware")
                << mk(tr("Archives"), "searchArchives") << mk(tr("Disc Images"), "searchDiscImages");
    for (PushButton *b : {m_all, m_audio, m_video, m_pictures})
        b->setCheckable(true);   // the button of the current filter looks pressed (latched)
    {
        static const char *const codes[] = {"documents", "software", "archives", "discimages"};
        for (int i = 0; i < m_moreTypes.size(); ++i) {
            PushButton *b = m_moreTypes[i];
            const QString code = QString::fromLatin1(codes[i]);
            b->setCheckable(true);
            b->setProperty("typeCode", code);
            b->setToolTip(tr("Search only for this kind of file: %1").arg(filetypes::description(code)));
            b->hide();
            connect(b, &QPushButton::clicked, this, [this, code] { setFileTypeCode(code); });
        }
    }
    m_all->setToolTip(filetypes::description(QStringLiteral("any")));
    m_availTop->setCheckable(true);
    m_fullPaths->setCheckable(true);
    m_fullPaths->setChecked(st->b(keys::search::ShowFullPaths));

    // [ui-g] what each button does (item 5 of the ui-g brief)
    m_search->setToolTip(tr("Ask every connected network for files whose names contain all the words typed on the left. "
                            "A magnet, ed2k or http link, or a file hash, searches for that one file."));
    m_download->setToolTip(tr("Start downloading the selected results; they appear on the Transfers page. "
                              "Double-clicking a result does the same."));
    m_resume->setToolTip(tr("Continue a partial file you already have (for example from another program) with the selected "
                            "result as its source: choose the partial file, OctoMX checks and completes it."));
    m_newWindow->setToolTip(tr("Run this search in a new tab and keep the results of the current one"));
    m_availTop->setToolTip(tr("Sort the files that can be downloaded right now (free sources, seeds) to the top of the list"));
    m_fullPaths->setToolTip(tr("Show the folder each file is in on the sharing computer, not only its name"));
    m_stop->setToolTip(tr("Stop asking the networks; the results found so far stay in the list"));
    m_close->setToolTip(tr("Close this search tab and its results"));
    m_pingScreen->setToolTip(tr("Measure the response time of the users in the visible rows. Lower is better, "
                                "and users who do not answer are probably offline."));
    m_pingAll->setToolTip(tr("Measure the response time of the users of every result, also those scrolled out of view. "
                             "Takes longer with many results."));
    connect(m_search, &QPushButton::clicked, this, &SearchTab::startSearch);
    connect(m_download, &QPushButton::clicked, this, &SearchTab::downloadSelected);
    connect(m_resume, &QPushButton::clicked, this, &SearchTab::manuallyResumeSelected);
    connect(m_newWindow, &QPushButton::clicked, this, [this] { emit newWindowRequested(m_edit1->text(), m_edit2->text()); });
    connect(m_availTop, &QPushButton::toggled, this, [this](bool on) { m_proxy->setAvailableOnTop(on); });
    connect(m_fullPaths, &QPushButton::toggled, this, [this](bool on) {
        m_model->setShowFullPaths(on);
        m_core->settings()->setValue(keys::search::ShowFullPaths, on);
    });

    connect(m_stop, &QPushButton::clicked, this, &SearchTab::stopSearch);
    connect(m_close, &QPushButton::clicked, this, &SearchTab::closeRequested);
    connect(m_pingScreen, &QPushButton::clicked, this, [this] { ping(false); });
    connect(m_pingAll, &QPushButton::clicked, this, [this] { ping(true); });

    // Auto Find Sources / Auto Enter Queue (UI_SPEC §3.4)
    m_afs = new MenuButton(this);
    m_afs->setObjectName(QStringLiteral("searchAfs"));
    m_afs->addChoice(tr("Never"), 0, tr("Auto Find Sources - Never"));
    for (int m : {10, 15, 20, 25, 30, 40, 50, 60})
        m_afs->addChoice(tr("Every %1 min").arg(m), m, tr("Auto Find Sources - %1m").arg(m));
    m_afs->setCurrentData(st->i(keys::search::AutoFindSourcesMin));
    m_afs->setToolTip(tr("While a download started from here still misses parts, search again for more sources every "
                         "few minutes (here the interval). Click to change the interval, or choose Never."));
    connect(m_afs, &MenuButton::choiceChanged, this, [this](const QVariant &v) {
        m_core->settings()->setValue(keys::search::AutoFindSourcesMin, v.toInt());
        if (v.toInt() > 0)
            m_core->settings()->setValue(keys::transfers::SourceSearchIntervalMin, v.toInt());
    });
    m_aeq = new MenuButton(this);
    m_aeq->setObjectName(QStringLiteral("searchAeq"));
    m_aeq->addChoice(tr("Never"), 0, tr("Auto Enter Queue - Never"));
    for (int n : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 30, 35, 40, 45, 50, 60, 70, 80, 90, 100})
        m_aeq->addChoice(QString::number(n), n, tr("Auto Enter Queue - %1").arg(n));
    m_aeq->setCurrentData(st->i(keys::search::AutoEnterQueue));
    m_aeq->setToolTip(tr("When a user's uploads are busy, wait in their queue only if your place is at most this number. "
                         "A longer queue is left when the download has other sources to use. Click to change it."));
    connect(m_aeq, &MenuButton::choiceChanged, this, [this](const QVariant &v) {
        m_core->settings()->setValue(keys::search::AutoEnterQueue, v.toInt());
    });

    // file type (UI_SPEC §3.4 + §8.4, [ui-f] §8.11): labels generated from the type table
    m_fileType = new MenuButton(this);
    m_fileType->setObjectName(QStringLiteral("searchFileType"));
    filetypes::populateMenu(m_fileType, true);
    m_fileType->setTextFitter([this](const QVariant &, const QFontMetrics &fm, int room) {
        return filetypes::fittedButtonText(m_typeCode, fm, room);   // also "custom:mkv,srt" behind the "custom" entry
    });
    for (QMenu *menu : filetypes::menusOf(m_fileType))
        connect(menu, &QMenu::hovered, this, [this](QAction *a) {
            if (!a->statusTip().isEmpty())
                m_main->showNotice(a->statusTip());   // the whole extension list in the status bar
        });
    m_customExts = parseExtensionList(st->s(keys::search::CustomExtensions));
    // the last type used is the default of the next search (search/defaultType holds the code)
    setFileType(fileTypeCodeForText(st->s(keys::search::DefaultType)));
    connect(m_fileType, &MenuButton::choiceChanged, this, &SearchTab::onFileTypeChosen);
    updateTypeButtons();
    const QList<PushButton *> typeBtns = {m_audio, m_video, m_pictures};
    for (int i = 0; i < 3; ++i)
        connect(typeBtns[i], &QPushButton::clicked, this, [this, i] {
            const QString text = m_core->settings()->s(QString::fromLatin1(kTypeButtons[i].typeKey));
            setFileTypeCode(fileTypeCodeForText(text));
        });
    connect(m_all, &QPushButton::clicked, this, [this] { setFileTypeCode(QStringLiteral("any")); });

    // networks (UI_SPEC §8.4)
    m_networks = new MenuButton(this);
    m_networks->setObjectName(QStringLiteral("searchNetworks"));
    m_networks->addChoice(tr("All Available Networks"), QString());
    m_networks->addSeparator();
    m_networks->addChoice(tr("OctoMX Peer Network"), netName(Net::Overlay));
    m_networks->addChoice(tr("BitTorrent (DHT)"), netName(Net::BitTorrent));
    m_networks->addChoice(tr("eD2k Servers"), netName(Net::ED2K));
    m_networks->addChoice(tr("Kad"), netName(Net::Kad));
    m_networks->addChoice(tr("Gnutella"), netName(Net::Gnutella));
    m_networks->addChoice(tr("Web Torrent Indexes"), netName(Net::Web));
    m_networks->setCurrentData(st->s(keys::ui::SearchNetworkFilter));
    m_networks->setToolTip(tr("Which network the next search asks: all networks that are online, or only one of them"));
    connect(m_networks, &MenuButton::choiceChanged, this, [this](const QVariant &v) {
        m_core->settings()->setValue(keys::ui::SearchNetworkFilter, v.toString());
    });

    // connection filter
    m_connection = new MenuButton(this);
    m_connection->setObjectName(QStringLiteral("searchConnection"));
    m_connection->addChoice(connectionTextForRank(0), 0);
    m_connection->addSeparator();
    for (int r = kConnectionSpeedRankMax; r >= 1; --r)   // [qa-1] modern connection types, fastest first
        m_connection->addChoice(connectionTextForRank(r), r);
    m_connection->setCurrentData(connectionRankForText(st->s(keys::search::DefaultConnection)));
    m_connection->setToolTip(tr("Hide results from users whose declared connection is slower than this. "
                                "Results without a declared speed (most BitTorrent and eD2k) always stay."));
    connect(m_connection, &MenuButton::choiceChanged, this, [this](const QVariant &v) {
        m_proxy->setMinConnectionRank(v.toInt());
        m_core->settings()->setValue(keys::search::DefaultConnection, connectionTextForRank(v.toInt()));
    });

    // the same keys change from the Settings dialog / another search tab
    connect(st, &Settings::changed, this, [this](const QString &k, const QVariant &v) {
        if (k == keys::search::AutoFindSourcesMin)
            m_afs->setCurrentData(v.toInt());
        else if (k == keys::search::AutoEnterQueue)
            m_aeq->setCurrentData(v.toInt());
        else if (k == keys::search::DefaultType && !m_everSearched && fileTypeCodeForText(v.toString()) != m_typeCode)
            setFileType(fileTypeCodeForText(v.toString()));
        else if (k == keys::search::DefaultConnection && !m_everSearched) {
            m_connection->setCurrentData(connectionRankForText(v.toString()));
            m_proxy->setMinConnectionRank(m_connection->currentData().toInt());
        } else if (k.startsWith(QLatin1String("search/typeButton")))
            updateTypeButtons();
        else if (k == keys::search::FlagSuspects)
            m_model->setFlagSuspects(v.toBool());
        else if (k == keys::search::HideSuspects)
            m_proxy->setHideSuspects(v.toBool());
        else if (k == keys::search::ShowFullPaths && m_fullPaths->isChecked() != v.toBool())
            m_fullPaths->setChecked(v.toBool());   // [set-1] from the Settings dialog: every tab
        else if (k == keys::search::AutoComplete)
            updateCompleter();                     // [set-1] at once, not at the next search
    });

    m_legend << new LegendItem(QString(keys::colors::MatchIncomplete), tr("Matches incomplete local file"), this)
             << new LegendItem(QString(keys::colors::CanJoinDownload), tr("Can be added to running multi-point download"), this)
             << new LegendItem(QString(keys::colors::CurrentlyDownloading), tr("Currently downloading"), this);
}

void SearchTab::persistFilters()
{
    m_core->settings()->setValue(keys::search::DefaultType, m_typeCode);
    // the coarse media type of the last search (ui/searchType, schema enum)
    SearchQuery q;
    applyFileTypeToQuery(q);
    static const char *const types[] = {"any", "audio", "video", "image", "document", "software", "archive", "any"};
    m_core->settings()->setValue(keys::ui::SearchType, QString::fromLatin1(types[qBound(0, int(q.type), 7)]));
}

QString SearchTab::fileTypeCode() const
{
    return m_typeCode;
}

void SearchTab::setFileTypeCode(const QString &code)
{
    setFileType(code);
    persistFilters();
    m_main->showNotice(filetypes::description(m_typeCode));
}

void SearchTab::setFileType(const QString &code)
{
    FileTypeFilter f = FileTypeFilter::fromCode(code);
    if (!f.isValid() || (f.kind == QLatin1String("custom") && f.custom.isEmpty()))
        f = FileTypeFilter::fromCode(QStringLiteral("any"));
    QString data = f.code();
    if (f.kind == QLatin1String("custom")) {
        m_customExts = f.custom;
        data = QStringLiteral("custom");
        for (QAction *a : m_fileType->menu()->actions())
            if (a->data().toString() == data)
                a->setProperty("buttonText", filetypes::buttonText(f.code()));
    }
    m_fileType->setCurrentData(data);
    if (m_fileType->currentData().toString() != data) {   // not in the menu (cannot happen for a valid code)
        f = FileTypeFilter::fromCode(QStringLiteral("any"));
        m_fileType->setCurrentData(f.code());
    }
    m_typeCode = f.code();
    updateFileTypeButton();
}

void SearchTab::onFileTypeChosen(const QVariant &data)
{
    const QString previous = m_typeCode;
    if (data.toString() == QLatin1String("custom")) {
        // the dialog runs after the menu has closed
        QTimer::singleShot(0, this, [this, previous] { askCustomExtensions(previous); });
        return;
    }
    setFileTypeCode(data.toString());
}

std::function<bool(QString *)> SearchTab::s_customExtensionsHook;

void SearchTab::askCustomExtensions(const QString &previousCode)
{
    Settings *st = m_core->settings();
    QString text = (m_customExts.isEmpty() ? parseExtensionList(st->s(keys::search::CustomExtensions)) : m_customExts)
                       .join(QLatin1Char(' '));
    bool ok = false;
    if (s_customExtensionsHook) {
        ok = s_customExtensionsHook(&text);
    } else {
        CustomExtensionsDialog dlg(text, this);
        ok = dlg.exec() == QDialog::Accepted;
        text = dlg.text();
    }
    const QStringList exts = parseExtensionList(text);
    if (!ok || exts.isEmpty()) {
        setFileType(previousCode);   // Cancel (or nothing usable typed): the filter stays as it was
        if (ok)
            m_main->showNotice(tr("No file extension typed: the file type stays %1.").arg(filetypes::buttonText(m_typeCode)));
        return;
    }
    st->setValue(keys::search::CustomExtensions, exts.join(QLatin1Char(' ')));
    setFileTypeCode(QStringLiteral("custom:") + exts.join(QLatin1Char(',')));
}

void SearchTab::updateFileTypeButton()
{
    // the text fits the button (fewer extensions rather than a clipped label); the tool tip has them all
    m_fileType->refitText();
    m_fileType->setToolTip(filetypes::description(m_typeCode));
    // the quick button of the current filter looks pressed (All = All Files)
    Settings *st = m_core->settings();
    const QList<PushButton *> btns = {m_audio, m_video, m_pictures};
    m_all->setChecked(m_typeCode == QLatin1String("any"));
    bool any = m_all->isChecked();
    updateSearchLabel();
    for (int i = 0; i < btns.size(); ++i) {
        const QString preset = fileTypeCodeForText(st->s(QString::fromLatin1(kTypeButtons[i].typeKey)));
        const bool on = !any && preset == m_typeCode;   // two presets with the same type: the first
        btns[i]->setChecked(on);
        any = any || on;
    }
    for (PushButton *b : std::as_const(m_moreTypes)) {   // [ui-g]
        const bool on = !any && b->property("typeCode").toString() == m_typeCode;
        b->setChecked(on);
        any = any || on;
    }
}

QString SearchTab::searchLabelFor(const QString &typeCode)
{
    // [ui-g] the owner: "Artist and / or Title" only when audio is chosen. Both lines are keywords that
    // must all appear in the name (the second line may also hold a link or a HASH> line).
    const QString kind = typeCode.section(QLatin1Char(':'), 0, 0);
    if (kind == QLatin1String("audio") || kind == QLatin1String("mp3") || kind == QLatin1String("lossless"))
        return tr("Artist and / or Title");
    if (kind == QLatin1String("video"))
        return tr("Title of the film or show");
    if (kind == QLatin1String("pictures"))
        return tr("Words in the picture's name");
    if (kind == QLatin1String("documents"))
        return tr("Title or author");
    if (kind == QLatin1String("software"))
        return tr("Program name");
    if (kind == QLatin1String("archives") || kind == QLatin1String("discimages") || kind == QLatin1String("torrents"))
        return tr("Name or words");
    if (kind == QLatin1String("servers"))
        return tr("Server list name");
    if (kind == QLatin1String("clr"))
        return tr("Colour scheme name");
    return tr("File name or words");
}

QString SearchTab::searchLabel() const
{
    return m_label ? m_label->text() : QString();
}

void SearchTab::updateSearchLabel()
{
    if (!m_label)
        return;
    m_label->setText(searchLabelFor(m_typeCode));
    // the second line: a title for music, more words for everything else
    const QString kind = m_typeCode.section(QLatin1Char(':'), 0, 0);
    const bool audio = kind == QLatin1String("audio") || kind == QLatin1String("mp3") || kind == QLatin1String("lossless");
    m_edit2->setPlaceholderText(audio ? QString() : tr("more words (optional)"));
    m_edit1->setToolTip(audio ? tr("Artist or title, or words of the file name. All words of both lines must appear in the name.")
                              : tr("Words of the file name; all words of both lines must appear. A magnet or ed2k link or a "
                                   "hash searches for that file."));
    m_edit2->setToolTip(audio ? tr("Title or more words (optional); all words of both lines must appear in the name")
                              : tr("More words that must also appear in the name (optional)"));
}

void SearchTab::updateTypeButtons()
{
    Settings *st = m_core->settings();
    const QList<PushButton *> btns = {m_audio, m_video, m_pictures};
    for (int i = 0; i < 3; ++i) {
        const QVariant v = st->value(QString::fromLatin1(kTypeButtons[i].labelKey));
        const QString caption = v.isValid() && !v.toString().trimmed().isEmpty() ? v.toString() : tr(kTypeButtons[i].def);
        btns[i]->setText(caption);
        btns[i]->setToolTip(filetypes::description(fileTypeCodeForText(st->s(QString::fromLatin1(kTypeButtons[i].typeKey)))));
    }
    updateFileTypeButton();
}

void SearchTab::applyFileTypeToQuery(SearchQuery &q) const
{
    FileTypeFilter::fromCode(m_typeCode).applyTo(q);   // [ui-f] type, extension(s), bitrate limits
}

void SearchTab::showHistoryMenu()
{
    if (!m_core->settings()->b(keys::ui::SearchHistoryMenu))
        return;
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("searchHistoryMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const QStringList hist = m_core->search()->history();
    for (const QString &h : hist.mid(0, 20)) {
        QAction *a = menu->addAction(QString(h).replace(QLatin1Char('&'), QStringLiteral("&&")));
        connect(a, &QAction::triggered, this, [this, h] {
            m_edit1->setText(h);
            m_edit2->clear();
            startSearch();
        });
    }
    if (hist.isEmpty())
        menu->addAction(tr("(no recent searches)"))->setEnabled(false);
    menu->addSeparator();
    QAction *clear = menu->addAction(tr("Clear Search History"), this, [this] {
        m_core->search()->clearHistory();
        updateCompleter();
        m_main->showNotice(tr("The search history is cleared."));
    });
    clear->setEnabled(!hist.isEmpty());
    menu->setMinimumWidth(m_edit1->width());
    tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
    menu->popup(m_edit1->mapToGlobal(QPoint(0, m_edit1->height())));
}

void SearchTab::updateCompleter()
{
    if (m_core->settings()->b(keys::search::AutoComplete)) {
        m_historyModel->setStringList(m_core->search()->history());
        m_edit1->setCompleter(m_completer);
    } else {
        m_edit1->setCompleter(nullptr);
    }
}

void SearchTab::resizeEvent(QResizeEvent *)
{
    // geometry measured on web/search_idle_354b4_wikipedia.png (page coordinates, page top = client y 48).
    // [ui-g] on a window at least kWideFormWidth design px wide the left block grows by 266 px: wider
    // keyword fields, four more quick type buttons (Documents, Software, Archives, Disc Images) and wider
    // network / connection menus; the button block moves right with it. 800 x 600 keeps the classic rows.
    m_wideForm = toDesign(width()) >= kWideFormWidth;
    const int E = m_wideForm ? 266 : 0;   // extra width of the left block
    const int L = 334 + E;                // right edge of the left block
    const int y0 = 28;   // label
    m_label->setGeometry(SR(18, y0 - 1, 300 + E, 14));
    const int r1 = 41, r2 = 62, r3 = 83, r4 = 104;
    // the edits start under the CLEAR button's right edge (x 16), as in the original WinMX
    m_edit1->setGeometry(SR(16, r1, L - 18, 21));
    m_edit2->setGeometry(SR(16, r2, L - 18, 21));
    m_clear->setGeometry(SR(0, r1, 17, 42));
    m_clear->raise();
    m_search->setGeometry(SR(L, r1, 123, 42));
    m_download->setGeometry(SR(L + 123, r1, 123, 21));
    m_resume->setGeometry(SR(L + 246, r1, 123, 21));
    m_afs->setGeometry(SR(L + 123, r2, 123, 21));
    m_aeq->setGeometry(SR(L + 246, r2, 123, 21));
    // [ui-f] All | Audio | Video | Pictures in the 165 px of the classic three (54 54 57): All takes
    // 30, the others give 10 each; [ui-g] wide: Documents 70 | Software 60 | Archives 60 | Disc Images 76
    m_all->setGeometry(SR(0, r3, 30, 21));
    m_audio->setGeometry(SR(30, r3, 44, 21));
    m_video->setGeometry(SR(74, r3, 44, 21));
    m_pictures->setGeometry(SR(118, r3, 47, 21));
    static const int moreW[] = {70, 60, 60, 76};
    int x = 165;
    for (int i = 0; i < m_moreTypes.size(); ++i) {
        m_moreTypes[i]->setVisible(m_wideForm);
        m_moreTypes[i]->setGeometry(SR(x, r3, moreW[i], 21));
        x += moreW[i];
    }
    m_fileType->setGeometry(SR(165 + E, r3, 169, 21));
    updateFileTypeButton();
    m_newWindow->setGeometry(SR(L, r3, 123, 21));
    m_availTop->setGeometry(SR(L + 123, r3, 123, 21));
    m_fullPaths->setGeometry(SR(L + 246, r3, 123, 21));
    m_networks->setGeometry(SR(0, r4, 165 + E / 2, 21));
    m_connection->setGeometry(SR(165 + E / 2, r4, 169 + E - E / 2, 21));
    m_stop->setGeometry(SR(L, r4, 62, 21));
    m_close->setGeometry(SR(L + 62, r4, 61, 21));
    m_pingScreen->setGeometry(SR(L + 123, r4, 123, 21));
    m_pingAll->setGeometry(SR(L + 246, r4, 123, 21));
    const int ly = 127;
    m_legend[0]->setGeometry(SR(4, ly, 210, 16));
    m_legend[1]->setGeometry(SR(223, ly, 290, 16));
    m_legend[2]->setGeometry(SR(522, ly, 200, 16));
    // [ui-g] "Filter results:" directly above the list: beside the legend when it fits, else its own row
    const int fx = S(736);
    int listTop = S(146);
    if (width() - fx - S(4) >= m_filterRow->preferredWidth()) {
        m_filterRow->setGeometry(fx, S(ly - 3), m_filterRow->preferredWidth(), S(21));
    } else {
        m_filterRow->setGeometry(S(4), S(147), qMin(width() - S(8), m_filterRow->preferredWidth()), S(21));
        listTop = S(171);
    }
    m_list->setGeometry(0, listTop, width(), height() - listTop);
}

QString SearchTab::label() const
{
    // a search by hash: the file's name (search_tabs_hash_expanded_group.png: "Artist with HASH ...")
    if (m_isHashSearch)
        return QStringLiteral("\"%1 with HASH\"").arg(m_hashName);
    return QStringLiteral("\"%1\" \"%2\"").arg(m_t1, m_t2);
}

QString SearchTab::hashDisplayName(const QString &t1, const QString &t2)
{
    if (t2.startsWith(QLatin1String("HASH>"), Qt::CaseInsensitive) && !t1.isEmpty())
        return t1;
    const QString link = t2.isEmpty() ? t1 : t2;
    if (link.startsWith(QLatin1String("ed2k://"), Qt::CaseInsensitive)) {
        const QString n = QUrl::fromPercentEncoding(link.section(QLatin1Char('|'), 2, 2).toUtf8());
        if (!n.isEmpty())
            return n;
    } else if (link.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)) {
        // [qa-1] form-encoded: "+" is a space ("Big+Buck+Bunny"), and "&" inside a name is %26
        for (const QString &kv : QUrl(link).query(QUrl::FullyEncoded).split(QLatin1Char('&')))
            if (kv.startsWith(QLatin1String("dn=")))
                return QUrl::fromPercentEncoding(kv.mid(3).replace(QLatin1Char('+'), QLatin1Char(' ')).toUtf8());
    }
    if (!t1.isEmpty() && !looksLikeHashOrLink(t1))
        return t1;
    const QString h = link.section(QLatin1Char(':'), -1);
    return h.left(12) + (h.size() > 12 ? QStringLiteral("...") : QString());
}

QStringList SearchTab::statusFields() const
{
    QStringList out;
    if (!m_resumeFile.isEmpty())
        out << tr("Double-click the matching file to resume %1").arg(QFileInfo(m_resumeFile).fileName());
    if (!m_bundleDialogKey.isEmpty())
        out << tr("Getting the file list of the selected download...");
    if (!m_everSearched)
        return out;
    int n = m_model->hitCount();
    if (m_proxy->hasTypeFilter())   // [ui-f] rows the file type hides (a late file list) are not results
        for (int r = 0; r < m_model->hitCount(); ++r)
            if (!m_proxy->typeAccepts(m_model->hitAt(r)))
                --n;
    // [ui-g] the results filter: the count is what is shown
    if (m_list && !m_list->filterText().isEmpty() && n > 0) {
        const int shown = m_list->filterMatches();
        out.prepend(tr("%1 of %2 results shown (Filter results: %3)").arg(shown).arg(n).arg(m_list->filterText()));
    } else if (m_running && n == 0)
        out.prepend(tr("Waiting for results..."));
    else if (!m_running && m_stopped)
        out.prepend(n == 1 ? tr("Search stopped - 1 result") : tr("Search stopped - %1 results").arg(n));   // [qa-1] the original WinMX: "164 results"
    else
        out.prepend(n == 1 ? tr("1 result") : tr("%1 results").arg(n));
    // the selected row is a suspected fake: say why (also the rows' tooltip)
    const QModelIndex cur = m_list->selectionModel() ? m_list->currentIndex() : QModelIndex();
    if (cur.isValid()) {
        const QModelIndex src = m_list->mapToSource(cur);
        const QModelIndex top = src.parent().isValid() ? src.parent() : src;
        if (top.isValid() && top.row() < m_model->hitCount()) {
            const QString why = m_model->suspectReason(m_model->hitAt(top.row()));
            if (!why.isEmpty())
                out << tr("Suspected fake: %1").arg(why);
        }
    }
    return out;
}

void SearchTab::setResumeFile(const QString &path)
{
    m_resumeFile = path;
    emit statusChanged();
}

QString SearchTab::hashQueryFor(const QString &t1, const QString &t2)
{
    // HASH> line: "HASH>9a2f...  -12345" (Search For Alternates) or a pasted link / raw hash
    QString h = t2;
    if (h.startsWith(QLatin1String("HASH>"), Qt::CaseInsensitive)) {
        h = h.mid(5).trimmed();
        const QStringList parts = h.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        h = parts.value(0);
        qint64 size = 0;
        for (const QString &p : parts.mid(1))
            if (p.startsWith(QLatin1Char('-')))
                size = p.mid(1).toLongLong();
        // an eD2k hash with its size becomes an ed2k:// link, so the size is known before any source answers
        if (size > 0 && h.size() == 32 && QByteArray::fromHex(h.toLatin1()).size() == 16)
            return QStringLiteral("ed2k://|file|%1|%2|%3|/").arg(t1.isEmpty() ? QStringLiteral("file") : t1).arg(size).arg(h);
        return h;
    }
    if (looksLikeHashOrLink(t2))
        return t2;
    if (t2.isEmpty() && looksLikeHashOrLink(t1))
        return t1;
    return QString();
}

bool SearchTab::looksLikeHashOrLink(const QString &raw)
{
    const QString s = raw.trimmed();
    if (s.isEmpty() || s.contains(QLatin1Char(' ')))
        return false;
    if (s.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive) || s.startsWith(QLatin1String("ed2k://"), Qt::CaseInsensitive))
        return true;
    QString h = s;
    static const char *const prefixes[] = {"urn:sha1:", "urn:btih:", "urn:tree:tiger:", "urn:ed2k:", "urn:ed2khash:",
                                           "ed2k:", "sha1:", "btih:", "tth:", "sha256:", "urn:"};
    for (const char *p : prefixes)
        if (h.startsWith(QLatin1String(p), Qt::CaseInsensitive)) {
            h = h.mid(int(qstrlen(p)));
            break;
        }
    auto all = [&h](bool (*ok)(QChar)) {
        for (QChar c : h)
            if (!ok(c))
                return false;
        return true;
    };
    const bool hex = all([](QChar c) { return c.isDigit() || (c.toLower() >= QLatin1Char('a') && c.toLower() <= QLatin1Char('f')); });
    const bool b32 = all([](QChar c) {
        const QChar u = c.toUpper();
        return (u >= QLatin1Char('A') && u <= QLatin1Char('Z')) || (u >= QLatin1Char('2') && u <= QLatin1Char('7'));
    });
    const int n = int(h.size());
    // 32 hex eD2k / MD4, 40 hex SHA-1 / BTIH, 64 hex SHA-256, 32 base32 SHA-1 / BTIH, 39 base32 TTH.
    // A pure-letter 32-char word is not a hash: require a digit.
    bool digit = false;
    for (QChar c : h)
        digit = digit || c.isDigit();
    return digit && ((hex && (n == 32 || n == 40 || n == 64)) || (b32 && (n == 32 || n == 39)));
}

void SearchTab::setTexts(const QString &t1, const QString &t2)
{
    m_edit1->setText(t1);
    m_edit2->setText(t2);
}

void SearchTab::startSearch()
{
    const QString t1 = m_edit1->text().trimmed();
    const QString t2 = m_edit2->text().trimmed();
    // an http(s) URL or a .torrent file downloads directly (UI_SPEC §8.4: the keyword lines accept links)
    if (t2.isEmpty() && looksLikeDirectLink(t1)) {
        m_main->addLinks({t1});
        m_edit1->clear();
        return;
    }
    if (t1.isEmpty() && t2.isEmpty())
        return;
    SearchService *s = m_core->search();
    if (m_id)
        s->closeSearch(m_id);
    m_pending.clear();
    m_model->clear();
    // magnet: / ed2k:// links and raw hashes (keyword line or the HASH> line): search by hash,
    // one row for that file whose sources grow as the networks answer
    const QString hashQuery = hashQueryFor(t1, t2);
    if (!hashQuery.isEmpty()) {
        m_t1 = t1;
        m_t2 = t2;
        m_running = true;
        m_stopped = false;
        m_everSearched = true;
        m_isHashSearch = true;
        m_proxy->setTypeFilter(SearchQuery());   // [ui-f] a hash names one file: no type filter
        m_hashName = hashDisplayName(t1, t2);
        m_id = s->searchHash(hashQuery);
        emit labelChanged();
        emit statusChanged();
        return;
    }
    m_isHashSearch = false;
    SearchQuery q;
    q.text = t1;
    q.text2 = t2;
    applyFileTypeToQuery(q);
    m_proxy->setTypeFilter(q);   // [ui-f] the merged list follows the same rule (late file lists, child names)
    const QString net = m_networks->currentData().toString();
    if (!net.isEmpty())
        q.networks = {netFromName(net)};
    q.maxResults = m_core->settings()->i(keys::search::MaxResults);
    if (q.maxResults <= 0)
        q.maxResults = 2000;
    m_t1 = t1;
    m_t2 = t2;
    m_running = true;
    m_stopped = false;
    m_everSearched = true;
    m_id = s->startSearch(q);
    emit labelChanged();
    emit statusChanged();
    updateCompleter();
}

void SearchTab::stopSearch()
{
    if (!m_id || !m_running)
        return;
    m_core->search()->stopSearch(m_id);
    flushPending();
    m_running = false;
    m_stopped = true;
    emit statusChanged();
}

void SearchTab::onHits(quint32 id, const QList<SearchHit> &hits)
{
    if (id != m_id || !m_id)
        return;
    m_pending << hits;
    if (!m_flush->isActive())
        m_flush->start();
    if (!m_bundleDialogKey.isEmpty()) {
        for (const SearchHit &h : hits)
            if (h.key == m_bundleDialogKey && !h.files.isEmpty()) {
                m_bundleDialogKey.clear();
                m_bundleTimer->stop();
                emit statusChanged();
                const SearchHit copy = h;
                QTimer::singleShot(0, this, [this, copy] { flushPending(); openBundleDialog(copy); });
                break;
            }
    }
}

void SearchTab::flushPending()
{
    m_flush->stop();
    if (m_pending.isEmpty())
        return;
    // keep the first visible row in place while rows are inserted above it
    QScrollBar *vs = m_list->verticalScrollBar();
    const bool atTop = vs->value() == 0;
    const QPersistentModelIndex anchor = atTop ? QPersistentModelIndex() : QPersistentModelIndex(m_list->indexAt(QPoint(2, 2)));
    const QList<SearchHit> batch = m_pending;
    m_pending.clear();
    m_model->applyHits(batch);
    if (anchor.isValid())
        m_list->scrollTo(anchor, QAbstractItemView::PositionAtTop);
    emit statusChanged();
}

void SearchTab::onRemoved(quint32 id, const QStringList &keys)
{
    if (id != m_id || !m_id)
        return;
    flushPending();   // apply queued updates first; the survivor follows in the next hitsChanged
    QScrollBar *vs = m_list->verticalScrollBar();
    QPersistentModelIndex anchor;
    if (vs->value() > 0) {
        // anchor on the first visible row that survives
        for (QModelIndex i = m_list->indexAt(QPoint(2, 2)); i.isValid(); i = m_list->indexBelow(i)) {
            const QModelIndex src = m_list->mapToSource(i);
            const QModelIndex top = src.parent().isValid() ? src.parent() : src;
            if (!keys.contains(m_model->hitAt(top.row()).key)) {
                anchor = i;
                break;
            }
        }
    }
    m_model->removeHits(keys);
    if (anchor.isValid())
        m_list->scrollTo(anchor, QAbstractItemView::PositionAtTop);
    emit statusChanged();
}

void SearchTab::openBundleDialog(const SearchHit &h)
{
    BundleFilesDialog dlg(h.fileName, h.files, {}, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    DownloadRequest req;
    req.name = h.fileName;
    req.size = h.size;
    req.hashes = h.hashes;
    req.sources = h.sources;
    req.wantedFiles = dlg.selectedIndexes();
    if (m_core->transfers()->download(req).isEmpty())   // [qa-1] never silent
        m_main->reportDownloadProblem(h.fileName, tr("The download could not be started."));
}

void SearchTab::onFinished(quint32 id)
{
    if (id != m_id)
        return;
    flushPending();
    m_running = false;
    emit statusChanged();
}

bool SearchTab::confirmDownload(const SearchHit &h)
{
    Settings *st = m_core->settings();
    if (dangerous(h.fileName) && !Session::instance()->suppressDangerousWarning() &&
        !st->b(keys::transfers::SuppressDangerousWarning)) {
        if (ask(tr("OctoMX"),
                tr("You are about to download\n\n    %1\n\nThis is a program or script, or a file that can hide one. Such files may "
                   "carry a virus or other harmful code, so it is safer not to download it.\n\nDownload it all the same?\n\n"
                   "(This warning can be switched off for the rest of the session in the program settings.)")
                    .arg(h.fileName)) != QMessageBox::Yes)
            return false;
    }
    // a suspected fake: warn once per file (the same row is not asked about again in this tab)
    const QString why = m_model->suspectReason(h);
    if (!why.isEmpty() && !m_suspectConfirmed.contains(h.key)) {
        if (ask(tr("OctoMX"),
                tr("You are about to download\n\n%1\n\nIt looks like a fake:\n%2\n\nFiles like this are often "
                   "mislabelled, damaged or harmful.  Download it all the same?\n\n(Suspected fakes can be hidden in the "
                   "program settings, Search Options page.)")
                    .arg(h.fileName, why)) != QMessageBox::Yes)
            return false;
        m_suspectConfirmed.insert(h.key);
    }
    return true;
}

int SearchTab::ask(const QString &title, const QString &text)
{
    if (s_askHook)
        return s_askHook(title, text);
    QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::Yes | QMessageBox::No, this);
    box.setObjectName(QStringLiteral("searchWarning"));
    // [qa-1] dialog_dangerous_file_warning.png: "Yes" is the default button and the long sentence is not
    // wrapped (the dialog is ~610 px wide); QMessageBox would wrap at 500 px
    box.setDefaultButton(QMessageBox::Yes);
    // [qa-2] the widest line, not line 5: the fake-file warning's line 5 is "looks like a fake:", which
    // made a 178-px box that cut every longer line off
    if (auto *label = box.findChild<QLabel *>(QStringLiteral("qt_msgbox_label"))) {
        int widest = 0;
        for (const QString &line : text.split(QLatin1Char('\n')))
            widest = qMax(widest, label->fontMetrics().horizontalAdvance(line));
        label->setMinimumWidth(qMin(widest + S(4), S(700)));
    }
    return box.exec();
}

std::function<int(const QString &, const QString &)> SearchTab::s_askHook;

void SearchTab::downloadIndex(const QModelIndex &viewIndex)
{
    const QModelIndex src = m_list->mapToSource(viewIndex);
    if (!src.isValid())
        return;
    const SearchHit h = m_model->hitFor(src.sibling(src.row(), 0));
    if (h.sources.isEmpty() && h.hashes.isEmpty()) {
        m_main->reportDownloadProblem(h.fileName, tr("This result has no source or link to download from."));
        return;
    }
    if (!confirmDownload(h))
        return;
    if (!m_resumeFile.isEmpty()) {
        // "Load Incomplete": this search looks for the file to resume
        if (resumeFrom(h, m_resumeFile))
            setResumeFile(QString());
        return;
    }
    if (h.isBundle && h.files.isEmpty()) {
        // [ui-e] file list unknown (e.g. a torrent only a web index listed): the download starts at once,
        // its Transfers row appears now and fetches the torrent; files can be picked there later
        // (Select Files...). Owner report: waiting for the list looked like "nothing happens".
        if (m_core->transfers()->downloadHit(h).isEmpty()) {
            m_main->reportDownloadProblem(h.fileName, tr("The download could not be started."));
            return;
        }
        m_main->showNotice(tr("Downloading %1: the file list comes with the torrent (right-click it in Transfers and pick Select Files... to choose files).")
                               .arg(h.fileName));
        markDownloading(src);
        return;
    }
    if (h.isBundle && h.files.size() > 1) {
        openBundleDialog(h);
    } else if (m_core->transfers()->downloadHit(h).isEmpty()) {
        // the core says why through TransferService::downloadNotStarted (MainWindow shows it); this
        // covers a core that refuses silently
        m_main->reportDownloadProblem(h.fileName, tr("The download could not be started."));
        return;
    }
    markDownloading(src);
}

void SearchTab::startWholeBundle(const QString &why)
{
    if (m_bundleDialogKey.isEmpty())
        return;
    m_bundleTimer->stop();
    m_bundleDialogKey.clear();
    const SearchHit h = m_bundlePendingHit;
    m_bundlePendingHit = SearchHit();
    emit statusChanged();
    if (m_core->transfers()->downloadHit(h).isEmpty())
        m_main->reportDownloadProblem(h.fileName, tr("The download could not be started."));
    else
        m_main->showNotice(tr("The file list of %1 is not available (%2): downloading all of its files.").arg(h.fileName, why));
}

void SearchTab::markDownloading(const QModelIndex &src)
{
    // the row now matches a running download
    SearchHit marked = m_model->hitAt(src.parent().isValid() ? src.parent().row() : src.row());
    marked.currentlyDownloading = true;
    m_model->applyHits({marked});
}

DownloadRequest SearchTab::requestFor(const SearchHit &h)
{
    DownloadRequest req;
    req.name = h.fileName;
    req.size = h.size;
    req.hashes = h.hashes;
    req.sources = h.sources;
    return req;
}

bool SearchTab::resumeFrom(const SearchHit &h, const QString &file)
{
    const QString id = m_core->transfers()->resumeDownload(requestFor(h), file);
    if (id.isEmpty()) {
        QMessageBox::warning(this, tr("OctoMX"),
                             tr("Unable to resume from\n\n%1\n\nThe file is larger than the download, cannot be read, or is "
                                "already used by another download.")
                                 .arg(QDir::toNativeSeparators(file)));
        return false;
    }
    rememberResumedFile(m_core->settings(), id, file);
    const int row = m_model->rowOfKey(h.key);
    if (row >= 0)
        markDownloading(m_model->index(row, 0));
    return true;
}

void SearchTab::rememberResumedFile(Settings *s, const QString &transferId, const QString &file)
{
    QVariantMap m = s->value(QStringLiteral("ui/resumedFiles")).toMap();
    m.insert(transferId, file);
    s->setValue(QStringLiteral("ui/resumedFiles"), m);
}

void SearchTab::manuallyResumeSelected()
{
    const QModelIndexList rows = m_list->selectionModel()->selectedRows(0);
    if (rows.isEmpty())
        return;
    const QModelIndex src = m_list->mapToSource(rows.first());
    const SearchHit h = m_model->hitFor(src);
    if (h.sources.isEmpty() || !confirmDownload(h))
        return;
    FileDownloadDialog dlg(h, m_core->transfers()->partialFiles(), m_core->transfers()->incompleteDir(), this);
    if (s_resumeDialogHook ? !s_resumeDialogHook(&dlg) : dlg.exec() != QDialog::Accepted)
        return;
    const QString file = dlg.chosenFile();
    if (file.isEmpty()) {
        m_core->transfers()->downloadHit(h);   // "Save to a new file"
        markDownloading(src);
    } else {
        resumeFrom(h, file);
    }
}

std::function<bool(FileDownloadDialog *)> SearchTab::s_resumeDialogHook;

void SearchTab::downloadSelected()
{
    QModelIndexList rows = m_list->selectionModel()->selectedRows(0);
    for (int i = int(rows.size()) - 1; i >= 0; --i)   // [ui-g] only what the results filter shows
        if (m_list->isFilteredOut(rows.at(i)))
            rows.removeAt(i);
    if (rows.isEmpty()) {   // [ui-e] never silent
        m_main->showNotice(tr("Select a file in the list first, then press Download."));
        return;
    }
    QList<QPersistentModelIndex> keep;
    for (const QModelIndex &i : rows)
        keep << i;
    for (const QPersistentModelIndex &i : keep)
        if (i.isValid())
            downloadIndex(i);
}

void SearchTab::ping(bool allResults)
{
    if (!m_id) {
        m_main->showNotice(tr("Nothing to ping: start a search first."));
        return;
    }
    QStringList keysToPing;
    for (int r = 0; r < m_model->hitCount(); ++r) {
        if (!allResults) {
            const QModelIndex vi = m_list->mapFromSource(m_model->index(r, 0));
            if (!vi.isValid() || !m_list->viewport()->rect().intersects(m_list->visualRect(vi)))
                continue;
        }
        keysToPing << m_model->hitAt(r).key;
    }
    if (!keysToPing.isEmpty())
        m_core->search()->pingHits(m_id, keysToPing);   // answers arrive as hitsChanged (pingMs / "?")
}

void SearchTab::showContextMenu(const QModelIndex &viewIndex, const QPoint &globalPos)
{
    if (!viewIndex.isValid())
        return;
    const QModelIndex src = m_list->mapToSource(viewIndex);
    const SearchHit h = m_model->hitFor(src);
    const SearchSource s = h.sources.size() == 1 ? h.sources.first() : SearchSource();
    const UserRef u = s.user;
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("searchMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction *dl = menu->addAction(tr("Download"), this, &SearchTab::downloadSelected);
    menu->setDefaultAction(dl);
    menu->addAction(tr("Manually Download"), this, &SearchTab::manuallyResumeSelected);
    menu->addSeparator();
    const bool hasUser = u.isValid();
    menu->addAction(tr("Browse"), this, [this, u] { m_main->openBrowse(u); })->setEnabled(hasUser);
    menu->addAction(tr("Add To Hotlist"), this, [this, u] { m_core->users()->addToHotlist(u); })->setEnabled(hasUser);
    menu->addAction(tr("Whois"), this, [this, u] { m_main->openWhois(u); })->setEnabled(hasUser);
    menu->addAction(tr("Send Message"), this, [this, u] { m_main->openPrivateMessage(u); })->setEnabled(hasUser);
    menu->addAction(tr("Chat"), this, [this, u] { m_main->openChat(u); })->setEnabled(hasUser);
    // [ui-e] links and name for the clipboard; a result that is already on this computer opens
    menu->addSeparator();
    const QString magnet = magnetLinkOf(h), ed2k = ed2kLink(h.fileName, h.size, h.hashes);
    menu->addAction(tr("Copy Magnet Link"), this, [magnet] { QGuiApplication::clipboard()->setText(magnet); })->setEnabled(!magnet.isEmpty());
    menu->addAction(tr("Copy ed2k Link"), this, [ed2k] { QGuiApplication::clipboard()->setText(ed2k); })->setEnabled(!ed2k.isEmpty());
    menu->addAction(tr("Copy Name"), this, [n = h.fileName] { QGuiApplication::clipboard()->setText(n); });
    const QString local = localPathFor(h);
    if (!local.isEmpty()) {
        menu->addSeparator();
        menu->addAction(tr("Open File"), this, [this, local] {
            QString why;
            if (!launcher::openFile(this, m_core->settings(), local, &why) && !why.isEmpty())
                m_main->showNotice(why);
        })->setObjectName(QStringLiteral("actOpenFile"));
        menu->addAction(tr("Open Containing Folder"), this, [this, local] {
            QString why;
            if (!launcher::showInFolder(local, &why))
                m_main->showNotice(why);
        })->setObjectName(QStringLiteral("actShowInFolder"));
    }
    tips::explainMenu(menu);   // [ui-g] tool tips + status bar text of the entries
    menu->popup(globalPos);
}

QString SearchTab::localPathFor(const SearchHit &h) const
{
    // a completed download of the same file, else a shared file (same hash, or same name and size)
    auto same = [&h](const QString &name, qint64 size, const FileHashes &hashes) {
        if (!h.hashes.isEmpty() && !hashes.isEmpty())
            return h.hashes.matches(hashes);
        return size == h.size && size > 0 && name.compare(h.fileName, Qt::CaseInsensitive) == 0;
    };
    for (const TransferInfo &t : m_core->transfers()->downloads())
        if (t.state == TransferState::Completed || t.state == TransferState::Seeding)
            if (!t.savePath.isEmpty() && same(t.name, t.size, t.hashes) && QFileInfo::exists(t.savePath))
                return t.savePath;
    for (const SharedFile &f : m_core->library()->files())
        if (same(f.name, f.size, f.hashes))
            return f.path;
    return QString();
}

// ----------------------------------------------------------------------------------- SearchPage

SearchPage::SearchPage(mx::Core *core, MainWindow *mw, QWidget *parent)
    : Page(QStringLiteral("search"), tr("Search"), core, mw, parent)
{
    m_tabs = new TabStrip(this);
    m_tabs->setObjectName(QStringLiteral("searchTabs"));
    m_tabs->setSlotGeometry(168, 172, 0, 682);
    m_tabs->setLeadingButton(IconId::OpenNewSearch, tr("Open New Search"))->setObjectName(QStringLiteral("searchOpenNew"));
    if (QWidget *b = m_tabs->findChild<QWidget *>(QStringLiteral("searchOpenNew")))
        b->setToolTip(tr("Open an empty search tab; the other tabs keep their results"));
    m_stack = new QStackedWidget(this);
    connect(m_tabs, &TabStrip::leadingClicked, this, [this] { newTab(true); });
    connect(m_tabs, &TabStrip::currentChanged, this, [this](int i) {
        if (SearchTab *t = m_tabList.value(i))
            m_stack->setCurrentWidget(t);
        emit statusChanged();
    });
    newTab(true);
}

SearchTab *SearchPage::newTab(bool makeCurrent)
{
    auto *t = new SearchTab(core(), mainWindow(), m_stack);
    m_tabList << t;
    m_stack->addWidget(t);
    const int idx = m_tabs->addTab(t->label(), true, IconId::SearchTab);
    m_tabs->tabButton(idx)->setObjectName(QStringLiteral("searchTab%1").arg(idx));
    connect(t, &SearchTab::labelChanged, this, &SearchPage::updateLabels);
    connect(t, &SearchTab::statusChanged, this, [this, t] {
        if (t == currentTab())
            emit statusChanged();
    });
    connect(t, &SearchTab::newWindowRequested, this, &SearchPage::searchInNewTab);
    connect(t, &SearchTab::closeRequested, this, [this, t] { closeTab(t); });
    if (makeCurrent) {
        m_tabs->setCurrentIndex(idx);
        m_stack->setCurrentWidget(t);
        t->keywords()->setFocus();
    }
    return t;
}

void SearchPage::closeTab(SearchTab *t)
{
    const int i = m_tabList.indexOf(t);
    if (i < 0)
        return;
    if (m_tabList.size() == 1) {
        // the last tab is never removed: it is reset to an empty search
        auto *fresh = newTab(false);
        Q_UNUSED(fresh);
    }
    m_tabList.removeAt(i);
    m_tabs->removeTab(i);
    m_stack->removeWidget(t);
    t->deleteLater();
    for (int k = 0; k < m_tabList.size(); ++k)
        m_tabs->tabButton(k)->setObjectName(QStringLiteral("searchTab%1").arg(k));
    if (SearchTab *c = currentTab())
        m_stack->setCurrentWidget(c);
    emit statusChanged();
}

void SearchPage::searchInNewTab(const QString &t1, const QString &t2)
{
    SearchTab *t = currentTab();
    // reuse the current tab if it never searched
    if (!t || t->searchId() != 0)
        t = newTab(true);
    t->setTexts(t1, t2);
    t->startSearch();
}

void SearchPage::updateLabels()
{
    for (int i = 0; i < m_tabList.size(); ++i)
        m_tabs->setTabText(i, m_tabList[i]->label());
}

SearchTab *SearchPage::currentTab() const
{
    return m_tabList.value(m_tabs->currentIndex());
}

SearchTab *SearchPage::tab(int i) const
{
    return m_tabList.value(i);
}

int SearchPage::tabCount() const
{
    return int(m_tabList.size());
}

QStringList SearchPage::statusFields() const
{
    SearchTab *t = currentTab();
    return t ? t->statusFields() : QStringList();
}

void SearchPage::activated()
{
    if (SearchTab *t = currentTab())
        t->keywords()->setFocus();
}

bool SearchPage::selectSubTab(const QString &name)
{
    bool ok = false;
    const int i = name.toInt(&ok);
    if (ok && i >= 0 && i < m_tabList.size()) {
        m_tabs->setCurrentIndex(i);
        return true;
    }
    for (int k = 0; k < m_tabList.size(); ++k)
        if (m_tabList[k]->label().contains(name, Qt::CaseInsensitive)) {
            m_tabs->setCurrentIndex(k);
            return true;
        }
    return false;
}

void SearchPage::saveState()
{
    for (SearchTab *t : std::as_const(m_tabList))
        t->list()->saveState();
}

void SearchPage::resizeEvent(QResizeEvent *)
{
    m_tabs->setGeometry(S(2), S(3), width() - S(2), S(21));
    m_stack->setGeometry(0, 0, width(), height());
    m_tabs->raise();
}

} // namespace mx::ui
