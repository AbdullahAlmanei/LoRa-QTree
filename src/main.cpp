#include <Arduino.h>
#include "esp_system.h"
#if defined(HELTEC_V3_NODE)
#include "pins_heltec_v3.h"
#elif defined(TBEAM_S3_NODE)
#include "pins.h"
#else
#error "No board type defined"
#endif
#include <RadioLib.h>
#include "oled.h"
#include "protocol.h"

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
struct LoraCfg
{
  float freq;
  float bw;
  uint8_t sf;
  uint8_t cr;
  uint8_t sw;
} cfg;

int16_t initRadio()
{
  cfg = {868.0, 125.0, 12, 5, 0x12};

  int16_t st = radio.begin(cfg.freq);
  if (st)
    return st;

  radio.setBandwidth(cfg.bw);
  radio.setSpreadingFactor(cfg.sf);
  radio.setCodingRate(cfg.cr);
  radio.setSyncWord(cfg.sw);
  return radio.startReceive();
}

static_assert(
    std::is_same<SX1262, decltype(radio)>::value,
    "radio is NOT SX1262 -> wrong type");

#ifdef ROLE_GATEWAY
void meshSetupGateway();
void meshLoopGateway();
#else
void meshSetupNode();
void meshLoopNode();
#endif

void setup()
{
  Serial.begin(115200);
  delay(4400); // allow serial monitor to attach
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
#ifdef HELTEC_V3_NODE
  SPI.setFrequency(1000000); // 1MHz - slow and reliable
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);
  delay(100);
#endif
  Serial.println("readying");
  Serial.printf("radio: %d", initRadio());
  Serial.printf("GW  freq=%.1f  BW=%.0f  SF=%u  CR=4/%u  SW=0x%02X\n",
                cfg.freq, cfg.bw, cfg.sf, cfg.cr, cfg.sw);
#ifdef ROLE_GATEWAY
  oledInit();
  // oledPrint("Gateway boot!");
  meshSetupGateway();
#else
  meshSetupNode();
#endif
}

void loop()
{
#ifdef ROLE_GATEWAY
  meshLoopGateway();
#else
  meshLoopNode();
#endif
}
