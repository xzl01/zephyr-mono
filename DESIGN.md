---
name: PaperMono Mono Editorial
description: A reference-led one-bit editorial interface for a local-first information terminal.
colors:
  ink-black: "#000000"
  paper-white: "#ffffff"
corners:
  rectangle: "0px"
  circle: "true circles only"
spacing:
  micro: "4px"
  base: "8px"
  compact: "12px"
  section: "18px"
  page: "18px"
typography:
  clock: "papermono_digits_96"
  metric: "papermono_digits_58"
  title: "papermono_titles_28"
  body: "papermono_cjk_16"
components:
  panel:
    borderRadius: "0px"
    borderWidth: "2px"
  launcherGrid:
    columns: 3
    rows: 3
    cell: "148px x 210px"
    gap: "0px"
---

# Design System: PaperMono Mono Editorial

## Direction

The user-provided monochrome editorial reference is the pinned visual direction.
Its useful grammar is the contrast between oversized type, outlined labels,
ruled lists, strike-through marks, small matrices, dense information and generous
open paper. The UI must not collapse that grammar into one repeated card template.

Order comes before decoration. All screen geometry follows a 4 px base scale;
ordinary rules use only 1, 2 or 4 px weights. Shared columns, title rails and
content baselines must align exactly. Rough outlines, ornamental dashed dividers
and arbitrary 2–14 px offsets are not part of this visual world.

The 480 x 800 SSD1677 panel remains strictly one bit. There is no gray, gradient,
shadow, translucent overlay, random jitter or press animation. Every rebuild is
deterministic so the same state lands on the same pixels.

## Non-negotiable rules

- Every rectangular surface has a 0 px radius, including detail-page panels,
  buttons, headers, footer marks, sleep screens and launcher cells.
- A radius is allowed only when width equals height and the result is a true
  semantic circle, such as a progress dot or slider knob.
- The launcher is one shared-border 3 x 3 grid, never nine floating cards.
- Grid cells share geometry with touch hit testing. No second coordinate table is
  permitted.
- Each launcher cell must use a different composition; changing only its title or
  one-character mark is insufficient.
- Every launcher cell shares a `12 px` title inset, a `34 px` title lane, a solid
  divider at local `y=52`, and content baselines at `68 / 104 / 144 / 172 px`.
- Frequently changing values stay black on white. Ordinary navigation surfaces
  and action labels remain paper-white; solid black is limited to thin rules,
  compact marks, progress and an exceptional critical control.
- A motif may summarize a real value only when that value is available from the
  application model. Decorative fixed numbers, percentages, volume levels and
  market deltas are prohibited because they look like live device state.

## Color

Paper White `#ffffff` is the dominant surface. Ink Black `#000000` provides text,
outlines, dividers, matrices and compact emphasis marks. Hierarchy comes from type
scale, whitespace and rule weight before fill; the one-bit panel never simulates a
light color with noisy dithering.

## Typography

- `papermono_digits_96`: the calibrated home clock. An uncalibrated RTC uses the
  literal `TIME` in Montserrat 48 together with an explicit Chinese explanation.
- `papermono_digits_58`: dates, counts and progress metrics; includes `%`, `-`,
  digits and colon.
- `papermono_titles_28`: fixed home and launcher headings. It is a Source Han Sans
  SC subset and must include every shipped title, including `便签`.
- `papermono_cjk_16`: all body copy, status, metadata and dynamic Chinese text.
- Montserrat 14/18/28/48 may be used for compact Latin editorial labels.

`tests/host/test_font_coverage.py` verifies the full basic CJK font and all three
specialized UI subsets. Unicode symbols are not a substitute for icons.

## Home screen

The home page is an asymmetric daily brief, not a dashboard grid:

- licensed Phosphor cat mark, wordmark and right-aligned connection status at the
  masthead;
- 96 px calibrated time on open paper, or the truthful `TIME / 时钟尚未校准`
  fallback when the RTC is invalid;
- one straight-edged recent-reading field with an outlined `READ` tab, a truthful
  `打开书库` action and a progress column separated at `x=328`; EPUB/TXT metadata
  is omitted when no recent book exists, while persisted-but-not-loaded progress
  is labelled `已有记录` rather than `未开始`;
- ruled next-event and crossed task rows;
- one aligned bottom row for all apps, system status and power. The app count is
  derived from the launcher item table rather than duplicated as display copy.

The event empty state distinguishes `时间未校准` from a calibrated calendar with
`暂无日程`. The task heading is deliberately `待办`, because the current data query
does not filter by due date; an empty row does not draw a misleading checkbox.

The six visible interaction regions are file-level data shared by rendering and
`home_event()`. The reader, including its progress column, is one 444 px target.
The launcher, system and power targets share `y=644`, `height=88` and 8 px gaps.
Rendering must continue to consume these same rectangles directly.

## Launcher geometry

The launcher grid is fixed:

```text
origin       x=18, y=112
cell         148 x 210
columns      3
rows         3
gap          0
outer bounds x=18..461, y=112..741
```

Eleven applications are preserved across two pages:

1. Page 1: Library, WeRead, Calendar, Tasks, Notes, Remote, Content Center,
   Information Cards and Hardware Tools.
2. Page 2: Settings and Power; the remaining seven cells are blank and inert.

The outer frame and two vertical/two horizontal rules are drawn as shared lines.
The header right-aligns connectivity, total app count and page count to one rail.
Every cell then uses the same title rail and content baselines while its interior
composition remains deliberately different:

- Library: open paper, three outlined book spines, one status rule and the real
  recent title (or `尚无最近阅读`).
- WeRead: outlined local shelf block and compact mark row.
- Calendar: large day and two aligned matrix rows.
- Tasks: neutral checkboxes and ruled items without a fabricated completed state.
- Notes: vertical binding bar and five writing rules.
- Remote: centered outlined `HID` label, three hard-edged outlined keys and a
  centered `BLE HID` footer; the primary key is emphasized by rule weight only.
- Content Center: centered `SYNC`, `LOCAL / WIFI` and an outlined `UPLOAD` action.
- Information Cards: actual provider-card count (clamped to two display digits)
  and three list rules; it never displays a fabricated price or percentage.
- Hardware Tools: square diagnostic matrix.
- Settings: three line controls with true circular knobs.
- Power: oversized black `OFF` on paper with a short terminal rule.

Square mark rows are rendered as one dashed LVGL line rather than one object per
mark. This preserves the reference rhythm without exhausting the 32 KiB LVGL heap.
The fixed-line pool is reset before every page tree is rebuilt.

## Detail pages

Existing information architecture remains intact, but all standard panels use
square 2 px outlines. The short top brush rule is also square-ended. Headers use a
slim vertical marker and two short rules instead of a wide inverted banner;
selected tabs use a heavier outline and underline instead of reversing the whole
surface. Confirmation and power-state surfaces follow the same white-first rule.

## Asset policy

Visible icons must come from a traceable source. The masthead uses Phosphor's
MIT-licensed `cat-fill` converted to a 64 x 64 one-bit LVGL I1 resource. Do not
trace the reference mascot or add improvised emoji/Unicode icon substitutes.
Font and icon provenance is maintained in `docs/font-licenses.md`.

## E-paper behavior

- Do not add transient pressed states; one action produces one destination refresh.
- Keep high-frequency changing pixels on white whenever practical.
- Keep ordinary home and launcher captures below 15% black pixels; reserve any
  exception for an explicit critical or power transition state.
- Use deterministic coordinates and shapes to limit unnecessary dirty pixels.
- Build success does not prove visual quality, ghosting, runtime LVGL heap or touch
  alignment. Those require a flashed C153 and physical interaction checks.

## Verification checklist

- Default, partial-demo and driver-selftest profiles build.
- Host interaction tests cover all nine cell centers and exact outer boundaries.
- Specialized font tests cover `便签`, `-`, `%`, digits and colon.
- Home-to-launcher navigation repeated ten times does not lose any fixed lines.
- Both launcher pages are checked for crop, overlap and correct app mapping.
- Physical C153 confirms square corners, readable one-bit type, return gestures,
  page switching, partial-refresh ghosting and touch hit alignment.
