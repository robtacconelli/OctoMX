# reference/: screenshots of the original WinMX (not included)

During development this directory held about 130 screenshots of the original WinMX 2.6-3.54
client (FrontCode Technologies, 2001-2005). They were the visual reference for `docs/UI_SPEC.md`
and for pixel measurements quoted in some source comments (for example "measured on
reference/web/settings_*.png").

The screenshots are third-party copyrighted images (Wikipedia/Wikimedia Commons, reddit,
winmxworld.com tutorials, the Wayback Machine copy of winmx.com, Betanews), so they are **not
part of the published source tree**. Nothing in the build or the automated tests needs them.

To re-collect them for UI comparison work, download each image listed below from its source and
save it under the file name in the first column (`reference/<file>`). The index is the one used
during development, unchanged apart from this introduction.

---

Abbreviations for sources:
- **WMW-T** = https://www.winmxworld.com/tutorials/<page>.html (images under /images/)
- **WMW-H** = https://www.winmxworld.com/help/<page>.html
- **WB** = Wayback Machine (web.archive.org)

## Original four (supplied by the coordinator)

| file | shows | version | source |
|---|---|---|---|
| ref1_wikipedia_1024x709.jpg | Main window, Networks page, "WinMX Peer Network" sub-tab, title `WinMX v3.53  <Online P=5 S=12 spm=4284>`, status bar, Float tab. Wine rendering (flat buttons, centred title). Same file as Wikipedia `File:Winmxscreenshot.JPG` | 3.53 | en.wikipedia.org (Commons) |
| ref2_reddit_1295x1083.jpg | Search page with results (upscaled ×1.52 of web/search_results_322_native.gif; use that one for colours) | 3.22 | reddit |
| ref3_thumb_607x505.jpg | Old 2.6 search layout (secondary, layout differs from 3.x) | 2.6 | web thumb |
| ref4_533x376.jpg | Settings dialog, Incoming TCP Connections page; tree uses older "Colors" root (3.31-era tree, Wine fixed font) | 3.31-ish | web |

## web/: collected

### Main window / pages
| file | shows | version | source |
|---|---|---|---|
| web/networks_wpn_tab_354b3.png | Networks page, WPN sub-tab, green "Ready" indicator, Refresh/Disconnect, Network Identity, Connection Type (annotated with red numbers 1-3). News bar black with yellow text + magenta link | 3.54b3, XP | WMW-T winmx_basics |
| web/networks_connection_status_finding_yellow.png | Connection Status box in the yellow "Finding network connections" state | 3.5x | WMW-T winmx_obstacle_course (obstacle_1a.png) |
| web/networks_opennap_tab_buttons.png | Sub-tab "OpenNap Protocol Networks" + "Disconnect" button | 3.54b4 | WMW-T opennap |
| web/networks_opennap_wsx_buttons.png | Buttons "Find .WSX", "Import .WSX", "Export .WSX" | 3.54b4 | WMW-T opennap |
| web/networks_opennap_server_list_red.png | OpenNap server list, rows red (not connected) + context menu | 3.54b4 | WMW-T opennap |
| web/networks_opennap_server_menu.png | Server context menu (Connect / Disconnect / Add / Edit / Remove / View Server Messages / Select All) | 3.54b4 | WMW-T opennap |
| web/networks_opennap_server_status_colors.png | Server rows: green "Ready (N users, N files, N gigs)", yellow "Connecting...", "Waiting... (-2:30)" | 3.54b4 | WMW-T opennap |
| web/titlebar_opennap_status.png | Title suffix `<OpenNap: 3 nets online, 3 pending 17426 users 11499847 files 514567 gigs>` | 3.54b4 | WMW-T opennap |
| web/shared_files_button_353.png | 3.53 page row with Shared Files + Transfers buttons row (disabled lower buttons) | 3.53, classic 16-bit | WMW-T sharing_on_353 |
| web/shared_files_page_353.png | Shared Files page: Share Folder / Unshare Folder / Refresh / Shared File Types, "File Path" header | 3.53 | WMW-T sharing_on_353 |
| web/shared_files_page_columns_353.png | Same, full width: + Scan For Media; columns File Path / Size / Bitrate / Time / Freq. Best native classic pixel reference for chrome geometry | 3.53, classic | WMW-T sharing_on_353 |
| web/main_page_buttons_row_native.png | Native-pixel page-button row (Networks...Bandwidth) | 3.54, XP | WB winmxworld.com/images/scd001.bmp |
| web/library_page_354b3.png | Library page (3.54 replacement of Shared Files): Add Folder / Remove Folder / Refresh All / Settings / Scan For Media | 3.54b3 | WMW-T winmx_basics |
| web/library_buttons_columns_354.png | Library header: Name / Shared / Bytes / Duration / Bitrate / Compression / Resolution / Freq | 3.54b4 | WMW-T sharing_on_354 |
| web/library_354alpha_tree.png | Full main window 936×726, Library folder tree, Float tab, status bar | 3.54 alpha 041007 | WB winmx.com/beta354preview/winmx_ss_2004-10-08-a.zip |
| web/library_354alpha_files.png | Library with files: red incomplete rows "N% of size", bitrate colours, share check boxes | 3.54 alpha | same zip |
| web/library_354alpha_context_menu.png | Library folder context menu with Filetypes submenu | 3.54 alpha | same zip |
| web/library_share_checks_354.png | Green check / red x share-state icons | 3.54b4 | WMW-T sharing_on_354 |
| web/chat_channel_list_354b3.png | Chat page, channel list: List Channels / WinMX Peer Network tabs, Filter, Join, Close, Join Selected Channel, Refresh List, Favorites, Channel/Users/Topic, "1959 channels". Purple dots are tutorial annotations | 3.54b3 | WMW-T winmx_basics |
| web/chat_channel_list_filter_host_bar.png | Filter bar with Host button (primary connection) | 3.5x | WMW-T winmx_hosting |
| web/chat_list_channels_network_dropdown_354b4.png | "List Channels" dropdown listing WinMX Peer Network + OpenNap networks; title with OpenNap status; "Patch Operational" news bar | 3.54b4 | WMW-H opennap-chat_rooms |
| web/chat_room_window.png | **Open chat room**: header (bold channel name "... on WinMX Peer Network" + topic), black output with coloured lines (MOTD blue, join green, bot lines), user list User/Files/Speed, input bar, Commands / Exit Channel | 3.5x (patched), XP | WB winmxworld.com/images/chat_channel.png |
| web/chat_room_header_name_topic.png | Room header (name + topic line) | 3.5x | WMW-T winmx_hosting |
| web/chat_room_channelname_output.png | `/channelname` echo in red | 3.5x | WMW-T winmx_hosting |
| web/search_idle_354b4_wikipedia.png | Search page idle, full 1024 width, "Waiting for results..." status | 3.54b4 | Wikimedia Commons File:WinMX_v3.54_beta4.PNG |
| web/search_idle_annotated_354b3.png | Search page idle with red annotation marks | 3.54b3 | WMW-T winmx_basics |
| web/search_results_322_native.gif | Search with results, native 849×693 (best colour reference: grouped "N users" rows, bitrate, connection, status colours) | 3.22 | Betanews screenshot 984813857-1.gif via WB |
| web/search_results_nonmp3_354.png | Results for "health" with Any Non-MP3 File, expand boxes, gray selected row | 3.54b4 | WMW-H winmx-searching-file_types |
| web/search_results_tilde_filter_354.png | Results for "health ~pdf" | 3.54b4 | same |
| web/search_status_server_column.png | Speed / Ping / Status-Server column contents incl. group rows | 3.54b4 | WMW-H winmx-searching-column_header |
| web/search_ping_column.png | Ping values and "?" | 3.5x | WMW-T searcing_file__types |
| web/search_tabs_hash_expanded_group.png | Search tabs ("Open New Search", per-search tabs), HASH> line, expanded group with cyan "can be added" rows and dark-blue "currently downloading" row | 3.5x | WMW-T hash_numbers |
| web/search_group_expanded_selected.png | Expanded group, gray selection band | 3.5x | WMW-T hash_numbers |
| web/search_header_resize_cursor.png | Squeezed header ("Bitr...", "Fr...", "Sp...") | 3.54b4 | WMW-H winmx-searching-column_header |
| web/transfers_page_354b3.png | **Transfers page**: download buttons (2 rows), download list, "0 downloads from 0 users at 0 bytes per second total", upload buttons, upload list with blue progress bars | 3.54b3 | WMW-T winmx_basics |
| web/transfers_downloads_header_354b4.png | Download header File / User - AFS - AEQ / Speed / Bytes / Status / K/s / Time Left, yellow "Finding sources..." | 3.54b4 | WMW-H winmx-transfers-download_section |
| web/transfers_uploads_header_354b4.png | Upload buttons + header File / User / Speed / Bytes / Status / K/s / Time Left | 3.54b4 | WMW-H winmx-transfers-uploads |
| web/transfers_buttons_354b4_patched_titlebar.png | Transfers buttons + title `WinMX v3.54 beta 4 - 4 UL @ 22,879 B/s <Online P=8 S=5 spm=12369>` | 3.54b4 | WMW-H winmx-transfers-incompletes |
| web/transfers_download_row_progress.png | One download row: "[Jaggers] PUBLIC ...wsx  1 user - 15m - 10  12,240 of 31,840  [38%]  0.00" | 3.54b4 | WMW-T opennap |
| web/browse_window_and_transfers.png | Transfers (older layout) + a Browse window with white list | 3.3x | WB winmxworld.com/images/browse_trans.png |
| web/browse_window_354b4.png | **Browse window**: Refresh / Send Message / Whois / Add To Hotlist / Show Full Paths / Download / Manually Resume... / AFS / AEQ, legend, file list, status "Finished downloading list> 2320 files (665,937 megs) total" | 3.54b4 | WMW-T fake_file_info (fis5.png) |
| web/bandwidth_graph_page.png | **Bandwidth page**: stacked-area graph incoming (top) / outgoing (bottom), status "Incoming (top) 8,953 bytes per second   Outgoing (bottom) 7,651 bytes per second" | 3.5x | WMW-T bandwidth_graph |
| web/bandwidth_graph_legend_annotated.png | Graph with legend added by the tutorial author | 3.5x | same |
| web/hotlist_temporary_and_button.png | Hotlist page "Hotlist Options" + Temporary entries | 3.54b3 | WMW-T winmx_basics |
| web/hotlist_temporary_offline.png | Hotlist sections Temporary / Offline | 3.54b4 | WMW-H winmx-hotlist |
| web/hotlist_temporary_online.png | Hotlist sections Temporary / Online | 3.54b4 | WMW-H opennap-hotlist_entries |
| web/ignored_users_window.png | **Ignored Users** tool window: Add / Whois / Remove, entries, "6 users in list" | 3.54b4 | WMW-H winmx-ignore |
| web/ignored_users_window_empty_354b3.png | Same, empty, "145 users in list" | 3.54b3 | WMW-T winmx_basics |
| web/icon_button_bandwidth.png, web/icon_button_settings.png | Button closeups | 3.5x | WMW-T bandwidth_graph |

### Context menus
| file | shows | source |
|---|---|---|
| web/menu_search_result.png | Search result: Download / Manually Download / Browse / Add To Hotlist / Whois / Send Message / Chat | WMW-T winmx_basics |
| web/menu_transfers_user_row.png | Transfers user row: Cancel / Clear / Bandwidth Priority ▸ / Browse / Send Message / Whois / Add To Hotlist / Open / Play / Enqueue | WMW-T winmx_basics |
| web/menu_transfers_download_file.png | Download file row (3.54b4): Cancel / Clear / Delete File / Trim 10K from end of file / Find Sources Now / AFS ▸ / AEQ ▸ / Search For Alternates... / Search For Similar... / Open / Play | WMW-H winmx-transfers-download_section |
| web/menu_transfers_download_file_afs_submenu.png | Same with "Auto Find Sources - N" submenu Never / Every 10...60 min | WMW-H winmx-transfers-incompletes |
| web/menu_transfers_download_file_delete.png | Same, "Delete File" highlighted, "Open / Start" | same |
| web/menu_transfers_import_wsx.png | Completed .wsx download row: Clear / Delete File / Search For Alternates... / Search For Similar... / Import | WMW-T opennap |
| web/menu_transfers_upload.png | Upload row: Start Transfer / Cancel / Clear / Bandwidth Priority ▸ / Browse / Send Message / Whois / Add To Hotlist / Open / Play | WMW-H winmx-transfers-uploads |
| web/menu_transfers_upload_bandwidth_priority.png | Bandwidth Priority submenu x10 ... 10% | same |
| web/menu_auto_find_sources.png / web/menu_auto_enter_queue.png | AFS menu (Never, Every 10...60 min) / AEQ menu (Never, 1...100) | WMW-H winmx-searching-afs_aeq |
| web/menu_search_filetype.png, _mp3_sub.png, menu_search_bitrate_sub.png | File type menu, MP3 sub-menu, bitrate list | WMW-T searcing_file__types |
| web/menu_search_networks.png | Network selector (All Available Networks / WinMX Peer Network / [Net] host:port ...) | same |
| web/menu_search_connection_filter.png | Connection filter (Any Connection / T3 or faster ... 14.4K or faster) | WMW-H winmx-searching-connection_types |
| web/menu_hotlist_user.png | Hotlist user: Send Message / Chat / Perm add to list / Browse / Whois / Ignore / Rename on list / Remove | WMW-T winmx_basics |
| web/menu_hotlist_options.png, web/menu_hotlist_options_autoresponder.png | Hotlist Options menu | WMW-H winmx-hotlist, WMW-T autoresponder |
| web/menu_channel_list_row.png | Channel list row: Join Channel / Add To Favorites / Copy Channel Name | WMW-T winmx_chat |
| web/menu_chat_favorites.png | Favorites: Add Selected Channel / list / Remove ▸ | WMW-H winmx-chat-entering_rooms |
| web/menu_chat_room_commands.png | Chat room "Commands" menu | WMW-T winmx_chat |
| web/menu_chat_room_commands_admin_config.png | Commands ▸ Admin Channel Config Commands submenu | WMW-T winmx_hosting |
| web/menu_shared_files_folder_353.png | Shared Files folder row: Expand / Open Folder / Un-share Folder | WMW-T sharing_on_353 |
| web/menu_system_and_tray.png | System menu (left) and tray-icon menu (right) | WMW-H winmx-browse_whois |

### Dialogs
| file | shows | source |
|---|---|---|
| web/dialog_chat_warning.png | "WinMX Decentralized Chat" warning, Continue / Cancel | WMW-T winmx_chat |
| web/dialog_close_prompt_minimize_or_exit.png | Close prompt (minimise to tray / exit / Don't show this again) | WMW-H winmx-settings-misc |
| web/dialog_confirm_file_delete.png | "Confirm File Delete" (send '__INCOMPLETE___...' to the Recycle Bin?) | WMW-H winmx-transfers-incompletes |
| web/dialog_dangerous_file_warning.png | Executable download warning | WMW-H winmx-searching-dangerous_files |
| web/dialog_file_download_manual_resume.png | "File Download" manual-resume dialog | WMW-T searcing_file__types |
| web/dialog_auto_responders.png, web/dialog_new_auto_responder.png | Auto Responders dialog / New Auto Responder | WMW-T autoresponder |
| web/dialog_shared_file_types_353.png | "Shared File Types" (3.53) | WMW-T sharing_on_353 |
| web/dialog_share_folder_browse_353.png | Share Folder browse ("Hold down SHIFT when clicking OK to EXCLUDE sub folders.") | WMW-T sharing_on_353 |
| web/dialog_add_library_folder_354.png, web/dialog_add_library_folder_filetypes_354.png | 3.54 Add Library Folder dialogs | WMW-T sharing_on_354 |

### Settings dialog (all 3.53/3.54 classic unless noted)
| file | page | source |
|---|---|---|
| web/settings_appearance_page.png | Appearance (Setup Wizard button highlighted) | WMW-H winmx-settings-setup_wizard |
| web/settings_appearance_subtree.png | Appearance sub-tree (OpenNap Network Window ... Bandwidth Window) | WMW-T bandwidth_graph |
| web/settings_appearance_channel_window_colors.png | Appearance ▸ Channel Window colour buttons (default dark scheme) | WMW-H winmx-settings-appearance |
| web/settings_appearance_channel_window_colors_custom.png | Same after user change (blue bg) | same |
| web/settings_appearance_bandwidth_window_colors.png | Appearance ▸ Bandwidth Window colours (crop) | WMW-T bandwidth_graph |
| web/settings_internet_connection.png | Internet Connection (connection type radios + 4 buttons), XP | WMW-H winmx-internet_connection_types |
| web/settings_outgoing_tcp.png | Outgoing TCP Connections (proxy) | WMW-H ...outgoing_tcp |
| web/settings_incoming_tcp.png | Incoming TCP Connections | WMW-T changing_port_numbers |
| web/settings_inout_udp.png | In/Out UDP Packets | same |
| web/settings_host_name_resolution.png | Host Name Resolution | WMW-H ...hostnames |
| web/settings_winmx_peer_network.png | WinMX Peer Network | WMW-H ...winmx_peer_network |
| web/settings_opennap_networks.png | OpenNap Networks | WMW-H ...opennap_networks |
| web/settings_incoming_msgs_privacy.png | Incoming Msgs & Privacy | WMW-H ...incoming_messages |
| web/settings_library.png | Library (Available Filetypes) | WMW-H ...library |
| web/settings_channels.png | Channels | WMW-H ...channels |
| web/settings_search.png | Search | WMW-H ...search |
| web/settings_file_transfers.png, web/settings_file_transfers_353.png | File Transfers | WMW-H ...file_transfers, WMW-T sharing_on_353 |
| web/settings_queuing.png | File Transfers ▸ Queuing | WMW-H ...file_transfers |
| web/settings_bandwidth_throttle.png | Bandwidth Throttle (red X is the tutorial's annotation) | WMW-H ...bandwidth_throttle |
| web/settings_bandwidth_throttle_graph_avg.png | Throttle crop showing "BW Graph Rolling Avg. Duration" | WMW-T bandwidth_graph |
| web/settings_file_launching.png | File Launching (tree shows "Advanced Settings" = community patch addition) | WMW-H ...file_launching |
| web/settings_miscellaneous_patched.png | Miscellaneous (tree shows patch-added "Advanced Settings") | WMW-H ...misc |

## Not found (reconstructed in the spec)
Private-message window, Whois result window, the floating ("Float") page window, Setup Wizard pages,
splash/about, "Change name" dialog, OpenNap tab full-page layout, tray icon pixel art, sounds.
Searched: winmxworld.com (all help/tutorial pages), Wayback copies of winmx.com (only v1.8-2.6 help
exists; 3.x help pages were "under construction"), Wikipedia/Commons (2 images), Betanews, Neowin,
WinFuture, general web search. Rejected as junk: network diagrams, Windows Explorer/firewall/router
tutorials, third-party tools (MXMonitor, Leechammer, RoboMX, OurMX, Spanish-language duplicates).
