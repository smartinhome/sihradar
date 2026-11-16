#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NVS_NAMESPACE "sih_cfg"

// Konfiguracja zapisywana w NVS jako jeden blob.
// Nowe pola ZAWSZE na koncu (zgodnosc blobu przy aktualizacji firmware).
typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char ap_pass[32];      // haslo punktu dostepowego SIH-Radar-XXXX
} sih_config_t;

void                nvs_config_init(void);
sih_config_t        nvs_config_get(void);
const sih_config_t *nvs_config_ptr(void);
bool                nvs_config_save(const sih_config_t *cfg);
// Kasuje tylko siec domowa - po restarcie modul startuje w trybie AP.
void                nvs_config_forget_wifi(void);
