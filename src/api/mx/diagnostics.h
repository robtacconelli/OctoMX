// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX: connection diagnostics ("Connection Test"): the report model shared by the core, the
// mock core, the UI and mxcli, plus the plain-text report and address masking helpers.
// [added by diag-1] Owner: coordinator. Additive changes only.
#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "mx/types.h"

namespace mx {

// Colour of a row in the Connection Test window: green = fine, yellow = works with limits,
// red = broken, white = unknown (no evidence yet), gray = switched off.
enum class DiagLevel : int { Unknown = 0, Ok, Limited, Broken, Off };
QString diagLevelName(DiagLevel l);            // "OK", "LIMITED", "BROKEN", "UNKNOWN", "OFF"
// The worse of two levels for a group (Broken > Limited > Ok > Unknown > Off).
DiagLevel diagWorse(DiagLevel a, DiagLevel b);

// One check: a row of the list.
struct DiagItem {
    QString   id;          // stable, e.g. "bt.port.tcp"
    QString   check;       // "Listen port (TCP 6881)"
    DiagLevel level = DiagLevel::Unknown;
    QString   result;      // short: "Yes", "Mapped, external 6881", "LowID"
    QString   details;     // evidence in a few words
    QString   explain;     // plain words for the text box: what it means and what to do
};

// A group of checks: "General", one per network, one per active download.
struct DiagGroup {
    QString   id;          // "general", "net.BitTorrent", "dl.<transfer id>"
    QString   title;       // "General", "BitTorrent", "ubuntu-24.04.3-desktop-amd64.iso"
    Net       net = Net::Unknown;
    QString   downloadId;  // non-empty for a download group (TransferInfo::id)
    DiagLevel level = DiagLevel::Unknown;
    QString   result;      // one line for the group row ("Reachable", "Firewalled, works")
    QString   explain;
    QList<DiagItem> items;
};

struct DiagReport {
    QDateTime generated;
    QString   version;     // MX_VERSION_STRING
    QString   os;          // QSysInfo::prettyProductName() + kernel / wine note
    DiagLevel level = DiagLevel::Unknown;
    QString   summary;     // "Everything works", "Works, but you cannot be reached from the internet ..."
    QList<DiagGroup> groups;
    QStringList logLines;  // the last warnings / errors of the text log (at most 200)
    QString   logFile;     // <profile>/logs/octomx.log
    // the active test ("Test Now")
    bool      testRunning = false;
    int       testPercent = 0;
    QString   testStep;
    QDateTime lastTest;    // invalid = never run in this session
};

// Masks public IP addresses in text consistently (the first public address becomes "ext-1", the
// next different one "ext-2", ...). Private, loopback, link-local and carrier-grade NAT
// (100.64.0.0/10) addresses are kept: they identify nobody and are needed to give advice.
class AddressMasker {
public:
    QString mask(const QString &text);
    int count() const { return int(m_map.size()); }
private:
    QHash<QString, QString> m_map;
};

enum class AddressKind : int { Invalid = 0, Loopback, Private, LinkLocal, CarrierNat, Public };
// Classify an IPv4 / IPv6 address text (ports and brackets allowed: "1.2.3.4:5", "[::1]:6").
AddressKind classifyAddress(const QString &address);
bool isPublicAddress(const QString &address);

// The self-contained plain-text report (Copy Report / Save Report / mxcli diag).
QString diagReportText(const DiagReport &r, bool hideAddresses);
// The group / item a UI should select for a download ("Why Is This Slow?").
QString diagGroupIdForDownload(const QString &transferId);

// Overlay "probe my ports", UDP part: an OctoMX helper sends probeDatagram(nonce, port) to one of
// our UDP ports. A plugin that owns a UDP socket hands every datagram it receives to
// consumeProbeDatagram() first (true = it was a probe, drop it); the requester then asks
// probeDatagramSeen(). Thread-safe.
QByteArray probeDatagram(const QString &nonce, quint16 port);
bool consumeProbeDatagram(const QByteArray &datagram);
bool probeDatagramSeen(const QString &nonce, quint16 port);

} // namespace mx

Q_DECLARE_METATYPE(mx::DiagReport)
