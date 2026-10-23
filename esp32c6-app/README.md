# esp32c6-app

Zephyr-projekt til **ESP32-C6 DevKitC** (Zephyr v4.4.0, 4 MB flash).

Konsollen kører boardets **USB Serial/JTAG**-controller og vises som
`/dev/ttyACM0`, når DevKitC'en sluttes til USB.

## Kloning

```bash
git clone https://github.com/Rasmus-Holm/Zephyr.git
cd Zephyr/esp32c6-app
```

## Forudsætninger

- West-workspace: `~/zephyrproject` (Zephyr v4.4.0, venv i `~/zephyrproject/.venv`)
- Zephyr SDK: `~/zephyr-sdk-1.0.1`
- West-workspacet sættes op med `Setup-Zephyr.sh` fra repoets rod

## Mappestruktur

```
esp32c6-app/
├── CMakeLists.txt        # Build-definition (board: esp32c6_devkitc/esp32c6/hpcore)
├── prj.conf              # Kconfig (USB-serial-konsol + logging)
├── app.overlay           # Vælger USB Serial/JTAG som zephyr,console
├── src/main.c            # Hello World-app
├── scripts/              # Byg-/flash-/monitor-scripts
│   ├── zephyr-env.sh     # Sætter miljøet (source det manuelt hvis ønsket)
│   ├── build.sh          # Bygger via west
│   ├── flash.sh          # Flasher via esptool
│   └── monitor.sh        # Åbner serial monitor på /dev/ttyACM0
└── .vscode/              # VSCode-tasks (build/flash)
```

## Byg

```bash
cd Zephyr/esp32c6-app
scripts/build.sh          # inkrementelt
scripts/build.sh -p       # ren byg
```

Uden scriptet (kræver at workspacet er initieret, eller `ZEPHYR_BASE` er sat):

```bash
west build -b esp32c6_devkitc/esp32c6/hpcore -d build
```

Boardet er også sat i `CMakeLists.txt`, så `west build -d build` uden `-b`
virker. Build-output ligger i `build/` (fx `build/zephyr/zephyr.elf`).

## Flash

Slut ESP32-C6 DevKitC'en til USB og kør:

```bash
scripts/flash.sh
```

Det svarer til:

```bash
west flash -d build --runner esp32
```

## Monitor (USB Serial/JTAG)

Når boardet er tilsluttet, vises konsollen som `/dev/ttyACM0`:

```bash
scripts/monitor.sh                 # standard: /dev/ttyACM0
scripts/monitor.sh /dev/ttyACM1    # anden port
```

Scriptet kalder `west espressif monitor` (west-extensionen findes kun inde i
workspacet, derfor `cd ~/zephyrproject` inde i scriptet). Vent på beskeden
`Hello World!` og de periodiske `LOG_INF`-linjer.

## VSCode

Åbn mappen i VSCode. Der er foruddefinerede tasks:

- `Ctrl+Shift+B` → byg
- "Zephyr: Byg (clean)" og "Zephyr: Flash" under Terminal → Run Task

## Fejlfinding

- USB-enheder kræver typisk at brugeren er i gruppen `dialout`:
  `sudo usermod -aG dialout $USER` (genstart sessionen bagefter).
- Ingen `/dev/ttyACM0`? Prøv et andet USB-kabel (kun datakabel uden
  strøm-funktion virker ikke) eller en anden USB-port.
- Den første build tager et øjeblik (ESP-IDF-moduler initieres).

## Konsol

Læs konsoluddata uden DTR/RTS-toggling (vigtigt for ESP32-C6):

```bash
./scripts/console.sh
PORT=/dev/ttyACM0 TIMEOUT=30 ./scripts/console.sh
```

Efter flash kan chippen stå i ROM-downloadmode; hvis der ikke kommer output, tryk
**RST/EN** på boardet. I VMware skal USB-enheden forbindes via
*Removable Devices → Connect*. Konsollen forventes på DevKitC'ens **USB**-port,
ikke UART-porten.

Forventet output:
```
Hello World! esp32c6_devkitc/esp32c6/hpcore
... <inf> zephyr_app: Kører - uptime: ... ms
```

## Byg, flash og monitor

Miljøet skal være aktiveret (f.eks. via `source ~/zephyrproject/.venv/bin/activate` eller `zephyr-env`).

### Byg
```bash
west build -p always -b esp32c6_devkitc/esp32c6/hpcore ./esp32c6-app -d ./esp32c6-app/build
```

### Flash
```bash
west flash --runner esptool
```

### Monitor (USB Serial/JTAG)
Der kræves **ikke** ekstern UART-adapter. Konsollen vises på `/dev/ttyACM0` via USB Serial/JTAG.

Med West (Espressif runner):
```bash
west espressif monitor
```

Eller med projektets script:
```bash
./esp32c6-app/scripts/console.sh
```

Efter reset af boardet skal `LOG_INF`, `LOG_WRN` og `LOG_ERR` vises korrekt i monitoren.
