// SPDX-License-Identifier: GPL-3.0-or-later
// [diag-1 follow-up] The Windows UPnP NAT API (IUPnPNAT, natupnp), used by the PortMapper as a
// fallback when its own SSDP discovery finds no router on Windows. It goes through the Windows SSDP
// Discovery service, which the firewall allows and which handles the adapters itself; no elevation.
// Every call is BLOCKING (Windows may search for seconds): call it from a worker thread. On other
// systems every call answers available = false, error = "only on Windows".
#pragma once

#include <QtCore/QString>

namespace mx::diag {

struct WinUpnpResult {
    bool    available = false;   // Windows knows an internet gateway (a static mapping collection exists)
    bool    ok = false;          // the operation succeeded
    QString externalAddress;     // the router's internet address, when Windows reports it
    int     externalPort = 0;
    int     count = -1;          // probe: mappings Windows lists for the router (-1 = unknown)
    QString error;
};

WinUpnpResult winUpnpProbe();
WinUpnpResult winUpnpAdd(int externalPort, const QString &proto, int internalPort, const QString &internalClient,
                         const QString &description);
WinUpnpResult winUpnpRemove(int externalPort, const QString &proto);

} // namespace mx::diag
