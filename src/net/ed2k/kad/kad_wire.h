// SPDX-License-Identifier: GPL-3.0-or-later
// Kad2 wire format: opcodes, tag names, packet framing (0xE4 / zlib-packed 0xE5), message codecs,
// and eMule's UDP obfuscation for Kad and eD2k client datagrams. Pure functions, no sockets: every
// decoder takes hostile bytes and either fills its struct or returns false.
//
// Byte orders: Kad writes IPv4 addresses as little-endian uint32 of the *host-order* address
// (1.2.3.4 -> 0x01020304 -> bytes 04 03 02 01). The eD2k part of the module uses "eD2k order"
// (network-order bytes read as little-endian). kadIpFromEd2k()/kadIpToEd2k() convert.
#pragma once

#include "kad_id.h"

#include "../ed2k_io.h"
#include "../ed2k_tags.h"

#include <QtCore/QList>

namespace mx::ed2k::kad {

// ---------------------------------------------------------------- versions / limits
constexpr quint8 KADEMLIA_VERSION = 0x09;           // 0.50a level: obfuscation, FIREWALLED2, HELLO_RES_ACK, AICH on keywords
constexpr quint8 KADEMLIA_VERSION5_48a = 0x05;
constexpr quint8 KADEMLIA_VERSION6_49aBETA = 0x06;  // obfuscation, sender/receiver keys
constexpr quint8 KADEMLIA_VERSION7_49a = 0x07;      // FIREWALLED2_REQ, OP_KAD_FWTCPCHECK_ACK
constexpr quint8 KADEMLIA_VERSION8_49b = 0x08;      // TAG_KADMISCOPTIONS, HELLO_RES_ACK
constexpr quint8 KADEMLIA_VERSION9_50a = 0x09;      // AICH hashes on keyword storage
constexpr int UDP_KAD_MAXFRAGMENT = 1420;
constexpr int KAD_MAX_DATAGRAM = 8192;              // we never accept bigger (eMule's receive buffer is 5000)
constexpr int KAD_MAX_UNPACKED = 65536;

constexpr int K = 10;                 // bucket size
constexpr int KBASE = 4;
constexpr int KK = 5;
constexpr int ALPHA_QUERY = 3;
constexpr int LOG_BASE_EXPONENT = 5;
constexpr quint32 SEARCHTOLERANCE = 16777216;

enum : quint8 {
    KADEMLIA2_BOOTSTRAP_REQ = 0x01,
    KADEMLIA2_BOOTSTRAP_RES = 0x09,
    KADEMLIA2_HELLO_REQ = 0x11,
    KADEMLIA2_HELLO_RES = 0x19,
    KADEMLIA2_REQ = 0x21,
    KADEMLIA2_HELLO_RES_ACK = 0x22,
    KADEMLIA2_RES = 0x29,
    KADEMLIA2_SEARCH_KEY_REQ = 0x33,
    KADEMLIA2_SEARCH_SOURCE_REQ = 0x34,
    KADEMLIA2_SEARCH_NOTES_REQ = 0x35,
    KADEMLIA2_SEARCH_RES = 0x3B,
    KADEMLIA2_PUBLISH_KEY_REQ = 0x43,
    KADEMLIA2_PUBLISH_SOURCE_REQ = 0x44,
    KADEMLIA2_PUBLISH_NOTES_REQ = 0x45,
    KADEMLIA2_PUBLISH_RES = 0x4B,
    KADEMLIA2_PUBLISH_RES_ACK = 0x4C,
    KADEMLIA_FIREWALLED_REQ = 0x50,
    KADEMLIA_FINDBUDDY_REQ = 0x51,
    KADEMLIA_CALLBACK_REQ = 0x52,
    KADEMLIA_FIREWALLED2_REQ = 0x53,
    KADEMLIA_FIREWALLED_RES = 0x58,
    KADEMLIA_FIREWALLED_ACK_RES = 0x59,
    KADEMLIA_FINDBUDDY_RES = 0x5A,
    KADEMLIA2_PING = 0x60,
    KADEMLIA2_PONG = 0x61,
    KADEMLIA2_FIREWALLUDP = 0x62,
};

// KADEMLIA2_REQ "type" = number of contacts wanted
enum : quint8 { KADEMLIA_FIND_VALUE = 0x02, KADEMLIA_STORE = 0x04, KADEMLIA_FIND_NODE = 0x0B, KADEMLIA_FIND_VALUE_MORE = 0x0B };

// Kad tag names (one-byte names)
enum : quint8 {
    TAG_FILENAME = 0x01, TAG_FILESIZE = 0x02, TAG_FILETYPE = 0x03, TAG_FILEFORMAT = 0x04,
    TAG_DESCRIPTION = 0x0B, TAG_SOURCES = 0x15, TAG_COMPLETE_SOURCES = 0x30, TAG_PUBLISHINFO = 0x33,
    TAG_KADAICHHASHPUB = 0x36, TAG_KADAICHHASHRESULT = 0x37,
    TAG_MEDIA_ARTIST = 0xD0, TAG_MEDIA_ALBUM = 0xD1, TAG_MEDIA_TITLE = 0xD2, TAG_MEDIA_LENGTH = 0xD3,
    TAG_MEDIA_BITRATE = 0xD4, TAG_MEDIA_CODEC = 0xD5,
    TAG_KADMISCOPTIONS = 0xF2, TAG_ENCRYPTION = 0xF3, TAG_USER_COUNT = 0xF4, TAG_FILE_COUNT = 0xF5,
    TAG_FILECOMMENT = 0xF6, TAG_FILERATING = 0xF7, TAG_BUDDYHASH = 0xF8, TAG_CLIENTLOWID = 0xF9,
    TAG_SERVERPORT = 0xFA, TAG_SERVERIP = 0xFB, TAG_SOURCEUPORT = 0xFC, TAG_SOURCEPORT = 0xFD,
    TAG_SOURCEIP = 0xFE, TAG_SOURCETYPE = 0xFF,
};

inline quint32 kadIpFromEd2k(quint32 ed2kIp) { return ((ed2kIp & 0xFF) << 24) | ((ed2kIp & 0xFF00) << 8) | ((ed2kIp >> 8) & 0xFF00) | (ed2kIp >> 24); }
inline quint32 kadIpToEd2k(quint32 hostIp) { return kadIpFromEd2k(hostIp); }   // byte swap is its own inverse
QString kadIpToString(quint32 hostIp);
bool isGoodKadIpPort(quint32 hostIp, quint16 port, bool allowLan);   // eMule IsGoodIPPort (+ LAN switch)
bool isLanIp(quint32 hostIp);

// ---------------------------------------------------------------- tags
void writeKadTag(Writer &w, const Tag &t);                // old-style name, ints shrunk to the smallest type
void writeKadTagList(Writer &w, const TagList &tags);     // <count 1><tags>
bool readKadTagList(Reader &r, TagList *out, int maxTags = 64);
const Tag *kadTag(const TagList &tags, quint8 id);
quint64 kadTagInt(const TagList &tags, quint8 id, quint64 def = 0);
QString kadTagStr(const TagList &tags, quint8 id);
// TAG_FILESIZE as int, or as 8-byte BSOB (files > 4 GB)
quint64 kadTagFileSize(const TagList &tags);

// ---------------------------------------------------------------- framing
// <0xE4><opcode><payload>; payloads > 200 bytes are zlib-packed (0xE5) when that is smaller.
QByteArray kadFrame(quint8 opcode, const QByteArray &payload, bool allowPack = true);
// Plain (already de-obfuscated) datagram -> opcode + payload. Packed payloads are inflated with a
// hard limit. False for anything that is not a well-formed Kad datagram.
bool kadUnframe(const QByteArray &datagram, quint8 *opcode, QByteArray *payload);

// ---------------------------------------------------------------- messages
struct KadPeer {
    KadId id;
    quint32 ip = 0;          // host order
    quint16 udpPort = 0, tcpPort = 0;
    quint8 version = 0;
};
void writePeer(Writer &w, const KadPeer &p);
KadPeer readPeer(Reader &r);

struct HelloMsg {            // KADEMLIA2_HELLO_REQ / KADEMLIA2_HELLO_RES
    KadId id;
    quint16 tcpPort = 0;
    quint8 version = 0;
    quint16 udpPortTag = 0;  // TAG_SOURCEUPORT (0 = absent: use the packet's source port)
    bool hasMisc = false;    // TAG_KADMISCOPTIONS
    quint8 misc = 0;         // bit0 UDP firewalled, bit1 TCP firewalled, bit2 requests HELLO_RES_ACK
};
QByteArray encodeHello(const HelloMsg &m);
bool decodeHello(const QByteArray &p, HelloMsg *m);

struct BootstrapRes {
    KadId id;
    quint16 tcpPort = 0;
    quint8 version = 0;
    QList<KadPeer> contacts;
};
QByteArray encodeBootstrapRes(const BootstrapRes &m);
bool decodeBootstrapRes(const QByteArray &p, BootstrapRes *m);

struct KadReq { quint8 type = KADEMLIA_FIND_NODE; KadId target; KadId receiver; };
QByteArray encodeKadReq(const KadReq &m);
bool decodeKadReq(const QByteArray &p, KadReq *m);

struct KadRes { KadId target; QList<KadPeer> contacts; };
QByteArray encodeKadRes(const KadRes &m);
bool decodeKadRes(const QByteArray &p, KadRes *m);   // exact size check, ≤ 32 contacts

struct SearchKeyReq { KadId target; quint16 start = 0; QByteArray tree; };   // tree non-empty = restrictive
QByteArray encodeSearchKeyReq(const SearchKeyReq &m);
bool decodeSearchKeyReq(const QByteArray &p, SearchKeyReq *m);

struct SearchSourceReq { KadId target; quint16 start = 0; quint64 size = 0; };
QByteArray encodeSearchSourceReq(const SearchSourceReq &m);
bool decodeSearchSourceReq(const QByteArray &p, SearchSourceReq *m);

struct SearchResEntry { KadId answer; TagList tags; };
struct SearchRes { KadId sender; KadId target; QList<SearchResEntry> results; };
QByteArray encodeSearchRes(const SearchRes &m);
bool decodeSearchRes(const QByteArray &p, SearchRes *m);   // ≤ 300 results, ≤ 64 tags each

struct PublishKeyEntry { KadId file; TagList tags; };
struct PublishKeyReq { KadId keyword; QList<PublishKeyEntry> entries; };
QByteArray encodePublishKeyReq(const PublishKeyReq &m);
bool decodePublishKeyReq(const QByteArray &p, PublishKeyReq *m);

struct PublishSourceReq { KadId file; KadId source; TagList tags; };
QByteArray encodePublishSourceReq(const PublishSourceReq &m);
bool decodePublishSourceReq(const QByteArray &p, PublishSourceReq *m);

struct PublishRes { KadId target; quint8 load = 0; bool hasOptions = false; quint8 options = 0; };
QByteArray encodePublishRes(const PublishRes &m);
bool decodePublishRes(const QByteArray &p, PublishRes *m);

struct Firewalled2Req { quint16 tcpPort = 0; KadId userHash; quint8 connectOptions = 0; };
QByteArray encodeFirewalled2Req(const Firewalled2Req &m);
bool decodeFirewalled2Req(const QByteArray &p, Firewalled2Req *m);   // also accepts FIREWALLED_REQ (2 bytes)

struct FindBuddyReq { KadId buddyId; KadId userHash; quint16 tcpPort = 0; };
QByteArray encodeFindBuddyReq(const FindBuddyReq &m);
bool decodeFindBuddyReq(const QByteArray &p, FindBuddyReq *m);

struct FindBuddyRes { KadId check; KadId userHash; quint16 tcpPort = 0; bool hasOptions = false; quint8 connectOptions = 0; };
QByteArray encodeFindBuddyRes(const FindBuddyRes &m);
bool decodeFindBuddyRes(const QByteArray &p, FindBuddyRes *m);

struct CallbackReq { KadId buddyId; KadId fileId; quint16 tcpPort = 0; };
QByteArray encodeCallbackReq(const CallbackReq &m);
bool decodeCallbackReq(const QByteArray &p, CallbackReq *m);

struct FirewallUdpMsg { quint8 error = 0; quint16 port = 0; };
QByteArray encodeFirewallUdp(const FirewallUdpMsg &m);
bool decodeFirewallUdp(const QByteArray &p, FirewallUdpMsg *m);

// ---------------------------------------------------------------- UDP obfuscation (eMule)
// Bytes that start a plain datagram and are never used as the obfuscation marker.
bool isProtocolMarker(quint8 b);
// eMule CPrefs::GetUDPVerifyKey: per-target-IP 32-bit key derived from our persistent base key.
quint32 udpVerifyKey(quint32 baseKey, quint32 ed2kIp);

struct ObfuscationParams {   // for deterministic tests
    bool fixed = false;
    quint16 randomKeyPart = 0;
    quint8 marker = 0;       // before the low 2 marker bits are set
};
// Kad datagram, keyed by the receiver's KadID (requests) or, if `targetId` is null/zero, by the
// receiver verify key it gave us earlier (responses). Adds <receiverKey 4><senderKey 4>.
QByteArray obfuscateKad(const QByteArray &plain, const KadId *targetId, quint32 receiverVerifyKey,
                        quint32 senderVerifyKey, const ObfuscationParams &prm = {});
// eD2k client datagram, keyed by MD5(<target user hash><our public IP, eD2k order><91><rand 2>).
QByteArray obfuscateEd2k(const QByteArray &plain, const QByteArray &targetUserHash, quint32 ourPublicEd2kIp,
                         const ObfuscationParams &prm = {});

struct Deobfuscated {
    bool obfuscated = false;   // the datagram carried the magic value under one of our keys
    bool kad = false;          // decrypted with a Kad key (NodeID / receiver key)
    bool viaReceiverKey = false;
    quint32 receiverVerifyKey = 0, senderVerifyKey = 0;
    QByteArray plain;          // de-obfuscated datagram (protocol byte first) or the input unchanged
};
// Tries the keys in eMule's order (marker bits as a hint). `ourKadId` null = Kad not running.
// `verifyKeyForSender` = udpVerifyKey(baseKey, senderIp). `senderEd2kIp` is part of the eD2k key.
Deobfuscated deobfuscate(const QByteArray &datagram, const KadId *ourKadId, const QByteArray &ourUserHash,
                         quint32 senderEd2kIp, quint32 verifyKeyForSender);

} // namespace mx::ed2k::kad
