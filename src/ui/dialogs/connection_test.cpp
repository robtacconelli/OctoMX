// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1] Connection Test window (see connection_test.h)
#include "ui/dialogs/connection_test.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtGui/QClipboard>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QToolTip>

#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/colors.h"
#include "ui/kit/draw.h"
#include "ui/kit/icons.h"
#include "ui/kit/launcher.h"
#include "ui/kit/list_view.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/main_window.h"
#include "ui/models/roles.h"
#include "ui/script_driver.h"

namespace mx::ui {

std::function<QString(QWidget *, const QString &)> ConnectionTestDialog::s_saveHook;
std::function<void(const QString &)> ConnectionTestDialog::s_openFolderHook;

namespace {
QPointer<ConnectionTestDialog> g_instance;
constexpr int DiagIdRole = UserRoleBase + 61;
constexpr int DiagExplainRole = UserRoleBase + 62;
constexpr int DiagLevelRole = UserRoleBase + 63;

// the summary lamp: a 12 x 12 square in the level colour with a black border
class Lamp : public QWidget {
public:
    explicit Lamp(QWidget *parent) : QWidget(parent) {}
    void setColor(const QColor &c) { m_c = c; update(); }
    QColor colorNow() const { return m_c; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        p.fillRect(rect().adjusted(uiLine(), uiLine(), -uiLine(), -uiLine()), m_c);
    }

private:
    QColor m_c = Qt::white;
};
} // namespace

ConnectionTestDialog *ConnectionTestDialog::instance() { return g_instance; }

ConnectionTestDialog *ConnectionTestDialog::open(mx::Core *core, QWidget *parent, const QString &downloadId)
{
    if (!g_instance) {
        QWidget *top = parent ? parent->window() : nullptr;
        g_instance = new ConnectionTestDialog(core, top);
        g_instance->setAttribute(Qt::WA_DeleteOnClose);
    }
    g_instance->show();
    g_instance->raise();
    g_instance->activateWindow();
    if (!downloadId.isEmpty()) g_instance->selectDownload(downloadId);
    return g_instance;
}

ConnectionTestDialog::ConnectionTestDialog(mx::Core *core, QWidget *parent)
    : QDialog(parent, Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinMaxButtonsHint)
    , m_core(core)
{
    setObjectName(QStringLiteral("connectionTestDialog"));
    setWindowTitle(tr("Connection Test"));
    setWindowIcon(appIcon());
    setModal(false);

    m_lamp = new Lamp(this);
    m_lamp->setObjectName(QStringLiteral("diagLamp"));
    m_summary = new StaticText(QString(), this);
    m_summary->setObjectName(QStringLiteral("diagSummary"));
    m_summary->setFont(boldFont());
    m_progressText = new StaticText(QString(), this);
    m_progressText->setObjectName(QStringLiteral("diagProgressText"));
    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("diagProgress"));
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);

    m_list = new MxListView(this);
    m_list->setObjectName(QStringLiteral("diagList"));
    m_list->setTreeMode(true);
    m_list->setSortingEnabled(false);
    m_list->setRootIsDecorated(true);
    m_model = new QStandardItemModel(0, 3, this);
    m_model->setHorizontalHeaderLabels({tr("Check"), tr("Result"), tr("Details")});
    m_list->setModel(m_model);
    m_list->header()->setStretchLastSection(true);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &ConnectionTestDialog::onSelection);
    connect(m_list, &QTreeView::collapsed, this, [this](const QModelIndex &vi) {
        m_collapsed.insert(m_list->mapToSource(vi).siblingAtColumn(0).data(DiagIdRole).toString());
    });
    connect(m_list, &QTreeView::expanded, this, [this](const QModelIndex &vi) {
        m_collapsed.remove(m_list->mapToSource(vi).siblingAtColumn(0).data(DiagIdRole).toString());
    });

    m_explain = new QPlainTextEdit(this);
    m_explain->setObjectName(QStringLiteral("diagExplain"));
    m_explain->setReadOnly(true);
    m_explain->setFont(normalFont());
    m_explain->setToolTip(tr("What the selected row means and what to do about it"));

    // [ui-h] the box masked only the copied / saved report while the window showed the addresses:
    // it now masks the window too (for screenshots), and says so
    m_hide = new QCheckBox(tr("Hide my IP addresses (window and report)"), this);
    m_hide->setObjectName(QStringLiteral("diagHideAddresses"));
    m_hide->setChecked(core->settings()->b(keys::diag::HideAddresses));
    m_hide->setToolTip(tr("This window, Copy Report and Save Report show your public addresses as ext-1, ext-2, ..., "
                          "so a screenshot or a report shows nobody your address; addresses inside your own network "
                          "are kept. Default: on."));
    connect(m_hide, &QCheckBox::toggled, this, [this](bool on) {
        m_core->settings()->setValue(QString(keys::diag::HideAddresses), on);
        refresh();
    });

    auto button = [this](const QString &text, const char *name, const QString &tip) {
        auto *b = new PushButton(text, this);
        b->setObjectName(QLatin1String(name));
        b->setToolTip(tip);
        return b;
    };
    m_test = button(tr("Test Now"), "diagTestNow",
                    tr("Check the ports, ask the router to forward them, look up the server and tracker names, "
                       "test UDP and wait a little for the networks' own reachability checks"));
    m_online = button(tr("Check Ports Online"), "diagCheckOnline", QString());
    m_copy = button(tr("Copy Report"), "diagCopy", tr("Copy the whole report as plain text, to paste it to a developer"));
    m_save = button(tr("Save Report..."), "diagSave", tr("Save the whole report as a text file"));
    m_logs = button(tr("Open Log Folder"), "diagLogs", tr("Show the folder with octomx.log"));
    m_close = button(tr("Close"), "diagClose", QString());
    connect(m_test, &QPushButton::clicked, this, &ConnectionTestDialog::testNow);
    connect(m_online, &QPushButton::clicked, this, &ConnectionTestDialog::checkOnline);
    connect(m_copy, &QPushButton::clicked, this, &ConnectionTestDialog::copyReport);
    connect(m_save, &QPushButton::clicked, this, &ConnectionTestDialog::saveReport);
    connect(m_logs, &QPushButton::clicked, this, &ConnectionTestDialog::openLogFolder);
    connect(m_close, &QPushButton::clicked, this, &QDialog::close);

    if (DiagnosticsService *d = core->diagnostics()) {
        connect(d, &DiagnosticsService::reportChanged, this, &ConnectionTestDialog::refresh);
        connect(d, &DiagnosticsService::testProgress, this, &ConnectionTestDialog::onTestProgress);
        connect(d, &DiagnosticsService::testFinished, this, &ConnectionTestDialog::onTestFinished);
    }
    connect(Colors::instance(), &Colors::changed, this, &ConnectionTestDialog::refresh);

    const QByteArray g = core->settings()->value(keys::ui::dialogKey(QStringLiteral("connectionTest"))).toByteArray();
    if (g.isEmpty() || !restoreGeometry(g)) resize(SS(760, 540));
    setMinimumSize(SS(620, 400));   // the six buttons need 611 px
    m_restored = true;
    refresh();
    updateButtons();
}

ConnectionTestDialog::~ConnectionTestDialog()
{
    if (g_instance == this) g_instance = nullptr;
}

QColor ConnectionTestDialog::levelColor(DiagLevel l) const
{
    switch (l) {
    case DiagLevel::Ok: return color(keys::colors::PingGood);
    case DiagLevel::Limited: return color(keys::colors::PingMedium);
    case DiagLevel::Broken: return color(keys::colors::PingBad);
    case DiagLevel::Off: return color(keys::colors::InactiveText);
    case DiagLevel::Unknown: break;
    }
    return color(keys::colors::ListText);
}

QList<QStandardItem *> ConnectionTestDialog::makeRow(const QString &id, const QString &check, const QString &result,
                                                     const QString &details, DiagLevel level, const QString &explain, bool group)
{
    const QColor c = levelColor(level);
    QList<QStandardItem *> row;
    const bool hide = m_hide && m_hide->isChecked();
    const QString ex = hide ? m_windowMask.mask(explain) : explain;
    for (QString t : {check, result, details}) {
        if (hide)
            t = m_windowMask.mask(t);
        auto *it = new QStandardItem(t);
        it->setEditable(false);
        it->setData(c, Qt::ForegroundRole);
        it->setData(id, DiagIdRole);
        it->setData(ex, DiagExplainRole);
        it->setData(int(level), DiagLevelRole);
        QString tip = ex.isEmpty() ? (details.isEmpty() ? result : details) : ex;
        if (hide)
            tip = m_windowMask.mask(tip);
        it->setData(tip, Qt::ToolTipRole);
        if (group) it->setData(boldFont(), Qt::FontRole);
        row << it;
    }
    if (group || details.isEmpty()) row.at(1)->setData(true, OverflowRole);   // runs on over the empty Details cell
    return row;
}

void ConnectionTestDialog::refresh()
{
    DiagnosticsService *d = m_core->diagnostics();
    if (!d) {
        m_summary->setText(tr("The connection test is not available in this version."));
        m_model->removeRows(0, m_model->rowCount());
        updateButtons();
        return;
    }
    m_report = d->report();
    static_cast<Lamp *>(m_lamp)->setColor(levelColor(m_report.level));
    m_windowMask = AddressMasker();
    const QString summary = m_hide && m_hide->isChecked() ? m_windowMask.mask(m_report.summary) : m_report.summary;
    m_summary->setText(summary);
    m_summary->setToolTip(summary);
    if (!m_report.testRunning)
        m_progressText->setText(m_report.lastTest.isValid() ? tr("Last test: %1").arg(QLocale().toString(m_report.lastTest.time(), QLocale::ShortFormat))
                                                            : tr("Not tested yet: the list shows what the networks found by themselves. Press Test Now for the active checks."));
    // rebuild, keeping selection, collapsed groups and scroll position
    const int scroll = m_list->verticalScrollBar() ? m_list->verticalScrollBar()->value() : 0;
    const QString keep = m_selected;
    m_model->removeRows(0, m_model->rowCount());
    QStandardItem *root = m_model->invisibleRootItem();
    bool downloadsHeader = false;
    for (const DiagGroup &g : m_report.groups) {
        if (!g.downloadId.isEmpty() && !downloadsHeader) downloadsHeader = true;
        const QString title = g.downloadId.isEmpty() ? g.title : tr("Download: %1").arg(g.title);
        QList<QStandardItem *> grow = makeRow(g.id, title, g.result, QString(), g.level,
                                              g.explain.isEmpty() ? g.result : g.result + QStringLiteral("\n\n") + g.explain, true);
        for (const DiagItem &i : g.items) {
            QString ex = QStringLiteral("%1: %2").arg(i.check, i.result);
            if (!i.details.isEmpty()) ex += QStringLiteral(" (%1)").arg(i.details);
            if (!i.explain.isEmpty()) ex += QStringLiteral("\n\n") + i.explain;
            else if (i.level == DiagLevel::Ok) ex += QStringLiteral("\n\n") + tr("Fine.");
            grow.first()->appendRow(makeRow(i.id, i.check, i.result, i.details, i.level, ex, false));
        }
        root->appendRow(grow);
    }
    m_list->expandAll();
    for (int r = 0; r < m_model->rowCount(); ++r) {
        const QModelIndex si = m_model->index(r, 0);
        if (m_collapsed.contains(si.data(DiagIdRole).toString())) m_list->collapse(m_list->mapFromSource(si));
    }
    if (!keep.isEmpty()) selectRow(keep);
    if (m_list->verticalScrollBar()) m_list->verticalScrollBar()->setValue(scroll);
    onSelection();
    updateButtons();
}

bool ConnectionTestDialog::selectRow(const QString &id)
{
    const QModelIndexList hits = m_model->match(m_model->index(0, 0), DiagIdRole, id, 1, Qt::MatchExactly | Qt::MatchRecursive);
    if (hits.isEmpty()) return false;
    const QModelIndex vi = m_list->mapFromSource(hits.first());
    if (vi.parent().isValid()) m_list->expand(vi.parent());
    m_list->setCurrentIndex(vi);
    m_list->selectionModel()->select(vi, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_list->scrollTo(vi, QAbstractItemView::PositionAtTop);
    m_selected = id;
    onSelection();
    return true;
}

void ConnectionTestDialog::selectDownload(const QString &transferId)
{
    const QString id = diagGroupIdForDownload(transferId);
    m_collapsed.remove(id);
    if (!selectRow(id)) {
        m_explain->setPlainText(tr("This download has nothing to report right now (finished, or not known to the connection test)."));
        return;
    }
    m_list->expand(m_list->currentIndex());
}

void ConnectionTestDialog::onSelection()
{
    const QModelIndex vi = m_list->currentIndex();
    if (!vi.isValid()) {
        m_explain->setPlainText(m_report.summary.isEmpty() ? QString()
                                                           : tr("%1\n\nSelect a row to see what it means and what to do.").arg(m_report.summary));
        return;
    }
    const QModelIndex si = m_list->mapToSource(vi).siblingAtColumn(0);
    m_selected = si.data(DiagIdRole).toString();
    m_explain->setPlainText(si.data(DiagExplainRole).toString());
}

QString ConnectionTestDialog::explanation() const { return m_explain->toPlainText(); }
QString ConnectionTestDialog::summaryText() const { return m_summary->text(); }

QString ConnectionTestDialog::reportText() const
{
    DiagnosticsService *d = m_core->diagnostics();
    return diagReportText(d ? d->report() : m_report, m_hide->isChecked());
}

void ConnectionTestDialog::updateButtons()
{
    DiagnosticsService *d = m_core->diagnostics();
    const bool testing = d && d->isTesting();
    m_test->setText(testing ? tr("Cancel Test") : tr("Test Now"));
    m_test->setEnabled(d != nullptr);
    m_progress->setVisible(testing);
    const QString svc = d ? d->portCheckService() : QString();
    m_online->setEnabled(d != nullptr);
    m_online->setToolTip(svc.isEmpty()
                             ? tr("Asks connected OctoMX users to connect back to your ports (no public port checker is set: diag/portCheckUrl)")
                             : tr("Asks %1 (a public port checker run by the Transmission project) and connected OctoMX users "
                                  "whether your TCP ports can be reached from the internet. Only done when you press this button.").arg(svc));
    m_copy->setEnabled(d != nullptr);
    m_save->setEnabled(d != nullptr);
}

void ConnectionTestDialog::onTestProgress(int percent, const QString &step)
{
    m_progress->setVisible(true);
    m_progress->setValue(percent);
    m_progressText->setText(tr("Testing: %1").arg(step));
    updateButtons();
}

void ConnectionTestDialog::onTestFinished(bool cancelled)
{
    m_progress->setVisible(false);
    refresh();
    if (cancelled) m_progressText->setText(tr("Test cancelled."));
    updateButtons();
}

void ConnectionTestDialog::testNow()
{
    DiagnosticsService *d = m_core->diagnostics();
    if (!d) return;
    if (d->isTesting()) {
        d->cancelTest();
    } else {
        m_progress->setValue(0);
        m_progress->setVisible(true);
        d->startTest();
    }
    updateButtons();
}

void ConnectionTestDialog::checkOnline()
{
    DiagnosticsService *d = m_core->diagnostics();
    if (!d) return;
    const QString svc = d->portCheckService();
    m_progressText->setText(svc.isEmpty() ? tr("Asking connected OctoMX users to connect back...")
                                          : tr("Asking %1 and connected OctoMX users to connect back...").arg(svc));
    d->checkPortsOnline();
}

void ConnectionTestDialog::copyReport()
{
    QGuiApplication::clipboard()->setText(reportText());
    m_progressText->setText(tr("The report is in the clipboard."));
}

void ConnectionTestDialog::saveReport()
{
    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                  .filePath(QStringLiteral("octomx-connection-report-%1.txt").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"))));
    const QString path = s_saveHook ? s_saveHook(this, suggested)
                                    : QFileDialog::getSaveFileName(this, tr("Save Report"), suggested, tr("Text files (*.txt);;All files (*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_progressText->setText(tr("Could not write %1: %2").arg(QDir::toNativeSeparators(path), f.errorString()));
        return;
    }
    f.write(reportText().toUtf8());
    f.close();
    m_progressText->setText(tr("Report saved to %1").arg(QDir::toNativeSeparators(path)));
}

void ConnectionTestDialog::openLogFolder()
{
    const QString log = m_report.logFile.isEmpty() ? m_core->profileDir() + QStringLiteral("/logs/octomx.log") : m_report.logFile;
    if (s_openFolderHook) {
        s_openFolderHook(log);
        return;
    }
    QString why;
    if (!launcher::showInFolder(QFileInfo::exists(log) ? log : QFileInfo(log).absolutePath(), &why) && !why.isEmpty())
        m_progressText->setText(why);
}

void ConnectionTestDialog::resizeEvent(QResizeEvent *e)
{
    QDialog::resizeEvent(e);
    const int w = width(), h = height();
    const int m = S(8);
    m_lamp->setGeometry(m, S(9), S(12), S(12));
    m_summary->setGeometry(m + S(18), S(8), w - 2 * m - S(18), S(16));
    m_progress->setGeometry(w - m - S(160), S(28), S(160), S(14));
    m_progressText->setGeometry(m, S(28), w - 2 * m - (m_progress->isVisible() ? S(168) : 0), S(14));
    const int buttonsY = h - m - S(23);
    const int hideY = buttonsY - S(24);
    const int explainH = S(78);
    const int explainY = hideY - S(4) - explainH;
    m_list->setGeometry(m, S(48), w - 2 * m, explainY - S(6) - S(48));
    if (!m_columnsSet && isVisible()) {   // first layout: Check 34 %, Result 30 %, Details the rest
        m_columnsSet = true;
        const int lw = m_list->viewport()->width();
        m_list->header()->resizeSection(0, qMax(S(160), lw * 34 / 100));
        m_list->header()->resizeSection(1, qMax(S(140), lw * 30 / 100));
    }
    m_explain->setGeometry(m, explainY, w - 2 * m, explainH);
    m_hide->setGeometry(m, hideY, w - 2 * m, S(18));
    // buttons right-aligned in the original WinMX order
    struct B { PushButton *b; int w; };
    const B bs[] = {{m_test, 84}, {m_online, 118}, {m_copy, 88}, {m_save, 96}, {m_logs, 104}, {m_close, 75}};
    int x = w - m;
    for (int i = int(std::size(bs)) - 1; i >= 0; --i) {
        x -= S(bs[i].w);
        bs[i].b->setGeometry(x, buttonsY, S(bs[i].w), S(23));
        x -= S(6);
    }
    if (m_restored && isVisible())
        m_core->settings()->setValue(keys::ui::dialogKey(QStringLiteral("connectionTest")), saveGeometry());
}

void ConnectionTestDialog::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
    if (!m_columnsSet) {
        QResizeEvent re(size(), size());
        resizeEvent(&re);
    }
}

void ConnectionTestDialog::closeEvent(QCloseEvent *e)
{
    m_core->settings()->setValue(keys::ui::dialogKey(QStringLiteral("connectionTest")), saveGeometry());
    QDialog::closeEvent(e);
}

// [diag-1] script verbs (screenshots, UI tests); here, not in a file of their own, so a static
// link keeps the registration
namespace {

bool diagVerbs(MainWindow *mw, const QString &verb, const QString &rest, const QStringList &args, QString *error)
{
    Q_UNUSED(rest)
    if (verb == QLatin1String("open-conntest")) {
        ConnectionTestDialog::open(mw->core(), mw, args.value(0));
        return true;
    }
    ConnectionTestDialog *d = ConnectionTestDialog::instance();
    if (verb == QLatin1String("conntest-save")) {   // conntest-save <file>: the report as Save Report writes it
        if (!d) {
            *error = QStringLiteral("the Connection Test window is not open");
            return true;
        }
        QFile f(args.value(0));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) *error = QStringLiteral("cannot write %1").arg(args.value(0));
        else f.write(d->reportText().toUtf8());
        return true;
    }
    if (verb == QLatin1String("conntest-test") || verb == QLatin1String("conntest-select") || verb == QLatin1String("conntest-online")) {
        if (!d) {
            *error = QStringLiteral("the Connection Test window is not open");
            return true;
        }
        if (verb == QLatin1String("conntest-test")) {
            if (auto *b = d->findChild<QPushButton *>(QStringLiteral("diagTestNow"))) b->click();
        } else if (verb == QLatin1String("conntest-online")) {
            if (auto *b = d->findChild<QPushButton *>(QStringLiteral("diagCheckOnline"))) b->click();
        } else if (args.value(0) == QLatin1String("first-download")) {
            bool any = false;
            for (int r = 0; r < d->model()->rowCount() && !any; ++r) {
                const QString id = d->model()->index(r, 0).data(DiagIdRole).toString();
                if (id.startsWith(QLatin1String("dl."))) any = d->selectRow(id);
            }
            if (!any) *error = QStringLiteral("no download in the Connection Test");
        } else if (!d->selectRow(args.value(0))) {
            *error = QStringLiteral("no row %1").arg(args.value(0));
        }
        return true;
    }
    return false;
}

[[maybe_unused]] const bool g_registered = [] {
    ScriptDriver::addCommandHandler(&diagVerbs);
    return true;
}();

} // namespace

} // namespace mx::ui
