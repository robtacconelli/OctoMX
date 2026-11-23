// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnut_thex.h"

#include "mxhash/encoding.h"
#include "mxhash/tiger_tree.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QString>
#include <algorithm>

namespace mx::gnut {

static const char kTreeType[] = "http://open-content.net/spec/thex/breadthfirst";

static int pad4(int n) { return (n + 3) & ~3; }

static void putBe16(QByteArray &b, int v) { b.append(char((v >> 8) & 0xff)); b.append(char(v & 0xff)); }
static void putBe32(QByteArray &b, quint32 v)
{
    for (int i = 3; i >= 0; --i) b.append(char((v >> (8 * i)) & 0xff));
}
static void appendPadded(QByteArray &b, const QByteArray &d)
{
    b.append(d);
    b.append(QByteArray(pad4(int(d.size())) - int(d.size()), '\0'));
}

QByteArray encodeDime(const QList<DimeRecord> &records)
{
    QByteArray out;
    for (int i = 0; i < records.size(); ++i) {
        const DimeRecord &r = records[i];
        quint8 b0 = 0x01 << 3;
        if (i == 0) b0 |= 0x04;                       // MB
        if (i == records.size() - 1) b0 |= 0x02;      // ME
        out.append(char(b0));
        out.append(char((r.typeFormat & 0x0f) << 4));
        putBe16(out, 0);
        putBe16(out, int(r.id.size()));
        putBe16(out, int(r.type.size()));
        putBe32(out, quint32(r.data.size()));
        appendPadded(out, r.id);
        appendPadded(out, r.type);
        appendPadded(out, r.data);
    }
    return out;
}

bool parseDime(const QByteArray &msg, QList<DimeRecord> *out, int maxRecords)
{
    int p = 0;
    const int n = int(msg.size());
    while (p < n && out->size() < maxRecords) {
        if (p + 12 > n) return false;
        const auto *h = reinterpret_cast<const quint8 *>(msg.constData() + p);
        if ((h[0] >> 3) != 1) return false;
        const bool me = h[0] & 0x02;
        if (h[0] & 0x01) return false;                // chunked records unsupported
        const int optLen = (h[2] << 8) | h[3];
        const int idLen = (h[4] << 8) | h[5];
        const int typeLen = (h[6] << 8) | h[7];
        const quint32 dataLen = (quint32(h[8]) << 24) | (quint32(h[9]) << 16) | (quint32(h[10]) << 8) | h[11];
        if (dataLen > 16u * 1024 * 1024) return false;
        DimeRecord r;
        r.typeFormat = h[1] >> 4;
        p += 12;
        const qint64 need = qint64(pad4(optLen)) + pad4(idLen) + pad4(typeLen) + pad4(int(dataLen));
        if (p + need > n + 3) return false;          // tolerate a missing final pad
        p += pad4(optLen);
        r.id = msg.mid(p, idLen); p += pad4(idLen);
        r.type = msg.mid(p, typeLen); p += pad4(typeLen);
        if (p + qint64(dataLen) > n) return false;
        r.data = msg.mid(p, int(dataLen)); p += pad4(int(dataLen));
        *out << r;
        if (me) return true;
    }
    return !out->isEmpty();
}

QList<qint64> thexLevelCounts(qint64 size)
{
    QList<qint64> up;
    qint64 c = qMax<qint64>(1, (size + 1023) / 1024);
    up << c;
    while (c > 1) { c = (c + 1) / 2; up << c; }
    std::reverse(up.begin(), up.end());
    return up;
}

int thexGoodDepth(qint64 size)
{
    static const qint64 thresholds[] = {256 * 1024LL, 512 * 1024LL, 1024 * 1024LL, 2 * 1024 * 1024LL,
                                        4 * 1024 * 1024LL, 8 * 1024 * 1024LL, 16 * 1024 * 1024LL,
                                        32 * 1024 * 1024LL, 64 * 1024 * 1024LL, 256 * 1024 * 1024LL,
                                        1024 * 1024 * 1024LL};
    int d = 0;
    for (qint64 t : thresholds) { if (size < t) break; ++d; }
    const int height = int(thexLevelCounts(size).size()) - 1;
    return qMin(d, height);
}

QByteArray buildThex(qint64 fileSize, const QList<QList<QByteArray>> &levels)
{
    QByteArray tree;
    for (const auto &l : levels)
        for (const QByteArray &n : l) tree.append(n);
    const QByteArray root = levels.isEmpty() || levels[0].isEmpty() ? QByteArray() : levels[0][0];
    const QByteArray uuidSrc = QCryptographicHash::hash(root, QCryptographicHash::Sha1).toHex();
    const QByteArray uuid = "uuid:" + uuidSrc.mid(0, 8) + '-' + uuidSrc.mid(8, 4) + '-' + uuidSrc.mid(12, 4) + '-' +
                            uuidSrc.mid(16, 4) + '-' + uuidSrc.mid(20, 12);
    const QByteArray xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<!DOCTYPE hashtree SYSTEM \"http://open-content.net/spec/thex/thex.dtd\">\r\n"
        "<hashtree>\r\n"
        "<file size=\"" + QByteArray::number(fileSize) + "\" segmentsize=\"1024\"/>\r\n"
        "<digest algorithm=\"http://open-content.net/spec/digest/tiger\" outputsize=\"24\"/>\r\n"
        "<serializedtree depth=\"" + QByteArray::number(qMax(0, int(levels.size()) - 1)) +
        "\" type=\"" + kTreeType + "\" uri=\"" + uuid + "\"/>\r\n"
        "</hashtree>\r\n";
    DimeRecord rx;
    rx.type = "text/xml";
    rx.data = xml;
    rx.typeFormat = 1;
    DimeRecord rt;
    rt.type = kTreeType;
    rt.id = uuid;
    rt.data = tree;
    rt.typeFormat = 2;
    return encodeDime({rx, rt});
}

bool parseThex(const QByteArray &dime, qint64 fileSize, const QByteArray &expectedRoot, ThexTree *out, QString *error)
{
    const auto fail = [&](const QString &e) { if (error) *error = e; return false; };
    QList<DimeRecord> recs;
    if (!parseDime(dime, &recs)) return fail(QStringLiteral("bad DIME message"));
    const DimeRecord *tree = nullptr;
    for (const DimeRecord &r : recs)
        if (r.type.contains("breadthfirst")) { tree = &r; break; }
    if (!tree && recs.size() >= 2) tree = &recs[1];
    if (!tree) return fail(QStringLiteral("no tree record"));
    if (tree->data.size() % 24) return fail(QStringLiteral("tree size not a multiple of 24"));
    const qint64 nodes = tree->data.size() / 24;
    const QList<qint64> counts = thexLevelCounts(fileSize);
    qint64 sum = 0;
    int depth = -1;
    for (int i = 0; i < counts.size(); ++i) {
        sum += counts[i];
        if (sum == nodes) { depth = i; break; }
        if (sum > nodes) break;
    }
    if (depth < 0) return fail(QStringLiteral("tree has %1 nodes, not a whole number of levels").arg(nodes));
    QList<QList<QByteArray>> levels;
    int p = 0;
    for (int i = 0; i <= depth; ++i) {
        QList<QByteArray> l;
        for (qint64 k = 0; k < counts[i]; ++k, p += 24) l << tree->data.mid(p, 24);
        levels << l;
    }
    for (int i = depth; i >= 1; --i)
        if (mx::hash::TigerTree::foldLevel(levels[i]) != levels[i - 1])
            return fail(QStringLiteral("level %1 does not fold into level %2").arg(i).arg(i - 1));
    if (!expectedRoot.isEmpty() && levels[0][0] != expectedRoot) return fail(QStringLiteral("root mismatch"));
    const int height = int(counts.size()) - 1;
    out->depth = depth;
    out->deepest = levels[depth];
    out->unitSize = qint64(1024) << (height - depth);
    out->root = levels[0][0];
    return true;
}

} // namespace mx::gnut
