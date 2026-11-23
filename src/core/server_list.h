// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: server list files for NetworkService::importServerList when the plugin does not
// understand the file itself (NetworkPlugin::importServers returned -1):
//   * eMule server.met (header 0x0E or 0xE0; entries ip/port + tags, ST_SERVERNAME = 0x01);
//   * text: one server per line, "host:port", "host:port name...", "ed2k://|server|ip|port|/",
//     '#' / ';' comments.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QString>

namespace mx {

struct ServerListEntry {
    QString address;     // "host:port"
    QString name;
};

// Returns the entries, or an empty list + *error when the data is neither format.
QList<ServerListEntry> parseServerListFile(const QByteArray &data, QString *error = nullptr);
// Exposed for tests: the binary server.met reader alone.
bool parseServerMetEntries(const QByteArray &data, QList<ServerListEntry> *out);

} // namespace mx
