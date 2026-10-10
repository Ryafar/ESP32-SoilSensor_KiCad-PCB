# Soil Sensor — Hardware, rev 0.1.0

Everything in this document describes board revision **0.1.0**. A new board
revision gets its own pass through this file, not a silent edit.

Battery-powered soil-moisture sensor built on the **ESP32-C6-FH4**, with an e-ink display, an addressable RGB status LED, a BQ25186 Li-ion charger (I²C), and a capacitive soil probe. Firmware targets **ESP-IDF + FreeRTOS**.

Hardware: KiCad project *Soil Sensor*, Rev 0.1.0 (Robin Bühler), dated
31.07.2026.

| | |
|---|---|
| Schematic | [Schematic_v0.1.0.pdf](Schematic_v0.1.0.pdf) |
| Board, top | [3D/TOP_3D_rev0.1.0.png](3D/TOP_3D_rev0.1.0.png) |
| Board, bottom | [3D/BOT_3D_rev0.1.0.png](3D/BOT_3D_rev0.1.0.png) |
| Board, angled | [3D/Side_3D_rev0.1.0.png](3D/Side_3D_rev0.1.0.png) |
| Dimensions for enclosure design | [pcb-dimensions.md](pcb-dimensions.md) |
| Dimension drawing | [3D/soil-sensor_pcb_dimensions_rev0.1.0.pdf](3D/soil-sensor_pcb_dimensions_rev0.1.0.pdf) |
| E-paper module outline (WeAct) | [3D/WeAct-EpaperModule-1.54_board-shape.pdf](3D/WeAct-EpaperModule-1.54_board-shape.pdf) |
| Board outline (DXF) | [3D/soil-sensor_pcb_outline_rev0.1.0.dxf](3D/soil-sensor_pcb_outline_rev0.1.0.dxf) |

The schematic is the authority. Where this document and the PDF disagree, the
PDF is right and this file needs fixing.

**Design principle:** power state is the top-level state machine. Every wake begins by checking the battery; measurement, display, and status LED only run when there is enough energy. When the battery is low, the device commands the charger into ship mode and powers itself off until USB is reconnected.

---

## 1. Hardware overview

| Block | Part | Interface to MCU |
|---|---|---|
| MCU | ESP32-C6-FH4 (U3), chip-down QFN with on-board antenna | — |
| Battery charger / power-path | BQ25186 (U5) | I²C (0x6A) + INT + CHARGE_EN + POWER_GOOD |
| 3.3 V rail | XC6220B331MR LDO (U7), fed from charger SYS | — |
| E-ink display | WeAct Studio 1.54" module on CON1: Molex PicoBlade 532610871, 8-pin, 1.25 mm pitch (also sold as "JST MX1.25") | SPI (4-wire) + BUSY/RST/DC + power switch |
| RGB status LED | Würth WL-ICLED 1312020030000 (U2) | 1-wire (RMT), WS2812-compatible, runs on the 3.3 V rail |
| Soil probe | JST PH, CON2, bottom side (S3B-PH-SM4 — see note below) | ADC + power switch |
| Battery voltage sense | 2 × 100 kΩ divider + BSS138 switch | ADC + enable |
| Charger thermistor | TH1, bottom side | none — wired to the charger's TS pin |
| Buttons | RESET, BOOT (GPIO9), and SW3 (ship mode) | see §1.2 |
| Battery | 1-cell LiPo, 500 mAh or 1000 mAh, on JST PH 2-pin (U6, S2B-PH-K-S) | — |
| Main crystal | 40 MHz, Murata XRCGB40M000F1S1BR0 (U1) | — |
| USB data protection | ECMF02-2AMX6 common-mode filter with ESD (U4), plus ESD9B5.0ST5G on VBUS (D1) | on the D+/D− pair |
| Status LED | SML-P12PTT86R + 1.8 kΩ (LED1, R5) | GPIO17, see §2 |
| Indicator LEDs | LED2 on VIN (USB present), LED3 on the 3.3 V rail | none — hardwired, see §2 "LED2 and LED3" |

> **CON2 pin count.** The part number `S3B-PH-SM4` is a **3-circuit** JST PH
> connector, and the bottom-side render shows three contacts. Earlier revisions
> of this document called it 4-pin. If the probe really needs four wires, the
> part number is wrong; if three is right, the probe is powered by
> `SOIL_POWER` switching its supply rather than by a dedicated enable line.
> **Check the schematic and correct whichever half is wrong.**

Two soft power switches (AO3401A P-FETs) gate the e-ink rail and the soil-probe rail so both can be fully powered down between measurements — important for battery life.

### 1.1 Where things sit

Read off the 3D renders. Everything is on the top side except where noted.

| Ref | What | Where |
|---|---|---|
| U3 | ESP32-C6-FH4, QFN | centre |
| U5 | BQ25186 charger | right edge, below the buttons |
| U7 | XC6220B331MR LDO, SOT-23-5 | left edge |
| U2 | WL-ICLED RGB status LED | bottom edge, beside SW3 |
| L5 | chip antenna, in a ground keep-out | top-left corner |
| L1–L4 | RF matching network | around U3 and the antenna |
| CON1 | e-ink cable connector, PicoBlade 8-pin | top edge |
| CON2 | soil probe connector | **bottom side** |
| TH1 | thermistor | **bottom side**, centre |
| USB-C | power and USB-Serial-JTAG | bottom edge |
| U1 | 40 MHz crystal | beside U3 |
| U4 | USB common-mode filter | by the USB-C connector |
| U6 | battery JST PH 2-pin | **bottom side** |
| LED1–LED3 | status LED and two hardwired indicators | bottom edge, left of USB-C |

The MCU is a bare chip with an on-board antenna, not a shielded module. That
matters for anything that touches the RF path — keep-out areas, enclosure
material and how the board is mounted in the pot all affect range, and none of
it is adjustable in firmware.

### 1.2 Three buttons, not two

The renders show three switches, silkscreened `R`, `B` and `SW3`:

| Button | Function |
|---|---|
| `R` | RESET, straight to the MCU's EN pin |
| `B` | BOOT, GPIO9 — hold low at reset for download mode |
| `SW3` | ship mode — the BQ25186's TS/MR button; enters and leaves ship mode |

`SW3` matters more than its designator suggests. It goes to the charger, not
to the MCU, and ship mode (§3.2) wakes on "valid VIN **or** TS/MR button
long-press" — so `SW3` is the only way to bring the board back from ship mode
without plugging in USB, and the only way to put an unprogrammed or hung board
into it. Firmware never sees the button directly.

---

## 2. Pin map

Read directly from the schematic. **Verify against your own board before flashing.**

### E-ink display — SPI (CON1)
| Signal | GPIO | Notes |
|---|---|---|
| BUSY | GPIO2 | input |
| RST  | GPIO3 | output |
| D/C  | GPIO4 | output |
| CS   | GPIO5 | output |
| CLK  | GPIO6 | SPI SCLK |
| SDO (MOSI) | GPIO7 | SPI MOSI (display is write-only) |
| PAPER_POWER | GPIO23 | display rail load switch, **active low** — drive to GND to power the panel |

**Panel:** WeAct Studio 1.54" e-paper module, connected to CON1 (Molex
PicoBlade, 8-pin, 1.25 mm pitch — the same connector is sold as "JST MX1.25")
by an 8-wire cable. Two variants exist and
the firmware supports both. Which one is fitted is one `#define` in
`components/eink/include/eink_panel.h` — set one of the two to 1 and rebuild:

| Variant | Panel | Controller | Resolution | Refresh |
|---|---|---|---|---|
| Black/white | GDEH0154D67 | SSD1681 | 200×200, 1 bpp | ~2.5 s full, ~0.5 s partial |
| Four colour (**fitted**) | GDEM0154F51H | JD79660 | 200×200, 2 bpp | ~21 s, no partial mode |

The fitted module is the four-colour one — black, white, red and yellow. Two
consequences worth carrying into the firmware: there is **no partial refresh**,
so every update is the full ~21 s with the rail up, and red and yellow are
available as real colours rather than as emphasis that has to be faked with
black.

Sources: [WeActStudio.EpaperModule](https://github.com/WeActStudio/WeActStudio.EpaperModule)
(vendor reference code under `Doc/1.54 Inch Black&Write/`, controller names in
`Example/EpaperModuleTest_Arduino_ESP32/`) and GxEPD2's `GxEPD2_154_D67` /
`GxEPD2_154c_GDEM0154F51H`.

Two consequences worth carrying into the power budget:

- The module's 3/4-wire select (`BS`) is tied on the module PCB, so CON1 needs
  no extra line and the interface is 4-wire SPI as assumed.
- Both controllers sit at ~1 µA in deep sleep and e-paper holds its image with
  no power at all, so `PAPER_POWER` is insurance rather than a necessity. When
  it is cut, every SPI and control line must be driven low first — otherwise
  the ESP32 back-feeds the module through its ESD diodes and the rail never
  actually reaches 0 V. `epd_bus_power_off()` does this.
- A four-colour refresh takes ~21 s **per wake**. At the 30-minute measurement
  interval that is a different energy budget from the 2.5 s black/white panel,
  and it is the reason the panel choice is a build-time decision.

### Soil probe (CON2)
| Signal | GPIO | Notes |
|---|---|---|
| SOIL_MEAS | GPIO1 | ADC input (analog) |
| SOIL_POWER | GPIO14 | probe rail load switch, **active low** — drive to GND to power the probe |

### Battery monitoring — BQ25186 (I²C)
| Signal | GPIO | Notes |
|---|---|---|
| SDA | GPIO18 | I²C data, 10 kΩ pull-up fitted |
| SCL | GPIO19 | I²C clock, 10 kΩ pull-up fitted |
| INT | GPIO20 | charger interrupt, input, active low, 10 kΩ pull-up fitted |
| CHARGE_EN | GPIO21 | charger `/CE`, **active low**, 10 kΩ pull-up fitted — the pull-up disables charging, see §4.2a |
| POWER_GOOD | GPIO22 | input from charger, 10 kΩ pull-up fitted |
| BAT_MEAS | GPIO0 | ADC input, 1:2 divider (2 × 100 kΩ) with 100 nF to GND |
| BAT_MEAS_EN | GPIO16 | battery divider enable (BSS138), **active high**, pull-down fitted — assert only when reading |

> **ADC note:** `BAT_MEAS` (GPIO0) and `SOIL_MEAS` (GPIO1) are **two separate ADC channels**. The ESP32-C6 has a single SAR ADC, so the two are converted sequentially, not truly simultaneously — but firmware just reads one then the other, so both are effectively available without powering either down for the other. The real constraint is the enables: battery reads require `BAT_MEAS_EN` (GPIO16) asserted to switch in the divider, and soil reads require `SOIL_POWER` (GPIO14) to power the probe. Both are independent and could be on together, but for power saving, assert each only around its own measurement.

### RGB status LED
| Signal | GPIO | Notes |
|---|---|---|
| RGB DIN | GPIO8 | Würth WL-ICLED — drive with the RMT peripheral (WS2812 protocol) |

### LED1 — plain status LED
| Signal | GPIO | Notes |
|---|---|---|
| LED1 | GPIO17 | SML-P12PTT86R behind R5 = 1.8 kΩ, lit by driving the pin **high** (~0.8 mA). Unused by firmware; driven low at boot |

Two things make this pin worth treating carefully rather than as a spare
indicator:

* **A steady LED is unaffordable here.** An SML-P12PTT86R behind R5 = 1.8 kΩ
  draws roughly 0.8 mA at 3.3 V, which is about 19 mAh per day out of a 500 mAh
  cell — nearly two orders of magnitude above the ~10 µA sleep target, and it
  would flatten the cell in under four weeks on its own. Whatever it is used
  for has to be momentary.
* **GPIO17 is UART0 RX by default**, which is the second reason the UART console
  must stay off (the first is GPIO16 = `BAT_MEAS_EN`). An input pull-up on this
  net leaks roughly 70 µA through the LED, which is seven times the sleep
  budget, and may show as a faint glow.

Out of reset and in deep sleep the pad is high-impedance and the LED is dark,
which is the right default; firmware drives it low so that "off" is a level
rather than an assumption about what the ROM left behind.

### LED2 and LED3 — hardwired indicators

Neither is connected to the MCU, so firmware cannot switch them.

| LED | Lit when | Cost on battery |
|---|---|---|
| LED2 | VIN present, i.e. USB plugged in | none — only lit while USB supplies the board |
| LED3 | the 3.3 V rail is up | **always**, deep sleep included |

**LED3 has to be desoldered on a board that runs from the cell.** Deep sleep
only stops the MCU; the 3.3 V rail stays up, so LED3 burns its current around
the clock. At the ~0.8 mA LED1 would draw (see above) that is about 80× the
~10 µA sleep target and flattens a 500 mAh cell in under four weeks — the deep
sleep design is pointless while it is fitted. In ship mode the rail is gone and
LED3 is dark, so it does not affect that number. On a bench board powered from
USB it is harmless and useful.

### System / boot
| Signal | GPIO | Notes |
|---|---|---|
| BOOT button (`B`) | GPIO9 | strapping pin: LOW at reset → download mode |
| RESET button (`R`) | — | wired to EN, not visible to firmware |
| `SW3` | — | charger TS/MR, ship mode on/off, see §1.2 — not an MCU pin |
| USB D- / D+ | GPIO12 / GPIO13 | native USB-Serial-JTAG — the only console/log interface for this project |

> **UART not used.** All console/logging goes over USB-Serial-JTAG (GPIO12/13). That frees GPIO16 to serve as `BAT_MEAS_EN` with no conflict.

> **Strapping caution:** GPIO8 (RGB) and GPIO9 (BOOT) are strapping pins. GPIO8 must not be held low at reset. An earlier revision of this document put `SOIL_POWER` on GPIO15 and warned about it for that reason; the schematic says **GPIO14**, and GPIO15 now carries no signal at all.

---

## 3. Power management — the core of the design

The BQ25186 controls whether the board has power at all. The ESP32 supervises the battery over I²C + ADC and decides when to keep running, when to sleep, and when to shut the whole board down.

### 3.1 Two-threshold battery strategy

| Threshold | Where measured | Action |
|---|---|---|
| **~3.3 V** (soft, chosen) | ESP32 ADC on `BAT_MEAS` | Proactively command BQ25186 into **ship mode** → board powers down cleanly, well before the cell is damaged. |
| **3.0 V** (hard, BUVLO) | BQ25186 internal | Chip's own cutoff. Safety net you should never actually reach. |

Acting at 3.3 V protects the LiPo: 3.0 V under load is already deep discharge on a 1-cell LiPo. The 3.0 V hardware BUVLO is only a backstop.

### 3.2 Charger power modes (BQ25186)

| Mode | BATFET (system rail) | Quiescent | Wakes on |
|---|---|---|---|
| Charger/Adapter | on | — | (USB present) |
| Battery | on | ~4 µA | — |
| **Ship** | **off** | ~3.2 µA | valid VIN (USB) **or** TS/MR button (`SW3`) long-press |
| Shutdown | off | ~15 nA | valid VIN or button |

**Ship mode is the "off until charged" state** you want for low battery. When the ESP32 commands ship mode over I²C, the BATFET opens and the system rail (hence the ESP32 itself) loses power. So:

> **Critical sequencing:** entering ship mode kills the ESP32 mid-operation. The low-battery routine must do everything else first — finish any measurement, update the display with a "low battery" indication, flush logs — and make the **I²C write that enters ship mode the very last instruction**. There is no code "after" it.

### 3.3 Design decision to confirm

Two ways to handle low battery:
- **(A) Ship mode** — fully powers down the board (~3.2 µA). Wakes only on USB insertion or a long press on `SW3`. Lowest possible drain; loses all RTC state.
- **(B) ESP32 deep sleep** — keeps the RTC alive, wakes on its own timer. Higher drain than ship mode, but the device can re-check the battery periodically and self-recover without USB.

For "off until USB is reconnected," **(A) ship mode** matches your intent. Consider (B) only if you want the device to auto-recover after the battery relaxes/recovers slightly without user intervention.

---

## 4. BQ25186 configuration

### 4.1 Register defaults at power-on reset

The chip boots with these values **before** the ESP32 writes anything:

| Setting | Reg | Reset | Meaning |
|---|---|---|---|
| Charge voltage (VBATREG) | 0x03 = 0x46 | **4.2 V** | Standard LiPo full charge. ✅ leave as-is |
| Charge current (ICHG) | 0x04 = 0x05 | **10 mA** | ⚠️ trickle only — must be raised (see below) |
| Battery UVLO (BUVLO) | 0x06 | **3.0 V** | Hard cutoff, 150 mV hysteresis |
| Termination current | 0x05 | 10% of ICHG | Charge stops when current tapers to 10% |
| Precharge threshold (VLOWV) | 0x07 | 3.0 V | Below this: trickle charge |
| Safety timer | 0x07 | 6 h | Fast-charge timeout |
| Watchdog | 0x07 = 0x84 | **160 s, resets regs to default** | ⚠️ see 4.3 |
| Thermal regulation | 0x05 | 100 °C | |
| Discharge OCP | 0x06 | 3000 mA | |

### 4.1a Measured power-on values

Read off a known-good board over I²C at boot, USB connected, no cell attached,
before firmware writes anything. Useful as a baseline: a board that dumps
something else either has been configured already or is not healthy.

| Reg | Name | Value |
|---|---|---|
| 0x00 | STAT0 | 0x61 |
| 0x01 | STAT1 | 0x40 |
| 0x02 | FLAG0 | 0x00 |
| 0x03 | VBAT_CTRL | **0x46** — matches the documented 4.2 V reset |
| 0x04 | ICHG_CTRL | **0x05** — 10 mA, i.e. never configured |
| 0x05 | CHARGECTRL0 | 0x20 |
| 0x06 | CHARGECTRL1 | 0xD6 |
| 0x07 | IC_CTRL | **0x84** — confirms the watchdog register and its reset value |
| 0x08 | TMR_ILIM | 0x4D |
| 0x09 | SHIP_RST | 0x11 — EN_RST_SHIP bits [6:5] are 00, "do nothing" |
| 0x0A | SYS_REG | 0x40 |
| 0x0B | TS_CONTROL | 0x00 |
| 0x0C | MASK_ID | 0x41 |

Decoded against §4.4, the two status bytes describe exactly the situation the
dump was taken in: `STAT0 = 0x61` is `CHG_STAT = 2b11` (charge done or disabled)
with `VIN_PGOOD = 1`, and `STAT1 = 0x40` is `BUVLO_STAT = 1` — battery
undervoltage, which is what "no cell attached" looks like. Nothing here is a
fault.

**Four registers disagree with the datasheet, and the board is right.** For
0x05, 0x06, 0x0A and 0x0C the datasheet's `[Reset = ...]` header line and its
bit-field figure contradict its own field-description table; the measured value
matches the field table every time:

| Reg | Datasheet header | Measured | Which field disagrees |
|---|---|---|---|
| 0x05 | 0x24 | 0x20 | `VINDPM_1:0` — table says `2b00` (VBAT+300 mV), header/figure say `2b01` |
| 0x06 | 0x56 | 0xD6 | `IBAT_OCP_1:0` — table says `2b11` (3000 mA), header/figure say `2b01` |
| 0x0A | 0x42 | 0x40 | `WATCHDOG_15S_ENABLE` — table says `1b0`, header says set |
| 0x0C | 0x40 | 0x41 | `Device_ID` — table says `4b0001`, figure says `4b0000` |

So when bring-up says "compare against the datasheet reset values", it means the
field-description tables, or better, this measured baseline. Comparing against
the header lines flags four healthy registers as suspicious.

`MASK_ID` low nibble = `0x1` is a usable identity check: an ACK at 0x6A only
proves something is on the bus, the device ID proves it is a BQ25186.

### 4.2 Charge current for this project

Standard 1-cell LiPo charge rate is **0.5C–1C**:

| Cell | 0.5C | 1C |
|---|---|---|
| 500 mAh | 250 mA | 500 mA |
| 1000 mAh | 500 mA | 1000 mA |

**Chosen default: 500 mA** — this is 1C for the 500 mAh cell and 0.5C for the 1000 mAh cell, so it's safe for either without changing firmware based on which cell is populated. (Confirm your specific cell's datasheet allows 1C if using the 500 mAh.)

To set 500 mA, from the datasheet formula for ICHG > 35 mA:
`ICHG = 40 + (CODE − 31) × 10` mA → CODE = 31 + (500 − 40)/10 = **77 = 0x4D**.

> **Write `0x4D` to register `0x04`** (CHG_DIS = 0, ICHG = 500 mA). The 10 mA default must be overwritten or the battery will never charge in reasonable time.

### 4.2a `/CE` and the pull-up that disabled charging

Datasheet, pin description:

> Charge Enable Pin. Drive `/CE` low or leave disconnected to enable charging
> when VIN is valid. Drive `/CE` high to disable charge. Has an internal
> pulldown resistor of around 5 MΩ. This pin has no effect when VIN is not
> present.

So the chip's own default — pin left alone, held down by 5 MΩ — is *charging
enabled*. **This board fits a 10 kΩ pull-up on that net, which beats the
internal pulldown by three orders of magnitude, so `/CE` sits high and charging
is disabled** until firmware drives GPIO21 low. A board with no firmware, or
firmware that ignores the pin, never charges its cell.

That also explains an otherwise ambiguous symptom: `CHG_STAT = 2b11` reads
"charge done **or** charging disabled" (§4.4), so a full-looking cell and a
never-started charge are the same two bits. On this board it was the second.

**Firmware rule:** drive `/CE` low exactly while the charger reports
`VIN_PGOOD`, high otherwise. Both halves matter:

* **low** is what makes charging happen at all here;
* **high** is what stops the pull-up from drawing V(SYS)/10 kΩ ≈ **330 µA** out
  of the cell. That is 33× the ~10 µA deep-sleep target, so leaving it low on
  battery would dominate the entire power budget — and `/CE` does nothing
  without VIN anyway, so there is no reason to.

Deep sleep can hold the level: the ESP32-C6 supports per-IO hold through
deep sleep (`SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP`, via `gpio_hold_en()` +
`gpio_deep_sleep_hold_en()`), unlike the ESP32/S2/C3/S3/C2. Without that,
charging would only ever run during the seconds the board is awake. One
consequence to accept: if USB is unplugged *during* sleep with the level held
low, the 330 µA flows until the next wake notices and releases it — bounded by
one sleep interval.

**Recommendation for the next board revision:** drop the pull-up, or make it a
pull-down. The part's own default is the state you want, the pin then needs no
firmware at all, and the 330 µA disappears.

### 4.3 Watchdog gotcha (matters for a sleepy device)

The charger watchdog defaults to **160 s**, and on expiry it **resets all registers to default** — meaning your 500 mA setting silently reverts to 10 mA if the ESP32 sleeps longer than 160 s without petting it. Options:

- **Recommended:** treat charger configuration as **stateless** — reconfigure the charger (0x04 = 0x4D, plus any other settings) on **every wake/boot**. Simple and robust for a deep-sleep device.
- Optionally also **disable the watchdog** (0x07, WATCHDOG_SEL = 2b11) so it never resets your config.

### 4.4 Status registers — every state the charger can report

Datasheet §6.5.1.1–6.5.1.3. These three registers are the entire observable
state of the charger. `STAT0` and `STAT1` are level ("what is true right now");
`FLAG0` is latched ("what happened since you last looked"). All three are
read-only to the host, but **`FLAG0` and the bottom three bits of `STAT1` clear
on read** — see "Reading is not free" below.

#### STAT0 (0x00) — charging state

Every bit is type `R`, reset value undefined (it reflects hardware, not a default).

Bits 6:5 are `CHG_STAT_1:0`, the four charging states:

| `CHG_STAT` | State |
|---|---|
| `2b00` | Not charging, *while charging is enabled* — no valid input, or charging suspended |
| `2b01` | Constant current — trickle, pre-charge **or** fast charge, not distinguished |
| `2b10` | Constant voltage |
| `2b11` | Charge done **or** charging disabled by the host |

The remaining bits are independent condition flags, `1` = active:

| Bit | Field | Meaning when set |
|---|---|---|
| 7 | `TS_OPEN_STAT` | TS/MR pin is open, **or** VBAT < V(BAT_HALT). Clamp is active |
| 4 | `ILIM_ACTIVE_STAT` | Input current limit is clamping |
| 3 | `VDPPM_ACTIVE_STAT` | DPPM active — the SYS load is taking current away from charging |
| 2 | `VINDPM_ACTIVE_STAT` | VINDPM active — input sagging, charge current pulled back |
| 1 | `THERMREG_ACTIVE_STAT` | Thermal regulation is reducing charge current |
| 0 | `VIN_PGOOD_STAT` | VIN is good (above UVLO, above VBAT+V(SLEEP), below OVP) |

Two ambiguities to know before writing a decoder:

* `2b11` does not say whether the pack is full or whether *we* turned charging
  off. Read `CHG_DIS` (`ICHG_CTRL` 0x04 bit 7) to separate the two.
* `2b01` covers trickle, pre-charge and fast charge alike. If that distinction
  matters it has to come from the battery voltage, not from this register.

#### STAT1 (0x01) — input, battery and thermistor state

| Bit | Field | Type | Meaning when set |
|---|---|---|---|
| 7 | `VIN_OVP_STAT` | R | Input overvoltage protection active |
| 6 | `BUVLO_STAT` | R | Battery undervoltage lockout active — **also what an absent cell looks like** |
| 5 | reserved | R | — |
| 2 | `SAFETY_TMR_FAULT_FLAG` | RC | Safety timer expired. Only clears after CE is toggled |
| 1 | `WAKE1_FLAG` | RC | TS/MR short press, Wake 1 duration met |
| 0 | `WAKE2_FLAG` | RC | TS/MR press, Wake 2 duration met |

Bits 4:3 are `TS_STAT_1:0`, the thermistor (JEITA) state — the four states are
what TH1 buys us:

| `TS_STAT` | State |
|---|---|
| `2b00` | Normal |
| `2b01` | V(TS) < V(HOT) or V(TS) > V(COLD) — **charging suspended** |
| `2b10` | Cool: V(COOL) < V(TS) < V(COLD) — charge *current* reduced per `TS_CONTROL` |
| `2b11` | Warm: V(WARM) > V(TS) > V(HOT) — charge *voltage* reduced per `TS_CONTROL` |

`TS_STAT` only means anything while `TS_EN` (`IC_CTRL` 0x07 bit 7) is set, which
it is by default.

#### FLAG0 (0x02) — latched events, every bit `RC`

Set when the condition occurs, cleared by reading the register. This is the only
place where a condition that came and went between two polls is visible.

| Bit | Field | Latches |
|---|---|---|
| 7 | `TS_FAULT` | TS fault detected |
| 6 | `ILIM_ACTIVE_FLAG` | Input current limit became active |
| 5 | `VDPPM_ACTIVE_FLAG` | DPPM became active |
| 4 | `VINDPM_ACTIVE_FLAG` | VINDPM became active |
| 3 | `THERMREG_ACTIVE_FLAG` | Thermal regulation occurred |
| 2 | `VIN_OVP_FAULT_FLAG` | Input overvoltage fault |
| 1 | `BUVLO_FAULT_FLAG` | Battery undervoltage fault |
| 0 | `BAT_OCP_FAULT` | Battery overcurrent fault |

The datasheet calls bits 6:3 "faults". They are not faults, they are regulation
loops doing their job — `VINDPM_ACTIVE_FLAG` on a weak USB supply is normal.
Only bits 7, 2, 1, 0 are worth surfacing to a user.

#### Reading is not free

Two side effects, both easy to miss because the API is called *read*:

1. **Any I²C transaction pets the watchdog.** With `WATCHDOG_SEL = 2b00`
   (the reset value, see §4.3) the first transaction after power-on *starts* the
   160 s timer, and every read afterwards restarts it. A firmware that polls
   status faster than 160 s will never see the register reset that §4.3 warns
   about — which also means polling cannot be used as evidence that the watchdog
   is harmless.
2. **`FLAG0` and `STAT1[2:0]` clear on read.** Whoever reads them first consumes
   the event; a second reader sees zeros. There must be exactly one place in the
   firmware that reads them, and it has to log or forward what it saw.

#### Interrupts, for later

`/INT` (GPIO20) pulses low for **128 µs** when an unmasked condition occurs —
far too short to poll for. It needs an edge-triggered GPIO interrupt; the ISR
then does nothing but notify the task that reads `FLAG0` (see
[firmware-architecture.md](../firmware-architecture.md) §1). Masking lives in
`CHARGECTRL1` (0x06) bits 2:0 and `MASK_ID` (0x0C) bits 7:4. On this board at
reset that leaves PG, battery OCP/BUVLO, TS and VINDPM/DPPM able to interrupt,
with charge-status, ILIM and thermal regulation masked.

---

## 5. Firmware architecture

A top-level **state machine** driven by power state, with helper modules underneath. This is not four co-equal tasks — measurement/display/status all live inside the "battery OK" branch.

```
        ┌─────────────┐
        │    BOOT     │  init I2C, configure charger (0x04=0x4D),
        │             │  read charger status
        └──────┬──────┘
               │
        ┌──────▼──────┐
        │ CHECK POWER │  read BAT_MEAS (GPIO0) via BAT_MEAS_EN
        └───┬──────┬──┘
   VBAT OK  │      │  VBAT < 3.3V and no USB
        ┌───▼───┐  └────────────┐
        │ ACTIVE│         ┌──────▼──────┐
        │       │         │  LOW-BATT   │  display "low batt",
        │ soil  │         │             │  flush logs, then LAST
        │ batt  │         │  → I2C: BQ  │  I2C write = ship mode
        │ disp  │         │    ship mode│──────► board powers off
        │ rgb   │         └─────────────┘        until USB inserted
        └───┬───┘
            │ done
        ┌───▼─────────┐
        │ DEEP SLEEP  │  timer wake → back to CHECK POWER
        │ (timed)     │  (both load switches off, RGB dark)
        └─────────────┘
```

### Suggested modules (`components/`)
- **`bsp/`** — pin definitions (`board.h`), rail/power-switch helpers, ADC channel setup.
- **`charger/`** — BQ25186 driver: configure (ICHG, watchdog), read status/faults, enter ship mode, handle INT.
- **`battery/`** — assert `BAT_MEAS_EN`, sample GPIO0, convert to voltage/SoC, deassert. Apply the 3.3 V threshold decision.
- **`soil/`** — assert `SOIL_POWER`, settle, sample GPIO1, average, convert to % moisture (calibration curve), deassert.
- **`display/`** — e-ink init, framebuffer, partial/full refresh, render soil % + battery + charge state.
- **`rgb/`** — WL-ICLED status colors (charging, low battery, measuring, fault).
- **`power/`** — deep-sleep scheduling, wake sources, ship-mode entry sequencing.
- **`app/`** — the state machine + shared state.

---

## 6. Measurement cycle (ACTIVE state)

1. (Already configured charger at boot.)
2. `charger`: read BQ25186 status over I²C (charging? fault? POWER_GOOD?).
3. `battery`: assert `BAT_MEAS_EN` (GPIO16), read GPIO0 ADC, compute voltage, deassert. **If < 3.3 V and no USB → go to LOW-BATT.**
4. `soil`: assert `SOIL_POWER` (GPIO14), settle (see `SOIL_SETTLE_MS`; measured by `soil_test_task`, not guessed), read GPIO1 ADC, average, deassert.
5. `display`: refresh e-ink with soil % + battery + charge state (full refresh only when needed to reduce ghosting).
6. `rgb`: brief status blink (skip on low battery to save power).
7. `power`: compute next interval, enter deep sleep.

Keep both load switches OFF and the RGB dark during sleep.

---

## 7. Key implementation notes

- **ADC:** ESP-IDF `adc_oneshot` + `adc_cali`. Sample after the rail settles; average several reads. Establish soil dry/wet calibration points empirically.
- **Battery divider:** 2 × 100 kΩ, so `VBAT = 2 × ADC`. At a full 4.2 V cell the ADC sees 2.1 V, which fits `ADC_ATTEN_DB_12` (usable to ~2.9 V) with headroom. The 100 nF to GND buffers the ADC's sample-and-hold charge injection, so the 100 kΩ source impedance is not a problem — but the cap needs time to recharge after `BAT_MEAS_EN` is asserted, so settle before the first conversion and discard it. The exact factor is a constant in `main/config.h`, not a literal `* 2`: resistor tolerance means the real ratio is not exactly 2, and calibrating against a multimeter should touch one number.
- **I²C:** new `i2c_master` driver. BQ25186 at 0x6A. Configure ICHG = 0x4D on every boot (watchdog note in 4.3). Watch TS/thermistor limits (schematic sets 0 °C cold / 60 °C hot).
- **I²C pull-ups:** 10 kΩ on both lines, on the board. Comfortable at 100 kHz; on the weak side for 400 kHz, where the rise time starts to depend on how much capacitance the traces add. Bring up at 100 kHz and only raise the clock once the bus is proven. The ESP32's internal pull-ups (~45 kΩ) may stay enabled — in parallel they help rather than hurt — but the board does not depend on them.
- **SPI e-ink:** write-only; wait on BUSY between commands. Prefer partial refresh; full refresh periodically to clear ghosting.
- **RGB:** ESP-IDF `led_strip` (RMT backend), wrapped by `components/rgb/`. The WL-ICLED 1312020030000 is listed by Würth as *single wire, 24 bit* and uses the standard WS2812 protocol, so it's a drop-in for the old SK6812 in software. It runs on the **same 3.3 V rail as the ESP32**, so GPIO8's logic level needs no shifter — ignore the 5 V / 5.5 V figures some distributor listings quote. Remaining unknown: byte order may be **RGB** instead of the WS2812-default **GRB**; set `color_component_format` in the `led_strip` config if red and green come out swapped. Don't drive GPIO8 low at reset (strapping pin).
- **Power switches:** AO3401A are P-FETs, so both rails are **active low** — `SOIL_POWER` and `PAPER_POWER` conduct when the GPIO is driven to GND. **Each gate carries a 100 kΩ resistor to its source**, which is what actually defines the rails: with the MCU's pad floating — at reset, or in deep sleep, where digital pads go high-impedance — the gate sits at the source and the FET is off. Firmware still drives both pins to their off level early, but that is belt and braces rather than the thing keeping the probe unpowered. Without those resistors, every deep sleep would leave two gates floating and the sleep current would be anyone's guess.
- **Pull-ups and pull-downs on the charger pins:** `INT`, `POWER_GOOD` and `CHARGE_EN` each carry a 10 kΩ pull-up, so the ESP32's internal pull-ups are not needed on any of them. `BAT_MEAS_EN` has a pull-down instead, which is the right default: the divider stays disconnected until firmware actively asks for a reading, including while the MCU is asleep or in reset.
- **Ship mode sequencing:** the I²C write that enters ship mode is the last thing the ESP32 does — nothing executes after it (see 3.2).
- **Console:** UART unused; route all logging over **USB-Serial-JTAG** (GPIO12/13). Set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` in menuconfig.
- **Deep sleep:** disable unused peripherals; only RTC timer stays up. Target the wake interval to your battery budget.

---

## 8. Build & flash

```bash
idf.py set-target esp32c6
idf.py menuconfig      # set flash size; Channel = USB Serial/JTAG for console
idf.py build
idf.py -p <PORT> flash monitor
```

To enter download mode manually: hold **BOOT** (GPIO9) low, tap **RESET**, release BOOT.

---

## 9. Open items to confirm on hardware

- [x] ADC channels: BAT_MEAS = GPIO0, SOIL_MEAS = GPIO1 — two separate channels.
- [x] Console: USB-Serial-JTAG only; UART unused.
- [x] RGB: Würth WL-ICLED 1312020030000, WS2812-compatible.
- [x] Charge current: 500 mA (0x4D) chosen — safe for both 500/1000 mAh cells.
- [ ] Low-battery behavior: ship mode (A) vs ESP32 deep sleep (B) — see 3.3.
- [ ] Confirm 500 mAh cell datasheet allows 1C (500 mA) charge.
- [x] Battery divider: 2 × 100 kΩ + 100 nF, ratio 1:2 → `ADC_ATTEN_DB_12`.
- [ ] **Battery divider leaks into the 3.3 V rail when `BAT_MEAS_EN` is low.**
  The BSS138 switches the *low* leg, so with the divider off GPIO0 is still tied
  to VBAT through the top 100 kΩ and sits above the rail — bring-up measures
  ~3.6 V there, i.e. GPIO0's ESD clamp to VDD is conducting. Running, that is
  ~4–5 µA. In ship mode, with the 3.3 V rail collapsed, the same path is order
  35 µA into a dead rail, well above the BQ25186's own ~1–2 µA ship-mode
  current. Confirm against the schematic and measure; if real, the fix is a
  switch in the *high* leg (or a series element) so the cell is fully isolated
  from GPIO0.
- [x] RGB supply rail: 3.3 V, shared with the ESP32 — no level shifter needed.
- [ ] RGB color byte order (RGB vs GRB) — verify once lit.
- [x] Active polarity: `SOIL_POWER` and `PAPER_POWER` are **active low** (P-FET gates to GND), each gate biased off by a 100 kΩ resistor to its source, so both rails are defined with the MCU asleep or unprogrammed; `BAT_MEAS_EN` is **active high** with a pull-down.
- [x] Charger pin biasing: 10 kΩ pull-ups on `INT`, `POWER_GOOD`, `CHARGE_EN`; pull-down on `BAT_MEAS_EN`.
- [ ] **`CHARGE_EN` pull-up is wrong for this part** (§4.2a). `/CE` is active low with an internal 5 MΩ pulldown, so the 10 kΩ pull-up disables charging by default and costs 330 µA whenever firmware holds it low. Works around it in firmware today; the fix is a pull-down, or no resistor, in the next revision.
- [x] Exact e-ink controller: the fitted module is the **four-colour** WeAct 1.54" (black/white/red/yellow), panel GDEM0154F51H, controller **JD79660**, 200×200, 2 bpp, ~21 s per refresh and no partial mode. The black/white variant (GDEH0154D67, **SSD1681**) is the other supported option — both drivers exist, see §2 "E-ink display".
- [ ] Soil probe output range (for calibration).
- [x] CON2 is **three** contacts: the schematic says `S3B-PH-SM4-TB`, a 3-circuit JST PH, top-entry SMD. The probe is powered by `SOIL_POWER` switching its supply, not by a dedicated enable line.
- [x] `SW3` is the charger's TS/MR button: it enters and leaves ship mode, so ship mode can be exited without USB.
- [ ] **LED3 sits on the 3.3 V rail and is lit in deep sleep** (§2 "LED2 and LED3"). Desoldered on battery-powered boards for now; the fix in the next revision is to drop it, or move it onto a GPIO like LED1.
- [x] CON1 is a Molex PicoBlade 532610871 (8-pin, 1.25 mm, "JST MX1.25") cable connector, not an FPC.
- [x] `U1`, `U4`, `U6` identified from the schematic: `U1` is the **40 MHz main crystal** (Murata XRCGB40M000F1S1BR0) for the C6's oscillator — not to be confused with the 32.768 kHz RTC crystal, which this board deliberately does not have (§5 of the product design). `U4` is an **ECMF02-2AMX6** common-mode filter with integrated ESD protection on the USB D+/D− pair, with `D1` (ESD9B5.0ST5G) on VBUS. `U6` is the **battery connector**, JST PH 2-circuit `S2B-PH-K-S` — now in the §1 table.
- [x] `LED1` is a plain status LED on **GPIO17**, lit by driving the pin high. See §2 "LED1".
- [ ] The four ringed pads on the bottom side: test points, and for what?
