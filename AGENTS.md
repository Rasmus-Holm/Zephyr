# AGENTS.md

Zephyr firmware project for the **ESP32-C6 DevKitC** (UCL IoT project, Zephyr v4.4.0).
The git root is this directory; the whole project is a single Zephyr app in `esp32c6-app/`.

`esp32c6-app/README.md` (Danish) holds the app-level documentation and
`docs/git-workflow.md` the branch/PR notes — read those instead of duplicating them here.

## Environment

The west workspace lives **outside** the repo at `~/zephyrproject`
(Zephyr v4.4.0, west 1.5.0, SDK `~/zephyr-sdk-1.0.1`); its manifest is filtered to
`-hal_.*,+hal_espressif,+hal_nordic`.

- `west` is **not on `PATH`** — it is `~/zephyrproject/.venv/bin/west`. Either use
  `scripts/*.sh` (they source `scripts/zephyr-env.sh`) or
  `source ~/zephyrproject/.venv/bin/activate`; the `zephyr-env` alias in `~/.bashrc`
  also `cd`s into `~/zephyrproject`.
- `ZEPHYR_BASE` (must be `~/zephyrproject/zephyr`) and `ZEPHYR_TOOLCHAIN_VARIANT=zephyr`
  have to be exported, otherwise `west build` cannot find the Zephyr package.
  `scripts/zephyr-env.sh` sets both. West finds the workspace from `ZEPHYR_BASE` when
  the CWD is outside it, so building from the app dir works.
- `west espressif ...` is a **workspace** extension and only resolves when the CWD is
  inside `~/zephyrproject` (that is why `scripts/monitor.sh` `cd`s there).
  `west build` / `west flash` run fine from the app directory.
- `Setup-Zephyr.sh` (repo root) provisions everything: venv, west workspace, SDK,
  `west blobs fetch hal_espressif`, OpenOCD udev rules, the `zephyr-env` alias.
  Run it as the normal user (never with sudo) and re-run it if it fails midway.
  It is mode `644` in git, so use `bash Setup-Zephyr.sh`, not `./Setup-Zephyr.sh`.
- `~/zephyrproject/build/` and `~/zephyrproject/usb-console.overlay` are leftovers from a
  `samples/hello_world` experiment — not part of this repo, do not build on them.

## Commands

```bash
cd esp32c6-app
scripts/build.sh            # incremental
scripts/build.sh -p         # pristine rebuild
scripts/flash.sh            # west flash -d build --runner esp32  (esptool)
scripts/console.sh          # raw read of /dev/ttyACM0, no DTR/RTS (PORT/TIMEOUT env)
scripts/monitor.sh [port]   # west espressif monitor -p /dev/ttyACM0 -e build/zephyr/zephyr.elf
```

- Board target is `esp32c6_devkitc/esp32c6/hpcore`; it is also the default in
  `CMakeLists.txt`, so `-b` may be omitted. The three-part qualifier is required —
  plain `esp32c6_devkitc` is ambiguous (`hpcore` vs `lpcore`).
- **There are no tests, no linter and no CI here.** Verification = the build succeeds,
  then flash + monitor on the real board. Do not look for a test command.
- The build produces only the app image (`build/zephyr/zephyr.bin`, flashed at `0x0` by
  the `esp32`/esptool runner). The board also offers an MCUboot bootloader, but only in
  a sysbuild (`west build --sysbuild`) — the plain build used here has none.
- `esp32c6-app/build/` is generated output and is gitignored (via
  `esp32c6-app/.gitignore`; there is no root `.gitignore`). Never edit or commit it.
  A move of the repo invalidates the absolute paths cached in `build/CMakeCache.txt` —
  rebuild pristine (`scripts/build.sh -p`) afterwards.

## Console and hardware

- The console is the board's USB Serial/JTAG controller: **`/dev/ttyACM0`**, not
  `/dev/ttyUSB0`. It requires `CONFIG_SERIAL_ESP32_USB=y` in `prj.conf` *and*
  `app.overlay` selecting `zephyr,console = &usb_serial`. The board's *default*
  console is `uart0`, so both are needed. Verified in the built image: the
  `serial_esp32_usb` driver and `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` are linked in.
- Read the console with `scripts/console.sh` (plain `cat`) or the serial-monitor
  extension. **Never toggle DTR/RTS** from a script: esptool's reset sequences
  (`USBJTAGSerialReset`, `HardReset(uses_usb=True)`) leave the ESP32-C6 in the ROM
  downloader, showing `boot:0x4 (DOWNLOAD...)` / `waiting for download` and silence.
- If there is no output after `scripts/flash.sh`, **press RST/EN once** (or replug
  USB). The app then boots and prints `Hello World! esp32c6_devkitc/esp32c6/hpcore`
  followed by `Kører - uptime: … ms` from `zephyr_app` every 5 s. Verifying the app
  runs: `esptool --before no-reset --after no-reset chip-id` fails with a write
  timeout when the app (not the ROM downloader) owns the port.
- Ubuntu here is a VMware guest. After a VM reboot, attach the board via
  *Removable Devices → Connect*; use the DevKitC's **USB** port (native
  Serial/JTAG), not the **UART** port (CP210x → `/dev/ttyUSB0`). The board
  re-enumerates when the app's USB driver starts, so the very first `printk` can be
  missed if the reader opens too late — the 5 s periodic log confirms liveness.
- esptool may warn `Set flash_size 8MB is larger than the available flash size of
  4MB`; harmless (the DevKitC has 4 MB).
- The user must be in `dialout`/`plugdev`, and a USB **data** cable is required —
  charge-only cables enumerate no device.

## Editor

- Only `ms-vscode.vscode-serial-monitor` is installed — no CMake Tools, cpptools or
  clangd. The `cmake.*` settings in `.vscode/settings.json` are therefore inert; use
  the tasks instead (`Ctrl+Shift+B` → "Zephyr: Build"), which set up the environment
  themselves via `scripts/zephyr-env.sh`.
- IntelliSense requires clangd plus the generated `build/compile_commands.json`.
- The Serial Monitor extension can open `/dev/ttyACM0` straight from the command palette.

## Code conventions

- `CMakeLists.txt` lists sources explicitly: a new `.c` file must be added to
  `target_sources(app PRIVATE ...)` or it is not compiled.
- Config split: Kconfig in `prj.conf`, devicetree changes in `app.overlay`.
  Kconfig symbols are version-sensitive — an unknown symbol aborts the build
  (`attempt to assign the value 'y' to the undefined symbol ...`).
- Indentation is **tabs** (Zephyr style) in C, CMake and devicetree; LF line endings;
  UTF-8 with Danish letters. There is no `.clang-format` — match the surrounding file.
- Docs, code comments and commit messages are **Danish**, with a conventional-commit
  prefix (`docs: ...`, `esp32c6-app: ...`).
- New log sources need `LOG_MODULE_REGISTER(...)`; `prj.conf` currently enables
  `CONFIG_LOG=y` with `CONFIG_LOG_DEFAULT_LEVEL=3` (INFO).

## Git

- Remote: `https://github.com/Rasmus-Holm/Zephyr.git`.
- Branches are numbered by task: `feature/<n>-<slug>`, plus `docs/<n>-…`, `test/<n>-…`,
  `chore/<n>-…` (1–18). Branches 2–18 currently contain only the root `README.md` and
  `Setup-Zephyr.sh` — they are empty placeholders, not partial implementations.
- Pull requests merge into `main` (see `chore/18-pull-request-to-main`).
- The root `README.md` currently only holds the merge-conflict exercise note — it is not
  project documentation.
