#include "captive_dns.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "DNS";
static uint32_t s_ip;
static bool     s_running = false;

static void dns_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Nie mozna utworzyc gniazda");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Port 53 zajety");
        close(sock);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Portal: odpowiadam na zapytania DNS");

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        // Zapas 16 B na rekord odpowiedzi doklejany za pytaniem
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        if (buf[2] & 0x80) continue;                       // to odpowiedz, nie pytanie
        if (((buf[4] << 8) | buf[5]) < 1) continue;        // brak pytan

        // Pierwsze pytanie: nazwa (etykiety), typ, klasa
        int p = 12;
        while (p < n && buf[p] != 0) {
            if ((buf[p] & 0xC0) != 0) { p = n; break; }    // kompresja w pytaniu - nie obslugujemy
            p += buf[p] + 1;
        }
        p++;                                               // zero konczace nazwe
        if (p + 4 > n) continue;
        uint16_t qtype  = (uint16_t)((buf[p] << 8) | buf[p + 1]);
        uint16_t qclass = (uint16_t)((buf[p + 2] << 8) | buf[p + 3]);
        p += 4;

        // Odpowiedz w miejscu zapytania: naglowek + pytanie (+ rekord A).
        // Pozostale sekcje zapytania (np. EDNS) odcinamy.
        bool answer = (qtype == 1 && qclass == 1);         // A, IN
        buf[2] = 0x81 | (buf[2] & 0x01);                   // QR, opcode 0, RD jak w pytaniu
        buf[3] = 0x80;                                     // RA, RCODE 0
        buf[4] = 0; buf[5] = 1;                            // 1 pytanie
        buf[6] = 0; buf[7] = answer ? 1 : 0;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = p;
        if (answer) {
            const uint8_t *ip = (const uint8_t *)&s_ip;
            uint8_t rr[16] = {
                0xC0, 0x0C,               // wskaznik na nazwe z pytania
                0x00, 0x01, 0x00, 0x01,   // A, IN
                0x00, 0x00, 0x00, 0x3C,   // TTL 60 s
                0x00, 0x04,
                ip[0], ip[1], ip[2], ip[3],
            };
            memcpy(buf + len, rr, sizeof(rr));
            len += sizeof(rr);
        }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, fl);
    }
}

void captive_dns_start(uint32_t ip) {
    s_ip = ip;
    if (s_running) return;
    s_running = true;
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
}
