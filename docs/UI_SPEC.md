# WinMX 3.53, UI specification (for the Qt 6 Widgets recreation)

Status: v1, written by task `ui-spec`. Screenshot index: `reference/INDEX.md`.

## 0. How to read this document

* **[obs: file]**, taken from that screenshot (paths are relative to `reference/`). Text in
  `code` quotes is verbatim, including capitalisation, ellipses (`...` = three ASCII dots, which is
  what the original WinMX draws) and odd spacing.
* **(reconstructed)**: not visible in any reference we found; inferred from the original WinMX help texts on
  winmxworld.com, from neighbouring versions (3.31/3.54), or from memory of the client. Treat it as a
  sensible default and keep it easy to change.
* Most screenshots are WinMX 3.54 beta 3/4 or WinMX 3.53 + "WinMX Community Patch". 3.53 and 3.54 share the
  same chrome; the visible differences are listed in §3.2 (Shared Files vs Library). **Target 3.53**
  ("Shared Files" button) unless this spec says otherwise.
* Pixel sizes are at 96 dpi, 8 pt UI font. All colours are `#RRGGBB`.

---------------------------------------------------------------------------------------------------

## 1. Global look

### 1.1 Window

* Single main window, normal resizable top-level window. Default size **800 × 600** (reconstructed;
  screenshots exist at 690, 796, 936 and 1024 px wide, all from users' resized windows). Minimum
  size ≈ 700 × 450 (reconstructed: the fixed 682-px button bands must fit).
* No menu bar, no toolbar widget, no tab widget: everything is rows of buttons.
* The system menu (window icon) is extended, see §6.1.
* Title bar text, **[obs: ref1, web/transfers_buttons_354b4_patched_titlebar.png,
  web/networks_wpn_tab_354b3.png, web/shared_files_page_353.png, web/titlebar_opennap_status.png]**:

  ```
  WinMX v3.53  <Online P=5 S=12 spm=4284>                         primary, no uploads   [ref1]
  WinMX v3.53  <Online 52:39>                                      secondary             [353 shots]
  WinMX v3.54 beta 3 - 1 UL @ 832 B/s  <Online 5:48>               secondary + uploads
  WinMX v3.54 beta 4 - 4 UL @ 22,879 B/s  <Online P=8 S=5 spm=12369>
  WinMX v3.54 beta 4 - 3 UL @ 27,635 B/s  <Online 3:21:59>  <OpenNap: 14 nets online, 1 pending 27911 users ...>
  <OpenNap: 3 nets online, 3 pending 17426 users 11499847 files 514567 gigs>
  ```
  Grammar (original WinMX): `WinMX v<version>` + (if uploads > 0) ` - <n> UL @ <bytes/s with thousands sep> B/s` +
  two spaces + `<state>` + (if any OpenNap net) two spaces + `<OpenNap: <n> nets online, <m> pending
  <users> users <files> files <gigs> gigs>`.
  `<state>` = `<Online P=<primary peers> S=<secondary clients> spm=<searches per minute>>` when
  primary; `<Online h:mm:ss>` / `<Online m:ss>` when secondary (the number is the connected time,
  meaning reconstructed); `<Connecting>` while connecting (from help text); `<Offline>` when
  disconnected (reconstructed).
* Window icon: 16×16 black disc with a gray ring and green `MX` letters (§1.9).

### 1.2 Fonts

* UI font: **Tahoma 8 pt** on XP shots, **MS Sans Serif 8 pt** on 98/2000 shots. Use Tahoma 8 pt
  (fallback: "MS Shell Dlg 2", then any 8-pt sans; on Linux bundle or substitute a Tahoma-metric
  font). Text height 13 px, list row pitch **14 px** (search, chat, channel list; measured in
  web/search_results_322_native.gif and web/chat_room_window.png) or **16 px** in lists that carry
  16×16 icons or progress bars (Library, Transfers).
* Bold variants: chat room header name, "Ready" in the Connection Status box, default (first) item
  in context menus, the news bar text.
* News bar text: bold, ~10-11 pt ("Large font", yellow) **[obs: web/shared_files_page_columns_353.png]**.
* Settings ▸ Appearance has "Change Normal Font...", "Change Dynamic Font...", "Change Large Font...":
  three configurable fonts (normal = UI, dynamic = list/chat text, large = news bar/headers).

### 1.3 Chrome colours and widget styles

The client uses the Windows system colours; the classic (Windows 2000) scheme is the target:

| role | colour | evidence |
|---|---|---|
| face (window, buttons, headers, status bar) | `#D4D0C8` | web/transfers_page_354b3.png, web/search_tabs_hash_expanded_group.png |
| 3-D highlight | `#FFFFFF` | |
| 3-D shadow | `#808080` | |
| 3-D dark shadow | `#404040` | |
| text | `#000000` | |
| disabled text | `#808080` with `#FFFFFF` emboss 1 px down-right (classic) | Retry button in web/transfers_page_354b3.png |
| caption (active) | Win2000 gradient `#0A246A`→`#A6CAF0`, white bold text (just use the platform title bar) | |
| menu highlight | `#0A246A` bg, white text | web/menu_* |

(On Win98 16-bit shots the face is `#C0C0C0`; on XP-themed shots some headers/scrollbars are
`#ECE9D8`. Do not copy those, use `#D4D0C8`.)

Widget styles (all measured on native-pixel shots):

1. **Thin button** (page buttons, top-row buttons, Shared-Files/Library toolbar, sub-tab strip):
   height **21 px**, 1-px raised edge: top+left `#FFFFFF`, bottom+right `#808080`, face `#D4D0C8`.
   Icon 16×16 at x+3, centred vertically; text left-aligned right after the icon (page/top buttons)
   or centred (toolbar buttons without icon / wide buttons). **[obs: web/shared_files_page_columns_353.png
   y=48-68, web/transfers_page_354b3.png]**
2. **Selected thin button** (current page, current sub-tab, current search tab): 1-px *sunken* edge
   (top+left `#808080`, bottom+right `#FFFFFF`), **face `#FFFFFF`**, text black.
   **[obs: Transfers button in web/transfers_page_354b3.png; Shared Files in
   web/shared_files_page_columns_353.png; "WinMX Peer Network" half in ref1]**
3. **Push button** (commands inside pages: Transfers buttons, Search buttons, Connect/Disconnect,
   Change, dialogs): standard Win32 classic 2-px bevel (outer ring: top/left `#FFFFFF`,
   bottom/right `#404040`; inner ring: top/left `#D4D0C8`, bottom/right `#808080`; pressed = inverted
   1-px `#808080` frame + text shifted 1 px; i.e. Qt "Windows" style `PE_PanelButtonCommand`
   with the palette above), height **19 px** on Transfers, 21 px on
   Search, 23 px in dialogs. Text centred. Default button in dialogs has the extra 1-px black frame.
4. **Latched toggle push button** ("Available Files On Top", "Show Full Paths", "Filter",
   "Auto Complete"): when ON it is drawn sunken with the classic 50% dither (`#FFFFFF`/`#D4D0C8`
   checkerboard) face. **[obs: web/search_tabs_hash_expanded_group.png, web/search_idle_354b4_wikipedia.png]**
5. **Sunken field / list frame**: 2-px classic sunken edge (outer `#808080` top-left / `#FFFFFF`
   bottom-right, inner `#404040` top-left / `#D4D0C8` bottom-right).
6. **Group boxes** (Networks page, Settings pages): classic etched frame, caption text inset 8 px.
7. **Combo-like buttons**: the original WinMX uses *push buttons that pop a menu* for most choices
   ("MP3 Audio - Any Bitrate", "Any Connection", "All Available Networks", "Auto Find Sources - 10m",
   "Auto Enter Queue - 100"); in 3.22 the network and connection selectors were real combo boxes
   **[obs: web/search_results_322_native.gif]**. 3.5x: buttons with popup menus whose current item
   has a radio bullet `●` **[obs: web/menu_search_connection_filter.png, web/menu_auto_find_sources.png]**.
8. **Scrollbars**: classic (`#D4D0C8` thumb with bevel, dithered trough).
9. **List view header**: classic header, face `#D4D0C8`, 1-px raised items, height 16-17 px, text
   left-aligned with 4-6 px padding; numeric columns' header text is **right-aligned** (Bytes, Time,
   Bitr..., Freq, Ping, Files, Users). Truncated header text ends with `...` ("Bitr..." is the
   default look because the column is 43 px wide). Columns are resizable and **draggable/reorderable**;
   clicking a header sorts, clicking again reverses. Header persists widths and order.

### 1.4 Main-window layout (all pages share rows A-B and the status bar)

```
+-----------------------------------------------------------------------------------------------+
| [MX] WinMX v3.53  <Online P=5 S=12 spm=4284>                                     [_][□][X]   |  title
+-----------------------------------------------------------------------------------------------+
| ##### news bar (black, yellow bold text) ##### |(o)Ignored |🔧Settings|(?) Help |               |  row A  y=24..44
| [Networks ] [Shared Files] [Chat    ] [Search  ] [Hotlist ] [Transfers] [Bandwidth]            |  row B  y=48..68
|  page content (rows C.. defined per page)                                                     |
| ...                                                                                           |
| status text fields...                                                          \Float/  ◢     |  status bar 20 px
+-----------------------------------------------------------------------------------------------+
```

Measured on web/shared_files_page_columns_353.png (client-area coordinates, x from window left edge
incl. 3-px frame, title bar 18 px):

* Row A (top): y 24-44 (21 px). **News bar** x 5-489 (485 px wide, 2-px sunken border, black inside).
  `Ignored` x 494-554 (61 px), `Settings` 559-619 (61), `Help` 624-685 (62). 5-px gaps.
* 3 px gap.
* Row B (page buttons): y 48-68. Seven thin buttons, **94 px wide, 98 px pitch**, starting x = 3/4:
  `Networks` 4-97, `Shared Files` 102-195, `Chat` 200-293, `Search` 298-391, `Hotlist` 396-489,
  `Transfers` 494-587, `Bandwidth` 592-685.
* Rows A and B are a **fixed 682-px band, left-aligned; they do not stretch** when the window is
  wider [obs: ref1 at 1024 px, web/library_354alpha_tree.png at 936 px]. The remainder of rows A/B is
  plain face colour.
* 5 px gap, then the page area (to the status bar). Page area content is described per page.
* Status bar: 20 px high, face colour, no visible panel bevels; text fields separated by ~2 spaces
  (actually separate 3D-less panes). Right end: the **Float tab** then the size grip.

**News bar** ("mainbar"): in the original this is an embedded HTML control loaded from
`http://www.winmx.com/v353/mainbar/index.html` (black page, white text, refresh 10000 s)
[WB copy]. In practice users saw: black background, **bold yellow `#FFFF00`** text, e.g.
`Patch Operational` [obs: web/shared_files_page_columns_353.png], `WinMX v3.54 beta 4 NOW AVAILABLE
@ www.winmx.com/beta/beta354.html` with the URL in magenta/underlined [obs:
web/networks_wpn_tab_354b3.png], or right-aligned underlined `Currently helping to block flooders`
on gray when patched [obs: web/search_idle_354b4_wikipedia.png]. Ref1 (Wine) shows it white/empty.
Recreate as a QLabel-like rich-text strip: black bg, yellow bold large font, clickable links in
`#FF00FF` underlined; content comes from our app (news/patch status line). Empty = plain black.

**Float tab** [obs: web/library_354alpha_tree.png zoom, ref1]: a small trapezoid "sheet tab" sitting
on the status bar just left of the size grip: ~38×14 px, text `Float` (8 pt), left edge a white
slanted line, right edge and bottom a `#808080` slanted line (the tab narrows downward, like an
upside-down Excel sheet tab). Clicking it detaches the **current page** into its own top-level
window (same content; the page's own window title is the page name, reconstructed) and the tab in
that window reads `Dock`; clicking `Dock` puts it back [help text: winmx-chat-getting_started:
"Down in the bottom right hand corner it should say "Float" and if you click on that to change it to
"Dock" you can make that window smaller"]. While a page is floated, its page button still exists and
brings the floating window to front (reconstructed). Every open chat room, search, browse window can
float/dock (up to 8 chat rooms at once).

### 1.5 Page buttons, top buttons and their behaviour

* Exactly one page button is "selected" (white sunken): plus `Hotlist`, which is an independent
  toggle: it shows/hides the Hotlist side pane docked at the right of the page area (§3.5) and is
  drawn selected while the pane is visible. Clicking any other page button switches the page
  area (QStackedWidget). The selection survives restarts? (reconstructed: start on **Networks**;
  help text: "its the first screen you will see each time you start up").
* `Ignored` opens the floating **Ignored Users** tool window (§5.1).
* `Settings` opens the modeless **Settings** dialog (§7).
* `Help` opens a browser on the help URL (reconstructed; original opened winmx.com help).
* `Chat` the first time per session shows the **WinMX Decentralized Chat** warning (§5.6).

### 1.6 Black list views (the "classic WinMX look")

All data lists (search results, transfers, shared files/library, channel list, chat output and user
list, hotlist, ignored, browse, OpenNap server list) are owner-drawn list/tree views:

* background `#000000`, default text `#FFFFFF`;
* **no grid lines**; rows 14 px (16 px with icons/progress);
* **selection** = full-row background **`#808080`** (gray) with the row's text keeping its own
  colours (not white-on-blue) [obs: web/transfers_page_354b3.png (#808080 measured),
  web/search_results_nonmp3_354.png, web/menu_channel_list_row.png, web/search_group_expanded_selected.png];
  focus rectangle not drawn (reconstructed);
* tree expanders: classic 9×9 `[+]`/`[-]` box (white/light-gray box outline, white glyph) followed by
  a dotted horizontal tree line `···` in gray, drawn at the very left of the File column
  [obs: web/search_results_322_native.gif "⊞···Artist - Song Title.mp3"]; children are drawn with
  dotted vertical/horizontal tree lines `|--` before the name [obs: web/search_tabs_hash_expanded_group.png];
* frame: 2-px sunken edge, header on top (§1.3.9);
* numbers right-aligned, thousands separator from the locale (space in Europe shots `4 388 571`,
  comma in US shots `5,792,882`). Use the locale.
* colours are user-configurable per window in Settings ▸ Appearance (§7.1) and saved in `.clr`
  colour-scheme files. Everything in §2 is the **"Standard Dark Background / Light Text"** default.

### 1.7 Context menus

Standard Win32 popup menus (classic): `#D4D0C8` bg, 1-px raised border, separators etched, first /
default item **bold** where the screenshot shows it (double-click action), submenu arrows `▸`,
radio bullets `●` for the current choice, check marks `✔` for toggles.

### 1.8 Sizing/splitters

* Transfers: horizontal splitter between the download half and the upload half (help text: "hover
  the mouse over the main bar just above the Upload buttons and a double arrow will appear").
* Chat room: vertical splitter between output and user list (reconstructed but obvious).
* All list headers resizable/reorderable.

### 1.9 Icons (16×16 unless noted; describe → redraw as pixel art)

Measured from 10× zooms of web/shared_files_page_columns_353.png and web/main_page_buttons_row_native.png.
All have transparent background.

| button | icon |
|---|---|
| app / window | black disc (Ø 14) with 1-px gray `#808080` ring, inside two green `#00FF00` letters "MX" in a blocky 3-px-stroke font; navy corners (= transparency) |
| Networks | beige/gray PC: CRT monitor (gray `#C0C0C0` case, black outline, **teal `#008080` screen** 10×6) on a flat desktop base unit; base front-left has a **green** and a **red** LED pixel pair |
| Shared Files (3.53) | open yellow folder (`#FFFF00` front, olive `#808000` outline/shade) with two white sheets of paper sticking out of the top, slight perspective |
| Library (3.54) | three upright books side by side: magenta `#FF00FF`, cyan/teal, green `#00FF00` spines with black outlines, gray tops |
| Chat | white speech balloon (black outline, gray `#808080` drop shadow right/bottom), tail at bottom centre-left pointing down-left; inside three horizontal **navy `#000080`** text lines of decreasing length |
| Search | magnifying glass: round lens (Ø 8) with cyan `#00FFFF`/white glint and black rim at top-left, handle going down-right drawn in blue `#0000FF` with cyan highlight, black outline |
| Hotlist | "hot list": flames (yellow `#FFFF00` with red `#FF0000` tongues, black outline) on top of a white card (lower third) bearing a row of blue `#0000FF` dashes |
| Transfers | two dark-green `#008000` arrows, thin (1-2 px shaft): left one points **up**, right one points **down**, slightly offset vertically |
| Bandwidth | little area chart: blue `#0000FF` zig-zag line with two peaks across the top, dark-red `#800000` filled band under it, gray `#808080` base area at the bottom, black baseline |
| Ignored | red `#FF0000` "no entry" sign: 2-px circle Ø 14 with a diagonal bar from top-left to bottom-right, no fill |
| Settings | crossed tools: gray/white wrench and hammer heads at the top, handles at the bottom: one **blue `#0000FF`**, one **red `#800000`/`#FF0000`**, black outline |
| Help | yellow `#FFFF00` bold question mark with olive `#808000`/black outline |
| Share Folder | bright green `#00FF00` plus sign, 4-px arms, dark-green `#008000` outline |
| Unshare Folder | bright red `#FF0000` horizontal bar 14×4, dark-red `#800000` outline |
| Refresh | green `#00FF00` circular arrow (≈300°, clockwise, arrowhead top-right), black outline |
| Shared File Types | white page with folded corner, black outline, a blue `#0000FF` "?" in the middle, small red tab at top-left |
| Scan For Media | white page with black outline, magnifier (cyan lens, blue handle) overlapping lower-left, small red mark at left |
| Open New Search | Search magnifier + small yellow 4-point sparkle at top-left |
| search tab | small magnifier |
| List Channels (Chat sub-tab) | chat balloon with a yellow star/flash (reconstructed detail) |
| open channel tab | white square with black `#` |
| Browse window | black binoculars |
| Ignored Users window | the Ignored icon |
| Settings window | the Settings icon |
| tray icon | the app icon (reconstructed: same black/green MX disc) |

---------------------------------------------------------------------------------------------------

## 2. Colour table (default "dark background / light text" scheme)

Sampled from native-pixel PNG/GIF screenshots (exact palette values, no anti-aliasing in these
captures).

### 2.1 Generic list colours

| use | colour | evidence |
|---|---|---|
| list background | `#000000` | all |
| default text (file names, user names, numbers, white status) | `#FFFFFF` | web/search_results_322_native.gif |
| selection background | `#808080` | web/transfers_page_354b3.png |
| header / chrome | `#D4D0C8` | |
| **green**, "good" | `#00FF00` | |
| **yellow**, "medium" / in progress | `#FFFF00` | |
| **red**, "bad" / error | `#FF0000` | |
| **orange**, "1 slot free", "% of" partial files in search | `#FF8040` | search_results_322_native.gif, search_ping_column.png |
| light-red (hotlist section headers, chat "part") | `#FF8080` | hotlist_temporary_online.png |
| offline red (hotlist) | `#FF4040` | hotlist_temporary_offline.png |
| pale yellow (temporary hotlist entries, ignored list entries) | `#FFFFCC` | hotlist_temporary_offline.png, ignored_users_window.png |
| progress bar fill | `#0080FF` | transfers_page_354b3.png |
| gray text (inactive) | `#7F7F7F`/`#808080` | chat_room_window.png |

### 2.2 Search result colours [obs: web/search_results_322_native.gif, web/search_status_server_column.png,
web/search_ping_column.png, web/search_tabs_hash_expanded_group.png, web/dialog_file_download_manual_resume.png]

| column/content | rule | colour |
|---|---|---|
| File, Bytes, Time, Freq, User, Ping | normal | `#FFFFFF` |
| Bitrate | ≥ 160 kbps (observed 160, 192, 205, 256, 320) | `#00FF00` |
| Bitrate | 112-128 kbps (observed 112, 128) | `#FFFF00` |
| Bitrate | < 112 (reconstructed: 96, 64 ...) | `#FF0000` |
| Bytes of a *partial* file shared by the source | `25% of 109 878 998` | `#FF8040` |
| Speed (single source) | `Cable`, `DSL`, `T1`, `T3+` | `#00FF00` |
| Speed | `128K ISDN`, `64K ISDN` | `#FFFF00` |
| Speed | `56K`, `33.6K`, `28.8K`, `14.4K`, `Unknown` | `#FF0000` |
| Speed (group row) | `<hi> + <lo>`: hi-speed count green, `+` white, lo-speed count red | |
| Status (single) | free slots ≥ 2: `9 of 12 available` | `#00FF00` |
| Status (single) | exactly 1 free: `1 of 4 available`, `1 of 1 available` | `#FF8040` |
| Status (single) | none free: `8 in queue (0 of 3 available)` | `#FFFFFF` |
| Status (group) | some free: `2 + 0 available users` (count colours like Speed) | green / red / white |
| Status (group) | none free: `14 17 in lowest queues` | `#FFFFFF` |
| whole-row text override: **matches an incomplete local file** | | `#FFFF00` |
| whole-row text override: **can be added to running multi-point download** | | `#80FFFF` |
| whole-row text override: **currently downloading** (this source/file) | | `#004080` |

The three overrides recolour the File/Bytes/Time/Bitrate/Freq/User text of the row; Speed and Status
keep their own colours. The legend row above the list shows them as 30×9 filled swatches with a
1-px black border followed by the label [obs: legend in every search shot]:
`Matches incomplete local file` (`#FFFF00`), `Can be added to running multi-point download`
(`#80FFFF`), `Currently downloading` (`#004080`).

### 2.3 Transfer colours [obs: web/transfers_page_354b3.png, web/transfers_downloads_header_354b4.png,
web/transfers_download_row_progress.png, browse_window_and_transfers.png; strings+colours from the
winmxworld "Transfer screen messages" and "Downloads/Uploads" help pages]

Download/upload rows take their colour from the status:

| status text (verbatim) | colour |
|---|---|
| `Finding sources...` | `#FFFF00` |
| `Waiting for Network Reply...` | `#FFFF00` |
| `Connecting to Remote User` | `#FFFF00` |
| `Waiting for incoming connection...` | `#FFFF00` [obs] |
| `Connected, Negotiating...` | `#FFFFFF` (reconstructed) |
| transferring: progress bar + `49%` in the Status column, file/bytes/K-s text | `#00FF00` [obs] |
| `Complete` | blue (help: "(blue)"; use `#0080FF`) |
| `Remotely Queued <n>` | purple (`#C080FF`, reconstructed shade) |
| `Busy (<n> Users in Queue)` | pink / light purple (`#FF80FF`, reconstructed shade) |
| `Locally Queued` | royal blue (`#4060FF`, reconstructed shade) |
| `Queued` (upload side) | purple |
| `Timed out (Waiting for incoming connection)` | `#FF0000` [obs] |
| `Timed Out (Waiting for Network Reply)` | `#FF0000` |
| `Timed Out (Connecting to Remote User)` | `#FF0000` |
| `Connection Refused` | `#FF0000` |
| `File/User Unavailable` | `#FF0000` |
| `Disconnected` | `#FF0000` |
| `Connection Reset by Peer` | `#FF0000` |
| `Connection Timed Out` | `#FF0000` |
| `Both Users Firewalled` | `#FF0000` |
| `File Mismatch (002/004)` | `#FF0000` |
| `Segment Error` | `#FF0000` |
| `Cancelled` | `#FFFFFF` (reconstructed) |
| `Paused` (not a state of the original WinMX; if needed) | `#FFFFFF` |
| file row of a download that has no live source ("gone red") | `#FF0000` [obs: red file row in web/transfers_page_354b3.png] |
| speed `Unknown`/connection class in Speed column | as Search Speed colours |

Progress cell: inside the Status (uploads) / Bytes (downloads, reconstructed) column a **bar** with a
1-px `#0080FF` outline, filled `#0080FF` from the left to the percentage, the text `49%` centred in
white over it [obs: web/transfers_page_354b3.png, web/transfers_download_row_progress.png `[38%]`].
[ui-i] OctoMX: the bar fills the whole Status column and a download's bar is a map of the file (8.11).

### 2.4 Chat colours [obs: web/settings_appearance_channel_window_colors.png (defaults),
web/chat_room_window.png]

| element | default |
|---|---|
| Channel Background | `#000000` |
| Message Text | `#FFFFFF` |
| Message User Name | `#FFFF00` |
| Action/Emote Text | `#FF00FF` |
| User Join Text | `#80FF00` |
| User Part Text | `#FF8080` |
| Channel MOTD | `#80FFFF` (observed room MOTD body lines in `#80FFFA` cyan and the first MOTD/topic line in blue) |
| Topic Change | `#0000FF` |
| Server Error | `#FF0000` |
| User list: "OpenNap Traffic" / "Other" / "Background" buttons | `#000000` / `#808080` / `#FFFFFF` (labels verbatim; meaning not documented, observed user list is black bg with white names) |
| User list connection colours | `Unk`, `14.4`, `28.8`, `33.6`, `56K` → `#FF0000`; `64K`, `128K` → `#FFFF00`; `Cable`, `DSL`, `T1`, `T3+` → `#00FF00` |
| Input box Background / Text | `#000000` / `#FFFFFF` (observed room had `#4A4A4A` input bg = user scheme) |
| links in chat text | `#0000FF` underlined (observed) |
| "admin flagged" message brackets (host bots) | `#FF0000` name, `>>` red, text gray `#7F7F7F` / white (observed) |

### 2.5 Bandwidth graph [obs: web/settings_appearance_bandwidth_window_colors.png, web/bandwidth_graph_page.png]

| series | incoming graph | outgoing graph |
|---|---|---|
| Background | `#000000` | `#000000` |
| Labels | `#FFFFFF` | `#FFFFFF` |
| Lines (horizontal grid) | `#808080` | `#808080` |
| Uploads | `#FFFF00` | `#FFFF00` |
| Downloads | `#FFFFFF` | `#FFFFFF` |
| WPN Primary | `#FF8000` | `#FF8000` |
| WPN Packets | `#FF00FF` | `#FF00FF` |
| WPN Incoming Sec | `#0080C0` | `#0080C0` |
| WPN Outgoing Sec | `#00FFFF` | `#00FFFF` |
| OpenNap Traffic | `#80FF00` | `#00FF00` |
| Other | `#FF0000` | `#FF0000` |

### 2.6 Other

* OpenNap server rows: connected `Ready (...)` `#00FF00`; connecting/waiting `#FFFF00`; failed /
  not connected `#FF0000` [obs: web/networks_opennap_server_status_colors.png].
* Hotlist: section titles and their rule lines `#FF8080`; temporary entries `#FFFFCC` (white in
  3.54b3 shot); online `#00FF00`; offline `#FF4040`.
* Ignored list entries `#FFFFCC`.
* Connection-status indicator: black box, lamp green `#00FF00` (+1-px `#008000` rim), yellow
  `#FFFF00`, red `#FF0000` (red reconstructed).
* Library (3.54): incomplete files red `#FF0000` with `N% of <size>`; video bitrate `1500 / 192`
  first number green/yellow/red by quality, audio part coloured like Search bitrate.

---------------------------------------------------------------------------------------------------

## 3. Pages

### 3.1 Networks

Sub-tab strip (row C, y = 74...93): two thin buttons **each half of the full window width**
(stretching, unlike rows A/B): `WinMX Peer Network` | `OpenNap Protocol Networks`; the active one is
white/sunken [obs: ref1, web/networks_wpn_tab_354b3.png].

#### 3.1.1 "WinMX Peer Network" sub-page [obs: ref1 (3.53), web/networks_wpn_tab_354b3.png (3.54)]

Content is a fixed **476-px-wide column centred horizontally** (ref1: x 275-750 in a 1024 window),
top-aligned:

```
            [        Connect         ]   [       Disconnect       ]        y 103-124 (push, 230 px each, 15 gap)
            +- Connection Status ----------------------------------+      y 142-223
            |  ███                                                  |
            |  ███   Ready                    (bold)                |
            |  ███   It is now OK to search and download.           |
            |  ▓▓▓  (black 32x56 box, lamp)                         |
            +------------------------------------------------------+
            +- Network Identity -----------------------------------+      y 242-295
            |        Screen Name:  User605         [   Change   ]  |
            +------------------------------------------------------+
            +- Connection Type ------------------------------------+      y 314-530
            | ( ) Make a secondary connection to the network        |
            |     This option is required for users connecting ...  |
            |                                                       |
            | (•) Make a primary connection to the network          |
            |     A primary connection is recommended for ...       |
            +------------------------------------------------------+
```

* Buttons: 3.53 shows `Connect` (disabled when online) and `Disconnect` [ref1]; 3.54 shows
  `Refresh` and `Disconnect` [web/networks_wpn_tab_354b3.png]. **Use `Connect` / `Disconnect`**,
  with `Connect` disabled while online and `Disconnect` disabled while offline.
* Indicator: black rectangle 32×56 at (box.x+17, box.y+17). Lamp 24×13 inset 4 px from the bottom
  and sides (green = ready, at the bottom) [measured]. Yellow lamp = middle position, red lamp = top
  position (traffic-light semantics; yellow observed in web/networks_connection_status_finding_yellow.png,
  its exact vertical position is the middle, reconstructed; red reconstructed).
* Status texts (bold headline + plain line):
  * green: `Ready` / `It is now OK to search and download.` [obs]
  * yellow: `Finding network connections` / `Please wait before doing any searching or downloading...` [obs]
  * red: `Not connected` / `Click Connect to join the network.` (reconstructed)
* Network Identity: label `Screen Name:` then the name (no field, plain text), push button
  `Change` (122×21) at the right. `Change` opens a small dialog "Change Screen Name"
  (reconstructed): edit box + `OK`/`Cancel`; the original WinMX appends `_<5-digit id>` to the name.
* Connection Type radio texts: **verbatim** (ref1):
  * `Make a secondary connection to the network`
    `This option is required for users connecting with a dial-up modem, ISDN line, or other slow
    internet connections.  Users that are on a LAN or firewalled and cannot open certain ports may
    also need to use this option.  File listings and search queries will be routed through another
    user with a primary connection to the network.`
  * `Make a primary connection to the network`
    `A primary connection is recommended for users with DSL, cable modem, T1, T3, or other types of
    high-speed internet connections.  This will provide a more stable connection and optimal
    performance, but will continuously use a small amount of bandwidth and idle CPU time to manage
    the network connection.  Users that are on a LAN or otherwise firewalled, are on broadband
    connections with very limited outgoing bandwidth, are on very congested networks, or keep WinMX
    online for short periods of time may have much better results with a secondary connection.`
  (two spaces after full stops, word-wrapped, indented under the radio label.)
* Status bar: primary `Online` · `5 Primary Connections` · `12 Incoming Secondary Connections` ·
  `4284 / 4284 SPPM` [ref1]; secondary just `Online` [web/networks_wpn_tab_354b3.png].
  Offline: `Offline` (reconstructed); connecting: `Connecting...` (reconstructed).

#### 3.1.2 "OpenNap Protocol Networks" sub-page [obs (fragments): web/networks_opennap_tab_buttons.png,
web/networks_opennap_wsx_buttons.png, web/networks_opennap_server_*.png; layout reconstructed]

```
[ Connect ][ Disconnect ][ Stop Attempts ][ Add ][ Edit ][ Remove ]   [ Find .WSX ][ Import .WSX ][ Export .WSX ]
+--------------------------------------------------------------------------------------------------+
|[+] [ServerOne]    opennap-one.example.net:8888  Ready (7842 users, 4960610 files, 190257 gigs)  | green
|[+] [ServerTwo]    napserver.example.org         Ready (6769 users, 5397155 files, 247984 gigs)  | green
|[+] [Server Number Three]  nap3.example.com:7777 Connecting...                                   | yellow
|[+] [^°^FOUR-SERVER^°^]    Four-Server.example.net:8888 Waiting... (-2:30)                       | yellow
|[+] [ServerFive]   napfive.example.com:7777                                                       | red
+--------------------------------------------------------------------------------------------------+
```

* Button labels from the help text: `Connect`, `Disconnect`, `Stop Attempts`, `Remove`; from the
  screenshot: `Find .WSX`, `Import .WSX`, `Export .WSX` (3 push buttons side by side), `Disconnect`
  (wide push button). `Add`, `Edit` from the context menu. Exact arrangement reconstructed.
* List = black tree, one row per server: `[<network name>]` + two spaces + `host:port` + two spaces +
  status; `[+]` expands to per-host-name alternates (reconstructed). No column header visible in the
  crops (reconstructed: no header).
* Status strings: `Ready (<u> users, <f> files, <g> gigs)`, `Connecting...`, `Waiting... (-m:ss)`
  (countdown to retry), `Sending <n> Files To The Server` (help text), `Logging in...`
  (reconstructed), error text in red (e.g. `Connection refused`, reconstructed).
* Context menu [obs: web/networks_opennap_server_menu.png]: `Connect` / `Disconnect` / - / `Add` /
  `Edit` / `Remove` / - / `View Server Messages` / - / `Select All`.
* `Remove` asks for confirmation (OK/Cancel). `Add`/`Edit` open a server dialog with Network name,
  Host, Port, Username, Password, "Register username with the following E-mail" checkbox (help
  text), OK/Cancel (reconstructed layout).
* `View Server Messages` opens a floating window with the server's MOTD/messages (black text view,
  colours from Appearance ▸ "OpenNap Server Msgs") (reconstructed).

### 3.2 Shared Files (3.53) / Library (3.54)

#### 3.2.1 3.53 "Shared Files", the target [obs: web/shared_files_page_353.png,
web/shared_files_page_columns_353.png, web/menu_shared_files_folder_353.png,
web/dialog_shared_file_types_353.png, web/dialog_share_folder_browse_353.png]

Row C: thin buttons with icons, same 682-px band, 5 buttons × 133 px (pitch 137/138):
`Share Folder` (green +) · `Unshare Folder` (red −) · `Refresh` (green circular arrow) ·
`Shared File Types` (page with ?) · `Scan For Media` (page with magnifier).

List (black tree), columns: `File Path` (wide) · `Size` · `Bitrate` · `Time` · `Freq`
(Size/Bitrate/Time/Freq right-aligned, ~60 px each). Root rows are the shared folders with `[+]`;
expanding shows sub-folders and files:

```
File Path                                                 Size  Bitrate   Time    Freq
[+] C:\MX Shared\
    [folder] C:\MX Shared\Artist 1\
       Artist 1 - Song.mp3                           4 388 571      128   4:34   44100
```
Folder rows have a small yellow folder icon. Context menu on a folder: **`Expand`** (bold) /
`Open Folder` / `Un-share Folder`.
* `Share Folder` → "Browse for Folder" dialog, caption text `Hold down SHIFT when clicking OK to
  EXCLUDE sub folders.`, OK/Cancel.
* `Unshare Folder` → popup menu listing the shared folders (help text).
* `Shared File Types` → dialog `Shared File Types` with check boxes: `MP3 Audio (.mp3)`,
  `OGG Audio (.ogg)`, `Video (.avi .asf .mpg .mpeg)`, `Pictures (.jpg .jpeg .gif)`,
  `Server Address Files (.wsx)`, `WinMX Color Scheme Files (.clr)`,
  `Other Audio Types (.wav .au .aiff .midi .mpc)`, `Archive (.zip .iso .rar .exe)`; label
  `Other file extensions`, edit box, `(separate with spaces)`, `OK` `Cancel`.
* `Scan For Media` → scans all drives for media and shares them (confirm first, reconstructed).
* Status bar: `<n> files shared` (reconstructed).

#### 3.2.2 3.54 "Library" (alternative, if we want the richer version) [obs: web/library_*]

Row C: five push-like thin buttons **stretched to full width**: `Add Folder` · `Remove Folder` ·
`Refresh All` · `Settings` · `Scan For Media`. Columns: `Name` (386) · `Shared` (47) · `Bytes` (105)
· `Duration` (60) · `Bitrate` (69) · `Compression` (97) · `Resolution / Freq` (97). Folder rows:
`Downloads    254 files   12 969 715 761 bytes`; filetype sub-rows `Audio (.mp3 .ogg)` with a
green ✔ / red ✖ share box; files with 16×16 type icons; incomplete files red with `0% of 30 576 575`.
Folder menu: **`Collapse`** / `Collapse All` / - / `Open In Explorer` / - / `Filetypes ▸` / -
/ `Refresh` / - / `Rebuild` / `Remove` / `Recurse Sub-Folders` / - / `Add Folder...` / - /
`Settings...` / `Refresh All`.

### 3.3 Chat

#### 3.3.1 Chat page / channel list [obs: web/chat_channel_list_354b3.png,
web/chat_channel_list_filter_host_bar.png, web/chat_list_channels_network_dropdown_354b4.png,
web/menu_channel_list_row.png, web/menu_chat_favorites.png, web/dialog_chat_warning.png]

```
[≡List Channels            ][# WinMX Peer Network  ][                 ][                 ]   row C: tab strip
[Filter][############### black edit (white text) ##############][ Join ][ Host ][ Close ]   row D
[        Join Selected Channel          ][          Refresh List          ][      Favorites      ]  row E
+-----------------------------------------------------------------------------------------------+
| Channel                                                     |  Users | Topic                    |
| A*** dads place_AD6662471A2B                                 | 6 / 108| A room for sharing AUDIO...|
| ...                                                                                             |
+-----------------------------------------------------------------------------------------------+
1959 channels                                                                          \Float/
```

* Tab-strip geometry (shared by Chat and Search row C): the 682-px band is divided into **4 equal
  slots** (~166 px wide, ~171 px pitch); unused slots are drawn as blank thin buttons [obs:
  web/chat_list_channels_network_dropdown_354b4.png shows 2 used + 2 blank slots]. With more than 3
  open tabs the slots get narrower so that all fit (the 1013-px chat shot shows ~100-px tabs),
  reconstructed rule: width = max(70, 682 / count).
* Row C is the **chat tab strip** (left-aligned): first is
  `List Channels` (icon), then one tab per open channel list/room. The channel-list tab is labelled
  with the network (`WinMX Peer Network`), room tabs with the (truncated) room name
  (`WinMxWorld.C...`) [obs: web/chat_room_window.png]. Active tab white/sunken. `List Channels`
  pops a menu of networks (`WinMX Peer Network`, then `[Net] host:port` for each connected OpenNap
  server) and opens that network's list [obs: web/chat_list_channels_network_dropdown_354b4.png].
* Row D: `Filter` (latched toggle push button, ~60 px), black edit box (fills), `Join` (disabled
  unless text), `Host` (only shown when primary + Cable or better), `Close` (closes this list tab).
  With Filter ON, the list shows only channels whose name (and topic if Settings ▸ Channels ▸
  "Include topic in channel filter") contains the text. Pasting a full room name (with hash) and
  pressing `Join` joins directly. `Host` creates a room named by the text.
* Row E: three wide push buttons, each 1/3 of the width: `Join Selected Channel`, `Refresh List`,
  `Favorites` (popup menu).
* List columns: `Channel` (315) · `Users` (85, right-aligned, `23 / 90` = users / limit) ·
  `Topic` (rest). Name format `<room name>_<12 hex digits>` (hash of host IP+port). Default sort
  none (arrival order); header click sorts.
* Double-click a row = join. Row menu: **`Join Channel`** / - / `Add To Favorites` /
  `Copy Channel Name`.
* Favorites menu: `Add Selected Channel` / - / *one item per favourite* (click = join) / - /
  `Remove ▸` (submenu of favourites).
* Status bar: `<n> channels`.
* First click on Chat per session: modal dialog **`WinMX Decentralized Chat`** with warning icon:
  ```
  *** WARNING ***

  The channels available through the WinMX Peer Network operate in a completely decentralized
  manner.  Only the user that is hosting a channel is able to moderate it.  Some channels could
  contain very offensive or graphic dialog.

  YOU SHOULD NOT CONTINUE IF YOU ARE EASILY OFFENDED OR ARE UNCOMFORTABLE PARTICIPATING IN AN
  UNMODERATED AND UNREGULATED CHAT ENVIRONMENT.

  The developers, publishers, and maintainers of the WinMX software DO NOT participate in or
  monitor these channels.  Please don't complain to us if you are upset by the actions of other
  users.

  Would you like to continue?
                         [ Continue ]  [ Cancel ]
  ```
  (centred text, verbatim from the screenshot).

#### 3.3.2 Chat room window [obs: web/chat_room_window.png, web/chat_room_header_name_topic.png,
web/menu_chat_room_commands.png, web/menu_chat_room_commands_admin_config.png]

```
[≡List Channels][# WinMxWorld.C...]                                                      row C tabs
 WinMxWorld.Com Help_49717657BABE on WinMX Peer Network          (bold, 8pt bold)          header
 Welcome to the WinMX World Help Room! << NEW ADDRESS >> please make sure to update your Favorites!
+------------------------------------------------------------+---------------------------------+
| (black output, word-wrapped, 14 px lines)                   | User            | Files | Speed |
| Welcome to the WinMXWorld.com Help Room! ...      (MOTD)    | @UserName_123...  |  1226 | DSL   |
| User836_24616 (DSL 178 files) has entered       (join)      | UserName1234 ...  |     0 | DSL   |
| <Nick> message text                                         | ...                             |
| * Nick does something                         (action)      |                                 |
|                                               [vscroll]     |                       [vscroll] |
+------------------------------------------------------------+---------------------------------+
[############ input line (black, white text) ###############]      [ Commands ][ Exit Channel ]
                                                                                    \Float/
```

* Header (face colour, 2 lines): line 1 bold `<channel name> on WinMX Peer Network`, line 2 the
  topic in normal weight. Height ≈ 45 px.
* Output: black rich-text view, word wrap, indented wrapped lines, selectable/copyable (Ctrl+C),
  backscroll limit (Commands ▸ Backscroll Limit: 100/300/500/1000 lines).
* User list: right side, ~240 px wide, black, columns `User` (136) · `Files` (60, right-aligned) ·
  `Speed` (~45, coloured per §2.4). Ops are prefixed `@`, voiced `+` (per /level_disp_*). Sorted
  alphabetically, ops first (reconstructed). Raw `#c<n>#` colour codes in names are shown verbatim in
  the list [obs].
* Bottom: input line (fills width minus buttons, 2-px sunken, colours from Appearance ▸ Input Box),
  push buttons `Commands` (73×21) and `Exit Channel` (70×21) at bottom-right.
* Line formats (colours §2.4):
  * message: `<Nick> text`, `<Nick>` in Message User Name colour (yellow), text white
    (reconstructed bracket style; matches the original WinMX default).
  * action: `* Nick text` all in Action colour (magenta) (reconstructed prefix).
  * join: `Nick (DSL 178 files) has entered`, User Join colour [obs].
  * part: `Nick has left`, User Part colour (reconstructed text).
  * topic: `Topic changed to: ...` blue (reconstructed text); MOTD lines at join in MOTD colour.
  * own messages same format as others.
  * links (`http://...`) blue underlined, clickable.
  * time stamps (Commands ▸ Time Stamps) prefix `[hh:mm]` (reconstructed formats).
* Input: Enter sends; Up/Down history (reconstructed); `/` commands (see below). Max line length 400
  (reconstructed).
* User-list context menu (reconstructed from other user menus): **`Send Message`** / - / `Browse` /
  `Whois` / - / `Add To Hotlist` / `Ignore` / - / `Copy Name`; admin entries when op
  (`Kick`, `Ban`, `Kick/Ban`, `Set Level...`).
* Double-click a user = Send Message (private message window, §5.4) (reconstructed).
* `Commands` popup [obs]:
  ```
  /action <text>
  ---------------------------------
  Admin Channel Config Commands   ▸
  Admin Level and Login Commands  ▸
  Admin Kick/Ban Commands         ▸
  ---------------------------------
  ✔ Hold Window Open After Part
  Join Channel                    (disabled while joined)
  Part Channel
  ---------------------------------
  Add To Favorite Channels
  ---------------------------------
  Clear Window
  Time Stamps                     ▸
  Backscroll Limit                ▸   (100 / 300 / 500 / 1000)
  ---------------------------------
  Help...
  ```
  Admin Channel Config Commands ▸ [obs]: `/setchannelname <channel>`, `/topic <topic>`,
  `/setmotd <motd text>`, `/motd`, `/limit <maxusers>`, `/setchannelvisible <0-1>`, -, `/kickall`,
  `/redirect <channel>`, `/allowincomingredirect <0-1>`, -, `/watchcmds [<minlevel> <maxlevel>]`,
  -, `Advanced ▸`, -, `/savecfg <1-100>`, `/loadcfg <1-1000>`. Choosing an item pastes the command
  into the input line.
  Admin Level and Login Commands ▸ (reconstructed from the command list): `/level [<user>]`,
  `/login <passphrase>`, `/setuserlevel <user> <level 5-230>`, `/setdeflevel <level 5-230>`,
  `/setvoicelevel <level 5-230>`, -, `/mklogin <access level 5-230> <passphrase>`,
  `/rmlogin <passphrase>`, `/lslogin`, `/clearlogins`.
  Admin Kick/Ban Commands ▸ (reconstructed): `/kick <user>`, `/ban <user> [minutes]`,
  `/kickban <user> [minutes]`, `/banip <user> <ip>[/nm] [minutes]`, `/unban <user>`, `/listbans`,
  `/clearbans`, `/opmsg <message>`.
* Client-side commands (verbatim from the original WinMX "channel commands" doc, June 2004):
  `/clr` (clear display), `/log [filename]` (start/stop logging to `filename.txt`),
  `/channelname` (echo the full room name, shown red in [obs: web/chat_room_channelname_output.png]),
  `/action <text>`, `/me <text>`, `/emote <text>`. Everything else is sent to the host:
  `/level`, `/login`, `/topic`, `/motd`, `/setmotd`, `/opmsg`, `/kick`, `/ban`, `/kickban`,
  `/banip`, `/unban`, `/listbans`, `/clearbans`, `/setvoicelevel`, `/setuserlevel`,
  `/setdeflevel`, `/limit`/`/setlimit`, `/kickall`/`/clearchannel`, `/watchcmds`, `/redirect`,
  `/allowincomingredirect`, `/setchannelname`, `/setchannelvisible`, `/mklogin`, `/rmlogin`,
  `/lslogin`, `/clearlogins`, all `/level_*` settings, `/setdefbanmin`, `/setssbthreshsec`,
  `/setssbmin`, `/savecfg <1-100>`, `/loadcfg <1-1000>`.
* Up to **8** rooms open simultaneously.

### 3.4 Search [obs: web/search_idle_354b4_wikipedia.png (1024 px, best layout reference),
web/search_results_322_native.gif (results), web/search_idle_annotated_354b3.png,
web/search_tabs_hash_expanded_group.png, web/search_results_nonmp3_354.png, menus]

```
[🔍✦Open New Search      ][🔍"song" ""            ][🔍"Linux" ""          ]                         row C (search tabs, 168 px each)
 Artist and / or Title
|C|[ song                                ]|              ||   Download         ||  Manually Resume...  |
|L|[                                    ]|    Search    ||Auto Find Sources - 10m||Auto Enter Queue - 100|
|E|                                       |              |
|A|
|R|
[Audio][Video][Pictures][ MP3 Audio - Any Bitrate ]|Search In New Window||Available Files On Top|| Show Full Paths |
[ All Available Networks ][    Any Connection     ]|  Stop  || Close   ||Ping On-Screen Results|| Ping All Results|
[■] Matches incomplete local file    [■] Can be added to running multi-point download    [■] Currently downloading
+---------------------------------------------------------------------------------------------------------------+
|File                                    | Bytes | Time |Bitr...| Freq | User     | Speed | Ping | Status / Server|
+---------------------------------------------------------------------------------------------------------------+
164 results                                                                                         \Float/
```

Geometry (3.54b4 at 1024 px; 3.53 identical):

| control | x range | y range |
|---|---|---|
| search tabs | 2-170, then 168-px pitch | 77-96 |
| label `Artist and / or Title` | 18 | 101-113 |
| `CLEAR` vertical button (text rotated 90°, reads bottom-to-top) | 0-16 | 114-156 |
| edit line 1 / line 2 (white, sunken) | 17-331 | 114-134 / 135-155 |
| `Search` (tall push) | 334-456 | 114-155 |
| `Download` / `Manually Resume...` | 457-579 / 580-702 | 114-134 |
| `Auto Find Sources - 10m` / `Auto Enter Queue - 100` | 457-579 / 580-702 | 135-155 |
| `Audio` `Video` `Pictures` / file-type button | 0-53, 54-107, 108-164 / 165-333 | 156-176 |
| `Search In New Window` | 334-456 | 156-176 |
| `Available Files On Top` (toggle) / `Show Full Paths` (toggle) | 457-579 / 580-702 | 156-176 |
| networks button / connection button | 0-164 / 165-333 | 177-197 |
| `Stop` / `Close` | 334-395 / 396-456 | 177-197 |
| `Ping On-Screen Results` / `Ping All Results` | 457-579 / 580-702 | 177-197 |
| legend row | 0- | 200-216 |
| results list (to status bar) | full width | 221- |

All controls form a fixed ~702-px block, left-aligned; the list stretches.

Controls & behaviour:
* Line 1 = keywords; line 2 = more keywords, `-word` excludes, `~word` must include, or
  `HASH>27fe34dc7204d10eef73d8e936d52f -7593984` for hash search (filled by "Search For
  Alternates..."). `CLEAR` empties both lines.
* `Search` starts the search **in this tab**; `Search In New Window` opens a new tab with the same
  parameters. Tab label: magnifier + `"<line1>" "<line2>"` (e.g. `"song" ""`). `Open New Search`
  creates an empty search tab. `Close` closes the tab; `Stop` stops receiving results.
* `Audio`/`Video`/`Pictures` set the file-type button to the type configured in Settings ▸ Search ▸
  File Type Buttons (defaults: Audio = `MP3 Audio - Any Bitrate`, Video = `Video (.avi .asf .mpg
  .mpeg)`, Pictures = `Pictures (.jpg .png .jpeg .gif)`).
* File-type button menu [obs: web/menu_search_filetype*.png, web/menu_search_bitrate_sub.png]:
  `MP3 Audio ▸`, `MP3/OGG Audio ▸`, `OGG Audio ▸`, `Video (.avi .asf .mpg .mpeg)`,
  `Pictures (.jpg .png .jpeg .gif)`, `Text (.txt .rtf)`, `HTML (.htm .html)`,
  `Archive (.zip .iso .rar .exe)`, `Servers (.wsx)`, `Colour Schemes (.clr)`,
  `Audio (.wav .au .aif .midi .mpc)`, `Any Non-MP3 File`, `Any File / Bitrate`.
  MP3 submenu: `Any Bitrate` / - / `Bitrate at least ▸` / `Bitrate equal to ▸` / `Bitrate at best ▸`;
  bitrate list `320 kbps`, `256 kbps`, `224 kbps`, `192 kbps`, `160 kbps`, `128 kbps`, `112 kbps`,
  `96 kbps`, `80 kbps`, `64 kbps`, `56 kbps`, `48 kbps`, `40 kbps`, `32 kbps`.
  Button text = choice, e.g. `MP3 Audio - Any Bitrate`, `MP3 Audio - At least 128 kbps`
  (reconstructed wording for the non-default), `Any Non-MP3 File`.
* Networks button menu [obs]: `All Available Networks` / - / `WinMX Peer Network` /
  `[ServerName] opennap.example.net:8889` / ... (one per connected OpenNap server).
* Connection button menu [obs]: `Any Connection` / - / `T3 or faster` / `T1 or faster` /
  `DSL or faster` / `Cable or faster` / `128K ISDN or faster` / `64K ISDN or faster` /
  `56K or faster` / `33.6K or faster` / `28.8K or faster` / `14.4K or faster`.
* `Auto Find Sources - 10m` menu [obs]: `Never`, `Every 10 min`, `Every 15 min`, `Every 20 min`,
  `Every 25 min`, `Every 30 min`, `Every 40 min`, `Every 50 min`, `Every 60 min`; label shows
  `Auto Find Sources - <n>m` or `- Never`. `Auto Enter Queue - 100` menu: `Never`, `1`...`10`, `15`,
  `20`, `25`, `30`, `35`, `40`, `45`, `50`, `60`, `70`, `80`, `90`, `100`.
  These apply to downloads started from this tab.
* `Download` = download selected rows (3.22: `Download Selected Files`); `Manually Resume...` opens
  the File Download dialog (§5.7).
* Ping buttons fill the Ping column (ms, or `?` on timeout).
* Columns, widths (px) and alignment [measured]: `File` (rest, ≥358, left) · `Bytes` (107, right) ·
  `Time` (49, right, `m:ss`/`h:mm:ss`) · `Bitrate` (43, right; header shows `Bitr...`) ·
  `Freq` (41, right, `44100`) · `User` (80, left; group rows right-aligned `2 users`) ·
  `Speed` (47, left) · `Ping` (47, right) · `Status / Server` (87+, left).
* **Grouping**: results with identical hash collapse into one parent row (default per Settings ▸
  Search ▸ Results Display):
  ```
  [+]···Artist - Song Title.mp3     4 388 571  4:34  128  44100     2 users  2 + 0   1 + 0 available users
  ```
  User = `<n> users`, Speed = `<hi> + <lo>` (hi = DSL or faster), Status = `<hi> + <lo> available
  users` or `<q1> <q2> in lowest queues`. Expanding (`[-]`) shows one child per source with its own
  user/speed/status; File/Bytes etc. repeated. Double-click on a collapsed group downloads from all
  sources (multi-point); on a child downloads from that source only. Single-source results have no
  box (indented by the box width).
* Single result row: `Artist - Song Title.mp3  5 861 376  4:53  160  44100  User233_2...  T1
  9 of 12 available`. User names are `name_12345` (truncated with `...`). With an OpenNap result,
  Status/Server shows `[ServerName] opennap.example.net:8888`.
* `Available Files On Top` sorts rows with free slots first. `Show Full Paths` shows
  `C:\Documents and Settings\...\file.mp3` in File.
* Context menu on a result [obs: web/menu_search_result.png]: **`Download`** / `Manually Download`
  / - / `Browse` / `Add To Hotlist` / `Whois` / `Send Message` / `Chat`.
* Dangerous types (exe/zip/scr...) trigger the warning dialog (§5.8).
* Status bar: `Waiting for results...` (searching, 0 results) [obs], `<n> results` [obs],
  `Search stopped - <n> results` (reconstructed).

### 3.5 Hotlist [obs: web/hotlist_temporary_and_button.png, web/hotlist_temporary_offline.png,
web/hotlist_temporary_online.png, web/menu_hotlist_*.png, dialogs]

**The Hotlist is not a page but a side pane.** Clicking `Hotlist` toggles a pane docked at the
**right edge** of the main window, below row B, *next to* the current page; both the Hotlist
button and the current page button are drawn selected (white) at the same time [obs:
web/hotlist_temporary_and_button.png shows Transfers on the left and the Hotlist pane on the right,
both buttons white; help text: "This will open on the side for you"]. Clicking `Hotlist` again
hides the pane. Pane width ≈ 190-260 px (reconstructed default 200, resizable with a splitter).

```
 ...current page...                    | [      Hotlist Options      ]  |   push button, full pane width, 19 px
                                       | +---------------------------+ |
                                       | |-------- Temporary --------| |   #FF8080 rule + centred label
                                       | | User876_62643 WinMX       | |   #FFFFCC (white in 3.54b3 shot)
                                       | | User810003_61603 WinMX    | |
                                       | |---------- Online ---------| |
                                       | | UserName [NetName] opennap.example.net:8888          |  #00FF00
                                       | |--------- Offline ---------| |
                                       | | UserName2 [NetName] opennap2.example.net:1111        |  #FF4040
                                       | +---------------------------+ |
```
Sections are only drawn when non-empty (Online/Offline only with permanent OpenNap contacts).
* Entry format: `<user> <network>` for WinMX (`User876_62643 WinMX`),
  `<user> [<net>] <host:port>` for OpenNap. Templates configurable (Hotlist Options).
* Sections: `Temporary` (WinMX 3.53 users you messaged/added this session; lost on exit), `Online` /
  `Offline` (permanent OpenNap contacts).
* `Hotlist Options` menu [obs]: `Add User...` / - / `Auto Responder ▸` (`Add/Remove...` + list of
  responders, checked = active) / - / `Privacy Level...` / - / `Confirm Remove` (check) /
  `Browse on Double-Click` (check) / - / `Change Temp User Display Template...` /
  `Change Offline User Display Template...` / `Change Online User Display Template...`.
* User menu [obs]: **`Send Message`** / `Chat` / - / `Perm add to list` / - / `Browse` / `Whois` /
  - / `Ignore` / - / `Rename on list` / - / `Remove`.
* Double-click = Send Message (or Browse if "Browse on Double-Click").
* Auto Responders dialog [obs]: title `Auto Responders`, buttons `Insert New` / `Remove Selected`,
  list of names (top), multi-line text of the selected responder (bottom). `Insert New` →
  `New Auto Responder`: `Enter a title for your new auto-responder`, edit, `OK` / `Cancel`.

### 3.6 Transfers [obs: web/transfers_page_354b3.png (layout), web/transfers_downloads_header_354b4.png,
web/transfers_uploads_header_354b4.png, menus, help pages]

```
     [Clear Complete][Load Incomplete][Purge Users][Queuing...][Options...][  Open Incoming Files Folder  ]   row C (centred)
     [Cancel][Clear][Retry][ Find Sources Now ][Auto Find Sources - 10m][Auto Enter Queue - 100]            row D (centred)
+-----------------------------------------------------------------------------------------------------------+
|File                               | User  -  AFS  -  AEQ | Speed |  Bytes            | Status       | K/s | Time Left |
|[-] Artist - Song Title (Long Name) 192.mp3   | 10m - 100 |       | 0 of 5,792,882    | Finding sources... |  |   |
|      +-- User233_21212          ...                                 Waiting for Network Reply...               |
+-----------------------------------------------------------------------------------------------------------+
 0 downloads from 0 users at 0 bytes per second total                                               (status line, face)
=========================================== splitter =========================================================
          [Clear Complete][Clear Incomplete][     Cancel     ][      Clear      ][Queuing...][Options...]   row (centred)
+-----------------------------------------------------------------------------------------------------------+
|File                       | User           | Speed  | Bytes                | Status             | K/s  | Time Left|
|Artist - Song.mp3          | user_21212     | Unknown| 0 of 5,576,633       | Timed out (Waiting for incoming conn...| |
|Song 2.mp3                 | user_55555     | Unknown| 3,416,968 of 6,8...  | [███49%   ]        | 6.51 | 8:55     |
+-----------------------------------------------------------------------------------------------------------+
 0 uploads to 0 users at 0 bytes per second total   (reconstructed)                                    \Float/
```

* Button rows are **centred** groups of classic push buttons, 19 px high, 3 px apart vertically,
  gaps 0-7 px. Widths at 690 px window [measured]: row C 96, 93, 93, 60, 60, 169; row D 60, 60, 60,
  121, 135, 135. Upload row: 96, 96, 130, 130, 60, 60.
* Labels verbatim (3.53 = 3.54): row C `Clear Complete`, `Load Incomplete` (becomes
  `Clear Incomplete` when there are incompletes with AFS = Never and AEQ = Never), `Purge Users`,
  `Queuing...`, `Options...`, `Open Incoming Files Folder`; row D `Cancel`, `Clear`, `Retry`
  (enabled when a user row is selected), `Find Sources Now`, `Auto Find Sources - <n>m`,
  `Auto Enter Queue - <n>` (per selected file; same menus as Search). Upload row: `Clear Complete`,
  `Clear Incomplete`, `Cancel`, `Clear`, `Queuing...`, `Options...`.
  `Queuing...` opens Settings at File Transfers ▸ Queuing; `Options...` at File Transfers.
* Download list columns [measured at 1025 px]: `File` (~430) · `User  -  AFS  -  AEQ` (~200) ·
  `Speed` (~48) · `Bytes` (~130, right) · `Status` (~85) · `K/s` (~50, right) · `Time Left`
  (~75, right). File rows are tree parents; each source user is a child row, folded away by default
  (new downloads and the downloads restored at start alike); Settings ▸ File Transfers ▸ "New
  downloads expanded by default" (off by default, owner's request) opens them all.
  * File row: name; User col `<afs>m - <aeq>` e.g. `10m - 100` (or `1 user - 15m - 10` when
    collapsed, [obs: web/transfers_download_row_progress.png]); Bytes `0 of 5,792,882` /
    `12,240 of 31,840`; Status = aggregate (`Finding sources...` yellow, progress bar while
    transferring, `Complete`); K/s `0.00`; Time Left `0:44`/`1:02:10`.
  * User row: user name, Speed connection class, Bytes for that source, Status = per-source status
    string (§2.3, e.g. `Remotely Queued 7`, `Busy (4 Users in Queue)`, `Timed Out (Waiting for
    Network Reply)`).
  * Inactive (no-source/failed) file rows red [obs].
* Status line under downloads: `<n> downloads from <u> users at <b> bytes per second total` [obs].
* Upload list columns [measured]: `File` (~230) · `User` (~90) · `Speed` (~46) · `Bytes` (~104,
  `583,408 of 6,110,...`) · `Status` (~80, progress bar or text) · `K/s` (~50) · `Time Left` (~65).
* Download file-row menu [obs: web/menu_transfers_download_file*.png]:
  `Cancel` / `Clear` / - / `Delete File` / `Trim 10K from end of file` / - / `Find Sources Now` /
  `Auto Find Sources - N ▸` / `Auto Enter Queue - N ▸` / - / `Search For Alternates...` /
  `Search For Similar...` / - / `Open` / `Play` (or `Start` for executables). For a completed .wsx:
  `Clear` / - / `Delete File` / - / `Search For Alternates...` / `Search For Similar...` / - /
  `Import`.
* Download user-row menu [obs: web/menu_transfers_user_row.png + help]: `Cancel` / `Clear` / - /
  `Bandwidth Priority ▸` (disabled unless outgoing throttle) / - / `Browse` / `Send Message` /
  `Whois` / `Add To Hotlist` / - / `Open` / `Play` / `Enqueue`; plus `Retry`,
  `Enter Remote Queue`, `Start Transfer` when applicable (help text).
* Upload menu [obs]: `Start Transfer` / `Cancel` / `Clear` / - / `Bandwidth Priority ▸` / - /
  `Browse` / `Send Message` / `Whois` / `Add To Hotlist` / - / `Open` / `Play` (+ `Enqueue`).
  Bandwidth Priority submenu [obs]: `x10`, `x8`, `x6`, `x5`, `x4`, `x3`, `x2`, `x1.5`, `● Normal`,
  `90%`, `80%`, `70%`, `60%`, `50%`, `40%`, `30%`, `20%`, `10%`.
* `Delete File` → `Confirm File Delete`: `Are you sure you want to send '__INCOMPLETE___<name>...' to
  the Recycle Bin?` `Yes` / `No` [obs]. Incomplete files on disk are named `__INCOMPLETE___<name>`.
* Double-click a completed download = open it; on an upload = nothing (reconstructed).
* Starting a download switches to Transfers if Settings ▸ File Transfers ▸ "Switch to Transfers
  window when starting downloads".

### 3.7 Bandwidth [obs: web/bandwidth_graph_page.png]

* Page = two stacked graphs filling the whole page area, separated by a 3-px face-colour gap:
  **incoming on top, outgoing at the bottom**. No buttons.
* Each graph: black background, horizontal grid lines `#808080` every round step (2000 / 5000 B/s),
  white labels at the left (y-axis in **bytes per second**, plain integers `18000`, `16000` ...),
  plot area starts ~48 px from the left; newest sample at the right, scrolls left; **stacked filled
  areas** per category (bottom → top: WPN Incoming Sec / WPN Outgoing Sec, WPN Packets, WPN Primary,
  OpenNap, Downloads/Uploads, Other, order reconstructed from the picture: blue at the bottom,
  magenta, orange, then yellow uploads on top in the outgoing graph). Auto-scaling y.
* Sample = rolling average over Settings ▸ Bandwidth Throttle ▸ "BW Graph Rolling Avg. Duration"
  (default `2 seconds`/`4 seconds`).
* Status bar: `Incoming (top) 8,953 bytes per second   Outgoing (bottom) 7,651 bytes per second`.

---------------------------------------------------------------------------------------------------

## 4. Status-bar strings (summary)

| page | text | source |
|---|---|---|
| Networks (primary) | `Online` · `5 Primary Connections` · `12 Incoming Secondary Connections` · `4284 / 4284 SPPM` | ref1 |
| Networks (secondary) | `Online` | web/networks_wpn_tab_354b3.png |
| Shared Files | `<n> files shared` | reconstructed |
| Chat list | `1959 channels` | web/chat_channel_list_354b3.png |
| Chat room | (empty) | web/chat_room_window.png |
| Search | `Waiting for results...` / `164 results` | web/search_idle_354b4_wikipedia.png, ref2 |
| Transfers | inline status lines (`0 downloads from 0 users at 0 bytes per second total`) | web/transfers_page_354b3.png |
| Bandwidth | `Incoming (top) 8,953 bytes per second   Outgoing (bottom) 7,651 bytes per second` | web/bandwidth_graph_page.png |
| Browse | `Finished downloading list> 2320 files (665,937 megs) total` | web/browse_window_354b4.png |
| Ignored Users | `6 users in list` | web/ignored_users_window.png |

Every page/window status bar ends with the Float tab + size grip.

## 5. Secondary windows and dialogs

### 5.1 Ignored Users [obs: web/ignored_users_window.png, web/ignored_users_window_empty_354b3.png]

Tool window (small caption, red ignore icon, title `Ignored Users`), ~350 × 250, resizable (size
grip). Top: three equal thin push buttons `Add` · `Whois` · `Remove` (Whois/Remove disabled with no
selection). Black list (no header), entries `#FFFFCC`:
`UserName [NetName] opennap.example.net:1111`, `WinMXWorld.bot123_42591 WinMX 904174ce1871`
(name, network, hash). Bottom status `<n> users in list`. Holds up to 512 entries (FIFO). Multi-
select with Shift/Ctrl. `Add` (OpenNap only) asks name + network.

### 5.2 Browse window [obs: web/browse_window_354b4.png, web/browse_window_and_transfers.png]

Floating window (binoculars icon), title `<user> on WinMX` (+ connection info), default ~640 × 930.
```
[ Refresh ][ Send Message ][ Whois ][ Add To Hotlist ][ Show Full Paths ]
[ Download ][ Manually Resume... ][Auto Find Sources - 15m][Auto Enter Queue - 10]
[■] Matches incomplete local file [■] Can be added to running multi-point download [■] Currently downloading
+ File | Bytes | Time | Bitrate | Freq (reconstructed beyond Bytes) ------------------------------+
| (black list; folders as tree when not "Show Full Paths")                                        |
+-------------------------------------------------------------------------------------------------+
Finished downloading list> 2320 files (665,937 megs) total
```
Status while loading: `Downloading list...` (reconstructed). Same row colours/menus as Search.

### 5.3 Whois (reconstructed)

Small modeless window titled `Whois: <user>`; read-only text (black bg, white text) such as:
```
User:        User605_12345
Connection:  DSL
Files:       1234 (12,345 megs)
Uploads:     2 of 3 available, 0 in queue
Online:      2 hours 13 minutes
Client:      WinMX v3.53
```
buttons `Browse`, `Send Message`, `Close`. (Whois data in the original WinMX = number of files, connection type,
online time, queue/slots, per help text.)

### 5.4 Private message window (reconstructed)

Floating window per user, title `<user> - Private Message`, ~420 × 300. Same widgets/colours as a
chat room minus the user list (Appearance ▸ "Private Chat Window" colours): black output, input line
at the bottom, buttons at bottom-right `Browse` · `Whois` · `Add To Hotlist`, Float tab. Incoming PM
from a new user opens the window (unless privacy settings block it), adds the user to Hotlist ▸
Temporary, plays the message alert (Settings ▸ Incoming Msgs). Auto-responder text is sent
automatically when enabled.

### 5.5 Close prompt [obs: web/dialog_close_prompt_minimize_or_exit.png]

Title `WinMX`. Text `Would you like WinMX to exit or would you like it to continue running in the
background?` Radios `Minimize to the tray and run in background` (default) / `Completely exit the
program`; (when downloads run, extra radio `Exit when downloads have finished`, help text);
text `You can always exit the program completely by right clicking the tray icon or clicking the
icon in the upper left corner of the main WinMX window.`; check box `Don't show this again`; `OK`.
**§8.11:** radios `Exit OctoMX` (default, first) / `Keep running in the tray`, check box
`Do not ask again`.

### 5.6 Chat warning, see §3.3.1.

### 5.7 File Download / manual resume [obs: web/dialog_file_download_manual_resume.png]

Title `File Download`. `The file you are about to download:` / indented file name /
`from <user> on WinMX` / `may be resumed from partially downloaded file(s) on your computer.` /
`What would you like to do?` radios `Save to a new file`, `Manually browse for the file to resume
from`, `Resume from the partially downloaded file:` (default) / list of candidates
(`<name>   Compatible multi-point download`, `<name>   Incomplete file (0 bytes)`) /
`If the file you choose to resume from is being used by another single point transfer, that transfer
will be cancelled before this download starts.` / `OK` `Cancel`.

### 5.8 Dangerous file warning [obs: web/dialog_dangerous_file_warning.png]

Title `WinMX`, warning icon: `The file you are about to download` / file name / `may be executable
and could potentially contain a virus or other code that could be very harmful to your computer.
It is strongly recommended that you do not download this file.` / `Download this file anyway?` /
`(You can disable this type of warning for this session in the program settings.)` / `Yes` `No`.

### 5.9 Setup Wizard (reconstructed, no screenshot found)

Modal wizard (Back/Next/Cancel), launched on first start and by Settings ▸ `Setup Wizard...`
(which first asks `Clear All Current Settings Before Running Setup Wizard?` Yes/No/Cancel, help
text). Pages: (1) Welcome; (2) Screen name; (3) Connection type (same radio list as Settings ▸
Internet Connection); (4) Incoming TCP port test (`Testing TCP port 6699...` → success/failure,
fallback "Unable to accept incoming TCP connections"); (5) UDP port test (6257) likewise;
(6) Incoming files folder + shared folders; (7) Finish (`Connect to the network now`).

---------------------------------------------------------------------------------------------------

## 6. System menu, tray, minimise, sounds

### 6.1 System menu (window icon) [obs: web/menu_system_and_tray.png left]
`Hide` (highlighted/first) / `Exit` / - / `Exit When Transfers Finish` /
`Shutdown When Transfers Finish` / - / `Auto - Startup` / - / `Reset All Windows` / - / standard
`Restore`, `Move`, `Size`, `Minimize`, `Maximize`, -, `Close  Alt+F4`.

### 6.2 Tray icon menu [obs: same file, right]
`Show` / **`Hide`** (bold default) / - / `Exit When Xfers Stop` / `Shutdown When Xfers Stop` / - /
`Reset All Windows` / - / `Auto - Startup` / - / `Exit`. Double-click the tray icon = Show/Hide.
Tooltip = window title text (reconstructed). §8.11: `Color Theme ▸` after `Reset All Windows`.

### 6.3 Close behaviour
Settings ▸ Miscellaneous "When main window closed:" `Prompt` (default) / `Minimize to tray` / `Exit`.
§8.11: `Keep running in tray` instead of `Minimize to tray`; the minimise button never goes to
the tray; without a system tray the close button exits.

### 6.4 Sounds
Only the incoming-message alert (Settings ▸ Incoming Msgs & Privacy: `Quiet` / `Use System Beep` /
`Browse for .wav file...`). No other sounds documented.

---------------------------------------------------------------------------------------------------

## 7. Settings dialog

Modeless dialog, title `Settings` (tools icon), fixed size **533 × 376** (outer) [obs: all
settings shots]. Left column (x 12-205): push button `Setup Wizard...` (y 30-52), tree view (white,
classic, dotted lines, `[+]/[-]`, y 62-327), push button `Close` (y 337-360). Right area x 215-525 =
page (no frame). Selecting a tree item shows its page; selected item highlighted (gray when the tree
lacks focus).

Tree (3.53/3.54) [obs: web/settings_*]:
```
[+] Appearance
     OpenNap Network Window | OpenNap Server Msgs | Library Window | Channel List Window |
     Channel Window | Search Window | Hotlist | Browse Window | Private Chat Window |
     Ignored Users Window | Transfers Window | Bandwidth Window
     (first items above "OpenNap Network Window" scrolled out of view, reconstructed:
      "Main Window", "WinMX Network Window")
[-] Internet Connection
     Outgoing TCP Connections
     Incoming TCP Connections
     In/Out UDP Packets
     Host Name Resolution
    WinMX Peer Network
    OpenNap Networks
    Incoming Msgs & Privacy
    Library
    Channels
    Search
[-] File Transfers
     Queuing
    Bandwidth Throttle
    File Launching
    Miscellaneous
```
(3.31 used `Colors` instead of `Appearance` and lacked Library/Channels [obs: ref4]. "Advanced
Settings" appears only with the community patch, omit.)

### 7.1 Appearance [obs: web/settings_appearance_page.png]
Column of push buttons (x 229-512, 23 px high, 5-px gaps):
`Load Standard Dark Background / Light Text Defaults`, `Load Standard Light Background / Dark Text
Defaults`, `Load Color Scheme From File...`, `Save Current Color Scheme To File...`,
`Search For Color Scheme Files` (double height), `Change Normal Font...`, `Change Dynamic Font...`,
`Change Large Font...`, `Load Language From File...`, `Reset Language To English`; text
`For more information about language files,visit:` and link `http://www.winmx.com/lng/`.

Sub-pages = a grid of **colour buttons**: each is a push button with the element name at the left
and a 30×9 colour swatch (1-px black border) at the right; click → standard Windows colour picker.
* Channel Window [obs]: full-width `Channel Background`; half-width pair `Message Text` |
  `Message User Name`; full-width `Action/Emote Text`, `User Join Text`, `User Part Text`,
  `Channel MOTD`, `Topic Change`, `Server Error`; caption `USER LIST`; `OpenNap Traffic`, `Other`,
  `Background`; quarter-width `Unk`, `14.4`, `28.8`, `33.6`, `56K`, `64K`, `128K`, `Cable`; third-
  width `DSL`, `T1`, `T3+`; caption `INPUT BOX`; half-width `Background` | `Text`. Defaults §2.4.
* Bandwidth Window [obs]: two columns `Incoming Graph` / `Outgoing Graph`, each with `Background`,
  `Labels`, `Lines`, `Uploads`, `Downloads`, `WPN Primary`, `WPN Packets`, `WPN Incoming Sec`,
  `WPN Outgoing Sec`, `OpenNap Traffic`, `Other`. Defaults §2.5.
* Other windows (reconstructed, same pattern): `Background`, `Text`, `Selection`, the
  connection-class colours (`Unk` ... `T3+`), bitrate colours, and per-window extras (Search: the 3
  legend colours, `Partial File`; Transfers: one colour per status family; Channel List: `Channel`,
  `Users`, `Topic`; Hotlist: `Section Headers`, `Temporary`, `Online`, `Offline`).

### 7.2 Internet Connection [obs: web/settings_internet_connection.png]
Group `Connection Type`, radios in two columns: `Unknown`, `14.4K dial-up`, `28.8K dial-up`,
`33.6K dial-up`, `56K dial-up`, `64K ISDN or satellite` | `128K ISDN`, `Cable`, `DSL`, `T1`,
`T3 or faster`. Buttons: `Outgoing TCP Connection Settings...`, `Incoming TCP Connection
Settings...`, `Incoming and Outgoing UDP Packet Settings...`, `Host Name Resolution / DNS
Settings...` (jump to the sub-pages). Text `You may need to restart WinMX for any of these settings
to take effect.`

#### Outgoing TCP Connections [obs]
`Connection attempts timeout after [25] seconds`; group `Proxy Server`: radios
`Don't use a proxy server (connect directly)` / `Use SOCKS4 proxy` / `Use SOCKS5 proxy`; disabled
fields `Address  (if port not 1080, use ip:port format)`, `Proxy Login Name (if required by proxy
server)`, `Password (if required by proxy server)`, check `Push host names through SOCKS5 proxy`.

#### Incoming TCP Connections [obs]
Header `YOU NEED TO RESTART WINMX FOR THESE SETTINGS TO TAKE EFFECT.` (centred). Radio
`Listen on port [6699] for incoming TCP connections` + `Important:  If you are behind a software or
hardware firewall, home cable/dsl router, internet connection sharing, OR access the internet through
a LAN, you may need to map an incoming TCP port through your router, firewall, or ICS software to
your local computer to use this option.  If you are having difficulty doing this, you should use the
other option.`; radio `Unable to accept incoming TCP connections` + `Use this setting if you are on
a LAN or behind a firewall, router, or internet connection sharing and are unable to accept incoming
TCP connections and can't do anything about it.`

#### In/Out UDP Packets [obs]
Same header. Radio `Send and receive UDP datagrams on port [6257]` + `Important:  If you are behind
a software or hardware firewall, home cable/dsl router, internet connection sharing, OR access the
internet through a LAN, you may need to map a UDP port through your router, firewall, or ICS software
to your local computer to use this option.  If you are having difficulty doing this, you should use
the other option.`; radio `Unable to send or receive UDP datagrams on any port` + `Use this setting
if you are on a LAN or behind a firewall, router, or internet connection sharing and are unable to
send and receive incoming UDP packets and can't do anything about it.`

#### Host Name Resolution [obs]
`DNS queries time out after [30] seconds`; group `Host Name Lookup`: radio `Use operating system to
resolve host names` + `Most users should use this option.`; radio `Use external DNS server to
resolve host names` + fields `Address` / `Port [53]`; `Note:  Pushing host names directly through a
SOCKS5 proxy server overrides these settings.`

### 7.3 WinMX Peer Network [obs: web/settings_winmx_peer_network.png]
Toggle push button `Automatically connect on startup` (full width, latched). Group `Preferred Peer
Cache`: radios `North America`, `Europe`, `Australia and NZ`, `South America`, `Asia`, `Africa`.
Group `Primary Connection Advanced Settings`: `These settings should only be changed by expert
users.`; `Allocated Bandwidth [7 KB/s OUT  10.5 KB/s IN ▼]`; bold-caps note `THIS DOES NOT INCLUDE
BANDWIDTH FOR DOWNLOADS/UPLOADS OR OPENNAP TRAFFIC.` followed by `If you need to allocated less
bandwidth for the network connection, you should use a SECONDARY connection instead.  Allowing more
bandwidth for your primary connection may result in slightly better results, but you should make
sure you don't allocate so much that your uploads and downloads are slowed.`;
`Search Thread Priority [IDLE (Recommended) ▼]`; `Search Thread Duty Cycle [50% ▼]`;
check `Allow primary connection with CPU under 266MHz`. Bottom full-width `Reset Defaults`.

### 7.4 OpenNap Networks [obs]
`Default Login [Me_Here]`, `Default Password (leave blank for random) [   ]`, button
`Reset all server login params to default`, `Login attempts timeout after [121] seconds`,
`Limit simultanious connection attempts to [10]` (sic), check `Do not import raw IP addresses from
WSX files` (checked).

### 7.5 Incoming Msgs & Privacy [obs]
Group `Incoming Message Privacy`: radios `Allow incoming messages from any source` /
`Only allow messages from the following sources :` with checks (enabled only for 2nd radio)
`Users on your hotlist (both perm and temp)`, `Users you are downloading from`, `Users you are
uploading to`, `Users in the same chat room`. Group `Incoming Message Alert`: current value text
(`Nothing`), buttons `Quiet`, `Use System Beep`, `Browse for .wav file...`.

### 7.6 Library [obs]
Group `Available Filetypes`: list with header `Name`: `Audio (.mp3 .ogg)`, `Video (.asf .avi .mpg
.mpeg)`, `Pictures (.png .jpg .jpeg .gif)`, `Text (.txt .rtf .pdf)`, `HTML (.htm .html)`,
`Archive (.zip .iso .rar .r##)`, `Executable (.exe)`, `Other Audio (.mpc .au .aif .wav .midi)`,
(`Servers (.wsx)`, `Colour Schemes (.clr)` below the fold); buttons `New`, `Edit`, `Move Up`,
`Add Defaults`, `Remove`, `Move Down`.

### 7.7 Channels [obs]
Checks `Fetch channels immediately when list opened` (on), `Include topic in channel filter`,
`Allow channel output backscroll over 1000 lines.` + `This is NOT RECOMMENDED because word-wrap
recalculating becomes very time consuming when channel windows with very large backscroll buffers
are re-sized.`

### 7.8 Search [obs]
Group `Default Search Parameters`: buttons `MP3 Audio - Any Bitrate`, `Any Connection`. Group
`Default Download Parameters`: `Auto Find Sources - 10m`, `Auto Enter Queue - 100` (install defaults
were "Remember previous setting"/15 and 10). Group `File Type Buttons`: three rows edit + button:
`Audio` / `MP3 Audio - Any Bitrate`, `Video` / `Video (.avi .asf .mpg .mpeg)`, `Pictures` /
`Pictures (.jpg .png .jpeg .gif)`. Group `Results Display`: radios `Groups of duplicate results
collapsed by default` / `Groups of duplicate results expanded by default`.

### 7.9 File Transfers [obs: web/settings_file_transfers.png]
Group `Incoming Files Folder` with path text and `Change...`. Rows (check + label + spin + `seconds`):
`Purge inactive DL sources after [60]` (✔), `Clear incomplete downloads after [600]`,
`Clear complete downloads after [300]`, `Clear incomplete uploads after [300]`,
`Clear complete uploads after [300]`; no-check rows `Pending downloads timeout after [300]`,
`Running downloads timeout after [300]`, `Pending uploads timeout after [300]`,
`Running uploads timeout after [300]` (install default 45). Checks `Switch to Transfers window when
starting downloads`, `New downloads expanded by default`, `Suppress dangerous filetype warning (this
session only)`, `Suppress dangerous filetype warning (permanent)`. Full-width `Reset Defaults`.

#### Queuing [obs]
`Max total downloads [40]`, `Max downloads per user [12]`, `Max total uploads [3]`, `Max uploads per
user [4]` (spin boxes, right-aligned labels); check `When there are [2] or more uploads, change` /
`OpenNap reported connection speed to [14.4 ▼]`.

### 7.10 Bandwidth Throttle [obs]
`☐ Limit incoming bandwidth [15000] bytes per second`, `☑ Limit outgoing bandwidth [24000] bytes per
second`; text `Includes bandwidth used by all file transfers, WinMX Peer Network traffic and OpenNap
traffic.` / `If you have a broadband connection you should set outgoing bandwidth to about 85% of
total capacity for optimal` / `If you are using an ADSL connection that has very limited outgoing
bandwidth (128 kbps), you may have better results with a SECONDARY WinMX Peer Network connection.` /
`If you have a PRIMARY CONNECTION to the WinMX Peer Network, you can adjust the amount of bandwidth
allocated specifically for that connection by changing the value in the "WinMX Peer Network"
settings.`; separator; `Throttle Process Interval [200 ms ▼]` + `This setting alters the
'resolution' of the bandwidth throttle.  Please only change this setting if you're an EXPERT user.`;
separator; `BW Graph Rolling Avg. Duration [4 seconds ▼]`.

### 7.11 File Launching [obs]
`These settings affect the launching options in the context menus in the Transfers and Library
windows. (Leave these settings alone if you aren't sure what you're doing.)`; `Title (displayed on
menus)` combo + `Save` `Delete`; `( %path% will be replaced with the actual file path )`; fields
`File Ext`, `Action`, `Command`, `Params`, `Working Dir`; `DON'T FORGET TO PRESS SAVE IF YOU'VE
CREATED A NEW ENTRY`. Default entries: Open, Play, Enqueue, View (help text).

### 7.12 Miscellaneous [obs]
Group-less frame `When main window closed:` radios `Prompt` / `Minimize to tray` / `Exit`; toggle
push button `Start WinMX hidden in system tray automatically when computer starts` (2 lines);
separator; `WinMX Main Thread Priority [Normal (Recommended) ▼]` + `(Be VERY careful if you are
going to adjust this setting.)`

---------------------------------------------------------------------------------------------------

## 8. Adaptation notes (OctoMX: BitTorrent + eD2k/Kad + Gnutella + HTTP + own overlay)

Principle: keep every widget, position, colour and almost every label; change only words that would
be false. Where the backend has no equivalent, keep the control and give it the nearest meaning
rather than removing it. Proposed relabellings (old → new):

### 8.1 Global
* Title: `WinMX v3.53  <Online P=5 S=12 spm=4284>` → `OctoMX v<ver>  <Online P=<p> S=<s>
  spm=<n>>` where P = overlay peers we route for / connected "primary" links (incl. DHT routing
  table "good" nodes is too big, use overlay links + eD2k server + Gnutella ultrapeers count),
  S = incoming connections accepted, spm = search requests per minute we answered/forwarded.
  Secondary mode: `<Online h:mm:ss>`. Uploads suffix unchanged. OpenNap suffix becomes
  `<Networks: 4 nets online, 1 pending 1234567 users ...>` (users/files where known).
* News bar: our own status/news line ("Ready, 4 networks online"), same yellow-on-black.
* Page buttons: `Networks`, `Shared Files`, `Chat`, `Search`, `Hotlist`, `Transfers`,
  `Bandwidth`, `Ignored`, `Settings`, `Help`. [owner's choice, see §8.11: the row is ordered
  `Networks`, `Shared Files`, `Search`, `Transfers`, `Bandwidth`, `Chat`, `Hotlist`.]

### 8.2 Networks
* Sub-tab 1 `WinMX Peer Network` → **`OctoMX Peer Network`** (the classic label with our
  name): same page = overall status. Lamp: green when at least one network can search and
  download (`Ready` / `It is now OK to search and download.`), yellow while bootstrapping
  (`Finding network connections`), red when nothing connects.
* Connection Type: keep both radios and texts nearly verbatim; meaning: *primary* = accept incoming
  connections, run as overlay router / DHT full node / Gnutella ultrapeer-capable / eD2k High-ID;
  *secondary* = leaf/client-only (NAT, Low-ID). Replace "File listings and search queries will be
  routed through another user with a primary connection", still true for our overlay; keep.
* Status bar: `Online  5 Primary Connections  12 Incoming Secondary Connections  4284 / 4284 SPPM`
  → same fields with our counts.
* Sub-tab 2 `OpenNap Protocol Networks` → **`Other Protocol Networks`**. Same black server list, one
  row per network/server, OpenNap row grammar:
  ```
  [BitTorrent DHT]  router.bittorrent.com:6881   Ready (412 nodes)
  [eD2k]            eMule Security No1 45.82.80.155:5687   Ready (512341 users, 98123456 files) High ID
  [Kad]             (kad)                          Ready (firewalled: no, 1203 contacts)
  [Gnutella]        12 ultrapeers                  Ready (12 ultrapeers, leaf mode)
  [HTTP/Web seeds]  -                              Ready
  [eD2k]            DonkeyServer No2 ...           Waiting... (-2:30)                     (yellow)
  ```
  Buttons `Connect` `Disconnect` `Stop Attempts` `Add` `Edit` `Remove`; `Find .WSX`/`Import .WSX`/
  `Export .WSX` → `Find .MET` / `Import .MET` / `Export .MET` (server.met / nodes.dat import; the
  wording keeps the classic WinMX shape). Context menu identical incl. `View Server Messages` (eD2k server
  messages, tracker/DHT log).

### 8.3 Shared Files
Unchanged (Share Folder / Unshare Folder / Refresh / Shared File Types / Scan For Media; columns
File Path / Size / Bitrate / Time / Freq). Shared files are published on all networks that support
publishing (eD2k/Kad, Gnutella, overlay; BitTorrent only for torrents we created/seed).

### 8.4 Search
  The network rows start folded (their servers and peers open with the plus sign; owner's request:
  everything collapsed by default); a row the user opened stays open while it is refreshed.
* Keep all buttons and the legend. Network button: `All Available Networks` / - / `OctoMX Peer
  Network` / `BitTorrent (DHT)` / `eD2k Servers` / `Kad` / `Gnutella` (+ individual eD2k servers as
  `[eD2k] server name host:port`, mirroring OpenNap rows).
* File-type menu: [superseded by ui-f, see §8.11 "File types"] keep all entries; `Servers (.wsx)` →
  `Server Lists (.met .wsx)`; add `Torrents (.torrent)` just above `Any Non-MP3 File`. Colour Schemes
  (.clr) kept (we support .clr).
* Connection menu: keep the labels; meaning = estimated source bandwidth class (from overlay
  declaration, eD2k/Gnutella speed fields, or measured). For torrents use swarm health:
  `T3 or faster` ≈ ≥ 50 seeds ... `14.4K or faster` = any.
* Columns (keep names and widths):
  * `File` name; `Bytes` size; `Time`/`Bitrate`/`Freq` from metadata (ID3/eD2k tags/Gnutella
    XML), blank if unknown.
  * `User`: single source = user/peer name (`name_12345`, eD2k nick, `192.0.2.4:4662` if none);
    grouped = `<n> users` = number of distinct sources. For a torrent, the group row shows
    `<seeds+peers> users` and children are *networks/trackers* (`DHT`, `tracker.example:80`) rather
    than individual peers (do not list hundreds of peers).
  * `Speed`: single source = connection class as now (`Cable`, `DSL`, `Unknown` ...) with the same
    colours; grouped = `<hi> + <lo>` = fast + slow sources; torrent group = `<seeds> + <leechers>`
    (green + red), visually identical to the original WinMX.
  * `Ping`: RTT in ms where we can ping (overlay, eD2k, Gnutella), `?` otherwise.
  * `Status / Server`: single = `<free> of <slots> available` / `<q> in queue (0 of <n>
    available)` (eD2k queue rank, Gnutella busy, overlay slots); torrent = `<seeds> of <total>
    available` coloured by the same ≥2 / 1 / 0 rule; for server-based results the server, as with
    OpenNap: `[eD2k] server name`.
  * Group keys: same hash (eD2k/ED2K hash, BTIH, SHA1/TTH for Gnutella, overlay hash). Cross-network
    merge when hashes are known to match (e.g. eD2k hash + BTIH via metadata) → one group, children
    from several networks (this is exactly the "multi-point download").
* `HASH>` line accepts `ed2k://`, `magnet:` links and raw hashes.

### 8.5 Chat
* Tab `WinMX Peer Network` → `OctoMX Peer Network` (our overlay rooms; room name `<name>_<12 hex>`
  where the hex is the host's overlay ID suffix). `List Channels` menu lists our overlay plus, if we
  ever add them, IRC/Gnutella chat networks as `[Net] host:port` rows. All commands, colours and
  menus unchanged; `/commands` implemented by the room host per §3.3.2.
* User list Speed column = declared connection type (our overlay carries it).

### 8.6 Hotlist / Ignored
Unchanged. `Temporary` = people you messaged; `Online`/`Offline` = permanent contacts (we *can*
make overlay contacts permanent, keep the sections; the original WinMX's "temporary only" limitation
no longer applies). Entry template `<user> <network>`: network = `OctoMX`, `eD2k`, ...

### 8.7 Transfers
* Buttons unchanged. `Auto Find Sources - 10m` = re-query DHT/servers/overlay for sources every n
  min; `Auto Enter Queue - 100` = enter remote queues up to rank n (eD2k queue rank, overlay
  queues); for BitTorrent ignored (always "enter"). `Purge Users` removes dead sources; `Retry`
  reconnects a source.
* `User  -  AFS  -  AEQ` column unchanged. Children of a torrent download are **networks/trackers +
  top peers** (max ~10 visible, rest summarised as `+ 37 more peers`).
* Status mapping (keep the original WinMX strings and colours):
  `Finding sources...` (no peers yet / DHT lookup), `Waiting for Network Reply...` (tracker/DHT
  announce or eD2k callback pending), `Connecting to Remote User`, `Waiting for incoming
  connection...` (eD2k callback / overlay push), `Connected, Negotiating...` (BT handshake/
  bitfield), transferring = progress bar, `Remotely Queued <n>` (eD2k QR n; BT: choked but
  interested → `Remotely Queued`), `Busy (<n> Users in Queue)` (queue longer than AEQ),
  `Locally Queued` (over Max total downloads / per user), `Timed Out (...)`, `Connection Refused`,
  `File/User Unavailable` (source no longer has it / tracker 404), `Disconnected`, `Connection
  Reset by Peer`, `Both Users Firewalled` (both Low-ID / both NATed without relay), `File Mismatch
  (002/004)` (hash check failed, piece/part retried), `Complete`. Add only if needed: `Paused`
  (white), `Seeding` (blue, same as Complete, for torrents still uploading after completion),
  `Checking...` (yellow, hash check).
* Upload list: one row per remote peer actually receiving data (BT unchoked peers, eD2k upload
  slots, overlay/Gnutella uploads); `Queued` rows = eD2k/overlay queue entries.
* Status line strings unchanged.

### 8.8 Bandwidth
Keep the two graphs and the eleven colour slots; relabel the four WPN series (labels appear only in
Settings ▸ Appearance ▸ Bandwidth Window and in tooltips):
`WPN Primary` → `Overlay Routing`, `WPN Packets` → `DHT/UDP Packets` (BT DHT + Kad + overlay UDP),
`WPN Incoming Sec` → `Incoming Peers`, `WPN Outgoing Sec` → `Outgoing Peers` (protocol overhead of
peer connections), `OpenNap Traffic` → `Server Traffic` (eD2k servers, trackers, Gnutella
ultrapeers), `Uploads`/`Downloads`/`Other` unchanged.

### 8.9 Settings
* Tree labels: keep all. `WinMX Peer Network` → `OctoMX Peer Network` (Preferred Peer Cache = preferred
  bootstrap region; Allocated Bandwidth = overlay routing budget; search thread priority/duty cycle
  unchanged). `OpenNap Networks` → `Other Networks` (Default Login = default nick for eD2k/Gnutella;
  password; login timeout; "Limit simultanious connection attempts to"; "Do not import raw IP
  addresses from WSX files" → "...from server lists").
* Internet Connection ▸ Connection Type list unchanged (it is the speed class we announce).
  Incoming TCP / UDP pages: same texts; ports become our listen ports (one TCP + one UDP shared by
  all protocols, defaults 6699/6257 as in the original WinMX) plus an optional "Use UPnP/NAT-PMP" check
  (new, below the radios).
* File Transfers/Queuing/Throttle/File Launching/Misc/Appearance/Library/Channels/Search: unchanged.

* [str-1] Other Networks ▸ **Streaming API** (new page, last child of Other Networks): group `Streaming API
  (for players and other programs)`: `Switch on the streaming API` (off), `Port` (17580), `Allow other
  devices on my network` (off), `Access token` (read-only edit) with `Copy` and `New`, `Address` (read-only,
  the base address) with `Copy`, a two-line state (`Off: nothing listens.` / `Listening on 127.0.0.1, ::1,
  port 17580.` / `Port 17580 is in use by another program: the streaming API does not listen. Choose
  another port.`); group `Stream Cache (in the incomplete folder)`: `All stream caches at most [4096] MB`,
  `Delete unread streams after [240] minutes`, `Keep what was streamed as a download` (off); a closing
  line. Every change applies at once. The calls are in docs/API.md.
* [str-1] Transfers: a stream is a download row whose status starts with `Streaming:` (`Streaming: 1
  player, 10.2 MB buffered ahead`, `Streaming: a player waits for the bytes at 512.0 MB`, `Streaming:
  paused by the player, ...`, `Streaming: idle, no player (the cache goes in 3 h 59 min)`); Pause and
  Cancel work on it as on any download (Cancel deletes its cache, never to the trash). `Verify File` and
  `Restart Download...` are greyed for a stream row, the tool tip says why (its blocks are checked as they
  arrive; stop it and select the result again).

### 8.10 Things to keep identical even if "meaningless"
`Float/Dock`, the news bar, `Scan For Media`, `Ping` buttons, the three-colour legend, the chat
warning, `__INCOMPLETE___` prefix (we may use our own temp names, but show WinMX-style names in the
delete dialog), the exact status-bar strings.

### 8.11 Owner-requested modernisations (ui-e, 2026-11-21), deliberate deviations
Fidelity checks must treat these as intentional (the classic forms stay reachable where noted):
* **Page button order (owner's choice)**: `Networks`, `Shared Files`, `Search`, `Transfers`,
  `Bandwidth`, `Chat`, `Hotlist` (WinMX 3.53: Networks, Shared Files, Chat, Search, Hotlist, Transfers,
  Bandwidth). Ctrl+1...7 and Ctrl+Tab follow the new order (Ctrl+7 = Hotlist pane).
* **Sizes**: "Bytes" columns are "Size" with human-readable values (`45.7 MB`, `1.16 GB`, 1024-based,
  three significant digits), transfers `12.4 of 45.7 MB`, rates `47.1 KB/s` (column "K/s" → "Rate"),
  Time Left `4 min 34 s` / `3 h 12 min`, status lines `... at 449 KB/s total`, `74 files shared (3.20 GB)`,
  Bandwidth axis in KB/s / MB/s. Exact count in the tooltip. Settings ▸ Appearance ▸ Interface ▸
  "Show exact byte counts" restores every classic text (headers Bytes / K/s / Speed included).
* **Search columns**: File | Size | Time | Bitrate | Freq | Network | Peers | Status. Network = the
  networks that have the file (`BT`, `eD2k+Kad`, `BT+Gnutella`, `OctoMX`; index-only `LimeTorrents (BT)`),
  Peers = one sortable number (unknown `?`, sorted last), Status keeps the detail (`6 seeds, 4 peers`,
  `348 of 403 available`; source rows add the declared line type and ping). The classic User / Speed /
  Ping columns are optional (header right-click ▸ column chooser) and hidden at first; "Status / Server"
  is "Status".
* **Transfers**: the "Speed" column is "Network" (`BT+eD2k` on file rows, `eD2k, VDSL` on source rows);
  file rows count users from `sourcesTotal` (network / channel / "+ N more" rows are not users);
  completed downloads have "Open File" (default, = double-click; a bundle opens its folder) and
  "Open Containing Folder" at the top of the menu; "Copy Link" is "Copy Magnet Link" / "Copy ed2k Link"
  / "Copy Name" (also in Search and Shared Files).
* **Every list**: header right-click = column chooser + "Reset Columns"; columns stretch with the
  window (File takes the slack) until a column is resized by hand, then the layout keeps its
  proportions; type-to-filter box (Ctrl+F) on Search results, Shared Files, Browse, channel list;
  tool tips with the full text of elided cells. [ui-g: see "Columns fill the list" and "Filter
  results" below.]
* **Rows A / B**: the seven page buttons share the window width (94 px at the classic 682-px band,
  at most 150 px); the news bar fills row A up to Ignored / Settings / Help, which end with row B.
  Tab strips offer the window width before their slots shrink.
* **Interface scale** (Settings ▸ Appearance ▸ Interface, wizard page 1): Automatic (the display's
  scale setting; 125 % / 150 % on a ≥ 2560 / ≥ 3200-px screen at 100 %), 100 ... 300 %. 100 % is the
  pixel-exact classic layout; above it fonts are antialiased (ui/smoothFonts) and drawn from outlines.
* **Search box**: a drop-down arrow (and Alt+Down / F4) with the recent searches and
  "Clear Search History"; Ctrl+V outside the text boxes downloads a pasted link.
* **Feedback**: a bundle whose file list is unknown starts downloading at once (files can be chosen
  later with Select Files...); refused actions say why in the status bar / a message box; a completed
  download shows a tray notification (ui/notifyDownloadDone).
* **Icons and logo**: original pixel art in the same subjects and palette (not copies of the original
  bitmaps); drawn again at every interface size.
* **Connection Test** [diag-1] (not in the original WinMX; the owner asked for "a section where to check if the
  ports are opened / upnp working / forwarding working"): `ConnectionTestDialog`
  (`src/ui/dialogs/connection_test.*`), a non-modal classic gray window, default 760 x 540 design px
  (minimum 620 x 400, fits 800 x 600), one per application. Top: a 12-px lamp in the summary colour and
  the bold summary sentence; below it the last-test / progress line (progress bar on the right while
  testing). Black tree list (`diagList`, MxListView tree mode) with the columns `Check | Result |
  Details`, grouped General / one group per network / one group per unfinished download ("Download:
  <name>"); group rows bold, the group result runs on over the empty Details cell. Colours from the list
  palette: PingGood green = fine, PingMedium yellow = works with limits, PingBad red = broken, ListText
  white = unknown, InactiveText gray = off. Below the list a read-only text box (`diagExplain`) with the
  selected row in plain words and what to do. Then the check box `Hide my IP addresses (window and report)`
  (diag/hideAddresses, default on; [ui-h] masks the list and the explanation too, not only the report) and the buttons, right-aligned: `Test Now` (becomes `Cancel Test`
  while running), `Check Ports Online` (tooltip names the public checker; only asked on this press),
  `Copy Report`, `Save Report...`, `Open Log Folder`, `Close`. Entry points: `Connection Test...` in the
  Connection Status box of the OctoMX Peer Network tab (below the detail text, right) and at the end of
  the Other Protocol Networks button row (right-aligned when there is room); Settings ▸ Internet
  Connection under the four jump buttons (the restart note moved down one button pitch); Transfers
  download menu `Why Is This Slow?...` (opens the window on that download); the status cell tooltip of a
  download row ends with its one-sentence diagnosis. The wizard's Test Port first checks the port is
  free, then asks the router to forward it and (TCP) the public checker.
* **File types** [ui-f] (owner: "add possibility to search for all files type and update the extension
  list of the different types. Audio is not just mp3 ... video should contain also mp4, mkv"): one
  extension table in mxapi (`mx::extensionsForMediaType()`, `mx::mediaTypeForFileName()`, no extension in
  two types) feeds every label, filter and shared-type group. The file-type menu replaces the 2001 one:
  `All Files` / `Audio (.mp3 .flac .m4a .ogg ...) ▸` (Any Bitrate / (separator) / Bitrate at least ▸ / equal to ▸ /
  at best ▸, for all audio) / `Video (.mkv .mp4 .avi .mov ...)` / `Pictures (.jpg .png .gif .webp ...)` /
  `Documents (.pdf .epub .docx .txt ...)` / `Software (.exe .msi .apk .iso ...)` (installers, packages and
  disc images) / `Archives (.zip .rar .7z .tar ...)` / `Disc Images (.iso .img .bin .cue ...)` (added on the
  owner's request: a subset of Software, asked from every network and filtered by extension) / `Torrents (.torrent)` / (separator) / `MP3 only (.mp3) ▸` (the
  classic bitrate submenus) / `Lossless audio (.flac .wav .ape .alac ...)` / `Server Lists (.met .wsx)` /
  `Colour Schemes (.clr)` / (separator) / `Custom Extensions...` (small input box, space or comma separated,
  remembered in search/customExtensions; button `Custom (.mkv .srt)`). Labels show the first four
  extensions then `...`; the tool tip and the status bar (on hover and on choice) list them all. Dropped:
  `MP3/OGG Audio`, `OGG Audio` (→ Audio), `Text`, `HTML` (→ Documents), `Archive (.zip .iso .rar .exe)`
  (→ Archives), `Audio (.wav .au .aif .midi .mpc)` (→ Audio), `Any Non-MP3 File`, `Any File / Bitrate`
  (→ All Files). Button texts: `All Files`, `Audio (.mp3 .flac .m4a ...)` (as many extensions as fit),
  `Audio - At least 192 kbps`, `MP3 only - Any Bitrate`. The quick-button row is `All | Audio | Video |
  Pictures` in the classic 165 px (30 / 44 / 44 / 47 design px); the button of the current filter is
  latched (pressed, the dither face of "Available Files On Top"); Audio = all audio (the presets of
  Settings ▸ Search ▸ File Type Buttons still choose any entry). search/defaultType and the presets
  store codes (`any`, `audio`, `audio:min:192`, `mp3:eq:128`, `video`, `lossless`, `custom:mkv,srt`);
  every older text / code is converted once. Shared File Types has the same nine groups (Audio, Video,
  Pictures, Documents, Software, Archives, Torrents, Server Lists, Colour Schemes) instead of the eight
  of 3.53; the old ids keep sharing what they shared (mp3 / ogg / other audio → Audio, archive →
  Archives + Software, documents and torrents while "files of all other types" is on). The filter
  rule (`mx::hitMatchesFileType()`, used by the core, every network and the merged list): a file by
  its extension; a torrent / bundle by its index category, else by the type with the most bytes in
  its file list, else by its name's extension, else by strong words of its name (release tags such as
  1080p / x264 / S01E02 / 320kbps / lossless, extensions written as words: "Sintel 872p mp4", "[FLAC]");
  a type still unknown is kept by Audio / Video / ... (a torrent called just "Sintel" may be anything)
  but a plain file without a known extension is not; the
  extension entries (MP3 only, Lossless, Disc Images, Custom, Torrents, Server Lists, Colour Schemes) need the
  extension (a bundle: a listed file, else the name, an extension written as a word, else a matching
  category).
* **Close and minimise** [post-1] (owner: "minimize to icon only if the user wants (with X), with -
  just reduce to icon normally"): the minimise button is always an ordinary minimise to the taskbar
  (misc/minimizeToTray is read from old profiles but has no effect and no check box; "Always start
  minimized" also minimises to the taskbar, only the autostart `--minimized` start goes to the tray).
  The close button follows Settings ▸ Miscellaneous "When main window closed:" `Prompt` (default) /
  `Keep running in tray` / `Exit`; without a system tray it always exits (after the "active downloads"
  question), never leaving an invisible program. The close prompt (§5.5) reads `Exit OctoMX`
  (preselected, first) / `Keep running in the tray` / (downloads running) `Exit when downloads have
  finished`, check box `Do not ask again`. Tray and system menu "Hide" still hide to the tray on
  request.
* **Colour theme** [post-1] (owner: "add possibility to switch color theme from dark to light"):
  Settings ▸ Appearance starts with `Color theme:` and a drop-down `Dark (classic)` (default) / `Light`
  / `Follow system` (ui/colorTheme); the rows below moved down 28 px and "Search For Color Scheme
  Files" is one button high. Quick entry without touching the button rows: `Color Theme ▸` in the tray
  menu (after Reset All Windows) and a right-click menu on the Settings button (`Settings...` /
  `Color Theme ▸`; a left click still opens Settings). The theme changes every list, chat view, filter
  box, graph and legend swatch of every window at once and is remembered; the gray chrome, the
  tooltips and the Networks status lamp stay the same. Light = white lists, black text and a darker
  counterpart of each state colour (at least 7:1 on white, 4.5:1 on the #d0d0d0 selection; table in
  `mx::colorSchemePreset(true)`); colours the user changed are kept when the theme switches. The two
  "Load Standard ... Defaults" buttons choose the theme and reset every colour.
* **Help button** [post-1]: a click pops a small menu `About OctoMX...` (bold default) / `Check for
  Updates...` (F1 still opens About directly); the About box has `Check for Updates...` left of OK.
  The update box is a classic message box titled `OctoMX`: "OctoMX 5.10 is available (you have 5.9)."
  with the first lines of the release notes as plain text and `Open Download Page` / `Remind Me Later`
  / `Skip This Version`. Settings ▸ Miscellaneous ▸ System Integration has an `Updates` group with
  "Check for a new version at start (at most once a day)" (updates/checkAtStart, default on).
  [upd-1: extended by "Updates that install themselves" below; this box stays as the
  `notify` mode and as the fallback whenever OctoMX cannot install a release itself.]
* **Updates that install themselves** [upd-1] (owner: "update should be automatic if possible,
  without manual download"): the `Updates` group now holds four radios for updates/mode, each with a
  tool tip: `Install new versions automatically (recommended)` (auto, default) / `Ask before
  downloading a new version` (ask) / `Only tell me, I install it myself` (notify, the classic box) / `Do
  not check at start` (off). A profile with the earlier check box off gets `off` (migrated once).
  auto: the start check downloads a newer signed release quietly, then a small box titled `OctoMX`
  (information icon, plain text) says "OctoMX 5.10 is ready to install (you have 5.9)." with the
  first lines of the release notes, "The download was checked against the OctoMX release
  signature.", with running downloads "Your 3 running downloads continue after the restart.", and
  `Restart Now` (default) / `Later` / `Skip This Version`; Later (also Escape or the close button)
  installs it when OctoMX is closed and a tray balloon says so once. ask, and always for Help ▸ Check
  for Updates...: "OctoMX 5.10 is available (you have 5.9)." with `Update Now` (a box with a progress
  bar, `Hide` / `Cancel`, then the ready box) / `Remind Me Later` / `Skip This Version` and `Open
  Download Page` as a link below the text. When the program cannot install a release itself (not a
  published package, a folder it cannot write, a release without signature, a download or check that
  failed), the classic box appears with the reason above the last line. The first start after an update
  shows "Updated to OctoMX 5.10 (from 5.9)." with `What's New` (the release page) / `OK`. The About
  box has one line under the description: "OctoMX 5.9 is up to date." / "OctoMX 5.10 is available
  (you have 5.9)." / "... is ready to install ..." / "..., not checked for updates yet.".
  Deviations: these boxes are not part of WinMX 3.53 (it had no updater); they are drawn as classic
  message boxes with our own buttons. Deliberate choices: the staging folder is inside the program
  folder (`.new-<version>`, same volume, only the program folder must be writable) instead of next
  to it; the restarted program gets the same options but not the links of the first start (they were
  added already).
* **Language file** [ui-g] (owner: "Load Language From File..." did nothing): Settings ▸ Appearance
  `Load Language From File...` takes a Qt translation (`.qm`, filter `Qt Translation Files (*.qm)`), stored in
  misc/languageFile and installed with QTranslator at start (before any window) and at once when it changes;
  windows built afterwards use it, a restart shows every text (the tool tips say so). A file that is no
  translation is refused with a message and the setting keeps its value. `Reset Language To English` removes
  it. The original WinMX 3.x `.lng` format is not read (dropped from the filter). A translator makes the file with
  `lupdate src -ts octomx_xx.ts`, translates it in Qt Linguist and runs `lrelease octomx_xx.ts` (README).
* **No third-party sites as ours** [ui-g] (owner: "We should not have any link pointing to winmx.com
  that is not our"): every link the program shows for help or news points to the project page
  https://github.com/robtacconelli/OctoMX (Appearance: "For more information about language files,
  visit:"); the demo data uses our own names (room `OctoMX Help`, bots `HelpBot` / `NewsBot`, links to the
  project page). Functional endpoints the user can change stay (eD2k server.met / nodes.dat sources,
  GWebCaches, trackers, DHT routers, the port checker named in the Connection Test).
* **Gnutella encryption** [enc-1] (owner: "encryption/obfuscation ... must be actived by default ...";
  later narrowed to "just Gnutella could be a good idea to add encryption"): Settings ▸ Gnutella, group
  Gnutella Network, a combo `Encryption` (`Prefer encryption` default / `Require encryption` / `Disable
  encryption`, key gnutella/encryption = enabled / forced / disabled, like BitTorrent's) with a tool tip
  saying what TLS protects and what not. No wizard step and no global encryption setting (owner's
  choice). Connection Test: every network group gets a row `Encryption` / `Obfuscation` (BitTorrent
  `12 of 30 peers encrypted`, eD2k `Server link obfuscated` + peer figures, Kad packet figures, Gnutella
  `TLS on 1 of 3 ultrapeer links` + transfer figures, OctoMX / Rats `Always on`, web indexes `HTTPS for
  6 of 6 indexes`). Gnutella source and upload rows add ` [TLS]` to the client name (User column tool
  tip), as eD2k rows show `[obf]`; BitTorrent rows already show `uTP (encrypted)` in the Network column.
* **Columns fill the list** [ui-g] (owner: "the columns have to occupy the entire width, without
  leaving white space on the right"): in every MxListView the visible columns end exactly at the right
  edge of the viewport, at every size and scale, and there is never a horizontal scroll bar. Dragging a
  column border makes the columns to its right give or take the difference in proportion (each keeps
  20 design px, or its default if smaller); the last border moves the last two columns; when the others
  cannot give more the drag stops. Hide / show / reorder / Reset Columns, window size, splitters, the
  Hotlist pane and the vertical scroll bar keep it; layouts stored earlier with a gap or an overflow
  are scaled to the width when loaded (proportions kept). The Search list no longer keeps the
  WinMX 3.53 strip of 29 px for the scroll bar right of Status (qa-1 measured it on search_idle_354b4):
  the columns reach the edge with or without the scroll bar. Cause of the owner's gap: before this a
  border dragged to make a column narrower (or an old layout stored in pixels) left the rest of the row
  empty, and that layout was saved as fractions that no longer added up to the width.
* **Filter results** [ui-g] (owner: "possibility to sub-search ... like emule does"): every Search
  tab has an always visible row `Filter results: [ words, -exclude, "a phrase" ] [Clear]  37 of 165
  shown` directly above the result list (beside the colour legend when the window is wide enough, else
  its own slim row under the legend). Words must all appear, `-word` hides rows with that word, quotes
  make a phrase, any case; it looks at the file name, User, Network and Status (so "BT", "eD2k", "flac"
  work); a file row stays when one of its source rows matches. Live while results arrive, per tab, kept
  when sorting and by a new search in the tab; the status bar says `37 of 165 results shown (Filter
  results: ...)`; hidden rows are deselected, so Download acts only on what is shown; the tab's CLEAR
  bar and the Clear button empty it, Esc in the field too, Ctrl+F jumps to it, Enter goes to the list.
  The channel list keeps its classic Filter well as its always visible filter (Ctrl+F now goes there);
  Shared Files and Browse keep the Ctrl+F bar, with the same matching rules.
* **Search form** [ui-g] (owner: "Artist and / or Title must be in that way only when Audio is
  selected ... restructure the search section because we've plenty of space with bigger
  resolutions"; earlier "add also archive and cd-image"): the label above the two keyword lines follows
  the file type: `Artist and / or Title` (audio, MP3, lossless), `Title of the film or show` (video),
  `Words in the picture's name`, `Title or author` (documents), `Program name` (software), `Name or
  words` (archives, disc images, torrents), `Server list name`, `Colour scheme name`, `File name or
  words` (All Files, custom). Both lines are keywords that must all appear (the second may hold a link or
  a HASH> line); outside audio the second line shows the grey hint `more words (optional)`. From a page
  width of 975 design px the form is wide: the left block grows by 266 px (wider keyword lines and
  network / connection menus) and the quick types row reads `All | Audio | Video | Pictures | Documents |
  Software | Archives | Disc Images` (70 / 60 / 60 / 76 px) before the type menu button (which still
  offers every choice); the button block keeps its 3.53 grid and moves right with it. Below 975 px (800 x
  600) the classic rows stay. At 1024 x 768 the form is therefore wider than the 3.53 reference.
* **Tool tips** [ui-g] (owner: "for each button ... add tooltips to explain the user what the action
  does"): every button of every page, pane, floating window and dialog, the non-obvious check boxes and
  radios, and the context-menu entries that need it say in one or two plain sentences what happens
  (menus: QMenu tool tips plus the same text in the status bar while hovered). Long tips are wrapped at
  320 design px and shown below the widget, never over it, in the classic yellow tip.
* **Hotlist** [ui-g]: an empty Hotlist shows one grey line of help in the list (`Right-click a user
  in a chat room, a search result or Transfers and choose Add To Hotlist ...`); a user added while online
  shows Online at once (the core asks a whois); "Notify when Hot List users come online" (hotlist/
  notifyOnline) now works: status bar note and tray notification, not for an entry added a moment ago.
* **Confirmations name the row** [ui-g]: Transfers ▸ Cancel asks `Cancel the download of
  '<file>'?` (several: the list of names) like Delete File; Hotlist ▸ Remove names the user.
* **Speed limits one click away** [bw-1] (owner: "the speed limiter that is configurable for both
  upload and download"): the Bandwidth page has a face-coloured row above the two graphs (3 px below
  the page buttons, 22 px): `Limit download [x] [value] KB/s   Limit upload [x] [value] KB/s`, a grey
  note (`Schedule in force until 18:00: DL ..., UL ...`, `Your limits win until 18:00, then the schedule
  applies again`, `Schedule starts Mon 08:00`, or `LAN peers are not limited`) and a `Settings...` thin
  button (Settings ▸ Bandwidth Throttle). The graphs below keep the classic layout. Each graph draws the
  limit in force as a dashed line (text colour dashes, background colour gaps, so it shows over any
  series) with `Limit 500 KB/s` at its right end; while the traffic stays below a third of the limit the
  axis does not stretch to it and only `Limit ... (far above)` shows at the top. The status bar adds
  `(limit 500 KB/s)` after each rate; the title (and so the tray tool tip) adds ` - DL limit 500 KB/s,
  UL limit 50 KB/s` (` (schedule)` when the schedule sets them) whenever a limit is in force. The tray
  menu has `Speed Limits ▸` after `Color Theme ▸`: `Speed Limits On` (switches both limits off and back
  on, the values and which of the two were on are kept), `Download: <in force> ▸` and `Upload: <in
  force> ▸` (`Unlimited`, four presets from the connection type, 10 / 25 / 50 / 75 % of a typical line,
  plus the last custom value, `Other...` for a typed value), the schedule state and `Resume Schedule`
  while the user's limits win, `Bandwidth Settings...`. Values are KB/s (1024 bytes) everywhere; the
  spin boxes and `Other...` also take `2.5 MB`, `800 KB`, `1 MB/s`. Upload limits are at least 10 KB/s
  (tool tip says why: tit for tat / eD2k credits). Who wins against the schedule: the latest action. A
  limit changed during a schedule window (row, tray, Settings, script) wins until that window ends;
  editing the schedule hands control back to it at once. Settings ▸ Bandwidth Schedule says what is in
  force now and why.
* **Download order and Preview** [dl-1] (owner: qBittorrent's two flags, and "preview a file ...
  let the user choose the player the first time"): the download file-row menu has two checkable entries
  after `Priority ▸`: `Download In Sequential Order` and `Download First And Last Pieces First` (checked =
  every selected unfinished download has it; a click switches it for all of them; greyed for a completed
  download). A download with a flag shows a small mark after its name (`  [Seq]`, `  [F+L]`,
  `  [Seq F+L]`) and the File / Status tool tips say `Download order: in sequential order, first and last
  pieces first. Followed by: BitTorrent, HTTP.` (or that no connected source can follow yet). Defaults for
  new downloads: Settings ▸ File Transfers ▸ Downloads (`Download new files in sequential order`,
  `Download first and last pieces of new files first`, both off); Add Link has the same two check boxes
  (dialog 420 x 236, buttons moved down). The classic incomplete-download block `Open` / `Play` (greyed) /
  `Open Containing Folder` gets `Preview` after `Play` for an audio / video file: `Preview` (single file),
  `Preview...` (a bundle: a chooser `Preview` lists its media files with progress and `Ready` or why not),
  or the reason when the file cannot be played yet (`Preview: needs the beginning of the file`,
  `Preview: needs the end of the file`); clicking that reason switches both flags on for the download (the
  entry is greyed when both are on already). Not offered for other file types or when Settings ▸ File
  Launching ▸ Launch Options ▸ `Allow previewing incomplete files` is off. The first preview asks `Preview
  With`: `The system's default player` / `This program:` [editable list of the players found] [`Browse...`],
  `Remember my choice` (checked); Settings ▸ File Launching ▸ `Preview Player` (new sub page) changes it
  (`Ask the first time` / system / program + path, lists the players found). A completed audio / video
  file adds `Open With Preview Player` after `Open Containing Folder` (Open File keeps the system
  default). Double-click is unchanged (a running download expands / collapses; the original WinMX had no preview on
  double-click, and a modal player question on a double-click would surprise).

* **Verify File and Restart Download** [ver-1] (owner: "reverify the file once download finished or if
  paused, or to restart it from the beginning if user wants. In case of verification, parts/blocks damaged
  have to be redownloaded"): the download file-row menu has `Verify File` and `Restart Download...` right
  after `Pause` / `Resume` (menu only: the two button rows keep the classic layout). Both act on every
  selected download that can; a greyed entry's tool tip says why (`Cannot be verified now: Nothing of it has
  been received yet.`, `... It is being verified now.`). `Verify File` is offered for a finished row, a
  paused, stopped (error, timed out) and a running one (a running download stops fetching for the check and
  goes on by itself): every block on disk is read again and checked against the block hashes the download
  knows (torrent pieces, eD2k parts, tiger tree leaves, the block hashes a file keeps from its final check);
  the row says `Verifying 42%...` (`(1 part damaged so far)`); Pause stops the check. Damaged blocks, and
  only those, are downloaded again (a finished row becomes unfinished until they are back, its file is not
  shared meanwhile and stays where it was, with its name). The answer is the page's notice and the row's
  status text: `Verified: 'name' is intact.`, `Verified: 2 parts of 'name' (19.5 MB) are damaged; they are
  being downloaded again.` (a paused one: `... when you resume it.`), `Cannot verify 'name': the finished
  file is no longer at <path> ... Restart Download fetches it again.`, `... was changed (it has 1.2 MB, the
  download 1.3 MB) ...`, `'name' cannot be verified: no checksum is known for this file.`, and for damage only
  a whole-file checksum shows: `... the damage cannot be located yet. OctoMX asks the networks for block
  hashes ...`, then either the repair or `'name' is damaged, and no network gave block hashes ... Restart
  Download fetches it again.` (the row `Complete, but damaged: Restart Download fetches it again`, not
  shared). `Restart Download...` is offered for every download and always asks (`Restart Download`, buttons
  `Yes` / `No`, default `No`): `Restart "name" from the beginning?` with one line per download naming what is
  thrown away (`the finished file goes to the trash`, `the 12.3 MB received so far go to the Recycle Bin`, or
  `... are deleted for good (there is no trash for them)`, a file the download did not create `stays on the
  disk`). Yes: the same row (same id, sources, folder, name, priority, wanted files, order flags, AFS / AEQ)
  starts from zero, running even if it was paused; the notice `'name' starts again from zero: its old data
  went to the trash.` Script driver: `verify-file <name part>`, `restart-download <name part> [yes|no]`.
* **[ui-h] No control that does nothing.** Every visible setting either works (proven by a test or
  a reader the settings registry `tests/ui/settings_registry.h` names) or is off the UI; its key stays
  readable (hidden in the schema) so old profiles load. Removed from the original pages, because
  nothing in OctoMX reads them and they have no sensible meaning today:
  - Internet Connection: the `In/Out UDP Packets` page (the OctoMX network uses the TCP port number
    for UDP too, every other network has its own port on its page) and the `Host Name Resolution` page
    (names are looked up by the operating system; the SOCKS5 host name switch stays). Their two jump
    buttons are gone, `Connection Test...` moves up. The Setup Wizard has no UDP page any more.
  - OctoMX Peer Network: `Preferred Peer Cache` and the whole `Primary Connection Advanced Settings` box
    (allocated bandwidth, search thread priority and duty cycle, the slow-CPU switch) and its Reset
    Defaults; a short text says why. `Connect automatically on startup` stays.
  - Other Networks: the default password, the login time out, the connection attempt limit and the raw
    IP import switch (no network logs in with a password). Default Login stays (eD2k nick).
  - File Transfers: six of the nine rows (purge inactive sources, clear incomplete downloads / uploads,
    running download, pending / running upload time outs); the check boxes below move up.
    Queuing: `Max downloads per user`. Bandwidth Throttle: `Throttle Process Interval` (the limiter
    measures time continuously). Miscellaneous: `WinMX Main Thread Priority`.
  - Library: the editable `Available Filetypes` list becomes a read-only list of the type table with a
    `Shared File Types...` button (the dialog where sharing per type is chosen).
  - Appearance: the colours `Already Shared`, `Private Message` and the OpenNap user list colours
    `Server Traffic` / `Other`; the Channel Window page shows the user list colours that are used
    instead (`User Names`, `Admins (@)`, `Voiced (+)`).
  - Pages of our own: connection cap (all networks), eD2k servers from clients, chat on / off, web back-ends and
    local index, logging chat rooms and private messages, Group identical files, Ping results, the
    stall time out, folder per bundle, clear cancelled (a cancelled download is removed at once),
    graph minutes, warn before opening programs (the same as the dangerous file type switches),
    one copy only, prevent sleep, list font size, language code.
* **[ui-h] Made to work instead:** the Incoming Message Alert (Quiet / System Beep / new `Built-in
  Sound` / .wav, plus a `Test` button), `Play sounds`, `Play a sound when a download completes`, a new
  `Play the message alert when a Hotlist user comes online`; the chat warning's `Do not show this
  warning again` box (18 px taller dialog); Groups expanded by default; New downloads expanded (default
  now on, as the page always behaved); Switch to Transfers (downloads from Search / Browse; Add Link and
  pasted links always showed Transfers); the double-click action; the File Launching entries in the
  menu of a complete download (entries with a program; on Windows also a shell verb, never a second
  Open) and the Launch Options programs; Always on top; Flash the taskbar; Show notifications in the
  tray (a master switch for the balloons); the last Settings page; grid lines and the Grid, Selected
  Text and Column Header Text colours; the chat user list background, admin and voiced colours; log
  size and files kept; Rats Search maximum results and bootstrap peers; the Queuing "change reported
  connection speed" (on the OctoMX network, from its running uploads); "Unable to accept incoming
  TCP connections" now also marks the OctoMX network firewalled.
* **[ui-h] Lists:** the chat user list keeps column minimum widths from its content (the Speed column
  never cuts "Unknown"), the list and its splitter keep the width they need; the channel list's Users
  column likewise. **Update boxes** use OctoMX's own information sign on every platform.
* **Same file on several networks, matched by name and size** [merge-1] (owner: "why the files are
  separated? From size+name seems to be the same file", then "apply this file name / size rule to all
  networks if the same hash is not available"): Search shows one row for hits of any networks (torrents
  with one file included) with exactly the same byte size, the same extension and the same name by the
  one rule (`SearchMerger::sameName`, 60 % of the words); the Network column names all of them
  (`BT+eD2k+Kad`), Peers is their sum. The child row of a copy matched only by name and size has
  `same name and size, checked while downloading` at the start of its Status; the parent row's tool tip
  (File, Network, Status) says `Probably the same file on <networks>: same name and exact size, but no
  common hash.` / `Every block is checked while downloading; a network whose copy differs is dropped.`
  No new colour: the text is the mark. A folder torrent that holds such a file is a child row of the
  plain file, `BT (inside "<torrent name>")`, and the folder's own row stays, its tool tip listing
  `Also found elsewhere (same name and size): <path> (<networks>)`. Transfers: such a source says
  `(same name and size, checked while downloading)` after its status until a verified block confirms it,
  then `(confirmed)`; extra rows `The eD2k copy is a different file: dropped (3 sources failed the check,
  1.2 MB fetched from it)` / `The eD2k copy is the same file: confirmed by BitTorrent piece hashes`;
  `Search by name and size` says how often it searched, when it searches again and which hashes check
  the blocks, followed by one row per network (`By name and size: eD2k`: `2 copies with the same size
  and name, 1 delivering`, `none (searched twice)`, `Skipped: not connected`); a network asked for
  sources that has none says `Asked, nobody has this file yet (asking again in 9 min)` instead of an
  endless `Waiting for Network Reply...`. A completed file whose other copy disagreed at the end says
  `Complete, but the eD2k copy was not the same file (its eD2k hash differs; this file is complete by
  BitTorrent piece hashes)`. With the setting `search/mergeByNameAndSize` off (default on), only plain
  files with equal normalised names merge (a bundle never meets a non-bundle).
* **The name OctoMX** [ren-1] (owner: "as a name I choose OctoMX"; "OctoMX is the only version and
  name we use"): every place where the program names itself says OctoMX (title `OctoMX v5.9`,
  `About OctoMX...`, `Exit OctoMX`, message box titles, the `OctoMX Peer Network` tabs and pages, the
  network name `OctoMX` in the Network columns, the default room `OctoMX Lobby`). Where it describes
  itself it says that its interface is in the style of the classic WinMX, with the statement that it
  is independent and not affiliated; it never names an earlier name or talks about earlier versions.
  Labels, menu names and column titles of the original stay as they are; longer texts that were
  taken word for word from WinMX 3.53 (the chat warning, the close prompt, the dangerous-file
  warnings, the connection type explanations, the restart notes and port explanations of the
  Settings pages, the Bandwidth Throttle and File Launching notes, the folder dialog caption, the
  Connection Status texts, the demo chat room) are said in our own words with the same meaning. The
  **About box** is 520 x 346: "OctoMX: file sharing with the classic WinMX-style interface on modern
  peer-to-peer networks.", the networks, and the statement that OctoMX is independent of the
  original WinMX. The first start after an update shows `Updated to OctoMX <v> (from <old>).` with
  the plain version numbers, also after an update from a copy installed before 5.0.
* **The download bar is a map of the file** [ui-i] (owner: "the download bar, in the same style, instead of
  being incremental, shows the parts downloading/downloaded. Of course the bar should occupy the 100% of the
  column width"). The classic progress cell (6-px block, 2-px gap, a bar from row top + 2 to the row bottom
  with a 1-px outline in `colors/progressBar`) now runs from the cell's left edge to its right text margin
  (2 px), in both lists, at every interface scale, following the column when it is resized. On a download
  row the bar shows the file from its first byte (left) to its last byte (right, a bundle: its wanted files
  laid end to end), one flat colour per pixel column: verified parts `colors/progressBar` (`#0080FF`;
  light theme `#0050B0`), parts arriving right now (requested from a source and being received, never merely
  planned) the new colour `colors/progressActive` (`#00FF00`, the transferring green of the original WinMX;
  light theme `#2EA02E`; Settings ▸ Appearance ▸ Transfers Window ▸ `Parts Arriving Now`, which made that page
  two columns, so `Finding Sources / Connecting` is now `Finding / Connecting`), missing parts the bar
  background, and a column whose cells are only partly verified one intermediate tone, a third of the way
  from the background to the bar colour (dark `#002B55`, light `#AAC5E5`): never drawn full while a byte is
  missing, never empty while a byte is there. A column that touches a cell in progress is drawn green, so a
  single part in progress stays visible at any width. The percentage (or, for a paused, queued, waiting or
  checked download that has data, its status text in its state colour) stays centred in the bar, inverted
  as before: the text colour over the missing and partly there parts, the background colour over the
  verified parts and the outline, black (light theme) / background colour (dark theme) over the green.
  Deviations from the classic look, and why: the bar is no longer 68 px but the column's width; it shows
  where the data is instead of how much; one more colour (green) for "arriving now" and one derived tone for
  "partly there" (one tone, not a gradient: every column stays one flat colour, so the inverted text keeps
  whole strokes; filling partial columns from the bottom was tried and drawn as a wave that broke the
  digits). Rows without a map keep the incremental fill: uploads, and downloads whose size or metadata is
  not known yet. A paused, queued, waiting or checked download that has data shows its bar with the status
  text inside (classic: text only); errors keep their red text running over the empty cells, finished rows
  say `Complete`. The status tool tip starts with `412 MB of 1.09 GB (38 %), 6 parts downloading`
  (parts = runs of cells in progress). The core reports the map in `TransferInfo::pieceMap` / `pieceFill`
  (512 cells: state and verified share per cell).
* **Removing a download asks what happens to its file** [ui-j] (owner: "when you delete a transfer
  (completed or not) to keep the file (complete or partial) or to delete both, row and file"). Every way
  a download leaves the list (row D `Cancel` / `Clear`, the file-row menu `Cancel` / `Clear` /
  `Delete File...`, Del, a multi-selection: one question for all) shows one classic question,
  `Remove Download` / `Remove Downloads`: it names the file (or `these 5 downloads`, the first six by name,
  `and 3 more`), says what is on the disk (`The download is finished. The file (263 MB) is in ...`,
  `The download is not finished: 120 of 263 MB are on the disk, in ...`, `On the disk: 1.5 GB in 2 finished
  files and the partial data of 3 downloads.`), what keeping does and, in bold, what deleting does
  (`moved to the trash` / `Recycle Bin`, `where you can restore it`, or `deleted for good. It is on a drive
  without a Recycle Bin and cannot be restored.` with the warning icon), with the buttons
  `Remove from the list, keep the file` / `Remove from the list and delete the file` / `Cancel`
  (plural `files` for several) and `Do not ask again`. Cancel is the default button (Enter and Esc
  change nothing); the delete button is off when nothing is on the disk. Settings ▸ File Transfers
  `Removing a download: [Ask what to do with the file | Keep the file | Delete the file too]` is the
  remembered choice; `Delete File...` asks every time, and a delete that would be for good (no trash) or
  that would delete a file the user chose for Manually Resume / Load Incomplete is always asked. Keep:
  a finished file stays; partial data stays where it is and OctoMX remembers the download
  (`<profile>/kept-downloads.json`): Load Incomplete lists it by its name (`Removed download, 120 of
  263 MB kept`) with its link, the Search mark `Matches incomplete local file` (yellow) shows it, and
  downloading the same file again (or Resume in Load Incomplete) brings the download back with what it
  had (part files: the range map; torrents: libtorrent checks the data it finds). Delete: only what the
  download created goes (the finished file, or the part file / the torrent's files and libtorrent's
  `.parts` file, the folder of a folder torrent as one item when the download created it and nothing
  else is in it, else its files and the folders they leave empty), with its resume data and its record
  in the profile; files that were there before the download, a row that only points to a library file
  (`Complete (already in your library)`) and a finished file whose size changed since are never deleted
  (the question names them). The classic `Confirm File Delete` (`Are you sure you want to send ... to
  the Recycle Bin?` `Yes` / `No`) and the setting `Confirm cancel` are gone: this question replaces both.
  `Clear Complete` (and the automatic clearing of finished downloads) keeps its classic meaning: it tidies
  the list, never asks and never deletes a file; the upload buttons never touch the shared files. Del on
  the download list is `Clear` (it was `Cancel`, which ignored finished rows).
* **Shared Files ▸ Delete File... / Delete Folder...** [ui-j] (owner: "in shared files, give the
  possibility to delete a file or folder"; not in 3.53, which could only unshare). In the menu of a file
  row (`Delete File...`, or `Delete 3 Items...` for a selection), of a folder row (`Delete Folder...`)
  and with Del. The question (`Delete File` / `Delete Folder` / `Delete Shared Folder` / `Delete Files`)
  names the file, or the folder with its number of files and their size, says `It leaves OctoMX's shared
  files and the disk` and whether it goes to the trash / Recycle Bin or is deleted for good; for a shared
  folder (one added with Share Folder) the path is in bold, `Deleting it removes the share AND deletes
  its content (...) from the disk`, and `To stop sharing it without deleting anything, use Unshare
  Folder instead.` Buttons `Delete File` / `Delete Folder` / `Delete 3 Items` and `Cancel` (the default).
  Refused with the reason (nothing deleted, no question): anything outside the shared folders or reached
  through a link that leads out of them; the incoming folder itself or a folder that holds it; the folder
  of unfinished downloads, anything in it or holding it; the profile and the program folder likewise; the
  data of a download still in the Transfers list (unfinished, or a seeding torrent: `Remove that download
  there first: it can delete its file too.`); a file being uploaded right now (`... is being sent to
  <user> right now. Cancel that upload on the Transfers page, or wait until it has finished`: refused,
  never cut off). The library forgets the deleted files at once (the page, what others browse and find).
* **Search** [ui-j]: the results filter hides a merged row that has an excluded word even when one of its
  source rows does not (so `-eD2k -Kad` or `-debian` alone filter; before, a source row without the word
  kept its parent on the list). Source rows of eD2k / Kad say `4 sources (2 complete)`, never `seeds` and
  `peers` (BitTorrent rows keep `2 seeds, 2 peers`).
* **[set-1] Every setting checked by its effect** (docs/status/set-1.md has the key-by-key table). Changes a
  user sees:
  * Appearance: the three `Ping` colours of the Search Window page colour the Connection Test results:
    they moved to the Main Window page as `Connection Test: OK / Warning / Problem`; `Network Name` moved
    from the Search Window page to the Transfers Window page, where it is used (the network a source row names).
    Lines already in a chat room, a message window or Server Messages take a changed colour at once.
  * Networks page, Connection Type: the texts say what the choice changes (the status line and the Host
    button); the OctoMX Peer Network has no primary / secondary roles. Peer Network Options:
    `Primary connection (full status, host chat rooms)`.
  * Upload Queue: `Only upload to users on my Hotlist (OctoMX Peer Network)`; Downloads: the `Look for
    sources every` row is gone (Auto Find Sources on the Search page sets it, nothing else read it).
  * Every setting that waits for the next start says `Takes effect after a restart.` in its tool tip, the
    language file too (windows opened later use it at once).
  * The Setup Wizard's network and association boxes show the current settings (they were always ticked);
    a mixed set of associations shows partly checked and is left as it is.
  * The Browse window asks before downloading a program, script or archive, like Search (one list of
    risky types for Search, Browse and opening a file).
