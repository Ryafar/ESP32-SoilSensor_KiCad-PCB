# Firmware Architecture

How this firmware is cut into components, who owns which hardware resource, and
in what order the remaining pieces get built.

Companion to [hardware/hardware-info.md](hardware/hardware-info.md), which describes the board
and is the authority on pins, part numbers and electrical behaviour. This
document is the authority on code structure. When the two disagree, the
hardware document wins and this one gets fixed.

Status: `rgb/` is implemented, everything else is planned.

---

## 1. Layering

Three layers, and the dependency arrows only ever point downwards:

```
   main/          app_main, state machine, config.h
                  owns the shared buses, knows every pin
        |
        v
   components/    drivers: rgb, charger, battery, soil, eink, power
                  board-agnostic, no knowledge of pins or of each other
        |
        v
   ESP-IDF        led_strip, driver/i2c_master, esp_adc, driver/spi_master,
                  esp_sleep, driver/rmt_tx
```

ESP-IDF knows exactly one building block for all of this: the *component* — a
directory with a `CMakeLists.txt` and sources. Where it sits only says who owns
it, and the build system searches all three locations:

| Location | Owner | Tracked in git |
|---|---|---|
| `$IDF_PATH/components/` | Espressif, ships with the IDF (`driver`, `freertos`, `esp_adc`, …) | no, lives in the toolchain |
| `managed_components/` | pulled from the registry per `idf_component.yml` (`espressif__led_strip`) | no — regenerated from `dependencies.lock` |
| `components/` | us | yes |

`main/` is a component too. It is not special apart from being mandatory and
holding `app_main()`. Inside it:

```
main/
  main.c            app_main: picks which half of the firmware runs, nothing else
  config.h          pin map, thresholds, divider factor
  secrets.h         WiFi and MQTT credentials - gitignored, template next to it
  bringup/          hardware checks: does this board work at all
    bringup.c       their entry point: buses, drivers, task switches
  app/              the real firmware: state machine, board glue
    app_run.c       its entry point: buses, drivers, the measurement sequence
    app_net.c       WiFi, MQTT, Home Assistant discovery
    app_display.c   when the panel gets redrawn, and powering it for that
    app_screen.c    what this device's screen looks like - layout, icons, colours
    app_sample.h    one cycle's readings, shared by everything above
```

The split between `bringup/` and `app/` is deliberate. Bring-up code answers
"is the hardware sound", exists to be read by a human watching a serial log,
and gets deleted or disabled once its question is answered. Application code
answers "what should the device do" and has to survive. Mixing the two means
the throwaway half never gets thrown away.

Not called `test/`, because in ESP-IDF that name belongs to Unity unit tests,
and next to `app/` it would read as "the tests for app/" — which these are not.

`main.c` stays short by design: it includes `bringup.h` and `app_run.h`, calls
one of them, and holds no drivers, tasks or shared resources of its own. Each
half is then self-contained — `bringup.c` and `app_run.c` each create their own
buses and decide their own stack sizes and priorities, so the checks can be
rewired without touching the application and the application can be flashed
without dragging the checks along. Neither half includes the other; the only
thing that knows about both is `main.c`, because a file cannot sensibly decide
whether it itself should run.

Priorities are still decided in one visible place per build — that place is the
entry point of the half being built, not a file shared between them.
Sources are picked up through `SRC_DIRS`; a newly added file needs an
`idf.py reconfigure` before the glob sees it.

### Switching between the two halves

One line in [`main.c`](../main/main.c):

```c
/* 1 = flash the hardware checks, 0 = flash the application. */
#define RUN_BRINGUP 1
```

Then `idf.py build`. Inside a bring-up build the `RUN_*_TEST` switches at the
top of [`bringup.c`](../main/bringup/bringup.c) pick which check runs.

A `#define` rather than a Kconfig option, deliberately: the state of the switch
is then part of the diff, and there is no way to build something different from
what the repository says because of an untracked local `sdkconfig`. The cost is
that changing it is a commit — which is the right cost for "which firmware is
on the board".

**Only the selected half is compiled.** `main/CMakeLists.txt` greps the
`#define` out of `main.c` and puts only that half into `SRC_DIRS`, so the
application image contains no bring-up code at all — not unreferenced, not
sleeping, absent. Throwaway code that ships anyway is not throwaway; and it
would drag `esp_wifi` and the panel driver into an image whose power budget is
the whole design (§3.5 of the product design). The numbers, measured: the
bring-up image is ~960 KB, the application half ~144 KB.

Three consequences worth knowing:

* `main.c` is a cmake configure dependency, so flipping the switch re-runs
  cmake by itself. Without that, `SRC_DIRS` — a configure-time glob — would
  keep serving the previous half and the switch would look broken.
* The switch must stay a literal `#define RUN_BRINGUP 0|1` on one line. If the
  grep finds nothing the build fails with a message saying so, rather than
  quietly picking a half.
* The excluded half is not compiled, so it can rot unnoticed. Building the
  other half occasionally is the only thing that catches it — cheap, since it
  is one edited line.

### The rule that makes it work

**A driver never hardcodes a pin, and never opens a shared bus itself.**
Everything board-specific arrives through its init struct. `rgb_init()` does not
know that the status LED is on GPIO8; `main` reads `PIN_RGB_DIN` from
[`config.h`](../main/config.h) and passes it in.

Two payoffs. First, a board revision that moves a pin touches exactly one file.
Second, no two drivers can fight over the same peripheral, because none of them
allocate one.

### FreeRTOS stays in `main`

Components are passive. A driver exposes functions that do a thing and return —
it does not create tasks, does not own queues, and does not schedule anything.
Task creation, priorities, stack sizes and the state machine all live in `main`.

Two reasons, and the second one is the one that bites on this board:

* A driver that spawns its own task sets scheduling policy for the whole system
  from inside a leaf of the dependency tree, where nobody reading `main` can see
  it. Priorities only make sense when they are decided in one place.
* Power is the top-level state machine here. A task ticking away inside a driver
  keeps the CPU out of deep sleep, and the cost shows up as battery life months
  later rather than as a bug. If nothing runs except what `main` started,
  "everything is idle, sleep now" is a decision `main` can actually make.

Consequences:

* **Driver calls are synchronous.** `soil_read_percent()` blocks its caller for
  the settling time; `eink_render()` blocks while polling BUSY. That is
  intended — the caller chose to wait, so the caller can also choose to do it
  from a task that may block.
* **One task per driver.** Drivers are not thread-safe unless their header says
  so. With a single measurement task that is free; if that ever changes, the
  locking is added deliberately rather than assumed.
* **Interrupts notify, they do not work.** The charger INT on GPIO20 is the only
  interrupt source in the design. The ISR does nothing but a task notification;
  the reaction happens in a `main`-owned task. `charger_config_t` therefore
  carries a callback rather than the driver registering handlers behind
  `main`'s back.

### When a wrapper earns its place

Not every IDF library needs one. A wrapper is worth writing when it does at
least one of these:

* **Adds domain meaning.** `rgb_set_status(RGB_STATUS_BATTERY_LOW)` instead of
  `(255, 0, 0)`; `soil_read_percent()` instead of raw millivolts plus a
  calibration curve living at the call site.
* **Hides a stateful sequence.** E-ink init, the charger's register setup — a
  fixed order of operations that must not be re-derived by every caller.
* **Owns a resource with a lifetime.** Something that must be released again:
  the RMT channel in `rgb/`, a rail that has to go off on every exit path.

It is *not* worth writing when it forwards a single IDF call unchanged.
`gpio_set_level()` wrapped as `board_set_pin()` is one more file to maintain and
buys nothing. The test is whether the wrapper lets call sites stop thinking
about something.

### Where the hardware README's `bsp/` went

The hardware document suggests a `bsp/` component holding `board.h`, rail
helpers and ADC setup. In practice that layer dissolves into three places:

| `bsp/` responsibility | Where it actually lives | Why |
|---|---|---|
| Pin definitions (`board.h`) | `main/config.h` | Keeps drivers board-agnostic. A `board.h` inside `components/` would let any driver reach for GPIO8 directly, and the layering is gone. |
| Rail / power-switch helpers | Inside the driver that owns the rail | `soil/` owns `SOIL_POWER`, `eink/` owns `PAPER_POWER`, `battery/` owns `BAT_MEAS_EN`. Each can then guarantee its rail is off when its function returns — a promise no external helper can make. |
| ADC channel setup | `main`, handed to drivers as a handle | See below. |

So there is no `bsp/` component. Nothing is lost; the responsibilities just sit
where the invariants can actually be enforced.

### Shared resources are owned by the entry point

Two drivers need the same ADC unit: `battery/` samples GPIO0 and `soil/` samples
GPIO1, and on the C6 both are ADC1 channels. `adc_oneshot_new_unit()` on the
same unit twice fails, so exactly one place must own the unit handle. That place
is the entry point of the build — `bringup.c` today, `app_run.c` once it
exists — which creates it once and passes the handle into both init structs.
The same goes for the I²C master bus: the entry point creates it, `charger/`
receives it. Since only one half ever runs, there is no contest over who
allocates what.

Drivers therefore **borrow, never own** shared buses. They own only what is
exclusively theirs — `rgb/` owns its RMT channel, each driver owns its enable
GPIO.

---

## 2. Component inventory

Each is a directory under `components/` with `CMakeLists.txt`, sources, and an
`include/` directory holding the one public header.

### `rgb/` — status LED ✅ implemented

Würth WL-ICLED on GPIO8, WS2812 protocol via `espressif/led_strip` on RMT.

```c
esp_err_t rgb_init(const rgb_config_t *config);   // {gpio_num, brightness}
esp_err_t rgb_deinit(void);                       // frees the RMT channel
esp_err_t rgb_set(uint8_t r, uint8_t g, uint8_t b);
esp_err_t rgb_set_status(rgb_status_t status);    // BOOT/MEASURING/CHARGING/BATTERY_LOW/FAULT
esp_err_t rgb_off(void);
esp_err_t rgb_blink(rgb_status_t, uint32_t on_ms, uint32_t off_ms, uint32_t times);
```

Callers talk about *state*, not colour — the mapping lives in `rgb.c` so a
colour scheme change never touches call sites. `rgb_deinit()` must run before
deep sleep so the RMT peripheral is released.

Open: byte order (GRB vs RGB) unverified — see hardware README §9.

### `bq25186/` — charger ✅ implemented

Named after the part, not after the role: it is a chip driver with no policy in
it. I²C at 0x6A; INT, CHARGE_EN and POWER_GOOD are plain GPIO and belong to
whoever reads them (`battery/` does).

```c
esp_err_t bq25186_init(const bq25186_config_t *config); // borrows i2c_master_bus_handle_t
esp_err_t bq25186_probe(void);                          // reads STAT0: does it answer at all
esp_err_t bq25186_set_charge_current(uint16_t ma);      // also clears CHG_DIS
esp_err_t bq25186_read_status(bq25186_status_t *out);   // raw STAT0/STAT1/FLAG0
esp_err_t bq25186_read_charge_state(bq25186_charge_state_t *, bool *vin_good);
esp_err_t bq25186_enter_ship_mode(void);                // does not return in the usual sense
esp_err_t bq25186_read_reg(uint8_t reg, uint8_t *value); // and write/update, for bring-up
```

There is no `charger_configure()`. The application calls
`bq25186_set_charge_current()` on **every wake**, because the charger's watchdog
expires after 160 s and resets ICHG back to its 10 mA default — but which
current, and when, is policy and stays out of the driver.

`bq25186_read_status()` hands back raw registers on purpose: most of those bits
have never been needed, and a wrong flag name is worse than a byte.
`bq25186_read_charge_state()` decodes the two fields that *are* needed, with the
bit positions taken from hardware README §4.4.

`bq25186_enter_ship_mode()` opens the BATFET, which removes the system rail and
therefore powers down the ESP32 mid-instruction. It is the last thing the
firmware ever executes. It gets a doc comment saying exactly that, because it
looks like an ordinary function and is not one.

### `battery/` — cell voltage ✅ implemented

```c
esp_err_t battery_init(const battery_config_t *config); // borrows adc_oneshot_unit_handle_t
esp_err_t battery_read(battery_reading_t *out);         // millivolts + usb_present + charger_int
esp_err_t battery_deinit(void);
```

One call rather than a voltage function and a status function: the divider
enable, the two charger status pins and the conversion all happen in one place,
and the caller cannot take them apart in a way that leaves the divider
connected. `battery_read()` asserts the enable, waits out the RC, averages,
deasserts — on the error path too — and only then returns.

It returns `ESP_ERR_INVALID_STATE` for a voltage no 1-cell LiPo can have. That
check is in the driver because the alternative was seen during bring-up: a
divider that never switched in reads near full scale, and the ratio turns that
into a confident-looking 7 V.

There is no `battery_is_low()`. Which voltage counts as low is policy, it needs
hysteresis and the USB state to mean anything, and it lives in the application.

Open: the divider ratio is still the nominal 2.0 — it wants calibrating against
a multimeter (`BATTERY_DIVIDER_RATIO` in `config.h`).

### `soil/` — capacitive probe ✅ implemented

```c
esp_err_t soil_init(const soil_config_t *config); // borrows the same ADC unit
esp_err_t soil_read(soil_reading_t *out);         // millivolts and percent together
esp_err_t soil_deinit(void);
```

Same discipline as `battery/`: rail on, settle, average, rail off, error paths
included. Millivolts and percentage come back together rather than from two
functions, because the percentage depends on a calibration that is provisional
and the raw value is what stays true — separating them invites reporting one
without the other.

`percent` is `-1` when the two calibration points do not bracket a range, which
is more useful than a number derived from a division that should not have
happened.

Open: the wet calibration point has never been measured — `config.h` carries a
placeholder, so every percentage the firmware reports today is wrong by whatever
the real value turns out to be.

### `eink/` — e-ink ✅ implemented

SPI write-only, plus BUSY / RST / DC and the `PAPER_POWER` rail.

```c
esp_err_t eink_init(const eink_config_t *config);  // pins, SPI host, rail polarity
esp_err_t eink_refresh(bool force_full);           // pushes whatever the canvas holds
esp_err_t eink_show_pattern(eink_pattern_t);       // bring-up diagnostics
bool      eink_is_busy(void);
esp_err_t eink_power_off(void);
```

plus the drawing primitives in `eink_canvas.h`: `clear`, `pixel`, `fill_rect`,
`rect`, `line`, `disc`, `digit`, `number`, `percent`, against a four-value
colour enum.

**This component knows about a panel, not about a device**, and that is a
deliberate constraint rather than an accident. It holds no layout, no
thresholds and no idea what is being displayed; the caller draws with the canvas
and then calls `eink_refresh()`. The screen of *this* product lives in
[`main/app/app_screen.c`](../main/app/app_screen.c) — the water drop, the
battery icon, the moisture colours, the cell-voltage-to-icon mapping.

It was not always this way. `eink.h` used to expose an `eink_screen_t` with a
`soil_percent` field and an `eink_moisture_level_t`, and `eink.c` drew the
layout. That reads as convenient and is the wrong boundary: it made a display
driver un-reusable for anything that is not a plant sensor, and it put product
decisions — where "dry" starts, what the battery icon means — behind a header
whose subject is a controller. Moving the layout into `main/app/` cost one file
and made the component droppable into the next project unchanged.

The one thing the split has to give up: the e-ink bring-up task used to preview
the layout with made-up readings. It cannot any more, because `bringup/` and
`app/` do not include each other and only one of them is compiled. The
bring-up task keeps the patterns, which are what actually answer "is this panel
wired correctly"; how the layout looks is a question for a build that has real
readings.

Inside, the display splits into files rather than into a monolith:

| File | Job | Header |
|---|---|---|
| `eink.c` | Lifecycle, refresh policy, patterns | `include/eink.h` — **public** |
| `eink_canvas.c` | Drawing primitives and the pixel packing | `include/eink_canvas.h` — **public** |
| `epd_bus.c` | SPI, D/C, RST, BUSY and the rail. The half both controllers share | `priv_include/` |
| `ssd1681.c` | Black/white controller. Packed 1 bpp, full and partial refresh | `priv_include/` |
| `jd79660.c` | Four-colour controller. Packed 2 bpp, one refresh mode | `priv_include/` |

The controller files are named after their chips for the same reason the charger
driver is `bq25186` rather than `charger`: the name says which datasheet the
register writes came from. `eink.c` is the panel-agnostic half and holds no
controller registers at all.

These were four separate components at first, and that was a mistake worth
recording. A component is a unit of *dependency and reuse*, not of file
organisation — nothing outside `eink/` ever includes `ssd1681.h`, so the extra
boundaries cost three `CMakeLists.txt` and bought nothing. Collapsing them also
fixed two real things: `PRIV_INCLUDE_DIRS` makes `eink.h` the only reachable
header, so `main` can no longer bypass the component and drive the controller
directly, and the unused controller stopped being compiled at all (see below).

Which controller is compiled in is a **build-time** choice — the two `#define`s
in `components/eink/include/eink_panel.h` — because it sets the framebuffer
depth: 1 bpp for the black/white panel, 2 bpp for the four-colour one. It sits
in the component rather than in `main/config.h` because `components/` may not
include that file, and a build-time constant cannot travel through an init
struct the way a pin number does. A `#error` catches setting both or neither. Making it a runtime choice
would mean carrying both, and the drawing code would have to branch per pixel.
As it stands the drawing code is written once against a colour enum, and the
packing is the only thing that differs.

Both controller files are listed in `SRCS` and each one's body sits behind the
`#if`, so only the fitted panel's driver produces code. CMake cannot read that
header, which is why the selection is done by the preprocessor rather than in
`CMakeLists.txt`.

The one thing this costs for reuse: picking the panel means editing a file
*inside* the component, which is exactly what you do not want when the
component is a shared dependency. A Kconfig `choice` would let the consuming
project select it from its own `sdkconfig.defaults` instead. Worth doing if this
component is ever actually copied out; not worth the churn while it has one
consumer.

`epd_bus.c` earns its place under the rule above — it is not a wrapper, it is
the thing that lets the two controller drivers contain only their own command
sequences. It also owns two non-obvious invariants:

* `epd_bus_power_off()` frees the SPI bus and drives every line low *before*
  opening the load switch. An idle SPI peripheral holding its pins high
  back-feeds the panel through its ESD diodes and the rail never reaches 0 V.
* CS is a plain GPIO, not the SPI peripheral's hardware CS, because a frame is
  5–10 kB and gets chunked on the way out — hardware CS would deassert between
  chunks in the middle of a RAM write.

There is no font. Everything on this screen is a number, a bar or an icon, so
the digits are drawn as seven segments from rectangles: a few hundred bytes of
code instead of a few kilobytes of glyph bitmaps, and more legible at the size
a 200×200 panel needs.

### `power/` — sleep and wake

```c
esp_err_t power_deep_sleep(uint32_t seconds);   // does not return
```

Deep sleep and wake sources only. Ship mode deliberately lives in `charger/`,
not here — it is an I²C write to the charger, and putting it in `power/` would
make `power/` depend on `charger/`. Instead `main` sequences the two. The
dependency graph stays a tree.

---

## 3. Dependency graph

```
                        main
              (config.h, state machine,
               owns i2c bus + adc unit)
        ______________|_______________
       /       /      |      \        \
     rgb   charger  battery  soil    eink     power
       \       \      |      /        /        /
        \_______\_____|_____/________/________/
                      |
                   ESP-IDF
```

No component includes another component's header. If two ever need to share
something, it goes through `main` — that is what `main` is for.

---

## 4. The state machine

Lives in `main/`, not in a component: nothing else calls it, so it needs no
public interface. Follows the flow in the hardware README §5.

```
BOOT ──► CHECK POWER ──► ACTIVE ──► DEEP SLEEP ──┐
             │                                    │
             │ battery low, no USB                │ timer wake
             ▼                                    │
         LOW-BATT ──► charger ship mode           │
                      (board off until USB)       │
             ▲────────────────────────────────────┘
```

**BOOT** — create the I²C bus and ADC unit, init every driver, reconfigure the
charger (watchdog, see above).

**CHECK POWER** — read the cell. Below `BATTERY_SHIP_MODE_MV` (3300) *and* no
USB present, go to LOW-BATT.

**ACTIVE** — read charger status, sample soil, refresh the display, blink the
status LED, compute the next wake interval.

**LOW-BATT** — everything that must still happen, happens here first: show a low
battery indication, flush logs, release peripherals. The I²C write entering ship
mode is the final statement. There is no code path after it.

**DEEP SLEEP** — both load switches off, RGB dark and its RMT channel released,
timer armed.

### What survives deep sleep

Deep sleep wipes RAM but keeps RTC memory (`RTC_DATA_ATTR`). Candidates worth
keeping: boot count, last soil reading and last rendered display state (to skip
a redundant e-ink refresh), consecutive fault count. Ship mode wipes even this —
after a ship-mode wake everything starts from zero, which is fine, since a board
that has been off has nothing worth remembering.

---

## 5. Conventions

**Error handling.** Every fallible function returns `esp_err_t`; the last
parameter is the out-value. Inside components use `ESP_RETURN_ON_ERROR` /
`ESP_RETURN_ON_FALSE` from `esp_check.h`, as `rgb.c` already does.

**`ESP_ERROR_CHECK` is for bring-up code only.** It aborts, and abort means
reboot, and on a battery-powered device a reboot loop is a flat cell in a few
days with nobody watching. Production paths log the error, show
`RGB_STATUS_FAULT`, and still reach deep sleep. Reaching deep sleep is more
important than completing a measurement.

**Power discipline.** Any function that enables a rail disables it again on
every exit path, including error paths. A driver that returns an error still
leaves the hardware off.

**Naming.** Public symbols are prefixed with the component name (`rgb_`,
`charger_`). File-static state is `s_`-prefixed. One `TAG` per source file,
matching the component name.

**Logging.** All console output goes over USB-Serial-JTAG. The UART console must
stay disabled — it drives GPIO16/17, and GPIO16 is `BAT_MEAS_EN` on this board.

---

## 6. Build order

Ordered so that each step is testable on real hardware before the next begins,
and so that unresolved hardware questions block as little as possible.

| # | Step | Unblocks / notes |
|---|---|---|
| 1 | `rgb/` | ✅ done — first proof the board runs our code |
| 2 | Console + flash config | ✅ done — `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`, `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`. Frees GPIO16 for `BAT_MEAS_EN` |
| 3 | `bq25186/` | ✅ done — I²C bring-up. First real check that the bus works: a known register against the datasheet reset value |
| 4 | `battery/` | ✅ done — the divider ratio is still nominal, which affects the absolute voltage and not the code |
| 5 | `soil/` | ✅ done — the wet calibration point is still a placeholder |
| 6 | `eink/` | ✅ done — panel driver, both controllers, bring-up task |
| 7 | App stage 1: measure + report | ✅ done — `main/app/`: the §2 sequence without sleep, on a one-minute timer, publishing to MQTT every cycle |
| 8 | `/CE` control + the screen in the app | ✅ done — charging is enabled only while VIN is present (hardware README §4.2a), and the panel is redrawn when the picture would differ |
| 9 | Deep sleep | ✅ done — measure once per boot, radio up only on a publish cycle, `/CE` held across sleep, LED dark and RMT released before it, RTC memory for what has to survive. **The sleep current still has to be measured** |
| 10 | `FLAG0` in the app | The charger's latched events are invisible, and they clear on read - a sleeping device loses them unseen. Next |
| 11 | RTC ring buffer + bundled publish | ✅ done — samples accumulate in RTC memory, the radio runs every 6th wake, and the buffer is cleared only on a PUBACK |
| 12 | SNTP and timestamps | §5. Samples carry an age today, which is enough to place them but not to name a time |
| 13 | Calibration into NVS | The two soil points and the divider ratio, so a board can be calibrated without a rebuild |

Steps 10, 12 and 13 are what is left, and the first real task is not code: put a
meter in the USB lead and find out what the board actually draws asleep.
Everything in §3.5 of the product design is arithmetic until that number
exists.

---

## 7. Open decisions

Carried from the hardware README §9, listed here because each one changes code:

- **Low battery: ship mode vs deep sleep.** Ship mode is the plan. Deep sleep
  would let the device self-recover without USB, at a much higher idle drain.
  Decides whether `LOW-BATT` calls `charger_enter_ship_mode()` or
  `power_deep_sleep()`.
- **RGB byte order** (GRB vs RGB) — one enum in `rgb.c`, verified by the colour
  cycle currently running in `main.c`.
- **Battery divider ratio** — a constant in `config.h`; without it
  `battery_read_mv()` cannot be written.
- **Active polarity of `SOIL_POWER`, `PAPER_POWER`, `CHARGE_EN`,
  `BAT_MEAS_EN`** — they drive P-FETs, so asserted may well mean driving low.
  Getting this wrong means a rail that is never off, which is a battery-life
  bug rather than a visible one.
