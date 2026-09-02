# Soil Sensor — Firmware

Battery-powered soil-moisture sensor built on the **ESP32-C6-FH4**, with an e-ink display, an addressable RGB status LED, a BQ25186 Li-ion charger (I²C), and a capacitive soil probe. Firmware targets **ESP-IDF + FreeRTOS**.

Hardware: KiCad project *Soil Sensor*, Rev 0.1.0 (Robin Bühler).

**Design principle:** power state is the top-level state machine. Every wake begins by checking the battery; measurement, display, and status LED only run when there is enough energy. When the battery is low, the device commands the charger into ship mode and powers itself off until USB is reconnected.

---

## 1. Hardware overview

| Block | Part | Interface to MCU |
|---|---|---|
| MCU | ESP32-C6-FH4 (U3) | — |
| Battery charger / power-path | BQ25186 (U5) | I²C (0x6A) + INT + CHARGE_EN + POWER_GOOD |
| 3.3 V rail | XC6220B331MR LDO (U7), fed from charger SYS | — |
| E-ink display | 8-pin FPC (CON1) | SPI (4-wire) + BUSY/RST/DC + power switch |
| RGB status LED | Würth WL-ICLED 1312020030000 (U2) | 1-wire (RMT), WS2812-compatible |
| Soil probe | 4-pin JST (CON2, S3B-PH-SM4) | ADC + power switch |
| Battery voltage sense | divider + BSS138 switch | ADC + enable |
| Buttons | BOOT (SW2), RESET | GPIO9 / EN |
| Battery | 1-cell LiPo, 500 mAh or 1000 mAh | — |

Two soft power switches (AO3401A P-FETs) gate the e-ink rail and the soil-probe rail so both can be fully powered down between measurements — important for battery life.

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
| PAPER_POWER | GPIO23 | HIGH-side load switch enable for the display rail |

### Soil probe (CON2)
| Signal | GPIO | Notes |
|---|---|---|
| SOIL_MEAS | GPIO1 | ADC input (analog) |
| SOIL_POWER | GPIO15 | load switch enable — power the probe only while sampling |

### Battery monitoring — BQ25186 (I²C)
| Signal | GPIO | Notes |
|---|---|---|
| SDA | GPIO18 | I²C data |
| SCL | GPIO19 | I²C clock |
| INT | GPIO20 | charger interrupt (input, active-low) |
| CHARGE_EN | GPIO21 | charge enable output |
| POWER_GOOD | GPIO22 | input from charger |
| BAT_MEAS | GPIO0 | ADC input (via divider) — separate channel from soil |
| BAT_MEAS_EN | GPIO16 | enables the battery divider (BSS138) — assert only when reading |

> **ADC note:** `BAT_MEAS` (GPIO0) and `SOIL_MEAS` (GPIO1) are **two separate ADC channels**. The ESP32-C6 has a single SAR ADC, so the two are converted sequentially, not truly simultaneously — but firmware just reads one then the other, so both are effectively available without powering either down for the other. The real constraint is the enables: battery reads require `BAT_MEAS_EN` (GPIO16) asserted to switch in the divider, and soil reads require `SOIL_POWER` (GPIO15) to power the probe. Both are independent and could be on together, but for power saving, assert each only around its own measurement.

### RGB status LED
| Signal | GPIO | Notes |
|---|---|---|
| RGB DIN | GPIO8 | Würth WL-ICLED — drive with the RMT peripheral (WS2812 protocol) |

### System / boot
| Signal | GPIO | Notes |
|---|---|---|
| BOOT button | GPIO9 | strapping pin: LOW at reset → download mode |
| USB D- / D+ | GPIO12 / GPIO13 | native USB-Serial-JTAG — the only console/log interface for this project |

> **UART not used.** All console/logging goes over USB-Serial-JTAG (GPIO12/13). That frees GPIO16 to serve as `BAT_MEAS_EN` with no conflict.

> **Strapping caution:** GPIO8 (RGB) and GPIO9 (BOOT) are strapping pins. GPIO8 must not be held low at reset. GPIO15 is strapping-adjacent on some C6 variants — check levels at boot for your load switches so the board always boots in SPI-flash mode.

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
| **Ship** | **off** | ~3.2 µA | valid VIN (USB) **or** TS/MR button long-press |
| Shutdown | off | ~15 nA | valid VIN or button |

**Ship mode is the "off until charged" state** you want for low battery. When the ESP32 commands ship mode over I²C, the BATFET opens and the system rail (hence the ESP32 itself) loses power. So:

> **Critical sequencing:** entering ship mode kills the ESP32 mid-operation. The low-battery routine must do everything else first — finish any measurement, update the display with a "low battery" indication, flush logs — and make the **I²C write that enters ship mode the very last instruction**. There is no code "after" it.

### 3.3 Design decision to confirm

Two ways to handle low battery:
- **(A) Ship mode** — fully powers down the board (~3.2 µA). Wakes only on USB insertion or button. Lowest possible drain; loses all RTC state.
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

### 4.3 Watchdog gotcha (matters for a sleepy device)

The charger watchdog defaults to **160 s**, and on expiry it **resets all registers to default** — meaning your 500 mA setting silently reverts to 10 mA if the ESP32 sleeps longer than 160 s without petting it. Options:

- **Recommended:** treat charger configuration as **stateless** — reconfigure the charger (0x04 = 0x4D, plus any other settings) on **every wake/boot**. Simple and robust for a deep-sleep device.
- Optionally also **disable the watchdog** (0x07, WATCHDOG_SEL = 2b11) so it never resets your config.

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
4. `soil`: assert `SOIL_POWER` (GPIO15), settle (~tens of ms), read GPIO1 ADC, average, deassert.
5. `display`: refresh e-ink with soil % + battery + charge state (full refresh only when needed to reduce ghosting).
6. `rgb`: brief status blink (skip on low battery to save power).
7. `power`: compute next interval, enter deep sleep.

Keep both load switches OFF and the RGB dark during sleep.

---

## 7. Key implementation notes

- **ADC:** ESP-IDF `adc_oneshot` + `adc_cali`. Sample after the rail settles; average several reads. Establish soil dry/wet calibration points empirically. Account for the battery divider ratio when converting `BAT_MEAS` to volts.
- **I²C:** new `i2c_master` driver. BQ25186 at 0x6A. Configure ICHG = 0x4D on every boot (watchdog note in 4.3). Watch TS/thermistor limits (schematic sets 0 °C cold / 60 °C hot).
- **SPI e-ink:** write-only; wait on BUSY between commands. Prefer partial refresh; full refresh periodically to clear ghosting.
- **RGB:** ESP-IDF `led_strip` (RMT backend). The WL-ICLED 1312020030000 uses the standard WS2812 single-wire protocol, so it's a drop-in for the old SK6812 in software. Watch: (1) byte order may be **RGB** not the WS2812-default **GRB** — set it in `led_strip` config if colors are swapped; (2) it's a ~5 V part (rated ~5.5 V) — confirm rail and that GPIO8's 3.3 V logic-high meets its data threshold. Don't drive GPIO8 low at reset.
- **Power switches:** AO3401A are P-FETs — confirm active polarity of `SOIL_POWER` / `PAPER_POWER` on the board (GPIO level ↔ rail on/off mapping).
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
- [ ] Battery divider ratio (to convert BAT_MEAS ADC → volts).
- [ ] RGB color byte order (RGB vs GRB) — verify once lit.
- [ ] RGB supply rail / logic-high threshold (~5 V part, 3.3 V data).
- [ ] Active polarity of `SOIL_POWER`, `PAPER_POWER`, `CHARGE_EN`, `BAT_MEAS_EN`.
- [ ] Exact e-ink controller (panel driver IC) → picks the display driver/lib.
- [ ] Soil probe output range (for calibration).
