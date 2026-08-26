#!/usr/bin/env bash
# =============================================================================
#  Zephyr 4.4.0 opsætning på Ubuntu — UCL / Micro Technic Industrielt IoT-projekt
# =============================================================================
#  Kør med:   chmod +x setup-zephyr.sh && ./setup-zephyr.sh
#  Scriptet kan køres igen uden problemer hvis noget fejler undervejs.
#
#  Baseret på den officielle guide til Zephyr v4.4.0:
#  https://docs.zephyrproject.org/4.4.0/develop/getting_started/index.html
# =============================================================================

set -euo pipefail

WS="$HOME/zephyrproject"
ZEPHYR_TAG="v4.4.0"

# ---- Farver til output ------------------------------------------------------
G='\033[1;32m'; Y='\033[1;33m'; R='\033[1;31m'; N='\033[0m'
step()  { echo -e "\n${G}==> $*${N}"; }
warn()  { echo -e "${Y}[!] $*${N}"; }
die()   { echo -e "${R}[FEJL] $*${N}"; exit 1; }

[[ $EUID -eq 0 ]] && die "Kør IKKE dette script som root/sudo. Kør det som din normale bruger."

# ---- 0. Tjek OS -------------------------------------------------------------
step "Tjekker system"
. /etc/os-release
echo "    OS:     ${PRETTY_NAME}"
echo "    Arch:   $(uname -m)"
if [[ "${ID}" != "ubuntu" ]]; then
    warn "Dette script er skrevet til Ubuntu. Fortsætter alligevel."
fi

# ---- 1. Systempakker --------------------------------------------------------
step "Opdaterer pakkelister og installerer værktøjer (kræver din adgangskode)"
sudo apt-get update
sudo apt-get upgrade -y

PKGS=(git cmake ninja-build gperf ccache dfu-util device-tree-compiler wget
      python3-dev python3-venv python3-pip python3-tk xz-utils file make gcc
      libsdl2-dev libmagic1 usbutils minicom picocom)

# gcc-multilib findes ikke på ARM64
if [[ "$(uname -m)" == "x86_64" ]]; then
    PKGS+=(gcc-multilib g++-multilib)
else
    warn "ARM64 detekteret — springer gcc-multilib/g++-multilib over (findes ikke)."
fi

sudo apt-get install -y --no-install-recommends "${PKGS[@]}"

# ---- 2. Versionstjek --------------------------------------------------------
step "Verificerer versioner (minimumskrav for Zephyr 4.4.0)"
printf "    %-10s %-14s (min. %s)\n" "cmake"  "$(cmake --version | head -1 | awk '{print $3}')"  "3.20.5"
printf "    %-10s %-14s (min. %s)\n" "python3" "$(python3 --version | awk '{print $2}')"        "3.12"
printf "    %-10s %-14s (min. %s)\n" "dtc"    "$(dtc --version | awk '{print $NF}')"            "1.4.6"

PYMIN=$(python3 -c 'import sys; print(1 if sys.version_info >= (3,12) else 0)')
[[ "$PYMIN" == "1" ]] || die "Python 3.12+ er påkrævet. Opgradér Ubuntu til 24.04 LTS eller nyere."

# ---- 3. Seriel-adgang uden sudo --------------------------------------------
step "Giver din bruger adgang til USB/seriel-porte"
for grp in dialout plugdev; do
    if getent group "$grp" >/dev/null; then
        sudo usermod -aG "$grp" "$USER"
        echo "    Tilføjet til gruppen '$grp'"
    fi
done
warn "Du skal logge ud og ind igen (eller genstarte) før gruppe-ændringerne virker."

# ---- 4. Python virtuelt miljø ----------------------------------------------
step "Opretter Python-virtualenv i $WS/.venv"
mkdir -p "$WS"
if [[ ! -f "$WS/.venv/bin/activate" ]]; then
    python3 -m venv "$WS/.venv"
else
    echo "    Findes allerede — genbruger."
fi
# shellcheck disable=SC1091
source "$WS/.venv/bin/activate"
pip install --upgrade pip
pip install west

# ---- 5. Hent Zephyr-kildekode ----------------------------------------------
step "Henter Zephyr $ZEPHYR_TAG med west (dette tager nogle minutter)"
if [[ ! -d "$WS/.west" ]]; then
    west init "$WS" --mr "$ZEPHYR_TAG"
else
    echo "    West-workspace findes allerede."
fi
cd "$WS"
west update

step "Eksporterer Zephyr CMake-pakke"
west zephyr-export

step "Installerer Zephyrs Python-afhængigheder"
west packages pip --install

# ---- 6. Zephyr SDK (compilere) ---------------------------------------------
step "Installerer Zephyr SDK — matchende toolchain hentes automatisk (~1-2 GB)"
cd "$WS/zephyr"
if compgen -G "$HOME/zephyr-sdk-*" > /dev/null; then
    echo "    En SDK findes allerede i \$HOME — springer download over."
    echo "    Kør 'west sdk install' manuelt hvis du vil geninstallere."
else
    west sdk install
fi

# ---- 7. udev-regler til flash/debug-probes ---------------------------------
step "Installerer udev-regler så du kan flashe uden sudo"
RULES=$(find "$HOME" /opt /usr/local -maxdepth 5 -path '*zephyr-sdk-*' \
        -name '60-openocd.rules' 2>/dev/null | head -1 || true)
if [[ -n "$RULES" ]]; then
    sudo cp "$RULES" /etc/udev/rules.d/
    sudo udevadm control --reload
    sudo udevadm trigger
    echo "    Kopieret: $RULES"
else
    warn "Kunne ikke finde 60-openocd.rules. Se guidens fejlfindings-afsnit."
fi

# ---- 8. Espressif binary blobs (Projekt A og B) ----------------------------
step "Henter Espressif binære blobs (nødvendige for ESP32 — Projekt A og B)"
cd "$WS/zephyr"
west blobs fetch hal_espressif || warn "Blob-hentning fejlede. Kør senere: west blobs fetch hal_espressif"

# ---- 9. Testbyg uden hardware ----------------------------------------------
step "Testbygger 'hello_world' til native_sim (kræver ingen hardware)"
cd "$WS/zephyr"
west build -p always -b native_sim samples/hello_world -d /tmp/zbuild-test

echo -e "\n${G}--- Kører den byggede applikation ---${N}"
/tmp/zbuild-test/zephyr/zephyr.exe || true
rm -rf /tmp/zbuild-test

# ---- 10. Bekvemmelighed: aktiverings-alias ---------------------------------
step "Opretter genvej 'zephyr-env' i din ~/.bashrc"
SNIPPET="alias zephyr-env='source $WS/.venv/bin/activate && cd $WS'"
if ! grep -qF "alias zephyr-env=" "$HOME/.bashrc" 2>/dev/null; then
    { echo ""; echo "# Zephyr udviklingsmiljø (UCL IoT-projekt)"; echo "$SNIPPET"; } >> "$HOME/.bashrc"
    echo "    Tilføjet. Skriv 'zephyr-env' i en ny terminal for at aktivere miljøet."
else
    echo "    Alias findes allerede."
fi

# ---- Færdig -----------------------------------------------------------------
cat <<EOF

$(echo -e "${G}")=============================================================
 FÆRDIG — Zephyr $ZEPHYR_TAG er klar i $WS
=============================================================$(echo -e "${N}")

NÆSTE SKRIDT:

  1. Log ud og ind igen (nødvendigt for USB/seriel-adgang).

  2. I hver ny terminal, aktivér miljøet:
         zephyr-env

  3. Se hvilke boards Zephyr kender:
         west boards | grep -i esp32c6

  4. Åbn workspacet i VS Code:
         code $WS

Se guiden ZEPHYR-OPSAETNING.md for VS Code-konfiguration,
projektstruktur og hvad du gør når du får tildelt projekt A, B eller C.
EOF
