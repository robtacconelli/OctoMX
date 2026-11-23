// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: Settings ▸ Appearance and its colour sub-pages [ui-c] (UI_SPEC §7.1, §2).
// Every colour applies live: the ColorButton writes "colors/...", mx::ui::Colors emits changed()
// and every list / graph / chat view repaints.
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTextStream>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>

#include "mx/services.h"
#include "ui/dialogs/settings_pages.h"
#include "ui/kit/colors.h"
#include "ui/kit/theme.h"
#include "ui/kit/widgets.h"
#include "ui/kit/scale.h"
#include "ui/kit/translation.h"
#include "ui/main_window.h"

namespace mx::ui {

using namespace mx::keys;

namespace {

// row n of a colour page: y = 9, 28, 48, 67, 87 ... (19.5-px pitch, measured)
int rowY(int n)
{
    return 9 + (n * 39) / 2;
}

struct ColorItem {
    const char *label;
    QLatin1StringView key;
};

// x/width of the column layouts measured on settings_appearance_channel_window_colors.png
struct Span { int x, w; };
const Span kFull{217, 299};
const Span kHalf[2] = {{217, 147}, {368, 148}};
const Span kQuarter[4] = {{217, 70}, {290, 74}, {367, 73}, {443, 73}};
const Span kThird[3] = {{217, 98}, {318, 98}, {419, 97}};

void caption(PageKit &k, const QString &t, int n)
{
    QString spaced;
    for (const QChar c : t)
        spaced += c + QStringLiteral(" ");
    k.label(spaced.trimmed(), 217, rowY(n) + 3, 299, Qt::AlignHCenter);
}

int g_colorSeq = 0;

ColorButton *colorAt(PageKit &k, const QString &label, QLatin1StringView key, Span s, int n)
{
    ColorButton *b = k.color(label, QString(key), s.x, rowY(n), s.w, 17);
    b->setObjectName(QStringLiteral("color_%1_%2").arg(k.page->property("mxPageId").toString().section(QLatin1Char('/'), -1)).arg(g_colorSeq++));
    // [ui-g] what the colour is for, and what a click does
    b->setToolTip(QObject::tr("%1. Click to choose another colour; it is used at once in every window.").arg(settingLabel(QString(key))));
    return b;
}

// a reconstructed page: up to 16 items one per row, more in two half-width columns
void listPage(SettingsDialog *d, const QString &id, const QString &label, std::initializer_list<ColorItem> items)
{
    PageKit k(d, d->addPage(id, label, QStringLiteral("appearance")));
    g_colorSeq = 0;
    const int n = int(items.size());
    int i = 0;
    for (const ColorItem &it : items) {
        if (n <= 16)
            colorAt(k, QObject::tr(it.label), it.key, kFull, i);
        else
            colorAt(k, QObject::tr(it.label), it.key, kHalf[i / ((n + 1) / 2)], i % ((n + 1) / 2));
        ++i;
    }
}

void applyScheme(SettingsDialog *d, const QVariantMap &m)
{
    for (auto it = m.constBegin(); it != m.constEnd(); ++it)
        if (it.key().startsWith(QLatin1String("colors/")))
            d->binder()->commit(it.key(), it.value());
}

QVariantMap currentScheme(mx::Settings *s)
{
    QVariantMap m;
    for (const SettingDef &def : settingsSchema())
        if (def.type == SettingType::Color)
            m.insert(def.key, s->s(def.key));
    for (const QString &k : Colors::knownKeys())
        if (!m.contains(k))
            m.insert(k, Colors::instance()->get(k).name());
    return m;
}

// colour scheme file: "[OctoMX Color Scheme]" + "colors/key=#rrggbb" lines
bool loadSchemeFile(const QString &path, QVariantMap *out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        const QString line = ts.readLine().trimmed();
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0 || line.startsWith(QLatin1Char(';')) || line.startsWith(QLatin1Char('#')))
            continue;
        QString key = line.left(eq).trimmed();
        if (!key.startsWith(QLatin1String("colors/")))
            key.prepend(QStringLiteral("colors/"));
        const QColor c(line.mid(eq + 1).trimmed());
        if (c.isValid())
            out->insert(key, c.name());
    }
    return !out->isEmpty();
}

bool saveSchemeFile(const QString &path, const QVariantMap &m)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return false;
    QTextStream ts(&f);
    ts << "[OctoMX Color Scheme]\n";
    for (auto it = m.constBegin(); it != m.constEnd(); ++it)
        ts << it.key() << '=' << it.value().toString() << '\n';
    return true;
}

// [ui-g] a font button whose tool tip says what the font is for and which font is chosen now
class ExplainedFontButton : public FontButton {
public:
    ExplainedFontButton(const QString &text, const QString &what) : FontButton(text), m_what(what) {}
    void setBoundValue(const QVariant &v) override
    {
        FontButton::setBoundValue(v);
        const QString now = v.toString().isEmpty() ? QObject::tr("the standard Tahoma 8 pt") : v.toString();
        setToolTip(QObject::tr("%1 Click to choose another font; it is used at once.\nNow: %2").arg(m_what, now));
    }
private:
    QString m_what;
};

// [ui-g] Load Language From File...: only a Qt translation (.qm) is taken; anything else is refused
// with a message and the setting keeps its value (kit/translation.h loads the file when it changes)
class LanguageField : public PathField {
public:
    explicit LanguageField(const QString &text) : PathField(text, false) {}
    void setValueByUser(const QVariant &v) override
    {
        const QString path = v.toString();
        if (!path.isEmpty() && !translation::isTranslationFile(path)) {
            SettingsBinder::message(window(), QObject::tr("OctoMX"),
                                    QObject::tr("%1 is not a Qt translation file (.qm). OctoMX reads translations "
                                                "made with Qt Linguist; the language files of WinMX 3.x cannot be used.")
                                        .arg(QFileInfo(path).fileName()));
            return;
        }
        PathField::setValueByUser(v);
    }
};

class LinkText : public StaticText {
public:
    LinkText(const QString &t, const QString &url) : StaticText(t), m_url(url)
    {
        QPalette p = palette();
        p.setColor(QPalette::WindowText, QColor(0x00, 0x00, 0xFF));
        setPalette(p);
        setCursor(Qt::PointingHandCursor);
    }
protected:
    void mousePressEvent(QMouseEvent *) override { QDesktopServices::openUrl(QUrl(m_url)); }
private:
    QString m_url;
};

} // namespace

void buildAppearancePages(SettingsDialog *d)
{
    // ---- Appearance [obs: settings_appearance_page.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("appearance"), QObject::tr("Appearance")));
        const int x = 227, w = 282;
        // [post-1] owner request: one obvious switch between the dark and the light lists (live, every
        // window); the rows below moved down by 28 px, "Search For Color Scheme Files" is one line now
        k.label(QObject::tr("Color theme:"), x, 17, 70);
        QComboBox *theme = k.combo(keys::ui::ColorTheme, x + 72, 13, w - 72, 21);
        theme->setToolTip(QObject::tr("The colours of the lists, chat rooms and graphs in every window. Follow system uses "
                                      "the light or dark setting of the operating system. Colours you changed yourself are kept."));
        PushButton *dark = k.button(QObject::tr("Load Standard Dark Background / Light Text Defaults"), x, 41, w, 23);
        dark->setObjectName(QStringLiteral("appearanceDark"));
        dark->setToolTip(QObject::tr("Black lists with light text, the classic WinMX look. Every colour goes back to its "
                                     "standard value, also the ones you changed yourself."));
        // [post-1] the two presets choose the theme and reset every colour to its defaults
        QObject::connect(dark, &QPushButton::clicked, d, [d] {
            d->binder()->commit(QString(keys::ui::ColorTheme), QStringLiteral("dark"));
            QVariantMap m = colorSchemePreset(false);
            for (const QString &key : Colors::knownKeys())
                if (!m.contains(key))
                    m.insert(key, Colors::specDefault(key).name());
            m.insert(QString(colors::ThemeApplied), QStringLiteral("dark"));
            applyScheme(d, m);
        });
        PushButton *light = k.button(QObject::tr("Load Standard Light Background / Dark Text Defaults"), x, 68, w, 23);
        light->setObjectName(QStringLiteral("appearanceLight"));
        light->setToolTip(QObject::tr("White lists with dark text. Every colour goes back to its standard light value, "
                                      "also the ones you changed yourself."));
        QObject::connect(light, &QPushButton::clicked, d, [d] {
            d->binder()->commit(QString(keys::ui::ColorTheme), QStringLiteral("light"));
            QVariantMap m = colorSchemePreset(true);
            m.insert(QString(colors::ThemeApplied), QStringLiteral("light"));
            applyScheme(d, m);
        });
        PushButton *load = k.button(QObject::tr("Load Color Scheme From File..."), x, 96, w, 23);
        load->setObjectName(QStringLiteral("appearanceLoad"));
        load->setToolTip(QObject::tr("Read a colour scheme file (.clr) and use its colours at once in every window. "
                                     "Colours the file does not name stay as they are."));
        QObject::connect(load, &QPushButton::clicked, d, [d] {
            const QString p = PathField::browse(d->core()->settings()->s(keys::ui::ColorSchemeFile), false, d,
                                                QObject::tr("Color Schemes (*.clr *.txt);;All Files (*)"));
            if (p.isEmpty())
                return;
            QVariantMap m;
            if (!loadSchemeFile(p, &m)) {
                SettingsBinder::message(d, QObject::tr("OctoMX"), QObject::tr("%1 is not a color scheme file.").arg(QFileInfo(p).fileName()));
                return;
            }
            applyScheme(d, m);
            d->core()->settings()->setValue(keys::ui::ColorSchemeFile, p);
        });
        PushButton *save = k.button(QObject::tr("Save Current Color Scheme To File..."), x, 124, w, 23);
        save->setObjectName(QStringLiteral("appearanceSave"));
        save->setToolTip(QObject::tr("Write every current colour to a .clr file, to keep your colours or to give them "
                                     "to someone else."));
        QObject::connect(save, &QPushButton::clicked, d, [d] {
            QString p = QFileDialog::getSaveFileName(d, QObject::tr("Save Color Scheme"), d->core()->settings()->s(keys::ui::ColorSchemeFile),
                                                     QObject::tr("Color Schemes (*.clr)"));
            if (p.isEmpty())
                return;
            if (QFileInfo(p).suffix().isEmpty())
                p += QStringLiteral(".clr");
            if (saveSchemeFile(p, currentScheme(d->core()->settings())))
                d->core()->settings()->setValue(keys::ui::ColorSchemeFile, p);
            else
                SettingsBinder::message(d, QObject::tr("OctoMX"), QObject::tr("Could not write %1.").arg(p));
        });
        PushButton *find = k.button(QObject::tr("Search For Color Scheme Files"), x, 151, w, 23);
        find->setObjectName(QStringLiteral("appearanceSearch"));
        find->setToolTip(QObject::tr("Open the Search page and look for .clr colour scheme files that other users share."));
        QObject::connect(find, &QPushButton::clicked, d, [d] {
            if (d->mainWindow())
                d->mainWindow()->startSearch(QStringLiteral(".clr"));
        });
        const struct { const char *t; QLatin1StringView key; int y; const char *what; } fonts[] = {
            {QT_TR_NOOP("Change Normal Font..."), keys::ui::FontNormal, 178,
             QT_TR_NOOP("The font of buttons, labels, menus and list headers.")},
            {QT_TR_NOOP("Change Dynamic Font..."), keys::ui::FontDynamic, 205,
             QT_TR_NOOP("The font of the rows in every list (search results, transfers, shared files, users).")},
            {QT_TR_NOOP("Change Large Font..."), keys::ui::FontLarge, 233,
             QT_TR_NOOP("The font of the news bar, the Networks headline and the chat room titles.")}};
        for (const auto &f : fonts) {
            auto *fb = k.place(new ExplainedFontButton(QObject::tr(f.t), QObject::tr(f.what)), x, f.y, w, 23);
            fb->setObjectName(PageKit::nameFor(QString(f.key)));
            k.b->bind(fb, QString(f.key));
        }
        // [ui-g] a Qt translation (.qm) loaded with QTranslator (kit/translation.h); the WinMX 3.x ".lng"
        // files cannot be read, so the filter offers .qm only. A file that is no translation is refused.
        auto *lang = k.place(new LanguageField(QObject::tr("Load Language From File...")), x, 260, w, 23);
        lang->setButtonGeometry(QRect(0, 0, 0, 0), QRect(0, 0, w, 23));
        lang->setFilter(QObject::tr("Qt Translation Files (*.qm);;All Files (*)"));
        lang->setObjectName(PageKit::nameFor(misc::LanguageFile));
        lang->setToolTip(QObject::tr("The translation file (.qm) OctoMX shows its texts in. Empty: English."));
        lang->button()->setToolTip(QObject::tr("Show the texts of OctoMX in another language: choose a Qt translation "
                                               "file (.qm). Windows opened from now on use it; restart OctoMX to see every "
                                               "text in the new language."));
        k.b->bind(lang, misc::LanguageFile);
        PushButton *english = k.button(QObject::tr("Reset Language To English"), x, 288, w, 23);
        english->setObjectName(QStringLiteral("appearanceEnglish"));
        english->setToolTip(QObject::tr("Remove the language file and show the texts in English again. Windows opened "
                                        "from now on are in English; restart OctoMX to see every text in English."));
        QObject::connect(english, &QPushButton::clicked, d, [d] {
            d->binder()->resetKeys({QString(misc::LanguageFile), QString(misc::Language)});
        });
        // the project's own page: OctoMX has no tie to winmx.com (the 3.53 dialog linked there)
        k.label(QObject::tr("How to make or get a language file:"), 231, 316, 280);
        k.place(new LinkText(QStringLiteral("https://github.com/robtacconelli/OctoMX"),
                             QStringLiteral("https://github.com/robtacconelli/OctoMX")), 234, 330, 280, 13)
            ->setToolTip(QObject::tr("Open the OctoMX project page in your web browser: how to make a translation, "
                                     "news and the source code."));
    }

    // ---- colour sub-pages (tree order of settings_appearance_subtree.png)
    listPage(d, QStringLiteral("appearance/main"), QObject::tr("Main Window"),
             {{QT_TR_NOOP("News Bar Background"), ckeys::NewsBackground}, {QT_TR_NOOP("News Bar Text"), ckeys::NewsText},
              {QT_TR_NOOP("News Bar Link"), ckeys::NewsLink}, {QT_TR_NOOP("List Header Text"), colors::ListHeaderText},
              {QT_TR_NOOP("List Grid Lines"), colors::ListGrid}, {QT_TR_NOOP("Inactive Text"), colors::InactiveText}});
    listPage(d, QStringLiteral("appearance/wpn"), QObject::tr("OctoMX Network Window"),
             {{QT_TR_NOOP("Status: Ready"), colors::StatusLedOn}, {QT_TR_NOOP("Status: Connecting"), colors::StatusLedConnecting},
              {QT_TR_NOOP("Status: Failed"), colors::StatusLedError}, {QT_TR_NOOP("Status: Off"), colors::StatusLedOff}});
    listPage(d, QStringLiteral("appearance/other"), QObject::tr("Other Networks Window"),
             {{QT_TR_NOOP("Background"), colors::ListBackground}, {QT_TR_NOOP("Text"), colors::ListText},
              {QT_TR_NOOP("Selection"), colors::ListSelectedBackground}, {QT_TR_NOOP("Connected"), colors::ServerConnected},
              {QT_TR_NOOP("Connecting / Waiting"), colors::ServerConnecting}, {QT_TR_NOOP("Not Connected"), colors::ServerDisconnected}});
    listPage(d, QStringLiteral("appearance/servermsgs"), QObject::tr("Server Msgs"),
             {{QT_TR_NOOP("Background"), colors::ChatBackground}, {QT_TR_NOOP("Text"), colors::ChatText},
              {QT_TR_NOOP("Server Messages"), colors::ChatSystem}, {QT_TR_NOOP("Errors"), colors::ChatError},
              {QT_TR_NOOP("Message Of The Day"), colors::ChatMotd}, {QT_TR_NOOP("Timestamps"), colors::ChatTimestamp}});
    listPage(d, QStringLiteral("appearance/library"), QObject::tr("Library Window"),
             {{QT_TR_NOOP("Background"), colors::ListBackground}, {QT_TR_NOOP("Text"), colors::ListText},
              {QT_TR_NOOP("Selection"), colors::ListSelectedBackground}, {QT_TR_NOOP("Selected Text"), colors::ListSelectedText},
              {QT_TR_NOOP("Bitrate Below 112"), colors::BitrateLow}, {QT_TR_NOOP("Bitrate 112-159"), colors::BitrateMedium},
              {QT_TR_NOOP("Bitrate 160 And Above"), colors::BitrateHigh}, {QT_TR_NOOP("Partial File"), colors::PartialText},
              {QT_TR_NOOP("Inactive Text"), colors::InactiveText}});
    listPage(d, QStringLiteral("appearance/channellist"), QObject::tr("Channel List Window"),
             {{QT_TR_NOOP("Background"), colors::ListBackground}, {QT_TR_NOOP("Channel"), colors::ChannelName},
              {QT_TR_NOOP("Users"), colors::ChannelUsers}, {QT_TR_NOOP("Topic"), colors::ChannelTopic},
              {QT_TR_NOOP("Selection"), colors::ListSelectedBackground}, {QT_TR_NOOP("User List Text"), colors::ChatUserListText},
              {QT_TR_NOOP("Room Administrators (@)"), colors::ChatUserAdmin}, {QT_TR_NOOP("Voiced Users (+)"), colors::ChatUserVoiced}});

    // ---- Channel Window [obs: settings_appearance_channel_window_colors.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("appearance/channel"), QObject::tr("Channel Window"), QStringLiteral("appearance")));
        g_colorSeq = 0;
        colorAt(k, QObject::tr("Channel Background"), colors::ChatBackground, kFull, 0);
        colorAt(k, QObject::tr("Message Text"), colors::ChatText, kHalf[0], 1);
        colorAt(k, QObject::tr("Message User Name"), colors::ChatOtherName, kHalf[1], 1);
        colorAt(k, QObject::tr("Action/Emote Text"), colors::ChatAction, kFull, 2);
        colorAt(k, QObject::tr("User Join Text"), colors::ChatJoin, kFull, 3);
        colorAt(k, QObject::tr("User Part Text"), colors::ChatPart, kFull, 4);
        colorAt(k, QObject::tr("Channel MOTD"), colors::ChatMotd, kFull, 5);
        colorAt(k, QObject::tr("Topic Change"), colors::ChatTopic, kFull, 6);
        colorAt(k, QObject::tr("Server Error"), colors::ChatError, kFull, 7);
        caption(k, QObject::tr("USER LIST"), 8);
        // [ui-h] "Server Traffic" / "Other" (OpenNap user list colours) are gone: the user list has
        // users only. Their rows show the user list colours that are used.
        colorAt(k, QObject::tr("User Names"), colors::ChatUserListText, kFull, 9);
        colorAt(k, QObject::tr("Admins (@)"), colors::ChatUserAdmin, kHalf[0], 10);
        colorAt(k, QObject::tr("Voiced (+)"), colors::ChatUserVoiced, kHalf[1], 10);
        colorAt(k, QObject::tr("Background"), colors::ChatUserListBackground, kFull, 11);
        const struct { const char *t; QLatin1StringView key; } speeds[] = {
            {"Unk", colors::SpeedSlow}, {"14.4", colors::SpeedSlow}, {"28.8", colors::SpeedSlow}, {"33.6", colors::SpeedSlow},
            {"56K", colors::SpeedSlow}, {"64K", colors::SpeedMedium}, {"128K", colors::SpeedMedium}, {"Cable", colors::SpeedFast},
            {"DSL", colors::SpeedFast}, {"T1", colors::SpeedFast}, {"T3+", colors::SpeedFast}};
        for (int i = 0; i < 8; ++i)
            colorAt(k, QString::fromLatin1(speeds[i].t), speeds[i].key, kQuarter[i % 4], 12 + i / 4);
        for (int i = 8; i < 11; ++i)
            colorAt(k, QString::fromLatin1(speeds[i].t), speeds[i].key, kThird[i - 8], 14);
        caption(k, QObject::tr("INPUT BOX"), 15);
        // the last row sits 4 px higher than the 19.5-px grid (measured)
        colorAt(k, QObject::tr("Background"), colors::ChatInputBackground, kHalf[0], 16)->move(S(kHalf[0].x) - S(SettingsDialog::kPageX), S(317));
        colorAt(k, QObject::tr("Text"), colors::ChatInputText, kHalf[1], 16)->move(S(kHalf[1].x) - S(SettingsDialog::kPageX), S(317));
    }

    // ---- [ui-e] Interface: size (scale), font smoothing, size format, notifications, NG additions
    {
        PageKit k(d, d->addPage(QStringLiteral("appearance/interface"), QObject::tr("Interface"), QStringLiteral("appearance")));
        Form f(k);
        f.group(QObject::tr("Interface Size"));
        QComboBox *scale = f.combo(keys::ui::Scale, QObject::tr("Interface size:"), 110, 150);
        for (int i = 0; i < scale->count(); ++i)
            scale->setItemText(i, scaleChoiceLabel(scale->itemData(i).toString()));
        // [qa-2] tool tips on the new controls
        scale->setToolTip(QObject::tr("How large OctoMX draws its windows, text and icons. A size too large for the screen is reduced."));
        f.combo(keys::ui::SmoothFonts, QObject::tr("Smooth font edges:"), 110, 150)
            ->setToolTip(QObject::tr("Antialiased text. Automatic: on above 100 %, off at 100 % (the classic look)."));
        f.text(QObject::tr("Automatic follows the display's scale setting (Windows: Settings > Display > "
                           "Scale). Changes take effect when OctoMX is restarted."), 3);
        f.endGroup();
        f.group(QObject::tr("Lists"));
        f.check(keys::ui::ExactSizes, QObject::tr("Show exact byte counts (classic Bytes column)"))
            ->setToolTip(QObject::tr("Sizes as exact byte counts (4 388 571) instead of 4.19 MB, rates in K/s."));
        f.check(keys::ui::SearchHistoryMenu, QObject::tr("Offer recent searches in the search box"))
            ->setToolTip(QObject::tr("A drop-down arrow in the search box lists the recent searches (also Alt+Down)."));
        f.endGroup();
        f.group(QObject::tr("Notifications"));
        f.check(keys::ui::NotifyDownloadDone, QObject::tr("Notify me when a download completes"))
            ->setToolTip(QObject::tr("A balloon from the tray icon names each completed download."));
        f.endGroup();
    }

    listPage(d, QStringLiteral("appearance/search"), QObject::tr("Search Window"),
             {{QT_TR_NOOP("Background"), colors::ListBackground}, {QT_TR_NOOP("Text"), colors::ListText},
              {QT_TR_NOOP("Selection"), colors::ListSelectedBackground}, {QT_TR_NOOP("Selected Text"), colors::ListSelectedText},
              {QT_TR_NOOP("Low Bitrate"), colors::BitrateLow}, {QT_TR_NOOP("128K Bitrate"), colors::BitrateMedium},
              {QT_TR_NOOP("High Bitrate"), colors::BitrateHigh}, {QT_TR_NOOP("Slow Speed"), colors::SpeedSlow},
              {QT_TR_NOOP("ISDN Speed"), colors::SpeedMedium}, {QT_TR_NOOP("Fast Speed"), colors::SpeedFast},
              {QT_TR_NOOP("Partial File"), colors::PartialText}, {QT_TR_NOOP("Suspected Fake"), ckeys::SuspectText},
              {QT_TR_NOOP("Slots Free"), colors::Available}, {QT_TR_NOOP("1 Slot Free"), colors::SlotOne},
              {QT_TR_NOOP("No Slots Free"), colors::Unavailable}, {QT_TR_NOOP("Ping Fast"), colors::PingGood},
              {QT_TR_NOOP("Ping Medium"), colors::PingMedium}, {QT_TR_NOOP("Ping Slow"), colors::PingBad},
              {QT_TR_NOOP("Incomplete Match"), colors::MatchIncomplete}, {QT_TR_NOOP("Can Join DL"), colors::CanJoinDownload},
              {QT_TR_NOOP("Downloading"), colors::CurrentlyDownloading},
              {QT_TR_NOOP("Network Name"), colors::NetworkLabel}});
    listPage(d, QStringLiteral("appearance/hotlist"), QObject::tr("Hotlist"),
             {{QT_TR_NOOP("Background"), colors::ListBackground}, {QT_TR_NOOP("Section Headers"), colors::HotlistHeader},
              {QT_TR_NOOP("Temporary"), colors::HotlistTemporary}, {QT_TR_NOOP("Online"), colors::UserOnline},
              {QT_TR_NOOP("Offline"), colors::UserOffline}, {QT_TR_NOOP("Selection"), colors::ListSelectedBackground}});
    listPage(d, QStringLiteral("appearance/browse"), QObject::tr("Browse Window"),
             {{QT_TR_NOOP("Background"), colors::ListBackground}, {QT_TR_NOOP("Text"), colors::ListText},
              {QT_TR_NOOP("Selection"), colors::ListSelectedBackground}, {QT_TR_NOOP("Bitrate Below 112"), colors::BitrateLow},
              {QT_TR_NOOP("Bitrate 112-159"), colors::BitrateMedium}, {QT_TR_NOOP("Bitrate 160 And Above"), colors::BitrateHigh},
              {QT_TR_NOOP("Matches Incomplete File"), colors::MatchIncomplete},
              {QT_TR_NOOP("Can Join Multi-Point Download"), colors::CanJoinDownload},
              {QT_TR_NOOP("Currently Downloading"), colors::CurrentlyDownloading},
              {QT_TR_NOOP("Partial File"), colors::PartialText}});
    listPage(d, QStringLiteral("appearance/private"), QObject::tr("Private Chat Window"),
             {{QT_TR_NOOP("Background"), colors::ChatBackground}, {QT_TR_NOOP("Message Text"), colors::ChatText},
              {QT_TR_NOOP("My Name"), colors::ChatOwnName}, {QT_TR_NOOP("User Name"), colors::ChatOtherName},
              {QT_TR_NOOP("Action/Emote Text"), colors::ChatAction},
              {QT_TR_NOOP("System Messages"), colors::ChatSystem}, {QT_TR_NOOP("Timestamps"), colors::ChatTimestamp},
              {QT_TR_NOOP("Links"), colors::ChatLink}, {QT_TR_NOOP("Input Box Background"), colors::ChatInputBackground},
              {QT_TR_NOOP("Input Box Text"), colors::ChatInputText}});
    listPage(d, QStringLiteral("appearance/ignored"), QObject::tr("Ignored Users Window"),
             {{QT_TR_NOOP("Background"), colors::ListBackground}, {QT_TR_NOOP("Ignored Users"), colors::IgnoredText},
              {QT_TR_NOOP("Selection"), colors::ListSelectedBackground}});
    listPage(d, QStringLiteral("appearance/transfers"), QObject::tr("Transfers Window"),
             {{QT_TR_NOOP("Background"), colors::ListBackground}, {QT_TR_NOOP("Text"), colors::ListText},
              {QT_TR_NOOP("Transferring"), colors::TransferActive}, {QT_TR_NOOP("Locally Queued"), colors::TransferQueued},
              {QT_TR_NOOP("Complete"), colors::TransferComplete}, {QT_TR_NOOP("Errors / Timed Out"), colors::TransferError},
              {QT_TR_NOOP("Paused"), colors::TransferPaused}, {QT_TR_NOOP("Finding / Connecting"), colors::TransferConnecting},
              {QT_TR_NOOP("Negotiating"), colors::TransferNegotiating}, {QT_TR_NOOP("Remotely Queued"), colors::TransferRemoteQueued},
              {QT_TR_NOOP("Busy"), colors::TransferBusy}, {QT_TR_NOOP("Cancelled"), colors::TransferCancelled},
              {QT_TR_NOOP("Progress Bar"), colors::ProgressBar}, {QT_TR_NOOP("Progress Background"), colors::ProgressBackground},
              {QT_TR_NOOP("Parts Arriving Now"), colors::ProgressActive},
              {QT_TR_NOOP("Partial File"), colors::PartialText}, {QT_TR_NOOP("Selection"), colors::ListSelectedBackground}});

    // ---- Bandwidth Window [obs: settings_appearance_bandwidth_window_colors.png]
    {
        PageKit k(d, d->addPage(QStringLiteral("appearance/bandwidth"), QObject::tr("Bandwidth Window"), QStringLiteral("appearance")));
        g_colorSeq = 0;
        k.label(QObject::tr("Incoming Graph"), kHalf[0].x, rowY(0) + 5, kHalf[0].w, Qt::AlignHCenter);
        k.label(QObject::tr("Outgoing Graph"), kHalf[1].x, rowY(0) + 5, kHalf[1].w, Qt::AlignHCenter);
        for (int col = 0; col < 2; ++col) {
            const ColorItem items[] = {
                {QT_TR_NOOP("Background"), colors::GraphBackground}, {QT_TR_NOOP("Labels"), colors::GraphText},
                {QT_TR_NOOP("Lines"), colors::GraphGrid}, {QT_TR_NOOP("Uploads"), colors::GraphUp},
                {QT_TR_NOOP("Downloads"), colors::GraphDown}, {QT_TR_NOOP("Overlay Routing"), colors::GraphOverlay},
                {QT_TR_NOOP("DHT/UDP Packets"), colors::GraphPackets}, {QT_TR_NOOP("Incoming Peers"), colors::GraphIncomingPeers},
                {QT_TR_NOOP("Outgoing Peers"), colors::GraphOutgoingPeers},
                {QT_TR_NOOP("Server Traffic"), col == 0 ? colors::GraphServerIn : colors::GraphServerOut},
                {QT_TR_NOOP("Other"), colors::GraphOther}};
            int n = 1;
            for (const ColorItem &it : items)
                colorAt(k, QObject::tr(it.label), it.key, kHalf[col], n++);
        }
    }
}

} // namespace mx::ui
