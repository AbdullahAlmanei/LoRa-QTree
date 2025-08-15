#pragma once
/*  Heltec WiFi LoRa 32 V3  (ESP32-S3 FN4R8 + SX1262)
    Pin mapping based on official schematic diagram */

#define LORA_SCK    9     // GPIO9  – SCK
#define LORA_MISO   11    // GPIO11 – MISO
#define LORA_MOSI   10    // GPIO10 – MOSI
#define LORA_CS     8     // GPIO8  – NSS / CS
#define LORA_RST    12    // GPIO12 – RESET
#define LORA_BUSY   13    // GPIO13 – BUSY
#define LORA_DIO1   14    // GPIO14 – DIO1 IRQ

#define LED_BUILTIN 35
