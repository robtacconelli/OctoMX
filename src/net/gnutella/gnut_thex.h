// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: THEX tiger-tree exchange: DIME messages with the XML description and the
// breadth-first serialized tree (as served at /uri-res/N2X?urn:sha1:... / X-Thex-URI).
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>

namespace mx::gnut {

struct DimeRecord {
    QByteArray type;          // "text/xml" or the tree type URI
    QByteArray id;
    QByteArray data;
    int typeFormat = 1;       // 1 = MIME media type, 2 = absolute URI
};
QByteArray encodeDime(const QList<DimeRecord> &records);
bool parseDime(const QByteArray &msg, QList<DimeRecord> *out, int maxRecords = 8);

// Number of nodes on each level (root first) for a file of `size` bytes, with 1 KiB leaves.
QList<qint64> thexLevelCounts(qint64 size);
// Depth gtk-gnutella serves for this size (deepest level ≈ 2^depth nodes), capped by the tree.
int thexGoodDepth(qint64 size);

// Build a THEX DIME message. `levels` = level 0 (root) ... level `depth` (each list of 24-byte nodes).
QByteArray buildThex(qint64 fileSize, const QList<QList<QByteArray>> &levels);

struct ThexTree {
    int depth = 0;                    // index of the deepest level received
    QList<QByteArray> deepest;        // nodes of that level
    qint64 unitSize = 0;              // bytes covered by each deepest node (1024 << (height - depth))
    QByteArray root;
};
// Parse + verify a THEX reply against the known file size and TTH root (if non-empty): every
// level must fold into the one above and the root must match. Breadth-first trees only.
bool parseThex(const QByteArray &dime, qint64 fileSize, const QByteArray &expectedRoot, ThexTree *out,
               QString *error = nullptr);

} // namespace mx::gnut
