OctoMX @VERSION@ for Linux (x86_64)
===================================

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
  tar xf OctoMX-@VERSION@-linux-x86_64.tar.xz
  OctoMX-@VERSION@-linux-x86_64/octomx
Nothing is installed and no root rights are needed; the folder can live anywhere. Qt does not
have to be installed: the needed Qt 6 libraries are in lib/ and plugins/. To get a menu entry
and icon, run ./install-desktop-entry.sh once (again after moving the folder).

The bundle uses X11 (on Wayland desktops it runs through XWayland). It needs a 64-bit
distribution with glibc @GLIBC@ or newer and libstdc++ from GCC @GCC@ or newer (for example
@DISTRO_EXAMPLE@), plus the usual desktop libraries: X11/xcb, xkbcommon, fontconfig,
freetype, OpenGL (libglvnd), glib 2, D-Bus, and OpenSSL 3 for HTTPS. These are normally present
on any desktop installation.

On the first start a Setup Wizard opens. Click "Finish" on the first page ("Recommended
settings") for the one-click setup: all networks on, ports mapped automatically on your router
(UPnP / NAT-PMP), magnet:, ed2k:// links and .torrent files opened by OctoMX. Or click "Next" to
choose your screen name, connection type, ports, folders and networks. The wizard can be opened
again from Settings.

Where your data lives
---------------------
Settings, the database, logs and download state are in ~/.local/share/OctoMX. Start with
  ./octomx --profile /some/where/profile
to use another folder. If a file named portable.txt is next to bin/octomx, the folder
bin/profile is used instead. Completed downloads go to ~/Downloads/OctoMX unless you choose
another folder. Completed downloads are shared with other users by default (this can be turned
off in Settings).

Ports (defaults; change them in Settings or in the wizard)
-----------------------------------------------------------
  OctoMX network     TCP 6699, UDP 6257
  BitTorrent         TCP+UDP 6881
  eD2k               TCP 4662, UDP 4672 (Kad shares UDP 4672)
  Gnutella           TCP+UDP 6346
  rats-search        port 4444 (+ a random UDP DHT port)
Downloading works without open ports. To be reachable by others (High ID on eD2k, non-firewalled
on Kad/Gnutella), open these ports in your firewall (ufw/firewalld) and on your router, or leave
UPnP / NAT-PMP on.

Command line
------------
  ./octomx [--profile <dir>] [--minimized] [--page <networks|shared|chat|search|hotlist|transfers|bandwidth>]
           [--no-wizard] [--demo] [<magnet:...|ed2k://...|http(s)://...|file.torrent> ...]
  --demo shows canned sample data without any network access. --help lists all switches.

Known limitations
-----------------
  * No native Wayland plugin is bundled (X11/XWayland only).
  * See README.md in the source tree for the full list (firewalled operation, missing eD2k/
    Gnutella extensions, untested proxy support, ...).

Licences
--------
OctoMX is free software under the GNU GPL version 3 or later (LICENSE.txt, NOTICE.txt); where
to get its source code is written in SOURCE.txt. The bundled third-party
components and their licences are listed in THIRD-PARTY-NOTICES.txt and licenses/. The Qt
libraries in lib/ and plugins/ are LGPL-3.0 and may be replaced by your own build of Qt 6.12.1.
