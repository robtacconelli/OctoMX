// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/media_info.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace mx {

namespace {

quint32 be32(const uchar *p) { return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | p[3]; }
quint32 le32(const uchar *p) { return (quint32(p[3]) << 24) | (quint32(p[2]) << 16) | (quint32(p[1]) << 8) | p[0]; }

struct FrameHeader {
    int version = 0;     // 1 = MPEG1, 2 = MPEG2, 25 = MPEG2.5
    int layer = 0;       // 1..3
    int bitrate = 0;     // kbps
    int sampleRate = 0;
    int padding = 0;
    int channelMode = 0; // 3 = mono
    int frameLength = 0; // bytes
    int samplesPerFrame = 0;
};

bool parseHeader(quint32 h, FrameHeader *out)
{
    if ((h & 0xFFE00000u) != 0xFFE00000u) return false;
    const int verBits = (h >> 19) & 3;
    const int layerBits = (h >> 17) & 3;
    const int brIdx = (h >> 12) & 15;
    const int srIdx = (h >> 10) & 3;
    if (verBits == 1 || layerBits == 0 || brIdx == 0 || brIdx == 15 || srIdx == 3) return false;
    FrameHeader f;
    f.version = verBits == 3 ? 1 : verBits == 2 ? 2 : 25;
    f.layer = 4 - layerBits;
    f.padding = (h >> 9) & 1;
    f.channelMode = (h >> 6) & 3;
    static const int br[5][16] = {
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0}, // V1 L1
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},    // V1 L2
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},     // V1 L3
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},    // V2 L1
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},         // V2 L2/L3
    };
    static const int sr[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
    const int table = f.version == 1 ? f.layer - 1 : (f.layer == 1 ? 3 : 4);
    f.bitrate = br[table][brIdx];
    f.sampleRate = sr[f.version == 1 ? 0 : f.version == 2 ? 1 : 2][srIdx];
    if (f.layer == 1) {
        f.samplesPerFrame = 384;
        f.frameLength = (12 * f.bitrate * 1000 / f.sampleRate + f.padding) * 4;
    } else if (f.layer == 2 || f.version == 1) {
        f.samplesPerFrame = 1152;
        f.frameLength = 144 * f.bitrate * 1000 / f.sampleRate + f.padding;
    } else {
        f.samplesPerFrame = 576;
        f.frameLength = 72 * f.bitrate * 1000 / f.sampleRate + f.padding;
    }
    if (f.frameLength < 21) return false;
    *out = f;
    return true;
}

MediaInfo parseWav(const QByteArray &head, qint64 fileSize)
{
    MediaInfo mi;
    const uchar *p = reinterpret_cast<const uchar *>(head.constData());
    const int n = head.size();
    int pos = 12;
    quint32 byteRate = 0, sampleRate = 0;
    while (pos + 8 <= n) {
        const QByteArray id = head.mid(pos, 4);
        const quint32 len = le32(p + pos + 4);
        if (id == "fmt " && pos + 8 + 16 <= n) {
            sampleRate = le32(p + pos + 12);
            byteRate = le32(p + pos + 16);
        } else if (id == "data") {
            qint64 dataLen = len;
            if (dataLen == 0 || dataLen > fileSize) dataLen = fileSize - pos - 8;
            if (byteRate > 0) mi.durationSec = int(dataLen / byteRate);
            break;
        }
        pos += 8 + int(len) + (len & 1);
    }
    if (sampleRate) mi.sampleRateHz = int(sampleRate);
    if (byteRate) mi.bitrateKbps = int(qint64(byteRate) * 8 / 1000);
    return mi;
}

MediaInfo parseFlac(const QByteArray &head, qint64 fileSize)
{
    MediaInfo mi;
    int pos = 4; // after "fLaC"
    const uchar *p = reinterpret_cast<const uchar *>(head.constData());
    if (head.size() < pos + 4 + 18) return mi;
    if ((p[pos] & 0x7F) != 0) return mi; // STREAMINFO must be first
    const uchar *s = p + pos + 4;
    const quint32 rate = (quint32(s[10]) << 12) | (quint32(s[11]) << 4) | (s[12] >> 4);
    const quint64 total = (quint64(s[13] & 0x0F) << 32) | be32(s + 14);
    if (rate) {
        mi.sampleRateHz = int(rate);
        if (total) {
            const double secs = double(total) / rate;
            mi.durationSec = int(secs + 0.5);
            if (secs > 0) mi.bitrateKbps = int(fileSize * 8 / secs / 1000);
        }
    }
    return mi;
}

// The serial number of the Ogg page that contains byte offset `pos` (the page header before it).
qint64 oggSerialAt(const QByteArray &buf, int pos)
{
    const int page = buf.lastIndexOf("OggS", pos);
    if (page < 0 || page + 18 > buf.size()) return -1;
    return qint64(le32(reinterpret_cast<const uchar *>(buf.constData()) + page + 14));
}

MediaInfo parseOgg(const QByteArray &head, const QByteArray &tail, qint64 fileSize)
{
    MediaInfo mi;
    quint32 rate = 0;
    int nominal = 0;
    bool opus = false;
    qint64 serial = -1;
    const int v = head.indexOf("\x01vorbis");
    if (v >= 0 && v + 7 + 4 + 1 + 4 + 4 + 4 + 4 <= head.size()) {
        const uchar *p = reinterpret_cast<const uchar *>(head.constData()) + v + 7;
        rate = le32(p + 5);
        nominal = int(le32(p + 13));
        serial = oggSerialAt(head, v);
    } else {
        const int o = head.indexOf("OpusHead");
        if (o >= 0 && o + 16 <= head.size()) {
            opus = true;
            rate = 48000; // granule positions are always 48 kHz for Opus
            serial = oggSerialAt(head, o);
        }
    }
    if (!rate || rate > 768000) return mi;
    mi.sampleRateHz = opus ? 48000 : int(rate);
    // An Ogg file with a video stream (Theora, Dirac, VP8, a Skeleton index) is a video in an audio
    // container: the file size says nothing about the audio bitrate, so only the length is kept.
    const bool video = head.contains("\x80theora") || head.contains("BBCD") || head.contains("OVP80")
                       || head.contains("fishead");
    // Granule position of the last page of the AUDIO stream: other streams (video) count their
    // granules differently (Theora packs keyframe numbers into it), which gave absurd lengths.
    const uchar *t = reinterpret_cast<const uchar *>(tail.constData());
    for (int last = tail.lastIndexOf("OggS"); last >= 0; last = last > 0 ? tail.lastIndexOf("OggS", last - 1) : -1) {
        if (last + 18 > tail.size()) continue;
        if (serial >= 0 && qint64(le32(t + last + 14)) != serial) continue;
        const quint64 gran = quint64(le32(t + last + 6)) | (quint64(le32(t + last + 10)) << 32);
        if (gran == 0 || gran == ~quint64(0)) continue;   // -1: no packet ends on this page
        if (gran < (quint64(1) << 50)) {
            const double secs = double(gran) / rate;
            if (secs >= 1 && secs < 7 * 24 * 3600) {
                mi.durationSec = int(secs + 0.5);
                if (!video) mi.bitrateKbps = int(fileSize * 8 / secs / 1000);
            }
        }
        break;
    }
    if (mi.bitrateKbps < 0 && nominal > 0 && !video) mi.bitrateKbps = nominal / 1000;
    return mi;
}

// Values outside what an audio file can have are a parse error, not metadata: drop them.
MediaInfo plausible(MediaInfo mi)
{
    if (mi.durationSec <= 0 || mi.durationSec > 7 * 24 * 3600) mi.durationSec = -1;
    if (mi.bitrateKbps < 8 || mi.bitrateKbps > 9216) mi.bitrateKbps = -1;   // 9216 = 24 bit / 192 kHz stereo PCM
    if (mi.sampleRateHz < 4000 || mi.sampleRateHz > 768000) mi.sampleRateHz = -1;
    return mi;
}

} // namespace

MediaInfo parseMp3(const QByteArray &head, qint64 fileSize, bool hasId3v1)
{
    MediaInfo mi;
    const uchar *p = reinterpret_cast<const uchar *>(head.constData());
    const int n = head.size();
    qint64 pos = 0;
    // ID3v2
    if (n >= 10 && p[0] == 'I' && p[1] == 'D' && p[2] == '3') {
        const qint64 sz = (qint64(p[6] & 0x7F) << 21) | ((p[7] & 0x7F) << 14) | ((p[8] & 0x7F) << 7) | (p[9] & 0x7F);
        pos = 10 + sz + ((p[5] & 0x10) ? 10 : 0);
    }
    // find two consecutive valid frames
    FrameHeader f;
    qint64 start = -1;
    for (qint64 i = pos; i + 4 <= n && i < pos + 128 * 1024; ++i) {
        if (p[i] != 0xFF || (p[i + 1] & 0xE0) != 0xE0) continue;
        if (!parseHeader(be32(p + i), &f)) continue;
        const qint64 next = i + f.frameLength;
        FrameHeader g;
        if (next + 4 <= n) {
            if (!parseHeader(be32(p + next), &g) || g.sampleRate != f.sampleRate || g.layer != f.layer) continue;
        } else if (next != fileSize) {
            continue; // can't confirm
        }
        start = i;
        break;
    }
    if (start < 0) return mi;
    mi.sampleRateHz = f.sampleRate;
    const qint64 audioBytes = fileSize - start - (hasId3v1 ? 128 : 0);

    // Xing / Info
    const bool mono = f.channelMode == 3;
    const int sideInfo = f.version == 1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    const qint64 x = start + 4 + sideInfo;
    qint64 frames = -1, vbrBytes = -1;
    if (x + 16 <= n && (memcmp(p + x, "Xing", 4) == 0 || memcmp(p + x, "Info", 4) == 0)) {
        const quint32 flags = be32(p + x + 4);
        qint64 o = x + 8;
        if (flags & 1) { frames = be32(p + o); o += 4; }
        if (flags & 2) { vbrBytes = be32(p + o); o += 4; }
    } else {
        const qint64 v = start + 4 + 32;
        if (v + 18 <= n && memcmp(p + v, "VBRI", 4) == 0) {
            vbrBytes = be32(p + v + 10);
            frames = be32(p + v + 14);
        }
    }
    if (frames > 0) {
        const double secs = double(frames) * f.samplesPerFrame / f.sampleRate;
        mi.durationSec = int(secs + 0.5);
        const qint64 bytes = vbrBytes > 0 ? vbrBytes : audioBytes;
        if (secs > 0) mi.bitrateKbps = int(bytes * 8 / secs / 1000 + 0.5);
    } else {
        mi.bitrateKbps = f.bitrate;
        if (f.bitrate > 0) mi.durationSec = int(audioBytes * 8 / (qint64(f.bitrate) * 1000));
    }
    return mi;
}

namespace {
MediaInfo readMediaInfoRaw(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const qint64 size = f.size();
    const QString ext = QFileInfo(path).suffix().toLower();
    QByteArray head = f.read(256 * 1024);
    if (head.size() < 16) return {};
    if (head.startsWith("RIFF") && head.mid(8, 4) == "WAVE") return parseWav(head, size);
    if (head.startsWith("fLaC")) return parseFlac(head, size);
    if (head.startsWith("OggS")) {
        QByteArray tail;
        if (size > 65536) {
            f.seek(size - 65536);
            tail = f.read(65536);
        } else {
            tail = head;
        }
        return parseOgg(head, tail, size);
    }
    if (ext == QLatin1String("mp3") || ext == QLatin1String("mp2") || ext == QLatin1String("mpa")
        || head.startsWith("ID3")) {
        // a large ID3v2 tag (embedded cover art) may push the first frame beyond the buffer
        const uchar *p = reinterpret_cast<const uchar *>(head.constData());
        if (head.startsWith("ID3")) {
            const qint64 sz = (qint64(p[6] & 0x7F) << 21) | ((p[7] & 0x7F) << 14) | ((p[8] & 0x7F) << 7) | (p[9] & 0x7F);
            if (sz + 10 + 64 * 1024 > head.size() && sz < size) {
                // re-read from the tag end, then prepend a fake empty tag header so offsets work
                const qint64 skip = 10 + sz + ((p[5] & 0x10) ? 10 : 0);
                f.seek(skip);
                QByteArray rest = f.read(256 * 1024);
                bool v1 = false;
                if (size >= 128) { f.seek(size - 128); v1 = f.read(3) == "TAG"; }
                return parseMp3(rest, size - skip, v1);
            }
        }
        bool v1 = false;
        if (size >= 128) { f.seek(size - 128); v1 = f.read(3) == "TAG"; }
        return parseMp3(head, size, v1);
    }
    return {};
}
} // namespace

MediaInfo readMediaInfo(const QString &path)
{
    return plausible(readMediaInfoRaw(path));
}

void dropNonAudioMedia(MediaType type, int *durationSec, int *bitrateKbps, int *sampleRateHz)
{
    if (type == MediaType::Audio) return;
    if (durationSec) *durationSec = -1;
    if (bitrateKbps) *bitrateKbps = -1;
    if (sampleRateHz) *sampleRateHz = -1;
}

MediaInfo parseOggForTest(const QByteArray &head, const QByteArray &tail, qint64 fileSize)
{
    return plausible(parseOgg(head, tail, fileSize));
}

} // namespace mx
