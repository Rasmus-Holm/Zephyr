<!-- _Formål: Dokumentere det Zephyr-projekt, der tester Wi-Fi, TLS og almindelig MQTT uden SparkplugB mod en Linux-Mosquitto-broker. -->

# 1. wifi-tls-no-sparkplugB

## 1.1. 📋 Indholdsfortegnelse

- [1. wifi-tls-no-sparkplugB](#1-wifi-tls-no-sparkplugb)
  - [1.1. 📋 Indholdsfortegnelse](#11--indholdsfortegnelse)
  - [1.2. Formål og scope](#12-formål-og-scope)
  - [1.3. Pipeline](#13-pipeline)
  - [1.4. Lokal konfiguration](#14-lokal-konfiguration)
  - [1.5. Filer til hver version](#15-filer-til-hver-version)
  - [1.6. Build](#16-build)
    - [1.6.1. Build med iot-wrapperen](#161-build-med-iot-wrapperen)
  - [1.7. Runtime-gates](#17-runtime-gates)
  - [1.8. Modem-variant (Stamp CatM over PPP)](#18-modem-variant-stamp-catm-over-ppp)
    - [1.8.1. Opstartsoprydning af modemet](#181-opstartsoprydning-af-modemet)
    - [1.8.2. Uret over mobilnettet](#182-uret-over-mobilnettet)
    - [1.8.3. Kontrol af certifikatets datoer](#183-kontrol-af-certifikatets-datoer)
  - [1.9. NDATA batching (test/power-batching)](#19-ndata-batching-testpower-batching)
  - [1.10. Åbne punkter](#110-åbne-punkter)

Dette er en ESP32-C6/Zephyr test- og udviklingsapplikation til Wi-Fi eller Stamp CatM/cellular og MQTT over TLS uden SparkplugB. Begge netværksvarianter deler TLS-, MQTT- og Modbus-koden. Projektet er samme TLS- og MQTT-spor som `wifi-tls-with-sparkplugB`, men uden protobuf, nanopb, birth/death-payloads og will-message. Det er ikke den færdige Project A-firmware eller fuld cellular acceptance.

## 1.2. Formål og scope

Projektet er et trin, før SparkplugB eventuelt tages med. Det skal vise, at Wi-Fi, SNTP, TLS 1.2 med IP-SAN og MQTT CONNACK fungerer, og at der kan publiceres en almindelig MQTT-besked. Det har ingen SparkplugB-kode, ingen `proto/` og ingen `tools/`-consumer.

Den testede SparkplugB-version ligger i `zephyr-projects/_iot-project-tests/wifi-tls-with-sparkplugB/`. Dens TLS- og MQTT-logik er den samme som her, men `src/main.c` der har birth/death-håndtering og will-message.

## 1.3. Pipeline

```text
Wi-Fi/cellular
    -> DHCP/PPP
    -> SNTP mod pool.ntp.org, så certifikatet kan valideres
    -> TCP 8883
    -> TLS 1.2 med public CA og IP-SAN, uden SNI
    -> MQTT 3.1.1 CONNECT og CONNACK + Sparkplug B NBIRTH
    -> Modbus RTU samples hvert 5. sekund
    -> Hver APP_BATCH_SIZE samples: QoS 0 NDATA batch til spBv1.0/UCL/NDATA/<CONFIG_MQTT_CLIENT_ID>
```

Firmwaren forbinder til brokeren i den adresse, der står i `CONFIG_MQTT_BROKER_ADDRESS`. Den offentlige CA i `src/tls_credentials.h` er identisk med CA'en i `wifi-tls-with-sparkplugB`, og den aktive konfiguration i `wifi-local.conf` peger på Webdock-brokeren `92.113.144.124`.

Den lokale Linux-Mosquitto-route med lokal CA og broker-IP `192.168.87.119` beskrives i runbooken `wifi-tls-reproduction-runbook_linux.md` i workspace (`assignment-industriel-iot-projekt/ai-workspace/notes/technical/`). Den route passer ikke til den CA, som ligger i firmwaren, så de to routes skal ikke blandes sammen.

## 1.4. Lokal konfiguration

Opret eller opdatér `wifi-local.conf` lokalt med projektets faktiske værdier. Filen er git-ignoreret og indlæses af `run-build.sh` via `-DEXTRA_CONF_FILE`; `CMakeLists.txt` indlæser ingen conf-filer:

```conf
CONFIG_WIFI_SAMPLE_SSID="..."
CONFIG_WIFI_SAMPLE_PSK="..."
CONFIG_MQTT_BROKER_ADDRESS="..."
CONFIG_MQTT_CLIENT_ID="..."
CONFIG_MQTT_TOPIC="..."
```

Firmware bruger disse værdier direkte via Zephyr Kconfig. `CONFIG_MQTT_BROKER_ADDRESS` skal være en IPv4-adresse, fordi `main.c` bruger `inet_pton`. `src/tls_credentials.h` skal indeholde den offentlige CA, der signer brokerens servercertifikat. CA-private keys, servercertifikaternes private keys og MQTT-passwords må aldrig kopieres hertil.

## 1.5. Filer til hver version

Wi-Fi- og modem-versionen bruger samme kildekode og værktøjer. De adskiller sig kun i konfiguration, overlays og de lokale filer med værdier, der ikke må committes.

Skal med i git: `src/`, `Kconfig`, `CMakeLists.txt`, `prj.conf`, `README.md` og projektets scripts. Skal ikke med i git: `wifi-local.conf`, fordi den indeholder SSID og adgangskode, samt `build-wifi/` og `build-cellular/`, fordi det er genereret output. `cellular-local.conf` er committet med vilje, fordi den kun indeholder broker-adresse, klient-ID og topic og ingen credentials. Kom aldrig SSID, PSK, passwords eller nøgler i den.

Filer til Wi-Fi-versionen:

| Fil                    | Indhold                                       | Git       |
| ---------------------- | --------------------------------------------- | --------- |
| `wifi.conf`            | Wi-Fi-stack og `CONFIG_NETWORK_CELLULAR=n`    | committet |
| `wifi-local.conf`      | SSID, adgangskode, broker, klient-ID og topic | ignoreret |
| `modbus-uart0.overlay` | Modbus RTU på UART0, GPIO22 TX og GPIO23 RX   | committet |
| `usb.overlay`          | USB Serial/JTAG-konsol                        | committet |

Filer til modem-versionen:

| Fil                    | Indhold                                                                        | Git       |
| ---------------------- | ------------------------------------------------------------------------------ | --------- |
| `cellular.conf`        | `CONFIG_NETWORK_CELLULAR=y`, PPP, DNS-timeout, datokontrol, APN og CMUX-buffer | committet |
| `cellular-local.conf`  | Broker, klient-ID og topic, uden Wi-Fi-credentials                             | committet |
| `modem-uart1.overlay`  | Modemet på UART1, GPIO18 og GPIO19                                             | committet |
| `modbus-uart0.overlay` | Modbus RTU på UART0, GPIO22 TX og GPIO23 RX                                    | committet |
| `usb.overlay`          | USB-konsol, også med i modem-buildet                                           | committet |

Modbus-konfigurationen ligger i fælles `prj.conf` og `modbus-uart0.overlay`. `CMakeLists.txt` er kun Zephyr-grundstrukturen; alle variantvalg ligger i `run-build.sh`, som sender dem til `west build`. Wi-Fi-buildet får `-DEXTRA_CONF_FILE` med `wifi.conf` og `wifi-local.conf` samt USB- og Modbus-overlays. Modem-buildet får `cellular.conf` og `cellular-local.conf` i stedet samt modem-, Modbus- og USB-overlays. Scriptet stopper før build, hvis en conf-fil mangler. Tre uafhængige scripts tager varianten `wifi` eller `modem` som argument, og spørger i en menu, hvis argumentet mangler i en terminal: `run-build.sh` bygger (build-mappen er `build-wifi` for Wi-Fi og `build-cellular` for modem), `run-flash.sh` flasher det byggede firmware, og `run-monitor.sh` starter serial monitor (afslut med `Ctrl+]`; åbning af porten nulstiller kortet).

## 1.6. Build

Byg med `run-build.sh wifi` eller `run-build.sh modem`. Build-output ligger i `build-wifi/` eller `build-cellular/` i applikationen; workspace-reglen er, at build-output ikke placeres uden for applikationen. Scriptet kører `west build` med disse variantvalg, og en bar `west build` uden dem giver en Wi-Fi-build med `CHANGE_ME`-værdier:

```bash
west build -p always -b esp32c6_devkitc/esp32c6/hpcore -S espressif-flash-4M \
  -d <projekt>/build-wifi <projekt> -- \
  -DEXTRA_CONF_FILE="<projekt>/wifi.conf;<projekt>/wifi-local.conf" \
  -DEXTRA_DTC_OVERLAY_FILE="<projekt>/modbus-uart0.overlay;<projekt>/usb.overlay"
```

`-S espressif-flash-4M` er nødvendig, fordi boardet har 4 MB flash. Kontrollér den genererede `build-wifi/zephyr/.config` for MQTT TLS og Mbed TLS-symboler.

Verificeret 2026-10-28: `CMakeLists.txt` indlæser ikke længere conf-filer. Begge varianter blev bygget med `run-build.sh`, og den genererede `.config` var identisk med den fra før ændringen. Runtime er ikke kørt efter ændringen.

Senest verificeret 2026-10-27: Wi-Fi-buildet med Modbus/UART0 og USB Serial/JTAG-overlay byggede for 4 MB flash og blev flashet til ESP32-C6FH4. C6 læste register 0 = `1234` og register 1 fra slave-ESP32 over UART0 GPIO22/23 og publicerede 10 uptime-payloads med 10 sekunders interval. En `mosquitto_sub` modtog de samme 10 værdier på `iot/test`. Cellular-varianten blev tilsvarende testet med Stamp CatM på UART1 GPIO18/19 samtidig med Modbus på UART0 GPIO22/23. Sanitized konsol- og subscriberlogs ligger i workspace under `assignment-industriel-iot-projekt/ai-workspace/evidence/modbus-mqtt-integration/`. Resultatet er en syntetisk testdatavej, ikke fuld Project A acceptance.

### 1.6.1. Build med iot-wrapperen

Wrapperen `iot` understøttes ikke længere til dette projekt. Den sender kun `-DOVERLAY_CONFIG`, og `CMakeLists.txt` vælger ikke længere variant, så den giver ikke en komplet build. Brug `run-build.sh`.

## 1.7. Runtime-gates

1. Wi-Fi association og DHCP. Historisk baseline før Modbus-integrationen: IP `192.168.87.173` efter cirka 8 til 15 sekunder med én transient afbrydelse (`status=-1`) og `send failure status -5`. Den aktuelle Modbus-integration associerede efter to transient disconnects, fik IPv4 og fortsatte til SNTP/MQTT; sanitized log ligger i `assignment-industriel-iot-projekt/ai-workspace/evidence/modbus-mqtt-integration/wifi-console-sanitized-2026-10-27.log`.
2. SNTP-synkronisering før TLS. Verificeret.
3. Broker-IP kan nås på TCP `8883`. Verificeret implicit via TLS-handshake og CONNACK.
4. Korrekt CA og IP-SAN accepteres. Verificeret: CONNACK efter TLS-verifikation med CA fra `src/tls_credentials.h`.
5. MQTT CONNACK accepteres. Verificeret.
6. Publish ses i en subscriber på `CONFIG_MQTT_TOPIC`. Modbus-testen publicerer uptime-register 1 som `payload N` hvert 10. sekund; Wi-Fi og cellular blev begge observeret med 10 subscriber-modtagelser, der matchede C6-loggen.
7. Keepalive og reconnect observeres. Ikke testet i denne Modbus-integration.

Begge varianter i denne mappe har nu observeret Wi-Fi eller PPP readiness, SNTP, TLS, MQTT CONNACK og Modbus-uptime publish til Webdock, bekræftet af en subscriber. Resultaterne viser fælles højere-lagsgenbrug over Wi-Fi og cellular, men dette er stadig en test-/udviklingsvariant og tæller ikke som fuld Project A acceptance.

## 1.8. Modem-variant (Stamp CatM over PPP)

Samme firmware kan bygges til modem-varianten. Netværkslaget vælges i `src/main.c` med `#if IS_ENABLED(CONFIG_NETWORK_CELLULAR)`. TLS-, MQTT- og SNTP-koden er fælles for begge varianter.

Build-forskelle:

- Wi-Fi: `-DEXTRA_CONF_FILE="<projekt>/wifi.conf;<projekt>/wifi-local.conf"` og `-DEXTRA_DTC_OVERLAY_FILE="<projekt>/modbus-uart0.overlay;<projekt>/usb.overlay"`, ud over `prj.conf`.
- Modem: `-DEXTRA_CONF_FILE="<projekt>/cellular.conf;<projekt>/cellular-local.conf"` og `-DEXTRA_DTC_OVERLAY_FILE="<projekt>/modem-uart1.overlay;<projekt>/modbus-uart0.overlay;<projekt>/usb.overlay"`. `wifi.conf` og `wifi-local.conf` indlæses ikke, så Wi-Fi-credentials ikke kommer med i modem-buildet.

Den committede fil `cellular-local.conf` indeholder kun `CONFIG_MQTT_BROKER_ADDRESS`, `CONFIG_MQTT_CLIENT_ID` og `CONFIG_MQTT_TOPIC`. SSID og PSK står ikke i den.

Modemet ligger på `uart1` med TX på GPIO18 og RX på GPIO19. Modbus-klienten bruger fælles `uart0` på GPIO22 TX og GPIO23 RX via `modbus-uart0.overlay`; USB Serial/JTAG er fortsat konsol. `cellular.conf` sætter `CONFIG_MODEM_CMUX_WORK_BUFFER_SIZE_EXTRA=512`. Det er en testværdi. Standardbufferen gav `DLCI 1 receive buffer overrun` under TLS og MQTT.

Status pr. 2026-10-27: Cellular-varianten gav PPP med IP, TCP til brokeren, SNTP, TLS, MQTT CONNACK og 10 Modbus-uptime-publish i træk, cirka 10 sekunder fra hinanden. Subscriber modtog de samme 10 værdier. Wi-Fi-varianten bestod tilsvarende med 10 Modbus/MQTT-værdier. Det er én integreret testkørsel pr. netværksvariant; keepalive/reconnect og længerevarende stabilitet er fortsat ikke testet.

### 1.8.1. Opstartsoprydning af modemet

Modemet nulstilles ikke, når ESP32-C6 nulstilles. Uden oprydning svarede det med `F9 F9 F9 F9` på `AT` (en CMUX-session fra forrige boot), og driverens init-script fik timeout. Efter en CMUX-lukning afviste et modem, der allerede var tilsluttet, desuden `ATD*99***1#` med `NO CARRIER`.

`main.c` rydder derfor op ved cirka 5 sekunder, før driverens init-script sender sit første `AT` 11,5 sekunder efter boot. Driveren åbner selv UART'en tidligere:

1. `modem_close_stale_cmux()` sender CMUX-lukningen `F9 03 EF 05 C3 01 F2 F9` på modemets UART og kasserer svaret. Rammen er harmløs i AT-tilstand.
2. `modem_reset_module()` sender `AT+CFUN=1,1` og venter 4 sekunder. Modemet var klar igen efter 2,5 til 2,8 sekunder i to målinger. Ventetiden er fast, fordi firmwaren i de fleste kørsler ikke kan læse modemets svar.

Metoden og målingerne bag står i `uart-probe-recovery-experiment/`. Hvis modemet alligevel er tavst, er en strømcyklus af Stamp CatM den sikre udvej. Runbooken for modemet står i `assignment-industriel-iot-projekt/ai-workspace/notes/technical/stamp-catm-modem-bringup-runbook_linux.md`.

### 1.8.2. Uret over mobilnettet

Navnet `pool.ntp.org` skal slås op, før tiden kan hentes. `sntp_simple` returnerer fejlen fra navneopslaget, så `SNTP query failed: -11` og `-101` var opslagsfejl og ikke fejl i selve tidsprotokollen. To indstillinger i `cellular.conf` retter det:

- `CONFIG_NET_L2_PPP_OPTION_DNS_USE=y` bruger den DNS-server, som mobilnettet giver under PPP. Uden den fejlede opslaget med `-11` i alle kørsler.
- `CONFIG_NET_SOCKETS_DNS_TIMEOUT=10000` giver opslaget 10 sekunder. Med standardværdien 2000 fejlede opslaget med `-101` efter 2,16 sekunder i tre af seks kørsler. Med 10000 satte alle seks kørsler uret, og opslag og forespørgsel tog 0,5 til 5,3 sekunder.

Firmwaren logger nu tiden for opslag og forespørgsel (`SNTP name lookup and query took ... ms`) og ved fejl de DNS-servere, resolveren har.

### 1.8.3. Kontrol af certifikatets datoer

Før stod `CONFIG_MBEDTLS_HAVE_TIME_DATE` slået fra, og TLS kontrollerede da ikke certifikatets start- og udløbsdato. Det forklarede, at MQTT CONNACK blev accepteret, selv om uret ikke var sat. Zephyrs egen CMake-advarsel nævner samme risiko. Modem-buildet har indstillingen slået til i `cellular.conf`, og det er målt:

- Uret sat med SNTP: CONNACK i alle seks kørsler (tre med testkonfigurationen og tre med den endelige).
- Uret ikke sat (tidsserver, der ikke kan slås op, via `CONFIG_MQTT_SNTP_SERVER="unresolvable.invalid"`): TLS afviser med `-0x2700` (`X509_CERT_VERIFY_FAILED`), og der kommer ingen CONNACK. Den præcise årsag logges ikke; at det er datokontrollen, følger af, at samme opsætning uden datokontrol gav CONNACK.

Fordi et forkert ur nu stopper TLS, forsøger hovedløkken at sætte uret igen, før den prøver MQTT, og den starter ikke TLS, så længe uret ikke er sat. `CONFIG_MQTT_SNTP_SERVER` vælger tidsserveren, med `pool.ntp.org` som standard.

## 1.9. NDATA batching (test/power-batching)

Denne branch tilføjer simpel applikations-batching af Modbus-samples før MQTT-publish, som en power-savings test på samme Sparkplug B JSON-flow som `main`.

### Konfiguration

- `CONFIG_APP_BATCH_SIZE` i `modem_wifi_tls_mqtt_project/Kconfig`: antal samples pr. NDATA-besked.
- Default: `5`. Range: `1..30`.
- `APP_BATCH_SIZE=1` sender én besked pr. sample, stadig i array-format med længde 1.

Eksempel i en conf-fil eller som build-override:

```conf
CONFIG_APP_BATCH_SIZE=5
```

### Opførsel

- Firmwaren læser Modbus hvert `MODBUS_PUBLISH_INTERVAL_MILLISECONDS` (5000 ms) og gemmer et sample i en RAM-batch: `timestamp_ms`, `adc_millivolts`, `uptime_seconds`.
- Når `batch_count == APP_BATCH_SIZE`, sendes én Sparkplug B NDATA til `spBv1.0/UCL/NDATA/<CONFIG_MQTT_CLIENT_ID>` med QoS 0.
- `seq` tælles kun op ved vellykket batch-publish. NBIRTH/NDEATH/bdSeq/reconnect-flow er uændret.
- Nyeste data prioriteres: hvis en batch-publish fejler, payload formateres/trunkeres, eller en fuld usendt batch ville blokere et nyt sample, droppes den gamle batch (`batch_count=0`), og nye samples samles. Der forsøges ikke at gensende gamle data.
- Modbus-læsefejl bevarer den delvise batch.

### Payload-format (format A, parallelle arrays)

Top-level `timestamp` er timestamp for sidste sample i batchen. Metrikkerne er parallelle arrays med samme længde og indeks:

```json
{
  "timestamp": 1710000000000,
  "device_id": "esp32c6-wifi-tls",
  "seq": 7,
  "metrics": [
    {"name": "sample_timestamps", "value": [1709999980000, 1709999985000, 1709999990000, 1709999995000, 1710000000000]},
    {"name": "adc_millivolts", "value": [3150, 3148, 3151, 3149, 3150]},
    {"name": "uptime_seconds", "value": [100, 105, 110, 115, 120]}
  ]
}
```

### Build og test

Byg fra Zephyr workspace med aktiveret venv (samme variantvalg som ellers):

```bash
cd ~/zephyrproject
source .venv/bin/activate
west build -p always -b esp32c6_devkitc/esp32c6/hpcore -S espressif-flash-4M \
  -d /home/rasmus/Desktop/Zephyr/modem_wifi_tls_mqtt_project/build-wifi \
  /home/rasmus/Desktop/Zephyr/modem_wifi_tls_mqtt_project -- \
  "-DEXTRA_CONF_FILE=/home/rasmus/Desktop/Zephyr/modem_wifi_tls_mqtt_project/wifi.conf;/home/rasmus/Desktop/Zephyr/modem_wifi_tls_mqtt_project/wifi-local.conf" \
  "-DEXTRA_DTC_OVERLAY_FILE=/home/rasmus/Desktop/Zephyr/modem_wifi_tls_mqtt_project/modbus-uart0.overlay;/home/rasmus/Desktop/Zephyr/modem_wifi_tls_mqtt_project/usb.overlay"
```

For modem/cellular bruges `build-cellular`, `cellular.conf;cellular-local.conf` og `modem-uart1.overlay;modbus-uart0.overlay;usb.overlay`.

Forventet log ved batching:

```text
<inf> MAIN: Modbus sample: adc = 3150 mV, uptime = 120 s (1/5)
...
<inf> MAIN: Modbus sample: adc = 3150 mV, uptime = 140 s (5/5)
<inf> mqtt_transport: Publishing to spBv1.0/UCL/NDATA/... (len=..., qos=0)
<inf> MAIN: Published batched NDATA with 5 samples
```

Verificering: `build-wifi` med `APP_BATCH_SIZE=5` er bygget/flashet og viste flere på hinanden følgende 5-sample batches over Wi-Fi/MQTT. `build-cellular` med `APP_BATCH_SIZE=5` er bygget; end-to-end modem-batching blev ikke verificeret i denne session pga. ustabil modem-registrering/port. Payload-indhold på broker og PPK2-målinger er ikke verificeret her.

## 1.10. Åbne punkter

- Modem: Navneopslag over PPP tog 3,6 til 5,3 sekunder i fire af seks kørsler. Årsagen til de langsomme svar er ikke undersøgt.
- Datokontrollen er afprøvet med et ur, der ikke er sat, men ikke med et certifikat, der rent faktisk er udløbet. Wi-Fi-varianten har ikke fået datokontrollen slået til.
- Det længste navneopslag plus forespørgsel hidtil tog 10,5 sekunder, tæt på DNS-timeouten på 10 sekunder. Hæv timeouten, hvis opslag begynder at fejle.
- Modem: Opstartsoprydningen er set virke i seks kørsler i træk. Gentag over flere kørsler, også efter længere tid med modemet strømsat, før den regnes som stabil.
- Modem: Firmwaren kan ikke læse modemets svar på `uart1`, før driveren starter, så ventetiden efter `AT+CFUN=1,1` er fast. Årsagen er ikke undersøgt.
- Modem: Enkelte kørsler viser `periodic_chat_script: timed out` fra driveren (2 af 6 kørsler med opstartsoprydning). Sammenhæng med nulstillingen er ikke undersøgt.
- Keepalive er aktiv via Zephyrs default `CONFIG_MQTT_KEEPALIVE=60`, og `main.c` kalder `mqtt_live()`, når `poll` timer ud. Gate 7 mangler stadig en observation af PINGREQ/PINGRESP og reconnect mod brokeren.
- QoS 0-publish giver kun lokal accept. Broker-levering skal ses i en subscriber.
- Batching: broker-payload, PPK2-strømmåling og end-to-end modem/cellular med batching er ikke verificeret.
- Runbooken for Wi-Fi/TLS-routen ligger i workspace (`wifi-tls-reproduction-runbook_linux.md`), og ikke i projektmappen.
