#include "ld2450_proto.h"
#include <stdio.h>
#include <string.h>

static const uint8_t HDR_DATA[4] = { 0xAA, 0xFF, 0x03, 0x00 };
static const uint8_t HDR_ACK[4]  = { 0xFD, 0xFC, 0xFB, 0xFA };
static const uint8_t TAIL_ACK[4] = { 0x04, 0x03, 0x02, 0x01 };

void ld2450_parser_reset(ld2450_parser_t *p) {
    memset(p, 0, sizeof(*p));
}

// Zepsuta ramka (urwana, z bledna dlugoscia lub ogonem) moze zawierac poczatek
// nastepnej - np. radar zaczal nowa ramke w srodku poprzedniej. Bez tego po
// kazdym urwaniu przepadalaby takze kolejna, poprawna ramka.
static ld2450_frame_t resync(ld2450_parser_t *p, uint16_t n) {
    uint8_t tmp[LD2450_BUF_LEN];
    memcpy(tmp, p->buf, n);
    p->len = 0;
    for (uint16_t i = 1; i < n; i++) {
        ld2450_frame_t r = ld2450_parser_feed(p, tmp[i]);
        if (r != LD2450_FRAME_NONE) return r;
    }
    return LD2450_FRAME_NONE;
}

ld2450_frame_t ld2450_parser_feed(ld2450_parser_t *p, uint8_t b) {
    if (p->len == 0) {
        if (b == HDR_DATA[0])     p->kind = LD2450_FRAME_DATA;
        else if (b == HDR_ACK[0]) p->kind = LD2450_FRAME_ACK;
        else return LD2450_FRAME_NONE;
        p->buf[0] = b;
        p->len = 1;
        return LD2450_FRAME_NONE;
    }
    p->buf[p->len++] = b;

    if (p->len <= 4) {
        const uint8_t *h = (p->kind == LD2450_FRAME_DATA) ? HDR_DATA : HDR_ACK;
        if (b != h[p->len - 1]) {
            // To nie naglowek - ten bajt moze zaczynac prawdziwa ramke
            p->len = 0;
            return ld2450_parser_feed(p, b);
        }
        return LD2450_FRAME_NONE;
    }

    if (p->kind == LD2450_FRAME_DATA) {
        if (p->len < LD2450_DATA_LEN) return LD2450_FRAME_NONE;
        if (p->buf[28] != 0x55 || p->buf[29] != 0xCC) return resync(p, LD2450_DATA_LEN);
        p->len = 0;
        p->flen = LD2450_DATA_LEN;
        return LD2450_FRAME_DATA;
    }

    // Ramka odpowiedzi: dlugosc po naglowku
    if (p->len == 6) {
        uint16_t l = (uint16_t)(p->buf[4] | (p->buf[5] << 8));
        if (l < 2 || 6 + l + 4 > LD2450_BUF_LEN) return resync(p, 6);
        p->need = (uint16_t)(6 + l + 4);
        return LD2450_FRAME_NONE;
    }
    if (p->len < 6 || p->len < p->need) return LD2450_FRAME_NONE;
    uint16_t n = p->len;
    if (memcmp(p->buf + n - 4, TAIL_ACK, 4) != 0) return resync(p, n);
    p->len = 0;
    p->flen = n;
    return LD2450_FRAME_ACK;
}

static int16_t decode_signed(uint8_t lo, uint8_t hi) {
    int16_t v = (int16_t)(((hi & 0x7F) << 8) | lo);
    return (hi & 0x80) ? v : (int16_t)-v;
}

void ld2450_decode_targets(const uint8_t *frame, ld2450_target_t out[LD2450_MAX_TARGETS]) {
    for (int i = 0; i < LD2450_MAX_TARGETS; i++) {
        const uint8_t *t = frame + 4 + i * 8;
        ld2450_target_t *o = &out[i];
        o->x_mm      = decode_signed(t[0], t[1]);
        o->y_mm      = decode_signed(t[2], t[3]);
        o->speed_cms = decode_signed(t[4], t[5]);
        o->res_mm    = (uint16_t)(t[6] | (t[7] << 8));
        // Pusty slot to same zera
        o->valid = (o->x_mm != 0 || o->y_mm != 0);
    }
}

int ld2450_build_cmd(uint8_t *out, uint16_t cmd, const uint8_t *val, uint16_t vlen) {
    int n = 0;
    memcpy(out, HDR_ACK, 4); n += 4;
    uint16_t len = (uint16_t)(2 + vlen);
    out[n++] = (uint8_t)(len & 0xFF);
    out[n++] = (uint8_t)(len >> 8);
    out[n++] = (uint8_t)(cmd & 0xFF);
    out[n++] = (uint8_t)(cmd >> 8);
    if (vlen) { memcpy(out + n, val, vlen); n += vlen; }
    memcpy(out + n, TAIL_ACK, 4); n += 4;
    return n;
}

bool ld2450_parse_fw(const uint8_t *ack, int len, char *out, int cap) {
    // FD FC FB FA | 0C 00 | A0 01 | status 00 00 | typ 00 01 | major (2) | minor (4) | ogon
    if (len < 22) return false;
    if (ack[6] != (LD2450_CMD_READ_FW & 0xFF) || ack[7] != 0x01) return false;
    if (ack[8] != 0 || ack[9] != 0) return false;
    snprintf(out, cap, "V%u.%02X.%02X%02X%02X%02X",
             ack[13], ack[12], ack[17], ack[16], ack[15], ack[14]);
    return true;
}
