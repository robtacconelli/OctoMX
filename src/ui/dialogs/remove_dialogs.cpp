// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/dialogs/remove_dialogs.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtGui/QFontMetrics>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QStyle>

#include "mx/trash.h"
#include "ui/kit/format.h"
#include "ui/kit/icons.h"
#include "ui/kit/scale.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"

namespace mx::ui {

namespace {
QString q(const QString &name)
{
    return QStringLiteral("\"%1\"").arg(name);
}

// a path as shown: whole when it fits the text column, else shortened in the middle (the file name stays)
QString native(const QString &p, bool bold = false)
{
    return fmt::elideMiddle(QDir::toNativeSeparators(p), QFontMetrics(bold ? boldFont() : normalFont()), S(340));
}

QString place()
{
    return trash::placeName();
}

// one line of the multi-selection list
QString itemLine(const RemoveItem &it)
{
    const DownloadDiskInfo &d = it.disk;
    QString state;
    if (d.paths.isEmpty())
        state = QObject::tr("nothing on the disk yet");
    else if (d.finished)
        state = QObject::tr("finished, %1").arg(fmt::size(d.bytes));
    else if (d.total > 0)
        state = QObject::tr("partial, %1").arg(fmt::sizeOf(d.bytes, d.total));
    else
        state = QObject::tr("partial, %1").arg(fmt::size(d.bytes));
    return QStringLiteral("%1   (%2)").arg(it.t.name, state);
}

// one block of text of a classic message box (none for an empty text)
struct BoxBuilder {
    QDialog *dlg;
    WrapLabel *add(const QString &text, bool bold, const char *name)
    {
        if (text.isEmpty())
            return nullptr;
        auto *l = new WrapLabel(text, dlg);
        l->setObjectName(QString::fromLatin1(name));
        if (bold)
            l->setFont(boldFont());
        return l;
    }
};

void layoutBox(QDialog *dlg, QLabel *icon, const QList<WrapLabel *> &labels, QCheckBox *box, const QList<QPushButton *> &buttons)
{
    // button widths from their texts; the dialog is as wide as the row needs (at least 420 design px)
    const QFontMetrics fm(dlg->font());
    QList<int> bw;
    int rowW = 0;
    for (QPushButton *b : buttons) {
        const int w = qMax(S(75), fm.horizontalAdvance(b->text()) + S(20));
        bw << w;
        rowW += w;
    }
    rowW += S(6) * int(qMax<qsizetype>(0, buttons.size() - 1));
    const int width = qMax(S(420), rowW + S(24));
    const int textX = S(56);
    const int textW = width - textX - S(14);
    icon->setGeometry(S(12), S(14), S(32), S(32));
    int y = S(14);
    for (WrapLabel *l : labels) {
        if (!l)
            continue;
        const int h = qMax(l->linePitch(), l->heightForWidth(textW));
        l->setGeometry(textX, y, textW, h);
        y += h + S(8);
    }
    y = qMax(y, S(56));
    if (box) {
        box->setGeometry(textX, y, textW, S(16));
        y += S(16) + S(10);
    }
    int x = width - S(12) - rowW;
    for (int i = 0; i < buttons.size(); ++i) {
        buttons[i]->setGeometry(x, y, bw[i], S(23));
        x += bw[i] + S(6);
    }
    y += S(23) + S(12);
    dlg->setFixedSize(width, y);
}

QLabel *iconLabel(QDialog *dlg, bool warn)
{
    auto *icon = new QLabel(dlg);
    icon->setObjectName(QStringLiteral("boxIcon"));
    icon->setPixmap(dlg->style()->standardIcon(warn ? QStyle::SP_MessageBoxWarning : QStyle::SP_MessageBoxQuestion).pixmap(S(32), S(32)));
    return icon;
}
} // namespace

// ================================================================================= downloads

RemoveTexts removeTexts(const QList<RemoveItem> &items)
{
    RemoveTexts r;
    const int n = int(items.size());
    int finished = 0, partial = 0;
    qint64 bytes = 0;
    bool forGood = false, inPlace = false;
    QStringList keptFiles;
    QString keptWhy;
    for (const RemoveItem &it : items) {
        const DownloadDiskInfo &d = it.disk;
        if (!d.paths.isEmpty())
            ++(d.finished ? finished : partial);
        bytes += d.bytes;
        if (!d.paths.isEmpty() && !d.trash)
            forGood = true;
        if (!d.inPlaceFile.isEmpty())
            inPlace = true;
        for (const QString &k : d.kept)
            keptFiles << QFileInfo(k).fileName();
        if (!d.keptWhy.isEmpty())
            keptWhy = d.keptWhy;
    }
    r.bytes = bytes;
    r.canDelete = finished + partial > 0;
    r.forGood = forGood;
    const QString where = place();
    if (n == 1) {
        const RemoveItem &it = items.first();
        const DownloadDiskInfo &d = it.disk;
        r.title = QObject::tr("Remove Download");
        r.question = QObject::tr("Remove %1 from the download list?").arg(q(it.t.name));
        r.keepButton = QObject::tr("Remove from the list, keep the file");
        r.deleteButton = QObject::tr("Remove from the list and delete the file");
        if (d.paths.isEmpty()) {
            r.details = QObject::tr("Nothing of this download is on the disk yet, so there is no file to keep or delete.");
        } else if (d.finished) {
            r.details = QObject::tr("The download is finished. The file (%1) is in %2.").arg(fmt::size(d.bytes), native(d.folder));
            r.keepLine = QObject::tr("Keep the file: only the row goes, the file stays where it is.");
            r.deleteLine = d.trash ? QObject::tr("Delete the file: the file is moved to the %1, where you can restore it.").arg(where)
                                   : QObject::tr("Delete the file: the file is deleted for good. It is on a drive without a %1 and "
                                                 "cannot be restored.").arg(where);
        } else {
            r.details = d.total > 0 ? QObject::tr("The download is not finished: %1 are on the disk, in %2.")
                                          .arg(fmt::sizeOf(d.bytes, d.total), native(d.folder))
                                    : QObject::tr("The download is not finished: %1 are on the disk, in %2.").arg(fmt::size(d.bytes), native(d.folder));
            r.keepLine = QObject::tr("Keep the file: the partial file stays on the disk. Load Incomplete, or downloading the same "
                                     "file again, continues it.");
            r.deleteLine = d.trash ? QObject::tr("Delete the file: the partial file (%1) is moved to the %2, and OctoMX forgets "
                                                 "the download.").arg(fmt::size(d.bytes), where)
                                   : QObject::tr("Delete the file: the partial file (%1) is deleted for good. It is on a drive "
                                                 "without a %2 and cannot be restored.").arg(fmt::size(d.bytes), where);
        }
        if (!d.inPlaceFile.isEmpty())
            r.warning = QObject::tr("This download was resumed from a file you chose and writes into it: deleting deletes "
                                    "\"%1\" (in %2).")
                            .arg(QFileInfo(d.inPlaceFile).fileName(), native(QFileInfo(d.inPlaceFile).absolutePath(), true));
    } else {
        r.title = QObject::tr("Remove Downloads");
        r.question = QObject::tr("Remove these %1 downloads from the download list?").arg(n);
        r.keepButton = QObject::tr("Remove from the list, keep the files");
        r.deleteButton = QObject::tr("Remove from the list and delete the files");
        QStringList lines;
        for (int i = 0; i < n && i < 6; ++i)
            lines << itemLine(items.at(i));
        if (n > 6)
            lines << QObject::tr("and %1 more").arg(n - 6);
        r.list = lines.join(QLatin1Char('\n'));
        QStringList parts;
        if (finished)
            parts << (finished == 1 ? QObject::tr("1 finished file") : QObject::tr("%1 finished files").arg(finished));
        if (partial)
            parts << (partial == 1 ? QObject::tr("the partial data of 1 download") : QObject::tr("the partial data of %1 downloads").arg(partial));
        if (!r.canDelete)
            r.details = QObject::tr("Nothing of these downloads is on the disk yet, so there are no files to keep or delete.");
        else
            r.details = QObject::tr("On the disk: %1 in %2.").arg(fmt::size(bytes), parts.join(QObject::tr(" and ")));
        if (r.canDelete) {
            r.keepLine = QObject::tr("Keep the files: only the rows go. Finished files stay where they are; partial downloads can "
                                     "be continued later with Load Incomplete or by downloading them again.");
            r.deleteLine = forGood ? QObject::tr("Delete the files: all of it (%1) goes. Some of it is on a drive without a %2: "
                                                 "that part is deleted for good and cannot be restored; the rest is moved to the %2.")
                                         .arg(fmt::size(bytes), where)
                                   : QObject::tr("Delete the files: all of it (%1) is moved to the %2, where you can restore it.")
                                         .arg(fmt::size(bytes), where);
        }
        if (inPlace)
            r.warning = QObject::tr("One of them was resumed from a file you chose and writes into it: deleting deletes that file too.");
    }
    if (!keptFiles.isEmpty()) {
        QStringList shown = keptFiles.mid(0, 3);
        if (keptFiles.size() > 3)
            shown << QObject::tr("and %1 more").arg(keptFiles.size() - 3);
        r.keptNote = QObject::tr("Kept in any case, because %1: %2.").arg(keptWhy.isEmpty() ? QObject::tr("this download did not create it") : keptWhy,
                                                                         shown.join(QStringLiteral(", ")));
    }
    return r;
}

std::function<int(RemoveDownloadDialog *)> RemoveDownloadDialog::s_execHook;

RemoveDownloadDialog::RemoveDownloadDialog(const QList<RemoveItem> &items, QWidget *parent) : QDialog(parent), m_texts(removeTexts(items))
{
    setObjectName(QStringLiteral("removeDownloadDialog"));
    setWindowTitle(m_texts.title);
    setWindowIcon(appIcon());
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    const bool warn = m_texts.forGood || !m_texts.warning.isEmpty();
    QLabel *icon = iconLabel(this, warn);
    BoxBuilder b{this};
    QList<WrapLabel *> labels;
    labels << b.add(m_texts.question, false, "removeQuestion") << b.add(m_texts.list, false, "removeList")
           << b.add(m_texts.details, false, "removeDetails") << b.add(m_texts.keepLine, false, "removeKeepLine")
           << b.add(m_texts.deleteLine, true, "removeDeleteLine") << b.add(m_texts.warning, true, "removeWarning")
           << b.add(m_texts.keptNote, false, "removeKeptNote");
    m_dont = new QCheckBox(tr("Do not ask again"), this);
    m_dont->setObjectName(QStringLiteral("removeDontAsk"));
    m_dont->setToolTip(tr("Use the button you click now every time a download leaves the list, without this question. "
                          "Settings, File Transfers, \"Removing a download\" changes it back. Where a file would be deleted "
                          "for good you are still asked."));
    m_keep = new PushButton(m_texts.keepButton, this);
    m_keep->setObjectName(QStringLiteral("removeKeep"));
    m_keep->setToolTip(items.size() == 1 ? tr("The row leaves the Transfers list; the file, finished or partial, stays on the disk.")
                                         : tr("The rows leave the Transfers list; the files, finished or partial, stay on the disk."));
    m_delete = new PushButton(m_texts.deleteButton, this);
    m_delete->setObjectName(QStringLiteral("removeDelete"));
    m_delete->setToolTip(m_texts.forGood ? tr("The rows leave the list and their files are deleted; what cannot go to the %1 is deleted for good.")
                                               .arg(trash::placeName())
                                         : tr("The rows leave the list and their files are moved to the %1.").arg(trash::placeName()));
    m_delete->setEnabled(m_texts.canDelete);
    m_cancel = new PushButton(tr("Cancel"), this);
    m_cancel->setObjectName(QStringLiteral("removeCancel"));
    m_cancel->setToolTip(tr("Leave everything as it is: the downloads stay in the list, nothing is deleted."));
    // the question can delete files: Enter and Esc both mean Cancel
    for (QPushButton *p : {m_keep, m_delete, m_cancel})
        p->setAutoDefault(false);
    m_cancel->setDefault(true);
    m_cancel->setAutoDefault(true);
    connect(m_keep, &QPushButton::clicked, this, [this] {
        m_choice = Keep;
        accept();
    });
    connect(m_delete, &QPushButton::clicked, this, [this] {
        m_choice = Delete;
        accept();
    });
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        m_choice = Cancel;
        reject();
    });
    layoutBox(this, icon, labels, m_dont, {m_keep, m_delete, m_cancel});
    m_cancel->setFocus();
}

bool RemoveDownloadDialog::dontAskAgain() const
{
    return m_dont->isChecked();
}

RemoveDownloadDialog::Choice RemoveDownloadDialog::ask()
{
    if (s_execHook) {
        m_choice = Choice(s_execHook(this));
        return m_choice;
    }
    if (exec() != QDialog::Accepted)
        m_choice = Cancel;
    return m_choice;
}

// ================================================================================= shared files

std::function<int(DeleteFilesDialog *)> DeleteFilesDialog::s_execHook;

DeleteFilesDialog::DeleteFilesDialog(const DeleteTexts &t, QWidget *parent) : QDialog(parent), m_texts(t)
{
    setObjectName(QStringLiteral("deleteFilesDialog"));
    setWindowTitle(t.title);
    setWindowIcon(appIcon());
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    QLabel *icon = iconLabel(this, true);
    BoxBuilder b{this};
    QList<WrapLabel *> labels;
    labels << b.add(t.question, false, "deleteQuestion") << b.add(t.target, t.boldTarget, "deleteTarget")
           << b.add(t.details, false, "deleteDetails") << b.add(t.effect, false, "deleteEffect")
           << b.add(t.warning, true, "deleteWarning") << b.add(t.hint, false, "deleteHint");
    m_delete = new PushButton(t.button, this);
    m_delete->setObjectName(QStringLiteral("deleteConfirm"));
    m_delete->setToolTip(t.forGood ? tr("Delete it from the disk for good and stop sharing it.")
                                   : tr("Move it to the %1 and stop sharing it.").arg(trash::placeName()));
    m_cancel = new PushButton(tr("Cancel"), this);
    m_cancel->setObjectName(QStringLiteral("deleteCancel"));
    m_cancel->setToolTip(tr("Delete nothing: the files stay on the disk and shared."));
    m_delete->setAutoDefault(false);
    m_cancel->setDefault(true);
    m_cancel->setAutoDefault(true);
    connect(m_delete, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    layoutBox(this, icon, labels, nullptr, {m_delete, m_cancel});
    m_cancel->setFocus();
}

bool DeleteFilesDialog::ask()
{
    if (s_execHook)
        return s_execHook(this) == QDialog::Accepted;
    return exec() == QDialog::Accepted;
}

} // namespace mx::ui
