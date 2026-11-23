// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: Settings pages, part 2 [ui-c]: Incoming Msgs & Privacy, Library, Channels,
// Search, File Transfers (+ Queuing), Bandwidth Throttle, File Launching, Miscellaneous.
#include "ui/dialogs/settings_pages.h"
#include "ui/kit/session.h"
#include "ui/kit/sound.h"
#include "ui/dialogs/share_dialogs.h"

#include <QtCore/QDir>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTreeWidget>

#include "mx/services.h"
#include "ui/kit/file_types.h"
#include "ui/kit/format.h"
#include "ui/kit/launcher.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"
#include "ui/speed_limits.h"

namespace mx::ui {

using namespace mx::keys;

namespace {

// Settings ▸ Incoming Msgs: current alert text ("Nothing" / "System Beep" / file name)
class AlertText : public StaticText {
public:
    using StaticText::StaticText;
};

// The 7 day check boxes of the bandwidth schedule (bitmask Mon=1 ... Sun=64).
class DaysEditor : public QWidget, public BoundWidget {
    Q_OBJECT
public:
    explicit DaysEditor(QWidget *parent = nullptr) : QWidget(parent)
    {
        const char *names[] = {QT_TR_NOOP("Mon"), QT_TR_NOOP("Tue"), QT_TR_NOOP("Wed"), QT_TR_NOOP("Thu"),
                               QT_TR_NOOP("Fri"), QT_TR_NOOP("Sat"), QT_TR_NOOP("Sun")};
        for (int i = 0; i < 7; ++i) {
            auto *c = new QCheckBox(tr(names[i]), this);
            c->setObjectName(QStringLiteral("day%1").arg(i));
            connect(c, &QCheckBox::toggled, this, [this] { emit valueEdited(boundValue()); });
            m_days << c;
        }
    }
    // [qa-2] each box as wide as its text needs, the spare width shared between them: equal 41-px
    // slots cut "Wed" at 100 % and let the boxes overlap their neighbours' text at 175 %
    void resizeEvent(QResizeEvent *) override
    {
        int need[7], total = 0;
        for (int i = 0; i < 7; ++i) {
            QCheckBox *c = m_days[i];
            need[i] = style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, c) +
                      style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing, nullptr, c) +
                      c->fontMetrics().horizontalAdvance(c->text()) + S(2);
            total += need[i];
        }
        const int spare = width() - total;
        if (spare < 0) {   // too narrow: equal slots as before
            for (int i = 0; i < 7; ++i)
                m_days[i]->setGeometry(i * width() / 7, 0, (i + 1) * width() / 7 - i * width() / 7, height());
            return;
        }
        int x = 0;
        for (int i = 0; i < 7; ++i) {
            const int gap = (i + 1) * spare / 7 - i * spare / 7;
            m_days[i]->setGeometry(x, 0, need[i] + gap, height());
            x += need[i] + gap;
        }
    }
    QVariant boundValue() const override
    {
        int m = 0;
        for (int i = 0; i < 7; ++i)
            if (m_days[i]->isChecked())
                m |= 1 << i;
        return m;
    }
    void setBoundValue(const QVariant &v) override
    {
        for (int i = 0; i < 7; ++i) {
            QSignalBlocker b(m_days[i]);
            m_days[i]->setChecked(v.toInt() & (1 << i));
        }
    }
    void setValueByUser(const QVariant &v) override
    {
        setBoundValue(v);
        emit valueEdited(boundValue());
    }
signals:
    void valueEdited(const QVariant &v);

private:
    QList<QCheckBox *> m_days;
};

// Settings ▸ File Launching: the menu entries (Map {"entries": [{title, ext, action, command, params, workdir}]}).
class LaunchEditor : public QWidget, public BoundWidget {
    Q_OBJECT
public:
    explicit LaunchEditor(QWidget *parent = nullptr) : QWidget(parent)
    {
        m_title = new QComboBox(this);
        m_title->setObjectName(QStringLiteral("launchTitle"));
        m_title->setEditable(true);
        m_save = new PushButton(tr("Save"), this);
        m_save->setObjectName(QStringLiteral("launchSave"));
        m_delete = new PushButton(tr("Delete"), this);
        m_delete->setObjectName(QStringLiteral("launchDelete"));
        m_save->setToolTip(tr("Store the entry with the title typed on the left and the fields below. A new title adds a new entry."));
        m_title->setToolTip(tr("The entry's title as the Transfers menu shows it. Pick an entry to edit it, or type a new title and press Save."));
        m_delete->setToolTip(tr("Remove the entry with the title shown on the left from the list."));
        const char *fields[] = {"ext", "action", "command", "params", "workdir"};
        const char *tips[] = {
            QT_TR_NOOP("The extensions the entry is offered for, separated by spaces (mp3 ogg), or * for every file."),
            QT_TR_NOOP("Without a command: the Windows verb to use (play, enqueue, edit ...). Entries without a command are offered on Windows only."),
            QT_TR_NOOP("The program to start, for example the full path of a player. Empty: the verb on the left (Windows)."),
            QT_TR_NOOP("The program's arguments; %path% is replaced with the file. Empty: just the file."),
            QT_TR_NOOP("The folder the program starts in. Empty: OctoMX's own.")};
        for (int i = 0; i < 5; ++i) {
            auto *le = new QLineEdit(this);
            le->setObjectName(QStringLiteral("launch_") + QString::fromLatin1(fields[i]));
            le->setToolTip(tr(tips[i]));
            m_fields << le;
        }
        connect(m_title, &QComboBox::activated, this, [this](int i) { showEntry(i); });
        connect(m_save, &QPushButton::clicked, this, [this] { save(); });
        connect(m_delete, &QPushButton::clicked, this, [this] {
            const QString t = m_title->currentText().trimmed();
            for (int i = 0; i < m_entries.size(); ++i)
                if (m_entries[i].toMap().value(QStringLiteral("title")).toString() == t) {
                    m_entries.removeAt(i);
                    refill(qMin(i, int(m_entries.size()) - 1));
                    emit valueEdited(boundValue());
                    return;
                }
        });
    }
    // geometry in the editor's own coordinates
    void layoutAt(int x0, int y0)
    {
        const QPoint o(-S(x0), -S(y0));
        m_title->setGeometry(SR(225, 76, 165, 21).translated(o));
        m_save->setGeometry(SR(395, 76, 62, 21).translated(o));
        m_delete->setGeometry(SR(461, 76, 55, 21).translated(o));
        const int ys[] = {130, 162, 193, 224, 254};
        for (int i = 0; i < 5; ++i)
            m_fields[i]->setGeometry(SR(282, ys[i], 236, 21).translated(o));
    }
    QVariant boundValue() const override { return QVariantMap{{QStringLiteral("entries"), m_entries}}; }
    void setBoundValue(const QVariant &v) override
    {
        const QVariantList l = v.toMap().value(QStringLiteral("entries")).toList();
        if (l == m_entries)
            return;
        m_entries = l;
        refill(qMax(0, m_title->currentIndex()));
    }
    void setValueByUser(const QVariant &v) override
    {
        setBoundValue(v);
        emit valueEdited(boundValue());
    }
signals:
    void valueEdited(const QVariant &v);

private:
    void refill(int current)
    {
        QSignalBlocker b(m_title);
        m_title->clear();
        for (const QVariant &e : std::as_const(m_entries))
            m_title->addItem(e.toMap().value(QStringLiteral("title")).toString());
        m_title->setCurrentIndex(current < m_title->count() ? current : -1);
        showEntry(m_title->currentIndex());
    }
    void showEntry(int i)
    {
        const QVariantMap e = i >= 0 && i < m_entries.size() ? m_entries.at(i).toMap() : QVariantMap();
        const char *fields[] = {"ext", "action", "command", "params", "workdir"};
        for (int k = 0; k < 5; ++k)
            m_fields[k]->setText(e.value(QString::fromLatin1(fields[k])).toString());
    }
    void save()
    {
        const QString t = m_title->currentText().trimmed();
        if (t.isEmpty())
            return;
        QVariantMap e{{QStringLiteral("title"), t}};
        const char *fields[] = {"ext", "action", "command", "params", "workdir"};
        for (int k = 0; k < 5; ++k)
            e.insert(QString::fromLatin1(fields[k]), m_fields[k]->text().trimmed());
        int at = -1;
        for (int i = 0; i < m_entries.size(); ++i)
            if (m_entries[i].toMap().value(QStringLiteral("title")).toString() == t)
                at = i;
        if (at >= 0)
            m_entries[at] = e;
        else
            m_entries << e;
        refill(at >= 0 ? at : int(m_entries.size()) - 1);
        emit valueEdited(boundValue());
    }
    QVariantList m_entries;
    QComboBox *m_title;
    PushButton *m_save, *m_delete;
    QList<QLineEdit *> m_fields;
};

// Shared folders (LibraryService, not a setting): list + Add / Remove.
class SharedFoldersBox : public QWidget {
public:
    SharedFoldersBox(mx::Core *core, QWidget *parent) : QWidget(parent), m_core(core)
    {
        m_list = new QTreeWidget(this);
        m_list->setObjectName(QStringLiteral("sharedFoldersList"));
        m_list->setHeaderLabels({tr("Shared Folders")});
        m_list->setRootIsDecorated(false);
        m_list->header()->setSectionsClickable(false);
        m_add = new PushButton(tr("Share Folder..."), this);
        m_add->setObjectName(QStringLiteral("sharedFoldersAdd"));
        m_remove = new PushButton(tr("Unshare Folder"), this);
        m_remove->setObjectName(QStringLiteral("sharedFoldersRemove"));
        m_add->setToolTip(tr("Pick a folder whose files other users may download. Its sub folders are shared too."));
        m_remove->setToolTip(tr("Stop sharing the folder selected in the list. The files stay on your disk."));
        connect(m_add, &QPushButton::clicked, this, [this] {
            const QString p = PathField::browse(QDir::homePath(), true, window());
            if (!p.isEmpty())
                m_core->library()->addFolder(p, true);
        });
        connect(m_remove, &QPushButton::clicked, this, [this] {
            if (QTreeWidgetItem *it = m_list->currentItem())
                m_core->library()->removeFolder(it->data(0, Qt::UserRole).toString());
        });
        connect(core->library(), &LibraryService::foldersChanged, this, [this] { refill(); });
        refill();
    }
    void resizeEvent(QResizeEvent *) override
    {
        m_list->setGeometry(0, 0, width(), height() - S(28));
        m_add->setGeometry(0, height() - S(23), S(100), S(23));
        m_remove->setGeometry(S(106), height() - S(23), S(100), S(23));
    }

private:
    void refill()
    {
        m_list->clear();
        for (const SharedFolder &f : m_core->library()->folders()) {
            auto *it = new QTreeWidgetItem({QDir::toNativeSeparators(f.path)});
            it->setData(0, Qt::UserRole, f.path);
            m_list->addTopLevelItem(it);
        }
    }
    mx::Core *m_core;
    QTreeWidget *m_list;
    PushButton *m_add, *m_remove;
};

void resetButton(SettingsDialog *d, PageKit &k, int x, int y, int w, const char *name)
{
    PushButton *reset = k.button(QObject::tr("Reset Defaults"), x, y, w, 19);
    reset->setObjectName(QString::fromLatin1(name));
    reset->setToolTip(QObject::tr("Put every setting on this page back to its default value."));
    QWidget *page = k.page;
    QObject::connect(reset, &QPushButton::clicked, d, [d, page] { d->binder()->resetKeys(d->binder()->keysOf(page)); });
}

} // namespace

// ======================================================================= Incoming Msgs & Privacy

void buildMessagePages(SettingsDialog *d)
{
    // [obs: settings_incoming_msgs_privacy.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("messages"), QObject::tr("Incoming Msgs & Privacy")));
        k.group(QObject::tr("Incoming Message Privacy"), 218, 28, 303, 146);
        k.radio(QObject::tr("Allow incoming messages from any source"), messages::AllowFrom, QStringLiteral("any"), 242, 53, 260);
        k.radio(QObject::tr("Only allow messages from the following sources :"), messages::AllowFrom, QStringLiteral("restricted"), 242, 74, 270);
        QList<QWidget *> checks;
        checks << k.check(QObject::tr("Users on your hotlist (both perm and temp)"), messages::AllowFromHotlist, 258, 92, 250);
        checks << k.check(QObject::tr("Users you are downloading from"), messages::AllowFromDownloadingFrom, 258, 108, 250);
        checks << k.check(QObject::tr("Users you are uploading to"), messages::AllowFromUploadingTo, 258, 124, 250);
        checks << k.check(QObject::tr("Users in the same chat room"), messages::AllowFromSameRoom, 258, 140, 250);
        k.b->bindCustom({}, {QString(messages::AllowFrom)}, [checks, s = k.s] {
            for (QWidget *w : checks)
                w->setEnabled(s->s(messages::AllowFrom) == QLatin1String("restricted"));
        });
        k.group(QObject::tr("Incoming Message Alert"), 218, 202, 303, 101);
        auto *current = k.place(new AlertText, 234, 223, 270, 13);
        current->setObjectName(QStringLiteral("set_messages_alertText"));
        current->setToolTip(QObject::tr("The alert a new private message plays now. Default: Nothing (Quiet)."));
        k.b->bindCustom({current}, {QString(messages::AlertMode), QString(messages::AlertWav)}, [current, s = k.s] {
            const QString m = s->s(messages::AlertMode);
            if (m == QLatin1String("beep"))
                current->setText(QObject::tr("System Beep"));
            else if (m == QLatin1String("sound"))
                current->setText(QObject::tr("Built-in Sound"));
            else if (m == QLatin1String("wav") && !s->s(messages::AlertWav).isEmpty())
                current->setText(QDir::toNativeSeparators(s->s(messages::AlertWav)));
            else
                current->setText(QObject::tr("Nothing"));
        });
        PushButton *quiet = k.button(QObject::tr("Quiet"), 227, 269, 51, 22);
        quiet->setObjectName(QStringLiteral("set_messages_alertMode_quiet"));
        k.b->bindValueButton(quiet, messages::AlertMode, QStringLiteral("quiet"));
        quiet->setToolTip(QObject::tr("Play no sound when a private message arrives (the default)."));
        PushButton *beep = k.button(QObject::tr("Use System Beep"), 284, 269, 104, 22);
        beep->setObjectName(QStringLiteral("set_messages_alertMode_beep"));
        k.b->bindValueButton(beep, messages::AlertMode, QStringLiteral("beep"));
        beep->setToolTip(QObject::tr("Sound the system beep when a private message arrives. On Linux this is the "
                                     "desktop's bell sound, which some desktops leave silent."));
        // [ui-h] a short chime built into OctoMX, the same on every computer, and a test button
        PushButton *chime = k.button(QObject::tr("Built-in Sound"), 227, 241, 104, 22);
        chime->setObjectName(QStringLiteral("set_messages_alertMode_sound"));
        k.b->bindValueButton(chime, messages::AlertMode, QStringLiteral("sound"));
        chime->setToolTip(QObject::tr("Play a short chime that comes with OctoMX when a private message arrives."));
        PushButton *test = k.button(QObject::tr("Test"), 337, 241, 51, 22);
        test->setObjectName(QStringLiteral("set_messages_alertTest"));
        test->setToolTip(QObject::tr("Play the alert chosen here once, so you hear what a new message sounds like."));
        QObject::connect(test, &QPushButton::clicked, k.page, [s = k.s] {
            const QString m = s->s(messages::AlertMode);
            const QString wav = s->s(messages::AlertWav);
            sound::play(m == QLatin1String("beep") ? QStringLiteral("beep")
                        : m == QLatin1String("wav") && !wav.isEmpty() ? wav
                        : m == QLatin1String("sound") ? QStringLiteral("sound") : QString());
        });
        if (!sound::canPlayFiles())
            k.wrap(QObject::tr("No sound player was found: .wav files and the chime sound as the beep."), 394, 238, 115, 28);
        auto *wav = k.place(new PathField(QObject::tr("Browse for .wav file..."), false), 394, 269, 111, 22);
        wav->setObjectName(PageKit::nameFor(messages::AlertWav));
        wav->setToolTip(QObject::tr("Pick a .wav sound file to play when a private message arrives."));
        wav->setButtonGeometry(QRect(0, 0, 0, 0), QRect(0, 0, 111, 22));
        wav->setFilter(QObject::tr("Sounds (*.wav)"));
        wav->button()->setToolTip(QObject::tr("Pick a .wav sound file to play when a private message arrives. "
                                              "Windows plays it itself, Linux with the desktop's sound player."));
        k.b->bind(wav, messages::AlertWav);
        QObject::connect(k.b, &SettingsBinder::committed, wav, [b = k.b](const QString &key, const QVariant &v) {
            if (key == QLatin1String(messages::AlertWav) && !v.toString().isEmpty())
                b->commit(QString(messages::AlertMode), QStringLiteral("wav"));
        });
    }
    // ---- NG: private messages + hotlist
    {
        PageKit k(d, d->addPage(QStringLiteral("messages/private"), QObject::tr("Private Messages"), QStringLiteral("messages")));
        Form f(k);
        f.group(QObject::tr("Privacy"));
        f.combo(messages::AllowPrivateMessages, QObject::tr("Accept messages from"), 120);
        f.combo(messages::AllowBrowse, QObject::tr("Allow browsing my files"), 120);
        f.check(messages::AllowWhois);
        f.check(messages::IgnoreHotlistOnly);
        f.check(messages::BlockFloods, QObject::tr("Block message floods"));
        f.spin(messages::FloodLimit, QObject::tr("Mute a sender after"), QObject::tr("messages in 10 seconds"));
        f.edit(messages::FilterWords, QObject::tr("Filter messages containing (separated by ;)"));
        f.endGroup();
        f.group(QObject::tr("New Messages"));
        f.check(messages::OpenWindowOnMessage);
        f.check(messages::FlashOnMessage);
        f.check(messages::SoundOnMessage);
        f.endGroup();
    }
    {
        PageKit k(d, d->addPage(QStringLiteral("messages/autoreply"), QObject::tr("Auto Reply and Hotlist"), QStringLiteral("messages")));
        Form f(k);
        f.group(QObject::tr("Auto Responder"));
        f.check(messages::AutoReply, QObject::tr("Send an automatic reply to new messages"));
        f.edit(messages::AutoReplyText, QObject::tr("Reply text"));
        f.endGroup();
        f.group(QObject::tr("Hotlist"));
        f.check(hotlist::NotifyOnline);
        f.check(hotlist::SoundOnline, QObject::tr("Play the message alert when a Hotlist user comes online"));   // [ui-h]
        f.check(hotlist::AutoBrowse);
        f.endGroup();
    }
}

// ======================================================================================= Library

void buildLibraryPages(SettingsDialog *d)
{
    // [obs: settings_library.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("library"), QObject::tr("Library")));
        // [ui-h] the "Available Filetypes" list was an editor whose entries nothing read: the file types
        // come from the one type table (mx::extensionsForMediaType). The page shows that table and
        // opens the Shared File Types dialog, where the types to share are chosen (UI_SPEC 8.11)
        k.group(QObject::tr("Available Filetypes"), 228, 71, 286, 245);
        // one line per group, the extensions cut with "..." to the width; the tool tip has them all
        QString table, full;
        const QFontMetrics fm(normalFont());
        for (const QString &row : settingDefault(library::FileTypes).toStringList()) {
            table += fmt::elide(row, fm, S(258)) + QLatin1Char('\n');
            full += row + QLatin1Char('\n');
        }
        WrapLabel *types = k.wrap(table.trimmed(), 242, 92, 266, 9 * 13 + 4);
        types->setObjectName(QStringLiteral("libraryTypeTable"));
        types->setToolTip(full.trimmed());
        k.wrap(QObject::tr("These are the file types OctoMX knows. Which of them you share is chosen in Shared File Types."),
               242, 220, 262, 26);
        PushButton *shared = k.button(QObject::tr("Shared File Types..."), 242, 270, 140, 23);
        shared->setObjectName(QStringLiteral("libraryShareTypes"));
        shared->setToolTip(QObject::tr("Choose which of these file types other users may download from your shared folders."));
        QObject::connect(shared, &QPushButton::clicked, d, [d] {
            auto *dlg = new SharedFileTypesDialog(d->core()->library(), d->core()->settings(), d);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->open();
        });
    }
    // ---- NG: shared folders and sharing options
    {
        PageKit k(d, d->addPage(QStringLiteral("library/sharing"), QObject::tr("Sharing"), QStringLiteral("library")));
        auto *box = k.place(new SharedFoldersBox(d->core(), nullptr), 215, 9, 305, 112);
        Q_UNUSED(box)
        Form f(k, 215, 128);
        f.check(transfers::ShareCompleted);
        f.check(library::ShareIncomplete);
        f.check(library::WatchFolders);
        f.check(library::ShareHidden);
        f.check(library::ShareUnlistedTypes, QObject::tr("Share files of types not listed in Shared File Types"));
        f.spin(library::RescanIntervalMin, QObject::tr("Rescan shared folders every"), QObject::tr("minutes (0 = never)"));
        f.spin(library::HashThreads, QObject::tr("Hashing threads"));
        f.edit(library::ExcludePatterns, QObject::tr("Never share files matching (separated by ;)"));
    }
}

// ====================================================================================== Channels

void buildChatPages(SettingsDialog *d)
{
    // [obs: settings_channels.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("channels"), QObject::tr("Channels")));
        k.check(QObject::tr("Fetch channels immediately when list opened"), chat::FetchListImmediately, 224, 41, 280);
        k.check(QObject::tr("Include topic in channel filter"), chat::TopicInFilter, 224, 75, 280);
        k.check(QObject::tr("Allow channel output backscroll over 1000 lines."), chat::AllowLargeBackscroll, 224, 110, 280);
        k.wrap(QObject::tr("NOT RECOMMENDED: a room window with a very long backscroll gets slow to redraw "
                           "when its size changes."),
               245, 127, 262, 53);
    }
    // ---- NG: chat options
    {
        PageKit k(d, d->addPage(QStringLiteral("channels/options"), QObject::tr("Chat Options"), QStringLiteral("channels")));
        Form f(k);
        f.group(QObject::tr("Channel Windows"));
        f.check(chat::ShowJoinsParts);
        f.check(chat::ShowTimestamps);
        f.editRow(chat::TimestampFormat, QObject::tr("Timestamp format"), 100, 80);
        f.spin(chat::BacklogLines, QObject::tr("Lines kept per room"), QString(), 120);
        f.editRow(chat::FontFamily, QObject::tr("Chat font"), 100, 120);
        f.spin(chat::FontSize, QObject::tr("Chat font size"), QString(), 120);
        f.endGroup();
        f.group(QObject::tr("Joining"));
        f.check(chat::AutoJoinFavourites);
        f.editRow(chat::DefaultRoom, QObject::tr("Default room"), 100);
        f.endGroup();
    }
}

// ======================================================================================== Search

namespace {

MenuButton *fileTypeButton(QWidget *parent, Settings *st)
{
    // [ui-f] the Search page menu (codes, labels from the type table); custom extensions as the
    // last ones typed there and as stored in the presets
    auto *m = new MenuButton(parent);
    QStringList custom;
    const QStringList last = parseExtensionList(st->s(search::CustomExtensions));
    if (!last.isEmpty())
        custom << QStringLiteral("custom:") + last.join(QLatin1Char(','));
    for (const char *k : {"search/defaultType", "search/typeButtonAudio", "search/typeButtonVideo", "search/typeButtonPictures"}) {
        const FileTypeFilter f = FileTypeFilter::fromCode(st->s(QString::fromLatin1(k)));
        if (f.kind == QLatin1String("custom") && !custom.contains(f.code()))
            custom << f.code();
    }
    filetypes::populateMenu(m, false, custom);
    return m;
}

} // namespace

void buildSearchPages(SettingsDialog *d)
{
    // [obs: settings_search.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("search"), QObject::tr("Search")));
        k.group(QObject::tr("Default Search Parameters"), 226, 12, 288, 74);
        MenuButton *type = k.place(fileTypeButton(nullptr, k.s), 236, 30, 248, 22);
        type->setObjectName(PageKit::nameFor(search::DefaultType));
        type->setToolTip(QObject::tr("The file type a new search starts with. The Search page remembers the last type you used here too."));
        k.b->bind(type, search::DefaultType);
        auto *conn = k.place(new MenuButton, 236, 56, 248, 22);
        conn->setObjectName(PageKit::nameFor(search::DefaultConnection));
        conn->setToolTip(QObject::tr("Show only results from users whose connection is at least this fast. Any Connection shows every result."));
        conn->addChoice(QObject::tr("Any Connection"), QObject::tr("Any Connection"));
        conn->addSeparator();
        for (int r = kConnectionSpeedRankMax; r >= 1; --r) {   // [qa-1] same list as the Search page
            const QString t = connectionTextForRank(r);
            conn->addChoice(t, t);
        }
        k.b->bind(conn, search::DefaultConnection);

        k.group(QObject::tr("Default Download Parameters"), 226, 90, 288, 76);
        auto *afs = k.place(new MenuButton, 236, 112, 248, 21);
        afs->setObjectName(PageKit::nameFor(search::AutoFindSourcesMin));
        afs->setToolTip(QObject::tr("For new downloads, search again for more sources of the file at this interval while it is not complete."));
        afs->addChoice(QObject::tr("Never"), 0, QObject::tr("Auto Find Sources - Never"));
        for (int m : {10, 15, 20, 25, 30, 40, 50, 60})
            afs->addChoice(QObject::tr("Every %1 min").arg(m), m, QObject::tr("Auto Find Sources - %1m").arg(m));
        k.b->bind(afs, search::AutoFindSourcesMin);
        auto *aeq = k.place(new MenuButton, 236, 138, 248, 21);
        aeq->setObjectName(PageKit::nameFor(search::AutoEnterQueue));
        aeq->setToolTip(QObject::tr("When a source puts a download in its waiting queue, wait only if your place is this number or better; "
                                    "a longer queue is left while other sources work. Never leaves every queue."));
        aeq->addChoice(QObject::tr("Never"), 0, QObject::tr("Auto Enter Queue - Never"));
        for (int n : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 30, 35, 40, 45, 50, 60, 70, 80, 90, 100})
            aeq->addChoice(QString::number(n), n, QObject::tr("Auto Enter Queue - %1").arg(n));
        k.b->bind(aeq, search::AutoEnterQueue);

        k.group(QObject::tr("File Type Buttons"), 226, 171, 288, 98);
        const struct { const char *labelKey; const char *def; const char *typeKey; } rows[] = {
            {"search/typeButtonAudioLabel", QT_TR_NOOP("Audio"), "search/typeButtonAudio"},
            {"search/typeButtonVideoLabel", QT_TR_NOOP("Video"), "search/typeButtonVideo"},
            {"search/typeButtonPicturesLabel", QT_TR_NOOP("Pictures"), "search/typeButtonPictures"},
        };
        int y = 190;
        for (const auto &r : rows) {
            const QString lk = QString::fromLatin1(r.labelKey);
            if (!k.s->value(lk).isValid())
                k.s->setValue(lk, QObject::tr(r.def));   // UI-side key (see status: settings keys wanted)
            k.edit(lk, 238, y + 1, 67, 19);
            MenuButton *mb = k.place(fileTypeButton(nullptr, k.s), 313, y, 171, 20);
            mb->setObjectName(PageKit::nameFor(QString::fromLatin1(r.typeKey)));
            mb->setToolTip(QObject::tr("The file type this quick button on the Search page selects. The box on the left is its caption."));
            k.b->bind(mb, QString::fromLatin1(r.typeKey));
            y += 26;
        }
        k.group(QObject::tr("Results Display"), 226, 275, 288, 55);
        k.radio(QObject::tr("Groups of duplicate results collapsed by default"), search::GroupsExpanded, false, 239, 291, 270);
        k.radio(QObject::tr("Groups of duplicate results expanded by default"), search::GroupsExpanded, true, 239, 308, 270);
    }
    // ---- NG: search options
    {
        PageKit k(d, d->addPage(QStringLiteral("search/options"), QObject::tr("Search Options"), QStringLiteral("search")));
        Form f(k);
        f.group(QObject::tr("Results"));
        f.spin(search::MaxResults, QObject::tr("Maximum results per search"), QString(), 160);
        f.spin(search::TimeoutSec, QObject::tr("A search finishes after"), QObject::tr("seconds"), 160);
        f.check(search::ShowFullPaths);
        f.check(search::FakeFilter);
        f.check(search::FlagSuspects, QObject::tr("Mark suspected fakes"));
        f.check(search::HideSuspects, QObject::tr("Hide suspected fakes"));
        f.check(search::DropTorrentBridges, QObject::tr("Drop eD2k torrent/Kad bridge entries"));
        f.check(search::AdultFilter);
        f.endGroup();
        f.group(QObject::tr("History"));
        f.check(search::AutoComplete);
        f.check(search::SaveHistory);
        f.spin(search::HistorySize, QObject::tr("Searches remembered"), QString(), 160);
        f.endGroup();
    }
    {
        PageKit k(d, d->addPage(QStringLiteral("search/networks"), QObject::tr("Search Networks"), QStringLiteral("search")));
        Form f(k);
        f.edit(search::Networks, QObject::tr("Networks to search (empty = all enabled; separated by ;)"));
        f.text(QObject::tr("Names: Overlay, BitTorrent, eD2k, Kad, Gnutella, HTTP, Web, Rats."), 2);
        f.edit(search::BlockedExtensions, QObject::tr("Hide results with these extensions (separated by ;)"));
        f.editRow(search::MinCandidateSize, QObject::tr("Match by name and size above (bytes)"), 190);
        f.text(QObject::tr("Files of the same name and size above this limit are grouped as one file and verified by "
                           "hashing when downloaded."),
               3);
    }
}

// ================================================================================ File Transfers

namespace {

// "Limit incoming bandwidth [n] KB per second": check = limit on (value != 0), spin = last value
void limitRow(PageKit &k, const QString &text, const QString &limitKey, const QString &lastKey, int y)
{
    auto *c = k.place(new QCheckBox(text), 217, y + 2, 140, 16);
    c->setObjectName(PageKit::nameFor(limitKey));
    // [bw-1] the same limits as the Bandwidth page row and the tray menu: "2.5 MB" input accepted,
    // upload never below bwlimit::kMinUploadKBs
    auto *sb = k.place(new RateSpinBox, 358, y, 70, 20);
    sb->setObjectName(PageKit::nameFor(lastKey));
    k.b->bind(sb, lastKey, SettingType::Int);
    const bool down = limitKey == QLatin1String(bandwidth::DownLimitKBs);
    if (!down)
        sb->setMinimum(bwlimit::kMinUploadKBs);
    StaticText *unit = k.label(QObject::tr("KB per second"), 434, y + 4, 86);
    for (QWidget *w : {static_cast<QWidget *>(c), static_cast<QWidget *>(sb), static_cast<QWidget *>(unit)})
        if (w)
            w->setToolTip(speed::toolTip(down));
    bwlimit::installOverrideRule(k.s);
    k.b->bindCustom({c}, {limitKey}, [c, k = limitKey, s = k.s] {
        QSignalBlocker bl(c);
        c->setChecked(s->i(k) > 0);
    });
    QObject::connect(c, &QCheckBox::toggled, k.b, [c, sb, limitKey, b = k.b](bool on) {
        b->commit(limitKey, on ? sb->value() : 0, c);
    });
    QObject::connect(k.b, &SettingsBinder::committed, c, [c, limitKey, lastKey, b = k.b](const QString &key, const QVariant &v) {
        if (key == lastKey && c->isChecked())
            b->commit(limitKey, v, c);
    });
}

} // namespace

void buildTransferPages(SettingsDialog *d)
{
    // [obs: settings_file_transfers.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("transfers"), QObject::tr("File Transfers")));
        k.group(QObject::tr("Incoming Files Folder"), 214, 9, 306, 47);
        auto *dir = k.place(new PathField(QObject::tr("Change..."), true), 222, 22, 289, 22);
        dir->setObjectName(PageKit::nameFor(transfers::IncomingDir));
        dir->setToolTip(QObject::tr("Where finished downloads are saved (and shared, with \"Share completed downloads\"). Change... picks another folder."));
        dir->setButtonGeometry(QRect(0, 5, 230, 13), QRect(235, 0, 54, 22));
        k.b->bind(dir, transfers::IncomingDir);
        // [ui-h] six of the nine WinMX 3.53 rows are gone (purge inactive sources, clear incomplete downloads /
        // uploads, the running / pending upload and running download time outs): the download engine
        // has its own source and stall handling and read none of them (UI_SPEC 8.11)
        const struct { const char *check; const char *text; const char *spin; const char *tip; } rows[] = {
            {"transfers/autoClearCompleted", QT_TR_NOOP("Clear complete downloads after"), "transfers/clearCompleteDownloadsSec",
             QT_TR_NOOP("Ticked: a finished download leaves the Transfers list this many seconds after it completed. "
                        "The file stays on disk. Off by default.")},
            {"transfers/autoClearUploads", QT_TR_NOOP("Clear complete uploads after"), "transfers/clearCompleteUploadsSec",
             QT_TR_NOOP("Ticked: a finished upload leaves the upload list this many seconds after it ended. Off by default.")},
            {nullptr, QT_TR_NOOP("Pending downloads timeout after"), "transfers/pendingDownloadTimeoutSec",
             QT_TR_NOOP("A download that finds no source that answers within this time is marked as timed out and "
                        "tried again later (default 300 seconds).")},
        };
        const int ys[] = {61, 82, 103};
        for (int i = 0; i < 3; ++i) {
            const auto &r = rows[i];
            if (r.check) {
                QCheckBox *c = k.check(QString(), QString::fromLatin1(r.check), 215, ys[i] + 2, 14);
                c->setToolTip(QObject::tr(r.tip));
            }
            k.label(QObject::tr(r.text), 229, ys[i] + 4, 164, Qt::AlignRight);
            k.spin(QString::fromLatin1(r.spin), 397, ys[i], 59, 20)->setToolTip(QObject::tr(r.tip));
            k.label(QObject::tr("seconds"), 462, ys[i] + 4, 55);
        }
        k.check(QObject::tr("Switch to Transfers window when starting downloads"), transfers::SwitchToTransfers, 215, 136, 300);
        k.check(QObject::tr("New downloads expanded by default"), transfers::NewDownloadsExpanded, 215, 153, 300);
        // "this session only": an in-memory switch (ui::Session), never written to the settings
        {
            auto *c = k.place(new QCheckBox(QObject::tr("Suppress dangerous filetype warning (this session only)")), 215, 170, 300, 16);
            c->setObjectName(QStringLiteral("sessionSuppressDangerWarning"));
            c->setToolTip(QObject::tr("Do not ask before opening programs, scripts and archives until OctoMX is closed. Off at every start."));
            c->setChecked(Session::instance()->suppressDangerousWarning());
            QObject::connect(c, &QCheckBox::toggled, Session::instance(), &Session::setSuppressDangerousWarning);
            QObject::connect(Session::instance(), &Session::changed, c,
                             [c] { c->setChecked(Session::instance()->suppressDangerousWarning()); });
        }
        k.check(QObject::tr("Suppress dangerous filetype warning (permanent)"), transfers::SuppressDangerousWarning, 215, 186, 300);
        // [ui-j] what happens to the file when a download leaves the list (the Remove question's "Do not ask again")
        k.label(QObject::tr("Removing a download:"), 215, 214, 128);
        k.combo(QString(transfers::RemoveAction), 345, 210, 175);
        resetButton(d, k, 215, 316, 305, "transfersResetDefaults");
    }
    // ---- Queuing [obs: settings_queuing.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("transfers/queuing"), QObject::tr("Queuing"), QStringLiteral("transfers")));
        const struct { const char *t; const char *key; int y; } rows[] = {
            {QT_TR_NOOP("Max total downloads"), "transfers/maxDownloads", 62},
            {QT_TR_NOOP("Max total uploads"), "transfers/maxUploads", 152},
            {QT_TR_NOOP("Max uploads per user"), "transfers/maxUploadsPerUser", 184},
        };
        for (const auto &r : rows) {
            k.label(QObject::tr(r.t), 230, r.y + 5, 156, Qt::AlignRight);
            k.spin(QString::fromLatin1(r.key), 388, r.y, 59, 22);
        }
        QCheckBox *c = k.check(QObject::tr("When there are"), queue::ChangeSpeedWhenBusy, 227, 239, 96);
        Q_UNUSED(c)
        k.spin(queue::ChangeSpeedThreshold, 325, 236, 59, 22);
        k.label(QObject::tr("or more uploads, change"), 388, 241, 130);
        k.label(QObject::tr("reported connection speed to"), 244, 267, 190, Qt::AlignRight);
        QComboBox *sp = k.combo(QString(), 436, 262, 74, 21);
        for (const QString &t : connectionTypes())   // [qa-1] short labels (owner request)
            sp->addItem(t, t);
        sp->setObjectName(PageKit::nameFor(queue::ChangedSpeed));
        sp->setToolTip(settingTip(queue::ChangedSpeed));
        k.b->bind(sp, queue::ChangedSpeed);
    }
    // ---- NG: downloads
    {
        PageKit k(d, d->addPage(QStringLiteral("transfers/downloads"), QObject::tr("Downloads"), QStringLiteral("transfers")));
        k.group(QObject::tr("Incomplete Files Folder"), 214, 9, 306, 47);
        auto *dir = k.place(new PathField(QObject::tr("Change..."), true), 222, 22, 289, 22);
        dir->setObjectName(PageKit::nameFor(transfers::IncompleteDir));
        dir->setToolTip(QObject::tr("Where downloads are kept while they are not complete. Change... picks another folder."));
        dir->setButtonGeometry(QRect(0, 5, 230, 13), QRect(235, 0, 54, 22));
        k.b->bind(dir, transfers::IncompleteDir);
        Form f(k, 215, 62, 305);
        f.check(transfers::AutoRetry);
        f.spin(transfers::RetryIntervalSec, QObject::tr("Retry every"), QObject::tr("seconds"), 150);
        f.spin(transfers::RetryMaxAttempts, QObject::tr("Retry attempts"), QObject::tr("(0 = forever)"), 150);
        f.spin(transfers::SourceSearchIntervalMin, QObject::tr("Look for sources every"), QObject::tr("minutes"), 150);
        f.spin(transfers::MaxSourcesPerDownload, QObject::tr("Max sources per download"), QString(), 150);
        f.spin(transfers::MetadataTimeoutSec, QObject::tr("Torrent metadata timeout"), QObject::tr("seconds"), 150);
        f.spin(transfers::MinFreeSpaceMB, QObject::tr("Pause when free space below"), QObject::tr("MB"), 150);
        const int y = f.y;
        f.w = 160;
        f.check(transfers::VerifyCompleted, QObject::tr("Verify completed files"));
        f.check(transfers::Preallocate);
        f.check(transfers::StartPaused);
        f.y = y + 3 * 17;   // [ui-h] below the left column ([ui-j] "Confirm cancel" on the right went: the Remove question)
        // [dl-1] defaults of the two order flags for new downloads (qBittorrent: both off)
        f.x0 = 215;
        f.w = 305;
        QCheckBox *seq = f.check(transfers::SequentialDefault, QObject::tr("Download new files in sequential order"));
        seq->setToolTip(QObject::tr("New downloads fetch their file from the beginning to the end, as far as the sources "
                                    "allow. Each download can be switched in its menu on the Transfers page."));
        QCheckBox *fl = f.check(transfers::FirstLastDefault, QObject::tr("Download first and last pieces of new files first"));
        fl->setToolTip(QObject::tr("New downloads fetch the beginning and the end of the file first, so audio and video can "
                                   "be previewed early. Each download can be switched in its menu on the Transfers page."));
    }
    // ---- NG: upload queue
    {
        PageKit k(d, d->addPage(QStringLiteral("transfers/uploads"), QObject::tr("Upload Queue"), QStringLiteral("transfers")));
        Form f(k);
        f.group(QObject::tr("Remote Queue"));
        f.check(queue::Enabled);
        f.spin(queue::MaxLength, QObject::tr("Maximum queue length"), QString(), 160);
        f.spin(queue::MaxPerUser, QObject::tr("Maximum queued files per user"), QString(), 160);
        f.spin(queue::TimeoutMin, QObject::tr("Drop inactive queued users after"), QObject::tr("min"), 164);   // [qa-2] 160 cut the label
        f.spin(queue::SmallFileBypassKB, QObject::tr("Files below this skip the queue"), QObject::tr("KB"), 160);
        f.check(queue::HotlistPriority);
        f.check(queue::SendPosition);
        f.endGroup();
        f.check(transfers::UploadsToHotlistOnly);
    }
    // ---- Bandwidth Throttle [obs: settings_bandwidth_throttle.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("throttle"), QObject::tr("Bandwidth Throttle")));
        limitRow(k, QObject::tr("Limit incoming bandwidth"), QString(bandwidth::DownLimitKBs), QString(bandwidth::LastDownLimitKBs), 13);
        limitRow(k, QObject::tr("Limit outgoing bandwidth"), QString(bandwidth::UpLimitKBs), QString(bandwidth::LastUpLimitKBs), 45);
        k.wrap(QObject::tr("The limits cover every transfer, the OctoMX Peer Network and the traffic with servers."), 215, 70, 305, 26);
        k.wrap(QObject::tr("On a broadband line, an outgoing limit of about 85% of the line's upload speed works best."),
               215, 101, 305, 26);
        k.wrap(QObject::tr("A line with very little upload speed (an old ADSL line with 128 kbps, for example) usually "
                           "does better with a SECONDARY connection to the OctoMX Peer Network."),
               215, 130, 305, 39);
        k.wrap(QObject::tr("With a PRIMARY CONNECTION to the OctoMX Peer Network, the share of bandwidth that connection "
                           "may use is set on the \"OctoMX Peer Network\" page."),
               215, 174, 305, 52);
        // [ui-h] "Throttle Process Interval" is gone (UI_SPEC 8.11): the limiter of OctoMX measures
        // time continuously and has no processing interval to set
        k.hline(215, 298, 305);
        k.label(QObject::tr("BW Graph Rolling Avg. Duration"), 220, 313, 174, Qt::AlignRight);
        QComboBox *avg = k.combo(QString(), 397, 308, 81, 21);
        for (const char *s : {"1", "2", "4", "8", "16"})
            avg->addItem(QString::fromLatin1(s) == QLatin1String("1") ? QObject::tr("1 second") : QObject::tr("%1 seconds").arg(QString::fromLatin1(s)),
                         QString::fromLatin1(s));
        avg->setObjectName(PageKit::nameFor(bandwidth::GraphAverageSec));
        avg->setToolTip(settingTip(bandwidth::GraphAverageSec));
        k.b->bind(avg, bandwidth::GraphAverageSec);
    }
    // ---- NG: scheduler
    {
        PageKit k(d, d->addPage(QStringLiteral("throttle/schedule"), QObject::tr("Bandwidth Schedule"), QStringLiteral("throttle")));
        Form f(k);
        f.group(QObject::tr("Alternative Limits"));
        f.check(bandwidth::ScheduleEnabled, QObject::tr("Use these limits at the times below"));
        f.spin(bandwidth::ScheduleDownKBs, QObject::tr("Incoming limit"), QObject::tr("KB/s (0 = none)"), 100);
        f.spin(bandwidth::ScheduleUpKBs, QObject::tr("Outgoing limit"), QObject::tr("KB/s (0 = none)"), 100);
        QLineEdit *from = f.editRow(bandwidth::ScheduleStart, QObject::tr("From (HH:mm)"), 100, 59);
        QLineEdit *to = f.editRow(bandwidth::ScheduleEnd, QObject::tr("Until (HH:mm)"), 100, 59);
        for (QLineEdit *le : {from, to})
            le->setInputMask(QStringLiteral("99:99"));
        auto *days = k.place(new DaysEditor, f.left(), f.y, 290, 16);
        days->setObjectName(PageKit::nameFor(bandwidth::ScheduleDays));
        days->setToolTip(settingTip(bandwidth::ScheduleDays));
        k.b->bind(days, bandwidth::ScheduleDays);
        f.gap(20);
        f.endGroup();
        f.check(bandwidth::LimitLan);
        // [bw-1] which limits are in force now, and why (kept up to date while the page is open)
        WrapLabel *now = f.text(QString(), 5);
        now->setObjectName(QStringLiteral("scheduleNow"));
        auto refresh = [now, s = k.s] {
            const bwlimit::InForce in = speed::inForce(s);
            const QString lim = QObject::tr("DL %1, UL %2").arg(speed::kbsText(in.downKBs), speed::kbsText(in.upKBs));
            const QString until = in.until.isValid() ? QLocale::c().toString(in.until, QStringLiteral("ddd HH:mm")) : QString();
            QString t;
            if (in.source == bwlimit::InForce::Schedule)
                t = QObject::tr("In force now: %1, from the schedule until %2.").arg(lim, until);
            else if (in.source == bwlimit::InForce::Manual)
                t = QObject::tr("In force now: your limits (%1) until %2, then the schedule.").arg(lim, until);
            else if (in.scheduleEnabled && !until.isEmpty())
                t = QObject::tr("In force now: your limits (%1). The schedule starts %2.").arg(lim, until);
            else
                t = QObject::tr("In force now: your limits (%1).").arg(lim);
            t += QObject::tr(" A limit changed on the Bandwidth page, in the tray menu or on the previous page during the "
                             "scheduled time wins until that time ends.");
            now->setText(t);
        };
        refresh();
        QObject::connect(k.s, &Settings::changed, now, [refresh](const QString &key) {
            if (key.startsWith(QLatin1String("bandwidth/")))
                refresh();
        });
        auto *tick = new QTimer(now);
        tick->setInterval(10000);
        QObject::connect(tick, &QTimer::timeout, now, refresh);
        tick->start();
    }
    // ---- File Launching [obs: settings_file_launching.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("launching"), QObject::tr("File Launching")));
        k.wrap(QObject::tr("The programs offered to open a file from the right-click menus of the Transfers and Library "
                           "windows.  (Change them only if you know what they do.)"),
               228, 15, 290, 40);
        k.label(QObject::tr("Title (displayed on menus)"), 228, 63, 160);
        auto *ed = k.place(new LaunchEditor, 225, 76, 295, 200);
        ed->layoutAt(225, 76);
        ed->setObjectName(PageKit::nameFor(launch::Entries));
        k.b->bind(ed, launch::Entries);
        k.label(QObject::tr("( %path% stands for the path of the file )"), 266, 114, 254);
        const struct { const char *t; int y; } labels[] = {{QT_TR_NOOP("File Ext"), 134}, {QT_TR_NOOP("Action"), 166},
                                                         {QT_TR_NOOP("Command"), 197}, {QT_TR_NOOP("Params"), 228},
                                                         {QT_TR_NOOP("Working Dir"), 258}};
        for (const auto &l : labels)
            k.label(QObject::tr(l.t), 215, l.y, 63, Qt::AlignRight);
        k.label(QObject::tr("A NEW ENTRY IS ONLY KEPT"), 215, 291, 305, Qt::AlignHCenter);
        k.label(QObject::tr("AFTER YOU PRESS SAVE"), 215, 304, 305, Qt::AlignHCenter);
    }
    // ---- [dl-1] NG: the player for Preview (Transfers menu) and Open With Preview Player
    {
        PageKit k(d, d->addPage(QStringLiteral("launching/preview"), QObject::tr("Preview Player"), QStringLiteral("launching")));
        k.wrap(QObject::tr("Preview plays an audio or video file while it is still downloading, as far as it is on disk. "
                           "Use this player:"),
               215, 12, 305, 26);
        QRadioButton *ask = k.radio(QObject::tr("Ask the first time (\"Preview With\")"), launch::PreviewPlayer, QString(), 223, 44, 290);
        ask->setToolTip(QObject::tr("The first preview asks which player to use."));
        QRadioButton *sys = k.radio(QObject::tr("The system's default player"), launch::PreviewPlayer, QStringLiteral("system"), 223, 62, 290);
        sys->setToolTip(QObject::tr("Play with the program your system opens audio and video files with."));
        QRadioButton *prog = k.radio(QObject::tr("This program:"), launch::PreviewPlayer, QStringLiteral("program"), 223, 80, 290);
        prog->setToolTip(QObject::tr("Play with the player below; it gets the file as its only argument."));
        auto *path = k.place(new PathField(QObject::tr("Browse..."), false), 239, 100, 281, 22);
        path->setObjectName(PageKit::nameFor(launch::PreviewProgram));
        path->setButtonGeometry(QRect(0, 5, 206, 13), QRect(211, 0, 70, 22));
        path->button()->setToolTip(QObject::tr("Pick the player program on your computer."));
        path->setToolTip(QObject::tr("The player program used for previews."));
#ifdef Q_OS_WIN
        path->setFilter(QObject::tr("Programs (*.exe)"));
#endif
        k.b->bind(path, launch::PreviewProgram);
        QObject::connect(path, &PathField::valueEdited, prog, [prog] { prog->setChecked(true); });
        QString found;
        for (const launcher::Player &p : launcher::findPlayers())
            found += QStringLiteral("\n") + p.name + QStringLiteral(": ") + p.path;
        WrapLabel *list = k.wrap(found.isEmpty() ? QObject::tr("No player was found in the usual places of this computer; the "
                                                               "system's default player or any program you pick works.")
                                                 : QObject::tr("Players found on this computer:") + found,
                                 223, 132, 297, 120);
        list->setObjectName(QStringLiteral("previewPlayersFound"));
        k.wrap(QObject::tr("Only audio and video files are ever previewed, and only as the argument of the player."), 215, 262, 305, 26);
    }
    {
        PageKit k(d, d->addPage(QStringLiteral("launching/options"), QObject::tr("Launch Options"), QStringLiteral("launching")));
        Form f(k);
        f.check(launch::UseOsDefault);
        f.combo(launch::DoubleClickAction, QObject::tr("Double-click on a file"), 120);
        f.check(launch::PreviewIncomplete);
        f.gap(4);
        f.text(QObject::tr("Programs used when \"Use the system default programs\" is off (%1 = the file):"), 2);
        f.editRow(launch::AudioCommand, QObject::tr("Audio"), 70);
        f.editRow(launch::VideoCommand, QObject::tr("Video"), 70);
        f.editRow(launch::ImageCommand, QObject::tr("Pictures"), 70);
        f.editRow(launch::DocumentCommand, QObject::tr("Documents"), 70);
        f.editRow(launch::OtherCommand, QObject::tr("Other files"), 70);
    }
}

// ================================================================================= Miscellaneous

void buildMiscPages(SettingsDialog *d)
{
    // [obs: settings_miscellaneous_patched.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("misc"), QObject::tr("Miscellaneous")));
        auto *frame = k.place(new QFrame, 228, 22, 283, 93);
        frame->setFrameShape(QFrame::Box);
        frame->setFrameShadow(QFrame::Sunken);
        frame->lower();
        k.label(QObject::tr("When main window closed:"), 245, 31, 200);
        k.radio(QObject::tr("Prompt"), misc::CloseAction, QStringLiteral("ask"), 263, 49, 120);
        k.radio(QObject::tr("Keep running in tray"), misc::CloseAction, QStringLiteral("tray"), 263, 66, 120);   // [post-1]
        k.radio(QObject::tr("Exit"), misc::CloseAction, QStringLiteral("exit"), 263, 84, 120);
        PushButton *t = k.toggle(QObject::tr("Start OctoMX hidden in system tray automatically\nwhen computer starts"),
                                 misc::StartWithOs, 242, 130, 252, 40);
        t->setToolTip(QObject::tr("When pressed in, OctoMX starts hidden in the tray each time you log in, so sharing and downloads go on."));
        // [ui-h] the WinMX 3.53 setting "Main Thread Priority" is gone (UI_SPEC 8.11): the network work runs in its own
        // threads, the priority of the window thread changed nothing that could be seen
        k.hline(228, 183, 283);
    }
    // ---- NG: tray, start-up and associations (OsIntegration)
    {
        PageKit k(d, d->addPage(QStringLiteral("misc/system"), QObject::tr("System Integration"), QStringLiteral("misc")));
        Form f(k);
        f.group(QObject::tr("Start-up and Tray"));
        f.check(misc::StartWithOs, QObject::tr("Start OctoMX with the computer (hidden in the tray)"));
        f.check(misc::StartMinimized, QObject::tr("Always start minimized"));
        f.check(misc::TrayBalloons);
        f.check(misc::ConfirmExit, QObject::tr("Confirm exit while transfers are running"));
        f.endGroup();
        // [post-1] Check for Updates: one request to GitHub at start, at most once a day
        // [upd-1] what a new version does (updates/mode; was the check box updates/checkAtStart)
        f.group(QObject::tr("Updates"));
        {
            const struct { const char *v; QString text, tip; } modes[] = {
                {"auto", QObject::tr("Install new versions automatically (recommended)"),
                 QObject::tr("At start (at most once a day) OctoMX asks GitHub for the latest release, downloads a newer one in the "
                             "background, checks its signature and asks you to restart. Nothing is installed without that check.")},
                {"ask", QObject::tr("Ask before downloading a new version"),
                 QObject::tr("OctoMX tells you about a newer release; Update Now downloads, checks and installs it.")},
                {"notify", QObject::tr("Only tell me, I install it myself"),
                 QObject::tr("OctoMX tells you about a newer release and opens its download page. Nothing is downloaded by itself.")},
                {"off", QObject::tr("Do not check at start"),
                 QObject::tr("No request at start. Help > Check for Updates... still checks when you ask.")},
            };
            for (const auto &m : modes) {
                k.radio(m.text, keys::updates::Mode, QString::fromLatin1(m.v), f.left(), f.y, f.width(), 17)->setToolTip(m.tip);
                f.gap(18);
            }
        }
        f.endGroup();
        f.group(QObject::tr("Links and Files"));
        f.check(misc::AssociateMagnet);
        f.check(misc::AssociateEd2k);
        f.check(misc::AssociateTorrent);
        f.check(misc::CheckAssociations, QObject::tr("Check these associations at every start"));
        f.endGroup();
    }
    {
        PageKit k(d, d->addPage(QStringLiteral("misc/interface"), QObject::tr("Interface and Logs"), QStringLiteral("misc")));
        Form f(k);
        f.group(QObject::tr("Windows"));
        f.check(keys::ui::RestoreLastPage);
        f.check(misc::AlwaysOnTop);
        f.check(misc::FlashTaskbar);
        f.check(misc::Sounds);
        f.check(misc::SoundDownloadComplete);
        f.check(keys::ui::ShowGrid);
        f.endGroup();
        f.group(QObject::tr("Log File"));
        f.combo(misc::LogLevel, QObject::tr("Log level"), 100, 100);
        f.spin(misc::LogMaxSizeMB, QObject::tr("Maximum size"), QObject::tr("MB"), 100);
        f.spin(misc::LogKeepFiles, QObject::tr("Old files kept"), QString(), 100);
        f.endGroup();
    }
}

} // namespace mx::ui

#include "settings_pages2.moc"
