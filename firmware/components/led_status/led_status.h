#pragma once
#include <stdint.h>

// Dioda RGB WS2812 na plytce (GPIO8) - stan Wi-Fi:
// zielona    = polaczono z siecia domowa,
// niebieska  = tryb AP i ktos jest podlaczony (konfiguracja z telefonu),
// czerwona   = tryb AP bez klientow albo brak sieci,
// zolta      = trwa proba polaczenia z siecia wybrana w panelu,
// biala      = przycisk BOOT trzymany (po 5 s kasowanie sieci).

typedef enum {
    LED_HINT_NONE,
    LED_HINT_BUTTON,
} led_hint_t;

void led_status_init(int gpio);
void led_status_hint(led_hint_t hint);
