#pragma once
#include "presence_core.h"

// Obecnosc w pokoju i w strefach - liczona w module 10 razy na sekunde,
// takze gdy panel jest zamkniety. Strefy i ustawienia w NVS.
void presence_init(void);

// Stan na zywo (obiekt JSON bez nazwy): {"occ":..,"latch":..,"n":..,"held":..,
// "z":[[occ,latch,n],...],"ts":[stan celu 1..3],"tz":[bity stref celu 1..3]}
int presence_json(char *buf, int cap);

// Ustawienia i strefy: {"cfg":{...},"zones":[{...}]}
int presence_zones_json(char *buf, int cap);

// Zapis calosci (strefy podane w kolejnosci, n <= PZ_MAX_ZONES).
// NULL = zapisano, inaczej komunikat dla panelu.
const char *presence_save(const pz_cfg_t *cfg, const pz_zone_t *zones, int n);
pz_cfg_t presence_get_cfg(void);
