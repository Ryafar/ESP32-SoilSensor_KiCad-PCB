# Soil Sensor rev 0.1.0 — PCB dimensions for enclosure design

Mechanical companion to [hardware-info.md](hardware-info.md): what an enclosure
has to fit around. Board-level positions come from the layout
(`overview.kicad_pcb`, KiCad 10); component envelopes are the courtyards from
that layout. Heights marked *check* are not in the KiCad file and are estimates
until confirmed against the datasheet or the STEP model.

| File | Contents |
|---|---|
| [3D/soil-sensor_pcb_dimensions_rev0.1.0.pdf](3D/soil-sensor_pcb_dimensions_rev0.1.0.pdf) | Dimension drawing, vector, prints 1:1 |
| [3D/soil-sensor_pcb_outline_rev0.1.0.dxf](3D/soil-sensor_pcb_outline_rev0.1.0.dxf) | Outline with radii, holes, keepout, top/bottom courtyards, button and LED centres on separate layers (mm, AutoCAD R2010, origin bottom left) |

For an exact 3D model, export the board with all parts from KiCad via
*File → Export → STEP* and import it into the CAD tool — the estimated heights
below can be measured there.

---

## 1. Board key data

| Parameter | Value |
|---|---|
| Width (X) | 31.0 mm |
| Height (Y) | 43.0 mm |
| Thickness | 1.6 mm (stackup 1.6058 mm, 4 layers) |
| Corner radius | R2.5, all corners |
| Mounting holes | 2 × Ø2.2 NPTH, for M2 |
| Assembly | double-sided: top electronics, bottom battery and sensor connectors |
| Silkscreen | "Soil Sensor v0.1.0", Robin Bühler, 31.07.2026 |

## 2. Reference system

Origin 0/0 is the bottom-left corner of the board, taken at the virtual sharp
corner before the radius. X points right, Y points up, viewed from above onto
the top side — so the USB-C connector sits on the bottom edge (Y = 0).

In KiCad that origin is (175.865 / 156.4), the grid origin:

```
X = X(KiCad) − 175.865
Y = 156.4 − Y(KiCad)
```

Z is measured upward from the top surface of the board, and for bottom-side
parts downward from the bottom surface.

In the drawing, blue marks top-side parts, pink dashed marks bottom-side parts
and green hatching the antenna keepout.

## 3. Mounting holes

Two non-plated Ø2.2 mm holes for M2, diagonally opposite, each 2.5 mm from both
edges. Hole pitch is 26.0 mm in X and 38.0 mm in Y.

| Hole | X (mm) | Y (mm) | Ø (mm) | Position |
|---|---|---|---|---|
| H1 | 28.5 | 40.5 | 2.2 | top right |
| H2 | 2.5 | 2.5 | 2.2 | bottom left |

Bosses: outer diameter ≤ 4.5 mm, so they clear the nearby parts. At H1 a 4.5 mm
boss ends about 0.9 mm from the CON2 courtyard on the bottom side — check that
in the 3D model before printing.

## 4. USB-C edge cutout

USB1 (LCSC C2765186) is the only part on the board edge. It is centred on the
bottom edge at X = 15.5 mm, flush with Y = 0, and opens toward −Y.

| Parameter | Value |
|---|---|
| Connector centre | X 15.5 mm (board centre) |
| Courtyard | X 11.05–19.95, Y 0–7.35 |
| Shield tabs (THT) | X 11.18 / 19.82, at Y 2.62 and 6.83 |
| Locating pegs Ø0.7 | X 12.61 / 18.39, Y 6.3 |
| Mating face (typical USB-C) | approx. 8.9 × 3.2 mm, height *check* |

Wall cutout about 12.5 × 6.5 mm with rounded corners, centred on X 15.5 — larger
than the receptacle so that thicker plug overmoulds still seat fully. Its Z
centre sits at half the connector height above the top surface. The THT tabs
protrude about 1 mm on the bottom side.

## 5. Buttons and LEDs

All three buttons and all four LEDs are on the top side, so the lid needs three
plungers and four light pipes or windows. Coordinates are the centre of the
actuator or LED.

### Buttons (KMR221GLFS, 4.2 × 2.8 × 1.9 mm)

| Button | Function | X (mm) | Y (mm) | Courtyard X / Y (mm) |
|---|---|---|---|---|
| SW1 | Reset (CHIP_PU) | 3.96 | 13.20 | 2.56–5.36 / 11.1–15.3 |
| SW2 | Boot (GPIO9) | 21.13 | 17.10 | 19.73–22.53 / 15.0–19.2 |
| SW3 | Power / ship mode (BQ25186 MR) | 28.43 | 3.80 | 27.03–29.83 / 1.7–5.9 |

The actuator top is 1.9 mm above the board; travel *check* in the datasheet.
Reset and Boot are rarely needed, so a recessed Ø1.5–2 mm hole for a paperclip
is enough and keeps them from being pressed by accident.

### LEDs

| LED | Function | Package | X (mm) | Y (mm) |
|---|---|---|---|---|
| LED1 | ESP status (GPIO17) | 1.0 × 0.6 (SML-P12) | 6.1 | 3.0 |
| LED3 | 3.3 V present | 1.0 × 0.6 | 7.7 | 3.0 |
| LED2 | VBUS / charging | 1.0 × 0.6 | 9.3 | 3.0 |
| U2 | RGB LED (WL-ICLED 1312020030000) | 2.0 × 2.0 | 24.3 | 3.1 |

LED1 to LED3 are only 1.6 mm apart: give each its own light pipe or put divider
walls between them, otherwise the light bleeds across.

## 6. Connectors and cable routing

Besides USB-C there are three cable connectors: display on the top side, sensor
and battery on the bottom side. All cables stay inside the enclosure except,
depending on the concept, the sensor cable to the probe.

| Ref | Purpose | Type | Side | Courtyard X (mm) | Courtyard Y (mm) | Mating direction (estimate) |
|---|---|---|---|---|---|---|
| CON1 | E-paper display | Molex PicoBlade 532610871, 8-pin, 1.25 mm, SMD | top | 14.54–29.29 | 32.41–36.61 | cable toward +Y / display |
| CON2 | Soil probe | JST S3B-PH-SM4-TB, 3-pin, 2.0 mm, SMD | bottom | 15.48–25.38 | 34.6–42.2 | cable toward −Y (board centre) |
| U6 | LiPo battery | JST S2B-PH-K-S, 2-pin, 2.0 mm, THT | bottom | 23.15–31.0 | 26.65–32.55 | cable toward −X (board interior) |

U6 is flush with the right edge (X = 31). Its pins at (29.4 / 28.6) and
(29.4 / 30.6) protrude about 1–1.5 mm on the top side.

A PH plug needs about 8 mm of free space in front of the opening to be plugged
and unplugged. The battery itself is best placed below the board, padded and
held by a clamping rib.

## 7. Height profile

Plan at least **4 mm above the top side** and **7 mm below the bottom side**,
each plus 0.5 mm tolerance. The tallest parts are USB-C and the PicoBlade on
top, and the two JST PH connectors underneath.

| Side | Part | Height above board (mm) | Source |
|---|---|---|---|
| top | USB1 USB-C | approx. 3.2–3.5 | *check* (datasheet C2765186) |
| top | CON1 PicoBlade 8-pin | approx. 3.0–3.5 | *check* (Molex 532610871) |
| top | SW1–SW3 buttons | 1.9 | 3D model |
| top | D2 (SOD-123) | 1.4 | 3D model |
| top | L5 chip antenna | 1.3 | 3D model |
| top | U3 ESP32-C6 (QFN 5 × 5) | 0.9 | package standard |
| top | passives, SOT-23, LEDs | ≤ 1.1 | package standard |
| top | U6 pins (THT) | approx. 1–1.5 | *check* |
| bottom | CON2 JST PH SMD 3-pin | approx. 5–6 | *check* (S3B-PH-SM4-TB) |
| bottom | U6 JST PH THT 2-pin | approx. 5–6 | *check* (S2B-PH-K-S) |
| bottom | TH1 NTC 0402 | 0.35 | package standard |
| bottom | USB-C shield tabs (THT) | approx. 1 | *check* |

Total height with the battery underneath: 4 + 1.6 + 7 mm, plus battery
thickness, plus two wall thicknesses.

## 8. Antenna keepout

The keepout is the top-left corner, X 0–11.3 and Y 34.9–43.0, on all layers.
The chip antenna L5 (2450AT18A0100001E, 3.2 × 1.6 × 1.3 mm) sits at
(2.25 / 39.5). The MCU is chip-down with no shielded module, so the enclosure
around this corner decides the range (see [hardware-info.md §1.1](hardware-info.md#11-where-things-sit)):

* No metal in this area or about 5 mm beyond it — no screws, heat-set inserts,
  metal foil or battery.
* Keep the wall at this corner as thin as practical (≤ 2 mm plastic) and leave a
  few mm of air to the antenna.
* No battery under the keepout; it belongs under the right or lower half.
* No conductive filaments (carbon- or metal-filled). PETG or ASA work well
  outdoors.

## 9. Enclosure recommendations

A board pocket of 31.6 × 43.6 mm (0.3 mm clearance per side) with R2.8 corners.
Two M2 bosses at H1 and H2 carry the board, with extra supports and clamping
ribs for stability.

* **Mounting:** two diagonal screws alone let the board rock about the
  diagonal. The other two corners hold SW3 and the antenna, so use narrow
  support ribs along the board edge (≤ 0.8 mm wide) and a hold-down in the lid
  there instead of bosses.
* **USB-C:** cutout centred on X 15.5 on the bottom edge; the board must sit
  flush against this wall, with a stop directly behind the connector to take
  the insertion force.
* **Buttons:** SW3 (power) gets an easy-to-reach plunger; Reset and Boot only
  recessed holes.
* **LEDs:** light pipes Ø1–1.5 mm for LED1–3 on their 1.6 mm pitch (or one
  shared window with dividers), plus one for the RGB LED.
* **Bottom side:** at least 7 mm for CON2 and U6; battery next to or below them,
  not under the antenna.
* **Display:** the PicoBlade cable runs from CON1 (Y 32–37) up to the display —
  allow for cable length and bend radius.
* **Sensor:** the probe sits in the soil, so the cable needs a gland or seal
  (e.g. PG7 or M8) and strain relief.
* **Sealing:** cover the USB-C cutout with a rubber cap; a small
  pressure-equalisation vent prevents condensation.

## 10. Open points

Not in the KiCad file — confirm before the first print, ideally with a test
print of just the board frame.

- [ ] USB-C connector height and mating-face position above the board (C2765186)
- [ ] Height and cable exit of PicoBlade CON1 (Molex 532610871)
- [ ] Height and mating direction of JST PH CON2 and U6, bottom side
- [ ] KMR221 button travel and plunger length
- [ ] Battery size and position
- [ ] Display size and position in the lid
