// SPDX-License-Identifier: GPL-3.0-or-later
// Gnutella module: descriptors (messages): 23-byte header framing and the payloads a leaf
// needs (Ping, Pong, Bye, Vendor, Push, Query, QueryHit). Parsers are defensive: every length
// and count is bounded and malformed input returns false instead of crashing.
#pragma once

#include "gnut_ggep.h"
#include "gnut_util.h"
#include "mx/types.h"

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QString>

namespace mx::gnut {

constexpr int HeaderSize = 23;
constexpr int MaxPayload = 64 * 1024;           // larger descriptors are dropped (connection closed)

enum MsgType : quint8 {
    MsgPing = 0x00, MsgPong = 0x01, MsgBye = 0x02, MsgQrp = 0x30, MsgVendor = 0x31,
    MsgStdVendor = 0x32, MsgPush = 0x40, MsgQuery = 0x80, MsgQueryHit = 0x81,
};
QString msgTypeName(quint8 t);

struct Message {
    QByteArray guid;          // 16 bytes
    quint8 type = 0;
    quint8 ttl = 1;
    quint8 hops = 0;
    QByteArray payload;
    QByteArray encode() const;                   // header + payload
    static Message make(quint8 type, const QByteArray &payload, quint8 ttl = 1, const QByteArray &guid = QByteArray());
};

// Splits a byte stream into messages. feed() appends; next() pops complete ones. A payload length
// above maxPayload puts the framer into the error state (caller must close the connection).
class Framer {
public:
    explicit Framer(int maxPayload = MaxPayload) : m_max(maxPayload) {}
    void feed(const QByteArray &data) { m_buf.append(data); }
    bool next(Message *out);
    bool error() const { return m_error; }
    QString errorText() const { return m_errorText; }
    int buffered() const { return int(m_buf.size()); }
private:
    QByteArray m_buf;
    int m_max;
    bool m_error = false;
    QString m_errorText;
};

// ---------------------------------------------------------------- Pong
struct Pong {
    quint16 port = 0;
    quint32 ip = 0;
    quint32 files = 0;
    quint32 kbytes = 0;
    GgepBlock ggep;
    QList<HostPort> ipp;          // GGEP "IPP" packed hosts (UHC replies)
    bool ultrapeer = false;       // GGEP "UP" present
    int freeLeafSlots = -1;
    QByteArray vendor;            // GGEP "VC" (4 chars)
    qint64 dailyUptime = -1;      // GGEP "DU"
    QStringList hostCaches;       // GGEP "PHC" (GWC URLs, best effort)
    bool tls = false;             // [enc-1] GGEP "TLS": the pong's host accepts TLS
    QList<HostPort> ippTls;       // [enc-1] the IPP hosts flagged in GGEP "IPP_TLS"
};
bool parsePong(const QByteArray &payload, Pong *out);
QByteArray buildPongPayload(const HostPort &self, quint32 files, quint32 kbytes, const GgepBlock &ggep = GgepBlock());

// Ping payload for a UHC: GGEP "SCP" (we are a leaf, want ultrapeers) + "VC".
QByteArray buildUhcPingPayload();
// [enc-1] TLS bit fields (GGEP "IPP_TLS", "PUSH_TLS", "ALT_TLS"; LimeWire / gtk-gnutella): bit i
// (most significant bit of byte 0 first) set = entry i of the matching host list accepts TLS.
QList<bool> decodeTlsBits(const QByteArray &bits, int count);
QByteArray encodeTlsBits(const QList<bool> &flags);

// ---------------------------------------------------------------- Bye
QByteArray buildByePayload(quint16 code, const QString &reason);
bool parseBye(const QByteArray &payload, quint16 *code, QString *reason);

// ---------------------------------------------------------------- Vendor messages
struct VendorMsg {
    QByteArray vendor;            // 4 bytes ("BEAR", "LIME", "GTKG", or 4 NULs for MessagesSupported)
    quint16 id = 0;
    quint16 version = 0;
    QByteArray body;
};
bool parseVendor(const QByteArray &payload, VendorMsg *out);
QByteArray buildVendorPayload(const QByteArray &vendor, quint16 id, quint16 version, const QByteArray &body = QByteArray());
struct VendorKey {
    QByteArray vendor; quint16 id = 0; quint16 version = 0;
    bool operator==(const VendorKey &o) const { return vendor == o.vendor && id == o.id; }
};
QList<VendorKey> parseMessagesSupported(const QByteArray &body);
QByteArray buildMessagesSupportedBody(const QList<VendorKey> &keys);

// ---------------------------------------------------------------- Push
struct PushMsg {
    QByteArray serventGuid;       // target servent
    quint32 index = 0;
    quint32 ip = 0;               // where the target must connect to
    quint16 port = 0;
    bool tls = false;             // [enc-1] GGEP "TLS": connect back with TLS
};
bool parsePush(const QByteArray &payload, PushMsg *out);
QByteArray buildPushPayload(const PushMsg &p);

// ---------------------------------------------------------------- Query
// "min speed" field flags (new semantics, bit 15 set)
enum QueryFlags : quint16 {
    QfMark = 0x8000, QfFirewalled = 0x4000, QfXml = 0x2000, QfLeafGuided = 0x1000, QfGgepH = 0x0800,
    QfOob = 0x0400, QfFwToFw = 0x0200,
};
struct QueryMsg {
    quint16 flags = 0;
    QString text;                 // search criteria (UTF-8 on the wire)
    QString extendedText;         // GGEP "XQ"
    QList<QByteArray> sha1s;      // urn:sha1 requests
    bool wantsAnyUrn = false;     // bare "urn:" request
    QByteArray xml;
    GgepBlock ggep;
    int metaMask = 0;             // GGEP "M" (LimeWire media type mask)
    bool firewalled() const { return (flags & QfMark) && (flags & QfFirewalled); }
    bool wantsOob() const { return (flags & QfMark) && (flags & QfOob); }
    QString effectiveText() const { return extendedText.isEmpty() ? text : extendedText; }
};
bool parseQuery(const QByteArray &payload, QueryMsg *out);
QByteArray buildQueryPayload(const QueryMsg &q);
// LimeWire "M" mask for a media type of the Search page (0 = any)
int metaMaskForType(mx::MediaType t);

// ---------------------------------------------------------------- QueryHit
struct HitResult {
    quint32 index = 0;
    quint64 size = 0;
    QString name;
    FileHashes hashes;
    QList<HostPort> altLocs;      // GGEP "ALT"
    QList<HostPort> altLocsTls;   // [enc-1] the ALT hosts flagged in GGEP "ALT_TLS"
    QString path;                 // GGEP "PATH" (if any)
    qint64 createTime = -1;       // GGEP "CT"
    int bitrateKbps = -1;         // from XML
    int durationSec = -1;
    int sampleRateHz = -1;
};
struct QueryHit {
    quint16 port = 0;
    quint32 ip = 0;
    quint32 speed = 0;
    QList<HitResult> results;
    QByteArray vendor;            // 4 chars (empty if no QHD)
    bool hasQhd = false;
    // flags; *Known = the "meaningful" bit was set
    bool push = false, pushKnown = false;
    bool busy = false, busyKnown = false;
    bool uploaded = false, uploadedKnown = false;
    bool measured = false, measuredKnown = false;
    bool ggepInTrailer = false;
    QList<HostPort> pushProxies;  // trailer GGEP "PUSH"
    QList<HostPort> pushProxiesTls;   // [enc-1] flagged in trailer GGEP "PUSH_TLS"
    bool browseHost = false;      // trailer GGEP "BH"
    bool tls = false;
    QByteArray xml;               // decoded XML block (may be empty)
    QByteArray serventGuid;       // last 16 bytes
};
// Parse a QueryHit payload (counts bounded to 255 results, names to 1024 bytes).
bool parseQueryHit(const QByteArray &payload, QueryHit *out, QString *error = nullptr);
// Build a QueryHit payload from results (at most 255; caller keeps messages small).
struct HitTrailer {
    QByteArray vendor = QByteArray(VendorCode, 4);
    bool firewalled = false;
    bool busy = false;
    bool uploaded = false;
    bool browseHost = true;
    QList<HostPort> pushProxies;
    QByteArray serventGuid;
    bool tls = false;             // [enc-1] GGEP "TLS": we accept TLS connections
};
QByteArray buildQueryHitPayload(const HostPort &self, quint32 speedKbps, const QList<HitResult> &results,
                                const HitTrailer &t);

// XML metadata (best effort): applies bitrate/seconds/sampleRate attributes of
// <audio index="N" .../> / <video .../> elements to results[N].
void applyXmlMetadata(const QByteArray &xml, QList<HitResult> *results);

} // namespace mx::gnut
