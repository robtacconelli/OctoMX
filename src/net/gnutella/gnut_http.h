// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: HTTP header values used by Gnutella transfers: Range / Content-Range,
// X-Available-Ranges, X-Queue / X-Queued (LimeWire queue + PARQ), Retry-After, alt-locs
// (X-Alt / X-Nalt / X-Falt).
#pragma once

#include "gnut_util.h"

#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QString>

namespace mx::gnut {

using Span = QPair<qint64, qint64>;            // half-open [first, second)

// "bytes=0-99,200-,-50" → spans clipped to [0,size). Returns false if syntactically invalid or
// nothing satisfiable.
bool parseRangeHeader(const QString &v, qint64 size, QList<Span> *out);
// "bytes 0-99/1000" (also "bytes=0-99/1000", "bytes 0-99/*"). total = -1 if '*'.
bool parseContentRange(const QString &v, qint64 *first, qint64 *lastInclusive, qint64 *total);
// "bytes 0-1023, 4096-8191" (inclusive ends) → merged half-open spans within [0,size)
QList<Span> parseAvailableRanges(const QString &v, qint64 size);
QString formatAvailableRanges(const QList<Span> &spans);
QList<Span> mergeSpans(QList<Span> spans);

struct QueueInfo {
    bool present = false;
    int position = -1, length = -1, limit = -1;
    int pollMin = -1, pollMax = -1;           // seconds
    QString id;                               // PARQ ID
    int lifetime = -1;                        // PARQ: re-request within this many seconds
    int eta = -1;
    QString rowId;                            // [glue-3] upload row id of our queue entry (serving side)
};
// LimeWire style "X-Queue: position=2, length=5, limit=4, pollMin=45, pollMax=120"
// and PARQ "X-Queued: ID=..; position=..; lifetime=..; length=..; ETA=.."; merged into *q.
void parseXQueue(const QString &v, QueueInfo *q);
void parseXQueued(const QString &v, QueueInfo *q);
// "120" or an HTTP date → seconds from now (-1 if unparsable); clamped to [0, 86400].
int parseRetryAfter(const QString &v);

// Alternate locations: "1.2.3.4:6346, 5.6.7.8" (port defaults to 6346); some servents send
// "http://ip:port/uri-res/N2R?urn:sha1:..." URLs, the host part is taken.
// [enc-1] a leading "tls=<hex>" item (LimeWire) flags which of the following hosts take TLS;
// they are added to *tls when given.
QList<HostPort> parseAltLocs(const QString &v, int max = 100, QList<HostPort> *tls = nullptr);
QString formatAltLocs(const QList<HostPort> &hosts);
// Firewalled alt-locs (X-Falt, LimeWire style): "<guid hex|base32>;pptls=..;ip:port;ip:port, ..."
struct PushLoc {
    QByteArray guid;                          // 16 bytes
    QList<HostPort> proxies;
};
QList<PushLoc> parsePushAltLocs(const QString &v, int max = 50);
QString formatPushAltLoc(const PushLoc &p);

} // namespace mx::gnut
