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
