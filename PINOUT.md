# SIH Radar – połączenia (Waveshare ESP32-C6-Zero + HLK-LD2450 + BME688)

Źródła: pinout i schemat Waveshare (ESP32-C6FH4, LDO ME6217 800 mA, antena ceramiczna na
dolnej krawędzi między GP5 a GP22, WS2812 na GP8, BOOT na GP9).

## Przypisanie pinów

| Urządzenie | Sygnał | Pin ESP32-C6-Zero | Uwagi |
|---|---|---|---|
| HLK-LD2450 | 5V | **5V** | zasilanie radaru |
| HLK-LD2450 | GND | **GND** | |
| HLK-LD2450 | RX | **GP18** (UART1 TX) | logika 3,3 V, 256000 bd 8N1 |
| HLK-LD2450 | TX | **GP19** (UART1 RX) | |
| BME688 | VCC/VIN | **3V3** | |
| BME688 | GND | **GND** | |
| BME688 | SDA | **GP20** | I2C, 400 kHz |
| BME688 | SCL | **GP21** | |
| BME688 | CS | 3V3 (lub zostawić, jeśli płytka ma podciąganie) | wymusza tryb I2C |
| BME688 | SDO | GND → adres 0x76, 3V3 → 0x77 | na płytkach Adafruit i Pimoroni domyślnie 0x77 |

**Wolne piny na krawędzi:** GP14 (cyfrowy), GP22, GP0–GP3 (ADC). GP0–GP3 zostawiam
na później, np. LD2410C lub PIR (GP14), przycisk albo czujnik analogowy (GP0–GP3).

## Dlaczego te piny
- UART i I2C na ESP32-C6 idą przez matrycę GPIO, więc **na każdym pinie mają tę samą
  wydajność**: 256000 bd i I2C 400 kHz nie stanowią problemu. O wyborze decyduje
  wyłącznie kompatybilność.
- **GP18–GP21** to „czyste” piny: nie pełnią funkcji strapping, nie są JTAG, UART0
  ani USB i nie są podłączone do niczego na płytce.
- Pominięte z premedytacją:
  - **TX/RX (GP16/17, UART0):** ROM wypisuje na TX log startowy. Zostawiam je jako
    zapasową konsolę do debugowania.
  - **GP4, GP5, GP15:** piny strapping (GP4 i GP5 to też JTAG MTMS/MTDI). Radar
    podłączony do nich mógłby zmienić stan przy starcie.
  - **GP22 i GP5:** leżą tuż przy antenie ceramicznej. Przewody przy antenie
    pogarszają Wi-Fi i Zigbee.
  - **GP0–GP3:** jedyne wyprowadzone na krawędź piny z ADC, warto je zachować.
  - **Pady od spodu (GP6–9, 12, 13, 23):** zgodnie z prośbą. GP8 to WS2812, GP9 to
    BOOT, a GP12 i GP13 to USB D-/D+.

## Uwagi praktyczne
- **5V radaru:** pin 5V płytki to VBUS za diodą B5819 (spadek około 0,3–0,4 V), więc
  radar dostaje około 4,6–4,7 V. W praktyce LD2450 tak pracuje, ale przy zasilaczu
  „na styk” warto to zmierzyć. Linie danych radaru mają poziom 3,3 V, więc konwerter
  poziomów nie jest potrzebny.
- **BME688 a ciepło:** ESP, LDO i radar się grzeją. BME688 umieść jak najdalej (osobna
  komora albo przewód 3–5 cm, otwory w obudowie). Inaczej temperatura będzie zawyżona,
  a odczyty gazu przekłamane.
- Przewody prowadź od strony złącza USB, z dala od dolnej krawędzi z anteną.
- **Montaż radaru pionowo, na ścianie.** Anteny LD2450 leżą w jednym rzędzie i radar mierzy
  kąt tylko w tej płaszczyźnie. Płytka ma stać pionowo, przodem do pokoju (anteny w
  poziomym rzędzie), najlepiej na wysokości ok. 1,5–2 m z lekkim pochyleniem w dół.
  Radar położony płasko (np. na suficie anteną w dół) mierzy praktycznie tylko odległość,
  a ruchu w bok nie rozróżnia.
- Podciąganie I2C: większość płytek BME688 ma rezystory 10 kΩ (lub 4,7 kΩ) do 3V3.
  Gołe czujniki wymagają rezystorów 4,7 kΩ na SDA i SCL.
- BSEC2 (IAQ, CO2eq od Bosch): przed wyborem sprawdź, czy wydanie BSEC2 ma prekompilowaną
  bibliotekę dla RISC-V ESP32-C6. Surowe odczyty (temperatura, wilgotność, ciśnienie,
  rezystancja gazu) działają bez BSEC.

## Firmware

Piny są zdefiniowane w `firmware/main/app_main.h`. Firmware (ESP-IDF, na wzór SIHOS17)
sam wykrywa adres BME688 (0x76 lub 0x77) i szuka czujnika co 10 s, więc można go
podłączyć także po włączeniu modułu.
