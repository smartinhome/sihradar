#pragma once
#include <stdint.h>

// Serwer DNS portalu konfiguracyjnego: w trybie AP na kazde zapytanie typu A
// odpowiada adresem modulu. Telefon po podlaczeniu do SIH-Radar-XXXX sam
// otwiera wtedy panel ("Zaloguj sie do sieci").
// ip - adres AP w kolejnosci sieciowej (jak esp_ip4_addr_t.addr).
void captive_dns_start(uint32_t ip);
