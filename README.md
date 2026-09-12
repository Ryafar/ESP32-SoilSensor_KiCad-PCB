# Soil Sensor Firmware (ESP32-C6, rev 0.1.0)

ESP-IDF firmware for the custom plant soil sensor board.

* Target: **ESP32-C6** (RISC-V)
* Toolchain: ESP-IDF **v6.0.1**

Two halves, one switch. The **bring-up** half is a set of hardware checks meant
to be read in a serial log; the **application** half is the firmware that has to
survive, designed in [docs/product-design.md](docs/product-design.md). The
application currently measures soil and battery on a one-minute timer, logs
each reading and publishes it to MQTT with Home Assistant discovery. Deep
sleep, the RTC sample buffer and the e-ink screen are the parts still missing.

## Repository layout

```
CMakeLists.txt        top-level ESP-IDF project file
sdkconfig.defaults    project defaults (target etc.); sdkconfig itself is generated
main/
  main.c              app_main(): picks bring-up or application, nothing else
  config.h            pin map and system constants - single source of truth
  bringup/            hardware checks, one *_test_task per question; throwaway
  app/                the real firmware; a placeholder so far
components/
  rgb/                WL-ICLED status LED, on top of espressif/led_strip
  bq25186/            I2C battery charger
  battery/            cell voltage behind the switched divider
  soil/               capacitive probe on its switched rail
  eink/               e-paper panel (JD79660 / SSD1681) and its SPI bus
docs/
  hardware/           the board: schematic notes, pinout, BOM, board revisions
  firmware-architecture.md   how the code is cut up and who owns which peripheral
  product-design.md   what the device does: wake cycle, thresholds, reporting
```

Which of the two halves gets flashed is decided by `RUN_BRINGUP` in
[main/main.c](main/main.c) — see
[Build, flash, monitor](#build-flash-monitor).

Components under `components/` stay board-agnostic: they take pins and
parameters through their init structs, and `main/config.h` is what supplies the
board-specific values. Only `main/` includes `config.h`.

## Build, flash, monitor

```sh
cp main/secrets.h.example main/secrets.h   # WiFi + MQTT; gitignored, needed by the app half
idf.py set-target esp32c6      # only needed once, or after deleting sdkconfig
idf.py build
idf.py -p COM6 flash monitor   # Ctrl-] to leave the monitor
idf.py reconfigure             # after adding a source file: SRC_DIRS globs at configure time
```

**Bring-up or application.** One line in [main/main.c](main/main.c):

```c
#define RUN_BRINGUP 1          // 1 = the hardware checks, 0 = the application
```

Rebuild and flash — cmake re-runs by itself, because only the selected half is
compiled: the application image contains no bring-up code at all. Within a
bring-up build the `RUN_*_TEST` switches at the top of
[main/bringup/bringup.c](main/bringup/bringup.c) select which check runs — keep
one at a time, because a task nobody is watching only fills the log with errors
you learn to ignore.

## Configuration

```sh
idf.py menuconfig
```

Settings that should hold for everybody belong in `sdkconfig.defaults`; the
generated `sdkconfig` is not tracked in git.

## Documentation

* [docs/hardware/README.md](docs/hardware/README.md) — the board: blocks, pin
  map, power strategy, charger configuration. Authority on anything electrical.
* [docs/firmware-architecture.md](docs/firmware-architecture.md) — how the
  firmware is cut into components, who owns which peripheral, build order.
* [docs/product-design.md](docs/product-design.md) — what the device does: the
  wake cycle, battery thresholds, intervals, where data lives, MQTT reporting.
  Authority on behaviour.

The KiCad project lives outside this repository.
