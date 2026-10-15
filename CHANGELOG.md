# Changelog

All notable changes to diskOS are documented here.

Entries follow the Keep a Changelog format, with Added, Changed, and Fixed categories where applicable.

## [1.3-custom] - 2026-10-15

Integrated custom release rebased on upstream v1.2.1 (`4cd4a60`). Combines all upstream 1.2.1 enhancements (Ogg Opus decoding, Windows installation toolchain, setup checklists, halo theme enhancements) with hardware sleep stabilization, battery fuel gauge tracking, external USB DAC output, and UI performance optimizations.

### Added

- **Hardware Sleep & Wake Synchronization**:
  - Linux evdev physical power key injection (`KEY_POWER` / code 265) synchronizing `mq_player`'s internal screen-on state with diskOS, ensuring single-press power button wake.
  - Stock CST816T deep sleep sequencing (0mA current draw), 120ms ST77916 timing delay, and panel suspend/resume routines.
  - Settings > Display > "Touch to Wake" toggle (`touch_wake`, default: Off).
  - 200ms standby polling interval to reduce CPU load and battery consumption while asleep.
- **Battery & Power Management**:
  - Real-time battery ADC fuel gauge tracking (`voltage_now` and `capacity`).
  - 24-hour circular Battery & Usage History dial (`SCR_USAGE`) accessible via Apps and Quick Settings.
  - Automated safe shutdown on critical low battery.
  - Instant clock population in Quick Settings drawer (eliminating `--:--` placeholder delay).
- **External USB DAC Host Mode (Mode 4)**:
  - Digital audio routing to external USB DACs via USB-C OTG (V2.40/V2.57).
  - Netlink uevent hotplug tracking (`a60a` 0x0212/0x0213) and procfs monitoring.
  - Transport guard blocking blind playback and preventing queue looping when unplugged.
- **Mode Lockdown Screens (`SCR_MODELOCK`)**:
  - Full-screen status views for USB DAC, Bluetooth Receiving (with AVRCP transport controls and volume sync), and USB Storage with safe edge-swipe navigation.
- **UI Performance & Typography**:
  - 14-slot row virtualization in library lists eliminating scrolling stutter on large music cards.
  - Folder-scoped playback and shuffle play in Browse Files.
  - Fast-scroll alphabetical jump popup.
  - Supplementary Japanese Kanji and Kana CJK font tables (3,724 glyphs).

### Fixed

- Fixed screen-off false wake loop and touch freeze on timed auto-sleep.
- Resolved float precision compiler warnings in albumwall and pruned unused header includes.
- Hardened BlueALSA and bluetoothd subprocess management to eliminate IPC lockups.

## [1.2.1] - 2026-10-14

Adds Ogg Opus support and Windows installation, updates the installer experience and
startup animation, and fixes text, playback, Wi-Fi, screen wake and app-update handling.

1.2.0 users can install the signed app update from Settings > System > Update diskOS
if updates are allowed. Fresh installs and older versions use the installer.
App updates do not replace the boot install script. Fixes marked "Requires a full
flash" need the complete 1.2.1 installer.

### Added

- [Public website](https://b0hemia.github.io/diskos/) with screenshots, a feature tour
  and installation links.
- Ogg Opus support for .opus, .oga and .ogg files: the scanner reads their tags and
  available lengths, and playback uses the Disc's stock player. When the player's
  title is empty or matches the filename, diskOS reads the file's TITLE tag and
  uses it when the read succeeds. If tags are absent or unreadable, the player's
  title is retained. Lengths the scanner cannot read remain unknown.
  The resolved title also reaches the current Up Next row, Song Info, lyrics and
  online artwork searches, and Last.fm.
- Playback progress around Home's now-playing border in the halo theme.
- Settings > System > Old tags: Auto / Western / Cyrillic for legacy music tags and
  CUE-sheet text. Auto is the default and guesses CP1251 Cyrillic; Western keeps
  Latin-1 decoding, and Cyrillic selects CP1251. Use Rescan Library after changing
  it. Valid UTF-8 and UTF-16 tags are unchanged.
- Windows 10/11 x64 installer support. Extract the complete
  `diskos-installer-windows-x86_64.zip` and open `diskos-installer.exe`.
  Keep `_internal`, `RECOVERY.txt` and the other files together. The program is
  unsigned; Windows SmartScreen may warn on first run.
- A Ready checklist before image preparation, with flash-mode detection, USB driver
  or permission hints, the firmware choice and installer tools. It shows items
  needing attention and a Refresh action. When no Disc is found, it explains how
  to power off, hold Volume Down and connect USB; the screen stays black.
  Checklist warnings do not block the install. Firmware and actual USB access
  are checked before flashing.
- A confirmation screen listing the firmware base, duration and update choice.
  Begin flash stays disabled until the acknowledgement is ticked.

### Changed

- The Default theme is now called halo.
- The installer on Windows, macOS and Linux uses the website's diskOS wordmark,
  Archivo and Michroma display lettering, a disc illustration, quieter sections
  and a rose primary button. Dynamic text uses the system font; no fonts are
  installed on the computer. Dev is under Advanced.
- The installer has dedicated preparation, flashing and result views. Flashing
  shows elapsed time, progress and a reminder to keep the Disc connected and
  computer awake. Details opens the log. Success explains the next step; errors
  show the code and recovery action. An unknown flash result shows the 25-minute
  wait and offers no Back action.
- The installer layout is compact for 1366x768 screens. Taller content scrolls
  inside the window, and result-screen Back and Close buttons remain outside
  the scrolling content.
- Restore stock firmware uses the saved stock image and offers a firmware ZIP
  picker only when no saved image is found. For a damaged saved backup, use
  `restore-stock --firmware <zip>` to rebuild it from matching stock firmware.
- Remove installer data clearly describes host cleanup. It removes saved state
  and owned GUI logs, while keeping the app and Windows USB driver binding.
  It does not remove diskOS from the Disc.
- A new startup animation: a light splits into two spectra across the disc,
  the diskOS wordmark appears in the hub, and the hub opens to reveal the
  interface. It lasts about 2.4 seconds; the glow follows the theme's accent.
  The wordmark uses Archivo, matching the website and installer.
- Settings > Display > Disc Colour is removed. It controlled only the previous
  startup drawing; the new animation uses the theme's accent.
- Theme details extend through lists, empty states, missing artwork and
  screensaver song displays: hi-fi's amber row lights, bauhaus's geometric
  bullets, blueprint's figure numbers and drafting marks, stone's pebble
  controls, and zine's tape and pressed-row underlines.
- Something has a blinking clock colon, dot arrows and a dotted Now Playing
  progress ring. Terminal has a blinking header cursor, aligned playlist
  durations and a highlighted Now Playing label in Up Next. Hi-fi uses bezel
  ticks for progress; stone uses a fading progress arc.
- Bauhaus Now Playing fills the left half with artwork, uses sharper covers
  when available and gives queue position its own line.
- Screensavers show song text while playing and hide it when paused. The Analog
  dial is smaller and higher. Vinyl hides song labels while showing a cover
  and keeps fallback text when no cover is available.
- Unchanged song and clock text no longer causes repeated text and layout work.
  Now Playing title scrolling stops off screen, and Vinyl rotation stops when
  there is no cover. With Covers only selected, idle cover preparation waits
  for work instead of waking every second.
- Screen-off drawing is paused and repainted before the backlight returns.
  Cover Flow and the Library scroll-letter timer stop checking when idle.
  Library row loading pauses on other screens and resumes on return.
  Sliding screens avoid repainting areas already covered by the moving panel.
- A-Z jumps wait for their target rows without building the remaining list
  at once; manual scrolling cancels a pending jump. Large playlists build
  rows incrementally. Folders are read in the background.
- Cover Flow prepares missing covers in the background, uses placeholders
  until they are ready and releases its cover memory after leaving.
  Library buffers are allocated as needed rather than all at startup.
  Album track-order reads are batched when possible, and artist queue
  membership checks use less work.
- Rescans skip embedded artwork when reading tags and look up existing files
  more efficiently. Cancelling or failing a rescan no longer forces an
  otherwise healthy library to reload.
- Unchanged settings and playlist records avoid unnecessary writes. Playlist
  counts are read together, and Last.fm avoids repeatedly rereading its queue
  just to count pending scrobbles.
- Wi-Fi checks back off while a connection remains down and return to faster
  checks after activity, wake, network recovery or a Wi-Fi setting change.
  Weather skips background downloads while offline, fetches after reconnecting
  and backs off repeated connected failures.
- Weather requests now use HTTPS.

### Fixed

- Keep or Go back is not asked again after changing theme, appearance, text size
  or language. A failed save leaves the question open for retry.
- Later app updates can complete their trial after an earlier update was kept,
  instead of rolling back because the running-build record outgrew its buffer.
- After confirmed playback, failed update-health saves are retried for a few
  minutes rather than abandoned after one attempt.
- A build previously rolled back on this Disc is refused before its bundle
  downloads, with an explanation.
- The boot install script no longer performs installation during shutdown.
  Requires a full flash.
- The 1.2.1 system image refuses saved app updates below its update limit.
  An app update does not change that image limit. Requires a full flash.
- Keeping an update waits until the previous saved version has actually been
  moved aside. If that move fails, the update remains on trial instead of
  being moved inside the previous version's directory. Requires a full flash.
- If a trial start cannot be recorded, that boot uses the previous accepted
  or flashed app and leaves the trial for a later start. Requires a full flash.
- A proven healthy update remains on trial if saving its approval fails,
  with promotion retried on a later start. Requires a full flash.
- On startup, an install log larger than 64 KB is trimmed to its newest
  300 lines. Requires a full flash.
- The boot installer removes its spare rejected-app copy after verifying
  the installed app, freeing space for updates. Requires a full flash.
- Returning from Now Playing to top-level Songs, Albums, Artists or Genres
  no longer rebuilds the list.
- Installer console output uses readable replacements for characters the
  terminal cannot display.
- Missing or unloadable firmware-decryption dependencies stop the installer
  before firmware work with E105 and corrective instructions.
- Unknown flash results explain the required wait: leave the Disc powered
  and connected, and the computer awake, for at least 25 minutes after the
  error. Do not retry or disconnect during that wait.
- USB flashing handles partial transfers and bounds stalled communication.
  Reconnection requires exactly one device at the same USB location.
- Chinese, Japanese and Korean names and titles use the stock font fallback
  across themes and text sizes. Latin-1 accented letters retain the zine and
  something title faces; zine also retains its face for Cyrillic U+0400-U+045F.
  Theme letter case handles Latin-1, Latin Extended-A and those Cyrillic pairs.
- V2.57 Wi-Fi skips the extra startup hold when Bluetooth is saved off.
  Other startup cases can still take about a minute.
- Headphone output can recover after a UI restart or boot that left the
  player routed to Bluetooth without a connected audio device (issue #9).
- On V2.57, one power-button press wakes diskOS Screen Off. Button wake
  restores the user's diskOS brightness.
- Now Playing accepts song details containing double quotes rather than
  retaining the previous song.
- diskOS briefly checks for a missed track advance near a track's end while
  the screen is lit, and checks once on wake. Advancing from one mono Opus file
  to another can still leave the old track displayed. Some mono FLAC, MP3 and Opus
  files still play slowed down in the stock player.
- Returning to Now Playing retries missing artwork after a theme restart or
  a temporary refusal of card access.
- Missing and blank artist tags appear under Unknown artist, and opening
  or playing that group finds its songs.
- Screensaver titles and artists refresh for tracks sharing a path, including
  CUE and SACD ISO tracks. Song displays leave room below the date and stay
  inside the round screen at different text sizes.
- Weather unavailable remains tappable on Home. Cancelling the city keyboard
  preserves the saved city; saving an empty city selects automatic location.
- Zine search text, placeholder and cursor remain readable. A populated
  search field stays clear of Back's touch area and the round screen's edge.
- The A-Z grid is readable in zine's dark appearance and dims the list behind
  it in every theme. Long translated Scan Library labels fit the round screen.
- V2.57 Custom EQ sliders visibly show their view-only state; tapping a band
  explains "Not on this firmware".
- Switching Wi-Fi networks renews the address for the selected network.
  Network details refresh during connection; IP and router values wait for
  a ready connection. Slow connections get about 75 seconds before failure,
  and status and scan checks have time limits.
- Saved Back swipe distances are clamped to 20-200 pixels when loaded.
- Delayed song details alone no longer trigger "Couldn't start playback".
  The playback-start check uses player progress and audio-output evidence;
  ambiguous brief starts retire quietly instead of producing a late error.

## [1.2.2] - 2026-10-08

Adds playback queue with history, 24-hour battery and usage history screen, instant quick settings clock, folder shuffle and A-Z jump controls, virtualized library list performance, extended Japanese CJK font coverage, and Bluetooth/IPC synchronization hardening.

### Added

- **Playback Queue with History and Folder Scope**: Dedicated Up Next queue viewer showing upcoming tracks, playback history, and folder-scoped queues, filtering dummy 24-hour durations.
- **24-hour Battery & Usage History Screen** (`SCR_USAGE`): Interactive 24-hour circular dial showing battery charge level, charging status, screen-on time, and playback time; accessible via new Battery tile in Apps or Quick Settings status shortcut.
- **Quick Settings Live Clock & Battery Shortcut**: Instant system clock and date display upon opening (eliminating the `--:--` placeholder delay) with a clickable battery shortcut directly to the usage dial.
- **A-Z Jump Index and Folder Shuffle Play**: Fast-scroll alphabet jump index grid in Folder Browser and Album Wall, plus Folder Shuffle Play action.
- **Supplementary CJK Japanese Kanji / Kana Font Coverage**: Expanded supplementary font tables with 3,724 Japanese glyphs for complete metadata rendering.
- **Custom OTA Update Tooling**: ECDSA P-256 key generation (`diskos-ota-keygen.sh`) and Wi-Fi binary signing/push staging (`diskos-ota-push.sh`).

### Changed

- **Music Library Virtualization**: Recycled row slot pool (`virt_update`, `virt_populate_row`, `virt_init_pool`) eliminating scrolling stutter on collections with thousands of tracks.
- **Deployment Tooling Improvements**: Added stream progress display (`pv`), remote MD5 pre-swap verification, structured 5-step UI swap, and SSH warning suppression in `diskos-deploy.sh`, `diskos-shot.sh`, and `diskos-touch.sh`.

### Fixed

- **Bluetooth & IPC Playback Title Mismatch**: Resolved rapid-skip metadata mismatch (e.g. "Shiawase Negau Kanata Kara") via `ipc_set_quiet` / `ipc_player_ready` synchronization; hardened Bluetooth subprocess lifecycle (`sync_system`, `bg_system`).
- **Now Playing Navigation Back-Stack Cycle**: Fixed back gesture cycle loop between Now Playing and Queue, added `ui_np_seek_cancel()` to cancel seek gestures when navigating away.
- **Album Wall Scrub Bubble**: Constrained A-Z scrub bubble popup to active scrollbar dragging, avoiding unwanted popup during normal album art swipe gestures.

## [1.2.1] - 2026-10-06

Adds Mode Lockdown screens, external USB DAC host mode audio output, and hardware sleep & backlight power-sequencing fixes.

### Added

- **Mode Lockdown screens**: Dedicated full-screen lockdown interfaces for Bluetooth Receiver, USB DAC, and USB Storage modes with theme kit styling, circular layout fitting, live connection badges (sample rate/bit depth), Bluetooth AVRCP controls, and swipe-to-exit prompts.
- **External USB DAC host mode output** (`USB Audio`): Route local playback directly to an external USB DAC over USB host mode, with automatic disconnect detection (pausing and null-routing) and clean resumption on reconnect.
- **Touch to Wake toggle** under **Settings -> Display** (default: **Off** for maximum standby battery life). When disabled, the touch controller enters hardware deep sleep (0mA) matching stock standby; when enabled, tapping a dark screen wakes the device.
- Authentic procedural vinyl record placeholder on the screensaver when playing tracks without album art, preventing a pitch-black screen.

### Changed

- Screen sleep now executes the exact stock hardware power-sequencing: CST816T deep sleep (`0x2000ef03`), backlight rail cut (`bl_power = 4`), 120ms controller discharge delay, and ST77916 LCD panel sleep (`0x2000ef01`).
- Screen wake executes stock resume sequence: ST77916 exit sleep (`0x2000ef02`), 50ms stabilization delay, backlight restoration, and CST816T resume (`0x2000ef04`).
- Screen off timeout now operates independently when the screensaver is set to Off.
- Reduced dark-screen main-loop polling interval to 200ms to cut CPU wakeups in half during standby.
- Aligned `source_send` IPC sequence with stock firmware for USB DAC (`0666000C0006` -> 100ms delay -> `0642000C0002` -> `0657000C0001`) and Bluetooth Receiver modes.

### Fixed

- Single-press physical power button wake: synchronized `mq_player`'s internal `screen_on` state via Linux evdev KEY_POWER injection on auto-sleep and touch-wake, ensuring single-press wake without touching process memory.
- Prevented blind touches on a dark screen from clicking or pausing UI controls underneath by disabling LVGL's touch input device while the screen is off and draining finger release on wake.
- Fixed auto-sleep triggering a false manual power-button blank event in the backlight poll loop.

## [1.2.0] - 2026-10-02

Adds V2.57 firmware support, more library and display choices, and signed in-app diskOS updates.
Installing 1.2.0 requires the complete installer and a new flash.

### Added

- Install from unmodified official V2.57 firmware, alongside V2.09, V2.28, and V2.40. The
  installer checks the Disc's existing kernel and recovery against the image before writing.
- Seven themes alongside Default, dark and light appearance, Outdoor Mode, font sizes, and an
  interface language choice with English fallback for missing text.
- Up Next shows the current queue and lets you jump to an upcoming song. Artists and Genres can
  open albums or All Songs.
- Library indexing for AAC, OGG, APE, AIFF, WMA, DSF, DFF, DTS, external CUE sheets, and SACD ISO
  track rows. Indexing does not guarantee playback of every format.
- Song Info with available audio and file details; settings for Album Artist grouping, Play
  Through Folders, track numbers, online album art, and online lyrics. Local and embedded lyrics
  remain available when online lyrics are off; timed lyrics can follow playback.
- Song lengths in library song lists and Up Next, read from the file when the library is scanned.
  Songs whose length can't be read show no length.
- Manual date and time, time-zone choice, and Automatic Time. A new startup animation has a
  selectable Disc body colour.
- On V2.57, screen rotation, volume-key actions, idle power-off, a charging limit, and a choice
  to pause or request shutdown when the Sleep Timer ends.
- Bluetooth Codec selection on V2.57. On other firmware the row says "Not on this firmware".
- Device Info, battery/board temperature, and a reset for diskOS appearance and behavior
  settings that keeps music, network, Last.fm, EQ, and audio settings.
- Signed in-app diskOS updates for later releases. The 1.2.0 release image includes the diskOS
  release key by default; its GUI checkbox, "Allow diskOS updates over Wi-Fi", starts checked.
  Untick it or use `--no-ota` to omit the key; `--ota-key PATH` uses your own public key.
  Settings > System > Update diskOS updates only the diskOS app, not stock firmware or the whole
  image. Allow diskOS Updates can stop downloads; turning it off with a staged update offers
  Discard or Cancel. A trial update offers Keep or Go back and rolls back if it fails to start.
  The first update over Wi-Fi will be a release after 1.2.0.
- A macOS release package for Apple Silicon with prebuilt flash tools.

### Changed

- Album artwork is now decoded by a new helper, `diskos-artdec`, instead of the stock `ffmpeg`
  command (which stock V2.57 ships without image decoders). It runs as a separate short-lived
  process and covers embedded and same-folder artwork on every supported firmware. Cached covers
  are rebuilt once after the update.
- Restart is back in Settings. It waits for diskOS card work and a successful flush; if either
  cannot finish, it reports the problem and leaves the Disc running.
- Library rescans show progress and a Stop action. Cancelling keeps the previous library.
- Playlists retain their order and appear in the stock playlist registry. You can add a displayed
  album, artist, or genre song list, remove a song, and rename, export, or delete a playlist. M3U
  import searches card folders within limits and preserves a differing same-name playlist.
- V2.57 Favourites use the player's favourite queue and row identity. Gain is available on V2.57;
  its Custom EQ editor is view-only.
- Update from SD Card identifies a file named like a stock update and explains the stock route;
  diskOS does not run that update.
- Wi-Fi lists place the connected network first and handle encoded names, passwords, and
  connection feedback more carefully.
- Setup and tool builds now work on Arch, CachyOS, and Fedora as well as Debian and Ubuntu;
  `doctor` identifies the Linux distribution.

### Fixed

- The installer reports a USB-permission problem when it sees a mask-ROM Disc but cannot open it.
- Saving the clock no longer waits on the interface loop for the RTC.
- Album cover caching no longer depends on an inode that can change for the same card file.
  Saved accent colours can refresh when cover content changes.
- Playlist export uses a unique temporary file instead of a fixed name that could overwrite a
  user's file.
- A refused playback send no longer opens Now Playing as if playback started. Delayed local-player
  setup no longer interrupts a song that has already begun. CUE and SACD track replay avoids a
  timeout route reset that could stop a live stream.
- A back swipe starting on a control no longer also taps that control, which could go back two
  screens or change a setting.
- Long one-button messages are no longer covered by the OK button.
- With Weather on Home off, diskOS no longer makes a weather request at startup.

## [1.1.3] - 2026-09-24

A safety release. The stock FiiO player, which diskOS runs alongside, tries to unmount the microSD
card and then deletes everything at the card's mount point, without checking that the unmount
worked. If the card is still mounted, this can delete its contents. The flaw is confirmed in the
stock player on V2.09, V2.28, and V2.40. It has not been proven to be the cause of the two reported
card wipes. Every diskOS user should update; updating requires the complete installer and a new flash.

### Added

- A delete guard in the flashed image, at `/bin/rm` and first on the stock player's `PATH` (so the
  stock fallback and watchdog launches are covered too). It refuses `rm` on the card or anything on
  it; the card's folder can only be removed when it is empty and unmounted. It blocks this specific
  deletion path, not every possible way to lose data.
- A boot selector that runs before diskOS starts and decides once per boot between diskOS and stock.
  Any missing or invalid result boots stock.
- Hold Volume Up for about three seconds to force-close an external app. A message says if this is
  unavailable.
- Extra punctuation in the 14, 16, 18, and 20 px interface fonts, and eight more CJK characters in
  the fallback font. This is not full CJK support.

### Changed

- USB Storage now waits for diskOS to finish reading and writing the card, confirms the card really
  was handed over and really came back, and only then resumes artwork caching. A pending handoff
  survives a UI restart.
- USB Storage is refused during a library scan, and whenever diskOS cannot confirm that the card is
  protected or who owns it. If a handoff is left uncertain, diskOS keeps card access off and asks for
  a reboot.
- Library scans, browsing, artwork, lyrics, book details, playlists, and playback commands wait for
  these card checks. If protection is unavailable, diskOS keeps its card features off and says so;
  the stock player still starts.
- On V2.40, cold-boot card handling tries the direct mount before sending an insert event.
- First-boot setup uses only the UI built into the flashed image; the SD-card fallback copy is gone.
- Restart is temporarily removed from Settings until it can wait for card writes to finish.
- Bluetooth starts off when there is no saved diskOS preference. Saved preferences are kept.
  Turning it on from Quick Settings now waits for it to become ready.
- Home, Now Playing, and Quick Settings share the same transport controls. The play/pause icon
  responds at once. Quick Settings skips 15 s back and 30 s forward in audiobooks.
- Quick Settings allows the transport row only with five or fewer tiles enabled.
- Settings values sit below their labels, controls use the current accent colour, and search fits
  the round screen better.
- Debug Mode's description now says SSH over Wi-Fi; the USB serial shell is for developer builds.
- The installer accepts only the unmodified official V2.09, V2.28, and V2.40 firmware for install
  and build. The untested-firmware override no longer applies to them. V2.57 is refused.
- Release packaging requires the boot selector, its probe and helper, and the delete guard, and the
  image check verifies all of them and the patched boot script.

### Fixed

- Stopping card access now cancels a running scan and all artwork work. A cancelled scan keeps the
  previous library.
- Artwork cache files are flushed to the card before they are used (where the card supports it);
  artwork and playlist exports also try to flush their folder.
- Player start-up and the hand-off to stock have firmer time limits, clean up their helper
  processes, and reset signals correctly.
- Default UI's boot flag is written by a time-limited helper that reports failure.
- Bluetooth status and audio-route checks are time-limited: the check is killed along with anything
  it started, and cleanup never waits without a limit. Bluetooth shutdown no longer waits on the
  Bluetooth service. This closes one way the screen could freeze, not every possible one.
- Scan messages now tell apart refused, running, failed, and finished scans, with a count of skipped
  files. Help hints no longer replace an active message.
- Failed playback, seek, and playlist-export requests now report failure.
- The mode picker keeps showing "switching" until a mode change finishes, and clears the checkmark
  if it fails.
- The boot log keeps earlier start-up messages until it reaches its size limit.

## [1.1.2] - 2026-09-20

### Fixed

- Install now works from a plain source checkout that does not include the bundled squashfs tools:
  the installer uses a system `mksquashfs`/`unsquashfs` from `PATH` (for example from the
  `squashfs-tools` package) when the vendored copies are absent. `usbboot` and the device files
  remain bundle-only. Previously a source checkout failed with "bundled tool 'unsquashfs' not found"
  even when squashfs-tools was installed.
- Added a squashfs LZO capability check that runs before the build and in `doctor`, so a squashfs
  tool built without LZO support fails with a clear, actionable message instead of failing partway
  through the build.

### Changed

- `doctor` now labels each native tool as bundled or system, flags broken entries, and reports the
  squashfs LZO check result. Tool-not-found guidance points at the correct per-platform build step
  instead of advising a re-download.

## [1.1.1] - 2026-09-20

### Fixed

- Fixed rootfs extraction on the default case-insensitive macOS filesystem, resolving E230
  `unsquashfs ... already exists` failures from filenames that differ only in case.
  The installer automatically creates, mounts, and verifies a case-sensitive APFS scratch image
  using `hdiutil`, uses it for extraction and validation, then detaches and removes it.
  Cleanup failures are reported.
- Kept the existing build path unchanged on Linux with a case-sensitive filesystem and on
  case-sensitive macOS volumes. Validated on real macOS with `back_home.png` and `BACK_HOME.png`
  coexisting after extraction, and on Linux. macOS device flashing remains unverified end to end.

## [1.1.0] - 2026-09-20

### Added

- V2.40 firmware support, flash-tested alongside V2.09 and V2.28 on Linux x86-64.
- Album cover flow with horizontal swipe navigation, pre-baked cover sprites, and reflections.
  Dynamic album lists and a bounded sprite cache accommodate libraries with thousands of albums.
- Dedicated Books view for single-file `.m4b` audiobooks with saved listening positions, plus
  chapter navigation from Now Playing (jump to a chapter, current chapter highlighted, with titles
  and durations).
- File/folder browsing on the microSD card, with playback of selected library tracks.
  Playback uses the all-songs queue, not a folder-only queue.
- Bundled Noto Sans glyphs for Latin-extended, Greek, and Cyrillic library text.
- On-device UI source under GPL-3.0-or-later, with vendored LVGL and build instructions.
  Installer, build tooling, and documentation remain MIT licensed.

### Changed

- Increased image and writer capacity from 580 to 768 NAND blocks (96 MiB) to fit V2.40's
  roughly 88 MB stock root filesystem, without changing the partition layout.
- Shortened the conservative flash wait; flashing and verification now take about 15 minutes.
- Separated `.m4b` entries from music views and queues, including migration of existing entries.
- Refreshed the circular listening interface and browsing views.

### Fixed

- Refuse undersized or unrecognized writers before any NAND write, and check reported capacity
  after flashing to detect writer/image mismatches.
- Repad older short saved stock images into a separate copy, then validate them before restore.
- Preserve USER EQ presets changed on the stock player when reselected in diskOS.

## [1.0.0-beta]

### Added

- Initial public release for the FiiO Snowsky Disc, flash-tested with V2.09 and V2.28 on Linux x86-64.
- Circular player interface with local playback, bezel and alphabet navigation, M3U playlist import,
  and album-art dynamic colors.
- Experimental weather and Last.fm integrations.
- Graphical and command-line installer using a local Python environment.
- Local image builds from user-supplied official FiiO firmware archives.
- Mask-ROM USB flashing with a bad-block-aware writer and block verification.
- Saved-stock restore, persistent stock UI selection, one-time stock boot, and stock UI fallback.
- Opt-in SSH Debug Mode and separate public and development installation variants.
- macOS build-from-source path, unverified for release artifacts and device flashing.
- MIT-licensed installer tooling, recovery documentation, hardware notes, and third-party notices.
  The initial release distributed the UI as a binary; UI source publication followed in v1.1.0.

[1.1.3]: https://github.com/b0hemia/diskos/compare/v1.1.2...v1.1.3
[1.1.2]: https://github.com/b0hemia/diskos/compare/v1.1.1...v1.1.2
[1.1.1]: https://github.com/b0hemia/diskos/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/b0hemia/diskos/compare/v1.0.0-beta...v1.1.0
[1.0.0-beta]: https://github.com/b0hemia/diskos/releases/tag/v1.0.0-beta
