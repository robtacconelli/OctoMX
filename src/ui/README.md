# src/ui: the classic WinMX look (target `mxui`)

Everything visible. Links **only** `mxapi` + Qt Widgets: the UI talks to `mx::Core` and its
services (`src/api/mx/services.h`), never to `mxcore` or a plugin. The demo core (`src/mock`,
`mx::createMockCore`) implements every service with frozen-able demo data for screenshots/tests.

```
kit/        classic look: style, palette, fonts, icons, list view, small widgets, status bar
models/     item models (search results, transfers, servers, hotlist) + custom roles
pages/      main-window pages (Page subclasses)
dialogs/    About, Change Screen Name, Server, Add Link, bundle file selection
main_window.*   rows A/B, page stack, Hotlist pane, status bar, Float/Dock, stubs
hotlist_pane.*  the docked Hotlist side pane
script_driver.* the --script automation driver
res/        bundled fonts (derived Wine Tahoma) and licence text
```

## The look in one paragraph

`installClassicLook()` (kit/theme.h) installs `ClassicStyle` (a `QProxyStyle` over Qt's "Windows"
style), the fixed Windows-2000 palette (face `#D4D0C8`, see `classicPalette()`) and the UI font:
**"OctoMX Tahoma" 11 px (= Tahoma 8 pt), no antialiasing** (the family name inside the bundled font
file; a stored font setting with its legacy family name "WinMX Tahoma" is read as Tahoma). That font is Wine's Tahoma with its
embedded-bitmap line metrics fixed to the Windows ones (`res/fonts/patch_tahoma.py`), so text has
the same baseline and 13-px line pitch as the original WinMX on Windows. On Windows the genuine system Tahoma is
used if present (detected by its 11-px ascent; Wine's copy is not genuine). Use the helpers:
`normalFont()`, `boldFont()`, `largeFont()` (news bar / headlines), `classicFont(px, bold)`.

**Scaling** ([ui-e], `kit/scale.h`): Qt's high-DPI scaling is off (`QT_ENABLE_HIGHDPI_SCALING=0`,
`AA_Use96Dpi`, set in `preApplicationSetup()`), the kit scales itself by `uiScale()` (setting `ui/scale`:
Automatic from the screen's logical DPI, or 100-300 %; applied at start, before any window). All
geometry is written in **design pixels** (as measured on the references) and converted with `S(px)`,
`SR(x, y, w, h)` (scales the edges, so touching widgets stay touching), `SS`, `SP`; chrome lines are
`uiLine()` wide (1 up to 150 %, 2 to 250 %, 3 at 300 %; `draw::edge` / `draw::inset` use it);
`classicFont(px)` takes design px and picks `fontPx()`, the largest size whose text is not wider than
the scaled layout (Wine's bitmap strikes are dropped above 100 %, bold is synthetic). Icons are drawn
at `S(16)`. At 1.0 every helper is the identity: 100 % is pixel-identical to the classic layout. Never
use point sizes or fractional geometry; new code passes design pixels through `S()` / `SR()`.

**Colours**: every list/graph/chat colour comes from `mx::ui::Colors` (kit/colors.h), which reads
the `colors/...` settings (defaults = UI_SPEC §2) and emits `changed()` when one changes, so a
Settings ▸ Appearance page only has to call `settings->setValue("colors/...", "#rrggbb")`. Use
`color(keys::colors::X)` / `color(ckeys::X)`; never hard-code a list colour. Chrome colours
(bevels) are fixed in `chrome::`.

## Widget kit (kit/)

| class | use |
|---|---|
| `ThinButton` | 1-px raised "thin button" (page buttons, row-A buttons, toolbars, tabs). `setSelected(true)` / checked = white sunken (current page). Icon at x+2, text after it, or `setCentered(true)`. |
| `PushButton` | classic 2-px push button (`QPushButton`, no auto-default). Checkable ones are drawn latched with the 50 % dither ("Available Files On Top"). |
| `MenuButton` | push button that pops a menu of exclusive choices (radio bullets); text = current choice. `addChoice(label, data, buttonText)`, `addSubmenu`, `choiceChanged(data)`. |
| `VerticalButton` | text rotated 90° (Search "CLEAR"). |
| `LegendItem` | 30×9 swatch with black border + label. |
| `NewsBar` | row-A black strip, bold yellow text, optional magenta link. |
| `TabStrip` | row of thin-button tabs; slot geometry (`setSlotGeometry`) or `setStretch(true)` (Networks sub-tabs); optional leading command button ("Open New Search"). |
| `WrapLabel` | multi-line static text with Win32 word wrap and a fixed 13-px pitch (use instead of `QLabel::setWordWrap` for WinMX-style explanatory texts). |
| `StatusBar`, `FloatTab` | 20-px status bar: borderless text fields + Float/Dock tab + size grip. |
| `MxListView` | **the black list**: QTreeView, 14-px rows, `#808080` full-row selection keeping text colours, no focus rect, classic WinMX tree decoration (`setTreeMode(true)`: [+] box and dotted lines in a 21-px margin of column 0; children are not indented), "..." elision, right-aligned numbers, persisted columns (`setStateKey(settings, "name")` → `ui/columns/name`, `ui/sort/name`), signals `rowActivated` (double-click / Enter) and `contextMenuAt` (right click selects first). `setRowHeight(16)` for icon lists. |
| `draw::` | bevel helpers (thinRaised, buttonRaised, fieldSunken, dither, etched line...). |
| `fmt::` | `number()` (space-grouped `4 388 571`, locale separator except C locale), `duration()`, `rateKBs()`, `bytesOf()`, `partialOf()`, `elide()` with ASCII "...". [ui-e] `size()` `45.7 MB`, `sizeOf()` `12.4 of 45.7 MB`, `sizeOfPercent()`, `rate()` `47.1 KB/s`, `rateLong()`, `etaText()` `3 h 12 min`, `exactBytes()` (tooltips), `dateTime()`; all follow `exactSizes()` (setting ui/exactSizes = the classic texts). |
| `icons` | `icon(IconId::...)`, `iconPixmap()`, `appIcon()`, original pixel art drawn in code on a 16-unit grid at any size (`iconImage(id, px)`; `IconId` list in kit/icons.h; `octomx --export-icons <dir>` writes every icon at 16...48 px). |
| `scale` | [ui-e] `S`, `SR`, `SS`, `SP`, `toDesign`, `uiLine`, `fontPx`, `smoothFonts`, `autoScale`, `applyUiScale` (see Scaling). |
| `launcher` | [ui-e] `openFile(parent, settings, path)` (dangerous-type warning) / `showInFolder(path)` (Explorer `/select`, FileManager1.ShowItems); `setHook()` for tests, `MX_LAUNCH_LOG=<file>` records instead of launching. [dl-1] `previewFile(parent, path, choice, program)` (audio / video only, the file is the player's only argument), `findPlayers()`, `playerArguments()` / `playerNativeArguments()`, `setPlayersHook()`. |
| `links` | [ui-e] `ed2kLink()`, `magnetLink()`, `magnetLinkOf(hit)` for the Copy ... Link entries. |
| `piecebar` | [ui-i] the progress cell as a map of the file (`kit/piece_bar.h`): `columns()` (cells of `TransferInfo::pieceMap` / `pieceFill` to pixel columns), `incremental()`, `geometry()` (block + frame over the cell), `paint()`, `tipLine()`; colours `colors/progressBar` / `progressBackground` / `progressActive`. |

Item roles understood by the list delegate (models/roles.h): `Qt::DisplayRole`, `ForegroundRole`
(QColor), `TextAlignmentRole`, `DecorationRole` (QPixmap), `FontRole`, plus `SegmentsRole`
(multi-colour text like the green/white/red `2 + 1`), `ProgressRole` (0-100: classic WinMX progress cell over the
whole cell; [ui-i] with `PieceMapRole` / `PieceFillRole` a map of the file, `BarTextColorRole` the text colour over the
empty part),
`SortRole`, `SectionHeaderRole` (centred label with rule lines, Hotlist), `ClipRole` (clip instead
of elide), `OverflowRole` (text may run on over the following empty cells), `IdRole`, `KindRole`.

## How to add (or replace) a page

1. Subclass `mx::ui::Page` (pages/page.h): `Page(id, title, core, mainWindow)`.
   Lay out children with absolute geometry in `resizeEvent()` (the original WinMX pages are pixel layouts; the
   page starts at client y = 48, directly below the page buttons; rows start 3-5 px lower).
2. Override `statusFields()` and emit `statusChanged()` when it changes; optionally
   `activated()`, `saveState()`, `selectSubTab()` and `scriptCommand()` (page-specific verbs).
3. In `MainWindow::createPages()` replace the `PlaceholderPage` for your id
   (`shared`, `chat`, `bandwidth`) with your class. Ids and button order are fixed.
4. Give important widgets a stable `objectName` (the script driver and tests find them by name).
5. Add a shot to `tools/shots.sh` and compare it with the reference (see below).

Stubs waiting for the follow-up workers (all in `MainWindow`, virtual, marked `[ui-b]` / `[ui-c]`):
`openIgnored()`, `openBrowse(user)`, `openWhois(user)`, `openPrivateMessage(user)`, `openChat(user)`
(ui-b); `openSettings(page)`, `setupTray()`, `handleCloseRequest()` (ui-c). Pages already call them
(Search / Transfers / Hotlist context menus, Queuing... / Options... buttons, Settings button).

## Script driver (`octomx --script file`, class `ScriptDriver`)

One command per line, `#` comments, 20 ms between commands. Widgets are looked up by
`objectName` in the main window and every visible top-level (dialogs, floating pages, menus).
A failing command stops the script with exit code 2.

| verb | |
|---|---|
| `page <networks\|shared\|chat\|search\|transfers\|bandwidth\|hotlist>` | show a page |
| `subtab <name\|index>` | current page's sub-tab (`wpn`, `other`; search tab index or label part) |
| `type <objectName> <text>` / `settext` | set the text of a line edit / text edit |
| `key <objectName> <key>` | send a key (`Return`, `Down`, `Ctrl+V` ..., QKeySequence syntax) |
| `click <objectName>` | click a button (also the Float tab) |
| `select-row\|dblclick-row\|expand-row\|collapse-row\|context-menu <list> <n>` | n = visible row index (expanded children count) |
| `menu-pick <text>` | trigger an item of the open popup menu (opens submenus) |
| `close-menus`, `close-dialogs` | |
| `hotlist on\|off`, `float [page]`, `dock [page]`, `resize W H` | |
| `set <key> <value>` | write a setting |
| `wait <ms>`, `echo <text>`, `expect-rows <list> <n>` | |
| `shot <file.png>` | grab the main window with open menus composited; open dialogs are saved next to it as `<file>.<dialogObjectName>.png` |
| `quit` | exit (code 0, or 2 if a command failed) |

## Screenshots and comparison

    tools/shots.sh build/ui-a-shots                       # Linux, offscreen, frozen demo data
    OCTOMX_EXE="wine $PWD/dist/ui-a/octomx/octomx.exe" OCTOMX_PATHCONV="winepath -w" \
        DISPLAY=:83 tools/shots.sh build/ui-a-shots-wine   # same shots under wine (Xvnc :83)

The client sizes match the references (ref1 → 1024×689, search_idle_354b4 → 1024×714,
search_results_322 → 841×660, transfers_page_354b3 → 690×445). Reference client offsets
(reference x/y of client (0,0)): ref1 (0,20), search_idle_354b4 (0,25), search_results_322 (4,29),
transfers_page_354b3 (1,21), shared_files_page_columns_353 (2,21). Helper scripts used during the
work live in `tests/ui/imgtools/` (`px.py` row/col colour runs and zooms, `cmp.py` ref-above-mine
crops and diff masks, `dark.py` text bounding boxes), measure, don't eyeball.

## Keyboard

Ctrl+1...7 pages in button order (7 = Hotlist pane), Ctrl+Tab next page, Ctrl+F Search, F1 Help, Ctrl+Shift+V add
links; in lists: arrows/Home/End/PgUp/PgDn, `+`/`-` expand/collapse, Enter = activate
(download / open), Menu key = context menu, Ctrl+V on the download list = paste links, Del = cancel.

## Shared Files, Chat, Bandwidth and the user windows ([ui-b])

| file | what |
|---|---|
| `pages/shared_page.*` + `models/shared_model.*` | Shared Files: Share Folder (Browse for Folder, SHIFT+OK = no sub folders) / Unshare Folder (menu) / Refresh / Shared File Types / Scan For Media; folder tree (`sharedList`) |
| `pages/chat_page.*`, `pages/chat_view.*`, `models/chat_models.*` | Chat: `List Channels` + channel list tab (`channelList`, Filter/Join/Host/Close in sunken wells) + up to 8 room tabs (`chatOutput`, `chatUserList`, `chatInput`, `chatCommands`, `chatExit`); `ChatOutput`/`ChatInput` are reused by the PM window |
| `pages/bandwidth_page.*` | Bandwidth: two stacked-area graphs (`bandwidthIncoming`, `bandwidthOutgoing`), series split in `bw::split()` |
| `dialogs/browse_window.*`, `dialogs/user_windows.*` | Browse, Whois, Private Message (+ `PrivateChatHub`: routes PMs, opens a window for new incoming ones), Ignored Users, Add Ignored User |
| `dialogs/share_dialogs.*`, `dialogs/chat_warning_dialog.*` | Browse for Folder, Shared File Types, "OctoMX Decentralized Chat" warning |
| `main_window_b.cpp` | `MainWindow::openIgnored / openBrowse / openWhois / openPrivateMessage / openChat` (one window per user, deleted on close) + the script verbs below |

Script verbs added through `ScriptDriver::addCommandHandler` (any page): `open-ignored`,
`open-browse <user> [net]`, `open-whois <user> [net]`, `open-pm <user> [net]`, `open-chat <user>`,
`pm-say <user> <text>`, `close-window <objectName>`. Page verbs: Chat `join <room>`, `say <text>`,
`part`, `subtab list|<n>|<room>`; Shared Files `share-folder <path> [norecurse]`,
`unshare-folder <path>`, `rescan`. Screenshots: `tools/shots-ui-b.sh <out-dir> [build-dir]`.

## ui-d additions (service wiring, fonts, persistence)

| file | what |
|---|---|
| `kit/session.*` | `Session` (in-memory "this session only" switches: dangerous-file warning), `migrateLegacySettings()` (retired `ui/search*` keys → `search/defaultType`, `search/autoFindSourcesMin`, `search/autoEnterQueue`, `search/defaultConnection`; runs at every start), file-type / connection text ↔ code helpers |
| `kit/theme.*` | `Fonts` follows `ui/fontNormal` (app font, menus), `ui/fontDynamic` (list rows: `dynamicFont()`), `ui/fontLarge` (`largeFont()`); default values = the built-in 11-px Tahoma faces exactly; changes apply live |
| `kit/list_view.*` | row / header heights follow the fonts; `setTreeGeometry(boxX, margin, connectors, iconGap)` (Shared Files uses the 3.53 layout 5/20/no lines/2); `setFillColumns()` = first-run column widths fill a wide list (Transfers) until the user resizes or a stored layout exists |
| `dialogs/file_download_dialog.*` | "File Download" (Search ▸ Manually Resume..., §5.7) and "Load Incomplete" (Transfers) → `TransferService::resumeDownload` / `partialFiles`; resumed files are remembered in `ui/resumedFiles` so "Delete File" warns that the file is used in place |
| `dialogs/server_messages.*` | Networks ▸ View Server Messages window (`serverMessages` + live `serverMessageReceived`) |
| `main_window_d.cpp` | floating-window placement (`ui/dialogs/float_<page>`), script verbs below |

Script verbs (ui-d): `select-text|dblclick-text|context-menu-text <list> <text>` (first visible row with
`<text>` in any column), `move-window|resize-window <obj> a b`, `expect-window <obj> x y w h`,
`sort|expect-sort <list> <col> asc|desc`, `move-column <list> <logical> <visual>`,
`expect-column-pos <list> <logical> <visual>`, `set-splitter|expect-splitter <name> <size0>`,
`expect-subtab networks <i>`, `expect-choice <menubutton> <data>`. Screenshots: `tools/shots-ui-d.sh <out> [build]`.
Test hooks: `SearchTab::s_askHook / s_resumeDialogHook`, `TransfersPage::s_askHook / s_dirHook /
s_loadIncompleteHook`, `NetworksPage::s_importHook`, `FileDownloadDialog::s_browseHook`.

## ui-e additions (scale, sizes, columns, open file, feedback)

| what | where |
|---|---|
| interface scale, design pixels | `kit/scale.*`, `theme.cpp` (fonts per scale), `classic_style.cpp` (metrics, scaled menus / check / radio / tree branches) |
| human-readable sizes, "Show exact byte counts" | `kit/format.*`; MainWindow applies ui/exactSizes live (headers + views) |
| Search columns Network / Peers | `models/search_model.*` (`search::networkText`, `search::totalPeers`; unknown sorts last in `SearchProxy`) |
| column chooser, Reset Columns, proportional columns, filter box, elided tool tips | `kit/list_view.*` (`setDefaultHiddenColumns`, `setColumnVisible`, `resetColumns`, `columnMenu`, `setFilterEnabled` / `showFilter`; layouts persist as `fractions` of the width) |
| Open File / Open Containing Folder | `kit/launcher.*`; Transfers (`openTransfer`, `showTransferInFolder`), Shared Files, Search rows that match a local file |
| Settings ▸ Appearance ▸ Interface, wizard interface size | `dialogs/settings_appearance.cpp`, `dialogs/setup_wizard.cpp`; `app/main.cpp` builds the main window after the wizard at the chosen scale |
| download-complete notification, connection-derived slots | `MainWindow::notifyDownloadDone`, `applyConnectionDefaults()` (`kit/session.*`) |

Script verbs (ui-e, `main_window_e.cpp`): `header-menu <list>`, `column-visible <list> <col> on|off`,
`reset-columns <list>`, `filter <list> [text]`, `expect-filter <list> <n>`, `search-history`,
`expect-column-header <list> <col> <text>`, `expect-cell <list> <row> <col> <text>`, `notice`.
Test hooks: `launcher::setHook / setAskHook`, `TransfersPage::s_messageHook`, `MainWindow::s_notifyHook`.
Screenshots: `tools/shots-ui-e.sh <out> [build]` (every page at 800×600 ... 3840×2160@200 %, "Automatic"
from the screen DPI). Icon review: `tests/ui/imgtools/iconsheet.py`, similarity to the original
screenshots: `tests/ui/imgtools/iconsim.py <export-dir>`.

## ui-f additions (honest file types in Search)

| what | where |
|---|---|
| the one extension table, filter codes, the filter rule | `src/api/types.cpp` (`mx::extensionsForMediaType`, `mediaTypeForFileName`, `losslessAudioExtensions`, `FileTypeFilter`, `fileTypeCodeFromLegacy`, `classifyHit`, `hitMatchesFileType`); used by the core (`SearchManager::acceptHit`), every network and the mock |
| file-type menu, labels, button texts, tool / status tips | `kit/file_types.*` (`filetypes::populateMenu`, `menuLabel`, `buttonText`, `fittedButtonText`, `description`) |
| codes in the settings, one-time migration of the earlier texts | `kit/session.*` (`migrateFileTypeSettings`, part of `migrateLegacySettings`) |
| quick buttons `All | Audio | Video | Pictures` (latched when current), Custom Extensions... | `pages/search_page.cpp` (`searchAll`, `setFileTypeCode`, `fileTypeCode`, `s_customExtensionsHook`), `dialogs/simple_dialogs.*` (`CustomExtensionsDialog`) |
| merged-list type filter (late bundle file lists, child rows with other names) | `models/search_model.*` (`SearchProxy::setTypeFilter`) |
| Settings ▸ Search menus, Shared File Types groups | `dialogs/settings_pages2.cpp` (`fileTypeButton`), `dialogs/share_dialogs.cpp` |

Codes: `any`, `audio`, `audio:min|eq|max:<kbps>`, `video`, `pictures`, `documents`, `software`, `archives`,
`discimages`, `torrents`, `mp3`, `mp3:<op>:<kbps>`, `lossless`, `servers`, `clr`, `custom:<ext>,<ext>` (search/defaultType,
search/typeButton*, search/customExtensions holds the last custom list). Tests: `tests/ui/tst_uif.cpp`.
Demo data for every type: search `sintel` in the demo core (open-movie files, torrents without extension,
bundles with and without file lists). `mxcli search ... --type <code>` prints hits per network and any hit
that contradicts the type.

## post-1 additions (close / minimise, colour theme, Check for Updates)

| what | where |
|---|---|
| minimise = taskbar always; X = misc/closeAction (Prompt / Keep running in tray / Exit), no tray = exit; close prompt `Exit OctoMX` (default) / `Keep running in the tray` / `Do not ask again` | `main_window_c.cpp` (`TrayController::handleClose`, `eventFilter`), `dialogs/close_prompt.*`, `app/main.cpp` ("Always start minimized" = taskbar) |
| colour theme Dark (classic) / Light / Follow system (ui/colorTheme): the light table, conversion that keeps customised colours (colors/themeApplied), QStyleHints::colorScheme() live | `kit/colors.*` (`Colors::applyTheme`, `themeDefault`, `wantedTheme`, `setSystemSchemeOverride`, `contrast`), `mx::colorSchemePreset(true)` in `src/api/settings_schema.cpp` |
| theme switch in Settings ▸ Appearance, `Color Theme ▸` in the tray menu and the Settings button's right-click menu | `dialogs/settings_appearance.cpp`, `createColorThemeMenu()` (`tray.h`), `MainWindow::settingsButtonMenu()` |
| Help button menu (About OctoMX... / Check for Updates...), About box button, update box, automatic check at most once a day | `update_ui.*` (`UpdateController`), `MainWindow::helpMenu()` / `checkForUpdates()`, `dialogs/about_dialog.*`; network part `mx::UpdateChecker` (`src/api/mx/update_check.h`) |

Never hard-code a list colour: anything drawn on a list, chat view or graph takes `color(...)` and
repaints on `Colors::changed` (the Light theme turns the backgrounds white). Elide with
`fmt::elide()` / `fmt::elideMiddle()` (three ASCII dots), not `QFontMetrics::elidedText()`.

Script verbs (post-1, `main_window_c.cpp`): `expect-minimized on|off`, `expect-hidden-to-tray on|off`,
`help-menu`, `settings-menu`, `check-updates`, `update-show <release.json>` (shows what a check with
that answer shows, no network; for screenshots), `expect-theme dark|light`.
Test hooks: `UpdateController::s_offerHook / s_messageHook / s_openUrlHook`, `UpdateController::setPolicy()`,
`Colors::setSystemSchemeOverride()`. Screenshots: `tools/shots-post-1.sh <out> [build]`.
Tests: `tests/ui/tst_theme.cpp`, `tst_update.cpp` (local fake server), `tst_nodashes.cpp`,
`tst_tray_close.cpp`, `tst_restart.cpp`; `tools/check-no-dashes.sh` (ctest `tools_check_no_dashes`).

## bw-1 additions (speed limits one click away)

| what | where |
|---|---|
| limits in force (normal / schedule / "your limits win until the window ends"), upload minimum 10 KB/s, LAN address test; shared by core and UI | `src/api/mx/bandwidth_limits.h` (`bwlimit::inForce`, `noteManualChange`, `installOverrideRule`, `isLanAddress`) |
| helpers: set a limit, `Speed Limits On` toggle, presets from the connection type, `2.5 MB` parsing, status texts | `speed_limits.*` (`speed::`) |
| Bandwidth page row `Limit download [x] [value] KB/s  Limit upload ...` + note + `Settings...` | `speed_limits.*` (`SpeedLimitRow`, `RateSpinBox`), `pages/bandwidth_page.cpp` |
| limit lines in the graphs, `(limit ...)` in the status bar | `pages/bandwidth_page.cpp` (`BandwidthGraph::setLimit`) |
| tray `Speed Limits >` | `createSpeedLimitsMenu()` (`speed_limits.*`), one line in `main_window_c.cpp` |
| ` - DL limit 500 KB/s, UL limit 50 KB/s` in the title | `MainWindow::titleText()` |
| Settings > Bandwidth Throttle spin boxes take `2.5 MB`; Bandwidth Schedule says what is in force | `dialogs/settings_pages2.cpp` (`limitRow`, schedule page) |

Object names: `speedLimitRow`, `limitDownCheck`, `limitDownValue`, `limitUpCheck`, `limitUpValue`, `limitNote`,
`limitSettings`; menus `speedLimitsMenu`, `speedLimitsDownMenu`, `speedLimitsUpMenu`, actions `speedLimitsOn`,
`speedLimitsDownOther`, `speedLimitsUpOther`, `speedLimitsSchedule`, `speedLimitsResume`, `speedLimitsSettings`;
Settings `scheduleNow`. Test hook: `speed::s_askHook` (the `Other...` question). Tests: `tests/ui/tst_bw1.cpp`
(`MX_BW1_SHOTS=<dir>` also saves the menus). Screenshots: `tools/shots-bw-1.sh <out> [build]`.

## dl-1 additions (download order flags, Preview)

| what | where |
|---|---|
| `Download In Sequential Order` / `Download First And Last Pieces First` (download menu, multi-selection), marks `[Seq]` `[F+L]` after the name, order lines in the File / Status tool tips | `pages/transfers_page.cpp` (`addOrderEntries`), `models/transfer_model.cpp` (`orderMarks`, `orderTip`); API `TransferService::setSequentialDownload / setFirstLastPiecesFirst`, `TransferInfo::sequential / firstLastFirst / orderFollowedBy` |
| `Preview` / `Preview...` / `Preview: needs ...` (incomplete media file), `Open With Preview Player` (complete one) | `pages/transfers_page.cpp` (`addPreviewEntry`, `previewTransfer`), `dialogs/preview_dialog.*` (`PreviewWithDialog`, `PreviewFileDialog`, `previewui::menuEntry / choosePlayer / run`); API `TransferService::previewInfo / preparePreview`; rules in `mx/download_order.h` |
| defaults for new downloads, Add Link check boxes | Settings ▸ File Transfers ▸ Downloads (`transfers/sequentialDefault`, `transfers/firstLastDefault`), `dialogs/simple_dialogs.*` (`AddLinkDialog::setOrder`) |
| player choice | Settings ▸ File Launching ▸ Preview Player (`launch/previewPlayer` = "" ask / `system` / `program`, `launch/previewProgram`) |

Test hooks: `PreviewWithDialog::s_execHook / s_browseHook`, `PreviewFileDialog::s_execHook`, `TransfersPage::s_addLinkHook`,
`launcher::setPlayersHook`. Tests: `tests/ui/tst_dl1.cpp`. Screenshots: `tools/shots-dl-1.sh <out> [build]`.

## ui-h additions (no control that does nothing)

| what | where |
|---|---|
| alert sounds (message, Hotlist online, download complete): beep / built-in chime / .wav; PlaySound on Windows, the desktop's player on Linux; never played in tests (hook, `MX_SOUND_LOG`, offscreen, `MX_DEMO_FROZEN`, `MX_SOUND_OFF`) | `kit/sound.*` (`sound::choiceFor`, `alert`, `play`, `setHook`, `builtinWav`) |
| one tool tip per setting key (what it does + the default), given by PageKit to every bound control | `dialogs/settings_tips.cpp` (`settingTip`, `hasSettingTip`) |
| File Launching entries in the Transfers menu, Launch Options programs | `kit/launcher.*` (`userCommand`, `menuEntries`, `runEntry`, `entryCommand`) |
| column minimum widths from content, per-list background key, grid lines, Selected Text / Header Text colours | `kit/list_view.*` (`setMinimumColumnTexts`, `minimumListWidth`, `setBackgroundKey`), `Colors::showGrid()` |
| own information sign for message boxes | `kit/icons.*` (`informationPixmap`) |

The settings registry `tests/ui/settings_registry.h` names, for every visible schema key, the test that proves
it, its reader, or why it is internal; `tst_settings_registry` fails for a visible key without an entry and for a
hidden key bound on a page. A setting nothing reads is hidden in the schema (`B.hide()`), never left on a page.
Test hooks: `sound::setHook`, `MainWindow::s_flashHook`. Tests: `tests/ui/tst_uih.cpp`, `tst_settings_registry.cpp`
(`MX_UIH_SHOTS=<dir>` saves the changed Settings pages and the chat user list at 100 ... 250 %).

## ui-i additions (the download bar is a map of the file)

| what | where |
|---|---|
| cells to pixel columns, the partly-there tone, the bar over the whole cell, inverted text, tool tip line | `kit/piece_bar.*` (`piecebar::`), drawn by `MxItemDelegate` for every `ProgressRole` cell |
| which download rows show a bar (transferring; paused / queued / waiting / checked with data), map roles, tool tip | `models/transfer_model.cpp` (`transfers::showsBar`) |
| the core's map: wanted bytes end to end, 512 cells, state + verified share; "in progress" = requested now | `TransferInfo::pieceMap` / `pieceFill` (`mx/types.h`), `src/core/engine/store.h` (`condensePieceMap`), `PartStore::pieceMap`, `TorrentStore::pieceMap` / `mapOf`, `Download::pieceMapOf`; libtorrent's download queue in `TorrentSnapshot::downloading` (`bt_worker.cpp`, `refreshQueue`) |
| demo maps (sequential, scattered eD2k parts, first and last, paused, nearly complete) | `src/mock/mock_data.cpp` (`PieceSim`, `demoPieces`), ticked by the demo core |

Tests: `tests/ui/tst_piece_bar.cpp`, `tests/engine/tst_engine_map.cpp`. Screenshots: `tools/shots-ui-i.sh <out> [build]`.
Live check against the disk: `mxcli maplog <id|last> <file.jsonl>` then `tests/engine/map_check.py <file.jsonl>
[--torrent F.torrent | --ref-url URL]` (recomputes the map from the ranges the engine reported and reads the complete
parts back from disk: SHA-1 against the .torrent's piece hashes, or bytes against the original URL).

