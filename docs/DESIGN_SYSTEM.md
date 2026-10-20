# diskOS design system

This is the visual and interaction baseline for the device UI, boot experience, project imagery,
and supporting pages. It is intentionally small: diskOS should feel calm, precise, premium, and
native to a round 360 x 360 display.

The implementation references are [theme.c](../ui/theme.c), [theme.h](../ui/theme.h),
[theme_defs.c](../ui/theme_defs.c), and [splash.c](../ui/splash.c). Layout recommendations
below are design guidance; the implementation uses theme-specific sizes and geometry.

## Product principles

1. **Round-native:** compose from the center outward and treat the circular edge as useful space.
2. **Music first:** artwork, track identity, and primary playback actions get the strongest contrast.
3. **Calm by default:** use one accent family at a time, limited glow, and purposeful motion.
4. **Readable at a glance:** short labels, large targets, and clear hierarchy beat dense controls.
5. **Honest motion:** progress always moves forward. Indeterminate work uses a moving segment rather
   than a fake percentage.

## Core tokens

### Color

| Token | Value | Use |
|---|---:|---|
| `CANVAS` | `#000000` | Default dark screen background |
| `SURFACE` | `#1C1C1E` | Cards and sheets |
| `SURFACE_RAISED` | `#2C2C2E` | Elevated surfaces |
| `CONTROL_TRACK` | `#2C2C2E` | Inactive controls |
| `TEXT_PRIMARY` | `#FFFFFF` | Titles and essential values |
| `TEXT_SECONDARY` | `#C7C7CC` | Metadata and supporting labels |
| `ACCENT_PRIMARY` | `#FF375F` | Default accent |
| `ACCENT_IDLE` | `#C4C6CB` | Neutral accent |
| `STATUS_SUCCESS` | `#34C759` | Completed and connected |
| `STATUS_WARNING` | `#FF5E5B` | Warnings |
| `STATUS_DANGER` | `#FF453A` | Destructive or failed state |
| `CONTROL_KNOB` | `#FFFFFF` | Control thumb |

These are roles in the default dark palette. Use `TC(...)` for colors and `TF(...)`
for fonts; theme variants provide their own values. Album-derived accents may replace
the selected accent. The splash has fixed spectrum colors and a hub glow using the
current theme's accent.

### Type

- UI family (as shipped): **Montserrat** for Latin text, with **Source Han Sans** as the CJK fallback. **Inter** is shipped in the hi-fi theme; other themes have their own bundled fonts.
- Suggested display scale: 28 px / 32 px, semibold.
- Suggested title scale: 20 px / 24 px, semibold.
- Suggested body scale: 16 px / 21 px, regular.
- Suggested metadata scale: 13 px / 17 px, medium.
- Avoid more than two weights on one screen. Do not place essential text in the curved edge band.

### Space and shape

Layout guidance for new components:

- Base spacing unit: **4 px**. Use 8, 12, 16, 24, and 32 px steps.
- Minimum touch target: **44 x 44 px**.
- Central safe zone for essential controls: **280 px diameter**.
- Corners: 12 px compact, 18 px card, 999 px pill.
- Borders: 1 px standard; 2 px for selected or focused controls.

## Canonical edge ring

The default Now Playing ring in `ui/ui.c` has a 286-degree sweep and a 74-degree
bottom gap. Some themes use different ring geometry or a straight seek bar.

| Property | 360 x 360 value |
|---|---:|
| Center | `180, 180` |
| Radius | `166 px` (332 px object diameter) |
| Stroke | `6 px` |
| Start | `127 degrees` (lower-left) |
| End | `413 degrees` (lower-right) |
| Direction | Clockwise |
| Thumb | `CONTROL_KNOB` color |

Playback progress fills clockwise. The seek recognizer follows the displayed ring or
bar and distinguishes a seek from navigation and drawer gestures. Volume uses its own
270-degree arc starting at 135 degrees. The boot splash does not use a progress ring.
Keep the edge available for bezel scrolling on list screens.

## Motion

| Token | Duration | Use |
|---|---:|---|
| Press down | 90 ms | Scale from 256 to 240 (about 94%) |
| `ANIM_FAST` | 180 ms | Fast helper animation / press return |
| `ANIM_BASE` | 260 ms | Base helper duration |
| `ANIM_SLOW` | 340 ms | Slow helper duration |
| `SP_MS` | 2400 ms | Startup splash |
| Shutdown | ~1450+ ms | Shutdown animation (contract, storage sync, and dissolve) |

Page transitions use a cubic ease-out curve: 300 ms on entry and 240 ms on return.
The Animations setting controls slide transitions. Keep decorative motion limited and
avoid perpetual pulsing.

## Boot screen and boot animation

### Sequence

1. **Light, 150-1800 ms:** a white streak spreads into two spectra, sweeps around the
   disc, and fades.
2. **Hub, 650-1300 ms:** the hub warms in the current theme's accent.
3. **Wordmark, 950-1900 ms:** the Archivo `diskOS` wordmark appears in the hub and fades.
4. **Reveal, 1800-2350 ms:** the hub opens outward to reveal Home.
5. **Finish, 2400 ms:** the splash layer is removed.

The splash is a timed reveal, not a boot-progress indicator.

### Boot states

| State | Ring | Center | Behavior |
|---|---|---|---|
| Light sweep | Disc rim and spectra | Dark hub | Light moves across the disc |
| Hub glow | Disc remains visible | Theme accent and wordmark | Hub warms as the name appears |
| Reveal | Disc moves beyond screen | Home visible through hub | Glow and wordmark fade |
| Finished | Splash removed | Home | Normal input resumes |

## Shutdown animation

The shutdown animation mirrors the boot splash in reverse, providing graceful visual feedback while flash storage is safely drained and synchronized.

### Sequence

1. **Contract, 0-550 ms:** the hub closes inward from the screen edge over Home (`ok`: 1024 -> 0), sealing the screen beneath the dark vinyl disc. The hub warms in the theme's accent and the Archivo `diskOS` wordmark emerges at the center.
2. **Light sweep & Sync, 550-1000+ ms:** a white streak splits into spectra across the tracks and orbits while storage writers drain and the filesystem sync completes.
3. **Fade & Power off, 450 ms:** once sync is verified, the light dissolves, the wordmark fades, the hub glow cools down to pure black, the backlight rail powers down, and the hardware powers off safely.

### Shutdown states

| State | Ring | Center | Behavior |
|---|---|---|---|
| Contract | Disc moves inward | Dark hub, warming glow, wordmark | Disc seals screen from edge to center |
| Storage sync | Rim and rotating spectra | Theme accent glow and wordmark | Light orbits while card drains and syncs |
| Dissolve | Spectra and rim fade | Glow and wordmark dissolve | Light leaves, display fades to solid black |
| Off | Screen black | Display backlight power rail disabled | Hardware power-off handoff |

## Component rules

### Buttons

- Primary: `ACCENT_PRIMARY` fill, `ON_ACCENT` label, 44 px minimum height.
- Secondary: `SURFACE_RAISED`, `TEXT_PRIMARY`, 1 px border.
- Ghost: transparent, used only where hierarchy is already obvious.
- Pressed: use the shared `anim_press()` helper where scale feedback fits the control.

### Lists & Scrollbars

- Prefer five or fewer visible rows at once.
- Use 56 px compact rows and 68 px artwork rows.
- High-volume lists use virtualized row recycling to avoid memory exhaustion on large collections.
- Scrollbars (`ui_setup_scrollbar`): 3 px translucent indicator along the curvature, fading when at rest; active dragging displays scrub position.
- Keep primary labels left-aligned inside the safe zone; reserve the outer edge for scrolling.
- Truncate long text once; keep motion from competing with navigation.

### Circular 24-hour Usage Dial

- Standardized in `SCR_USAGE` for battery history:
  - 24-hour clock face (midnight at the top, hours clockwise).
  - Concentric tracks: outer band for battery percentage, amber ring for screen-on periods, accent ring for active playback.
  - Centered text metrics: current charge %, estimated remaining runtime from recent drain, and cumulative day totals.

### Artwork

- Album art may be circular or softly rounded according to context.
- Use blur only as atmosphere behind readable content, never as the content surface itself.
- Extract one accent family from artwork; retain neutral text and controls.

### Feedback

- Success is quiet: icon or short line of text, dismissed automatically.
- Warnings explain the next safe action.
- Errors should explain the next action; retain stable codes where the subsystem provides them.

## Accessibility and quality checks

- Maintain at least 4.5:1 contrast for body text and 3:1 for large text and controls.
- Do not encode status through color alone; pair it with an icon, label, or stable position.
- Test every screen at native 360 x 360 resolution, not only in an enlarged mockup.
- Check the 44 px target minimum with fingers, not a pointer.
- Confirm the ring begins at lower-left, moves clockwise, and remains inside the display edge.
- Keep motion readable on the software renderer; respect the Animations setting for page transitions.

## Reference assets

The shipped splash is defined by [splash.c](../ui/splash.c). The earlier ring assets
remain in the repository as design references:

- [`assets/boot-animation.gif`](assets/boot-animation.gif): ring animation concept
- [`assets/boot-screen.png`](assets/boot-screen.png): ring screen concept
- [`assets/boot-storyboard.png`](assets/boot-storyboard.png): ring storyboard
