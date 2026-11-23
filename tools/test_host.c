// Testy czesci firmware bez ESP-IDF (parser LD2450, obliczenia BME688,
// logika obecnosci i stref). Kompilacja na komputerze, np.:
//   zig cc -std=c11 -Ifirmware/components/ld2450 -Ifirmware/components/bme688 \
//     -Ifirmware/components/presence \
//     tools/test_host.c firmware/components/ld2450/ld2450_proto.c \
//     firmware/components/bme688/bme68x_calc.c \
//     firmware/components/presence/presence_core.c -lm -o test_host && ./test_host
#include "ld2450_proto.h"
#include "bme68x_calc.h"
#include "presence_core.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("BLAD %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// Przyklad z instrukcji LD2450: cel 1 X=-782 mm, Y=1713 mm, -16 cm/s, rozdz. 320 mm
static const uint8_t FRAME[30] = {
    0xAA, 0xFF, 0x03, 0x00,
    0x0E, 0x03, 0xB1, 0x86, 0x10, 0x00, 0x40, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x55, 0xCC,
};

static int feed_all(ld2450_parser_t *p, const uint8_t *b, int n, int *data, int *ack) {
    int last = -1;
    for (int i = 0; i < n; i++) {
        ld2450_frame_t f = ld2450_parser_feed(p, b[i]);
        if (f == LD2450_FRAME_DATA) { (*data)++; last = i; }
        if (f == LD2450_FRAME_ACK)  { (*ack)++;  last = i; }
    }
    return last;
}

static void test_ld2450(void) {
    ld2450_target_t t[3];
    ld2450_decode_targets(FRAME, t);
    CHECK(t[0].valid);
    CHECK(t[0].x_mm == -782);
    CHECK(t[0].y_mm == 1713);
    CHECK(t[0].speed_cms == -16);
    CHECK(t[0].res_mm == 320);
    CHECK(!t[1].valid && !t[2].valid);

    // Strumien: smieci, ramka, urwana ramka, smieci z bajtem AA, ramka, odpowiedz z wersja
    static const uint8_t ACK_FW[22] = {
        0xFD, 0xFC, 0xFB, 0xFA, 0x0C, 0x00, 0xA0, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x02, 0x01, 0x16, 0x24, 0x06, 0x22, 0x04, 0x03, 0x02, 0x01,
    };
    uint8_t s[256];
    int n = 0;
    s[n++] = 0x13; s[n++] = 0xAA; s[n++] = 0x00; s[n++] = 0xFD;
    memcpy(s + n, FRAME, 30); n += 30;
    memcpy(s + n, FRAME, 12); n += 12;                 // urwana
    s[n++] = 0xAA; s[n++] = 0xAA;
    memcpy(s + n, FRAME, 30); n += 30;
    memcpy(s + n, ACK_FW, 22); n += 22;

    ld2450_parser_t p;
    ld2450_parser_reset(&p);
    int data = 0, ack = 0;
    feed_all(&p, s, n, &data, &ack);
    CHECK(data == 2);
    CHECK(ack == 1);
    char fw[24];
    CHECK(ld2450_parse_fw(p.buf, p.flen, fw, sizeof(fw)));
    CHECK(strcmp(fw, "V1.02.22062416") == 0);

    // Ramka danych podana bajt po bajcie w dwoch kawalkach
    ld2450_parser_reset(&p);
    data = ack = 0;
    feed_all(&p, FRAME, 17, &data, &ack);
    feed_all(&p, FRAME + 17, 13, &data, &ack);
    CHECK(data == 1);

    // Zly ogon - ramka odrzucona
    uint8_t bad[30];
    memcpy(bad, FRAME, 30);
    bad[29] = 0xCD;
    ld2450_parser_reset(&p);
    data = ack = 0;
    feed_all(&p, bad, 30, &data, &ack);
    CHECK(data == 0);

    // Budowa komendy wejscia w tryb konfiguracji
    static const uint8_t CFG_ON[14] = { 0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0xFF, 0x00, 0x01, 0x00,
                                        0x04, 0x03, 0x02, 0x01 };
    uint8_t cmd[24];
    static const uint8_t one[2] = { 0x01, 0x00 };
    CHECK(ld2450_build_cmd(cmd, LD2450_CMD_CONFIG_ON, one, 2) == 14);
    CHECK(memcmp(cmd, CFG_ON, 14) == 0);

    // Indeksy predkosci z protokolu: 115200 = 5, 256000 = 7
    CHECK(ld2450_baud_index(115200) == 5);
    CHECK(ld2450_baud_index(256000) == 7);
    CHECK(ld2450_baud_index(460800) == 8);
    CHECK(ld2450_baud_index(12345) == 0);
    // Komenda ustawienia 256000: FD FC FB FA 04 00 A1 00 07 00 04 03 02 01
    static const uint8_t SET_BAUD[14] = { 0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0xA1, 0x00, 0x07, 0x00,
                                          0x04, 0x03, 0x02, 0x01 };
    static const uint8_t b7[2] = { 0x07, 0x00 };
    CHECK(ld2450_build_cmd(cmd, LD2450_CMD_SET_BAUD, b7, 2) == 14);
    CHECK(memcmp(cmd, SET_BAUD, 14) == 0);
}

static void test_bme(void) {
    // Kalibracja przykladowego egzemplarza - wartosci w typowym zakresie
    uint8_t c[BME68X_COEFF_LEN] = {0};
    c[0] = 0x2D; c[1] = 0x67;            // T2 = 26413
    c[2] = 0x03;                         // T3
    c[4] = 0x9E; c[5] = 0x8F;            // P1 = 36766
    c[6] = 0x2C; c[7] = 0xD7;            // P2 = -10452
    c[8] = 0x58;                         // P3 = 88
    c[10] = 0x6A; c[11] = 0x1D;          // P4
    c[12] = 0x22; c[13] = 0xFF;          // P5
    c[14] = 0x2C; c[15] = 0x1E;          // P7, P6
    c[18] = 0x5E; c[19] = 0xF1;          // P8
    c[20] = 0x55; c[21] = 0xF0;          // P9
    c[22] = 0x1E;                        // P10
    c[23] = 0x3F; c[24] = 0x34; c[25] = 0x2C;   // H2 msb, H2 lsb|H1 lsb, H1 msb
    c[26] = 0x00; c[27] = 0x2D; c[28] = 0x14; c[29] = 0x78; c[30] = 0x9C;
    c[31] = 0x8C; c[32] = 0x66;          // T1 = 26252
    c[33] = 0x9F; c[34] = 0xEB;          // GH2
    c[35] = 0xEA; c[36] = 0x12;          // GH1, GH3
    c[37] = 0x2E; c[39] = 0x16; c[41] = 0x10;
    bme68x_calib_t cal;
    bme68x_parse_calib(c, &cal);
    CHECK(cal.par_t1 == 26252);
    CHECK(cal.par_t2 == 26413);
    CHECK(cal.par_h1 == ((0x2C << 4) | 0x4));
    CHECK(cal.par_h2 == ((0x3F << 4) | 0x3));
    CHECK(cal.res_heat_range == 1);
    CHECK(cal.range_sw_err == 1);
    float t = bme68x_temperature(&cal, 480000);
    CHECK(t > -40 && t < 85);
    float p = bme68x_pressure(&cal, 300000);
    CHECK(p > 30000 && p < 110000);
    float h = bme68x_humidity(&cal, 20000);
    CHECK(h >= 0 && h <= 100);
    CHECK(bme68x_gas_wait(150) == 0x65);      // 150 ms = 37 x 4
    CHECK(bme68x_gas_wait(5000) == 0xFF);
    float g = bme68x_gas_high(512, 0);
    CHECK(g > 63.99e6f && g < 64.01e6f);      // 1e6 * 262144 / 4096 Ohm
    printf("BME: T=%.2f C  P=%.1f hPa  H=%.1f %%  heater=%u\n",
           t, p / 100, h, bme68x_heater_res(&cal, 320, 25));
}

// --- obecnosc ---------------------------------------------------------------

static bool near_f(float a, float b) { return fabsf(a - b) < 1.0f; }

// Pokoj 4 x 4 m, radar na srodku gornej sciany, strefa 1 x 1 m na srodku
static void pz_setup(pz_state_t *s) {
    memset(s, 0, sizeof(*s));
    pz_cfg_default(&s->cfg);
    s->cfg.hold_s = 10;
    s->cfg.zone_off_s = 3;
    s->cfg.latch_min = 1;
    s->room = (pz_room_t){ .set = true, .w = 4000, .l = 4000, .x = 2000, .y = 0 };
    s->zones[0] = (pz_zone_t){ 1500, 1500, 2500, 2500, PZ_ZONE_DETECT, "Kanapa" };
}

static uint32_t clk;
// n ramek co 100 ms z jednym celem (x, y radaru) albo bez celu
static void frames(pz_state_t *s, int n, bool vis, int x0, int y0, int x1, int y1, int v) {
    for (int k = 0; k < n; k++) {
        pz_raw_t r[3] = {{0}};
        if (vis) {
            r[0].valid = true;
            r[0].x = (int16_t)(x0 + (x1 - x0) * (k + 1) / n);
            r[0].y = (int16_t)(y0 + (y1 - y0) * (k + 1) / n);
            r[0].speed = (int16_t)v;
        }
        clk += 100;
        pz_update(s, r, true, clk);
    }
}

static void test_presence(void) {
    float x, y;
    pz_room_t r0 = { .set = true, .w = 4000, .l = 4000, .x = 2000, .y = 0, .rot = 0 };
    pz_to_room(&r0, 1000, 2000, &x, &y);        // radar patrzy w dol: jego prawo = lewo planu
    CHECK(near_f(x, 1000) && near_f(y, 2000));
    r0.flip = true;
    pz_to_room(&r0, 1000, 2000, &x, &y);
    CHECK(near_f(x, 3000) && near_f(y, 2000));
    pz_room_t r1 = { .set = true, .w = 4000, .l = 4000, .x = 0, .y = 2000, .rot = -90 };
    pz_to_room(&r1, 500, 1000, &x, &y);         // na lewej scianie, patrzy w prawo
    CHECK(near_f(x, 1000) && near_f(y, 2500));

    pz_state_t s;
    // Odrzucanie: za daleko, poza pokojem, strefa pomijana
    pz_setup(&s);
    s.zones[1] = (pz_zone_t){ 0, 0, 800, 800, PZ_ZONE_IGNORE, "Firanka" };
    pz_raw_t r[3] = { { true, 0, 6500, 0 }, { true, 0, 4600, 0 }, { true, 1500, 500, 0 } };
    clk = 1000;
    pz_update(&s, r, true, clk);
    CHECK(s.t[0].state == PZ_T_FAR);
    CHECK(s.t[1].state == PZ_T_OUTSIDE);
    CHECK(s.t[2].state == PZ_T_IGNORED);          // (500, 500) na planie
    CHECK(!s.room_tr.occ);
    s.cfg.room_only = false;
    pz_update(&s, r, true, clk += 100);
    CHECK(s.t[1].state == PZ_T_PENDING && !s.room_tr.occ);   // jeszcze nie potwierdzony
    for (int k = 0; k < 4; k++) pz_update(&s, r, true, clk += 100);
    CHECK(s.t[1].state == PZ_T_OK && s.room_tr.occ && s.room_tr.n == 1);

    // Osoba wchodzi na kanape, siada i "znika" dla radaru -> pamiec
    pz_setup(&s);
    clk = 1000;
    frames(&s, 20, true, 0, 3800, 0, 2000, 60);    // 2 s marszu od dolnej sciany
    CHECK(s.zone_tr[0].occ && s.zone_tr[0].n == 1 && s.t[0].zones == 1);
    frames(&s, 50, true, 0, 2000, 0, 2010, 0);     // 5 s bez ruchu
    frames(&s, 100, false, 0, 0, 0, 0, 0);         // 10 s niewidoczna
    CHECK(s.zone_tr[0].occ && s.zone_tr[0].latch && s.zone_tr[0].held == 1);
    CHECK(s.room_tr.occ && s.room_tr.latch);
    frames(&s, 520, false, 0, 0, 0, 0, 0);         // > 1 min limitu pamieci
    CHECK(!s.zone_tr[0].occ && !s.room_tr.occ);

    // Bez pamieci: strefa gasnie po opoznieniu
    pz_setup(&s);
    s.cfg.memory = false;
    clk = 1000;
    frames(&s, 20, true, 0, 3800, 0, 2000, 60);
    frames(&s, 50, true, 0, 2000, 0, 2010, 0);
    frames(&s, 20, false, 0, 0, 0, 0, 0);
    CHECK(s.zone_tr[0].occ && !s.zone_tr[0].latch);  // 2 s < 3 s opoznienia
    frames(&s, 15, false, 0, 0, 0, 0, 0);
    CHECK(!s.zone_tr[0].occ);

    // Wychodzi ze strefy i dalej ja widac -> zwykle opoznienie, bez pamieci
    pz_setup(&s);
    clk = 1000;
    frames(&s, 30, true, 0, 2000, 0, 2010, 0);
    frames(&s, 10, true, 0, 2000, 0, 3200, 120);
    CHECK(!s.zone_tr[0].latch && s.room_tr.occ && !s.room_tr.latch);
    frames(&s, 40, true, 0, 3200, 0, 3200, 0);
    CHECK(!s.zone_tr[0].occ);

    // Idzie do drzwi przy dolnej scianie i znika -> wyszla, pokoj gasnie po 10 s
    pz_setup(&s);
    clk = 1000;
    frames(&s, 20, true, 0, 2000, 0, 3800, 90);
    frames(&s, 50, false, 0, 0, 0, 0, 0);
    CHECK(s.room_tr.occ && !s.room_tr.latch && s.room_tr.held == 0);
    frames(&s, 60, false, 0, 0, 0, 0, 0);
    CHECK(!s.room_tr.occ);

    // Brak danych z radaru nie kasuje obecnosci
    pz_setup(&s);
    clk = 1000;
    frames(&s, 10, true, 0, 2000, 0, 2000, 0);
    pz_raw_t none[3] = {{0}};
    for (int k = 0; k < 300; k++) pz_update(&s, none, false, clk += 100);
    CHECK(s.room_tr.occ);

    // Filtry: drgania pozycji osoby stojacej sa wygladzane
    pz_setup(&s);
    clk = 1000;
    float lo = 1e9f, hi = -1e9f;
    for (int k = 0; k < 60; k++) {
        pz_raw_t j[3] = { { true, (int16_t)(k % 2 ? 100 : -100), (int16_t)(2000 + (k % 3) * 60 - 60), 0 } };
        pz_update(&s, j, true, clk += 100);
        if (k >= 30) {
            if (s.t[0].rx < lo) lo = s.t[0].rx;
            if (s.t[0].rx > hi) hi = s.t[0].rx;
        }
    }
    CHECK(hi - lo < 100);                          // surowo 200 mm rozrzutu

    // Pojedyncze ramki z odbic (duch) nie wlaczaja obecnosci
    pz_setup(&s);
    clk = 1000;
    for (int k = 0; k < 50; k++) {
        pz_raw_t gh[3] = { { k % 4 == 0, 500, 3000, 0 } };   // co 4. ramka, 1 ramka naraz
        pz_update(&s, gh, true, clk += 100);
        CHECK(!s.room_tr.occ);
    }

    // Radar gubi osobe na 2 ramki - cel trwa, obecnosc bez przerwy
    pz_setup(&s);
    s.cfg.hold_s = 0;
    s.cfg.memory = false;
    clk = 1000;
    frames(&s, 10, true, 0, 2000, 0, 2000, 0);
    frames(&s, 2, false, 0, 0, 0, 0, 0);
    CHECK(s.t[0].valid && s.t[0].held && s.room_tr.n == 1);
    frames(&s, 2, true, 0, 2000, 0, 2000, 0);
    CHECK(s.t[0].valid && !s.t[0].held && s.t[0].state == PZ_T_OK);   // bez ponownego potwierdzania
    frames(&s, 4, false, 0, 0, 0, 0, 0);
    CHECK(!s.t[0].valid);                          // dluzsza przerwa - cel znika

    // Histereza: osoba stoi na krawedzi strefy (x 1500 na planie), pozycja skacze
    pz_setup(&s);
    clk = 1000;
    frames(&s, 10, true, 0, 2000, 0, 2000, 0);     // srodek strefy
    int out = 0;
    for (int k = 0; k < 40; k++) {
        // na planie x = 2000 - rx: 1440..1560 wokol krawedzi 1500
        pz_raw_t e[3] = { { true, (int16_t)(k % 2 ? 560 : 440), 2000, 0 } };
        pz_update(&s, e, true, clk += 100);
        if (!(s.t[0].zones & 1)) out++;
    }
    CHECK(out == 0 && s.zone_tr[0].n == 1);
    frames(&s, 20, true, 1000, 2000, 1000, 2000, 0);   // x = 1000: wyraznie poza strefa
    CHECK(!(s.t[0].zones & 1));

    pz_cfg_t c;
    pz_cfg_default(&c);
    CHECK(pz_check_cfg(&c) == NULL);
    c.max_dist_mm = 9000;
    CHECK(pz_check_cfg(&c) != NULL);
    pz_zone_t z = { 100, 100, 400, 900, PZ_ZONE_DETECT, "" };
    CHECK(pz_check_zone(&z, NULL) == NULL);
    z.x2 = 150;
    CHECK(pz_check_zone(&z, NULL) != NULL);
}

int main(void) {
    test_ld2450();
    test_bme();
    test_presence();
    if (fails) { printf("%d bledow\n", fails); return 1; }
    printf("OK - wszystkie testy przeszly\n");
    return 0;
}
