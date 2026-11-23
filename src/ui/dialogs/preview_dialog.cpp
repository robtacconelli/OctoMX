// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/preview_dialog.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QTreeWidget>

#include "mx/download_order.h"
#include "mx/services.h"
#include "mx/settings_keys.h"
#include "ui/kit/format.h"
#include "ui/kit/launcher.h"
#include "ui/kit/scale.h"
#include "ui/kit/widgets.h"

namespace mx::ui {

namespace {
PushButton *dlgButton(QDialog *d, const QString &t, const char *name, int x, int y, bool ok, const QString &tip)
{
    auto *b = new PushButton(t, d);
    b->setObjectName(QString::fromLatin1(name));
    b->setGeometry(SR(x, y, 75, 23));
    b->setDefault(ok);
    b->setAutoDefault(ok);
    b->setToolTip(tip);
    QObject::connect(b, &QPushButton::clicked, d, ok ? &QDialog::accept : &QDialog::reject);
    return b;
}
} // namespace

// ------------------------------------------------------------------------------- PreviewWithDialog

std::function<int(PreviewWithDialog *)> PreviewWithDialog::s_execHook;
std::function<QString(QWidget *)> PreviewWithDialog::s_browseHook;

PreviewWithDialog::PreviewWithDialog(const QString &fileName, const QString &choice, const QString &program, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("previewWithDialog"));
    setWindowTitle(tr("Preview With"));
    setFixedSize(SS(360, 170));
    auto *l = new QLabel(this);
    l->setGeometry(SR(12, 10, 336, 14));
    l->setText(fmt::elide(tr("Play \"%1\" with:").arg(fileName), l->fontMetrics(), S(336)));
    l->setToolTip(fileName);
    m_system = new QRadioButton(tr("The system's default player"), this);
    m_system->setObjectName(QStringLiteral("pwSystem"));
    m_system->setGeometry(SR(20, 32, 300, 16));
    m_system->setToolTip(tr("Play with the program your system opens this type of file with."));
    m_program = new QRadioButton(tr("This program:"), this);
    m_program->setObjectName(QStringLiteral("pwProgram"));
    m_program->setGeometry(SR(20, 52, 300, 16));
    m_program->setToolTip(tr("Play with the player below. It gets the file as its only argument."));
    m_path = new QComboBox(this);
    m_path->setObjectName(QStringLiteral("pwPath"));
    m_path->setEditable(true);
    m_path->setGeometry(SR(36, 72, 240, 21));
    m_path->setToolTip(tr("The player program. The players found on this computer are in the list; any other one can be typed in "
                          "or picked with Browse..."));
    for (const launcher::Player &p : launcher::findPlayers())
        m_path->addItem(QStringLiteral("%1").arg(p.path), p.path);
    for (int i = 0; i < m_path->count(); ++i)
        m_path->setItemData(i, m_path->itemText(i), Qt::ToolTipRole);
    if (!program.isEmpty())
        m_path->setEditText(QDir::toNativeSeparators(program));
    auto *browse = new PushButton(tr("Browse..."), this);
    browse->setObjectName(QStringLiteral("pwBrowse"));
    browse->setGeometry(SR(282, 71, 66, 23));
    browse->setToolTip(tr("Pick the player program on your computer."));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString f = s_browseHook ? s_browseHook(this)
                                       : QFileDialog::getOpenFileName(this, tr("Choose Player"), QString(),
#ifdef Q_OS_WIN
                                                                      tr("Programs (*.exe)")
#else
                                                                      QString()
#endif
                                         );
        if (!f.isEmpty()) {
            m_path->setEditText(QDir::toNativeSeparators(f));
            m_program->setChecked(true);
        }
    });
    m_remember = new QCheckBox(tr("Remember my choice"), this);
    m_remember->setObjectName(QStringLiteral("pwRemember"));
    m_remember->setGeometry(SR(20, 104, 200, 16));
    m_remember->setChecked(true);
    m_remember->setToolTip(tr("Use this player for every preview without asking again. You can change it in Settings, "
                              "File Launching, Preview Player."));
    dlgButton(this, tr("OK"), "dlgOk", 192, 136, true, tr("Start the preview with the chosen player."));
    dlgButton(this, tr("Cancel"), "dlgCancel", 273, 136, false, tr("Do not start a preview."));
    connect(m_path, &QComboBox::editTextChanged, this, [this] { m_program->setChecked(true); });
    setChoice(choice.isEmpty() ? QStringLiteral("system") : choice, program);
}

void PreviewWithDialog::setChoice(const QString &choice, const QString &program)
{
    if (!program.isEmpty())
        m_path->setEditText(QDir::toNativeSeparators(program));
    (choice == QLatin1String("program") ? m_program : m_system)->setChecked(true);
}

void PreviewWithDialog::setRemember(bool on)
{
    m_remember->setChecked(on);
}

QString PreviewWithDialog::choice() const
{
    return m_program->isChecked() ? QStringLiteral("program") : QStringLiteral("system");
}

QString PreviewWithDialog::program() const
{
    return QDir::fromNativeSeparators(m_path->currentText().trimmed());
}

bool PreviewWithDialog::remember() const
{
    return m_remember->isChecked();
}

void PreviewWithDialog::accept()
{
    if (choice() == QLatin1String("program") && !QFileInfo(program()).isFile()) {
        if (!s_execHook) {
            QMessageBox::information(this, tr("Preview With"),
                                     tr("The program \"%1\" was not found.").arg(QDir::toNativeSeparators(program())));
        }
        return;
    }
    QDialog::accept();
}

// ------------------------------------------------------------------------------- PreviewFileDialog

std::function<int(PreviewFileDialog *)> PreviewFileDialog::s_execHook;

PreviewFileDialog::PreviewFileDialog(const QString &bundleName, const QList<PreviewFile> &files, QWidget *parent)
    : QDialog(parent), m_files(files)
{
    setObjectName(QStringLiteral("previewFileDialog"));
    setWindowTitle(tr("Preview"));
    setFixedSize(SS(480, 230));
    auto *l = new QLabel(this);
    l->setGeometry(SR(12, 10, 456, 14));
    l->setText(fmt::elide(tr("Choose the file of \"%1\" to preview:").arg(bundleName), l->fontMetrics(), S(456)));
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("previewFiles"));
    m_tree->setGeometry(SR(12, 28, 456, 158));
    m_tree->setRootIsDecorated(false);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("File"), tr("Progress"), tr("Preview")});
    m_tree->header()->setStretchLastSection(true);
    m_tree->setColumnWidth(0, S(196));
    m_tree->setColumnWidth(1, S(58));
    m_tree->setToolTip(tr("The audio and video files of this download with how much of each is on disk. Grey files cannot be "
                          "played yet."));
    int first = -1;
    for (int i = 0; i < files.size(); ++i) {
        const PreviewFile &f = files.at(i);
        QString why = f.reason;   // "Preview: needs ..." -> "Needs ..." (the column says Preview)
        if (why.startsWith(QLatin1String("Preview: ")))
            why = why.mid(9, 1).toUpper() + why.mid(10);
        auto *it = new QTreeWidgetItem(m_tree, {QFileInfo(f.name).fileName(), QStringLiteral("%1 %").arg(fmt::percentInt(f.done, f.size)),
                                                f.ready ? tr("Ready") : why});
        it->setToolTip(0, f.name);
        it->setToolTip(2, f.ready ? tr("Ready") : f.reason);
        it->setData(0, Qt::UserRole, f.index);
        if (!f.ready)
            it->setFlags(it->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
        else if (first < 0)
            first = i;
    }
    if (first >= 0)
        m_tree->setCurrentItem(m_tree->topLevelItem(first));
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this] { accept(); });
    dlgButton(this, tr("OK"), "dlgOk", 312, 196, true, tr("Preview the selected file."));
    dlgButton(this, tr("Cancel"), "dlgCancel", 393, 196, false, tr("Do not start a preview."));
}

int PreviewFileDialog::chosenIndex() const
{
    const QTreeWidgetItem *it = m_tree->currentItem();
    if (!it || !(it->flags() & Qt::ItemIsEnabled))
        return -2;
    return it->data(0, Qt::UserRole).toInt();
}

int PreviewFileDialog::rowCount() const
{
    return m_tree->topLevelItemCount();
}

void PreviewFileDialog::selectRow(int row)
{
    m_tree->setCurrentItem(m_tree->topLevelItem(row));
}

// ------------------------------------------------------------------------------------- the flow

namespace previewui {

MenuEntry menuEntry(Core *core, const TransferInfo &t)
{
    MenuEntry e;
    bool media = !t.isBundle && preview::isMediaFile(t.name);
    for (const BundleFile &f : t.files)
        if (t.isBundle && f.wanted && preview::isMediaFile(f.path))
            media = true;
    if (!media || t.state == TransferState::Cancelled)
        return e;
    if (core->settings() && !core->settings()->b(keys::launch::PreviewIncomplete))
        return e;   // switched off in Settings, File Launching, Launch Options
    const PreviewInfo pi = core->transfers()->previewInfo(t.id);
    if (!pi.supported)
        return e;
    e.show = true;
    const bool both = t.sequential && t.firstLastFirst;
    int ready = 0;
    QString reason;
    for (const PreviewFile &f : pi.files) {
        if (f.ready)
            ++ready;
        else if (reason.isEmpty())
            reason = f.reason;
    }
    if (ready > 0) {
        e.enabled = e.ready = true;
        e.text = pi.files.size() > 1 ? QObject::tr("Preview...") : QObject::tr("Preview");
        e.tip = QObject::tr("Play what is on disk of this file with your preview player while it downloads.");
        return e;
    }
    e.text = reason.isEmpty() ? QObject::tr("Preview: needs the beginning of the file") : reason;
    // not yet: the entry offers the two order flags, so the file becomes previewable soon
    e.enabled = !both;
    e.tip = both ? QObject::tr("This download already fetches the beginning and the end first. Preview becomes possible when "
                               "enough of it is on disk.")
                 : QObject::tr("Switch on \"Download In Sequential Order\" and \"Download First And Last Pieces First\" for "
                               "this download, so it can be previewed soon.");
    return e;
}

bool choosePlayer(QWidget *parent, Settings *settings, const QString &fileName, QString *choice, QString *program, bool forceAsk)
{
    QString c = settings ? settings->s(keys::launch::PreviewPlayer) : QString();
    QString p = settings ? settings->s(keys::launch::PreviewProgram) : QString();
    if (!forceAsk && (c == QLatin1String("system") || (c == QLatin1String("program") && !p.isEmpty()))) {
        *choice = c;
        *program = p;
        return true;
    }
    PreviewWithDialog dlg(fileName, c, p, parent);
    const int r = PreviewWithDialog::s_execHook ? PreviewWithDialog::s_execHook(&dlg) : dlg.exec();
    if (r != QDialog::Accepted)
        return false;
    *choice = dlg.choice();
    *program = dlg.choice() == QLatin1String("program") ? dlg.program() : QString();
    if (dlg.remember() && settings) {
        settings->setValue(QString(keys::launch::PreviewPlayer), *choice);
        if (*choice == QLatin1String("program"))
            settings->setValue(QString(keys::launch::PreviewProgram), *program);
    }
    return true;
}

bool run(QWidget *parent, Core *core, const TransferInfo &t, int fileIndex, QString *why)
{
    if (why)
        why->clear();
    TransferService *ts = core->transfers();
    const PreviewInfo pi = ts->previewInfo(t.id);
    if (fileIndex == -2) {
        if (pi.files.size() > 1) {
            PreviewFileDialog dlg(t.name, pi.files, parent);
            const int r = PreviewFileDialog::s_execHook ? PreviewFileDialog::s_execHook(&dlg) : dlg.exec();
            if (r != QDialog::Accepted || dlg.chosenIndex() == -2)
                return false;
            fileIndex = dlg.chosenIndex();
        } else {
            fileIndex = pi.files.isEmpty() ? -1 : pi.files.first().index;
        }
    }
    QString err;
    const QString path = ts->preparePreview(t.id, fileIndex, &err);
    if (path.isEmpty()) {
        if (why)
            *why = err.isEmpty() ? QObject::tr("This file cannot be previewed now.") : err;
        return false;
    }
    QString choice, program;
    if (!choosePlayer(parent, core->settings(), QFileInfo(path).fileName(), &choice, &program))
        return false;
    return launcher::previewFile(parent, path, choice, program, why);
}

} // namespace previewui
} // namespace mx::ui
