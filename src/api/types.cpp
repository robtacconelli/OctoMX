// SPDX-License-Identifier: GPL-3.0-or-later
#include "mx/types.h"
#include "mx/plugin.h"
#include "mx/services.h"

#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QUrl>

namespace mx {

void registerMetaTypes()
{
    qRegisterMetaType<mx::Net>("mx::Net");
    qRegisterMetaType<mx::FileHashes>("mx::FileHashes");
    qRegisterMetaType<mx::UserRef>("mx::UserRef");
    qRegisterMetaType<mx::SearchQuery>("mx::SearchQuery");
    qRegisterMetaType<mx::SearchSource>("mx::SearchSource");
    qRegisterMetaType<mx::SearchHit>("mx::SearchHit");
    qRegisterMetaType<QList<mx::SearchHit>>("QList<mx::SearchHit>");
    qRegisterMetaType<QList<mx::SearchSource>>("QList<mx::SearchSource>");
    qRegisterMetaType<mx::TransferInfo>("mx::TransferInfo");
    qRegisterMetaType<mx::TransferSource>("mx::TransferSource");
    qRegisterMetaType<mx::DownloadRequest>("mx::DownloadRequest");
    qRegisterMetaType<mx::SharedFolder>("mx::SharedFolder");
    qRegisterMetaType<mx::SharedFile>("mx::SharedFile");
    qRegisterMetaType<QList<mx::SharedFile>>("QList<mx::SharedFile>");
    qRegisterMetaType<mx::NetworkStatus>("mx::NetworkStatus");
    qRegisterMetaType<mx::ServerEntry>("mx::ServerEntry");
    qRegisterMetaType<mx::ChatRoomInfo>("mx::ChatRoomInfo");
    qRegisterMetaType<mx::ChatUser>("mx::ChatUser");
    qRegisterMetaType<mx::ChatMessage>("mx::ChatMessage");
    qRegisterMetaType<mx::HotlistEntry>("mx::HotlistEntry");
    qRegisterMetaType<mx::BrowseEntry>("mx::BrowseEntry");
    qRegisterMetaType<QList<mx::BrowseEntry>>("QList<mx::BrowseEntry>");
    qRegisterMetaType<mx::BandwidthSample>("mx::BandwidthSample");
    qRegisterMetaType<mx::BundleFile>("mx::BundleFile");
    qRegisterMetaType<QList<mx::BundleFile>>("QList<mx::BundleFile>");
    qRegisterMetaType<mx::NetworkSummary>("mx::NetworkSummary");   // [glue-2]
    qRegisterMetaType<mx::ServerMessage>("mx::ServerMessage");
    qRegisterMetaType<mx::PartialFile>("mx::PartialFile");
    qRegisterMetaType<QList<QByteArray>>("QList<QByteArray>");
}

QString netName(Net n)
{
    switch (n) {
    case Net::BitTorrent: return QStringLiteral("BitTorrent");
    case Net::ED2K:       return QStringLiteral("eD2k");
    case Net::Kad:        return QStringLiteral("Kad");
    case Net::Gnutella:   return QStringLiteral("Gnutella");
    case Net::Http:       return QStringLiteral("HTTP");
    case Net::Overlay:    return QStringLiteral("OctoMX");
    case Net::Web:        return QStringLiteral("Web");
    case Net::Rats:       return QStringLiteral("Rats");
    case Net::Unknown:    break;
    }
    return QStringLiteral("Unknown");
}

QString netShortName(Net n)
{
    switch (n) {
    case Net::BitTorrent: return QStringLiteral("BT");
    case Net::ED2K:       return QStringLiteral("ed2k");
    case Net::Kad:        return QStringLiteral("Kad");
    case Net::Gnutella:   return QStringLiteral("Gnut");
    case Net::Http:       return QStringLiteral("HTTP");
    case Net::Overlay:    return QStringLiteral("MX");
    case Net::Web:        return QStringLiteral("Web");
    case Net::Rats:       return QStringLiteral("Rats");
    case Net::Unknown:    break;
    }
    return QStringLiteral("?");
}

Net netFromName(const QString &name)
{
    const QString s = name.trimmed().toLower();
    for (Net n : {Net::BitTorrent, Net::ED2K, Net::Kad, Net::Gnutella, Net::Http, Net::Overlay, Net::Web, Net::Rats}) {
        if (s == netName(n).toLower() || s == netShortName(n).toLower())
            return n;
    }
    // [ren-1] "winmx": the legacy name of the network (stored search/networks lists, scripts)
    if (s == QLatin1String("overlay") || s == QLatin1String("winmx")) return Net::Overlay;
    if (s == QLatin1String("torrent") || s == QLatin1String("bittorrent")) return Net::BitTorrent;
    return Net::Unknown;
}

// ---------------------------------------------------------------- FileHashes
bool FileHashes::isEmpty() const
{
    return btih.isEmpty() && btFileRoot.isEmpty() && ed2k.isEmpty() && aich.isEmpty()
        && sha1.isEmpty() && tth.isEmpty() && sha256.isEmpty() && md5.isEmpty();
}

namespace {
// 0 = not comparable, 1 = equal, -1 = different
int cmp(const QByteArray &a, const QByteArray &b)
{
    if (a.isEmpty() || b.isEmpty()) return 0;
    return a == b ? 1 : -1;
}
int cmpBt(const FileHashes &a, const FileHashes &b)
{
    if (a.btih.isEmpty() || b.btih.isEmpty()) return 0;
    if (a.btih != b.btih) return 0;            // different torrents may still contain the same file
    if (a.btFileIndex == b.btFileIndex) return 1;
    if (a.btFileIndex < 0 || b.btFileIndex < 0) return 0; // whole torrent vs one of its files
    return -1;                                 // same torrent, different file
}
} // namespace

bool FileHashes::matches(const FileHashes &o) const
{
    if (conflicts(o)) return false;
    return cmp(ed2k, o.ed2k) == 1 || cmp(sha1, o.sha1) == 1 || cmp(tth, o.tth) == 1
        || cmp(sha256, o.sha256) == 1 || cmp(md5, o.md5) == 1 || cmp(aich, o.aich) == 1
        || cmp(btFileRoot, o.btFileRoot) == 1 || cmpBt(*this, o) == 1;
}

bool FileHashes::conflicts(const FileHashes &o) const
{
    return cmp(ed2k, o.ed2k) == -1 || cmp(sha1, o.sha1) == -1 || cmp(tth, o.tth) == -1
        || cmp(sha256, o.sha256) == -1 || cmp(md5, o.md5) == -1 || cmp(aich, o.aich) == -1
        || cmp(btFileRoot, o.btFileRoot) == -1 || cmpBt(*this, o) == -1;
}

void FileHashes::mergeFrom(const FileHashes &o)
{
    if (btih.isEmpty() && !o.btih.isEmpty()) { btih = o.btih; btFileIndex = o.btFileIndex; }
    if (btFileRoot.isEmpty()) btFileRoot = o.btFileRoot;
    if (ed2k.isEmpty()) ed2k = o.ed2k;
    if (aich.isEmpty()) aich = o.aich;
    if (sha1.isEmpty()) sha1 = o.sha1;
    if (tth.isEmpty()) tth = o.tth;
    if (sha256.isEmpty()) sha256 = o.sha256;
    if (md5.isEmpty()) md5 = o.md5;
}

QVariantMap FileHashes::toVariant() const
{
    QVariantMap m;
    auto put = [&m](const char *k, const QByteArray &v) {
        if (!v.isEmpty()) m.insert(QLatin1String(k), QString::fromLatin1(v.toHex()));
    };
    put("btih", btih);
    if (!btih.isEmpty() && btFileIndex >= 0) m.insert(QStringLiteral("btFileIndex"), btFileIndex);
    put("btFileRoot", btFileRoot);
    put("ed2k", ed2k);
    put("aich", aich);
    put("sha1", sha1);
    put("tth", tth);
    put("sha256", sha256);
    put("md5", md5);
    return m;
}

FileHashes FileHashes::fromVariant(const QVariantMap &m)
{
    FileHashes h;
    auto get = [&m](const char *k) {
        return QByteArray::fromHex(m.value(QLatin1String(k)).toString().toLatin1());
    };
    h.btih = get("btih");
    h.btFileIndex = m.value(QStringLiteral("btFileIndex"), -1).toInt();
    h.btFileRoot = get("btFileRoot");
    h.ed2k = get("ed2k");
    h.aich = get("aich");
    h.sha1 = get("sha1");
    h.tth = get("tth");
    h.sha256 = get("sha256");
    h.md5 = get("md5");
    return h;
}

namespace {
QString base32(const QByteArray &in)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    QString out;
    int buffer = 0, bits = 0;
    for (unsigned char c : in) {
        buffer = (buffer << 8) | c;
        bits += 8;
        while (bits >= 5) {
            out.append(QLatin1Char(alphabet[(buffer >> (bits - 5)) & 31]));
            bits -= 5;
        }
    }
    if (bits > 0)
        out.append(QLatin1Char(alphabet[(buffer << (5 - bits)) & 31]));
    return out;
}
} // namespace

QString FileHashes::toMagnet(const QString &name, qint64 size) const
{
    QStringList parts;
    if (!btih.isEmpty())  parts << QStringLiteral("xt=urn:btih:") + QString::fromLatin1(btih.toHex());
    if (!ed2k.isEmpty())  parts << QStringLiteral("xt=urn:ed2k:") + QString::fromLatin1(ed2k.toHex());
    if (!sha1.isEmpty() && !tth.isEmpty())
        parts << QStringLiteral("xt=urn:bitprint:") + base32(sha1) + QLatin1Char('.') + base32(tth);
    else if (!sha1.isEmpty()) parts << QStringLiteral("xt=urn:sha1:") + base32(sha1);
    else if (!tth.isEmpty())  parts << QStringLiteral("xt=urn:tree:tiger:") + base32(tth);
    if (!aich.isEmpty())  parts << QStringLiteral("xt=urn:aich:") + base32(aich);
    if (!md5.isEmpty())   parts << QStringLiteral("xt=urn:md5:") + QString::fromLatin1(md5.toHex());
    if (!btih.isEmpty() && btFileIndex >= 0) parts << QStringLiteral("so=%1").arg(btFileIndex);
    if (size > 0) parts << QStringLiteral("xl=%1").arg(size);
    if (!name.isEmpty())
        parts << QStringLiteral("dn=") + QString::fromLatin1(QUrl::toPercentEncoding(name));
    return QStringLiteral("magnet:?") + parts.join(QLatin1Char('&'));
}

// ---------------------------------------------------------------- media types
// [ui-f] THE file-type table: mediaTypeForFileName(), extensionsForMediaType(), the Search file-type
// menu, the Shared File Types groups and every network's type filter read it. Most common first
// (the menu labels show the first four). No extension appears twice (tests/api/tst_file_types).
namespace {
struct TypeRow {
    MediaType type;
    const char *extensions;   // space separated
};
const TypeRow kTypeTable[] = {
    {MediaType::Audio, "mp3 flac m4a ogg aac opus wav wma alac aiff aif ape wv dsf dff ac3 dts mka mid midi mpc mp2 mp1 "
                       "oga spx m4b ra au tta tak amr mod xm it s3m"},
    {MediaType::Video, "mkv mp4 avi mov webm m4v wmv mpg mpeg ts m2ts mts flv 3gp vob ogv rmvb divx asf rm ogm xvid mpe "
                       "f4v 3g2 mxf"},
    {MediaType::Image, "jpg png gif webp jpeg avif heic heif bmp tiff tif svg psd xcf raw cr2 cr3 nef arw dng orf rw2 jxl "
                       "ico tga"},
    {MediaType::Document, "pdf epub docx txt mobi azw3 azw doc odt rtf xls xlsx ods ppt pptx odp djvu chm cbr cbz cb7 fb2 "
                          "lit html htm md csv nfo tex ps xps srt ass ssa sub vtt idx"},
    // installers and packages, then disc images (decision: disc images are software, as on eD2k
    // ("Pro") and the torrent indexes ("Applications"))
    {MediaType::Software, "exe msi apk iso dmg pkg deb rpm appimage jar msix appx flatpak snap xapk ipa img bin cue nrg "
                          "mdf mds ccd isz bat sh dll"},
    {MediaType::Archive, "zip rar 7z tar gz bz2 xz zst tgz tbz2 txz cab ace lzh lha arj lz lzma z"},
};
const char kLossless[] = "flac wav ape alac aiff aif wv dsf dff tta tak";
// CD / DVD / Blu-ray images, a subset of the Software extensions (the "Disc Images" search entry)
const char kDiscImages[] = "iso img bin cue nrg mdf mds ccd isz";

QStringList splitList(const char *s)
{
    return QString::fromLatin1(s).split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

const QHash<QString, MediaType> &typeByExtension()
{
    static const QHash<QString, MediaType> map = [] {
        QHash<QString, MediaType> m;
        for (const TypeRow &r : kTypeTable)
            for (const QString &e : splitList(r.extensions))
                if (!m.contains(e)) m.insert(e, r.type);   // duplicates are a table error (tested)
        return m;
    }();
    return map;
}

// "dir/Some.Name.MKV" -> "mkv"; "" when there is no dot (or only a leading one)
QString extensionOf(const QString &fileName)
{
    const int slash = qMax(fileName.lastIndexOf(QLatin1Char('/')), fileName.lastIndexOf(QLatin1Char('\\')));
    const QString base = fileName.mid(slash + 1).trimmed();
    const int dot = base.lastIndexOf(QLatin1Char('.'));
    return dot > 0 ? base.mid(dot + 1).toLower() : QString();
}

bool knownType(MediaType t) { return t != MediaType::Any && t != MediaType::Other; }
} // namespace

MediaType mediaTypeForFileName(const QString &fileName)
{
    const QString ext = extensionOf(fileName);
    if (ext.isEmpty()) return MediaType::Other;
    return typeByExtension().value(ext, MediaType::Other);
}

QStringList extensionsForMediaType(MediaType t)
{
    for (const TypeRow &r : kTypeTable)
        if (r.type == t) return splitList(r.extensions);
    return {};
}

QStringList losslessAudioExtensions()
{
    return splitList(kLossless);
}

QStringList discImageExtensions()
{
    return splitList(kDiscImages);
}

QStringList parseExtensionList(const QString &text)
{
    QStringList out;
    static const QRegularExpression sep(QStringLiteral("[\\s,;|]+"));
    for (QString e : text.toLower().split(sep, Qt::SkipEmptyParts)) {
        while (e.startsWith(QLatin1Char('*')) || e.startsWith(QLatin1Char('.'))) e.remove(0, 1);
        if (e.isEmpty() || e.size() > 16) continue;
        bool ok = true;
        for (const QChar c : std::as_const(e))
            if (!(c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-'))) ok = false;
        if (ok && !out.contains(e)) out << e;
    }
    return out;
}

// ---------------------------------------------------------------- [ui-f] file-type filters
QStringList FileTypeFilter::kinds()
{
    return {QStringLiteral("any"),       QStringLiteral("audio"),    QStringLiteral("video"),
            QStringLiteral("pictures"),  QStringLiteral("documents"), QStringLiteral("software"),
            QStringLiteral("archives"),  QStringLiteral("discimages"), QStringLiteral("torrents"), QStringLiteral("mp3"),
            QStringLiteral("lossless"),  QStringLiteral("servers"),  QStringLiteral("clr"),
            QStringLiteral("custom")};
}

FileTypeFilter FileTypeFilter::fromCode(const QString &code)
{
    FileTypeFilter f;
    const QString c = code.trimmed();
    const int colon = c.indexOf(QLatin1Char(':'));
    const QString kind = (colon < 0 ? c : c.left(colon)).toLower();
    const QString rest = colon < 0 ? QString() : c.mid(colon + 1);
    if (!kinds().contains(kind)) return f;
    if (kind == QLatin1String("custom")) {
        f.custom = parseExtensionList(rest);
        if (f.custom.isEmpty()) return f;
        f.kind = kind;
        return f;
    }
    const bool bitrateKind = kind == QLatin1String("audio") || kind == QLatin1String("mp3");
    if (colon >= 0) {
        const QStringList p = rest.toLower().split(QLatin1Char(':'));
        if (!bitrateKind) return f;
        if (p.size() == 1 && p.at(0) == QLatin1String("any")) {
            // "audio:any" = "audio"
        } else {
            bool ok = false;
            const int kb = p.value(1).toInt(&ok);
            if (p.size() != 2 || !ok || kb <= 0 || kb > 100000
                || !(p.at(0) == QLatin1String("min") || p.at(0) == QLatin1String("eq") || p.at(0) == QLatin1String("max")))
                return f;
            f.bitrateOp = p.at(0);
            f.kbps = kb;
        }
    }
    f.kind = kind;
    return f;
}

QString FileTypeFilter::code() const
{
    if (kind.isEmpty()) return QString();
    if (kind == QLatin1String("custom")) return kind + QLatin1Char(':') + custom.join(QLatin1Char(','));
    if (!bitrateOp.isEmpty() && kbps > 0) return QStringLiteral("%1:%2:%3").arg(kind, bitrateOp).arg(kbps);
    return kind;
}

MediaType FileTypeFilter::mediaType() const
{
    if (kind == QLatin1String("audio") || kind == QLatin1String("mp3") || kind == QLatin1String("lossless"))
        return MediaType::Audio;
    if (kind == QLatin1String("video")) return MediaType::Video;
    if (kind == QLatin1String("pictures")) return MediaType::Image;
    if (kind == QLatin1String("documents")) return MediaType::Document;
    if (kind == QLatin1String("software")) return MediaType::Software;
    if (kind == QLatin1String("archives")) return MediaType::Archive;
    if (kind == QLatin1String("custom")) {
        MediaType common = MediaType::Any;
        for (const QString &e : custom) {
            const MediaType t = typeByExtension().value(e, MediaType::Other);
            if (!knownType(t) || (common != MediaType::Any && common != t)) return MediaType::Any;
            common = t;
        }
        return common;
    }
    return MediaType::Any;
}

QStringList FileTypeFilter::requiredExtensions() const
{
    if (kind == QLatin1String("mp3")) return {QStringLiteral("mp3")};
    if (kind == QLatin1String("lossless")) return losslessAudioExtensions();
    if (kind == QLatin1String("discimages")) return discImageExtensions();
    if (kind == QLatin1String("torrents")) return {QStringLiteral("torrent")};
    if (kind == QLatin1String("servers")) return {QStringLiteral("met"), QStringLiteral("wsx")};
    if (kind == QLatin1String("clr")) return {QStringLiteral("clr")};
    if (kind == QLatin1String("custom")) return custom;
    return {};
}

void FileTypeFilter::applyTo(SearchQuery &q) const
{
    q.type = mediaType();
    const QStringList exts = requiredExtensions();
    q.extension = exts.size() == 1 ? exts.first() : QString();
    q.extensions = exts.size() > 1 ? exts : QStringList();
    q.minBitrate = 0;
    q.maxBitrate = 0;
    if (bitrateOp == QLatin1String("min") || bitrateOp == QLatin1String("eq")) q.minBitrate = kbps;
    if (bitrateOp == QLatin1String("max") || bitrateOp == QLatin1String("eq")) q.maxBitrate = kbps;
}

QString fileTypeCodeFromLegacy(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return QString();
    const FileTypeFilter now = FileTypeFilter::fromCode(t);
    if (now.isValid()) return now.code();
    const QString l = t.toLower();
    // old menu codes (ui/searchFileType, OctoMX <= 3.x): kind[:op:kbps]
    static const QHash<QString, QString> oldKinds = {
        {QStringLiteral("mp3"), QStringLiteral("mp3")},          {QStringLiteral("mp3ogg"), QStringLiteral("audio")},
        {QStringLiteral("ogg"), QStringLiteral("audio")},        {QStringLiteral("text"), QStringLiteral("documents")},
        {QStringLiteral("html"), QStringLiteral("documents")},   {QStringLiteral("archive"), QStringLiteral("archives")},
        {QStringLiteral("torrent"), QStringLiteral("torrents")}, {QStringLiteral("nonmp3"), QStringLiteral("any")},
        {QStringLiteral("image"), QStringLiteral("pictures")},   {QStringLiteral("document"), QStringLiteral("documents")},
        {QStringLiteral("all"), QStringLiteral("any")},          {QStringLiteral("all files"), QStringLiteral("any")},
    };
    {
        const QStringList p = l.split(QLatin1Char(':'));
        const auto it = oldKinds.constFind(p.first());
        if (it != oldKinds.constEnd()) {
            const FileTypeFilter f = FileTypeFilter::fromCode(p.size() > 1 ? *it + QLatin1Char(':') + p.mid(1).join(QLatin1Char(':')) : *it);
            if (f.isValid()) return f.code();
            return FileTypeFilter::fromCode(*it).code();
        }
    }
    // old button / menu texts: "<MP3 Audio|MP3/OGG Audio|OGG Audio> - Any Bitrate | At least|Equal to|At best N kbps"
    static const QRegularExpression bitrate(
        QStringLiteral("^(mp3 audio|mp3/ogg audio|ogg audio|audio|mp3 only)\\s*-\\s*(?:(any bitrate)|(at least|equal to|at best)\\s+(\\d+)\\s*kbps)$"));
    const QRegularExpressionMatch m = bitrate.match(l);
    if (m.hasMatch()) {
        const QString kind = m.captured(1) == QLatin1String("mp3 audio") || m.captured(1) == QLatin1String("mp3 only")
                                 ? QStringLiteral("mp3") : QStringLiteral("audio");
        if (!m.captured(2).isEmpty()) return kind;
        const QString op = m.captured(3) == QLatin1String("at least") ? QStringLiteral("min")
                           : m.captured(3) == QLatin1String("equal to") ? QStringLiteral("eq") : QStringLiteral("max");
        return FileTypeFilter::fromCode(QStringLiteral("%1:%2:%3").arg(kind, op, m.captured(4))).code();
    }
    // a button text "Custom (.mkv .srt)"
    if (l.startsWith(QLatin1String("custom (")) && l.endsWith(QLatin1Char(')'))) {
        const FileTypeFilter f = FileTypeFilter::fromCode(QStringLiteral("custom:") + l.mid(8, l.size() - 9));
        if (f.isValid() && !l.contains(QLatin1String("..."))) return f.code();
    }
    // the other old entries, by their name (the extension lists in brackets changed over time)
    static const struct { const char *prefix; const char *code; } texts[] = {
        {"mp3/ogg audio", "audio"}, {"ogg audio", "audio"}, {"mp3 audio", "mp3"}, {"video", "video"},
        {"pictures", "pictures"},   {"text", "documents"},  {"html", "documents"}, {"archive", "archives"},
        {"server lists", "servers"}, {"servers", "servers"}, {"server address files", "servers"},
        {"colour schemes", "clr"},  {"color schemes", "clr"}, {"other audio", "audio"}, {"audio", "audio"},
        {"torrents", "torrents"},   {"any non-mp3 file", "any"}, {"any file", "any"}, {"documents", "documents"},
        {"software", "software"},   {"archives", "archives"}, {"lossless audio", "lossless"}, {"mp3 only", "mp3"},
        {"disc images", "discimages"},
    };
    for (const auto &x : texts) {
        const QString p = QLatin1String(x.prefix);
        if (l == p || l.startsWith(p + QLatin1Char(' ')) || l.startsWith(p + QLatin1Char('(')) || l.startsWith(p + QLatin1String(" -")))
            return QString::fromLatin1(x.code);
    }
    // a new kind with parameters it does not take ("video:min:128"): the kind
    const QString kind = l.section(QLatin1Char(':'), 0, 0);
    if (kind != QLatin1String("custom") && FileTypeFilter::fromCode(kind).isValid()) return kind;
    return QString();
}

QStringList queryExtensions(const SearchQuery &q)
{
    QStringList all = parseExtensionList(q.extension);
    for (const QString &e : parseExtensionList(q.extensions.join(QLatin1Char(' '))))
        if (!all.contains(e)) all << e;
    return all;
}

MediaType typeFromNameWords(const QString &name)
{
    // strong words only: extensions written as words and release tags; common English words and
    // ambiguous extensions (it, ts, rm, sub, bin, img ...) are not used
    static const QRegularExpression nonWord(QStringLiteral("[^a-z0-9]+"));
    static const QRegularExpression resolution(QStringLiteral("^(\\d{3,4}p|\\d{3,4}i|[248]k|uhd|s\\d{1,2}e\\d{1,3})$"));
    static const QSet<QString> video = {
        QStringLiteral("mkv"), QStringLiteral("mp4"), QStringLiteral("avi"), QStringLiteral("webm"), QStringLiteral("wmv"),
        QStringLiteral("m4v"), QStringLiteral("divx"), QStringLiteral("xvid"), QStringLiteral("x264"), QStringLiteral("x265"),
        QStringLiteral("h264"), QStringLiteral("h265"), QStringLiteral("hevc"), QStringLiteral("av1"), QStringLiteral("bluray"),
        QStringLiteral("bdrip"), QStringLiteral("brrip"), QStringLiteral("webrip"), QStringLiteral("hdtv"), QStringLiteral("dvdrip"),
        QStringLiteral("hdrip"), QStringLiteral("remux"), QStringLiteral("dvd5"), QStringLiteral("dvd9")};
    static const QSet<QString> audio = {
        QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("aac"), QStringLiteral("ogg"), QStringLiteral("opus"),
        QStringLiteral("alac"), QStringLiteral("m4a"), QStringLiteral("wav"), QStringLiteral("320kbps"), QStringLiteral("256kbps"),
        QStringLiteral("192kbps"), QStringLiteral("128kbps"), QStringLiteral("lossless"), QStringLiteral("discography"),
        QStringLiteral("soundtrack"), QStringLiteral("ost"), QStringLiteral("24bit")};
    static const QSet<QString> docs = {QStringLiteral("pdf"), QStringLiteral("epub"), QStringLiteral("mobi"), QStringLiteral("azw3"),
                                       QStringLiteral("ebook"), QStringLiteral("cbr"), QStringLiteral("cbz"), QStringLiteral("djvu")};
    static const QSet<QString> soft = {QStringLiteral("iso"), QStringLiteral("exe"), QStringLiteral("msi"), QStringLiteral("apk"),
                                       QStringLiteral("dmg"), QStringLiteral("x64"), QStringLiteral("x86"), QStringLiteral("amd64"),
                                       QStringLiteral("arm64"), QStringLiteral("i386"), QStringLiteral("installer"), QStringLiteral("setup")};
    bool v = false, a = false, d = false, w = false;
    for (const QString &t : name.toLower().split(nonWord, Qt::SkipEmptyParts)) {
        if (video.contains(t) || resolution.match(t).hasMatch()) v = true;
        else if (audio.contains(t)) a = true;
        else if (docs.contains(t)) d = true;
        else if (soft.contains(t)) w = true;
    }
    if (v) return MediaType::Video;   // "Movie 1080p [FLAC 5.1]" is a film with a lossless track
    if (a) return MediaType::Audio;
    if (d) return MediaType::Document;
    if (w) return MediaType::Software;
    return MediaType::Other;
}

MediaType classifyHit(const SearchHit &h)
{
    const MediaType byName = mediaTypeForFileName(h.fileName);
    bool torrentLike = h.isBundle || !h.hashes.btih.isEmpty();
    for (const SearchSource &src : h.sources)
        if (src.net == Net::BitTorrent || src.net == Net::Web || src.net == Net::Rats) torrentLike = true;
    if (!h.isBundle) {
        if (knownType(byName)) return byName;
        if (knownType(h.type)) return h.type;
        // a torrent named without its extension ("Sintel 872p mp4"): the words of the name
        return torrentLike ? typeFromNameWords(h.fileName) : MediaType::Other;
    }
    if (knownType(h.type)) return h.type;   // the index category / content type
    if (!h.files.isEmpty()) {
        QHash<int, qint64> bytes;
        for (const BundleFile &f : h.files) {
            const MediaType t = mediaTypeForFileName(f.path);
            if (knownType(t)) bytes[int(t)] += qMax<qint64>(1, f.size);
        }
        int best = -1;
        qint64 most = 0;
        for (auto it = bytes.constBegin(); it != bytes.constEnd(); ++it)
            if (it.value() > most || (it.value() == most && it.key() < best)) { most = it.value(); best = it.key(); }
        if (best >= 0) return MediaType(best);
    }
    return knownType(byName) ? byName : typeFromNameWords(h.fileName);
}

bool hitMatchesFileType(const SearchQuery &q, const SearchHit &h)
{
    if (q.minBitrate > 0 && h.bitrateKbps > 0 && h.bitrateKbps < q.minBitrate) return false;
    if (q.maxBitrate > 0 && h.bitrateKbps > 0 && h.bitrateKbps > q.maxBitrate) return false;
    const MediaType c = classifyHit(h);
    bool torrentLike = h.isBundle || !h.hashes.btih.isEmpty();
    for (const SearchSource &src : h.sources)
        if (src.net == Net::BitTorrent || src.net == Net::Web || src.net == Net::Rats) torrentLike = true;
    const QStringList exts = queryExtensions(q);
    if (!exts.isEmpty()) {
        // the known types of the listed extensions: the fallback when the files cannot be seen
        auto typeFits = [&exts, c] {
            if (!knownType(c)) return false;
            for (const QString &e : exts)
                if (typeByExtension().value(e, MediaType::Other) == c) return true;
            return false;
        };
        const QString ext = extensionOf(h.fileName);
        if (!h.isBundle) {
            const bool nameKnown = typeByExtension().contains(ext) || ext == QLatin1String("torrent")
                                   || ext == QLatin1String("met") || ext == QLatin1String("wsx") || ext == QLatin1String("clr");
            // a single-file torrent listed under a name without its extension ("Sintel 1080p")
            if (!exts.contains(ext) && !(torrentLike && !nameKnown && typeFits())) return false;
        } else if (!h.files.isEmpty()) {
            bool any = false;
            for (const BundleFile &f : h.files)
                if (exts.contains(extensionOf(f.path))) { any = true; break; }
            if (!any) return false;
        } else {
            bool ok = exts.contains(ext);
            if (!ok) {
                static const QRegularExpression nonWord(QStringLiteral("[^a-z0-9]+"));
                const QStringList words = h.fileName.toLower().split(nonWord, Qt::SkipEmptyParts);
                for (const QString &e : exts)
                    if (words.contains(e)) { ok = true; break; }
            }
            if (!ok && !typeFits()) return false;
        }
    }
    // with extensions the extension check above decides (a bundle holding a wanted file is shown
    // whatever most of its bytes are); the type only narrows what the networks are asked for
    if (exts.isEmpty() && q.type != MediaType::Any && q.type != MediaType::Other) {
        if (knownType(c)) {
            if (c != q.type) return false;
        } else if (!torrentLike) {
            return false;   // a plain file whose extension says nothing
        }
    }
    return true;
}

bool fileNameMatchesFileType(const SearchQuery &q, const QString &fileName, MediaType reported, int bitrateKbps)
{
    SearchHit h;
    h.fileName = fileName;
    h.type = reported;
    h.bitrateKbps = bitrateKbps;
    return hitMatchesFileType(q, h);
}
// ---------------------------------------------------------------- misc
QString UserRef::key() const
{
    return netShortName(net) + QLatin1Char(':') + id;
}

QString countNoun(qint64 n, const QString &singular, const QString &plural)
{
    const QString word = (n == 1 || n == -1) ? singular : (plural.isEmpty() ? singular + QLatin1Char('s') : plural);
    return QStringLiteral("%1 %2").arg(n).arg(word);
}

QString transferStateText(TransferState s)
{
    switch (s) {
    case TransferState::Queued:         return QStringLiteral("Locally Queued");
    case TransferState::FindingSources: return QStringLiteral("Finding sources...");
    case TransferState::Connecting:     return QStringLiteral("Connecting...");
    case TransferState::RemotelyQueued: return QStringLiteral("Remotely Queued");
    case TransferState::Transferring:   return QStringLiteral("Transferring");
    case TransferState::Paused:         return QStringLiteral("Paused");
    case TransferState::Verifying:      return QStringLiteral("Verifying...");
    case TransferState::Completed:      return QStringLiteral("Complete");
    case TransferState::Seeding:        return QStringLiteral("Sharing");
    case TransferState::Error:          return QStringLiteral("Error");
    case TransferState::Cancelled:      return QStringLiteral("Cancelled");
    case TransferState::TimedOut:       return QStringLiteral("Timed out");
    }
    return QString();
}


// ---------------------------------------------------------------- shared file types ([glue-3], [ui-f])
QStringList sharedTypeIds()
{
    return {QStringLiteral("audio"),    QStringLiteral("video"),    QStringLiteral("pictures"),
            QStringLiteral("documents"), QStringLiteral("software"), QStringLiteral("archives"),
            QStringLiteral("torrents"), QStringLiteral("servers"),  QStringLiteral("clr")};
}

namespace {
const QStringList &legacySharedIds()
{
    static const QStringList ids = {QStringLiteral("mp3"), QStringLiteral("ogg"), QStringLiteral("otheraudio"),
                                    QStringLiteral("archive")};
    return ids;
}
} // namespace

bool isLegacySharedTypeList(const QStringList &ids)
{
    for (const QString &id : ids)
        if (legacySharedIds().contains(id.trimmed().toLower())) return true;
    return false;
}

QStringList normalizeSharedTypeIds(const QStringList &ids, bool shareUnlisted)
{
    QSet<QString> on;
    for (const QString &raw : ids) {
        const QString id = raw.trimmed().toLower();
        if (id == QLatin1String("mp3") || id == QLatin1String("ogg") || id == QLatin1String("otheraudio")) on << QStringLiteral("audio");
        else if (id == QLatin1String("archive")) { on << QStringLiteral("archives") << QStringLiteral("software"); }
        else if (sharedTypeIds().contains(id)) on << id;
    }
    if (isLegacySharedTypeList(ids) && shareUnlisted) on << QStringLiteral("documents") << QStringLiteral("torrents");
    QStringList out;
    for (const QString &id : sharedTypeIds())
        if (on.contains(id)) out << id;
    return out;
}

QStringList sharedTypeExtensions(const QString &typeId)
{
    const QString id = typeId.trimmed().toLower();
    if (id == QLatin1String("audio") || id == QLatin1String("mp3") || id == QLatin1String("ogg") || id == QLatin1String("otheraudio"))
        return extensionsForMediaType(MediaType::Audio);
    if (id == QLatin1String("video")) return extensionsForMediaType(MediaType::Video);
    if (id == QLatin1String("pictures")) return extensionsForMediaType(MediaType::Image);
    if (id == QLatin1String("documents")) return extensionsForMediaType(MediaType::Document);
    if (id == QLatin1String("software")) return extensionsForMediaType(MediaType::Software);
    if (id == QLatin1String("archives")) return extensionsForMediaType(MediaType::Archive);
    if (id == QLatin1String("archive")) return extensionsForMediaType(MediaType::Archive) + extensionsForMediaType(MediaType::Software);
    if (id == QLatin1String("torrents")) return {QStringLiteral("torrent")};
    if (id == QLatin1String("servers")) return {QStringLiteral("met"), QStringLiteral("wsx")};
    if (id == QLatin1String("clr")) return {QStringLiteral("clr")};
    return {};
}

bool fileMatchesSharedTypes(const QString &fileName, const QStringList &typeIds, const QString &otherExtensions,
                            bool shareUnlisted)
{
    const QString ext = extensionOf(fileName);
    if (!ext.isEmpty() && parseExtensionList(otherExtensions).contains(ext)) return true;
    const QStringList on = normalizeSharedTypeIds(typeIds, shareUnlisted);
    bool listed = false;
    for (const QString &id : sharedTypeIds()) {
        if (ext.isEmpty() || !sharedTypeExtensions(id).contains(ext)) continue;
        listed = true;
        if (on.contains(id)) return true;
    }
    return !listed && shareUnlisted;
}

} // namespace mx
