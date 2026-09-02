# Soil Sensor Firmware (ESP32-C6, rev 0.1.0)

ESP-IDF firmware for the custom plant soil sensor board.

* Target: **ESP32-C6** (RISC-V)
* Toolchain: ESP-IDF **v6.0.1**

The firmware currently does nothing but blink the status LED — a bring-up smoke
test for the new board. Sensor sampling and connectivity build on top of it.

## Repository layout

```
CMakeLists.txt        top-level ESP-IDF project file
sdkconfig.defaults    project defaults (target, pin assignment); sdkconfig itself is generated
main/
  main.c              app_main() entry point
  Kconfig.projbuild   project configuration (menuconfig -> "Soil Sensor Configuration")
docs/
  hardware/           hardware description: schematic notes, pinout, BOM, board revisions
```

## Build, flash, monitor

```sh
idf.py set-target esp32c6      # only needed once, or after deleting sdkconfig
idf.py build
idf.py -p COM6 flash monitor   # Ctrl-] to leave the monitor
```

## Configuration

```sh
idf.py menuconfig     # -> "Soil Sensor Configuration"
```

Settings that should hold for everybody belong in `sdkconfig.defaults`; the
generated `sdkconfig` is not tracked in git.

## Hardware

See [docs/hardware/](docs/hardware/) for the board description. The KiCad
project lives outside this repository.
