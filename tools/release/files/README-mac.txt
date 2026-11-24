OctoMX @VERSION@ for macOS (Apple Silicon and Intel)
====================================================

OctoMX is a file-sharing client with the classic WinMX-style interface:
the gray window of the 90s file-sharing clients, black lists with coloured text, chat rooms,
hotlist and transfers, working on today's networks (BitTorrent, eD2k/Kad, Gnutella, HTTP,
torrent index sites and its own OctoMX network).

OctoMX is an independent open-source program. It is not affiliated with, endorsed by or derived
from the original WinMX or its makers (Frontcode Technologies), and contains none of its code;
WinMX is named only to describe the style of the interface. It cannot connect to the network of
the original WinMX.

Installing
----------
OctoMX.app is one universal program for Apple Silicon and Intel Macs, macOS @MACOS_MIN@ or newer.
Drag OctoMX.app into your Applications folder (or keep it anywhere you like). Qt does not have to
be installed: everything OctoMX needs is inside the app.

First start
-----------
OctoMX is not signed with an Apple Developer ID (it has an ad-hoc signature only), so macOS does
not open it with a double-click the first time. Instead:
  1. In Finder, right-click (or Control-click) OctoMX.app and choose Open.
  2. In the dialog that says macOS cannot verify the developer, click Open.
On macOS 15 and newer the dialog may only offer "Done": then open System Settings > Privacy &
Security, scroll down to the line about OctoMX and click "Open Anyway", then confirm.
From then on OctoMX opens normally. If macOS says the app "is damaged", the quarantine flag of
the download is the cause; in Terminal:
  xattr -dr com.apple.quarantine /Applications/OctoMX.app

macOS then asks once whether OctoMX may find devices on your local network (needed for port
mapping on your router and for OctoMX users on the same network: click Allow) and, when the
firewall is on, whether it may accept incoming connections (click Allow).

On the first start a Setup Wizard opens. Click "Finish" on the first page ("Recommended
settings") for the one-click setup, or "Next" to choose your screen name, connection type, ports,
folders and networks. The wizard can be opened again from Settings.

On a Mac
--------
  * The window looks exactly as on Windows (the classic interface draws everything itself).
  * The menu bar has the OctoMX menu: About OctoMX, Check for Updates..., Settings... (Cmd+,),
    Quit OctoMX (Cmd+Q). The red close button hides the window; OctoMX keeps running (Dock and
    menu bar icons), a click on the Dock icon brings the window back. Quit asks as the close
    prompt does on Windows.
  * magnet: and ed2k:// links and .torrent files open in OctoMX once it is the default program
    for them (Settings > Miscellaneous > System Integration; or Finder > Get Info > Open with).
  * "Start OctoMX with the computer" writes ~/Library/LaunchAgents/io.github.robtacconelli.octomx.plist
    (macOS shows a "Background Items Added" notice); turning it off removes the file.
  * Updates: OctoMX tells you about a new version and opens its download page; it does not
    replace its own app bundle on macOS.

Where your data lives
---------------------
Settings, the database, logs and download state are in
  ~/Library/Application Support/OctoMX
Completed downloads go to ~/Downloads/OctoMX unless you choose another folder; they are shared
with other users by default (this can be turned off in Settings). If a file named portable.txt
is next to OctoMX.app, the folder "profile" next to it is used instead.

Removing OctoMX
---------------
Quit OctoMX, then move OctoMX.app to the Trash. To remove your settings and download state too,
delete ~/Library/Application Support/OctoMX and, if you used "Start with the computer",
~/Library/LaunchAgents/io.github.robtacconelli.octomx.plist. Your downloaded files in
~/Downloads/OctoMX stay until you delete them.

Ports (defaults; change them in Settings or in the wizard)
-----------------------------------------------------------
  OctoMX network     TCP 6699, UDP 6257
  BitTorrent         TCP+UDP 6881
  eD2k               TCP 4662, UDP 4672 (Kad shares UDP 4672)
  Gnutella           TCP+UDP 6346
  rats-search        port 4444 (+ a random UDP DHT port)
Downloading works without open ports. To be reachable by others, allow OctoMX in the macOS
firewall and leave UPnP / NAT-PMP on (or forward the ports on your router).

Command line
------------
  /Applications/OctoMX.app/Contents/MacOS/octomx [--profile <dir>] [--minimized] [--demo] [<link> ...]
  --demo shows canned sample data without any network access. --help lists all switches.

Licences
--------
OctoMX is free software under the GNU GPL version 3 or later. LICENSE.txt, NOTICE.txt,
SOURCE.txt (where to get the source code), THIRD-PARTY-NOTICES.txt and licenses/ are inside the
app: OctoMX.app/Contents/Resources (Finder: right-click > Show Package Contents). The Qt
frameworks in Contents/Frameworks are LGPL-3.0 and may be replaced by your own build of Qt.
