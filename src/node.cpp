#include "protocol.h"
#include <RadioLib.h>
#include <Preferences.h>

#ifndef ENABLE_TEST_TX
#define ENABLE_TEST_TX 0
#endif

extern SX1262 radio;
#define LED_BUILTIN 35

constexpr uint32_t LOST_PARENT_MS = 300000;
constexpr uint32_t CHILD_SILENT_MS = 180000;
constexpr uint8_t MAX_CHILDREN = 10;

static uint32_t nextJoinAt = 0;
static uint32_t joinAckDeadline = 0;
static uint8_t joinParentTrying = 0xFF;

constexpr uint32_t JOIN_RETRY_MS = 5000;
constexpr uint32_t JOIN_ACK_TIMEOUT_MS = 10000;

#if ENABLE_TEST_TX
static constexpr uint32_t TEST_PERIOD_MS = 90000;
static uint32_t lastTestTx = 0;
static uint32_t testSeq = 0;

#if (defined(TBEAM_S3_NODE) || defined(HELTEC_V3_NODE)) && defined(ROLE_NODE)
#include "XPowersAXP2101.tpp"
#include "XPowersLibInterface.hpp"
__attribute__((weak)) XPowersLibInterface *PMU = nullptr;
static inline uint16_t battery_mV()
{
    if (PMU)
    {
        PMU->enableBattVoltageMeasure();
        return (uint16_t)PMU->getBattVoltage();
    }
    return 0;
}
#else
static inline uint16_t battery_mV() { return 0; }
#endif
#endif

#ifndef MAX_HOPS
#define MAX_HOPS 3
#endif

static constexpr int16_t ERR_TX_DEFERRED = 1;
static constexpr uint32_t DC_WINDOW_MS = 3600000UL;
static constexpr uint32_t DC_CAP_MS = 36000UL;
static constexpr int32_t DC_BORROW_MS = 12000;

static uint32_t dc_free_at = 0;
static int32_t dc_tokens_ms = (int32_t)DC_CAP_MS;
static uint32_t dc_last_ref_ms = 0;
static uint16_t dc_ref_rem = 0;

static inline void dcRefill(uint32_t now)
{
    if (!dc_last_ref_ms)
    {
        dc_last_ref_ms = now;
        return;
    }
    uint32_t elapsed = now - dc_last_ref_ms;
    dc_last_ref_ms = now;
    uint32_t accum = dc_ref_rem + (elapsed % 100);
    uint32_t add = elapsed / 100;
    if (accum >= 100)
    {
        add += 1;
        accum -= 100;
    }
    dc_ref_rem = (uint16_t)accum;
    dc_tokens_ms += (int32_t)add;
    if (dc_tokens_ms > (int32_t)DC_CAP_MS)
        dc_tokens_ms = (int32_t)DC_CAP_MS;
}

static inline bool dcReady()
{
    return millis() >= dc_free_at;
}

static int16_t transmitWithDC(const uint8_t *buf, size_t len)
{
    uint32_t now = millis();
    if (!dcReady())
        return ERR_TX_DEFERRED;
    dcRefill(now);
    uint32_t t0 = millis();
    int16_t st = radio.transmit(buf, len);
    uint32_t t1 = millis();
    if (st == RADIOLIB_ERR_NONE)
    {
        uint32_t on = (t1 > t0) ? (t1 - t0) : 1;
        dc_tokens_ms -= (int32_t)on;
        if (dc_tokens_ms < -DC_BORROW_MS)
        {
            int32_t deficit = (-DC_BORROW_MS - dc_tokens_ms);
            dc_free_at = t1 + (uint32_t)deficit * 100UL;
        }
        else
        {
            dc_free_at = t1;
        }
        radio.startReceive();
    }
    return st;
}

struct Cand
{
    uint8_t id = 0xFF;
    int16_t rssi = -127;
    uint8_t hops = 0xFF;
    uint32_t lastSeen = 0;
};
static Cand cand[MAX_CAND];

struct Child
{
    uint8_t id = 0;
    uint32_t lastSeen = 0;
};
static Child children[MAX_CHILDREN];

static Child *findChild(uint8_t id)
{
    for (auto &c : children)
        if (c.id == id)
            return &c;
    return nullptr;
}
static bool isChild(uint8_t id) { return findChild(id); }
static int childCount()
{
    int n = 0;
    for (auto &c : children)
        if (c.id)
            ++n;
    return n;
}
static bool addChildLocal(uint8_t id)
{
    if (isChild(id) || childCount() >= MAX_CHILDREN)
        return false;
    for (auto &c : children)
        if (!c.id)
        {
            c.id = id;
            c.lastSeen = millis();
            return true;
        }
    return false;
}
static void removeChildLocal(uint8_t id)
{
    for (auto &c : children)
        if (c.id == id)
        {
            c.id = 0;
            return;
        }
}

static Preferences prefs;
static uint8_t myId = 0;
static uint8_t parentId = 0xFF;
static int16_t parentRssi = -140;
static uint32_t lastParentRx = 0;
static uint8_t myHopToGW = 0xFF;

#ifndef MSG_CHILD_ADD
enum : uint8_t
{
    MSG_CHILD_ADD = 0xA1,
    MSG_CHILD_GONE = 0xA2,
    MSG_JOIN_NACK = 0xA3
};
#endif

struct __attribute__((packed)) ChildEventPayload
{
    uint8_t child;
    uint8_t parent;
    uint8_t hops;
};

#ifndef MAX_PAYLOAD
#define MAX_PAYLOAD 64
#endif
constexpr uint8_t MAX_TXQ = 16;

struct PendingTx
{
    bool in_use = false;
    uint8_t src, dst, hops;
    MsgType type;
    uint8_t len;
    uint8_t data[MAX_PAYLOAD];
    uint32_t nextTry = 0;
    uint8_t tries = 0;
};
static PendingTx txq[MAX_TXQ];

static bool enqueueTx(uint8_t src, uint8_t dst, uint8_t hops, MsgType type,
                      const uint8_t *pl, uint8_t len, uint32_t when)
{
    for (auto &e : txq)
    {
        if (!e.in_use)
        {
            e.in_use = true;
            e.src = src;
            e.dst = dst;
            e.hops = hops;
            e.type = type;
            e.len = (len > MAX_PAYLOAD) ? (uint8_t)MAX_PAYLOAD : len;
            if (e.len && pl)
                memcpy(e.data, pl, e.len);
            e.nextTry = when;
            e.tries = 0;
            return true;
        }
    }
    return false;
}

static bool trySendOne(PendingTx &e)
{
    MeshHeader h{HDR_MAGIC, e.src, e.dst, e.hops, e.type, e.len};
    uint8_t buf[sizeof(MeshHeader) + MAX_PAYLOAD];
    memcpy(buf, &h, sizeof(h));
    if (e.len)
        memcpy(buf + sizeof(h), e.data, e.len);

    int16_t st = transmitWithDC(buf, sizeof(h) + e.len);
    if (st == RADIOLIB_ERR_NONE)
    {
        e.in_use = false;
        return true;
    }
    uint32_t now = millis();
    uint32_t slack = 50;
    if (st == ERR_TX_DEFERRED)
    {
        Serial.println("que AGAINnoiw");
        e.nextTry = dc_free_at + slack;
    }
    else
    {
        e.nextTry = now + 200;
    }
    e.tries = (uint8_t)std::min<uint8_t>(e.tries + 1, 200);
    return false;
}

static void processTxQueue()
{
    uint32_t now = millis();
    for (auto &e : txq)
    {
        if (!e.in_use)
            continue;
        if (now >= e.nextTry)
            (void)trySendOne(e);
    }
}

static int16_t sendPacket(uint8_t src, uint8_t dst, uint8_t hops, MsgType type,
                          const uint8_t *pl = nullptr, uint8_t len = 0)
{
    MeshHeader h{HDR_MAGIC, src, dst, hops, type, len};
    uint8_t buf[sizeof(MeshHeader) + MAX_PAYLOAD];
    uint8_t L = (len > MAX_PAYLOAD) ? (uint8_t)MAX_PAYLOAD : len;
    memcpy(buf, &h, sizeof(h));
    if (L)
        memcpy(buf + sizeof(h), pl, L);

    int16_t st = transmitWithDC(buf, sizeof(h) + L);
    if (st == ERR_TX_DEFERRED)
    {
        uint32_t when = dc_free_at + 50;
        Serial.println("que for noiw");
        (void)enqueueTx(src, dst, hops, type, pl, L, when);
        return st;
    }
    if (st != RADIOLIB_ERR_NONE)
    {
        Serial.printf("TX err %d\n", st);
    }
    return st;
}

#if ENABLE_TEST_TX
static void sendTestFrame()
{
    test_hdr_t th{};
    th.ver = 1;
    th.test_id = TEST_MAGIC;
    th.seq = ++testSeq;
    th.src = (uint32_t)myId;
    th.tx_epoch_ms = millis();
    th.hop_cnt = 0;
    th.batt_mV = battery_mV();

    (void)sendPacket(myId, GW_ID, MAX_HOPS, DATA_UP,
                     reinterpret_cast<uint8_t *>(&th), sizeof(th));
}
#endif

static void candUpdate(uint8_t id, int16_t rssi, uint8_t hops)
{
    if (rssi < -120 || hops > 6)
        return;
    int slot = -1, oldest = 0;
    for (uint8_t i = 0; i < MAX_CAND; ++i)
    {
        if (cand[i].id == id)
        {
            slot = i;
            break;
        }
        if (cand[i].lastSeen < cand[oldest].lastSeen)
            oldest = i;
    }
    if (slot == -1)
        slot = oldest;
    cand[slot].id = id;
    cand[slot].rssi = rssi;
    cand[slot].hops = hops;
    cand[slot].lastSeen = millis();
}
static uint8_t pickParent()
{
    int best = -1;
    for (uint8_t i = 0; i < MAX_CAND; ++i)
    {
        if (millis() - cand[i].lastSeen > 90000)
            continue;
        if (best == -1)
        {
            best = i;
            continue;
        }
        if (cand[i].rssi > cand[best].rssi)
        {
            best = i;
            continue;
        }
        if (cand[i].rssi < cand[best].rssi)
            continue;
        if (cand[i].hops < cand[best].hops)
        {
            best = i;
            continue;
        }
        if (cand[i].hops > cand[best].hops)
            continue;
        if (cand[i].id < cand[best].id)
            best = i;
    }
    if (best == -1)
        return 0xFF;
    parentRssi = cand[best].rssi;
    return cand[best].id;
}

static bool shouldRelay(const MeshHeader &h)
{
    return (h.src == parentId) || isChild(h.src);
}
static void forward(MeshHeader &h, uint8_t *pl)
{
    if (!shouldRelay(h) || h.hops >= MAX_HOPS)
        return;

#if ENABLE_TEST_TX
    if (h.len >= sizeof(test_hdr_t))
    {
        auto *th = reinterpret_cast<test_hdr_t *>(pl);
        if (th->ver == 1 && th->test_id == TEST_MAGIC)
        {
            th->hop_cnt++;
        }
    }
#endif
    ++h.hops;
    sendPacket(h.src, h.dst, h.hops, h.type, pl, h.len);
}

void meshSetupNode()
{
    pinMode(LED_BUILTIN, OUTPUT);
    prefs.begin("mesh", false);
    myId = prefs.getUChar("id", 0);
    if (!myId)
    {
        myId = random(1, 0xFE);
        prefs.putUChar("id", myId);
    }
    Serial.printf("MeshHeader=%u bytes\n", (unsigned)sizeof(MeshHeader));
    radio.startReceive();
}

static void handleRx()
{
    uint8_t buf[sizeof(MeshHeader) + MAX_PAYLOAD];
    int16_t rc = radio.readData(buf, sizeof(buf));
    if (rc == RADIOLIB_ERR_RX_TIMEOUT)
        return;
    if (rc != RADIOLIB_ERR_NONE)
    {
        radio.startReceive();
        return;
    }

    auto &h = *reinterpret_cast<MeshHeader *>(buf);
    if (h.magic != HDR_MAGIC)
        return;

    int16_t rssi = radio.getRSSI();

    if (h.src != myId)
        candUpdate(h.src, rssi, h.hops);

    if (h.src == parentId)
        lastParentRx = millis();

    if (isChild(h.src))
    {
        if (auto *c = findChild(h.src))
            c->lastSeen = millis();
    }

    if (h.dst != myId && h.dst != 0xFF)
    {
        forward(h, buf + sizeof(MeshHeader));
        return;
    }

    switch (h.type)
    {
    case JOIN_REQ:
    {
        if (parentId == 0xFF)
            break;

        if (childCount() < MAX_CHILDREN && addChildLocal(h.src))
        {
            sendPacket(myId, h.src, MAX_HOPS, JOIN_ACK);
            ChildEventPayload ev{h.src, myId, (uint8_t)((myHopToGW == 0xFF) ? 0xFF : (myHopToGW + 1))};
            sendPacket(myId, GW_ID, MAX_HOPS, (MsgType)MSG_CHILD_ADD, (uint8_t *)&ev, sizeof(ev));
        }
        else
        {
            sendPacket(myId, h.src, MAX_HOPS, (MsgType)MSG_JOIN_NACK);
        }
        break;
    }

    case JOIN_ACK:
        if (h.dst == myId)
        {
            if (parentId != 0xFF)
            {
                radio.startReceive();
                return;
            }
            parentId = h.src;
            lastParentRx = millis();
            Serial.printf("JOIN_ACK from 0x%02X -> parent set\n", parentId);
        }
        break;

    case (MsgType)MSG_JOIN_NACK:
        if (h.dst == myId)
        {
            parentId = 0xFF;
        }
        break;

    case QUERY:
    {
        myHopToGW = h.hops;
        StatusPayload sp{parentId, h.hops, int8_t(parentRssi)};
        Serial.println("they want me fr");
        int st = sendPacket(myId, GW_ID, MAX_HOPS, STATE, (uint8_t *)&sp, sizeof(sp));
        Serial.printf("n ey got me %d", st);
        break;
    }

    case DATA_ACK:
        break;

    default:
        break;
    }
}

static void pruneChildren()
{
    uint32_t now = millis();
    for (auto &c : children)
    {
        if (c.id && now - c.lastSeen > CHILD_SILENT_MS)
        {
            ChildEventPayload ev{c.id, myId, (uint8_t)((myHopToGW == 0xFF) ? 0xFF : (myHopToGW + 1))};
            sendPacket(myId, GW_ID, MAX_HOPS, (MsgType)MSG_CHILD_GONE, (uint8_t *)&ev, sizeof(ev));
            Serial.printf("Child 0x%02X aged out\n", c.id);
            c.id = 0;
        }
    }
}

void meshLoopNode()
{
    processTxQueue();
    handleRx();
    pruneChildren();

    uint32_t now = millis();

    digitalWrite(LED_BUILTIN, (parentId != 0xFF) ? ((now >> 8) & 1) : ((now >> 10) & 1));

    if (parentId != 0xFF && now - lastParentRx > LOST_PARENT_MS)
    {
        Serial.println(F("Parent silent → detach"));
        parentId = 0xFF;
        for (auto &c : children)
            c.id = 0;
    }

    if (parentId == 0xFF)
    {
        if (now >= nextJoinAt)
        {
            uint8_t p = pickParent();
            if (p == 0xFF)
            {
                nextJoinAt = now + JOIN_RETRY_MS;
            }
            else
            {
                Serial.printf("JOIN_REQ -> 0x%02X\n", p);
                int16_t st = sendPacket(myId, p, MAX_HOPS, JOIN_REQ);
                if (st == ERR_TX_DEFERRED)
                {
                    extern uint32_t dc_free_at;
                    uint32_t slack = 50;
                    nextJoinAt = max(now + 200, dc_free_at + slack);
                    Serial.printf("JOIN deferred; retry at +%lu ms\n",
                                  (unsigned long)(nextJoinAt - now));
                }
                else
                {
                    joinParentTrying = p;
                    joinAckDeadline = now + JOIN_ACK_TIMEOUT_MS;
                    nextJoinAt = now + JOIN_RETRY_MS;
                }
            }
        }
    }

#if ENABLE_TEST_TX
    if (parentId != 0xFF && now - lastTestTx > TEST_PERIOD_MS)
    {
        sendTestFrame();
        lastTestTx = now;
    }
#endif
}
