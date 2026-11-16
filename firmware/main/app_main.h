#pragma once

// Waveshare ESP32-C6-Zero - polaczenia opisane w PINOUT.md.
// Wylacznie piny z krawedzi plytki: bez strapping (4, 5, 15), UART0 (16, 17),
// pinow przy antenie (5, 22) i malych padow od spodu (6-9, 12, 13, 23).

// Radar HLK-LD2450 (UART1, 256000 bd 8N1)
#define LD2450_UART_NUM   1
#define LD2450_PIN_TX     18    // ESP TX -> RX radaru
#define LD2450_PIN_RX     19    // ESP RX <- TX radaru

// Czujnik BME688 (I2C, adres 0x76 lub 0x77 - wykrywany sam)
#define BME688_PIN_SDA    20
#define BME688_PIN_SCL    21

// Na plytce: dioda WS2812 i przycisk BOOT
#define LED_PIN           8
#define BOOT_BUTTON_PIN   9

// Przytrzymanie BOOT przez tyle sekund kasuje siec Wi-Fi (powrot do trybu AP)
#define WIFI_RESET_HOLD_S 5
