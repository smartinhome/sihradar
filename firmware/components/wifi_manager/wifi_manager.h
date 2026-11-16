#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_AP_MODE,
} wifi_state_t;

// Proba polaczenia z nowa siecia uruchomiona z trybu AP (z panelu).
typedef enum {
    WIFI_TEST_IDLE,
    WIFI_TEST_RUNNING,
    WIFI_TEST_OK,       // polaczono, dane zapisane, za chwile restart do pracy w sieci
    WIFI_TEST_FAIL,
} wifi_test_t;

void         wifi_manager_init(void);
wifi_state_t wifi_manager_get_state(void);
wifi_test_t  wifi_manager_test_state(void);

// Ilu klientow jest podlaczonych do naszego AP (0 = nikt).
int          wifi_manager_ap_clients(void);
int          wifi_manager_get_rssi(void);
// Kanal sieci domowej (0 = brak polaczenia).
int          wifi_manager_get_channel(void);
void         wifi_manager_get_ip(char *buf, size_t len);
// Nazwa w sieci: "sih-radar-1a2b" (panel: http://sih-radar-1a2b.local)
const char  *wifi_manager_hostname(void);
// Nazwa punktu dostepowego: "SIH-Radar-1A2B"
const char  *wifi_manager_ap_ssid(void);

// Skanowanie sieci. Wynik jako JSON: [{"ssid":"...","rssi":-60,"auth":3}, ...]
// Zwraca liczbe sieci albo -1 przy bledzie.
int          wifi_manager_scan(char *json_buf, int cap);

// Polaczenie z siecia z panelu.
// Tryb AP: proba na zywo przy dzialajacym AP - panel sledzi ja przez
// wifi_manager_test_json. Po sukcesie zapis i restart po kilkunastu sekundach,
// zeby przegladarka zdazyla pokazac nowy adres.
// Tryb STA: zapis i restart (zmiana sieci domowej).
// Zwraca NULL albo komunikat bledu dla panelu.
const char  *wifi_manager_connect(const char *ssid, const char *pass);
int          wifi_manager_test_json(char *buf, int cap);
