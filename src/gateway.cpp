#include "protocol.h"
#include <RadioLib.h>
#include <oled.h>
#include <memory>
#include <algorithm>

#if !defined(ROLE_GATEWAY)
void meshSetupGateway() {}
void meshLoopGateway() {}
#else

extern SX1262 radio;

constexpr uint32_t BEACON_PERIOD_MS = 60000;
constexpr uint32_t QUERY_PERIOD_MS = 50000;
constexpr uint32_t QUERY_TIMEOUT_MS = 15000;
constexpr uint8_t MAX_MISSES = 5;
constexpr uint32_t CHILD_TIMEOUT_MS = 180000;
constexpr uint32_t JOIN_ACK_GAP_MS = 2000;
constexpr uint8_t MAX_PENDING_JOINS = 16;

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

struct Child
{
    uint8_t id = 0;
    uint8_t parent = GW_ID;
    uint8_t hops = 1;
    uint8_t misses = 0;
    int16_t lastRssi = -127;
    uint32_t lastSeen = 0;
    uint32_t lastQuery = 0;
    uint32_t lastJoinAck = 0;
    bool answeredSinceQuery = false;
};
static Child children[64];

static Child *findChild(uint8_t id)
{
    for (auto &c : children)
        if (c.id == id)
            return &c;
    return nullptr;
}
static Child *allocChild(uint8_t id)
{
    if (auto *c = findChild(id))
        return c;
    for (auto &s : children)
    {
        if (!s.id)
        {
            s.id = id;
            s.lastQuery = 0;
            s.answeredSinceQuery = false;
            s.misses = 0;
            return &s;
        }
    }
    return nullptr;
}
static void eraseChild(Child &c) { memset(&c, 0, sizeof(c)); }
static int numChildren()
{
    int n = 0;
    for (auto &c : children)
        if (c.id)
            ++n;
    return n;
}

struct PendingJoin
{
    uint8_t id = 0;
    uint32_t nextTry = 0;
    uint8_t tries = 0;
    uint32_t lastSeen = 0;
};
static PendingJoin pend[MAX_PENDING_JOINS];

static PendingJoin *findPending(uint8_t id)
{
    for (auto &p : pend)
        if (p.id == id)
            return &p;
    return nullptr;
}
static PendingJoin *allocPending(uint8_t id)
{
    if (auto *p = findPending(id))
        return p;
    for (auto &slot : pend)
        if (!slot.id)
        {
            slot.id = id;
            return &slot;
        }
    return nullptr;
}
static void removePending(uint8_t id)
{
    for (auto &p : pend)
        if (p.id == id)
        {
            memset(&p, 0, sizeof(p));
            return;
        }
}

constexpr uint8_t MAX_PENDING_QUERIES = 32;
struct PendingQuery
{
    uint8_t id = 0;
    uint32_t nextTry = 0;
    uint8_t tries = 0;
};
static PendingQuery pq[MAX_PENDING_QUERIES];

static PendingQuery *findPendingQuery(uint8_t id)
{
    for (auto &x : pq)
        if (x.id == id)
            return &x;
    return nullptr;
}
static PendingQuery *allocPendingQuery(uint8_t id)
{
    if (auto *x = findPendingQuery(id))
        return x;
    for (auto &slot : pq)
        if (!slot.id)
        {
            slot.id = id;
            return &slot;
        }
    return nullptr;
}
static void removePendingQuery(uint8_t id)
{
    for (auto &x : pq)
        if (x.id == id)
        {
            memset(&x, 0, sizeof(x));
            return;
        }
}
static bool isPendingQuery(uint8_t id) { return findPendingQuery(id) != nullptr; }

static int16_t sendPacket(uint8_t dst, MsgType type,
                          const uint8_t *pl = nullptr, uint8_t len = 0)
{
    MeshHeader h{HDR_MAGIC, GW_ID, dst, 0, type, len};
    uint8_t buf[sizeof(MeshHeader) + MAX_PAYLOAD];
    uint8_t L = (len > MAX_PAYLOAD) ? (uint8_t)MAX_PAYLOAD : len;
    memcpy(buf, &h, sizeof(h));
    if (L)
        memcpy(buf + sizeof(h), pl, L);
    int16_t st = transmitWithDC(buf, sizeof(h) + L);
    if (st == ERR_TX_DEFERRED)
        return st;
    if (st != RADIOLIB_ERR_NONE)
        Serial.printf("TX err %d\n", st);
    return st;
}

static bool trySendJoinAck(uint8_t id)
{
    uint32_t now = millis();
    if (Child *c = findChild(id))
    {
        if (now - c->lastJoinAck < JOIN_ACK_GAP_MS)
            return false;
    }
    uint8_t seq = 0;
    int16_t st = sendPacket(id, JOIN_ACK, &seq, 1);
    if (st == RADIOLIB_ERR_NONE)
    {
        Serial.println("sent ack fr");
        Child *c = allocChild(id);
        if (c)
        {
            c->parent = GW_ID;
            c->hops = 1;
            c->misses = 0;
            c->lastSeen = now;
            c->lastJoinAck = now;
            c->answeredSinceQuery = true;
        }
        removePending(id);
        return true;
    }
    if (auto *p = allocPending(id))
    {
        uint32_t slack = 50;
        if (st == ERR_TX_DEFERRED)
            p->nextTry = (dc_free_at + slack);
        else
            p->nextTry = now + JOIN_ACK_GAP_MS;
        p->tries = (uint8_t)std::min<uint8_t>(p->tries + 1, 200);
    }
    return false;
}

static bool trySendQuery(Child &c)
{
    const uint32_t now = millis();
    int16_t st = sendPacket(c.id, QUERY);
    if (st == RADIOLIB_ERR_NONE)
    {
        c.lastQuery = now;
        c.answeredSinceQuery = false;
        removePendingQuery(c.id);
        return true;
    }
    if (auto *q = allocPendingQuery(c.id))
    {
        uint32_t slack = 50;
        q->nextTry = (st == ERR_TX_DEFERRED) ? (dc_free_at + slack) : (now + 50);
        q->tries = (uint8_t)std::min<uint8_t>(q->tries + 1, 200);
    }
    return false;
}

void meshSetupGateway()
{
    oledPrintfLines(0, 0, 12, "Gateway ready\nID 00");
    Serial.printf("MeshHeader=%u bytes\n", (unsigned)sizeof(MeshHeader));
    radio.startReceive();
}

static void handleRx()
{
    size_t pktLen = radio.getPacketLength();
    const size_t MAX_FRAME = sizeof(MeshHeader) + MAX_PAYLOAD;
    if (pktLen == 0 || pktLen > MAX_FRAME)
    {
        uint8_t scratch[32];
        (void)radio.readData(scratch, sizeof(scratch));
        return;
    }
    std::unique_ptr<uint8_t[]> buf(new uint8_t[pktLen]);
    int16_t rc = radio.readData(buf.get(), pktLen);
    if (rc == RADIOLIB_ERR_RX_TIMEOUT)
        return;
    if (rc != RADIOLIB_ERR_NONE)
    {
        Serial.printf("RX err %d\n", rc);
        radio.startReceive();
        return;
    }
    auto *h = reinterpret_cast<MeshHeader *>(buf.get());
    if (h->magic != HDR_MAGIC)
        return;
    const uint32_t now = millis();
    const int16_t rssi = radio.getRSSI();

    switch (h->type)
    {
    case JOIN_REQ:
    {
        if (auto *p = allocPending(h->src))
        {
            p->lastSeen = now;
            if (now >= p->nextTry)
                (void)trySendJoinAck(h->src);
        }
        if (Child *c = findChild(h->src))
        {
            c->lastSeen = now;
            c->lastRssi = rssi;
            c->misses = 0;
            c->answeredSinceQuery = true;
        }
        break;
    }

    case DATA_UP:
    {
        if (Child *c = allocChild(h->src))
        {
            c->lastSeen = now;
            c->lastRssi = rssi;
            c->misses = 0;
            c->answeredSinceQuery = true;
        }
        sendPacket(h->src, DATA_ACK);
        break;
    }

    case STATE:
    {
        auto *p = reinterpret_cast<StatusPayload *>(buf.get());
        if (Child *c = allocChild(h->src))
        {
            c->misses = 0;
            c->lastQuery = 0;
            c->answeredSinceQuery = true;
            c->lastSeen = now;
            c->lastRssi = rssi;
            c->parent = p->parent;
            c->hops = p->hops;
        }
        break;
    }

    case (MsgType)MSG_CHILD_ADD:
    {
        auto *ev = reinterpret_cast<ChildEventPayload *>(buf.get());
        if (Child *gc = allocChild(ev->child))
        {
            gc->parent = ev->parent;
            gc->hops = ev->hops;
            gc->lastSeen = now;
            gc->misses = 0;
            gc->answeredSinceQuery = true;
        }
        removePending(ev->child);
        break;
    }

    case (MsgType)MSG_CHILD_GONE:
    {
        auto *ev = reinterpret_cast<ChildEventPayload *>(buf.get());
        if (Child *gc = findChild(ev->child))
            eraseChild(*gc);
        removePending(ev->child);
        break;
    }

    default:
    {
        if (Child *c = findChild(h->src))
        {
            c->lastSeen = now;
            c->lastRssi = rssi;
            c->misses = 0;
            c->answeredSinceQuery = true;
        }
        break;
    }
    }
}

static uint32_t lastBeacon = 0, lastQueryRound = 0;
void meshLoopGateway()
{
    handleRx();
    uint32_t now = millis();

    for (auto &p : pend)
    {
        if (!p.id)
            continue;
        if (now >= p.nextTry)
            (void)trySendJoinAck(p.id);
    }

    for (auto &q : pq)
    {
        if (!q.id)
            continue;
        if (now >= q.nextTry)
        {
            if (Child *c = findChild(q.id))
                (void)trySendQuery(*c);
            else
                removePendingQuery(q.id);
        }
    }

    for (auto &c : children)
    {
        if (c.id && now - c.lastSeen > CHILD_TIMEOUT_MS)
            eraseChild(c);
    }

    if (now - lastQueryRound > QUERY_PERIOD_MS)
    {
        for (auto &c : children)
        {
            if (!c.id)
                continue;
            if (c.lastQuery || isPendingQuery(c.id))
                continue;
            (void)trySendQuery(c);
        }
        lastQueryRound = now;
    }

    for (auto &c : children)
    {
        if (!c.id)
            continue;
        if (c.lastQuery && (now - c.lastQuery > QUERY_TIMEOUT_MS))
        {
            bool unanswered = !c.answeredSinceQuery;
            c.lastQuery = 0;
            c.answeredSinceQuery = false;
            if (unanswered)
            {
                if (++c.misses > MAX_MISSES)
                    eraseChild(c);
            }
        }
    }

    if (numChildren() == 0 && now - lastBeacon > BEACON_PERIOD_MS)
    {
        uint8_t seq = 0;
        (void)sendPacket(0xFF, BEACON, &seq, 1);
        lastBeacon = now;
    }

    static uint32_t lastStat = 0;
    if (now - lastStat > 5000)
    {
        int16_t worst = 0;
        for (auto &c : children)
            if (c.id && c.lastRssi < worst)
                worst = c.lastRssi;
        oledPrintfLines(0, 10, 12, "Nodes:%u\nWorst:%ddBm", numChildren(), worst);

        Serial.println(F("\nID  P  H  RSSI  Age(ms)  Miss  Pending"));
        Serial.println(F("---------------------------------------"));
        for (auto &c : children)
            if (c.id)
            {
                bool pending = (c.lastQuery != 0);
                Serial.printf("%02X  %02X  %u  %4d  %7lu  %4u   %c\n",
                              c.id, c.parent, c.hops, c.lastRssi,
                              (unsigned long)(now - c.lastSeen), c.misses, pending ? 'Y' : 'N');
            }

        bool anyPend = false;
        for (auto &p : pend)
            if (p.id)
            {
                anyPend = true;
                break;
            }
        if (anyPend)
        {
            Serial.println(F("\nPENDING JOINS: id  tries  due(ms)"));
            for (auto &p : pend)
                if (p.id)
                {
                    long due = (long)p.nextTry - (long)now;
                    if (due < 0)
                        due = 0;
                    Serial.printf("               %02X   %3u   %ld\n", p.id, p.tries, due);
                }
        }

        bool anyPQ = false;
        for (auto &q : pq)
            if (q.id)
            {
                anyPQ = true;
                break;
            }
        if (anyPQ)
        {
            Serial.println(F("\nPENDING QUERIES: id  tries  due(ms)"));
            for (auto &q : pq)
                if (q.id)
                {
                    long due = (long)q.nextTry - (long)now;
                    if (due < 0)
                        due = 0;
                    Serial.printf("                  %02X   %3u   %ld\n", q.id, q.tries, due);
                }
        }

        lastStat = now;
    }
}

#endif
