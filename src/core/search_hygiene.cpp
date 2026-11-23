// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/search_hygiene.h"

#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>

namespace mx::hygiene {

namespace {

using RX = QRegularExpression;
const RX::PatternOptions CI = RX::CaseInsensitiveOption;

bool isExecutableExt(const QString &e)
{
    static const QSet<QString> s = {QStringLiteral("exe"), QStringLiteral("scr"), QStringLiteral("com"),
                                    QStringLiteral("pif"), QStringLiteral("bat"), QStringLiteral("cmd"),
                                    QStringLiteral("vbs"), QStringLiteral("msi"), QStringLiteral("lnk"),
                                    QStringLiteral("jar"), QStringLiteral("hta"), QStringLiteral("js")};
    return s.contains(e);
}

bool isArchiveExt(const QString &e)
{
    static const QSet<QString> s = {QStringLiteral("zip"), QStringLiteral("rar"), QStringLiteral("7z"),
                                    QStringLiteral("ace"), QStringLiteral("cab"), QStringLiteral("arj"),
                                    QStringLiteral("lzh")};
    return s.contains(e);
}

bool isMediaExt(const QString &e)
{
    const MediaType t = mediaTypeForFileName(QStringLiteral("x.") + e);
    return t == MediaType::Audio || t == MediaType::Video;
}

bool isMediaType(MediaType t) { return t == MediaType::Audio || t == MediaType::Video || t == MediaType::Image; }

const RX &mediaWords()
{
    static const RX rx(QStringLiteral(
        "(?:^|[^a-z0-9])(avi|mkv|mp4|mp3|wmv|mpe?g|flac|divx|xvid|[xh]\\.?26[45]|hevc|dvd(?:rip|scr)?|bd(?:rip)?|brrip|"
        "web-?(?:rip|dl)|hdtv|hdrip|blu-?ray|1080[pi]|720p|2160p|4k|480p|320kbps|album|discography|ost|soundtrack)"
        "(?:[^a-z0-9]|$)"), CI);
    return rx;
}

// tokens that promise a feature-length video
const RX &movieWords()
{
    static const RX rx(QStringLiteral("(?:^|[^a-z0-9])(1080[pi]|2160p|4k|blu-?ray|bd(?:rip)?|brrip|720p|dvd(?:rip|scr)?|"
                                      "xvid|divx|web-?(?:rip|dl)|hdrip|hdtv|[xh]\\.?26[45]|hevc|cam|telesync|ts)(?:[^a-z0-9]|$)"),
                       CI);
    return rx;
}

bool isHighDefWord(const QString &w)
{
    static const RX rx(QStringLiteral("^(1080[pi]|2160p|4k|blu-?ray|bd(rip)?|brrip)$"), CI);
    return rx.match(w).hasMatch();
}

const RX &baitWords()
{
    static const RX rx(QStringLiteral("(?:^|[^a-z0-9])(password|passwort|codec[ ._-]*(?:pack|required|needed)|"
                                      "license[ ._-]*required|player[ ._-]*required|unlock[ ._-]*code|"
                                      "download[ ._-]*(?:accelerator|manager)|survey)(?:[^a-z0-9]|$)"), CI);
    return rx;
}

QString stemOf(const QString &name)
{
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    return dot > 0 ? name.left(dot) : name;
}

QString mib(qint64 b) { return QString::number(double(b) / (1024.0 * 1024.0), 'f', b < 10 * 1024 * 1024 ? 1 : 0) + QStringLiteral(" MiB"); }

} // namespace

bool isSyntheticEd2kHash(const QByteArray &ed2k)
{
    // emule-qt.org bridge hashes: ed 2b 01 11 ff ff ...
    static const QByteArray prefix = QByteArray::fromHex("ed2b0111ffff");
    return ed2k.size() == 16 && ed2k.startsWith(prefix);
}

bool isTorrentBridge(const SearchHit &h)
{
    static const RX tag(QStringLiteral("^\\s*\\[(torrent|kad|bt|magnet)\\s+[^\\]]*\\]"), CI);
    if (tag.match(h.fileName).hasMatch()) return true;
    return isSyntheticEd2kHash(h.hashes.ed2k);
}

QString addReason(const QString &existing, const QString &reason)
{
    if (reason.isEmpty()) return existing;
    if (existing.isEmpty()) return reason;
    if (existing.split(QStringLiteral("; ")).contains(reason)) return existing;
    return existing + QStringLiteral("; ") + reason;
}

QString suspectReason(const SearchHit &h, MediaType reportedType)
{
    if (h.isBundle || h.fileName.isEmpty()) return QString();
    QString out;
    const QString name = h.fileName;
    const QString ext = QFileInfo(name).suffix().toLower();
    const QString stem = stemOf(name);
    const QString innerExt = QFileInfo(stem).suffix().toLower();   // "movie.avi" of "movie.avi.zip"
    const MediaType byExt = mediaTypeForFileName(name);

    // 1. executables / archives wrapped around a media name
    if (isExecutableExt(ext) && (isMediaExt(innerExt) || mediaWords().match(stem).hasMatch()))
        out = addReason(out, QStringLiteral("executable named like a media file"));
    if (isArchiveExt(ext) && isMediaExt(innerExt))
        out = addReason(out, QStringLiteral("archive wrapped around a media file (.%1.%2)").arg(innerExt, ext));
    if (isArchiveExt(ext) && h.size > 0 && h.size < 30LL * 1024 * 1024) {
        const QRegularExpressionMatch m = movieWords().match(stem);
        static const QRegularExpression clip(QStringLiteral("(?:^|[^a-z])(trailer|teaser|sample|preview)"), CI);
        if (m.hasMatch() && isHighDefWord(m.captured(1)) && !clip.match(stem).hasMatch())
            out = addReason(out, QStringLiteral("archive too small (%1) for the %2 video it names").arg(mib(h.size), m.captured(1)));
    }

    // 2. the network's type contradicts the extension
    if (reportedType != MediaType::Any && reportedType != MediaType::Other && byExt != MediaType::Other
        && reportedType != byExt && (isMediaType(reportedType) || isMediaType(byExt))) {
        static const char *names[] = {"any", "audio", "video", "image", "document", "software", "archive", "other"};
        out = addReason(out, QStringLiteral("listed as %1 but the extension says %2")
                                 .arg(QLatin1String(names[int(reportedType)]), QLatin1String(names[int(byExt)])));
    }

    // 3. implausible size for the claimed media
    if (h.size > 0) {
        if (byExt == MediaType::Video) {
            const QRegularExpressionMatch m = movieWords().match(stem);
            static const QRegularExpression shortClip(QStringLiteral("(?:^|[^a-z])(trailer|teaser|sample|preview|clip|promo)"), CI);
            if (h.size < 1024 * 1024 && ext != QLatin1String("ts")) {   // (even trailers are > 1 MiB)
                out = addReason(out, QStringLiteral("too small for a video (%1 bytes)").arg(h.size));
            } else if (m.hasMatch() && !shortClip.match(stem).hasMatch()) {
                const QString w = m.captured(1);
                const qint64 min = isHighDefWord(w) ? 50LL << 20 : 20LL << 20;
                if (h.size < min && w.compare(QLatin1String("ts"), Qt::CaseInsensitive) != 0
                    && w.compare(QLatin1String("cam"), Qt::CaseInsensitive) != 0)
                    out = addReason(out, QStringLiteral("too small (%1) for a %2 video").arg(mib(h.size), w));
            }
        } else if (byExt == MediaType::Audio) {
            const bool lossy = ext == QLatin1String("mp3") || ext == QLatin1String("ogg") || ext == QLatin1String("wma")
                               || ext == QLatin1String("m4a") || ext == QLatin1String("aac") || ext == QLatin1String("opus");
            if (lossy && h.size < 24 * 1024)
                out = addReason(out, QStringLiteral("too small for an audio file (%1 bytes)").arg(h.size));
            if (lossy && h.size > (700LL << 20))
                out = addReason(out, QStringLiteral("too large (%1) for a single %2 file").arg(mib(h.size), ext));
            if (lossy && h.durationSec > 0 && h.bitrateKbps > 0) {
                const double expected = double(h.durationSec) * h.bitrateKbps * 125.0;
                const double ratio = double(h.size) / expected;
                if (ratio < 0.5 || ratio > 2.0)
                    out = addReason(out, QStringLiteral("size does not match %1 kbps × %2 s").arg(h.bitrateKbps).arg(h.durationSec));
            }
        }
    }

    // 4. well-known fake patterns
    if ((ext == QLatin1String("wmv") || ext == QLatin1String("asf")) && movieWords().match(stem).hasMatch())
        out = addReason(out, QStringLiteral("WMV/ASF \"release\" (classic DRM/codec fake)"));
    if (baitWords().match(name).hasMatch())
        out = addReason(out, QStringLiteral("fake bait words in the name"));
    return out;
}

void sanitizeMedia(SearchHit &h)
{
    if (h.durationSec <= 0 || h.durationSec > 7 * 24 * 3600) h.durationSec = -1;
    if (h.bitrateKbps <= 0 || h.bitrateKbps > 100000) h.bitrateKbps = -1;
    if (h.sampleRateHz < 4000 || h.sampleRateHz > 768000) h.sampleRateHz = -1;
    MediaType t = h.isBundle ? MediaType::Other : mediaTypeForFileName(h.fileName);
    if (t == MediaType::Other && !h.isBundle && (h.type == MediaType::Audio || h.type == MediaType::Video)) t = h.type;
    if (t != MediaType::Audio && t != MediaType::Video) {
        h.durationSec = h.bitrateKbps = h.sampleRateHz = -1;
        return;
    }
    if (t == MediaType::Video && h.bitrateKbps > 0) {
        if (h.bitrateKbps < 32) {
            h.bitrateKbps = -1;   // no video stream is that thin: a unit slip
        } else if (h.durationSec > 0 && h.size > 0) {
            const double avgKbps = double(h.size) * 8.0 / h.durationSec / 1000.0;
            if (h.bitrateKbps < avgKbps / 8 || h.bitrateKbps > avgKbps * 8) h.bitrateKbps = -1;
        }
    }
}

QStringList ConstantSourceCountDetector::add(const QString &key, const QByteArray &fileId, int seeds)
{
    if (seeds < m_minSeeds || fileId.isEmpty()) return {};
    QSet<QByteArray> &files = m_files[seeds];
    const int before = int(files.size());
    files.insert(fileId);
    QStringList &keys = m_keys[seeds];
    const bool newKey = !keys.contains(key);
    if (newKey) keys << key;
    if (files.size() < m_minFiles) return {};
    // the reason names the file count: when it grew, every key of this value gets a new reason
    if (int(files.size()) == before && !newKey) return {};
    QStringList out;
    for (const QString &k : std::as_const(keys)) {
        if (int(files.size()) == before && k != key) continue;
        m_flagged.insert(k, seeds);
        out << k;
    }
    return out;
}

QString ConstantSourceCountDetector::reasonFor(int seeds) const
{
    return QStringLiteral("same complete-source count (%1) on %2 different eD2k files")
        .arg(seeds).arg(m_files.value(seeds).size());
}

} // namespace mx::hygiene
