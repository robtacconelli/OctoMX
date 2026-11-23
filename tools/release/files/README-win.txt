OctoMX @VERSION@ for Windows (64-bit)
=====================================

OctoMX is a file-sharing client with the classic WinMX-style interface:
the gray window of the 90s file-sharing clients, black lists with coloured text, chat rooms,
hotlist and transfers, working on today's networks (BitTorrent, eD2k/Kad, Gnutella, HTTP,
torrent index sites and its own OctoMX network).

OctoMX is an independent open-source program. It is not affiliated with, endorsed by or derived
from the original WinMX or its makers (Frontcode Technologies), and contains none of its code;
WinMX is named only to describe the style of the interface. It cannot connect to the network of
the original WinMX.

Starting
--------
Unzip the whole folder anywhere (for example to your Desktop) and double-click octomx.exe.
Nothing is installed; there is no installer yet. Requires 64-bit Windows 10 or 11.
Windows Firewall may ask whether OctoMX may accept connections: allow it for private (and, if
you want to be reachable, public) networks.

On the first start a Setup Wizard opens. Click "Finish" on the first page ("Recommended
settings") for the one-click setup: all networks on, ports mapped automatically on your router
(UPnP / NAT-PMP), magnet:, ed2k:// links and .torrent files opened by OctoMX. Or click "Next" to
choose your screen name, connection type, ports, folders and networks. The wizard can be opened
again from Settings.

Where your data lives
---------------------
Settings, the database, logs and download state are in %APPDATA%\OctoMX. Start with
  octomx.exe --profile D:\somewhere\profile
to use another folder (for example a portable copy on a USB stick). If a file named portable.txt
is next to octomx.exe, the folder "profile" next to it is used instead of %APPDATA%\OctoMX.
Completed downloads go to Downloads\OctoMX unless you choose another folder. Completed downloads
are shared with other users by default (this can be turned off in Settings).

Ports (defaults; change them in Settings or in the wizard)
-----------------------------------------------------------
  OctoMX network     TCP 6699, UDP 6257
  BitTorrent         TCP+UDP 6881
  eD2k               TCP 4662, UDP 4672 (Kad shares UDP 4672)
  Gnutella           TCP+UDP 6346
  rats-search        port 4444 (+ a random UDP DHT port)
Downloading works without open ports. To be reachable by others (High ID on eD2k, non-firewalled
on Kad/Gnutella), forward these ports on your router or leave UPnP / NAT-PMP on.

Command line
------------
  octomx.exe [--profile <dir>] [--minimized] [--page <networks|shared|chat|search|hotlist|transfers|bandwidth>]
            [--no-wizard] [--demo] [<magnet:...|ed2k://...|http(s)://...|file.torrent> ...]
  --demo shows canned sample data without any network access. --help lists all switches.

Known limitations
-----------------
  * This Windows build has so far been tested only under Wine 10 on Linux, not on a real
    Windows PC. Please report problems.
  * See README.md in the source tree for the full list (firewalled operation, missing eD2k/
    Gnutella extensions, untested proxy support, ...).

Licences
--------
OctoMX is free software under the GNU GPL version 3 or later (LICENSE.txt, NOTICE.txt); where
to get its source code is written in SOURCE.txt. The bundled third-party
components and their licences are listed in THIRD-PARTY-NOTICES.txt and licenses\. The Qt
libraries (Qt6*.dll and the plugin folders) are LGPL-3.0 and may be replaced by your own build
of Qt 6.12.1.
