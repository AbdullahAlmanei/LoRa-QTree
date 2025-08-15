#pragma once
#include <Arduino.h>

enum MsgType : uint8_t
{
  BEACON = 0x01,
  JOIN_REQ = 0x02,
  JOIN_ACK = 0x03,
  DATA_UP = 0x04,
  DATA_ACK = 0x05,
  QUERY = 0x06,
  STATE = 0x07
};

#ifndef MSG_CHILD_ADD
#define MSG_CHILD_ADD 0xA1
#define MSG_CHILD_GONE 0xA2
#define MSG_JOIN_NACK 0xA3
#endif

enum : uint8_t
{
  HDR_MAGIC = 0xA5
};
constexpr uint8_t GW_ID = 0x00;
constexpr uint8_t MAX_HOPS = 6; 
constexpr uint8_t MAX_CAND = 5;

#ifndef MAX_PAYLOAD
#define MAX_PAYLOAD 64 
#endif

struct __attribute__((packed)) MeshHeader
{
  uint8_t magic;
  uint8_t src;
  uint8_t dst;
  uint8_t hops;
  MsgType type;
  uint8_t len;
};
static_assert(sizeof(MeshHeader) == 6, "Header mis-sized");

struct __attribute__((packed)) StatusPayload
{
  uint8_t parent;
  uint8_t hops;
  int8_t rssi;
};

typedef struct __attribute__((packed))
{
  uint8_t ver;
  uint32_t test_id;
  uint32_t seq;
  uint32_t src;
  uint32_t tx_epoch_ms;
  uint8_t hop_cnt;
  uint16_t batt_mV;     // 0 if unknown
} test_hdr_t;

#ifndef TEST_MAGIC
#define TEST_MAGIC 0xA5A5A5A5UL
#endif
