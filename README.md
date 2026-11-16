# SIH Radar

Czujnik obecności mmWave na ESP32-C6 z panelem w przeglądarce. Firmware jest
napisany w ESP-IDF, na wzór SIHOS17.

Wersja 0.1 udostępnia przez przeglądarkę:
- podgląd osób wykrytych przez radar HLK-LD2450 (do 3 jednocześnie, pozycja,
  odległość, kąt, prędkość) z widokiem pola radaru na żywo,
- odczyty z BME688: temperaturę, wilgotność, ciśnienie i rezystancję gazu,
- konfigurację Wi-Fi: wyszukiwanie sieci, sprawdzenie hasła i nowy adres modułu,
- informacje o module i restart.

## Sprzęt

| Element | Połączenie |
|---|---|
| Waveshare ESP32-C6-Zero | – |
| HLK-LD2450 | 5V, GND, RX radaru ← **GP18**, TX radaru → **GP19** |
| BME688 | 3V3, GND, SDA **GP20**, SCL **GP21** |

Radar pracuje fabrycznie z prędkością 256000 bd. Firmware radaru 2.14 potrafi ją
zmienić po aktualizacji przez HLKRadarTool. Gdy radar milczy, SIH Radar sam sprawdza
wszystkie obsługiwane prędkości, a po znalezieniu właściwej ustawia radarowi z powrotem
256000. Podgląd łącza (prędkość, odebrane bajty, surowe dane) jest w zakładce Radar.

Szczegóły i uzasadnienie wyboru pinów są w [PINOUT.md](PINOUT.md), a rozpoznanie
projektu w [RESEARCH.md](RESEARCH.md).

## Pierwsze uruchomienie

1. Wgraj `sih-radar-full.bin` z [wydań](https://github.com/smartinhome/sihradar/releases)
   pod adres **0x0**, np. przez
   https://espressif.github.io/esptool-js/ (Chrome lub Edge, kabel USB-C).
2. Moduł bez zapisanej sieci tworzy punkt dostępowy **SIH-Radar-XXXX**
   (hasło `smartinhome`).
3. Połącz z nim telefon. Panel otworzy się sam jako okno logowania do sieci, a jeśli
   nie, wejdź na http://192.168.4.1.
4. Wybierz domową sieć (tylko 2,4 GHz) i wpisz hasło. Moduł sprawdza połączenie od
   razu i pokazuje nowy adres, np. `http://192.168.1.57` albo
   `http://sih-radar-xxxx.local`.
5. Po ok. 20 s moduł wyłącza punkt dostępowy i pracuje w sieci domowej.

Jeśli sieć domowa jest niedostępna przy starcie, moduł przechodzi w tryb punktu
dostępowego i co minutę sprawdza, czy sieć wróciła. Przytrzymanie przycisku
**BOOT przez 5 s** kasuje zapisaną sieć.

### Dioda RGB

| Kolor | Stan |
|---|---|
| zielony | połączono z siecią domową |
| niebieski | punkt dostępowy, ktoś jest podłączony |
| czerwony | punkt dostępowy bez klientów albo brak sieci |
| żółty, miga | trwa łączenie z siecią wybraną w panelu |
| biały | przycisk BOOT trzymany (po 5 s kasowanie sieci) |

## Plan pomieszczenia

Zakładka **Pomieszczenie**:
1. Wpisz wymiary dwóch ścian pokoju w metrach, np. 4,20 × 3,60.
2. Przeciągnij radar w miejsce montażu. Przy ścianie przyciąga się sam i patrzy w głąb
   pokoju, a w narożniku – na środek.
3. Kierunek zmieniasz niebieskim uchwytem, przyciskami ±15° albo wpisując kąt.

Zasięg (6 m) i kąt (±60°) radaru są rysowane w obrysie pokoju. Osoby widać na planie
na żywo, a te poza pokojem są przygaszone, bo to zwykle odbicia albo sąsiednie
pomieszczenie. Plan zapisuje się w module, więc każde urządzenie widzi ten sam.
Jeśli osoba idąca w prawo pojawia się po lewej, zaznacz **Odwróć lewo/prawo**.

## Aktualizacje

Zakładka **Aktualizacje** w panelu działa jak w SIHOS17:
- **Wersja oficjalna** to najnowsze wydanie bez bety (`vX.Y.Z`). Moduł sprawdza ją
  sam co 12 h, a nowa wersja pokazuje plakietkę „nowa” w menu. Sam niczego nie instaluje.
- **Wersja beta** to najnowsze wydanie `vX.Y.Z-betaN`. Listę wydań i zmian pobiera
  przeglądarka z API GitHuba.
- Można też podać **link do pliku .bin** (tylko https:// z github.com lub
  www.smartinhome.pl).
- Można też **wgrać plik `sih-radar.bin`** z komputera lub telefonu. To jedyna droga
  w trybie punktu dostępowego, bo moduł nie ma wtedy internetu.

Moduł przyjmuje tylko firmware SIH Radar dla ESP32-C6. Plik np. z SIHOS17 zostanie
odrzucony. Pobieranie z GitHuba wznawia się po zerwaniu połączenia. Nowa wersja
startuje „na próbę”: jeśli padnie przed pełnym uruchomieniem, bootloader wraca do
poprzedniej.

## Budowanie

Firmware buduje GitHub Actions (`.github/workflows/build.yml`, ESP-IDF v5.2.1,
target esp32c6):
- każdy push na `main` buduje firmware (artefakty są w zakładce Actions),
- tag `vX.Y.Z` lub `vX.Y.Z-betaN` dodatkowo tworzy wydanie z plikami `.bin`.

Panel `webui/index.html` jest pakowany gzipem automatycznie przy kompilacji, więc
pliku `.gz` nie trzeba odświeżać ręcznie.

## Podgląd panelu bez modułu

```bash
python tools/mock_server.py 8718
```

Otwórz http://localhost:8718. Opcja `--ap` udaje pierwsze uruchomienie. Hasło
`haslo1234` udaje poprawne połączenie z siecią, każde inne – błędne hasło.

## Testy

`tools/test_host.c` sprawdza parser ramek LD2450 (przykład z dokumentacji radaru,
urwane ramki, odczyt wersji) i obliczenia BME688. Uruchamia się je w GitHub Actions
przed kompilacją firmware.

## API

| Metoda | Adres | Opis |
|---|---|---|
| GET | `/api/status` | firmware, pamięć, Wi-Fi |
| GET | `/api/sensors` | radar i BME688 (ten sam JSON co WebSocket) |
| WS | `/ws` | dane czujników na żywo, ok. 10 razy na sekundę |
| GET | `/api/wifi/scan` | lista sieci |
| POST | `/api/wifi/connect` | `{"ssid":"…","password":"…"}` |
| GET | `/api/wifi/test` | wynik próby połączenia (tryb AP) |
| POST | `/api/reboot` | restart |
| GET | `/api/room` | plan pokoju i położenie radaru (mm, stopnie) |
| POST | `/api/room` | `{"w":4200,"l":3600,"x":2100,"y":0,"rot":0,"flip":false}` |
| POST | `/api/ota/github` | aktualizacja z GitHuba (`?channel=beta&tag=…`, bez parametrów = oficjalna) |
| POST | `/api/ota/url` | `{"url":"https://…/sih-radar.bin"}` |
| POST | `/api/ota/upload` | plik `sih-radar.bin` w treści zapytania |
| GET | `/api/ota/status` | postęp aktualizacji |
| POST | `/api/update` | `{"auto":true}` albo `{"action":"check"}` |
