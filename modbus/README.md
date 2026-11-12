# Modbus RTU over RS485 – ESP32-C6 + HW-519 ↔ PC + USB-RS485

Demo der viser begge RS485-komponenter i brug i vores IIoT-kæde:

```
ESP32-C6 (Zephyr)  --UART-->  HW-519  ==RS485 A/B==  USB-RS485-adapter  --USB-->  PC (Python)  --MQTT-->  Mosquitto
Modbus RTU server (slave, unit 1)                                              Modbus master + bro
```

* **HW-519 (TTL → RS485)** sidder på mikrocontrolleren. Den har automatisk retningsstyring,
  så der skal ikke styres en DE/RE-pin fra firmwaren.
* **USB TTL → RS485-adapteren** sidder i PC'en og dukker op som `/dev/ttyUSB0`.

## Ledningsføring

| ESP32-C6 DevKitC | HW-519 |
|---|---|
| 3V3 | VCC (brug 3,3 V – ESP32-C6 er **ikke** 5 V-tolerant) |
| GND | GND |
| GPIO5 (UART1 TX) | TXD |
| GPIO4 (UART1 RX) | RXD |

Mærkningen TXD/RXD på HW-519 refererer typisk til MCU-siden (TX→TXD). Svarer slaven ikke, så byt TX/RX – det skader ikke.

| HW-519 | USB-RS485-adapter |
|---|---|
| A | A (evt. mærket A+ / D+) |
| B | B (evt. mærket B- / D-) |
| GND (hvis den findes) | GND |

Kort kabel på bordet kræver ingen ekstra terminering. På lange kabler: 120 Ω i hver ende af bussen.

## Registerkort (unit id 1, 9600 8N1)

| Type | Adresse | Indhold |
|---|---|---|
| Coil | 0 | Aktuator/LED (læs/skriv) |
| Input register | 0 | Uptime i sekunder |
| Input register | 1 | Simuleret temperatur i 0,1 °C |
| Input register | 2 | Antal håndterede forespørgsler |
| Holding register | 0–7 | Frie parametre (læs/skriv) |

## Firmware (ESP32-C6)

```bash
cd ~/zephyrproject
west build -p always -b esp32c6_devkitc/esp32c6/hpcore zephyr/modbus/firmware
west flash
west espressif monitor     # forvent: "Modbus RTU server klar: unit 1 ..."
```

## PC-siden

```bash
sudo usermod -aG dialout $USER     # én gang, log ud/ind bagefter
cd zephyr/modbus/pc
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt

# 1) Ren Modbus-test
.venv/bin/python modbus_mqtt_bridge.py --no-mqtt

# 2) Med MQTT (start broker fra ~/iot-broker først)
.venv/bin/python modbus_mqtt_bridge.py --broker localhost
mosquitto_sub -t 'iiot/modbus/#' -v
mosquitto_pub -t iiot/modbus/1/cmd/coil -m 1
mosquitto_pub -t iiot/modbus/1/cmd/holding/0 -m 250
```

Kører I Linux i en VM (VMware), skal USB-adapteren sendes igennem til VM'en før `/dev/ttyUSB0` dukker op.

## Fejlfinding

* `Intet svar fra unit 1` → byt TX/RX på HW-519, tjek A/B (byt dem hvis i tvivl), tjek baudrate og fælles GND.
* `Kunne ikke åbne /dev/ttyUSB0` → `ls /dev/ttyUSB*`, og tjek `dialout`-gruppen.
* Til at se rå trafik: `picocom -b 9600 /dev/ttyUSB0` (binært, men viser at der sker noget).
