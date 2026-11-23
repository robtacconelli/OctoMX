// SPDX-License-Identifier: GPL-3.0-or-later
#include "hit_builder.h"

#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>

#include <algorithm>

#include "transforms.h"

namespace mx::web {

QStringList defaultAdultKeywords()
{
    return {QStringLiteral("xxx"), QStringLiteral("porn"), QStringLiteral("porno"), QStringLiteral("sex"),
            QStringLiteral("hentai"), QStringLiteral("nsfw"), QStringLiteral("milf"), QStringLiteral("brazzers"),
            QStringLiteral("onlyfans"), QStringLiteral("nude"), QStringLiteral("nudes"), QStringLiteral("naked"),
            QStringLiteral("erotic"), QStringLiteral("erotica"), QStringLiteral("fetish"), QStringLiteral("bdsm"),
            QStringLiteral("blowjob"), QStringLiteral("cumshot"), QStringLiteral("creampie"), QStringLiteral("anal"),
            QStringLiteral("gangbang"), QStringLiteral("xvideos"), QStringLiteral("pornhub"), QStringLiteral("jav")};
}

static MediaType keywordType(const QString &name)
{
    const QString n = QLatin1Char(' ') + normalizeForMatch(name) + QLatin1Char(' ');
    static const QRegularExpression video(QStringLiteral(
        " (1080p|720p|2160p|480p|4k|uhd|x264|x265|h264|h 264|h265|hevc|xvid|divx|bluray|bdrip|brrip|webrip|web dl|hdtv|dvdrip|hdrip|s\\d\\de\\d\\d|season \\d+|movie|film|yts|yify|remux) "));
    static const QRegularExpression audio(QStringLiteral(
        " (flac|mp3|320kbps|320|v0|aac|ogg|lossless|discography|album|ep|single|soundtrack|ost|24bit|16bit) "));
    static const QRegularExpression doc(QStringLiteral(" (epub|pdf|mobi|azw3|ebook|ebooks|book|books|handbook|magazine|comic|comics|cbr|cbz) "));
    static const QRegularExpression soft(QStringLiteral(
        " (iso|x64|x86|amd64|arm64|i386|linux|windows|macos|win|installer|portable|crack|setup|debian|ubuntu|fedora|distro|netinst|live|repack) "));
    if (video.match(n).hasMatch()) return MediaType::Video;
    if (audio.match(n).hasMatch()) return MediaType::Audio;
    if (doc.match(n).hasMatch()) return MediaType::Document;
    if (soft.match(n).hasMatch()) return MediaType::Software;
    return MediaType::Other;
}

MediaType guessType(const TorrentRecord &r)
{
    // a known extension of the name beats the provider's category (bt4g files ISOs as "Archive")
    const MediaType byExt = mediaTypeForFileName(r.name);
    if (byExt != MediaType::Other && byExt != MediaType::Any) return byExt;
    if (r.type != MediaType::Any && r.type != MediaType::Other) return r.type;
    if (!r.files.isEmpty()) {
        const RecordFile *largest = nullptr;
        for (const RecordFile &f : r.files)
            if (!largest || f.size > largest->size) largest = &f;
        const MediaType t = mediaTypeForFileName(largest->path);
        if (t != MediaType::Other && t != MediaType::Any) return t;
    }
    const MediaType kw = keywordType(r.name);
    if (kw != MediaType::Other) return kw;
    return r.type == MediaType::Any ? MediaType::Other : r.type;
}

bool isAdult(const TorrentRecord &r, const QStringList &keywords)
{
    if (r.adult) return true;
    if (keywords.isEmpty()) return false;
    const QStringList words = queryWords(r.name + QLatin1Char(' ') + r.category);
    for (const QString &k : keywords) {
        const QString kk = normalizeForMatch(k);
        if (kk.isEmpty()) continue;
        if (kk.contains(QLatin1Char(' '))) {
            if ((QLatin1Char(' ') + words.join(QLatin1Char(' ')) + QLatin1Char(' ')).contains(QLatin1Char(' ') + kk + QLatin1Char(' ')))
                return true;
        } else if (words.contains(kk)) {
            return true;
        }
    }
    return false;
}

static bool matchesTypeOnly(const SearchQuery &q, const SearchHit &h)
{
    SearchQuery t;
    t.type = q.type;
    return hitMatchesFileType(t, h);
}

bool recordMatches(const TorrentRecord &r, const SearchQuery &q, const FilterOptions &opt, QString *why)
{
    auto no = [why](const QString &w) { if (why) *why = w; return false; };
    if (opt.matchName) {
        QString hay = normalizeForMatch(r.name);
        for (const RecordFile &f : r.files) hay += QLatin1Char(' ') + normalizeForMatch(f.path);
        const QStringList words = queryWords(q.text + QLatin1Char(' ') + q.text2);
        for (const QString &w : words)
            if (!hay.contains(w)) return no(QStringLiteral("word '%1' not in name").arg(w));
    }
    if (q.type != MediaType::Any || !queryExtensions(q).isEmpty()) {
        // [ui-f] the shared rule (mx::hitMatchesFileType): category / file list / name, an unknown
        // type is kept ("Big Buck Bunny 1080p"), extensions are looked for in the file list
        SearchHit probe;
        probe.fileName = r.name;
        probe.type = guessType(r);
        probe.isBundle = guessIsBundle(r.numFiles > 0 ? r.numFiles : (r.files.size() > 1 ? int(r.files.size()) : r.numFiles), r.name);
        for (const RecordFile &f : r.files) probe.files << BundleFile{f.path, f.size, -1};
        if (!probe.isBundle && probe.files.size() == 1 && QFileInfo(r.name).suffix().isEmpty())
            probe.fileName = r.files.first().path;   // a single-file torrent: its file's name
        if (!probe.isBundle) probe.files.clear();
        SearchSource bt;
        bt.net = Net::BitTorrent;
        probe.sources << bt;
        if (!hitMatchesFileType(q, probe))
            return no(q.type != MediaType::Any && !matchesTypeOnly(q, probe) ? QStringLiteral("type") : QStringLiteral("extension"));
    }
    if (q.minSize > 0 && (r.size <= 0 || r.size < q.minSize)) return no(QStringLiteral("too small"));
    if (q.maxSize > 0 && (r.size <= 0 || r.size > q.maxSize)) return no(QStringLiteral("too large"));
    if (opt.adultFilter && isAdult(r, opt.adultKeywords)) return no(QStringLiteral("adult"));
    return true;
}

SearchHit recordToHit(const TorrentRecord &r, quint32 queryId, const QString &providerName, const QString &magnet)
{
    SearchHit h;
    h.queryId = queryId;
    h.fileName = r.name;
    h.size = qMax<qint64>(0, r.size);
    h.hashes.btih = r.btih;
    h.hashes.btFileIndex = -1;
    const int numFiles = r.numFiles > 0 ? r.numFiles : (r.files.size() > 1 ? r.files.size() : r.numFiles);
    h.isBundle = guessIsBundle(numFiles, r.name);
    if (h.isBundle) {
        const bool complete = r.numFiles > 0 && r.numFiles == r.files.size();
        for (int i = 0; i < r.files.size(); ++i) {
            BundleFile bf;
            bf.path = r.files.at(i).path;
            bf.size = qMax<qint64>(0, r.files.at(i).size);
            bf.index = complete ? i : -1;
            h.files << bf;
        }
    }
    h.type = guessType(r);
    h.published = r.published;

    if (!r.btih.isEmpty()) {
        SearchSource s;
        s.net = Net::BitTorrent;
        s.uri = magnet;
        s.connection = providerName;
        s.seeds = r.seeders;
        s.peers = r.leechers;
        s.extra.insert(QStringLiteral("provider"), r.providerId);
        s.extra.insert(QStringLiteral("providerName"), providerName);
        if (!r.torrentUrl.isEmpty()) s.extra.insert(QStringLiteral("torrentUrl"), r.torrentUrl);
        if (!r.detailsUrl.isEmpty()) s.extra.insert(QStringLiteral("detailsUrl"), r.detailsUrl);
        if (!r.category.isEmpty()) s.extra.insert(QStringLiteral("category"), r.category);
        for (auto it = r.extra.constBegin(); it != r.extra.constEnd(); ++it) s.extra.insert(it.key(), it.value());
        h.sources << s;
    }
    auto http = [&](const QString &url, const QString &path, qint64 size) {
        SearchSource s;
        s.net = Net::Http;
        s.uri = url;
        s.connection = QStringLiteral("HTTP");
        s.extra.insert(QStringLiteral("provider"), r.providerId);
        s.extra.insert(QStringLiteral("providerName"), providerName);
        if (!path.isEmpty()) s.extra.insert(QStringLiteral("bundlePath"), path);
        if (size > 0) s.extra.insert(QStringLiteral("size"), size);
        h.sources << s;
    };
    if (!r.httpUrl.isEmpty()) http(r.httpUrl, QString(), r.size);
    // largest files first (the main video, not the poster)
    QList<RecordFile> byUrl;
    for (const RecordFile &f : r.files)
        if (!f.url.isEmpty()) byUrl << f;
    std::stable_sort(byUrl.begin(), byUrl.end(), [](const RecordFile &a, const RecordFile &b) { return a.size > b.size; });
    int n = 0;
    for (const RecordFile &f : std::as_const(byUrl)) {
        if (f.url.isEmpty() || !f.url.startsWith(QLatin1String("http"))) continue;
        if (++n > 20) break;
        // a single-file item: the URL is a source for the whole hit
        http(f.url, h.isBundle ? f.path : QString(), f.size);
    }
    return h;
}

QString HitMerger::keyFor(const TorrentRecord &r)
{
    if (!r.btih.isEmpty()) return QStringLiteral("btih:") + QString::fromLatin1(r.btih.toHex());
    if (!r.httpUrl.isEmpty()) return QStringLiteral("url:") + r.httpUrl;
    return QString();
}

void HitMerger::sortSources(SearchHit &h)
{
    // HTTP sources first, then BitTorrent sources by ascending seeds: when the core collapses the BT
    // sources (same magnet URI) the last one (the maximum) wins.
    std::stable_sort(h.sources.begin(), h.sources.end(), [](const SearchSource &a, const SearchSource &b) {
        const int ka = a.net == Net::BitTorrent ? 1 : 0, kb = b.net == Net::BitTorrent ? 1 : 0;
        if (ka != kb) return ka < kb;
        return a.seeds < b.seeds;
    });
}

int HitMerger::add(const TorrentRecord &r, quint32 queryId, const QString &providerName, const QStringList &trackers)
{
    const QString key = keyFor(r);
    if (key.isEmpty()) return -1;
    auto it = m_index.constFind(key);
    if (it == m_index.constEnd()) {
        if (isFull()) return -1;
        QString magnet;
        if (!r.btih.isEmpty()) {
            QStringList tr = trackers;
            int extra = 0;
            for (const QString &t : magnetParams(r.magnet, QStringLiteral("tr")))
                if (!tr.contains(t) && extra++ < 10) tr << t;   // [diag-1] the index's own trackers (was 5)
            magnet = buildMagnet(r.btih, r.name, r.size, tr);
        }
        SearchHit h = recordToHit(r, queryId, providerName, magnet);
        sortSources(h);
        m_hits << h;
        Meta m;
        m.bundleKnown = r.numFiles > 0;
        m_meta << m;
        m_index.insert(key, m_hits.size() - 1);
        return m_hits.size() - 1;
    }
    const int idx = it.value();
    SearchHit &h = m_hits[idx];
    Meta &meta = m_meta[idx];
    const QString magnet = h.sources.isEmpty() ? QString() : [&] {
        for (const SearchSource &s : h.sources) if (s.net == Net::BitTorrent) return s.uri;
        return QString();
    }();
    const SearchHit n = recordToHit(r, queryId, providerName, magnet.isEmpty()
                                        ? buildMagnet(r.btih, r.name, r.size, trackers) : magnet);
    // sources: one per provider (replace that provider's previous source)
    for (const SearchSource &s : n.sources) {
        bool replaced = false;
        for (SearchSource &e : h.sources) {
            if (e.net == s.net && e.uri == s.uri
                && e.extra.value(QStringLiteral("provider")) == s.extra.value(QStringLiteral("provider"))) {
                e = s;
                replaced = true;
                break;
            }
        }
        if (!replaced) h.sources << s;
    }
    // all providers that know this torrent
    QStringList providers;
    for (const SearchSource &s : h.sources) {
        const QString p = s.extra.value(QStringLiteral("provider")).toString();
        if (s.net == Net::BitTorrent && !providers.contains(p)) providers << p;
    }
    for (SearchSource &s : h.sources)
        if (s.net == Net::BitTorrent) s.extra.insert(QStringLiteral("providers"), providers);
    // metadata: keep the best
    if (h.size <= 0 && n.size > 0) h.size = n.size;
    if (r.numFiles > 0 && !meta.bundleKnown) { h.isBundle = n.isBundle; meta.bundleKnown = true; }
    else if (!meta.bundleKnown && h.isBundle && !n.isBundle) h.isBundle = false;   // a name with an extension
    if (!h.isBundle) h.files.clear();
    if (h.isBundle && n.files.size() > h.files.size()) h.files = n.files;
    if (h.type == MediaType::Other && n.type != MediaType::Other) h.type = n.type;
    if (!h.published.isValid() || (n.published.isValid() && n.published < h.published)) h.published = n.published.isValid() ? n.published : h.published;
    sortSources(h);
    return idx;
}

} // namespace mx::web
