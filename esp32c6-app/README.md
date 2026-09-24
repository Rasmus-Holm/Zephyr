# esp32c6-app

Zephyr-start-skelet til **ESP32-C6 DevKitC** (Zephyr v4.4.0).

## Mappestruktur

```
esp32c6-app/
├── CMakeLists.txt        # Zephyr build-definition
├── prj.conf              # Kconfig-konfiguration (tomt skelet)
├── src/main.c            # App-kode
├── scripts/              # Byg-/flash-scripts
│   ├── zephyr-env.sh     # Sætter miljøet (source det manuelt hvis ønsket)
│   ├── build.sh          # Bygger via west
│   └── flash.sh          # Flasher via esptool
└── .vscode/              # VSCode-tasks (build/flash)
```

## Afhængigheder (allerede installeret på denne maskine)

- West-workspace: `~/zephyrproject` (Zephyr v4.4.0, venv i `~/zephyrproject/.venv`)
- Zephyr SDK: `~/zephyr-sdk-1.0.1`
- `west`, `esptool` m.m. ligger i venv'et

## Byg

```bash
cd ~/zephyr-apps/esp32c6-app
scripts/build.sh          # inkrementelt
scripts/build.sh -p       # ren byg
```

Build-output ligger i `build/` (fx `build/zephyr/zephyr.elf`).

## Flash

Slut ESP32-C6 DevKitC'en til USB og kør:

```bash
scripts/flash.sh
```

Runner: `esp32` (esptool). Hvis flere porte findes, kan porten sættes med:
`west flash -d build --runner esp32 --elf-name zephyr.elf` eller angiv port direkte med `esptool`'s port-argument.

## Seriel konsol

Konsollen er som standard på boardets UART (USB-serielforbindelse, 115200 baud):

```bash
screen /dev/ttyUSB0 115200    # eller: picocom /dev/ttyUSB0 -b 115200
```

## VSCode

Åbn mappen i VSCode. Der er foruddefinerede tasks:

- `Ctrl+Shift+B` → byg
- "Zephyr: Byg (clean)" og "Zephyr: Flash" under Terminal → Run Task

## Bemærkninger

- USB-enheder kræver typisk at brugeren er i gruppen `dialout`:
  `sudo usermod -aG dialout $USER` (genstart sessionen bagefter).
- Den første build tager et øjeblik (ESP-IDF-moduler initieres).
- Pins: USB-CDC via `usb_serial` kan aktiveres med en overlay (eksisterende
  eksempel: `usb-console.overlay` fra `~/zephyrproject`).