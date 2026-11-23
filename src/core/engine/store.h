// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX engine: the internal interface of the two stores (ARCHITECTURE "Downloads").
//
// Coordinates: a store has one byte space ("store coordinates"). PartFileStore: the file itself.
// TorrentStore: the torrent's concatenated payload (file i starts at TorrentFile::offset), so a
// RangeSource that serves file i is mapped with base = offset(i).
//
// The Download owns the sources and runs the shared range scheduler over fetchTarget(); the store
// decides what is wanted, how bytes are kept, and verifies them. Verification results come back
// as unitVerified / unitFailed / unitAbandoned with the unit's range so the Download can credit or
// blame the sources that contributed to it.
#pragma once

#include <QtCore/QObject>

#include "core/engine/range_set.h"

namespace mx::engine {

class Store : public QObject {
    Q_OBJECT
public:
    enum Kind { Part = 0, Torrent = 1 };
    using QObject::QObject;
    virtual Kind kind() const = 0;
    virtual qint64 extent() const = 0;                 // store size; -1 = unknown yet
    // Ranges RangeSources should fetch right now (excluding what is held / verified / assembled;
    // in-flight requests are NOT excluded: the Download subtracts them).
    virtual RangeSet fetchTarget() const = 0;
    // Verification unit containing pos (used to align chunks and for rarest-first).
    virtual Range unitAt(qint64 pos) const = 0;
    // True if a unit containing pos can be checked by a trusted fine-grained hash (so untrusted
    // candidate data may go there).
    virtual bool canVerifyUnit(qint64 pos) const = 0;
    // Validated payload for [off, off+bytes). The store keeps only what it still needs and returns
    // the accepted sub-ranges.
    virtual RangeSet deliver(qint64 off, const QByteArray &bytes) = 0;
    virtual qint64 bufferedBytes() const = 0;          // memory held for writes / assemblies
    virtual bool acceptingData() const = 0;
    virtual qint64 verifiedBytes() const = 0;
    virtual qint64 heldBytes() const = 0;              // received (verified or not)
    // [ui-i] The map of the file for the Transfers bar (TransferInfo::pieceMap / pieceFill, ≤ cells):
    // the wanted bytes laid end to end. inFlight = byte ranges (store coordinates) the Download has
    // requested from its sources right now; only those (and what the store or its backend is
    // receiving / checking right now) are "in progress".
    virtual void pieceMap(int cells, const RangeSet &inFlight, QByteArray *state, QByteArray *fill) const = 0;

signals:
    void unitVerified(qint64 start, qint64 end);
    void unitFailed(qint64 start, qint64 end);
    void unitAbandoned(qint64 start, qint64 end);      // no longer needed (e.g. a BT peer delivered it)
    void changed();
    void storeError(const QString &text, bool diskFull);
};

// [ui-i] Condense a map: `spans` (store coordinates) laid end to end form the map axis; complete =
// verified bytes, progress = bytes in progress (complete bytes win). Cell semantics: TransferInfo.
void condensePieceMap(int cells, const QList<Range> &spans, const RangeSet &complete, const RangeSet &progress,
                      QByteArray *state, QByteArray *fill);

} // namespace mx::engine
