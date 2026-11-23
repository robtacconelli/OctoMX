// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/file_download_dialog.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QRadioButton>

#include "ui/kit/format.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"

namespace mx::ui {

namespace {
QLabel *text(QWidget *parent, const QString &t, int x, int y, int w = 540)
{
    auto *l = new QLabel(t, parent);
    l->setGeometry(SR(x, y, w, 14));
    return l;
}

PushButton *button(QDialog *d, const QString &t, const char *name, int x, int y, int w, bool ok)
{
    auto *b = new PushButton(t, d);
    b->setObjectName(QString::fromLatin1(name));
    b->setGeometry(SR(x, y, w, 22));
    b->setDefault(ok);
    b->setAutoDefault(ok);
    QObject::connect(b, &QPushButton::clicked, d, ok ? &QDialog::accept : &QDialog::reject);
    return b;
}

// the incomplete file looks like the hit: same name, or the name without the original WinMX's prefix / ".part"
int matchScore(const QString &partialName, const QString &hitName)
{
    QString p = partialName.toLower();
    const QString h = hitName.toLower();
    if (p.startsWith(QLatin1String("__incomplete___")))
        p = p.mid(15);
    for (const char *suffix : {".part", ".!qb", ".tmp", ".incomplete"})
        if (p.endsWith(QLatin1String(suffix)))
            p.chop(int(qstrlen(suffix)));
    if (p == h)
        return 2;
    const QString base = QFileInfo(h).completeBaseName();
    return (!base.isEmpty() && p.contains(base)) ? 1 : 0;
}
} // namespace

std::function<QString(QWidget *, const QString &)> FileDownloadDialog::s_browseHook;

FileDownloadDialog::FileDownloadDialog(const SearchHit &hit, const QList<PartialFile> &partials, const QString &incompleteDir,
                                       QWidget *parent)
    : QDialog(parent), m_dir(incompleteDir)
{
    // geometry measured on web/dialog_file_download_manual_resume.png (client 577 × 340)
    setObjectName(QStringLiteral("fileDownloadDialog"));
    setWindowTitle(tr("File Download"));
    setFixedSize(SS(577, 340));
    text(this, tr("The file you are about to download:"), 6, 9);
    text(this, hit.fileName, 26, 27)->setObjectName(QStringLiteral("fdFileName"));
    const SearchSource src = hit.sources.value(0);
    const QString who = hit.sources.size() == 1 ? (src.user.name.isEmpty() ? src.user.id : src.user.name)
                                                : tr("%1 users").arg(hit.sources.size());
    text(this, tr("from %1").arg(who), 28, 53, 180);
    text(this, tr("on %1").arg(hit.sources.size() == 1 ? netName(src.net) : tr("OctoMX")), 189, 53, 200);
    text(this, tr("may be resumed from partially downloaded file(s) on your computer."), 12, 74);
    text(this, tr("What would you like to do?"), 5, 105);
    m_new = new QRadioButton(tr("Save to a new file"), this);
    m_new->setObjectName(QStringLiteral("fdNewFile"));
    m_new->setToolTip(tr("Start this download from the beginning in a new file. The partial files stay where they are."));
    m_new->setGeometry(SR(15, 122, 300, 16));
    m_browse = new QRadioButton(tr("Manually browse for the file to resume from"), this);
    m_browse->setObjectName(QStringLiteral("fdBrowse"));
    m_browse->setToolTip(tr("After OK, pick the partial file yourself, for example one kept in another folder. The download continues in it."));
    m_browse->setGeometry(SR(15, 139, 300, 16));
    m_fromList = new QRadioButton(tr("Resume from the partially downloaded file:"), this);
    m_fromList->setObjectName(QStringLiteral("fdFromList"));
    m_fromList->setToolTip(tr("Continue in the partial file selected below, so the part you already have is not downloaded again."));
    m_fromList->setGeometry(SR(15, 156, 300, 16));

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("fdCandidates"));
    m_list->setGeometry(SR(7, 178, 562, 128));
    QList<PartialFile> sorted = partials;
    std::stable_sort(sorted.begin(), sorted.end(), [&hit](const PartialFile &a, const PartialFile &b) {
        return matchScore(a.name, hit.fileName) > matchScore(b.name, hit.fileName);
    });
    for (const PartialFile &p : sorted) {
        QString state;
        if (!p.downloadId.isEmpty())
            state = tr("Compatible multi-point download");   // in use by a running download: grey, not selectable
        else
            state = tr("Incomplete file (%1)").arg(fmt::exactSizes() ? fmt::exactBytes(p.size) : fmt::size(p.size));
        auto *it = new QListWidgetItem(QStringLiteral("%1   %2").arg(p.name, state), m_list);
        it->setData(Qt::UserRole, p.path);
        it->setToolTip(QDir::toNativeSeparators(p.path));
        if (!p.downloadId.isEmpty())
            it->setFlags(it->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
    }
    int first = -1;
    for (int i = 0; i < m_list->count() && first < 0; ++i)
        if (m_list->item(i)->flags() & Qt::ItemIsEnabled)
            first = i;
    if (first >= 0) {
        m_list->setCurrentRow(first);
        m_fromList->setChecked(true);
    } else {
        m_new->setChecked(true);
        m_fromList->setEnabled(false);
    }
    connect(m_list, &QListWidget::itemClicked, this, [this] { m_fromList->setChecked(true); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this] {
        m_fromList->setChecked(true);
        accept();
    });

    auto *note1 = text(this, tr("When another download is still using the partial file you pick here,"), 7, 307, 400);
    auto *note2 = text(this, tr("that download is stopped first and this one takes the file over."), 7, 321, 400);
    Q_UNUSED(note1) Q_UNUSED(note2)
    button(this, tr("OK"), "dlgOk", 408, 312, 82, true);
    button(this, tr("Cancel"), "dlgCancel", 491, 312, 78, false);
}

int FileDownloadDialog::candidateCount() const
{
    return m_list->count();
}

void FileDownloadDialog::choose(int option, int row)
{
    (option == 0 ? m_new : option == 1 ? m_browse : m_fromList)->setChecked(true);
    if (option == 2)
        m_list->setCurrentRow(row);
}

void FileDownloadDialog::accept()
{
    m_chosen.clear();
    if (m_browse->isChecked()) {
        const QString f = s_browseHook ? s_browseHook(this, m_dir)
                                       : QFileDialog::getOpenFileName(this, tr("Resume From"), m_dir, tr("All Files (*)"));
        if (f.isEmpty())
            return;   // stay open
        m_chosen = f;
    } else if (m_fromList->isChecked()) {
        QListWidgetItem *it = m_list->currentItem();
        if (!it || !(it->flags() & Qt::ItemIsEnabled))
            return;
        m_chosen = it->data(Qt::UserRole).toString();
    }
    QDialog::accept();
}

// ------------------------------------------------------------------------- LoadIncompleteDialog

LoadIncompleteDialog::LoadIncompleteDialog(const QList<PartialFile> &partials, const QString &incompleteDir, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("loadIncompleteDialog"));
    setWindowTitle(tr("Load Incomplete"));
    setFixedSize(SS(577, 300));
    text(this, tr("Incomplete files in %1:").arg(QDir::toNativeSeparators(incompleteDir)), 7, 9, 560);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("liFiles"));
    m_list->setGeometry(SR(7, 27, 562, 160));
    for (const PartialFile &p : partials) {
        if (!p.downloadId.isEmpty())
            continue;   // already in the Transfers list
        auto *it = new QListWidgetItem(QStringLiteral("%1   Incomplete file (%2)").arg(p.name, fmt::exactSizes() ? fmt::exactBytes(p.size) : fmt::size(p.size)), m_list);
        it->setData(Qt::UserRole, p.path);
        it->setToolTip(QDir::toNativeSeparators(p.path));
    }
    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
    text(this, tr("Link of the same file (optional: magnet: or ed2k://):"), 7, 195, 560);
    m_link = new QLineEdit(this);
    m_link->setObjectName(QStringLiteral("liLink"));
    m_link->setToolTip(tr("Paste a magnet: or ed2k:// link of exactly this file: the download goes on from it. Empty: OctoMX searches for the file name."));
    m_link->setGeometry(SR(7, 212, 562, 21));
    text(this, tr("Without a link OctoMX searches for the file name; double-click the matching result to resume."), 7, 241, 560);
    auto *ok = button(this, tr("Resume"), "dlgOk", 408, 268, 82, true);
    ok->setToolTip(tr("Continue the selected partial file: with a link the download starts at once, without one OctoMX searches "
                      "for the file name and you double-click the matching result."));
    button(this, tr("Cancel"), "dlgCancel", 491, 268, 78, false);
    ok->setEnabled(m_list->count() > 0);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
}

QString LoadIncompleteDialog::chosenFile() const
{
    QListWidgetItem *it = m_list->currentItem();
    return it ? it->data(Qt::UserRole).toString() : QString();
}

QString LoadIncompleteDialog::link() const
{
    return m_link->text().trimmed();
}

int LoadIncompleteDialog::rowCount() const
{
    return m_list->count();
}

void LoadIncompleteDialog::selectRow(int row)
{
    m_list->setCurrentRow(row);
}

void LoadIncompleteDialog::setLink(const QString &l)
{
    m_link->setText(l);
}

} // namespace mx::ui
