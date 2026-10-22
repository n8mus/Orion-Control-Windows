# Original view sidebar — build plan

Operator's request 2026-10-18. Mockup: https://claude.ai/artifact/A7dNwfXiozG7bE9iKfK385

Goal: the Original (Classic) view's right column becomes a Panel-style
sidebar. It is 300 px wide, with a collapsible button grid on top and a
scrolling column of boxes under it. Each box can float (⧉), hide (×) and
be dragged to a new place. The button corner leaves the top deck. The
compass rose, the tuner/amp card and the solar block leave the waterfall
and become sidebar boxes in BOTH views.

## Decisions (operator, 2026-10-18; all five recommendations taken)

1. The TX bar runs the full window width under the sidebar, as in Panel.
   The sidebar ends just above it.
2. Solar becomes a SOLAR box. The waterfall carries no corner panels at all.
3. Classic boxes: BAND, MODE, RECEIVE (AGC → manual notch), plus ROSE,
   TUNER and SOLAR. Finer splits can come later.
4. Panel gets the same ROSE / TUNER / SOLAR boxes and the same button
   picker.
5. The top deck drops the "routing dead center over the pan" rule. The
   VFO cluster packs left after the meter, and the leftover width sits
   between VFO B and the sidebar.

Defaults chosen without asking (say so if any is wrong):
- Each view keeps its own box arrangement: Panel under `panel/`, Classic
  under `classic/side/`.
- The rose disc follows the box width (about 270 px across, bigger than
  today's 160 default). The DISPLAY "rose radius" slider goes away. A
  floating rose window resizes, so it can be made larger still.
- The UTC/local clock is pinned at the sidebar's foot, outside the scroll
  area, so it is always visible.
- Buttons unticked in the picker are hidden, not destroyed. Their menus
  and right-click actions (CW → WinKeyer box) are unchanged when shown.

## Width budget (CI ruler, DejaVu Sans 10, budget ≤ 1850)

| | today | after |
|---|---|---|
| top deck | 1648 | ≈1368 (no button corner, no 280 px right column) |
| TX bar | 1597 (under the pan) | 1597 (full width) |
| sidebar | 200 | 300 |
| **window minimum** | **1848** | **≈1668 = max(1368 + 300, 1597)** |

That frees about 180 px of headroom, the first real slack since the
budget was set.

## Phases (one commit each, guard / tests after every one)

### 1. Extract the sidebar from PanelLayout
New `src/app/SideBar.{h,cpp}`. PanelLayout's fixed-width side widget,
toggle grid, scroll area, `BoxDropArea`, drop indicator,
save/restore order and `ContainerManager` move there, parameterised by
settings prefix. PanelLayout then uses it. No behaviour change:
`containertest`, `TTC_PANELCHECK=1`, `panel.txt` and the Panel width
(1597) must be identical, and the Classic guard PASSes at 0 px (Classic
is untouched in this phase).

### 2. Classic gets the sidebar
`ClassicLayout` builds a `SideBar` ("classic/side/"):
- `menuRow()` / `toolRow()` now feed the sidebar's button grid. At
  `finish()` the nine buttons (DISPLAY▾ SPOTS▾ AUDIO▾ SDR▾ LOG CW SKIM▾
  CNTST ROT▾) are re-gridded 4 across, as Panel's tools box is. A
  "▲ BUTTONS" header collapses the grid (`classic/side/buttonsOpen`).
- `placeMain()`: the ControlPanel's groups are handed out as BAND, MODE
  and RECEIVE boxes, using `ControlPanel::group()` as Panel's band
  flyout does. The clock moves to the sidebar foot. The window becomes
  [top deck + pan | sidebar] over a full-width TX bar.
- Top grid: column 7 (menu corner) and its 280 px minimum go away, and
  so does the symmetry stretch.
- The contest deck (`placeUnderPan`) still docks under the pan.
- Selftest prints `[classic] sidebar 300`. `TTC_PANELCHECK=1` also proves
  Classic's width is fixed.

Guard: the radio commands and the script log must be identical (it
clicks by button text). The pixels change on purpose, so I build a
candidate baseline and do not promote it.

### 3. ROSE / TUNER / SOLAR boxes (both views), off the waterfall
- `src/ui/CompassRoseWidget`: the rose's painting, state (QTH, bearing,
  label, distance, rotor heading, sun marker, map image) and mouse
  handling (left-click = manual heading, right-click = clear) move out
  of PanadapterWidget. PanadapterWidget keeps `pointRoseAt()` as a
  forwarder, so a spot click still swings the rose, and MainWindow's
  `roseBearingChanged` wiring (rotor aim) is re-pointed to the widget.
- `src/ui/TuneBoxWidget`: the amber card, same text and colours.
  Left-click opens the entry form, right-click opens its menu, as today
  (`tuneBoxClicked` / `tuneBoxMenuRequested`).
- `src/ui/SolarWidget`: the green SFI/A/K/SSN/X-ray readout.
- PanadapterWidget stops drawing all three (CPU and GPU share the
  QPainter top layer, so both lose them together). The waterfall corners
  are empty.
- The DISPLAY checkboxes "Compass rose", "Tuner / amp settings box" and
  "Solar data panel" become the boxes' show/hide, with the same settings
  keys. The rose radius slider is removed.
- Both layouts get a `placeSideBox(id, title, toggle, widget)` call on
  LayoutHost.

### 4. Button picker in the DISPLAY panel
A "Sidebar buttons" group lists every button the current view's grid
can hold: the nine tools/menus, then the box toggles (ROSE, TUNER,
SOLAR, BAND, MODE, RX, and in Panel METER, VFO A …). Each has a
checkbox. Unticked means the button is hidden and never comes back
until it is ticked again. The setting is `sidebar/hidden` (a list of
names; absent = all shown). Unticking a box's button also hides the box.
The grid reflows with no gaps.

### 5. Docs, budget, verification
- CLAUDE.md: the width numbers (1848 → new), the "new toolbar buttons go
  on `topLay2_`" rule (now: `toolRow()`, the sidebar grid, never width),
  the rose/tuner/solar move, and the guard note. CI's comment number.
- Run: Release build plus the CI configuration (`-DBUILD_GPU_PAN=OFF`,
  no Aether meters), `containertest`, the selftest width in both views,
  `xvfb` screenshots of both views at 1920×1080, the Classic guard
  (commands identical; candidate baseline) and `panel.txt`.
- Then: your on-screen check. After your OK the guard baseline is
  promoted (the old one is kept), and then the commit. Nothing is pushed
  until your on-air test.

## Risks

- The rose extraction touches the spot-click → rose → rotor path. I'll
  check it by hand in the running console after phase 3 (RX only, the
  rotor moves only if you click).
- Not1MM can still cover the sidebar if its window overlaps there. A
  floated box can be moved anywhere.

## Status (2026-10-18)

Phases 1-5 built and committed on master (unpushed): def394a SideBar,
a576915 Original sidebar, cecb00b rose/tuner/solar boxes, 75d66d9 picker,
then the docs commit. Original view layout-min 1630 (was 1848), Panel 1597
unchanged; `[classic]` / `[panel]` sidebar width OK under TTC_PANELCHECK;
CI-style build (GPU pan off) + containertest pass. Classic guard: radio
commands and script steps identical in all six cases; pixels differ by
design. Candidate baseline:
`~/.local/share/n8mus/tentec-console/classic-guard/candidate-sidebar` —
promote to `base` (keep the old one, rebuild the noise masks) only after
the operator's on-screen OK.

One behaviour to know: the DISPLAY "Compass rose" / "Tuner box" settings
(`display/rose`, `display/tunebox`) are no longer read; the boxes start
shown and remember their own visibility.
