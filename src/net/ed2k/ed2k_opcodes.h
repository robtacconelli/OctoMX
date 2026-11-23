// SPDX-License-Identifier: GPL-3.0-or-later
// eD2k / eMule protocol constants (values from the public protocol as implemented by eMule/aMule;
// written down here from the protocol description, no code copied).
#pragma once

#include <QtCore/QtGlobal>

namespace mx::ed2k {

// ---------------------------------------------------------------- framing
constexpr quint8 PROT_EDONKEY = 0xE3;
constexpr quint8 PROT_EMULE   = 0xC5;
constexpr quint8 PROT_PACKED  = 0xD4;   // zlib-compressed eMule-extended packet
constexpr quint8 PROT_KAD     = 0xE4;   // Kad (UDP), used by the Kad task
constexpr quint8 PROT_KADPACK = 0xE5;

constexpr qint64  PARTSIZE      = 9728000;
constexpr qint64  EMBLOCKSIZE   = 184320;        // 180 KiB, max block per request range
constexpr qint64  OLD_MAX_FILE_SIZE = 4290048000LL; // files above need the 64-bit opcodes
constexpr qint64  MAX_FILE_SIZE = 0x4000000000LL;   // 256 GiB (eMule limit)
constexpr quint8  EDONKEYVERSION = 0x3C;

// ---------------------------------------------------------------- client <-> server (TCP)
enum : quint8 {
    OP_LOGINREQUEST      = 0x01,
    OP_REJECT            = 0x05,
    OP_GETSERVERLIST     = 0x14,
    OP_OFFERFILES        = 0x15,
    OP_SEARCHREQUEST     = 0x16,
    OP_DISCONNECT        = 0x18,
    OP_GETSOURCES        = 0x19,
    OP_SEARCH_USER       = 0x1A,
    OP_CALLBACKREQUEST   = 0x1C,
    OP_QUERY_MORE_RESULT = 0x21,
    OP_GETSOURCES_OBFU   = 0x23,
    OP_SERVERLIST        = 0x32,
    OP_SEARCHRESULT      = 0x33,
    OP_SERVERSTATUS      = 0x34,
    OP_CALLBACKREQUESTED = 0x35,
    OP_CALLBACK_FAIL     = 0x36,
    OP_SERVERMESSAGE     = 0x38,
    OP_IDCHANGE          = 0x40,
    OP_SERVERIDENT       = 0x41,
    OP_FOUNDSOURCES      = 0x42,
    OP_USERS_LIST        = 0x43,
    OP_FOUNDSOURCES_OBFU = 0x44,
};

// ---------------------------------------------------------------- client <-> server (UDP)
enum : quint8 {
    OP_GLOBSEARCHREQ3    = 0x90,
    OP_GLOBSEARCHREQ2    = 0x92,
    OP_GLOBGETSOURCES2   = 0x94,
    OP_GLOBSERVSTATREQ   = 0x96,
    OP_GLOBSERVSTATRES   = 0x97,
    OP_GLOBSEARCHREQ     = 0x98,
    OP_GLOBSEARCHRES     = 0x99,
    OP_GLOBGETSOURCES    = 0x9A,
    OP_GLOBFOUNDSOURCES  = 0x9B,
    OP_SERVER_DESC_REQ   = 0xA2,
    OP_SERVER_DESC_RES   = 0xA3,
};

// ---------------------------------------------------------------- client <-> client, eDonkey (0xE3)
enum : quint8 {
    OP_HELLO               = 0x01,
    OP_SENDINGPART         = 0x46,
    OP_REQUESTPARTS        = 0x47,
    OP_FILEREQANSNOFIL     = 0x48,
    OP_END_OF_DOWNLOAD     = 0x49,
    OP_ASKSHAREDFILES      = 0x4A,
    OP_ASKSHAREDFILESANSWER= 0x4B,
    OP_HELLOANSWER         = 0x4C,
    OP_CHANGE_CLIENT_ID    = 0x4D,
    OP_MESSAGE             = 0x4E,
    OP_SETREQFILEID        = 0x4F,
    OP_FILESTATUS          = 0x50,
    OP_HASHSETREQUEST      = 0x51,
    OP_HASHSETANSWER       = 0x52,
    OP_STARTUPLOADREQ      = 0x54,
    OP_ACCEPTUPLOADREQ     = 0x55,
    OP_CANCELTRANSFER      = 0x56,
    OP_OUTOFPARTREQS       = 0x57,
    OP_REQUESTFILENAME     = 0x58,
    OP_REQFILENAMEANSWER   = 0x59,
    OP_CHANGE_SLOT         = 0x5B,
    OP_QUEUERANK           = 0x5C,
    OP_ASKSHAREDDIRS       = 0x5D,
    OP_ASKSHAREDFILESDIR   = 0x5E,
    OP_ASKSHAREDDIRSANS    = 0x5F,
    OP_ASKSHAREDFILESDIRANS= 0x60,
    OP_ASKSHAREDDENIEDANS  = 0x61,
};

// ---------------------------------------------------------------- client <-> client, eMule extended (0xC5)
enum : quint8 {
    OP_EMULEINFO           = 0x01,
    OP_EMULEINFOANSWER     = 0x02,
    OP_COMPRESSEDPART      = 0x40,
    OP_QUEUERANKING        = 0x60,
    OP_FILEDESC            = 0x61,
    OP_REQUESTSOURCES      = 0x81,
    OP_ANSWERSOURCES       = 0x82,
    OP_REQUESTSOURCES2     = 0x83,
    OP_ANSWERSOURCES2      = 0x84,
    OP_PUBLICKEY           = 0x85,
    OP_SIGNATURE           = 0x86,
    OP_SECIDENTSTATE       = 0x87,
    OP_REQUESTPREVIEW      = 0x90,
    OP_PREVIEWANSWER       = 0x91,
    OP_MULTIPACKET         = 0x92,
    OP_MULTIPACKETANSWER   = 0x93,
    OP_PUBLICIP_REQ        = 0x97,
    OP_PUBLICIP_ANSWER     = 0x98,
    OP_CALLBACK            = 0x99,
    OP_REASKCALLBACKTCP    = 0x9A,
    OP_AICHREQUEST         = 0x9B,
    OP_AICHANSWER          = 0x9C,
    OP_AICHFILEHASHANS     = 0x9D,
    OP_AICHFILEHASHREQ     = 0x9E,
    OP_BUDDYPING           = 0x9F,
    OP_BUDDYPONG           = 0xA0,
    OP_COMPRESSEDPART_I64  = 0xA1,
    OP_SENDINGPART_I64     = 0xA2,
    OP_REQUESTPARTS_I64    = 0xA3,
    OP_MULTIPACKET_EXT     = 0xA4,
    OP_CHATCAPTCHAREQ      = 0xA5,
    OP_CHATCAPTCHARES      = 0xA6,
    OP_FWCHECKUDPREQ       = 0xA7,
    OP_KAD_FWTCPCHECK_ACK  = 0xA8,
    OP_MULTIPACKET_EXT2    = 0xA9,
    OP_MULTIPACKETANSWER_EXT2 = 0xB0,
    OP_HASHSETREQUEST2     = 0xB1,
    OP_HASHSETANSWER2      = 0xB2,
};

// client <-> client UDP (eMule extended)
enum : quint8 {
    OP_REASKFILEPING     = 0x90,
    OP_REASKACK          = 0x91,
    OP_FILENOTFOUND      = 0x92,
    OP_QUEUEFULL         = 0x93,
    OP_REASKCALLBACKUDP  = 0x94,
    OP_DIRECTCALLBACKREQ = 0x95,
    OP_PORTTEST          = 0xFE,
};

// ---------------------------------------------------------------- tag types
enum : quint8 {
    TAGTYPE_HASH      = 0x01,
    TAGTYPE_STRING    = 0x02,
    TAGTYPE_UINT32    = 0x03,
    TAGTYPE_FLOAT32   = 0x04,
    TAGTYPE_BOOL      = 0x05,
    TAGTYPE_BOOLARRAY = 0x06,
    TAGTYPE_BLOB      = 0x07,
    TAGTYPE_UINT16    = 0x08,
    TAGTYPE_UINT8     = 0x09,
    TAGTYPE_BSOB      = 0x0A,
    TAGTYPE_UINT64    = 0x0B,
    TAGTYPE_STR1      = 0x11,   // .. TAGTYPE_STR16 = 0x20 (length implied by the type)
    TAGTYPE_STR16     = 0x20,
    TAGTYPE_STR22     = 0x26,   // highest short-string type accepted by eMule on receive
};

// ---------------------------------------------------------------- tag names
enum : quint8 {
    // file tags
    FT_FILENAME        = 0x01,
    FT_FILESIZE        = 0x02,
    FT_FILETYPE        = 0x03,
    FT_FILEFORMAT      = 0x04,
    FT_LASTSEENCOMPLETE= 0x05,
    FT_SOURCES         = 0x15,
    FT_PERMISSIONS     = 0x16,
    FT_AICH_HASH       = 0x27,
    FT_FILEHASH        = 0x28,
    FT_COMPLETE_SOURCES= 0x30,
    FT_FILESIZE_HI     = 0x3A,
    FT_MEDIA_ARTIST    = 0xD0,
    FT_MEDIA_ALBUM     = 0xD1,
    FT_MEDIA_TITLE     = 0xD2,
    FT_MEDIA_LENGTH    = 0xD3,
    FT_MEDIA_BITRATE   = 0xD4,
    FT_MEDIA_CODEC     = 0xD5,
    FT_FILERATING      = 0xF7,
    // client tags (hello, login)
    CT_NAME            = 0x01,
    CT_PORT            = 0x0F,
    CT_VERSION         = 0x11,
    CT_SERVER_FLAGS    = 0x20,
    CT_MOD_VERSION     = 0x55,
    CT_EMULECOMPAT_OPTIONS1 = 0xEF,
    CT_EMULE_UDPPORTS  = 0xF9,
    CT_EMULE_MISCOPTIONS1 = 0xFA,
    CT_EMULE_VERSION   = 0xFB,
    CT_EMULE_BUDDYIP   = 0xFC,
    CT_EMULE_BUDDYUDP  = 0xFD,
    CT_EMULE_MISCOPTIONS2 = 0xFE,
    // server.met tags
    ST_SERVERNAME      = 0x01,
    ST_DESCRIPTION     = 0x0B,
    ST_PING            = 0x0C,
    ST_FAIL            = 0x0D,
    ST_PREFERENCE      = 0x0E,
    ST_PORT            = 0x0F,
    ST_IP              = 0x10,
    ST_DYNIP           = 0x85,
    ST_MAXUSERS        = 0x87,
    ST_SOFTFILES       = 0x88,
    ST_HARDFILES       = 0x89,
    ST_LASTPING        = 0x90,
    ST_VERSION         = 0x91,
    ST_UDPFLAGS        = 0x92,
    ST_AUXPORTSLIST    = 0x93,
    ST_LOWIDUSERS      = 0x94,
    ST_UDPKEY          = 0x95,
    ST_UDPKEYIP        = 0x96,
    ST_TCPPORTOBFUSCATION = 0x97,
    ST_UDPPORTOBFUSCATION = 0x98,
    // eMule info packet (OP_EMULEINFO) tags
    ET_COMPRESSION     = 0x20,
    ET_UDPPORT         = 0x21,
    ET_UDPVER          = 0x22,
    ET_SOURCEEXCHANGE  = 0x23,
    ET_COMMENTS        = 0x24,
    ET_EXTENDEDREQUEST = 0x25,
    ET_COMPATIBLECLIENT= 0x26,
    ET_FEATURES        = 0x27,
};

// search tree
enum : quint8 {
    SEARCH_OP_EQUAL = 0, SEARCH_OP_GREATER = 1, SEARCH_OP_LESS = 2,
    SEARCH_OP_GREATER_EQUAL = 3, SEARCH_OP_LESS_EQUAL = 4, SEARCH_OP_NOTEQUAL = 5,
};

// server capabilities we announce at login (CT_SERVER_FLAGS)
enum : quint32 {
    SRVCAP_ZLIB = 0x0001, SRVCAP_IP_IN_LOGIN = 0x0002, SRVCAP_AUXPORT = 0x0004,
    SRVCAP_NEWTAGS = 0x0008, SRVCAP_UNICODE = 0x0010, SRVCAP_LARGEFILES = 0x0100,
    SRVCAP_SUPPORTCRYPT = 0x0200, SRVCAP_REQUESTCRYPT = 0x0400, SRVCAP_REQUIRECRYPT = 0x0800,
};
// server TCP flags (OP_IDCHANGE)
enum : quint32 {
    SRV_TCPFLG_COMPRESSION = 0x0001, SRV_TCPFLG_NEWTAGS = 0x0008, SRV_TCPFLG_UNICODE = 0x0010,
    SRV_TCPFLG_RELATEDSEARCH = 0x0040, SRV_TCPFLG_TYPETAGINTEGER = 0x0080,
    SRV_TCPFLG_LARGEFILES = 0x0100, SRV_TCPFLG_TCPOBFUSCATION = 0x0400,
};
// server UDP flags (OP_GLOBSERVSTATRES / server.met)
enum : quint32 {
    SRV_UDPFLG_EXT_GETSOURCES = 0x0001, SRV_UDPFLG_EXT_GETFILES = 0x0002,
    SRV_UDPFLG_NEWTAGS = 0x0008, SRV_UDPFLG_UNICODE = 0x0010, SRV_UDPFLG_EXT_GETSOURCES2 = 0x0020,
    SRV_UDPFLG_LARGEFILES = 0x0100, SRV_UDPFLG_UDPOBFUSCATION = 0x0200,
    SRV_UDPFLG_TCPOBFUSCATION = 0x0400,
};

// special IDs used in OP_OFFERFILES when the server supports compression
constexpr quint32 FILE_COMPLETE_ID = 0xFBFBFBFB;   constexpr quint16 FILE_COMPLETE_PORT = 0xFBFB;
constexpr quint32 FILE_INCOMPLETE_ID = 0xFCFCFCFC; constexpr quint16 FILE_INCOMPLETE_PORT = 0xFCFC;

// IDs below this are LowIDs (server-assigned, not an IP)
constexpr quint32 LOWID_LIMIT = 16777216;
inline bool isLowId(quint32 id) { return id < LOWID_LIMIT; }

// obfuscation
constexpr quint32 MAGICVALUE_SYNC = 0x835E6FC4;
constexpr quint8  MAGICVALUE_REQUESTER = 34;
constexpr quint8  MAGICVALUE_SERVER = 203;
constexpr quint8  ENM_OBFUSCATION = 0x00;

// timing (milliseconds)
constexpr qint64 FILEREASKTIME_MS   = 29LL * 60 * 1000;   // TCP re-ask of a queued peer
constexpr qint64 SERVERREASKTIME_MS = 15LL * 60 * 1000;   // OP_GETSOURCES per file
constexpr qint64 SOURCEEXCHANGE_REASK_MS = 40LL * 60 * 1000;
constexpr qint64 UDPSERVERSTATTIME_MS = 5 * 1000;

// supported versions we announce
constexpr int SOURCEEXCHANGE2_VERSION = 4;

} // namespace mx::ed2k
