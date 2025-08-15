#pragma once
#include <Wire.h>
#include <U8g2lib.h>

constexpr uint8_t OLED_SDA = 17;
constexpr uint8_t OLED_SCL = 18;
constexpr uint8_t OLED_RST = U8X8_PIN_NONE;
constexpr uint8_t OLED_ADDR = 0x3C;

inline U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST);

inline bool oledInit() {
    Serial.println(F("OLED init…"));

    Wire.begin(OLED_SDA, OLED_SCL, 100000);
    u8g2.setI2CAddress(OLED_ADDR << 1);

    if(!u8g2.begin()) {
        Serial.println(F("u8g2.begin() FAILED"));
        return false;
    }
    u8g2.setBusClock(400000);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0,12,"OLED OK");
    u8g2.sendBuffer();
    return true;
}
inline void oledPrintfLines(int x, int yStart, int yStep,
                            const char* fmt, ...) {
    u8g2.clearBuffer();

    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    int y = yStart;
    for(char* line = strtok(tmp, "\n"); line; line = strtok(nullptr, "\n")) {
        u8g2.drawStr(x, y, line);
        y += yStep;
    }
    u8g2.sendBuffer();
}