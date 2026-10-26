# diskOS 1.2.2 release notes

Released 2026-10-24.

Adds Beam (play the Disc's music on Cast and AirPlay speakers and TVs), AirPlay receiving
from an iPhone or Mac, a custom screensaver image, editable device and Bluetooth names,
optional update checks, a swipe up to Home, a closing animation when the Disc powers
itself off and a Get firmware button in the installer, and fixes startup, Bluetooth,
library, charging, Hebrew text, Up Next and installer handling.

1.2.1 users can install the signed app update from Settings > System > Update diskOS if
updates are allowed. Fresh installs and older versions use the installer. App updates do
not replace the boot install script or the system image. Changes marked "Requires a full
flash" need the complete 1.2.2 installer.

Read the [installation guide](INSTALL.md) before flashing. Back up your music, keep the
saved stock image on separate storage, and keep the Disc connected and computer awake
until verification finishes. Recovery is not guaranteed.

### Added

- Beam: play what is on the Disc on speakers and TVs on your Wi-Fi. Open it from the Beam
  button on Now Playing, the first row of the Options page, or the Beam tile, which is off
  until you add it under Settings > Controls > Quick Settings. The picker lists Cast and
  AirPlay devices found on your network, each marked with its type. While you beam, the
  playback controls and the volume dial control the receiver, and the Disc stays quiet.
  Beam needs press, double press and long press volume-key actions set to Adjust Volume;
  switching them to track controls is unavailable while beaming.
- Beam to AirPlay: PIN pairing uses a keypad on the Disc and remembers successful
  pairings. If pairing fails before PIN entry, Beam keeps the failure on screen so you
  can retry. AirPlay 1 receivers (marked "AirPlay 1") are listed too; ones
  protected by a password are not supported. AirPlay beaming decodes mono or stereo MP3,
  FLAC and PCM WAV at 8-384 kHz. The Disc converts AAC, ALAC, Opus, Ogg Vorbis, WMA, APE
  and DSD for the receiver as it plays. AirPlay output is 44.1 or 48 kHz at 16 or 24 bits,
  depending on receiver support; AirPlay 1 uses 44.1 kHz at 16 bits. The Beam screen shows
  the output rate and bit depth and offers a Volume button.
- Compatible AirPlay receivers show the track's cover, title, artist and progress.
  TV remotes can play, pause and skip to the next or previous track where supported.
- Beam to Cast supports MP3, AAC, Opus, Ogg Vorbis, and 16- or 24-bit FLAC and PCM WAV up
  to 96 kHz, in mono or stereo. Google Home Mini receivers accept FLAC up to 48 kHz. ALAC,
  AIFF, WMA, APE, DSD, WavPack, SACD ISO files, CUE tracks and audiobooks cannot be beamed
  and say so.
- Beam follows library selections, favourites, playlists, play modes and Up Next. For
  AirPlay, hold an upcoming track to play it next or remove it. Unsupported tracks show a
  reason, and automatic queue progression reports skipped tracks.
- Beam offers Search again, Wi-Fi settings, Cancel while connecting and Stop beaming.
  When the current track can continue on the Disc, Stop beaming and Play on Disc
  resume its last playback position once the receiver is known to have stopped,
  without briefly playing from 0:00 or flashing 0:00 on the clock.
  If Cast has no track to resume, Stop beaming ends the session and leaves the
  Disc paused. AirPlay offers Forget pairing after a PIN
  or verification failure. Its Play on Disc confirmation explains when only the current
  track can continue or the receiver may still be playing.
- While beaming, lyrics follow the remote track, Favourite toggles for the playing track are unavailable, and
  Song Info hides the Disc's output sample rate. Cast listening can scrobble to Last.fm;
  AirPlay sending and receiving do not. Remote sessions hold off idle shutdown, and the
  sleep timer ends remote playback.
- AirPlay Receiving: pick it as a source (Quick Settings > Source, or Settings > Network >
  Audio Source) on V2.40 and V2.57, then choose the Disc on your iPhone or Mac. It appears
  as SNOWSKY DISC. Now Playing shows the sender's track, artist and cover. The Disc does
  not power off from idle while the receiving screen is up. Return to Local takes you back
  to local playback. The row is hidden on V2.09 and V2.28.
- AirPlay Receiving has a themed Now Playing view with playback controls, timing when
  supplied, connection retry and Wi-Fi settings. Return to Local asks for confirmation
  when needed; Options offers Return to Local and Source. A sleep timer ends receiving,
  and idle shutdown stays blocked even if receiving has failed or its state is uncertain.
- Custom screensaver image: Settings > Display > Saver Style > Custom shows your own JPG,
  PNG, BMP or animated GIF from the CustomCover folder on the SD card. GIFs must be 4 MiB
  or smaller and no larger than 364 x 364. Choose Image opens a preview screen (some images show a placeholder), and the
  saver can overlay Time, Date, Battery, Now Playing and Weather, in a font of your choice
  (Theme, Plain, Digital, Mono or Rounded) at the top, centre or bottom. If the image
  cannot be used, the saver falls back to Minimal.
- Custom starts with Time and Date on, the Theme font and Bottom position. Use Saved Image
  reuses the last image; Preview shows the complete result with labelled sample data where
  needed. Tap Save to keep overlay, font and position changes; leaving Custom without
  saving discards them. Weather uses the last reading, marks older readings Cached and
  hides them after two hours; Custom and Preview start no weather requests.
- Editable names: Settings > Network > Device name sets the Disc's network name (letters,
  digits and hyphens, up to 32 characters, starting and ending with a letter or digit) and
  the name Beam shows to receivers. It applies after a restart. Requires a full flash.
  Settings > Network > Bluetooth name sets the name other Bluetooth devices see (printable
  ASCII, up to 32 characters and nine words). A paired device's details have Rename to
  give it a nickname on the Disc; Use original name restores it. Reset diskOS Settings
  clears both names and all nicknames, and keeps your pairings.
- Settings > Device > Automatic Update Checks (off by default). When on, the Disc looks
  for a new diskOS release now and then over Wi-Fi and shows a one-time "diskOS x.y.z is
  available" notice and a dot on the Update diskOS row. It never downloads or installs
  without your tap.
- Update downloads show a progress bar with a percentage and a Cancel button, and then the
  Verifying and Saving steps.
- Swipe up from the bottom edge of the screen to go Home. A swipe right from the left edge
  still goes back one page. Going Home closes the screens above it; Library next opens at
  its top level.
- Closing animation: when the sleep timer shuts the Disc down, or it powers off after
  being idle, with the screen on, the screen closes into the disc, a light travels once
  round the ring while the card is saved safely, and the screen fades to black when it is
  done. If the save cannot finish, the screen comes back. Turning the Disc off with its
  power key is unchanged.
- Installer: Get firmware downloads the official FiiO firmware for the version on your
  Disc, checks it against the installer's recorded checksum and keeps it on the computer
  for next time. You can still choose a firmware file you downloaded yourself. If the
  download fails, the installer offers FiiO's download page and checks your Downloads
  folder.

### Changed

- Added SIL OFL 1.1 licence notices for the Beam glyph and merged icon font data, and
  Chromium copyright notices for the Cast schema files.
- Settings is organised into eight pages: Playback, Audio, Network, Display, Library,
  Controls, Battery & Power and Device, with short section headings inside the longer
  pages. Choose a Settings page to reach its rows. Some rows open a picker or further
  controls. Battery & Power shows the battery's charge, health, cycles and temperature.
  Device Info now includes what About showed. Working Mode is now called Audio Source, Old
  tags is Tag Encoding and Update from SD Card is Stock Update Guide. The DSD Output
  placeholder is gone. The Quick Settings picker shows how many tiles are selected: up to
  six, or five when playback controls are shown.
- Settings switches sit on their rows. Gain, DRE and Gapless are now on/off switches
  instead of cycling rows, the Wi-Fi and Bluetooth rows in Settings > Network carry their
  own power switch, and Equalizer and Saver Style open a list of choices.
- Settings remembers each page's scroll position; choice lists open with the current
  selection in view, and setting shortcuts reveal the relevant row. Outdoor Mode changes
  return to its setting. Tapping an off Wi-Fi or Bluetooth row starts the radio; further
  taps wait until startup finishes.
- Settings rows no longer carry a line of help text under them, except for a few one-line
  hints such as Gain ("For demanding headphones") and ReplayGain ("Evens out track
  volume").
- Page titles are centred and use up to two lines. A title that still does not fit, such
  as a very long album name, ends with "...".
- Search has a wider field below its page title, and Custom EQ leaves room for longer
  titles.
- Shared information and confirmation dialogs now fit their text. Longer messages scroll
  above buttons that stay in view.
- Audiobook chapters and narrator details are read in the background. The chapter list
  shows "Loading chapters..." until they are ready.
- Bluetooth details load in the background with Checking and Unknown states, and offer
  Connect or Disconnect, Rename, Use for audio and Forget This Device. Connection failures
  identify the step that failed.
- Full-screen album art shows the current track's sharp 360 px cover in every theme,
  instead of the player's cached online-art file or a blurred backdrop. It is drawn edge
  to edge, and the border lines some themes drew around it are gone.
- Something keeps its clock colon steady on Home and the screensaver instead of blinking.
- The Quick Settings handle is at the bottom of the panel, where the swipe up that closes
  it starts.
- Reinstalling with the full installer starts the diskOS version inside that installer and
  clears downloaded diskOS updates. Settings and music are kept. Requires a full flash.
- Bluetooth headphones: on V2.57 diskOS no longer switches the Bluetooth codec a few
  seconds after the headphones connect. The stock player applies the codec chosen in
  Settings > Network > Bluetooth Codec, and changing that row sends the choice at once.

### Fixed

- Cast and AirPlay covers update when the track changes, including covers that become
  ready after playback starts. Beam waits up to 15 seconds for a missing cover.
- Beam to Cast: seeking, volume keys and the volume dial now control the receiver;
  elapsed time follows receiver reports, and returning to the Disc resumes that position.
- Beam keeps connection and pairing failures on screen with retry guidance, and opens
  Controls directly when volume-key settings need changing. The search ring moves only
  during discovery.
- Battery & Power keeps each battery reading on its own line, including temperature.

- macOS installation bundles Python, modern Tk, libusb and LZO tools, unblocks bundled tools and prevents sleep during flashing; Safari ZIP guidance is clearer.
- Wi-Fi keeps your saved on/off setting across startup and retries reconnecting automatically while showing when Wi-Fi is starting.
- Beam: searching for speakers and TVs no longer fails before the network search starts.
- Slow left-edge Back swipes work where another control does not own the gesture.
  Selecting a track in Up Next returns directly to the existing Now Playing screen.
- The Analog screensaver shows the correct hands immediately on entry.
- Playlist creation, additions, removals and imports, and favourite removals report
  failures. Failed playlist creation keeps the entered name ready to retry.
- Audiobooks report unavailable chapters and failed position saves.
- A failed Custom EQ read keeps the previous curve and slot, and refuses editing until a
  successful read.
- Last.fm retries account and session setting saves that could not finish.

- "Player didn't respond" no longer appears at startup. Automatic commands at boot wait
  until the player has answered, and a real failure is reported at most once per boot
  (issue #29).
- Computer audio: the USB DAC source (the Disc as a computer's sound card) sends the work
  mode that plays the computer's audio (issue #24).
- Library: one unreadable or hidden entry on the SD card no longer stops deleted songs and
  books from leaving the library. Only the affected folder keeps its rows (issue #27).
- Library: a tag read that could not finish no longer replaces a song's stored tags, and
  large Ogg, Opus and Vorbis comment blocks are read without loading embedded pictures. A
  busy library no longer shows as empty.
- Cover Flow drops albums deleted from the card after a rescan, and retries a failed
  reload.
- Hebrew titles, artists and albums no longer show boxes in Library and Now Playing.
  Letters are shown, but right-to-left ordering is not corrected (issue #20).
- Vinyl Now Playing draws a round record in every theme with a smaller centre label, and
  the centre label no longer overlaps the no-cover note.
- The Quick Settings Wi-Fi tile shows whether Wi-Fi is actually on, not only the saved
  choice.
- The charging icon follows the stock player's own charging report: it shows while the
  Disc charges and goes away when the battery is full. The Disc does not power off from
  idle while it is charging.
- The Disc does not power off from idle while Bluetooth Receiving is selected, while it is a computer's sound card (USB DAC), or while
  its card is open on a computer (USB Storage).
- Library: Back, and coming back from another screen, return you to where you were in the
  list, with the item you opened in view, instead of the top.
- Up Next and the Beam queue no longer show 1440:00 for a track whose length is not known
  yet; a real recording of 24 hours or more keeps its length.
- If a Bluetooth speaker or headphones disappear while playback is paused, the Disc
  returns its output to itself quietly instead of leaving the player routed to the missing
  device.
- Text entry over its length limit shows "Text too long" instead of being cut short.
- Windows: flashing no longer fails when the temporary folder is a short (8.3) path, for
  example with a user name that contains a space. A flash that cannot start because
  Windows validation refuses it reports E304 with no recovery wait (issue #26).
- macOS: rebuilding the native tools from source checks the linked Homebrew packages and
  their bundled libraries, and accepts a revision suffix or several installed versions
  (issue #23).
- A full flash gives each installer image its own identity, so flashing an older installer
  can no longer leave a newer saved app update selected. Requires a full flash.
- A saved app update is kept and checked again on the next boot when the boot script could
  not finish verifying it, instead of being deleted. Requires a full flash.

## Why diskOS updates take time

diskOS is built around enjoying your music. Every addition should solve a real problem,
fit the round screen, and respect playback, battery life and your library.

More features do not automatically make a better player. Sometimes improving diskOS means
adding something; sometimes it means simplifying a screen, fixing an interaction, or
leaving an idea out. In 1.2.2 that meant removing the help lines from Settings, regrouping
Settings into clear sections, and replacing a hold gesture with a simple swipe up to Home.

A successful build is only the beginning. Source checks help catch regressions, but they
do not establish how every change behaves on a real Disc. Bluetooth, audio routing,
power-off and anything that touches your SD card get the most care, because a mistake
there interrupts your listening or risks your files.

That work takes time. The goal is to find problems before they reach you. Releases will
still have bugs, and we cannot try every speaker, headphone or listening setup, but we
will say clearly what changed and what is still uncertain.

Reliability and ease of use are features too.

## Acknowledgements

- [ericklv](https://github.com/ericklv) suggested the steady clock colon for the Something
  theme in [PR #25](https://github.com/b0hemia/diskos/pull/25); 1.2.2 takes that idea.
