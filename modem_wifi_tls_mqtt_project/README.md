<!-- _Formål: Dokumentere det Zephyr-projekt, der sender Modbus-data som Sparkplug B (nanopb) over MQTT/TLS via Wi-Fi eller Stamp CatM. -->

# 1. wifi-tls-no-sparkplugB

## 1.1. 📋 Indholdsfortegnelse

- [1. wifi-tls-no-sparkplugB](#1-wifi-tls-no-sparkplugb)
  - [1.1. 📋 Indholdsfortegnelse](#11--indholdsfortegnelse)
  - [1.2. Formål og scope](#12-formål-og-scope)
  - [1.3. Pipeline](#13-pipeline)
    - [1.3.1. Sparkplug B](#131-sparkplug-b)
    - [1.3.2. Strømmålinger med PPK2 (test 2)](#132-strømmålinger-med-ppk2-test-2)
  - [1.4. Lokal konfiguration](#14-lokal-konfiguration)
  - [1.5. Filer til hver version](#15-filer-til-hver-version)
  - [1.6. Build](#16-build)
    - [1.6.1. Build med iot-wrapperen](#161-build-med-iot-wrapperen)
  - [1.7. Runtime-gates](#17-runtime-gates)
  - [1.8. Modem-variant (Stamp CatM over PPP)](#18-modem-variant-stamp-catm-over-ppp)
    - [1.8.1. Opstartsoprydning af modemet](#181-opstartsoprydning-af-modemet)
    - [1.8.2. Uret over mobilnettet](#182-uret-over-mobilnettet)
    - [1.8.3. Kontrol af certifikatets datoer](#183-kontrol-af-certifikatets-datoer)
  - [1.9. Åbne punkter](#19-åbne-punkter)

> **Sparkplug B (2026-11-12):** Firmwaren sender nu Sparkplug B-protobuf (nanopb) i stedet for `payload N` på `CONFIG_MQTT_TOPIC`. Se [1.3.1](#131-sparkplug-b). Afsnittene nedenfor beskriver de tidligere tests uden Sparkplug B.

Dette er en ESP32-C6/Zephyr test- og udviklingsapplikation til Wi-Fi eller Stamp CatM/cellular og MQTT over TLS uden SparkplugB. Begge netværksvarianter deler TLS-, MQTT- og Modbus-koden. Projektet er samme TLS- og MQTT-spor som `wifi-tls-with-sparkplugB`, men uden protobuf, nanopb, birth/death-payloads og will-message. Det er ikke den færdige Project A-firmware eller fuld cellular acceptance.

## 1.2. Formål og scope

Projektet er et trin, før SparkplugB eventuelt tages med. Det skal vise, at Wi-Fi, SNTP, TLS 1.2 med IP-SAN og MQTT CONNACK fungerer, og at der kan publiceres en almindelig MQTT-besked. Det har ingen SparkplugB-kode, ingen `proto/` og ingen `tools/`-consumer.

Den testede SparkplugB-version ligger i `zephyr-projects/_iot-project-tests/wifi-tls-with-sparkplugB/`. Dens TLS- og MQTT-logik er den samme som her, men `src/main.c` der har birth/death-håndtering og will-message.

## 1.3. Pipeline

```text
Wi-Fi
    -> DHCP
    -> SNTP mod pool.ntp.org, så certifikatet kan valideres
    -> TCP 8883
    -> TLS 1.2 med public CA og IP-SAN, uden SNI
    -> MQTT 3.1.1 CONNECT og CONNACK
    -> MQTT CONNECT med NDEATH som Last Will, SUBSCRIBE til STATE, NCMD og DCMD
    -> venter på STATE {"online": true} fra red_01
    -> NBIRTH (og DBIRTH, når Modbus-slaven har svaret)
    -> Modbus RTU register 1 læses hvert Config/SampleInterval_s (standard 10 s)
    -> QoS 0 DDATA med 3 målinger ad gangen, hver med eget tidsstempel
```

### 1.3.1. Sparkplug B

Topics og beskedtyper følger afsnittet "Sparkplug B i Projekt A" i rapporten. ID'erne er Kconfig-værdier (`CONFIG_SPARKPLUG_GROUP_ID`, `..._EDGE_NODE_ID`, `..._DEVICE_ID`):

| Type   | Topic                                     | Indhold                                                           |
| ------ | ----------------------------------------- | ----------------------------------------------------------------- |
| NBIRTH | `spBv1.0/gruppe_4/NBIRTH/c6-01`           | `bdSeq`, `Node Control/Rebirth`, `Config/SampleInterval_s` (alias 4) |
| NDATA  | `spBv1.0/gruppe_4/NDATA/c6-01`            | Nyt måleinterval efter en NCMD (alias 4)                          |
| NDEATH | `spBv1.0/gruppe_4/NDEATH/c6-01`           | `bdSeq`; registreret som Last Will (QoS 1, ingen retain)          |
| NCMD   | `spBv1.0/gruppe_4/NCMD/c6-01`             | Modtages: `Node Control/Rebirth` og `Config/SampleInterval_s` (1-3600) |
| DBIRTH | `spBv1.0/gruppe_4/DBIRTH/c6-01/wroom_01`  | `Status/Uptime_s` (alias 10, UInt16) fra Modbus-slaven            |
| DDATA  | `spBv1.0/gruppe_4/DDATA/c6-01/wroom_01`   | `CONFIG_SPARKPLUG_BATCH_SIZE` målinger med alias og tidsstempel   |
| DDEATH | `spBv1.0/gruppe_4/DDEATH/c6-01/wroom_01`  | Sendes, når en Modbus-læsning fejler; næste gode læsning giver ny DBIRTH |
| DCMD   | `spBv1.0/gruppe_4/DCMD/c6-01/#`           | Abonneres, men ignoreres: enheden har ingen skrivbare metrics endnu |
| STATE  | `spBv1.0/STATE/red_01`                    | Modtages (JSON): NBIRTH sendes først ved `"online": true`; ved `"online": false` sender noden NDEATH og forbinder igen |

Host-ID'et er `CONFIG_SPARKPLUG_PRIMARY_HOST_ID` (standard `red_01`). Node-RED skal derfor publicere `{"online": true, "timestamp": ...}` med retain på `spBv1.0/STATE/red_01`, ellers sender noden aldrig NBIRTH. En tom værdi slår ventetiden fra. STATE-beskeder med ældre `timestamp` end den senest modtagne ignoreres.

`bdSeq` tælles op ved hver ny MQTT-forbindelse og står både i NDEATH-willen og i NBIRTH. `seq` starter på 0 i NBIRTH og tælles op for hver efterfølgende besked (0-255). Alias 1-3 er reserveret til `Process/Temperature`, `Status/CellularRSSI` og `Status/SupplyVoltage` fra rapporten, når de målinger findes.

Payloads kodes med nanopb ud fra `proto/sparkplug_b.proto`. Filen er en slanket udgave af Eclipse Tahus `sparkplug_b.proto` med samme feltnumre, så Node-RED/Tahu kan afkode dem; ubrugte felter (DataSet, Template, properties m.m.) er udeladt, og `proto/sparkplug_b.options` giver faste størrelser i stedet for callbacks. Målte størrelser: NBIRTH 104 B, DBIRTH 41 B, DDATA med 3 målinger 49 B, NDEATH 20 B.

Koden ligger i `src/sparkplug_payload.c` (protobuf og topics) og `src/sparkplug_node.c` (Sparkplug-forløbet). `CONFIG_MQTT_TOPIC` bruges ikke længere, men findes stadig, så eksisterende `wifi-local.conf`-filer bygger.

Verificeret 2026-11-12: Begge varianter bygger uden advarsler mod Zephyr `main` 1cacc749 (de modulversioner, der ligger i workspace). Beskederne er afkodet med Tahus officielle proto i Python, og en NCMD lavet af Python-protobuf med ekstra felter blev læst korrekt. STATE-forløbet (vent på online, ignorér ældre STATE, NDEATH og disconnect ved offline, ny NBIRTH med næste bdSeq) er testet på PC med falsk MQTT-transport. Ikke kørt på hardware eller mod brokeren endnu.

Åbne punkter for Sparkplug B:

- `bdSeq` starter på 0 ved hver opstart. Spec'en anbefaler at gemme den i flash, så en host kan skelne en genstart fra den forrige forbindelse.
- Rapporten skriver både `red-01` og `red_01` for host-applikationen; firmwaren bruger `red_01`, så tabellen "Navne og ID'er" i rapporten bør rettes.
- Målinger fra forbindelsestab til ny NBIRTH/DBIRTH kasseres; DBIRTH sender den seneste værdi.
- Én fejlet Modbus-læsning giver DDEATH. Hvis slaven ofte svarer for sent, bør der kræves flere fejl i træk.

Firmwaren forbinder til brokeren i den adresse, der står i `CONFIG_MQTT_BROKER_ADDRESS`. Den offentlige CA i `src/tls_credentials.h` er identisk med CA'en i `wifi-tls-with-sparkplugB`, og den aktive konfiguration i `wifi-local.conf` peger på Webdock-brokeren `92.113.144.124`.

Den lokale Linux-Mosquitto-route med lokal CA og broker-IP `192.168.87.119` beskrives i runbooken `wifi-tls-reproduction-runbook_linux.md` i workspace (`assignment-industriel-iot-projekt/ai-workspace/notes/technical/`). Den route passer ikke til den CA, som ligger i firmwaren, så de to routes skal ikke blandes sammen.

### 1.3.2. Strømmålinger med PPK2 (test 2)

Trinene fra test 2 i `Testplan_stroemforbrug_PPK2.docx` slås til med conf-filerne i `power/`, som `run-build*.sh` lægger oven i hinanden. Samme kode giver derfor både baseline og alle trin:

| Variant    | Indhold                                                           | Conf-filer oven i wifi/modem               |
| ---------- | ----------------------------------------------------------------- | ------------------------------------------ |
| `wifi-1`   | Baseline (test 1) med PPK2-markører og USB-konsol                 | `ppk.conf`                                 |
| `wifi-2a`  | Logging, konsol, printk og boot-banner fra; uden `usb.overlay`    | + `2a-lowpower.conf`                       |
| `wifi-2b`  | Wi-Fi power save (`WIFI_PS_MAX_MODEM`, listen interval 3)         | + `2b-wifi-ps.conf`                        |
| `wifi-2c`  | Poll indtil næste måling (maks. 10 s) og `CONFIG_PM` light sleep  | + `2c-pm.conf`                             |
| `modem-1`  | Baseline for modem med AT-shell til at tjekke PSM manuelt         | `ppk.conf`                                 |
| `modem-2a` | Som 2a, plus shell og AT-shell fra                                | + `2a-lowpower.conf`, `2a-lowpower-modem.conf` |
| `modem-2d` | `AT+CPSMS` og `AT+CEDRXS` sendes efter modem-reset               | + `2d-modem-psm.conf`                      |

Byg med fx `./run-build-Rasmus.sh wifi-2b` og flash med `./run-flash-Rasmus.sh wifi-2b`; build-mappen hedder `build-<variant>`.

- `ppk.conf` slår GPIO-markørerne til og sætter `CONFIG_SPARKPLUG_BATCH_SIZE=1` og 10 s interval, så der kommer én DDATA pr. Modbus-måling (30 pr. 5 min) som i testplanen. Batching hører til test 3.
- Markører: GPIO2 → PPK2 D0 er høj under Modbus-læsningen, GPIO3 → D1 under hver MQTT-publish (første puls er NBIRTH). En 1 s puls på D1 efter IPv4 betyder, at power save ikke kunne slås til.
- Med `CONFIG_PM` blokeres light sleep, mens et Modbus-svar ventes, så UART0 ikke mister det.
- 2d: Modemets svar kan ikke læses før driveren starter. Tjek med `at AT+CPSMS?` og `at AT+CEDRXS?` i en `modem-1`-build, hvilke timere netværket giver.
- Beskederne er nu Sparkplug B (binære). Tæl dem med `mosquitto_sub ... -t 'spBv1.0/gruppe_4/DDATA/c6-01/#' -F '%I %t %l'`. Node-RED skal publicere STATE online for `red_01`, ellers sendes der intet.

Verificeret 2026-11-12: Alle syv varianter bygger mod Zephyr `main` 1cacc749, og `.config` viser de forventede valg pr. trin. Wi-Fi-varianterne blev bygget uden Espressifs Wi-Fi-blobs (de mangler på build-PC'en), så ESP32-Wi-Fi-driveren var ikke linket ind; power save-kaldet går gennem Zephyrs generiske `NET_REQUEST_WIFI_PS`, som driveren implementerer (`esp32_wifi_set_power_save`). Ikke kørt på hardware.

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

## 1.9. Åbne punkter

- Modem: Navneopslag over PPP tog 3,6 til 5,3 sekunder i fire af seks kørsler. Årsagen til de langsomme svar er ikke undersøgt.
- Datokontrollen er afprøvet med et ur, der ikke er sat, men ikke med et certifikat, der rent faktisk er udløbet. Wi-Fi-varianten har ikke fået datokontrollen slået til.
- Det længste navneopslag plus forespørgsel hidtil tog 10,5 sekunder, tæt på DNS-timeouten på 10 sekunder. Hæv timeouten, hvis opslag begynder at fejle.
- Modem: Opstartsoprydningen er set virke i seks kørsler i træk. Gentag over flere kørsler, også efter længere tid med modemet strømsat, før den regnes som stabil.
- Modem: Firmwaren kan ikke læse modemets svar på `uart1`, før driveren starter, så ventetiden efter `AT+CFUN=1,1` er fast. Årsagen er ikke undersøgt.
- Modem: Enkelte kørsler viser `periodic_chat_script: timed out` fra driveren (2 af 6 kørsler med opstartsoprydning). Sammenhæng med nulstillingen er ikke undersøgt.
- Keepalive er aktiv via Zephyrs default `CONFIG_MQTT_KEEPALIVE=60`, og `main.c` kalder `mqtt_live()`, når `poll` timer ud. Gate 7 mangler stadig en observation af PINGREQ/PINGRESP og reconnect mod brokeren.
- QoS 0-publish giver kun lokal accept. Broker-levering skal ses i en subscriber.
- Runbooken for Wi-Fi/TLS-routen ligger i workspace (`wifi-tls-reproduction-runbook_linux.md`), og ikke i projektmappen.
