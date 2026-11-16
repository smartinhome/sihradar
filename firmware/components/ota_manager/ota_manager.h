#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Aktualizacja firmware jak w SIHOS17: z GitHuba (kanal oficjalny lub beta),
// z adresu URL albo z pliku .bin wgranego przez przegladarke.

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_WRITING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED,
    OTA_STATE_UPTODATE,
} ota_state_t;

typedef struct {
    ota_state_t state;
    int         progress_pct;
    char        error[64];
} ota_status_t;

void          ota_manager_init(void);
// Wywolac, gdy system wstal w calosci (Wi-Fi, serwer). Do tego momentu nowy
// obraz jest "na probe": restart wczesniej = bootloader wraca do poprzedniego.
void          ota_manager_mark_valid(void);

void          ota_start_from_url(const char *url);
// tag: najnowszy tag bety znany panelowi (NULL/"" = znajdz przez API GitHub)
void          ota_start_from_github(bool beta_channel, const char *tag);
ota_status_t  ota_get_status(void);
// Trwa pobieranie lub wgrywanie pliku - reszta modulu ma oszczedzac siec i RAM
bool          ota_busy(void);
void          ota_set_upload_busy(bool on);
const char   *ota_get_running_version(void);
const char   *ota_get_partition_label(void);

// Sprawdzanie nowej wersji oficjalnej co 12 h (bety pomijane, nic nie instaluje)
void          ota_update_check_now(void);
void          ota_update_set_auto(bool on);
int           ota_update_json(char *buf, size_t cap);   // "upd":{...} do /api/status
int           ota_version_cmp(const char *a, const char *b);  // >0: a nowsza

// Tylko https:// z dozwolonych hostow (GitHub, smartinhome.pl)
bool          ota_url_allowed(const char *url);

// Naglowek obrazu (pierwsze bajty pliku .bin): NULL = to firmware SIH Radar
// dla tego ukladu, inaczej komunikat dla panelu.
const char   *ota_check_image_header(const uint8_t *buf, size_t len);
size_t        ota_image_header_len(void);
