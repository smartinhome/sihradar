# SIH Radar – rozpoznanie (2026-11-03)

Czujnik obecności mmWave na ESP32 z Matter/Zigbee i konfiguracją w przeglądarce:
projekt pomieszczenia, meble, radar (pozycja, zasięg, kąt), strefy detekcji.

---

## 1. Radar

### HLK-LD2405
Nie znalazłem publicznej karty katalogowej ani protokołu dla LD2405. Ani Hi-Link, ani
ESPHome, ani sklepy go nie opisują. Możliwe, że chodzi o **LD2450** (popularny radar
śledzący). Do mapy pomieszczenia potrzebny jest radar podający **współrzędne X/Y
celów**. Same radary obecności (LD2410/LD2420) tego nie robią.

### Porównanie kandydatów

| Moduł | Pasmo | Cele | Współrzędne | Pole widzenia / zasięg | Montaż | ESPHome | Uwagi |
|---|---|---|---|---|---|---|---|
| **LD2450** | 24 GHz | 3 | X/Y + prędkość | ±60°, 6 m | ściana | natywny `ld2450` | tani i dojrzały; słabo trzyma osoby nieruchome |
| **LD2460** | 24 GHz, 2T4R, nowy chip | 3 statycznie / 5 w ruchu | X/Y co 0,1 m | 120°×90°; ruch ≥6 m, stat. ≥5 m; ±5°, ±0,3 m | **ściana lub sufit** (sufit: 4 m, 360°) | zewn. komponent (smarthomeshop/ld2460, ciriousjoker/esphome_ld2460) | 5 V / ≤250 mA, UART 115200, OTA modułu; lepszy od LD2450 przy osobach nieruchomych |
| **LD6002B** | 60 GHz | 3 | **X/Y/Z** | brak danych o zasięgu | sufit/ściana | natywny `ld6002b` | 4 obszary + obszary zakłóceń zapisywane w module; brak pinu reset |
| **LD6004** | 60 GHz (ADT6101P), 2T2R | 3 | **X/Y/Z** | ±60° poziomo i pionowo | sufit/ściana | brak (lib Arduino swherdman/HLK-LD6004) | 25×31,5 mm |
| LD2410C / DFRobot SEN0609 | 24 GHz | – | tylko odległość | do 25 m (SEN0609) | – | tak | radar „statyczny” jako dodatek do trackingu |

**Rekomendacja:** jako główny radar **HLK-LD2460**. Jest najnowszy z serii 24 GHz,
śledzi do 5 celów, ma dokładność ±0,3 m i ±5°, działa na ścianie i suficie, a do
ESPHome jest już działający komponent. Firmware warto zbudować z warstwą sterownika
radaru (`radar_driver`: LD2450 / LD2460 / LD6002B), tak żeby UI i strefy nie zależały
od modułu. Wariant „Pro” na suficie to 60 GHz (LD6002B/LD6004) ze współrzędną Z
(wykrywanie leżenia i siedzenia).

Do sprawdzenia: wszystkie te radary dają „duchy” od luster, metalowych szaf, wentylatorów
i firanek. Potrzebne są strefy wykluczeń i filtrowanie (opis niżej).

---

## 2. Mikrokontroler ESP32

Natywny Zigbee lub Thread (a więc Matter over Thread) wymaga radia IEEE 802.15.4.
Mają je tylko **ESP32-C6, ESP32-C5 i ESP32-H2**. ESP32-C3 i ESP32-S3 go nie mają.

| SoC | Wi-Fi | 802.15.4 (Zigbee/Thread) | CPU / RAM | Mały moduł / płytka | Dojrzałość |
|---|---|---|---|---|---|
| **ESP32-C6** | Wi-Fi 6, 2,4 GHz | tak | RISC-V 160 MHz, 512 KB SRAM, bez PSRAM | ESP32-C6-MINI-1; **XIAO ESP32-C6 21×17,8 mm** | najlepsza: esp-zigbee-sdk, esp-matter, ESPHome Zigbee (2026.5+) |
| **ESP32-C5** | **Wi-Fi 6, 2,4 + 5 GHz** | tak (Zigbee 3.0, Thread) | RISC-V 240 MHz, obsługa PSRAM | ESP32-C5-MINI-1, XIAO ESP32-C5 (od 01.2026) | nowszy: ESP-IDF ≥5.5, esp-zigbee-sdk ma dokumentację dla C5; ESPHome Zigbee na C5 jeszcze niepotwierdzony |
| ESP32-H2 | **brak** | tak | 96 MHz | H2-MINI-1 13,2×16,6 mm | odpada: bez Wi-Fi nie ma UI w przeglądarce |

**Rekomendacja:**
- **Prototyp i v1: ESP32-C6.** Na start XIAO ESP32-C6 (mały, z anteną i złączem U.FL),
  docelowo własna płytka z **ESP32-C6-MINI-1**. Na C6 wszystkie stosy (Wi-Fi + Zigbee
  + Matter) są dziś sprawdzone.
- **Do równoległego testu: ESP32-C5.** Wi-Fi może pracować w paśmie 5 GHz, a Zigbee
  zostaje w 2,4 GHz. Wtedy znika główny problem C6, czyli współdzielenie anteny i czasu
  radia między Wi-Fi a 802.15.4. C5 ma też szybszy CPU i obsługuje PSRAM (wariant
  N16R8), co pomaga przy UI, Matter i historii ruchu. Jeśli testy na C5 wyjdą dobrze,
  to rozsądny cel dla v2.

### Ważne ograniczenia protokołów
- Radio 802.15.4 obsługuje naraz jeden stos: **Zigbee albo Thread**. Wynikają z tego
  tryby firmware wybierane w UI (z restartem):
  1. Wi-Fi + Matter over Wi-Fi (+ opcjonalnie MQTT/HA API)
  2. Wi-Fi + Zigbee (end device). Wi-Fi zostaje dla UI.
  3. Matter over Thread. Wtedy UI tylko w trybie konfiguracji (AP lub BLE) albo przez Wi-Fi z koegzystencją.
- Zigbee na C6 działa stabilnie jako **end device**. Jako router społeczność zgłasza
  niestabilność przy włączonym Wi-Fi.
- Ani Matter, ani Zigbee nie przeniosą mapy ani współrzędnych celów. Na zewnątrz idą
  **strefy jako osobne czujniki zajętości** (Occupancy Sensing, jeden endpoint na
  strefę) plus liczba osób. Współrzędne na żywo idą tylko przez WebSocket do UI i
  przez MQTT/HA. Dlatego **transformacja radar → pokój i logika stref muszą działać na
  ESP**, a nie w Home Assistant.
- Zasilanie: LD2460 potrzebuje 5 V / do 250 mA, więc zasilanie przez USB-C. Bateria
  odpada.

---

## 3. Przegląd istniejących projektów

| Projekt | Sprzęt | Co warto przejąć |
|---|---|---|
| **SmartHomeShop** (CeilSense, UltimateSensor) – github.com/smarthomeshop | LD2450 / LD2460 / LD6002B, ESPHome | strefy-wielokąty do 20 punktów w mm (4 detekcji + 2 wykluczeń), **linie wejścia z kierunkiem i licznik osób** zachowywany po restarcie, profile montażu (wysokość, orientacja, rzut FOV), Room Designer jako panel HA |
| **NAMO** – github.com/David2766/NAMO-aint-motion-only (demo: david2766.github.io/NAMO-aint-motion-only) | XIAO ESP32-S3 + LD2450 + LD2410C + PIR | najbliższy Twojej wizji: **aplikacja webowa na urządzeniu**, wgrany rzut piętra ze skalą, edycja pokoi, **meble (przesuwanie, obrót, skalowanie)**, dopasowanie pozycji i obrotu radaru, strefy detekcji, wykluczeń, korekcji i wyjścia, **automatyczna korekta fałszywych detekcji**, fuzja wielu urządzeń. Licencja AGPL, więc tylko jako inspiracja |
| **Everything Presence Lite/Pro** – EverythingSmartHome/everything-presence-addons | LD2450 + SEN0609 (statyczny) + PIR, PoE | hybryda radar śledzący + radar obecności + PIR; wizualny konfigurator stref (dodatek HA) |
| **Sensy-One S1 Pro / E1 Pro** | C3 + LD2450 / LD2460 + PoE | przestroga: na ESP32-C3 brakowało RAM (zrywanie połączeń). To argument za C6/C5 |
| **Aqara FP2** (komercyjny wzorzec) | 60 GHz | do 30 stref, 5 osób, około 40 m² z jednego czujnika |
| LD2450_zones (komponent ESPHome), 53l3cu5/ESP32_LD2450, nick28s/IoTProject-ZonePresenceDetection-LD2450 (React), TillFleisch/ESPHome-HLK-LD2450 | LD2450 | proste edytory stref w przeglądarce, przykłady kodu |
| Reddit (ESP32-C3 + LD2450) | – | nie udało się pobrać, Reddit blokuje automatyczny dostęp |

---

## 4. Koncepcja modułu SIH Radar

### Sprzęt (v1)
- ESP32-C6-MINI-1 (prototyp: XIAO ESP32-C6) + **HLK-LD2460** na UART (nie UART0)
- opcjonalnie LD2410C lub PIR jako potwierdzenie obecności osób nieruchomych
- BH1750/LTR-390 (światło) i SHT4x (temperatura, wilgotność): tani dodatek
- USB-C 5 V, LED statusu, przycisk reset/parowanie
- obudowa z przegubem do ściany (pochylenie 25–40° w dół, wysokość 2,2–2,7 m) i wersja sufitowa

### Firmware (ESP-IDF, jak SIHOS17)
- `radar_driver`: parser ramek LD2450 / LD2460 / LD6002B, wspólny model `Target{x,y,z,v,id}`
- `tracker`: wygładzanie (np. Kalman lub α-β), utrzymanie ID, timeout „zniknięcia”, łączenie duchów
- `room_model`: transformacja radar → pokój (pozycja, obrót, wysokość, pochylenie),
  test punkt-w-wielokącie, linie wejścia, licznik osób
- Wyjścia: WebSocket (UI na żywo, ~10 Hz), REST (konfiguracja JSON w NVS/LittleFS),
  MQTT + HA discovery, Matter (endpoint Occupancy na strefę), Zigbee (endpoint Occupancy na strefę)
- OTA (HTTP), w trybie Zigbee opcjonalnie OTA cluster

### UI w przeglądarce (SPA w LittleFS, gzip, bez CDN)
1. **Kreator pomieszczenia:** wymiary w cm, wielokąt ścian (pokoje typu L), wysokość sufitu
2. **Obiekty:** kanapa, łóżko, stół, krzesło, szafa, drzwi (z łukiem otwarcia), okno,
   wentylator, lustro, kaloryfer. Przeciąganie, obrót, zmiana rozmiaru, przyciąganie do siatki i ścian
3. **Radar:** położenie na ścianie lub suficie, kierunek, wysokość, pochylenie. Wizualizacja
   **stożka FOV (±60°) i zasięgu** z cieniowaniem spadku czułości oraz strefą martwą przy radarze
4. **Strefy:** detekcji, wykluczeń (lustro, wentylator, firanka), linie wejścia z kierunkiem, strefy
   semantyczne („łóżko” → sen, „kanapa” → siedzenie)
5. **Podgląd na żywo:** cele jako kropki z śladem ruchu, zajętość stref, liczba osób
6. **Nauka i heatmapa:** zapis punktów przez N minut pustego pokoju, potem
   **automatyczna propozycja stref wykluczeń** (pomysł z NAMO)
7. Eksport i import konfiguracji JSON, tryb offline (AP + captive portal) przy pierwszym uruchomieniu

Technologia UI: lekki framework (Preact lub Svelte) + SVG dla geometrii (łatwe
zaznaczanie i edycja) lub Canvas dla podglądu na żywo. Rozmiar po gzip ≤ 300 KB.

### Etapy
1. Prototyp: XIAO C6 + LD2460, parser, WebSocket, surowe punkty w przeglądarce
2. Edytor pokoju, radaru i stref, logika stref na ESP, MQTT/HA
3. Matter (Wi-Fi), potem Zigbee end device jako drugi tryb
4. Własna PCB (C6-MINI-1), obudowa, test równoległy na ESP32-C5
5. v2: 60 GHz (LD6002B/LD6004) na sufit, fuzja kilku radarów w jednym pokoju

---

## Źródła
- HLK-LD2460, strona producenta: https://www.hlktech.net/index.php?id=1335
- Screek o LD2460/LD2461: https://screek.io/4a/latest-radar-hlk-ld2460-improved-ld2461-supports-five-targets-uses-second-generation-chip-more-powerful-antenna-for-multiplayer-tracking-radar-more-powerful-detection-capability-compared-to-ld2450
- Przegląd radarów Hi-Link 24G: https://openelab.com/blogs/learn/guidance-for-hi-link-24g-series-radar-modules
- ESPHome LD2450: https://esphome.io/components/sensor/ld2450/
- ESPHome LD6002B: https://esphome.io/components/sensor/ld6002b/
- HLK-LD6004: https://openelab.com/products/hlk-ld6004-60g-radar-module-multi
- Lista modułów mmWave: https://smarthomescene.com/blog/the-ultimate-list-of-mmwave-radar-sensor-modules/
- SmartHomeShop: https://smarthomeshop.io/en, https://github.com/smarthomeshop/ld2450, https://github.com/smarthomeshop/ld2460
- NAMO: https://community.home-assistant.io/t/namo-on-device-floorplan-tracking-and-visual-zones-with-esp32-and-ld2450/1017317
- Everything Presence Pro: https://devices.esphome.io/devices/everything-presence-pro/
- Everything Presence Lite: https://devices.esphome.io/devices/Everything-Presence-Lite
- Sensy-One S1 Pro: https://devices.esphome.io/devices/sensy-one-s1-pro/, https://community.homey.app/t/app-sensy-one-s1/160331
- Aqara FP2: https://aqara.cn/us/product/presence-sensor-fp2/
- LD2450_zones: https://community.home-assistant.io/t/ld2450-zones-custom-ld2450-esphome-component/1025916
- Edytor stref (Hackster): https://www.hackster.io/zonex/zone-presence-detection-system-c07540
- ESP32 z Zigbee/Thread: https://esp32.co.uk/best-esp32-matter-thread-zigbee-c6-h2-h21-h4-c5/, https://esp32.atomic14.com/modules/feature/thread-zigbee/
- ESP32-C6: https://www.espressif.com/products/socs/esp32-c6
- XIAO ESP32-C6: https://heise.de/en/news/Seeed-Studio-XIAO-ESP32C6-Small-board-for-Matter-Zigbee-and-much-more-9745138.html
- ESP32-C5: https://www.espressif.com/en/products/socs/esp32-c5, https://docs.espressif.com/projects/esp-zigbee-sdk/en/release-v1.0/esp32c5/introduction.html, https://wiki.seeedstudio.com/ja/xiao_esp32c5_zigbee_idf/
- Zigbee w ESPHome i na ESP: https://community.home-assistant.io/t/whats-the-current-situation-with-esp-devices-zigbee/1012706, https://github.com/luar123/zigbee_esphome
