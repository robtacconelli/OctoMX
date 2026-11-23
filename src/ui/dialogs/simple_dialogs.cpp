// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/simple_dialogs.h"

#include <QtCore/QRegularExpression>

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTreeWidget>

#include "ui/kit/format.h"
#include "ui/kit/icons.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"

namespace mx::ui {

namespace {
PushButton *okButton(QDialog *d, int x, int y)
{
    auto *b = new PushButton(QObject::tr("OK"), d);
    b->setObjectName(QStringLiteral("dlgOk"));
    b->setDefault(true);
    b->setAutoDefault(true);
    b->setGeometry(SR(x, y, 75, 23));
    QObject::connect(b, &QPushButton::clicked, d, &QDialog::accept);
    return b;
}
PushButton *cancelButton(QDialog *d, int x, int y)
{
    auto *b = new PushButton(QObject::tr("Cancel"), d);
    b->setObjectName(QStringLiteral("dlgCancel"));
    b->setGeometry(SR(x, y, 75, 23));
    QObject::connect(b, &QPushButton::clicked, d, &QDialog::reject);
    return b;
}
} // namespace

// ----------------------------------------------------------------------------- ChangeNameDialog

ChangeNameDialog::ChangeNameDialog(const QString &current, QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("changeNameDialog"));
    setWindowTitle(tr("Change Screen Name"));
    setFixedSize(SS(300, 104));
    auto *l = new QLabel(tr("Enter your new screen name:"), this);
    l->setGeometry(SR(12, 12, 276, 14));
    m_edit = new QLineEdit(this);
    m_edit->setObjectName(QStringLiteral("screenNameEdit"));
    m_edit->setMaxLength(40);
    // the original WinMX shows the name without the "_12345" suffix it appends
    QString base = current;
    const int us = base.lastIndexOf(QLatin1Char('_'));
    if (us > 0 && base.size() - us == 6)
        base = base.left(us);
    m_edit->setText(base);
    m_edit->selectAll();
    m_edit->setGeometry(SR(12, 32, 276, 21));
    // [ovl-2] the full name it becomes: the "_12345" part is fixed (the OctoMX network derives it from
    // your user key, so every other user sees exactly this name)
    const QString sfx = us > 0 && current.size() - us == 6 ? current.mid(us) : QString();
    if (!sfx.isEmpty()) {
        setFixedSize(SS(300, 124));
        auto *preview = new QLabel(this);
        preview->setObjectName(QStringLiteral("screenNamePreview"));
        preview->setGeometry(SR(12, 58, 276, 14));
        auto update = [this, preview, sfx] {
            QString b = m_edit->text().trimmed();
            static const QRegularExpression ws(QStringLiteral("[\\s\\\\/:*?\"<>|@%]+"));
            b.replace(ws, QStringLiteral("_"));
            static const QRegularExpression typed(QStringLiteral("_\\d{5}$"));
            b.remove(typed);
            preview->setText(tr("Others will see you as: %1").arg((b.isEmpty() ? QStringLiteral("Guest") : b.left(26)) + sfx));
        };
        connect(m_edit, &QLineEdit::textChanged, this, update);
        update();
        okButton(this, 132, 88);
        cancelButton(this, 213, 88);
        return;
    }
    okButton(this, 132, 68);
    cancelButton(this, 213, 68);
}

QString ChangeNameDialog::name() const
{
    return m_edit->text().trimmed();
}

// ----------------------------------------------------------------------- CustomExtensionsDialog

CustomExtensionsDialog::CustomExtensionsDialog(const QString &current, QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("customExtensionsDialog"));
    setWindowTitle(tr("Custom Extensions"));
    setFixedSize(SS(300, 120));
    auto *l = new QLabel(tr("Search for files with these extensions:"), this);
    l->setGeometry(SR(12, 12, 276, 14));
    m_edit = new QLineEdit(this);
    m_edit->setObjectName(QStringLiteral("customExtensionsEdit"));
    m_edit->setToolTip(tr("Extensions without dots, separated by spaces or commas, for example: mkv srt. Only files of these types are shown."));
    m_edit->setMaxLength(200);
    m_edit->setText(current);
    m_edit->selectAll();
    m_edit->setGeometry(SR(12, 30, 276, 21));
    auto *hint = new QLabel(tr("Separate them with spaces or commas, for example: mkv srt"), this);
    hint->setGeometry(SR(12, 56, 276, 14));
    okButton(this, 132, 84);
    cancelButton(this, 213, 84);
}

QString CustomExtensionsDialog::text() const
{
    return m_edit->text();
}

QStringList CustomExtensionsDialog::extensions() const
{
    return mx::parseExtensionList(m_edit->text());
}

// --------------------------------------------------------------------------------- ServerDialog

ServerDialog::ServerDialog(const QString &title, QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("serverDialog"));
    setWindowTitle(title);
    setFixedSize(SS(320, 170));
    auto label = [this](const QString &t, int y) {
        auto *l = new QLabel(t, this);
        l->setGeometry(SR(12, y + 3, 80, 14));
    };
    label(tr("Network:"), 12);
    m_net = new QComboBox(this);
    for (Net n : {Net::ED2K, Net::Kad, Net::Gnutella, Net::BitTorrent, Net::Overlay})
        m_net->addItem(netName(n), int(n));
    m_net->setGeometry(SR(96, 12, 212, 21));
    label(tr("Name:"), 40);
    m_name = new QLineEdit(this);
    m_name->setGeometry(SR(96, 40, 212, 21));
    label(tr("Host:"), 68);
    m_host = new QLineEdit(this);
    m_host->setObjectName(QStringLiteral("serverHost"));
    m_host->setGeometry(SR(96, 68, 212, 21));
    label(tr("Port:"), 96);
    m_port = new QLineEdit(this);
    m_port->setGeometry(SR(96, 96, 60, 21));
    m_port->setText(QStringLiteral("4661"));
    okButton(this, 152, 134);
    cancelButton(this, 233, 134);
}

void ServerDialog::setNetwork(Net n)
{
    const int i = m_net->findData(int(n));
    if (i >= 0)
        m_net->setCurrentIndex(i);
}

void ServerDialog::setAddress(const QString &a)
{
    const int c = a.lastIndexOf(QLatin1Char(':'));
    if (c > 0) {
        m_host->setText(a.left(c));
        m_port->setText(a.mid(c + 1));
    } else {
        m_host->setText(a);
    }
}

void ServerDialog::setName(const QString &n)
{
    m_name->setText(n);
}

Net ServerDialog::network() const
{
    return Net(m_net->currentData().toInt());
}

QString ServerDialog::address() const
{
    const QString h = m_host->text().trimmed();
    if (h.isEmpty())
        return {};
    if (h.contains(QLatin1Char(':')) || m_port->text().trimmed().isEmpty())
        return h;
    return h + QLatin1Char(':') + m_port->text().trimmed();
}

QString ServerDialog::name() const
{
    return m_name->text().trimmed();
}

// -------------------------------------------------------------------------------- AddLinkDialog

AddLinkDialog::AddLinkDialog(QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("addLinkDialog"));
    setWindowTitle(tr("Add Link"));
    setFixedSize(SS(420, 236));
    auto *l = new QLabel(tr("Paste magnet:, ed2k:// or http:// links (one per line), or browse for .torrent files:"), this);
    l->setWordWrap(true);
    l->setGeometry(SR(12, 8, 396, 28));
    m_edit = new QPlainTextEdit(this);
    m_edit->setObjectName(QStringLiteral("linkEdit"));
    m_edit->setGeometry(SR(12, 40, 396, 110));
    auto *browse = new PushButton(tr("Browse..."), this);
    browse->setObjectName(QStringLiteral("linkBrowse"));
    browse->setToolTip(tr("Pick one or more .torrent files on your computer; their paths are added to the box above."));
    browse->setGeometry(SR(12, 200, 75, 23));
    // [dl-1] download order of the new downloads (qBittorrent's two flags)
    m_seq = new QCheckBox(tr("Download in sequential order"), this);
    m_seq->setObjectName(QStringLiteral("linkSequential"));
    m_seq->setGeometry(SR(12, 156, 396, 16));
    m_seq->setToolTip(tr("Fetch the file from its beginning to its end, as far as the sources allow."));
    m_firstLast = new QCheckBox(tr("Download first and last pieces first"), this);
    m_firstLast->setObjectName(QStringLiteral("linkFirstLast"));
    m_firstLast->setGeometry(SR(12, 174, 396, 16));
    m_firstLast->setToolTip(tr("Fetch the beginning and the end of the file first, so audio and video can be previewed early."));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open Torrent"), QString(), tr("Torrent files (*.torrent)"));
        for (const QString &f : files)
            m_edit->appendPlainText(f);
    });
    okButton(this, 252, 200);
    cancelButton(this, 333, 200);
}

void AddLinkDialog::setOrder(bool sequential, bool firstLast)
{
    m_seq->setChecked(sequential);
    m_firstLast->setChecked(firstLast);
}

bool AddLinkDialog::sequential() const
{
    return m_seq->isChecked();
}

bool AddLinkDialog::firstLast() const
{
    return m_firstLast->isChecked();
}

void AddLinkDialog::setText(const QString &t)
{
    m_edit->setPlainText(t);
}

QStringList AddLinkDialog::links() const
{
    QStringList out;
    for (const QString &l : m_edit->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        if (!l.trimmed().isEmpty())
            out << l.trimmed();
    return out;
}

// ---------------------------------------------------------------------------- BundleFilesDialog

BundleFilesDialog::BundleFilesDialog(const QString &bundleName, const QList<BundleFile> &files, const QList<int> &selected,
                                     QWidget *parent)
    : QDialog(parent)
{
    // BitTorrent pad files (".pad/...", made by some clients) are never shown
    for (const BundleFile &f : files)
        if (!f.path.startsWith(QLatin1String(".pad/")) && !f.path.contains(QLatin1String("/.pad/")))
            m_files << f;
    setObjectName(QStringLiteral("bundleFilesDialog"));
    setWindowTitle(tr("Select Files To Download"));
    setFixedSize(SS(460, 330));
    // [qa-1] no literal "(s)" (no translations are loaded)
    const QString count = m_files.size() == 1 ? tr("1 file") : tr("%1 files").arg(m_files.size());
    auto *l = new QLabel(tr("The bundle \"%1\" contains %2. Check the files you want to download:").arg(bundleName, count),
                         this);
    l->setWordWrap(true);
    l->setGeometry(SR(12, 8, 436, 28));
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("bundleTree"));
    m_tree->setRootIsDecorated(false);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("File"), tr("Bytes")});
    m_tree->header()->resizeSection(0, S(320));
    m_tree->headerItem()->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    for (const BundleFile &f : std::as_const(m_files)) {
        auto *it = new QTreeWidgetItem(m_tree);
        it->setText(0, f.path);
        it->setText(1, fmt::size(f.size));
        it->setToolTip(1, fmt::exactBytes(f.size));
        it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        it->setIcon(0, icon(IconId::File));
        it->setData(0, Qt::UserRole, f.index);
        it->setCheckState(0, (selected.isEmpty() || selected.contains(f.index)) ? Qt::Checked : Qt::Unchecked);
    }
    m_tree->setGeometry(SR(12, 40, 436, 220));
    connect(m_tree, &QTreeWidget::itemChanged, this, &BundleFilesDialog::updateTotal);
    m_total = new QLabel(this);
    m_total->setGeometry(SR(12, 266, 436, 14));
    auto *all = new PushButton(tr("Select All"), this);
    all->setObjectName(QStringLiteral("bundleSelectAll"));
    all->setToolTip(tr("Tick every file of this download, so all of them are downloaded."));
    all->setGeometry(SR(12, 294, 75, 23));
    connect(all, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
            m_tree->topLevelItem(i)->setCheckState(0, Qt::Checked);
    });
    auto *none = new PushButton(tr("Select None"), this);
    none->setObjectName(QStringLiteral("bundleSelectNone"));
    none->setToolTip(tr("Untick every file, then tick only the ones you want."));
    none->setGeometry(SR(92, 294, 75, 23));
    connect(none, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
            m_tree->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
    });
    okButton(this, 292, 294);
    cancelButton(this, 373, 294);
    updateTotal();
}

void BundleFilesDialog::updateTotal()
{
    qint64 total = 0;
    int n = 0;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        if (m_tree->topLevelItem(i)->checkState(0) == Qt::Checked) {
            ++n;
            total += m_files.value(i).size;
        }
    m_total->setText(tr("%1 of %2 files selected, %3").arg(n).arg(m_files.size()).arg(fmt::exactSizes() ? fmt::exactBytes(total) : fmt::size(total)));
}

QList<int> BundleFilesDialog::selectedIndexes() const
{
    QList<int> out;
    bool all = true;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        if (m_tree->topLevelItem(i)->checkState(0) == Qt::Checked)
            out << m_tree->topLevelItem(i)->data(0, Qt::UserRole).toInt();
        else
            all = false;
    }
    return all ? QList<int>() : out;
}

} // namespace mx::ui
