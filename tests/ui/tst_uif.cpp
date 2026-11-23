// SPDX-License-Identifier: GPL-3.0-or-later
// [ui-f] honest file types in Search: the one extension table (no duplicates, labels built from it),
// the migration of every old menu text / code, the query and local filter of every menu entry,
// bundle / extension-less torrent classification, the quick buttons (All, pressed state, presets),
// Custom Extensions, the merged-list filter, Settings and Shared File Types built from the table.
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLineEdit>

#include "uib_helpers.h"
#include "mx/settings_keys.h"
#include "mx/simple_settings.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/share_dialogs.h"
#include "ui/dialogs/simple_dialogs.h"
#include "ui/kit/file_types.h"
#include "ui/kit/scale.h"
#include "ui/kit/session.h"
#include "ui/kit/widgets.h"
#include "ui/models/search_model.h"
#include "ui/pages/search_page.h"

using namespace mx;
using namespace mx::ui;

namespace {
const MediaType kTypes[] = {MediaType::Audio, MediaType::Video, MediaType::Image, MediaType::Document,
                            MediaType::Software, MediaType::Archive};

SearchHit file(const QString &name, MediaType reported = MediaType::Any, int kbps = -1)
{
    SearchHit h;
    h.fileName = name;
    h.size = 1000;
    h.type = reported;
    h.bitrateKbps = kbps;
    h.hashes.ed2k = QByteArray(16, 'e');
    SearchSource s;
    s.net = Net::ED2K;
    h.sources << s;
    return h;
}

SearchHit torrent(const QString &name, MediaType category = MediaType::Any, bool bundle = false,
                  const QList<BundleFile> &files = {})
{
    SearchHit h;
    h.fileName = name;
    h.size = 1000;
    h.type = category;
    h.isBundle = bundle;
    h.files = files;
    h.hashes.btih = QByteArray(20, 'b');
    SearchSource s;
    s.net = Net::BitTorrent;
    h.sources << s;
    return h;
}

SearchQuery queryOf(const QString &code)
{
    SearchQuery q;
    FileTypeFilter::fromCode(code).applyTo(q);
    return q;
}

bool passes(const QString &code, const SearchHit &h)
{
    return hitMatchesFileType(queryOf(code), h);
}

QStringList allChoiceCodes(MenuButton *b)
{
    QStringList out;
    for (QMenu *m : filetypes::menusOf(b))
        for (QAction *a : m->actions())
            if (a->isCheckable())
                out << a->data().toString();
    return out;
}

QAction *choiceAction(MenuButton *b, const QString &code)
{
    for (QMenu *m : filetypes::menusOf(b))
        for (QAction *a : m->actions())
            if (a->isCheckable() && a->data().toString() == code)
                return a;
    return nullptr;
}
} // namespace

class TstUif : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    uib::App m_app;
    QString m_customAnswer;
    bool m_customOk = true;
    int m_customAsked = 0;

    Settings *st() const { return m_app.core->settings(); }
    SearchPage *search() const { return qobject_cast<SearchPage *>(m_app.mw->page(QStringLiteral("search"))); }
    SearchTab *tab() const { return search()->currentTab(); }
    MenuButton *typeButton() const { return tab()->findChild<MenuButton *>(QStringLiteral("searchFileType")); }
    PushButton *quick(const char *name) const { return tab()->findChild<PushButton *>(QString::fromLatin1(name)); }

    SearchTab *runSearch(const QString &text)
    {
        m_app.mw->showPage(QStringLiteral("search"));
        SearchTab *t = tab();
        t->setTexts(text, QString());
        t->startSearch();
        QTest::qWait(20);
        t->flushPending();
        return t;
    }
    QStringList visibleNames(SearchTab *t) const
    {
        QStringList out;
        for (int r = 0; r < t->proxy()->rowCount(); ++r)
            out << t->proxy()->index(r, 0).data(Qt::DisplayRole).toString();
        return out;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        // a 4.0 profile: menu texts stored, the 2001 preset defaults, the WinMX 3.53 shared type ids
        const QString profile = m_dir.filePath(QStringLiteral("profile"));
        QDir().mkpath(profile);
        QFile f(QDir(profile).filePath(QStringLiteral("settings.json")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        QJsonObject o;
        o.insert(QStringLiteral("search/defaultType"), QStringLiteral("MP3 Audio - At least 128 kbps"));
        o.insert(QStringLiteral("search/typeButtonAudio"), QStringLiteral("MP3 Audio - Any Bitrate"));
        o.insert(QStringLiteral("search/typeButtonVideo"), QStringLiteral("Video (.avi .asf .mpg .mpeg)"));
        o.insert(QStringLiteral("search/typeButtonPictures"), QStringLiteral("Pictures (.jpg .png .jpeg .gif)"));
        o.insert(QStringLiteral("library/sharedTypes"),
                 QJsonArray{QStringLiteral("mp3"), QStringLiteral("ogg"), QStringLiteral("video"), QStringLiteral("pictures"),
                            QStringLiteral("servers"), QStringLiteral("clr"), QStringLiteral("otheraudio"), QStringLiteral("archive")});
        o.insert(QStringLiteral("ui/settingsMigrated"), 1);
        o.insert(QStringLiteral("ui/chatWarningDisabled"), true);
        f.write(QJsonDocument(o).toJson());
        f.close();
        SearchTab::s_customExtensionsHook = [this](QString *text) {
            ++m_customAsked;
            if (m_customOk)
                *text = m_customAnswer;
            return m_customOk;
        };
        m_app.start(profile);
    }
    void cleanupTestCase()
    {
        m_app.stop();
        SearchTab::s_customExtensionsHook = nullptr;
    }

    // ------------------------------------------------------------------ the table
    void tableHasNoDuplicates()
    {
        QHash<QString, MediaType> seen;
        for (MediaType t : kTypes) {
            const QStringList exts = extensionsForMediaType(t);
            QVERIFY(exts.size() >= 10);
            for (const QString &e : exts) {
                QVERIFY2(!seen.contains(e), qPrintable(QStringLiteral("%1 is in two types").arg(e)));
                seen.insert(e, t);
                QVERIFY2(e == e.toLower() && !e.startsWith(QLatin1Char('.')), qPrintable(e));
                // mediaTypeForFileName() reads the same table
                QCOMPARE(mediaTypeForFileName(QStringLiteral("Some.Name.") + e.toUpper()), t);
            }
        }
        QVERIFY(extensionsForMediaType(MediaType::Any).isEmpty());
        QVERIFY(extensionsForMediaType(MediaType::Other).isEmpty());
        for (const QString &e : losslessAudioExtensions())
            QCOMPARE(seen.value(e), MediaType::Audio);
        for (const QString &e : discImageExtensions())
            QCOMPARE(seen.value(e), MediaType::Software);
        // the owner's examples and the brief's lists
        const struct { MediaType t; const char *exts; } must[] = {
            {MediaType::Audio, "mp3 flac m4a aac ogg opus wav wma alac aiff ape dsf ac3 dts mka mid"},
            {MediaType::Video, "mkv mp4 avi mov webm m4v wmv mpg mpeg ts m2ts mts flv 3gp vob ogv rmvb divx"},
            {MediaType::Image, "jpg jpeg png gif webp avif heic bmp tiff svg psd raw cr2 nef dng jxl"},
            {MediaType::Document, "pdf epub mobi azw3 doc docx odt rtf txt xls xlsx ods ppt pptx odp djvu chm cbr cbz fb2 html htm md csv srt ass sub vtt"},
            {MediaType::Software, "exe msi apk dmg pkg deb rpm appimage jar iso img bin cue nrg mdf"},
            {MediaType::Archive, "zip rar 7z tar gz bz2 xz zst tgz cab"},
        };
        for (const auto &m : must)
            for (const QString &e : QString::fromLatin1(m.exts).split(QLatin1Char(' ')))
                QVERIFY2(seen.value(e, MediaType::Other) == m.t, qPrintable(e));
        // the ones the 2001 labels did not show, at the front of the labels
        QCOMPARE(extensionsForMediaType(MediaType::Audio).mid(0, 4), (QStringList{"mp3", "flac", "m4a", "ogg"}));
        QCOMPARE(extensionsForMediaType(MediaType::Video).mid(0, 4), (QStringList{"mkv", "mp4", "avi", "mov"}));
        QCOMPARE(mediaTypeForFileName(QStringLiteral("Big Buck Bunny 1080p")), MediaType::Other);
        QCOMPARE(mediaTypeForFileName(QStringLiteral(".hidden")), MediaType::Other);
        QCOMPARE(parseExtensionList(QStringLiteral(" .MKV, *.srt;ass  mkv ...")), (QStringList{"mkv", "srt", "ass"}));
    }

    void labelsAreBuiltFromTheTable()
    {
        QCOMPARE(filetypes::menuLabel(QStringLiteral("any")), QStringLiteral("All Files"));
        QCOMPARE(filetypes::menuLabel(QStringLiteral("audio")), QStringLiteral("Audio (.mp3 .flac .m4a .ogg ...)"));
        QCOMPARE(filetypes::menuLabel(QStringLiteral("video")), QStringLiteral("Video (.mkv .mp4 .avi .mov ...)"));
        QCOMPARE(filetypes::menuLabel(QStringLiteral("torrents")), QStringLiteral("Torrents (.torrent)"));
        QCOMPARE(filetypes::menuLabel(QStringLiteral("servers")), QStringLiteral("Server Lists (.met .wsx)"));
        QCOMPARE(filetypes::menuLabel(QStringLiteral("clr")), QStringLiteral("Colour Schemes (.clr)"));
        QCOMPARE(filetypes::menuLabel(QStringLiteral("lossless")), QStringLiteral("Lossless audio (.flac .wav .ape .alac ...)"));
        QCOMPARE(filetypes::menuLabel(QStringLiteral("discimages")), QStringLiteral("Disc Images (.iso .img .bin .cue ...)"));
        const struct { const char *kind; MediaType t; const char *name; } general[] = {
            {"audio", MediaType::Audio, "Audio"}, {"video", MediaType::Video, "Video"}, {"pictures", MediaType::Image, "Pictures"},
            {"documents", MediaType::Document, "Documents"}, {"software", MediaType::Software, "Software"},
            {"archives", MediaType::Archive, "Archives"},
        };
        for (const auto &g : general) {
            const QStringList e = extensionsForMediaType(g.t);
            const QString want = QStringLiteral("%1 (.%2 .%3 .%4 .%5 ...)").arg(QLatin1String(g.name), e[0], e[1], e[2], e[3]);
            QCOMPARE(filetypes::menuLabel(QLatin1String(g.kind)), want);
            // the whole list in the tool tip / status text
            QCOMPARE(filetypes::description(QLatin1String(g.kind)),
                     QStringLiteral("%1: .%2").arg(QLatin1String(g.name), e.join(QStringLiteral(" ."))));
        }
        // the Search menu: All Files first, the general types, Torrents, a separator, the special ones,
        // Custom Extensions...; no 2001 entry is left
        MenuButton *b = typeButton();
        QStringList top;
        for (QAction *a : b->menu()->actions())
            top << (a->isSeparator() ? QStringLiteral("-") : a->text());
        QCOMPARE(top, (QStringList{filetypes::menuLabel("any"), filetypes::menuLabel("audio"), filetypes::menuLabel("video"),
                                   filetypes::menuLabel("pictures"), filetypes::menuLabel("documents"),
                                   filetypes::menuLabel("software"), filetypes::menuLabel("archives"),
                                   filetypes::menuLabel("discimages"),
                                   filetypes::menuLabel("torrents"), QStringLiteral("-"), filetypes::menuLabel("mp3"),
                                   filetypes::menuLabel("lossless"), filetypes::menuLabel("servers"),
                                   filetypes::menuLabel("clr"), QStringLiteral("-"), QStringLiteral("Custom Extensions...")}));
        for (const QString &old : {QStringLiteral("MP3/OGG Audio"), QStringLiteral("OGG Audio"), QStringLiteral("Any Non-MP3 File"),
                                   QStringLiteral("Text (.txt .rtf)"), QStringLiteral("HTML (.htm .html)")})
            QVERIFY(!top.contains(old));
        // every choice: a valid code, its tool tip and status tip list every extension
        const QStringList codes = allChoiceCodes(b);
        QCOMPARE(codes.size(), 1 + 43 + 7 + 43 + 3 + 1);   // audio / mp3: any + 3 x 14 bitrates
        for (const QString &c : codes) {
            if (c == QLatin1String("custom"))
                continue;
            QVERIFY2(FileTypeFilter::fromCode(c).isValid() && FileTypeFilter::fromCode(c).code() == c, qPrintable(c));
            QAction *a = choiceAction(b, c);
            QCOMPARE(a->toolTip(), filetypes::description(c));
            QCOMPARE(a->statusTip(), filetypes::description(c));
            QCOMPARE(a->property("buttonText").toString(), filetypes::buttonText(c));
        }
        QVERIFY(b->menu()->toolTipsVisible());
        QCOMPARE(filetypes::buttonText(QStringLiteral("audio:min:192")), QStringLiteral("Audio - At least 192 kbps"));
        QCOMPARE(filetypes::buttonText(QStringLiteral("mp3:eq:128")), QStringLiteral("MP3 only - Equal to 128 kbps"));
        QCOMPARE(filetypes::buttonText(QStringLiteral("custom:mkv,srt")), QStringLiteral("Custom (.mkv .srt)"));
        // the bitrate submenus stand under Audio as a whole, not only under MP3
        QVERIFY(choiceAction(b, QStringLiteral("audio:max:96")));
        QVERIFY(choiceAction(b, QStringLiteral("mp3:min:320")));
    }

    // ------------------------------------------------------------------ old values
    void everyOldLabelMigrates()
    {
        const int kbps[] = {320, 256, 224, 192, 160, 128, 112, 96, 80, 64, 56, 48, 40, 32};
        const struct { const char *label; const char *code; } audio[] = {
            {"MP3 Audio", "mp3"}, {"MP3/OGG Audio", "audio"}, {"OGG Audio", "audio"}};
        const struct { const char *button; const char *op; } ops[] = {{"At least", "min"}, {"Equal to", "eq"}, {"At best", "max"}};
        const struct { const char *code; const char *kind; } oldCodes[] = {{"mp3", "mp3"}, {"mp3ogg", "audio"}, {"ogg", "audio"}};
        int n = 0;
        for (const auto &a : audio) {
            QCOMPARE(fileTypeCodeFromLegacy(QStringLiteral("%1 - Any Bitrate").arg(QLatin1String(a.label))), QLatin1String(a.code));
            for (const auto &o : ops)
                for (int kb : kbps) {
                    const QString text = QStringLiteral("%1 - %2 %3 kbps").arg(QLatin1String(a.label), QLatin1String(o.button)).arg(kb);
                    QCOMPARE(fileTypeCodeFromLegacy(text), QStringLiteral("%1:%2:%3").arg(QLatin1String(a.code), QLatin1String(o.op)).arg(kb));
                    ++n;
                }
        }
        QCOMPARE(n, 3 * 3 * 14);
        for (const auto &c : oldCodes) {
            QCOMPARE(fileTypeCodeFromLegacy(QStringLiteral("%1:any").arg(QLatin1String(c.code))), QLatin1String(c.kind));
            QCOMPARE(fileTypeCodeFromLegacy(QStringLiteral("%1:min:128").arg(QLatin1String(c.code))),
                     QStringLiteral("%1:min:128").arg(QLatin1String(c.kind)));
            QCOMPARE(fileTypeCodeFromLegacy(QStringLiteral("%1:max:64").arg(QLatin1String(c.code))),
                     QStringLiteral("%1:max:64").arg(QLatin1String(c.kind)));
        }
        const struct { const char *old; const char *code; } plain[] = {
            {"Video (.avi .asf .mpg .mpeg)", "video"},    {"Pictures (.jpg .png .jpeg .gif)", "pictures"},
            {"Text (.txt .rtf)", "documents"},           {"HTML (.htm .html)", "documents"},
            {"Archive (.zip .iso .rar .exe)", "archives"}, {"Server Lists (.met .wsx)", "servers"},
            {"Servers (.wsx)", "servers"},               {"Colour Schemes (.clr)", "clr"},
            {"Audio (.wav .au .aif .midi .mpc)", "audio"}, {"Torrents (.torrent)", "torrents"},
            {"Any Non-MP3 File", "any"},                 {"Any File / Bitrate", "any"},
            // old codes (ui/searchFileType) and media-type words (ui/searchType)
            {"video", "video"}, {"pictures", "pictures"}, {"text", "documents"}, {"html", "documents"},
            {"archive", "archives"}, {"servers", "servers"}, {"clr", "clr"}, {"audio", "audio"},
            {"torrent", "torrents"}, {"nonmp3", "any"}, {"any", "any"}, {"image", "pictures"}, {"document", "documents"},
            // new codes and the new button texts read back
            {"audio:any", "audio"}, {"custom: .MKV, srt", "custom:mkv,srt"}, {"All Files", "any"},
            {"Audio - At least 192 kbps", "audio:min:192"}, {"MP3 only - Any Bitrate", "mp3"}, {"Custom (.mkv .srt)", "custom:mkv,srt"},
        };
        for (const auto &p : plain)
            QCOMPARE(fileTypeCodeFromLegacy(QLatin1String(p.old)), QLatin1String(p.code));
        for (const QString &kind : FileTypeFilter::kinds())
            if (kind != QLatin1String("custom"))
                QCOMPARE(fileTypeCodeFromLegacy(filetypes::buttonText(kind)), kind);
        QCOMPARE(fileTypeCodeFromLegacy(QStringLiteral("nonsense")), QString());
        QCOMPARE(fileTypeCodeFromLegacy(QStringLiteral("custom:")), QString());
        QCOMPARE(fileTypeCodeFromLegacy(QStringLiteral("video:min:128")), QStringLiteral("video"));   // bitrate only for audio
    }

    void profileMigratedOnce()
    {
        // initTestCase stored 4.0 texts: they are codes now; the old preset defaults are the general types
        QCOMPARE(st()->s(keys::search::DefaultType), QStringLiteral("mp3:min:128"));
        QCOMPARE(st()->s(keys::search::TypeButtonAudio), QStringLiteral("audio"));
        QCOMPARE(st()->s(keys::search::TypeButtonVideo), QStringLiteral("video"));
        QCOMPARE(st()->s(keys::search::TypeButtonPictures), QStringLiteral("pictures"));
        // every WinMX 3.53 shared group on + unlisted on = every group of the table
        QCOMPARE(st()->sl(keys::library::SharedTypes), sharedTypeIds());
        QCOMPARE(typeButton()->currentData().toString(), QStringLiteral("mp3:min:128"));
        QCOMPARE(tab()->fileTypeCode(), QStringLiteral("mp3:min:128"));
        // idempotent; a preset the user chose keeps its meaning
        SimpleSettings s;
        s.setValue(keys::search::TypeButtonAudio, QStringLiteral("MP3 Audio - At least 192 kbps"));
        s.setValue(keys::search::TypeButtonVideo, QStringLiteral("Video (.avi .asf .mpg .mpeg)"));
        s.setValue(keys::search::DefaultType, QStringLiteral("garbage"));
        s.setValue(keys::search::CustomExtensions, QStringLiteral(".MKV, srt"));
        s.setValue(keys::library::SharedTypes, QStringList{QStringLiteral("mp3"), QStringLiteral("archive")});
        s.setValue(keys::library::ShareUnlistedTypes, false);
        migrateFileTypeSettings(&s);
        migrateFileTypeSettings(&s);
        QCOMPARE(s.s(keys::search::TypeButtonAudio), QStringLiteral("mp3:min:192"));
        QCOMPARE(s.s(keys::search::TypeButtonVideo), QStringLiteral("video"));
        QCOMPARE(s.s(keys::search::DefaultType), QStringLiteral("any"));
        QCOMPARE(s.s(keys::search::CustomExtensions), QStringLiteral("mkv srt"));
        QCOMPARE(s.sl(keys::library::SharedTypes),
                 (QStringList{QStringLiteral("audio"), QStringLiteral("software"), QStringLiteral("archives")}));
        // the defaults are codes, All Files first
        QCOMPARE(settingDefault(QString(keys::search::DefaultType)).toString(), QStringLiteral("any"));
        QCOMPARE(settingDefault(QString(keys::search::TypeButtonAudio)).toString(), QStringLiteral("audio"));
        QCOMPARE(settingDefault(QString(keys::library::SharedTypes)).toStringList(), sharedTypeIds());
        QVERIFY(settingDefault(QString(keys::library::FileTypes)).toStringList().first().startsWith(QLatin1String("Audio (.mp3 .flac")));
    }

    // ------------------------------------------------------------------ queries and filters
    void everyEntryMakesItsQuery()
    {
        const struct { const char *code; MediaType t; const char *ext; const char *exts; int min, max; } rows[] = {
            {"any", MediaType::Any, "", "", 0, 0},
            {"audio", MediaType::Audio, "", "", 0, 0},
            {"audio:min:192", MediaType::Audio, "", "", 192, 0},
            {"audio:eq:128", MediaType::Audio, "", "", 128, 128},
            {"audio:max:96", MediaType::Audio, "", "", 0, 96},
            {"video", MediaType::Video, "", "", 0, 0},
            {"pictures", MediaType::Image, "", "", 0, 0},
            {"documents", MediaType::Document, "", "", 0, 0},
            {"software", MediaType::Software, "", "", 0, 0},
            {"archives", MediaType::Archive, "", "", 0, 0},
            {"discimages", MediaType::Any, "", "iso img bin cue nrg mdf mds ccd isz", 0, 0},
            {"torrents", MediaType::Any, "torrent", "", 0, 0},
            {"mp3", MediaType::Audio, "mp3", "", 0, 0},
            {"mp3:min:320", MediaType::Audio, "mp3", "", 320, 0},
            {"lossless", MediaType::Audio, "", "flac wav ape alac aiff aif wv dsf dff tta tak", 0, 0},
            {"servers", MediaType::Any, "", "met wsx", 0, 0},
            {"clr", MediaType::Any, "clr", "", 0, 0},
            {"custom:mkv,srt", MediaType::Any, "", "mkv srt", 0, 0},
            {"custom:mkv,webm", MediaType::Video, "", "mkv webm", 0, 0},
            {"custom:epub", MediaType::Document, "epub", "", 0, 0},
        };
        for (const auto &r : rows) {
            const SearchQuery q = queryOf(QLatin1String(r.code));
            QCOMPARE(q.type, r.t);
            QCOMPARE(q.extension, QLatin1String(r.ext));
            QCOMPARE(q.extensions.join(QLatin1Char(' ')), QLatin1String(r.exts));
            QCOMPARE(q.minBitrate, r.min);
            QCOMPARE(q.maxBitrate, r.max);
        }
        // the Search tab sends what its button shows
        auto *cap = m_app.core->search();
        Q_UNUSED(cap)
        tab()->setFileTypeCode(QStringLiteral("lossless"));
        QCOMPARE(typeButton()->currentData().toString(), QStringLiteral("lossless"));
        QCOMPARE(st()->s(keys::search::DefaultType), QStringLiteral("lossless"));
        QCOMPARE(st()->s(keys::ui::SearchType), QStringLiteral("audio"));
    }

    void localFilterMatchesTheLabel()
    {
        // plain files: the extension decides; a name without one is not of any type
        QVERIFY(passes("audio", file("a.FLAC")));
        QVERIFY(passes("audio", file("a.opus")));
        QVERIFY(!passes("audio", file("a.mkv", MediaType::Audio)));   // the network's tag loses to the extension
        QVERIFY(passes("audio", file("track 01", MediaType::Audio)));  // no extension: the tag
        QVERIFY(!passes("audio", file("track 01")));
        QVERIFY(passes("video", file("Sintel.2010.720p.mkv")));
        QVERIFY(passes("video", file("clip.webm")));
        QVERIFY(!passes("video", file("clip.srt")));
        QVERIFY(passes("documents", file("clip.srt")));
        QVERIFY(passes("software", file("ubuntu-24.04.iso")));         // disc images are software
        QVERIFY(!passes("archives", file("ubuntu-24.04.iso")));
        QVERIFY(passes("archives", file("x.7z")));
        QVERIFY(passes("pictures", file("x.heic")));
        QVERIFY(passes("any", file("x.whatever")));
        // bitrate: audio as a whole, unknown bitrate kept
        QVERIFY(passes("audio:min:192", file("a.m4a", MediaType::Any, 256)));
        QVERIFY(!passes("audio:min:192", file("a.m4a", MediaType::Any, 128)));
        QVERIFY(passes("audio:min:192", file("a.flac")));
        QVERIFY(passes("audio:eq:128", file("a.mp3", MediaType::Any, 128)));
        QVERIFY(!passes("audio:eq:128", file("a.mp3", MediaType::Any, 160)));
        QVERIFY(passes("audio:max:128", file("a.mp3", MediaType::Any, 96)));
        QVERIFY(!passes("audio:max:128", file("a.mp3", MediaType::Any, 192)));
        // extension entries
        QVERIFY(passes("mp3", file("a.mp3")));
        QVERIFY(!passes("mp3", file("a.flac")));
        QVERIFY(passes("discimages", file("ubuntu-24.04-desktop-amd64.ISO")));
        QVERIFY(passes("discimages", file("disc.cue")));
        QVERIFY(!passes("discimages", file("setup.exe")));
        QVERIFY(!passes("discimages", file("backup.zip")));
        QVERIFY(passes("lossless", file("a.ape")));
        QVERIFY(!passes("lossless", file("a.m4a")));
        QVERIFY(passes("torrents", file("ubuntu.iso.torrent")));
        QVERIFY(!passes("torrents", file("ubuntu.iso")));
        QVERIFY(passes("servers", file("server.met")));
        QVERIFY(passes("clr", file("dark.clr")));
        QVERIFY(passes("custom:mkv,srt", file("x.srt")));
        QVERIFY(!passes("custom:mkv,srt", file("x.avi")));
    }

    void bundlesAndNamesWithoutExtension()
    {
        // an index category classifies a bundle / a torrent named without an extension
        QVERIFY(passes("video", torrent("Sintel 2010 1080p", MediaType::Video)));
        QVERIFY(!passes("audio", torrent("Sintel 2010 1080p", MediaType::Video)));
        // else the dominant file of the list (by bytes): one .mkv and an .nfo
        const QList<BundleFile> bbb = {BundleFile{"Big Buck Bunny/Big Buck Bunny 1080p.mkv", 900000000, 0},
                                       BundleFile{"Big Buck Bunny/Big Buck Bunny.nfo", 4096, 1}};
        const SearchHit folder = torrent("Big Buck Bunny (2008)", MediaType::Any, true, bbb);
        QCOMPARE(classifyHit(folder), MediaType::Video);
        QVERIFY(passes("video", folder));
        QVERIFY(!passes("documents", folder));
        QVERIFY(passes("custom:nfo", folder));   // extensions: any file of the list
        QVERIFY(!passes("custom:srt", folder));
        // else strong words of the name (release tags, extensions written as words)
        QCOMPARE(classifyHit(torrent("Big Buck Bunny 1080p")), MediaType::Video);
        QCOMPARE(classifyHit(torrent("Sintel 872p mp4", MediaType::Any, true)), MediaType::Video);
        QCOMPARE(classifyHit(torrent("Sintel Fan Remix [FLAC]", MediaType::Any, true)), MediaType::Audio);
        QCOMPARE(classifyHit(torrent("Some Distro 24.04 amd64", MediaType::Any, true)), MediaType::Software);
        QCOMPARE(classifyHit(torrent("Movie 2160p FLAC 5.1", MediaType::Any, true)), MediaType::Video);
        QCOMPARE(classifyHit(file("Big Buck Bunny 1080p")), MediaType::Other);   // plain files: no guessing
        QVERIFY(!passes("audio", torrent("Sintel 872p mp4", MediaType::Any, true)));
        // else unknown: kept by every type (a Video search must not come back empty)
        const SearchHit unknown = torrent("Big Buck Bunny");
        QCOMPARE(classifyHit(unknown), MediaType::Other);
        for (const char *c : {"audio", "video", "pictures", "documents", "software", "archives"})
            QVERIFY2(passes(c, unknown), c);
        QVERIFY(!passes("mp3", unknown));        // extension entries need evidence
        QVERIFY(passes("custom:mkv", torrent("Big Buck Bunny 1080p", MediaType::Video)));   // a single-file torrent: its type
        QVERIFY(!passes("custom:mkv", torrent("Big Buck Bunny 1080p", MediaType::Audio)));
        // a bundle without a file list: its name's extension, an extension written as a word, its type
        QVERIFY(passes("lossless", torrent("Sintel Fan Remix (2010) [FLAC]", MediaType::Any, true)));
        QVERIFY(passes("mp3", torrent("Some Album", MediaType::Audio, true)));
        QVERIFY(!passes("mp3", torrent("Some Album", MediaType::Video, true)));
        // the category beats a contradicting file list? no: the category is what the index says, the
        // list decides only when there is no category
        QCOMPARE(classifyHit(torrent("Disc", MediaType::Software, true, bbb)), MediaType::Software);
        // a known extension of a single file beats the category
        QCOMPARE(classifyHit(torrent("debian-13-netinst.iso", MediaType::Archive)), MediaType::Software);
    }

    void mockSearchPerType()
    {
        // the open-movie demo set: every type, torrents without extensions, bundles with / without lists
        const struct { const char *code; QStringList must; QStringList mustNot; } rows[] = {
            {"any", {"Sintel 2010 1080p", "sintel_poster_2048.png", "sintel-lite-2010.zip"}, {}},
            {"audio", {"Sintel Fan Remix - Captain Lentil - Sintel Theme.flac", "Sintel Fan Remix - Captain Lentil - Sintel Theme.mp3",
                       "Sintel Fan Remix (2010) [FLAC]", "Sintel Blender production files"},
             {"Sintel.2010.720p.mkv", "Sintel 2010 1080p", "Big Buck Bunny (2008)", "sintel_poster_2048.png"}},
            {"video", {"Sintel 2010 1080p", "Big Buck Bunny (2008)", "Sintel.2010.720p.mkv", "sintel-trailer-1080p.webm", "Sintel 4K"},
             {"Sintel Fan Remix - Captain Lentil - Sintel Theme.mp3", "Sintel.2010.English.srt", "Sintel (2010) DVD.iso"}},
            {"pictures", {"sintel_poster_2048.png", "Sintel concept art.webp"}, {"Sintel.2010.720p.mkv"}},
            {"documents", {"Sintel production script.pdf", "Sintel.2010.English.srt"}, {"sintel-lite-2010.zip"}},
            {"software", {"Sintel (2010) DVD.iso", "sintel-blender-addon.exe"}, {"sintel-lite-2010.zip", "Sintel 2010 1080p"}},
            {"archives", {"sintel-lite-2010.zip"}, {"Sintel (2010) DVD.iso", "sintel-blender-addon.exe"}},
            {"mp3", {"Sintel Fan Remix - Captain Lentil - Sintel Theme.mp3"}, {"Sintel Fan Remix - Captain Lentil - Sintel Theme.flac", "Sintel 4K"}},
            {"mp3:min:192", {"Sintel Fan Remix - Captain Lentil - Sintel Theme.mp3"}, {"Sintel Fan Remix - Captain Lentil - Snow Walk.mp3"}},
            {"audio:max:128", {"Sintel Fan Remix - Captain Lentil - Snow Walk.mp3", "Sintel Fan Remix - Captain Lentil - Sintel Theme.flac"},
             {"Sintel Fan Remix - Captain Lentil - Sintel Theme.mp3"}},
            {"lossless", {"Sintel Fan Remix - Captain Lentil - Sintel Theme.flac", "Sintel Fan Remix (2010) [FLAC]"},
             {"Sintel Fan Remix - Captain Lentil - Sintel Theme.mp3"}},
            {"custom:mkv,srt", {"Sintel.2010.720p.mkv", "Sintel.2010.English.srt", "Big Buck Bunny (2008)"},
             {"sintel-trailer-1080p.webm", "Sintel production script.pdf"}},
        };
        for (const auto &r : rows) {
            tab()->setFileTypeCode(QLatin1String(r.code));
            SearchTab *t = runSearch(QStringLiteral("sintel"));
            const QStringList names = visibleNames(t);
            QVERIFY2(!names.isEmpty(), r.code);
            for (const QString &m : r.must)
                QVERIFY2(names.contains(m), qPrintable(QStringLiteral("%1: %2 missing").arg(QLatin1String(r.code), m)));
            for (const QString &m : r.mustNot)
                QVERIFY2(!names.contains(m), qPrintable(QStringLiteral("%1: %2 shown").arg(QLatin1String(r.code), m)));
            // nothing listed contradicts the label
            const SearchQuery q = queryOf(QLatin1String(r.code));
            for (int i = 0; i < t->model()->hitCount(); ++i)
                QVERIFY2(hitMatchesFileType(q, t->model()->hitAt(i)), qPrintable(t->model()->hitAt(i).fileName));
        }
        tab()->setFileTypeCode(QStringLiteral("any"));
    }

    void mergedListFollowsLateFileLists()
    {
        SearchProxy proxy;
        SearchModel model;
        proxy.setSourceModel(&model);
        SearchQuery q = queryOf(QStringLiteral("video"));
        proxy.setTypeFilter(q);
        SearchHit b = torrent("Holiday photos 2026", MediaType::Any, true);
        b.key = QStringLiteral("btih:1");
        model.applyHits({b});
        QCOMPARE(proxy.rowCount(), 1);   // unknown: kept
        b.files = {BundleFile{"img_0001.jpg", 4000000, 0}, BundleFile{"img_0002.jpg", 4000000, 1}};
        model.applyHits({b});
        QCOMPARE(proxy.rowCount(), 0);   // the list arrived: pictures, not video
        proxy.setTypeFilter(queryOf(QStringLiteral("pictures")));
        QCOMPARE(proxy.rowCount(), 1);
        // a source listed under another name (merged by name + size) needs a wanted extension too
        SearchHit g = file("Sintel.2010.720p.mkv");
        g.key = QStringLiteral("ed2k:1");
        SearchSource other;
        other.net = Net::Gnutella;
        other.extra.insert(QStringLiteral("fileName"), QStringLiteral("Sintel.2010.720p.avi"));
        g.sources << other;
        model.applyHits({g});
        proxy.setTypeFilter(queryOf(QStringLiteral("custom:mkv")));
        QCOMPARE(proxy.rowCount(), 1);
        const QModelIndex parent = proxy.index(0, 0);
        QCOMPARE(proxy.rowCount(parent), 1);   // the .avi source row is hidden
        proxy.setTypeFilter(SearchQuery());
        QCOMPARE(proxy.rowCount(), 2);
    }

    // ------------------------------------------------------------------ buttons
    void quickButtonsAllFirstAndPressed()
    {
        m_app.mw->showPage(QStringLiteral("search"));
        PushButton *all = quick("searchAll"), *audio = quick("searchAudio"), *video = quick("searchVideo"),
                   *pics = quick("searchPictures");
        QVERIFY(all && audio && video && pics);
        // first position, the row no wider than the classic three (0..165 design px), even shrink
        QCOMPARE(all->x(), 0);
        QCOMPARE(audio->x(), all->x() + all->width());
        QCOMPARE(video->x(), audio->x() + audio->width());
        QCOMPARE(pics->x(), video->x() + video->width());
        QCOMPARE(pics->x() + pics->width(), S(165));
        // [ui-g] the wide form (a window of 975+ design px, UI_SPEC 8.11) puts Documents | Software | Archives |
        // Disc Images (266 px) between Pictures and the type button; the compact form keeps the classic row
        QCOMPARE(typeButton()->x(), tab()->isWideForm() ? S(165 + 266) : S(165));
        for (PushButton *b : {all, audio, video, pics})
            QVERIFY2(b->fontMetrics().horizontalAdvance(b->text()) <= b->width() - 4, qPrintable(b->text()));
        all->click();
        QVERIFY(all->isChecked() && !audio->isChecked() && !video->isChecked() && !pics->isChecked());
        QCOMPARE(tab()->fileTypeCode(), QStringLiteral("any"));
        QCOMPARE(typeButton()->text(), QStringLiteral("All Files"));
        all->click();   // clicking the pressed one keeps it pressed
        QVERIFY(all->isChecked());
        audio->click();
        QCOMPARE(tab()->fileTypeCode(), QStringLiteral("audio"));   // all audio, not MP3
        QVERIFY(audio->isChecked() && !all->isChecked());
        QVERIFY(typeButton()->text().startsWith(QLatin1String("Audio (.mp3 .flac")));
        QCOMPARE(typeButton()->toolTip(), filetypes::description(QStringLiteral("audio")));
        video->click();
        QCOMPARE(tab()->fileTypeCode(), QStringLiteral("video"));
        QVERIFY(video->isChecked() && !audio->isChecked());
        pics->click();
        QCOMPARE(tab()->fileTypeCode(), QStringLiteral("pictures"));
        QVERIFY(pics->isChecked() && !video->isChecked());
        // the menu choice of another entry releases them all
        choiceAction(typeButton(), QStringLiteral("documents"))->trigger();
        QCOMPARE(tab()->fileTypeCode(), QStringLiteral("documents"));
        for (PushButton *b : {all, audio, video, pics})
            QVERIFY(!b->isChecked());
        // a preset from Settings keeps working, and its button is pressed when chosen in the menu
        st()->setValue(keys::search::TypeButtonAudio, QStringLiteral("audio:min:192"));
        audio->click();
        QCOMPARE(tab()->fileTypeCode(), QStringLiteral("audio:min:192"));
        QCOMPARE(typeButton()->text(), QStringLiteral("Audio - At least 192 kbps"));
        choiceAction(typeButton(), QStringLiteral("audio"))->trigger();
        QVERIFY(!audio->isChecked());
        choiceAction(typeButton(), QStringLiteral("audio:min:192"))->trigger();
        QVERIFY(audio->isChecked());
        st()->setValue(keys::search::TypeButtonAudio, QStringLiteral("audio"));
        // the button text always fits (fewer extensions rather than a cut label)
        for (const QString &c : allChoiceCodes(typeButton())) {
            if (c == QLatin1String("custom"))
                continue;
            tab()->setFileTypeCode(c);
            QVERIFY2(typeButton()->fontMetrics().horizontalAdvance(typeButton()->text()) <= typeButton()->width() - 2 * S(6),
                     qPrintable(typeButton()->text()));
        }
        tab()->setFileTypeCode(QStringLiteral("any"));
    }

    void customExtensions()
    {
        m_customAsked = 0;
        m_customOk = true;
        m_customAnswer = QStringLiteral(" .MKV, srt ");
        choiceAction(typeButton(), QStringLiteral("custom"))->trigger();
        QTRY_COMPARE(m_customAsked, 1);
        QCOMPARE(tab()->fileTypeCode(), QStringLiteral("custom:mkv,srt"));
        QCOMPARE(typeButton()->text(), QStringLiteral("Custom (.mkv .srt)"));
        QCOMPARE(st()->s(keys::search::CustomExtensions), QStringLiteral("mkv srt"));   // remembered
        QCOMPARE(st()->s(keys::search::DefaultType), QStringLiteral("custom:mkv,srt"));
        // Cancel keeps the previous filter
        m_customOk = false;
        tab()->setFileTypeCode(QStringLiteral("video"));
        choiceAction(typeButton(), QStringLiteral("custom"))->trigger();
        QTRY_COMPARE(m_customAsked, 2);
        QCOMPARE(tab()->fileTypeCode(), QStringLiteral("video"));
        QVERIFY(typeButton()->text().startsWith(QLatin1String("Video")));
        // nothing usable typed: same
        m_customOk = true;
        m_customAnswer = QStringLiteral(" , . ");
        choiceAction(typeButton(), QStringLiteral("custom"))->trigger();
        QTRY_COMPARE(m_customAsked, 3);
        QCOMPARE(tab()->fileTypeCode(), QStringLiteral("video"));
        // a new tab starts on the remembered custom choice
        tab()->setFileTypeCode(QStringLiteral("custom:mkv,srt"));
        // the dialog itself
        CustomExtensionsDialog dlg(QStringLiteral("mkv srt"));
        dlg.findChild<QLineEdit *>(QStringLiteral("customExtensionsEdit"))->setText(QStringLiteral("epub, .PDF"));
        QCOMPARE(dlg.extensions(), (QStringList{QStringLiteral("epub"), QStringLiteral("pdf")}));
        tab()->setFileTypeCode(QStringLiteral("any"));
    }

    // ------------------------------------------------------------------ same truth elsewhere
    void settingsAndSharedTypesUseTheTable()
    {
        m_app.mw->openSettings(QStringLiteral("Search"));
        auto *dlg = m_app.mw->findChild<SettingsDialog *>();
        QVERIFY(dlg);
        auto *def = dlg->findChild<MenuButton *>(QStringLiteral("set_search_defaultType"));
        auto *preset = dlg->findChild<MenuButton *>(QStringLiteral("set_search_typeButtonAudio"));
        QVERIFY(def && preset);
        QCOMPARE(preset->currentData().toString(), QStringLiteral("audio"));
        QVERIFY(preset->text().startsWith(QLatin1String("Audio (.mp3")));
        QVERIFY(choiceAction(def, QStringLiteral("custom:mkv,srt")));   // the remembered custom list
        choiceAction(preset, QStringLiteral("lossless"))->trigger();
        QCOMPARE(st()->s(keys::search::TypeButtonAudio), QStringLiteral("lossless"));
        QCOMPARE(quick("searchAudio")->toolTip(), filetypes::description(QStringLiteral("lossless")));
        choiceAction(preset, QStringLiteral("audio"))->trigger();
        dlg->close();
        QTest::qWait(10);
        // Shared File Types: the table's groups, labels and full lists
        SharedFileTypesDialog types(m_app.core->library(), st());
        QCOMPARE(SharedFileTypesDialog::typeIds(), sharedTypeIds());
        for (const QString &id : sharedTypeIds()) {
            auto *c = types.findChild<QCheckBox *>(QStringLiteral("type_") + id);
            QVERIFY2(c, qPrintable(id));
            QCOMPARE(c->text(), filetypes::menuLabel(id));
            QCOMPARE(c->toolTip(), filetypes::kindDescription(id));
            QVERIFY(c->isChecked());
            QVERIFY(c->geometry().bottom() < types.findChild<QLineEdit *>(QStringLiteral("otherExtensions"))->y());
        }
        QVERIFY(types.findChild<QCheckBox *>(QStringLiteral("shareUnlistedTypes"))->geometry().bottom()
                < types.findChild<PushButton *>(QStringLiteral("dlgOk"))->y());
    }
};

QTEST_MAIN(TstUif)
#include "tst_uif.moc"
