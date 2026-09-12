# Product Design

What the device *does*: states, intervals, thresholds, what it stores and what
it reports.

Third of three documents, and the authority on behaviour:

| Document | Authority on | Answers |
|---|---|---|
| [hardware/README.md](hardware/README.md) | the board | which pin, which part, which voltage |
| [firmware-architecture.md](firmware-architecture.md) | the code | which component owns what, what may include what |
| this file | the device | what happens on a wake, and why |

Where this file and the hardware document disagree about a voltage or a pin, the
hardware document wins. Where it and the architecture document disagree about
behaviour, this one wins and the other gets fixed.

Status: partly implemented. `main/app/` runs the measure-and-report half of §2
- charger configuration, battery reading, the threshold branch, soil sampling,
and an MQTT publish with Home Assistant discovery - on a one-minute timer
instead of a deep-sleep cycle, so it can be watched in a terminal.

Not implemented yet, and each one changes what the numbers mean:

* **Deep sleep** (§2, §3.5). The firmware loops in a task; nothing measures the
  sleep current yet, so the budget below is still arithmetic.
* **Ship mode on a flat cell** (§3.2). The branch is taken and logged, but
  `APP_ALLOW_SHIP_MODE` in `app_run.c` is 0 - it opens the BATFET, and a
  threshold bug on the bench would shut the board down on every power-up.
* **The RTC ring buffer and bundled publishing** (§4, §6.2). Every measurement
  is published on its own, which §6.2 calls the right first implementation.
* **Calibration from NVS** (§7). Compile-time constants only, and the wet point
  is a placeholder.
* **The e-ink screen** (§2). The driver works, the application does not draw.

---

## 1. What it is

A soil-moisture sensor that lives in a plant pot on a single LiPo cell, shows
the current reading on e-paper, and reports to Home Assistant over MQTT. It is
asleep almost all of the time.

Three properties drive every decision below:

* **The cell is small.** 500 mAh, and there is no way to recharge it except
  someone noticing and plugging in USB-C. Everything is measured against what it
  costs per day.
* **E-paper holds its image with no power.** The display is only a cost when it
  changes, which makes a slow refresh rate free rather than a compromise.
* **Nothing is urgent.** Soil dries out over days. A reading that is twenty
  minutes old is as useful as one from this second, and that slack is what pays
  for the battery life.

---

## 2. The wake cycle

Deep sleep clears RAM, so there is no long-running program and there are no
persistent tasks. The application is a linear sequence that runs from the top on
every wake, and the only thing that carries across is what was deliberately put
in RTC memory (§4).

```
WAKE (RTC timer)  or  COLD BOOT (USB inserted, reset, exit from ship mode)
  │
  ├─ rails to a safe state: SOIL_POWER off, PAPER_POWER off, RGB dark
  ├─ I2C up, configure the charger  ─── every wake, not once (see §3.3)
  ├─ read VBAT through the divider, read VIN_PGOOD
  │
  ├── VBAT < 3.3 V  and  no USB ───────────► LOW BATTERY  (§3.2)
  │        ├─ e-ink: "battery empty", full refresh
  │        ├─ e-ink to sleep, panel rail down
  │        └─ I2C write: ship mode      ◄── the last instruction that executes
  │
  ├── VBAT < 3.5 V  and  USB present ──────► CHARGE WAIT  (§3.2)
  │        ├─ no measurement, no publish
  │        ├─ e-ink shows charging, only if the state changed
  │        └─ deep sleep, 5 min
  │
  └── VBAT ok ─────────────────────────────► MEASURE
           ├─ soil: rail on, settle, average, rail off
           ├─ append {timestamp, soil_mv, soil_pct, vbat_mv, charging} to the
           │  RTC ring buffer
           ├─ wake_count++
           │
           ├─ every 6th wake ─────────────► PUBLISH
           │        ├─ WiFi up
           │        ├─ SNTP  ─── every cycle, the clock drifts (§5)
           │        ├─ MQTT up; discovery on the first connect after a cold boot
           │        ├─ send the bundle; clear the buffer only on success
           │        ├─ MQTT and WiFi down
           │        └─ e-ink refresh
           │
           └─ deep sleep, 5 min
```

The order is not arbitrary. The battery is read before anything else is powered
because every other branch depends on the answer, and because a measurement
taken on a collapsing rail is worse than no measurement. The display is the last
thing before sleep because it is the only step that can be skipped without
losing data.

---

## 3. Power

### 3.1 The two thresholds

| Threshold | Where measured | What happens |
|---|---|---|
| **3.3 V** | ESP32 ADC on `BAT_MEAS` | firmware commands ship mode |
| **3.0 V** | BQ25186 internal BUVLO | the charger cuts the cell itself |

The 3.0 V hardware cutoff is a backstop that should never be reached. 3.0 V
under load is already deep discharge for a 1-cell LiPo; acting at 3.3 V leaves
the cell in a state it recovers from.

### 3.2 Hysteresis, and why it is not optional

Leaving the low-battery state happens at **3.5 V**, not 3.3 V. A cell at 3.35 V
sags below 3.3 V the moment WiFi transmits, so a single threshold produces a
device that shuts down, gets plugged in, boots, and shuts down again.

**While USB is present the device never enters ship mode.** Ship mode opens the
BATFET and the only ways back are USB or a TS/MR long press — so entering it
while charging would strand a device that is already being rescued.

Between 3.3 V and 3.5 V with USB present the device is in CHARGE WAIT: awake
briefly every 5 minutes, measuring nothing, waiting for the cell to come up.
Skipping the measurement there is deliberate — the soil rail and a WiFi
transmission are exactly what the cell cannot afford at that point.

### 3.3 The charger is configured on every wake

The BQ25186 watchdog resets all registers to their defaults after 160 s without
an I²C transaction (hardware README §4.3). A device that sleeps for 5 minutes
therefore wakes up to a charger set to 10 mA every single time.

This is not a problem to work around, it is simply how the part behaves: charger
configuration is **stateless**, rewritten on every wake, never assumed.

### 3.4 Charge current

| Setting | Value |
|---|---|
| Cell | 500 mAh, 1-cell LiPo |
| Charge rate | 0.5C |
| Charge current | **250 mA** |
| Charge voltage | 4.2 V (the BQ25186 reset default, left alone) |

0.5C is the conservative choice: it is safe for the 500 mAh cell without
depending on that cell's datasheet allowing 1C, and it stays safe if a 1000 mAh
cell is fitted instead. The capacity is the knob in `main/config.h`; the current
is derived from it, so swapping the cell is one number.

> This supersedes the 500 mA (1C) figure in hardware README §4.2, which was
> chosen before the cell was settled.

### 3.5 Budget

Rough, and to be replaced with measurements:

| Item | Per day |
|---|---|
| Deep sleep (~10 µA target, ESP32-C6 plus charger quiescent) | ~0.24 mAh |
| 288 wakes for a soil + battery measurement | small, dominated by settle time |
| 48 WiFi + MQTT cycles | **the dominant cost** |
| 48 e-ink refreshes | small on the black/white panel |

The publish cycle is what the interval choice is really about. Everything else
is noise next to bringing up a radio.

---

## 4. Where data lives

Three kinds of data with three different lifetimes, and putting them in the
wrong place is how flash gets worn out or calibration gets lost.

| What | Where | Survives | Why |
|---|---|---|---|
| Soil calibration points, divider ratio | **NVS** | everything | written a handful of times ever, must survive ship mode |
| Sample ring buffer, `wake_count`, last displayed state, frames since full refresh | **RTC RAM** | deep sleep | free to write, no flash wear, and losing it on a power cut costs nothing |
| The readings themselves, long term | **MQTT / Home Assistant** | — | the server is the archive; the device is not a logger |

**NVS is not for measurements.** It is a key-value store for configuration.
Writing a sample to it every five minutes uses it against its purpose and spends
flash endurance on data that is about to be transmitted anyway.

The ring buffer is sized at 24 slots — two hours of samples — so a broker that
is down for an hour costs nothing. On overflow the oldest sample is dropped:
recent soil moisture is worth more than old soil moisture.

---

## 5. Time

There is no external 32.768 kHz crystal — GPIO0 and GPIO1 double as the crystal
pins on the C6 and both are used as ADC inputs here. The RTC therefore runs on
an internal oscillator that drifts.

The RTC clock keeps running through deep sleep, so `time()` is continuous across
wakes. SNTP runs on **every publish cycle**, while WiFi is up anyway, which
bounds the drift to whatever accumulates in 30 minutes. Timestamps on buffered
samples are good to seconds, which is far more than the data needs.

Before the first successful SNTP sync the clock is meaningless. Samples taken in
that window are stamped, sent, and marked as unsynchronised rather than
discarded.

---

## 6. Reporting

### 6.1 MQTT Discovery

The device publishes Home Assistant discovery configuration on its first MQTT
connection after a cold boot, so the entities appear without anything being
configured by hand:

* soil moisture (%)
* battery voltage (V)
* charging state
* signal strength, as a diagnostic

### 6.2 What a bundle looks like, and the limitation

Home Assistant records an MQTT value with the **time it received it**. Timestamps
inside the payload do not change that. Six samples sent together therefore
appear in the history as six points a few seconds apart, not spread over the
half hour they were taken in.

So the payload does two things at once:

* the **most recent** sample is the entity's state — that is what Home Assistant
  graphs and what its history is built from;
* the **whole bundle** rides along as a JSON attribute, timestamps included.

Nothing is lost, the resolution of the HA graph is 30 minutes, and if true
5-minute history is wanted later the data is already in the payload — it needs a
consumer that can backfill (InfluxDB, or an HA automation writing to the
recorder), which is work on the server, not in firmware.

The alternative — publishing every 5 minutes and skipping the bundle — trades
that away for six times as many WiFi cycles. That is the wrong trade for a
device on a 500 mAh cell, but it is the right first implementation, because it
proves the whole chain with none of the buffering logic (see the build stages).

---

## 7. Calibration

The probe is capacitive: high output in air, lower in water. Two points define
the curve.

| Point | Value | Status |
|---|---|---|
| Dry, held in air | **2719 mV** | measured during bring-up |
| Wet, submerged to the working depth | 1200 mV | **placeholder, never measured** |

The placeholder exists so the percentage has a scale at all. Until the probe has
been in water at working depth, the millivolts logged next to the percentage are
the number to trust - `soil_read()` returns both for exactly that reason.

Between them the mapping is linear and clamped to 0–100 %. That is not
physically exact, but the number is a gardening aid, not an instrument reading,
and a two-point line is honest about what it is.

Calibration values are read from NVS at boot with the compiled-in defaults as a
fallback, so a device with an empty NVS still works and reports plausible
numbers.

---

## 8. Open decisions

* **How calibration gets into NVS.** A bring-up task that writes it, a
  serial command, or a long press on SW3 while the probe is in a known state.
  Until one exists, the values are compile-time constants.
* **Wet calibration point** — needs the probe in water at working depth.
* **What the display shows in CHARGE WAIT** versus a normal low battery, and
  whether the difference is worth a refresh.
* **Measurement interval under charge.** While on USB the energy argument
  disappears; measuring more often costs nothing and would make bench work far
  less tedious.
* **Battery divider leakage** (hardware README §9). It leaks into the 3.3 V rail
  whenever `BAT_MEAS_EN` is low, and in ship mode the same path is order 35 µA
  into a dead rail — which would dominate the sleep budget in §3.5. Needs
  measuring before the budget means anything.
