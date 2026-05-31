// client_time.cpp — clock-sync pong decoder. Split out from client.cpp so the test
// target can link it without pulling in client.cpp's full dependency tree (player,
// weapon, snapshot, glad, etc.). All other Client:: members stay in client.cpp.

#include "net/client.h"
#include "net/packet.h"     // PacketReader
#include "net/clock_sync.h"
#include "core/log.h"

// Decode a SV_TIME_PONG payload and feed its fields to ClockSyncOps::onPongReceived.
//
// Wire layout (12 bytes, no header — caller passes payload bytes only):
//   [0..3]  u32 clientTimeMs  — echo of the client's own send timestamp (ms since epoch start)
//   [4..7]  u32 serverTick    — server's simulation tick at pong send time
//   [8..11] u32 serverTimeMs  — server wall-clock at pong send time (reserved, unused)
//
// RTT = (pongRecvNowSec * 1000) - clientTimeMs.  oneWayTripMs = RTT / 2.
// serverTickEst = serverTick + (oneWayTripMs / 1000) * 60 ticks/s.
void Client::handleTimePong(const u8* data, u32 size, ClockSync& cs, f64 pongRecvNowSec) {
    if (size < 12) {
        LOG_WARN("net: short SV_TIME_PONG (%u bytes)", size);
        return;
    }
    PacketReader r;
    r.data   = data;
    r.size   = size;
    r.cursor = 0;
    const u32 clientTimeMs = r.readU32();
    const u32 serverTick   = r.readU32();
    const u32 serverTimeMs = r.readU32();
    (void)serverTimeMs;   // currently unused; reserved for future wall-time diagnostics

    ClockSyncOps::onPongReceived(cs, clientTimeMs, serverTick, pongRecvNowSec);

    // Log the first few pongs for diagnostics; suppress the rest to avoid log spam at ~1 Hz.
    if (cs.pongsReceived <= 3) {
        LOG_INFO("net: clock-sync pong #%u — oneWayTripMs=%.1f serverTickEst=%.1f",
                 cs.pongsReceived, cs.oneWayTripMs, cs.serverTickEst);
    }
}
