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
an MQTT publish with Home Assistant discovery, and the e-paper screen - on a
one-minute timer instead of a deep-sleep cycle, so it can be watched in a
terminal.

The screen is redrawn on a **reporting cycle only, and only when the picture
would actually differ** - two gates, both load-bearing:

* A refresh costs 2.5 s on the black/white panel and about 21 s on the
  four-colour one, with the rail up throughout: order 0.1 mAh a time. At one
  per wake that is roughly 29 mAh a day, more than everything else on the board
  combined. So the panel shares the radio's cadence: every 6th measurement.
* The change test alone would not be enough anyway. One percentage point is
  15 mV on this scale, inside the probe's own noise, so the displayed value
  jitters and every jitter would look like a change. `SOIL_DISPLAY_STEP_PERCENT`
  requires two points of movement - unless a colour threshold is crossed, or the
  reading appears or disappears, which redraw immediately.

The panel rail goes down after every refresh - e-paper holds its image with no
power - and the comparison state lives in RTC memory, so both gates keep working
across sleep cycles. Without that state, every wake would redraw the same
picture.

The low-battery screen is the exception and ignores both gates: it goes up
immediately, because the cycle it appears on is the last one before ship mode.

Three things survive a cycle, all in RTC memory: the measurement counter, the
last drawn screen, and whether Home Assistant discovery has been sent. Nothing
else does, and that is deliberate - there is no state to get subtly wrong over
a week of uptime, because there is no uptime.

Not implemented yet, and each one changes what the numbers mean:

* **The sleep current** (§3.5). Deep sleep itself is implemented - the firmware
  measures once per boot and sleeps - but nobody has put a meter in the USB lead
  yet, so the budget below is still arithmetic.
* **Ship mode on a flat cell** (§3.2). The branch is taken and logged, but
  `APP_ALLOW_SHIP_MODE` in `app_run.c` is 0 - it opens the BATFET, and a
  threshold bug on the bench would shut the board down on every power-up.
* **Timestamps and SNTP** (§5). Samples carry a sequence number and an age, not
  a wall-clock time.
* **The charger's latched FLAG0 events.** Nothing reads them, and they clear on
  read - so a fault that came and went between two wakes leaves no trace.
* **Calibration from NVS** (§7). Compile-time constants only, and the wet point
  is a placeholder.

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
recent soil moisture is worth more than old soil moisture, and the number of
dropped samples is published alongside the data so a gap in a graph has an
explanation rather than being a mystery.

Implemented in `main/app/app_buffer.c`. Two rules make it work: a sample is
buffered *before* any attempt to publish, so a failed transmission is not a lost
reading, and the buffer is cleared **only** after the broker has acknowledged
the message. A missing PUBACK, a refused connection or no association at all all
leave the samples in place, and the next cycle sends more rather than less.

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
that away for six times as many WiFi cycles. That was the right first
implementation, because it proved the whole chain with none of the buffering
logic; now that the buffer exists, `PUBLISH_EVERY_N_MEASUREMENTS` is back to the
design value of 6.

Each bundle entry carries `age_s` rather than a timestamp. There is no SNTP yet,
so an absolute time would be seconds since this board last cold-booted, which no
consumer can interpret; an age can be subtracted from the time of receipt and is
correct whether or not a clock was ever synchronised.

---

## 7. Calibration

The probe is capacitive: high output in air, lower in water. Two points define
the curve.

| Point | Value | Status |
|---|---|---|
| Wet, 100 % | **1000 mV** | chosen |
| Dry, 0 % | **2500 mV** | chosen |

Chosen rather than measured, and both ends clamp: the probe reads about 2736 mV
in air, which lands on 0 %, and anything below 1.0 V reads 100 %. For reference,
bring-up measured 2719 mV with the probe held in air - so "dry" on this scale is
slightly wetter than "in air", which is the right way round for a pot of soil.

The millivolts stay worth logging next to the percentage, and `soil_read()`
returns both: a scale whose ends were picked rather than measured can be
re-picked later, and the raw number is what makes that possible after the fact.
The open question is not the electrical range any more but what the numbers mean
in potting mix at working depth - soil never reaches either end of this scale.

The probe's output **falls** as the soil gets wetter: high in air, low in water.
`soil_init()` checks that the dry point is above the wet one and reports
`percent = -1` rather than dividing by a negative span, so a probe wired the
other way round shows up as a missing percentage instead of a plausible wrong
one.

Two further thresholds drive what the screen looks like, in `config.h`:
`SOIL_WARN_PERCENT` (40 %) turns the reading yellow, `SOIL_ALERT_PERCENT`
(20 %) turns it red. They are about the plant, so the display component only
ever sees "ok / warn / alert" and decides what that looks like - the same split
as the status LED. Both inherit the calibration's uncertainty: while the wet
point is a placeholder, so is the meaning of "20 %". A failed reading is drawn
neutral, never as an alert nobody can act on.

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
