# Soil Sensor Firmware (ESP32-C6, rev 0.1.0)

ESP-IDF firmware for the custom plant soil sensor board.

* Target: **ESP32-C6** (RISC-V)
* Toolchain: ESP-IDF **v6.0.1**

Bring-up stage: the firmware cycles the RGB status LED through red, green and
blue and logs which color it just set. Sensor sampling, e-ink and power
management build on top of that.

## Repository layout

```
CMakeLists.txt        top-level ESP-IDF project file
sdkconfig.defaults    project defaults (target etc.); sdkconfig itself is generated
main/
  main.c              app_main() entry point
  config.h            pin map and system constants - single source of truth
components/
  rgb/                WL-ICLED status LED, on top of espressif/led_strip
docs/
  hardware/           hardware description: schematic notes, pinout, BOM, board revisions
```

Components under `components/` stay board-agnostic: they take pins and
parameters through their init structs, and `main/config.h` is what supplies the
board-specific values. Only `main/` includes `config.h`.

## Build, flash, monitor

```sh
idf.py set-target esp32c6      # only needed once, or after deleting sdkconfig
idf.py build
idf.py -p COM6 flash monitor   # Ctrl-] to leave the monitor
```

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

The KiCad project lives outside this repository.
