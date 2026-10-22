# Appearance upgrade TODO — ALL ITEMS COMPLETE (2026-07-12)

Review vs KE9NS PowerSDR (meter options) and Thetis (world map, solar data,
compass rose) — Jon's screenshot reference, 2026-07-12. Meters are vector
QPainter faces, map imagery fetched from NASA Visible Earth (public domain),
cty.dat is AD1C's freely-distributable country file. With the donation-style
GPL release decided, GPL sources (Thetis, KE9NS) are studied and credited
directly.

## 1. S-meter suite — DONE, final lineup: 5 styles
- [x] **Style picker**: click cycles Orion / Edge / LED / Cross-needle /
      Magic eye, persisted. (Bar, Digital-dBm and History styles were
      built, tried on the air and dropped per Jon.)
- [x] **Orion face** (default): the 565's own meter — amber backlit
      background, black lettering, dual printed scales (S-units on the top
      arc, watts smaller on the inner arc, TX reads the lower scale), red
      instant needle + blue peak needle, ORION wordmark (no TT emblem —
      nominative-use call).
- [x] **Edge** edgewise ivory meter.
- [x] **LED bargraph** (Thetis LED item): 28 segments green/amber/red,
      unlit ghosts, peak segment held.
- [x] **Cross-needle TX face** (Daiwa style): FWD + REF needles crossing,
      SWR center; RX face = the Orion amber face.
- [x] **Magic eye** (Thetis MAGIC_EYE): glowing tube, shadow wedge closes
      with signal, crossbar slit.
- [x] **RX meter modes**: right-click cycles Signal / Average / Peak.
- [x] **TX faces** on every style (watts + SWR).

## 2. World map backgrounds (map pack + controls) — DONE
- [x] **Bundled NASA basemaps**: Map: Classic (Blue Marble), Map: Vegetation
      (BMNG July topo/bathy — the "Thetis look"), Map: Night lights (Black
      Marble 2016). All fetched from NASA Visible Earth, public domain.
- [x] **MAP DAY / MAP NIGHT brightness sliders** in DISPLAY (30–120 % day,
      0–60 % night), enabled only when a map backdrop is active.
- [x] **Map: Custom…** entry opens a file picker (any image, persisted path;
      cancel bounces back to the previous backdrop).
- [x] Grayline on every variant; terminator smoothsteps widened for a soft
      Thetis-style blend.

## 3. Solar / space-weather layer — DONE
- [x] **Sun marker on the map** at the subsolar point: orange glow + core,
      `SFI nnn  A nn  K n.n` caption (flips side near the map edge).
- [x] **Solar-Terrestrial data panel** bottom-right of the spectrum
      (green-on-black, DISPLAY "Solar data panel" toggle): SFI, SSN, A, K,
      X-ray class. NOAA SWPC (wwv.txt + daily indices + GOES X-ray JSON),
      polled every 15 min, last good values kept, fully async.
- [x] Band-condition verdict line in the panel (COND GOOD / FAIR / POOR
      from SFI + K).

## 4. Azimuth compass rose — DONE
- [x] Azimuthal-equidistant world disc bottom-left of the spectrum, centered
      on the station grid (DISPLAY → GRID field, station/grid setting),
      day/night tinted, sun-bearing tick on the ring, N/E/S/W labels.
- [x] Click a spot label → rose swings to that station with bearing° +
      distance km (POTA park coords from the API; DX calls placed via the
      bundled AD1C cty.dat). Click inside the rose = manual heading;
      right-click clears. DISPLAY "Compass rose" toggle, persisted.
      pointRoseAt() is public for future rotor integration.

## 5. VFO / top-strip cosmetics — DONE
- [x] Band label in each VFO caption row ("40m", amber, right-aligned),
      refreshed every second so external clients can't desync it.
- [x] "Large VFO digits" DISPLAY checkbox: digits + hit cells scale ~25 %.
- [x] SPLIT badge: red chip on VFO B's caption whenever B carries TX.
- [x] Top strip: UTC + local clock far left, meter right-aligned beside
      VFO A (was stranded at the window edge).

## 6. Smaller polish items — DONE
- [x] Meter reading modes (Sig/Avg/Peak) apply to every style incl. Bar.
- [x] UTC + local clock in the top strip.
- [x] Frequency-scale band-plan shading (US allocations: blue = CW/data,
      green = phone), DISPLAY "Band-plan shading" toggle.
- [x] Spectrum trace color combo in DISPLAY (Soft / White / Green / Yellow
      / Cyan).
