<!-- _Formål: Dokumentere det Zephyr-projekt, der tester Wi-Fi, TLS og almindelig MQTT uden SparkplugB mod en Linux-Mosquitto-broker. -->

# wifi-tls-no-sparkplugB

Dette er en ESP32-C6/Zephyr-applikation til Wi-Fi og MQTT over TLS uden SparkplugB. Projektet er samme TLS- og MQTT-spor som `wifi-tls-with-sparkplugB`, men uden protobuf, nanopb, birth/death-payloads og will-message. Det er et Wi-Fi-læringsspor og ikke Project A's cellular acceptance.

## Formål og scope

Projektet er et trin, før SparkplugB eventuelt tages med. Det skal vise, at Wi-Fi, SNTP, TLS 1.2 med IP-SAN og MQTT CONNACK fungerer, og at der kan publiceres en almindelig MQTT-besked. Det har ingen SparkplugB-kode, ingen `proto/` og ingen `tools/`-consumer.

Den testede SparkplugB-version ligger i `zephyr-projects/_iot-project-tests/wifi-tls-with-sparkplugB/`. Dens TLS- og MQTT-logik er den samme som her, men `src/main.c` der har birth/death-håndtering og will-message.

## Pipeline

```text
Wi-Fi
    -> DHCP
    -> SNTP mod pool.ntp.org, så certifikatet kan valideres
    -> TCP 8883
    -> TLS 1.2 med public CA og IP-SAN, uden SNI
    -> MQTT 3.1.1 CONNECT og CONNACK
    -> QoS 0 publish til CONFIG_MQTT_TOPIC, én gang pr. løkke
```

Firmwaren forbinder til brokeren i den adresse, der står i `CONFIG_MQTT_BROKER_ADDRESS`. Den offentlige CA i `src/tls_credentials.h` er identisk med CA'en i `wifi-tls-with-sparkplugB`, og den aktive konfiguration i `wifi-local.conf` peger på Webdock-brokeren `92.113.144.124`.

Den lokale Linux-Mosquitto-route med lokal CA og broker-IP `192.168.87.119` beskrives i runbooken `wifi-tls-reproduction-runbook_linux.md` i workspace (`assignment-industriel-iot-projekt/ai-workspace/notes/technical/`). Den route passer ikke til den CA, som ligger i firmwaren, så de to routes skal ikke blandes sammen.

## Lokal konfiguration

Opret eller opdatér `wifi-local.conf` lokalt med projektets faktiske værdier. Filen er git-ignoreret og indlæses automatisk af CMake som `EXTRA_CONF_FILE`:

```conf
CONFIG_WIFI_SAMPLE_SSID="..."
CONFIG_WIFI_SAMPLE_PSK="..."
CONFIG_MQTT_BROKER_ADDRESS="..."
CONFIG_MQTT_CLIENT_ID="..."
CONFIG_MQTT_TOPIC="..."
```

Firmware bruger disse værdier direkte via Zephyr Kconfig. `CONFIG_MQTT_BROKER_ADDRESS` skal være en IPv4-adresse, fordi `main.c` bruger `inet_pton`. `src/tls_credentials.h` skal indeholde den offentlige CA, der signer brokerens servercertifikat. CA-private keys, servercertifikaternes private keys og MQTT-passwords må aldrig kopieres hertil.

## Filer til hver version

Wi-Fi- og modem-versionen bruger samme kildekode og værktøjer. De adskiller sig kun i konfiguration, overlays og de lokale filer med værdier, der ikke må committes.

Skal med i git: `src/`, `Kconfig`, `CMakeLists.txt`, `prj.conf`, `README.md` og projektets scripts. Skal ikke med i git: `wifi-local.conf` og `cellular-local.conf`, fordi de indeholder broker- og netværksværdier, samt `build/` og `build-cellular/`, fordi det er genereret output.

Filer til Wi-Fi-versionen:

| Fil | Indhold | Git |
| --- | --- | --- |
| `wifi.conf` | Wi-Fi-stack og `CONFIG_NETWORK_CELLULAR=n` | committet |
| `wifi-local.conf` | SSID, adgangskode, broker, klient-ID og topic | ignoreret |
| `usb.overlay` | USB Serial/JTAG-konsol | committet |

Filer til modem-versionen:

| Fil | Indhold | Git |
| --- | --- | --- |
| `cellular.conf` | `CONFIG_NETWORK_CELLULAR=y`, PPP, DNS-timeout, datokontrol, APN og CMUX-buffer | committet |
| `cellular-local.conf` | Broker, klient-ID og topic, uden Wi-Fi-credentials | ignoreret |
| `modem-uart1.overlay` | Modemet på UART1, GPIO18 og GPIO19 | committet |
| `usb.overlay` | USB-konsol, også med i modem-buildet | committet |

Wi-Fi-buildet indlæser `wifi.conf` og `wifi-local.conf` automatisk. Modem-buildet kræver `-DOVERLAY_CONFIG` med `cellular.conf`, og så indlæses `cellular-local.conf` i stedet for `wifi-local.conf`. Scripts `run-wifi.sh` og `run-modem.sh` bruger de rigtige filer for hver version.

## Build

Byg med en lokal `build/`-mappe i applikationen. Workspace-reglen er, at build-output ikke placeres uden for applikationen:

```bash
source "$HOME/.venvs/zephyr-4.4.0/bin/activate"
cd "$HOME/Work/zephyrproject"
west build -p always -b esp32c6_devkitc/esp32c6/hpcore -S espressif-flash-4M \
  -d /home/oliver/code/zephyr-projects/_iot-project-tests/wifi-tls-no-sparkplugB/build \
  /home/oliver/code/zephyr-projects/_iot-project-tests/wifi-tls-no-sparkplugB
```

`-S espressif-flash-4M` er nødvendig, fordi boardet har 4 MB flash. Kontrollér den genererede `build/zephyr/.config` for MQTT TLS og Mbed TLS-symboler.

Senest verificeret: buildet er rent med `-p always` og `-DEXTRA_DTC_OVERLAY_FILE=usb.overlay`, som er nødvendigt for USB-konsollen, fordi CMake ikke indlæser overlayet. Flash-forbruget er 801620 B af 4194048 B (19,11 %). Buildet er flashet og kørt på ESP32-C6 mod Webdock. Resultaterne står under Runtime-gates.

### Build med iot-wrapperen

Wrapperen `iot` spørger om en config-fil og om overlays. Valgene gemmes i `.zephyr-build-config.json`, og `build-fast` bruger dem igen.

- **Config-fil:** `NONE` sender ingen `-DOVERLAY_CONFIG`. CMake vælger så Wi-Fi-mode og indlæser `wifi.conf` og `wifi-local.conf`. Vælg `cellular.conf` til modem-mode. Standardværdien i prompten er den gemte værdi, så skriv `NONE` eksplicit.
- **Overlays:** `AUTO-USB` og `AUTO-SCRIPT` tager alle `.overlay`-filer i projektmappen, herunder `modem-uart1.overlay`. `usb.overlay` udelades af `AUTO-USB`, kun når `/dev/ttyACM0` mangler. Til Wi-Fi skal overlay-værdien være stien til `usb.overlay` alene. Til modem er værdien `modem-uart1.overlay;usb.overlay`.

Det er verificeret ved at læse wrapperens kode og ved at køre dens discover-funktioner mod projektmappen. Der er ikke kørt en build med wrapperen i dette afsnit.

## Runtime-gates

1. Wi-Fi association og DHCP. Verificeret: IP `192.168.87.173` efter cirka 8 til 15 sekunder. Der er én transient afbrydelse med `status=-1` og en `send failure status -5` ved første forsøg.
2. SNTP-synkronisering før TLS. Verificeret.
3. Broker-IP kan nås på TCP `8883`. Verificeret implicit via TLS-handshake og CONNACK.
4. Korrekt CA og IP-SAN accepteres. Verificeret: CONNACK efter TLS-verifikation med CA fra `src/tls_credentials.h`.
5. MQTT CONNACK accepteres. Verificeret.
6. Publish ses i en subscriber på `CONFIG_MQTT_TOPIC`. Verificeret med `mosquitto_sub` på `iot/test`, som modtog `payload 46` til `payload 50`.
7. Keepalive og reconnect observeres. Ikke testet.

Ifølge `assignment-industriel-iot-projekt/ai-workspace/autoload/project-progress.md` er TLS, MQTT CONNACK og publish observeret i den testede Wi-Fi-version. Det gælder `wifi-tls-with-sparkplugB`, ikke denne mappe. Resultaterne er et Wi-Fi-læringsspor og tæller ikke som cellular acceptance for Projekt A.

## Modem-variant (Stamp CatM over PPP)

Samme firmware kan bygges til modem-varianten. Netværkslaget vælges i `src/main.c` med `#if IS_ENABLED(CONFIG_NETWORK_CELLULAR)`. TLS-, MQTT- og SNTP-koden er fælles for begge varianter.

Build-forskelle:

- Wi-Fi (standard): `prj.conf`, `wifi.conf` og `wifi-local.conf` indlæses. Der bruges kun `-DEXTRA_DTC_OVERLAY_FILE=usb.overlay`.
- Modem: `-DOVERLAY_CONFIG=<projekt>/cellular.conf` og `-DEXTRA_DTC_OVERLAY_FILE="<projekt>/modem-uart1.overlay;<projekt>/usb.overlay"`. `wifi.conf` og `wifi-local.conf` indlæses ikke, så Wi-Fi-credentials ikke kommer med i modem-buildet.

Den lokale, git-ignorerede fil `cellular-local.conf` indeholder kun `CONFIG_MQTT_BROKER_ADDRESS`, `CONFIG_MQTT_CLIENT_ID` og `CONFIG_MQTT_TOPIC`. SSID og PSK står ikke i den.

Modemet ligger på `uart1` med TX på GPIO18 og RX på GPIO19. `cellular.conf` sætter `CONFIG_MODEM_CMUX_WORK_BUFFER_SIZE_EXTRA=512`. Det er en testværdi. Standardbufferen gav `DLCI 1 receive buffer overrun` under TLS og MQTT.

Status pr. 2026-10-09: Modem-varianten gav PPP med IP, TCP til brokeren, MQTT CONNACK og nul CMUX-overrun i alle målte kørsler. Efter opstartsoprydningen (afsnittet nedenfor) er det set i seks kørsler i træk, hvor kun ESP32-C6 blev nulstillet, og hvor en subscriber modtog beskederne fra den nye boot. Uret sættes nu over mobilnettet i seks kørsler i træk (afsnittet om uret nedenfor), og gate 7 er ikke testet. Wi-Fi-varianten er bygget efter opdelingen, men ikke flashet efter `#if`-ændringen.

### Opstartsoprydning af modemet

Modemet nulstilles ikke, når ESP32-C6 nulstilles. Uden oprydning svarede det med `F9 F9 F9 F9` på `AT` (en CMUX-session fra forrige boot), og driverens init-script fik timeout. Efter en CMUX-lukning afviste et modem, der allerede var tilsluttet, desuden `ATD*99***1#` med `NO CARRIER`.

`main.c` rydder derfor op ved cirka 5 sekunder, før driveren åbner UART'en 11,5 sekunder efter boot:

1. `modem_close_stale_cmux()` sender CMUX-lukningen `F9 03 EF 05 C3 01 F2 F9` på modemets UART og kasserer svaret. Rammen er harmløs i AT-tilstand.
2. `modem_reset_module()` sender `AT+CFUN=1,1` og venter 4 sekunder. Modemet var klar igen efter 2,5 til 2,8 sekunder i to målinger. Ventetiden er fast, fordi firmwaren ikke kan læse modemets svar, før driveren har åbnet UART'en.

Metoden og målingerne bag står i `uart-probe-recovery-experiment/`. Hvis modemet alligevel er tavst, er en strømcyklus af Stamp CatM den sikre udvej. Runbooken for modemet står i `assignment-industriel-iot-projekt/ai-workspace/notes/technical/stamp-catm-modem-bringup-runbook_linux.md`.

### Uret over mobilnettet

Navnet `pool.ntp.org` skal slås op, før tiden kan hentes. `sntp_simple` returnerer fejlen fra navneopslaget, så `SNTP query failed: -11` og `-101` var opslagsfejl og ikke fejl i selve tidsprotokollen. To indstillinger i `cellular.conf` retter det:

- `CONFIG_NET_L2_PPP_OPTION_DNS_USE=y` bruger den DNS-server, som mobilnettet giver under PPP. Uden den fejlede opslaget med `-11` i alle kørsler.
- `CONFIG_NET_SOCKETS_DNS_TIMEOUT=10000` giver opslaget 10 sekunder. Med standardværdien 2000 fejlede opslaget med `-101` efter 2,16 sekunder i tre af seks kørsler. Med 10000 satte alle seks kørsler uret, og opslag og forespørgsel tog 0,5 til 5,3 sekunder.

Firmwaren logger nu tiden for opslag og forespørgsel (`SNTP name lookup and query took ... ms`) og ved fejl de DNS-servere, resolveren har.

### Kontrol af certifikatets datoer

Før stod `CONFIG_MBEDTLS_HAVE_TIME_DATE` slået fra, og TLS kontrollerede da ikke certifikatets start- og udløbsdato. Det forklarede, at MQTT CONNACK blev accepteret, selv om uret ikke var sat. Zephyrs egen CMake-advarsel nævner samme risiko. Modem-buildet har indstillingen slået til i `cellular.conf`, og det er målt:

- Uret sat med SNTP: CONNACK i alle seks kørsler (tre med testkonfigurationen og tre med den endelige).
- Uret ikke sat (tidsserver, der ikke kan slås op, via `CONFIG_MQTT_SNTP_SERVER="unresolvable.invalid"`): TLS afviser med `-0x2700` (`X509_CERT_VERIFY_FAILED`), og der kommer ingen CONNACK. Den præcise årsag logges ikke; at det er datokontrollen, følger af, at samme opsætning uden datokontrol gav CONNACK.

Fordi et forkert ur nu stopper TLS, forsøger hovedløkken at sætte uret igen, før den prøver MQTT, og den starter ikke TLS, så længe uret ikke er sat. `CONFIG_MQTT_SNTP_SERVER` vælger tidsserveren, med `pool.ntp.org` som standard.

## Åbne punkter

- Modem: Navneopslag over PPP tog 3,6 til 5,3 sekunder i fire af seks kørsler. Årsagen til de langsomme svar er ikke undersøgt.
- Datokontrollen er afprøvet med et ur, der ikke er sat, men ikke med et certifikat, der rent faktisk er udløbet. Wi-Fi-varianten har ikke fået datokontrollen slået til.
- Det længste navneopslag plus forespørgsel hidtil tog 10,5 sekunder, tæt på DNS-timeouten på 10 sekunder. Hæv timeouten, hvis opslag begynder at fejle.
- Modem: Opstartsoprydningen er set virke i seks kørsler i træk. Gentag over flere kørsler, også efter længere tid med modemet strømsat, før den regnes som stabil.
- Modem: Firmwaren kan ikke læse modemets svar på `uart1`, før driveren starter, så ventetiden efter `AT+CFUN=1,1` er fast. Årsagen er ikke undersøgt.
- Modem: Enkelte kørsler viser `periodic_chat_script: timed out` fra driveren (2 af 6 kørsler med opstartsoprydning). Sammenhæng med nulstillingen er ikke undersøgt.
- Keepalive er aktiv via Zephyrs default `CONFIG_MQTT_KEEPALIVE=60`, og `main.c` kalder `mqtt_live()`, når `poll` timer ud. Gate 7 mangler stadig en observation af PINGREQ/PINGRESP og reconnect mod brokeren.
- QoS 0-publish giver kun lokal accept. Broker-levering skal ses i en subscriber.
- Runbooken for Wi-Fi/TLS-routen ligger i workspace (`wifi-tls-reproduction-runbook_linux.md`), og ikke i projektmappen.
