// SPDX-License-Identifier: GPL-3.0-or-later
// [dl-1] download order bands and preview readiness (see mx/download_order.h)
#include "mx/download_order.h"

#include <QtCore/QFileInfo>
#include <QtCore/QObject>
#include <QtCore/QStringList>

#include "mx/types.h"

namespace mx {

namespace {
constexpr qint64 kMiB = 1024 * 1024;

qint64 band(qint64 size)
{
    if (size <= 0) return 0;
    const qint64 onePercent = (size + 99) / 100;
    return qMin(qMax(4 * kMiB, onePercent), qMax<qint64>(1, size / 4));
}

QString suffixOf(const QString &name)
{
    return QFileInfo(name).suffix().toLower();
}

quint64 be(const QByteArray &b, int at, int n)
{
    quint64 v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | quint8(b.at(at + i));
    return v;
}
} // namespace

namespace order {
qint64 headBandBytes(qint64 fileSize) { return band(fileSize); }
qint64 tailBandBytes(qint64 fileSize) { return band(fileSize); }
} // namespace order

namespace preview {

bool isAudioFile(const QString &name)
{
    return mediaTypeForFileName(name) == MediaType::Audio;
}

bool isMediaFile(const QString &name)
{
    const MediaType t = mediaTypeForFileName(name);
    return t == MediaType::Audio || t == MediaType::Video;
}

Index indexKind(const QString &name)
{
    static const QStringList bmff = {QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mov"),
                                     QStringLiteral("3gp"), QStringLiteral("3g2"), QStringLiteral("m4a"),
                                     QStringLiteral("m4b"), QStringLiteral("f4v")};
    const QString s = suffixOf(name);
    if (bmff.contains(s)) return Index::Mp4;
    if (s == QLatin1String("avi") || s == QLatin1String("divx") || s == QLatin1String("xvid")) return Index::AtEnd;
    return Index::None;
}

qint64 neededHead(const QString &name, qint64 size)
{
    if (size <= 0) return 0;
    const qint64 floor = isAudioFile(name) ? 512 * 1024 : 4 * kMiB;
    return qMin(size, qMax(floor, (size + 99) / 100));
}

Mp4Layout scanIsoBmff(qint64 size, const std::function<QByteArray(qint64, qint64)> &read,
                      const std::function<bool(qint64, qint64)> &have)
{
    Mp4Layout out;
    qint64 off = 0;
    qint64 mdatEnd = -1;
    for (int n = 0; n < 256 && off + 8 <= size; ++n) {
        if (!have(off, 16 <= size - off ? 16 : 8)) {
            if (mdatEnd >= 0) {   // after the media data: what follows (moov) is needed
                out.decided = true;
                out.tailFrom = mdatEnd;
            }
            return out;
        }
        const QByteArray h = read(off, 16);
        if (h.size() < 8) return out;
        qint64 len = qint64(be(h, 0, 4));
        const QByteArray type = h.mid(4, 4);
        if (len == 1) {
            if (h.size() < 16) return out;
            len = qint64(be(h, 8, 8));
        } else if (len == 0) {
            len = size - off;
        }
        if (len < 8 || off + len > size + 8) {   // not a box structure we understand: let the player try
            out.decided = true;
            out.tailFrom = 0;
            return out;
        }
        if (type == "moov") {
            out.decided = true;
            out.tailFrom = mdatEnd >= 0 ? mdatEnd : 0;
            return out;
        }
        if (type == "mdat") mdatEnd = qMin(size, off + len);
        off += len;
    }
    out.decided = true;   // no moov at all (or too many boxes): nothing we could wait for
    out.tailFrom = 0;
    return out;
}

Readiness evaluate(const QString &name, qint64 size, qint64 headHeld, qint64 tailHeld, const Mp4Layout &mp4)
{
    Readiness r;
    r.media = isMediaFile(name);
    if (!r.media) {
        r.reason = QObject::tr("Preview: not an audio or video file");
        return r;
    }
    if (size > 0 && headHeld >= size) {   // complete
        r.ready = true;
        return r;
    }
    r.needHead = neededHead(name, size);
    switch (indexKind(name)) {
    case Index::Mp4:
        if (mp4.decided && mp4.tailFrom > 0) r.needTail = size - mp4.tailFrom;
        break;
    case Index::AtEnd:
        r.needTail = order::tailBandBytes(size);
        break;
    case Index::None:
        break;
    }
    if (size <= 0 || headHeld < r.needHead || (indexKind(name) == Index::Mp4 && !mp4.decided)) {
        r.reason = QObject::tr("Preview: needs the beginning of the file");
        return r;
    }
    if (r.needTail > 0 && tailHeld < r.needTail) {
        r.reason = QObject::tr("Preview: needs the end of the file");
        return r;
    }
    r.ready = true;
    return r;
}

} // namespace preview
} // namespace mx
