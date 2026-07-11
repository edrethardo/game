// input_wire.cpp — Pure serialize/deserialize helpers for the rolling CL_INPUT window.
//
// Isolated from client.cpp so the test binary (tests/CMakeLists.txt) can link only
// this file and net_player.h without pulling in the full net/client.cpp dependency chain.
//
// Wire layout per input (15 B):
//   u32 clientTick  + u16 ackedSnapshotTick + u8 moveFlags + u8 weaponId
//   + u16 yawQ + u16 pitchQ + u8 extFlags + u8 skillSlot + u8 interpDelayMs
// Packet payload layout (4-byte header + N*15 B body):
//   u8  windowCount   (how many inputs follow, 1–INPUT_WINDOW_SIZE)
//   u8  reserved = 0
//   u16 reserved = 0
//   [N × 15-byte input structs in oldest→newest order]

#include "net/net_player.h"
#include "core/types.h"

// Wire sizes (must stay in sync with the struct field layout above)
static constexpr u32 INPUT_BYTES  = 15;  // wire size of one serialized NetInput
static constexpr u32 HEADER_BYTES =  4;  // windowCount (1) + reserved (3)

u32 serializeInputWindow(u8* outBuf, u32 outCap, const NetInput* inputs, u32 count, u8 targetSlot) {
    if (count > INPUT_WINDOW_SIZE) count = INPUT_WINDOW_SIZE;
    const u32 total = HEADER_BYTES + count * INPUT_BYTES;
    if (total > outCap) return 0;  // caller's buffer too small — safe no-op

    u32 o = 0;
    outBuf[o++] = static_cast<u8>(count);
    outBuf[o++] = targetSlot;  // online couch co-op: absolute target net slot (server validates owner)
    outBuf[o++] = 0;  // reserved
    outBuf[o++] = 0;  // reserved

    for (u32 i = 0; i < count; i++) {
        const NetInput& in = inputs[i];
        // clientTick — u32, little-endian
        outBuf[o++] = static_cast<u8>( in.clientTick        & 0xFF);
        outBuf[o++] = static_cast<u8>((in.clientTick >>  8) & 0xFF);
        outBuf[o++] = static_cast<u8>((in.clientTick >> 16) & 0xFF);
        outBuf[o++] = static_cast<u8>((in.clientTick >> 24) & 0xFF);
        // ackedSnapshotTick — u16, little-endian
        outBuf[o++] = static_cast<u8>( in.ackedSnapshotTick       & 0xFF);
        outBuf[o++] = static_cast<u8>((in.ackedSnapshotTick >> 8) & 0xFF);
        // single-byte fields
        outBuf[o++] = in.moveFlags;
        outBuf[o++] = in.weaponId;
        // yawQ / pitchQ — u16, little-endian
        outBuf[o++] = static_cast<u8>( in.yawQ       & 0xFF);
        outBuf[o++] = static_cast<u8>((in.yawQ >> 8) & 0xFF);
        outBuf[o++] = static_cast<u8>( in.pitchQ       & 0xFF);
        outBuf[o++] = static_cast<u8>((in.pitchQ >> 8) & 0xFF);
        // single-byte extended fields
        outBuf[o++] = in.extFlags;
        outBuf[o++] = in.skillSlot;
        // The client's ACTUAL interp delay for this input — the server rewinds enemies by it
        // when replaying, so both sides collide against the same world (net/lag_comp.h).
        outBuf[o++] = in.interpDelayMs;
    }
    return total;
}

u32 deserializeInputWindow(const u8* buf, u32 size, NetInput* outInputs, u32 maxCount) {
    if (size < HEADER_BYTES) return 0;  // too short to even hold the count byte

    const u32 count = buf[0];
    // Reject zero (nothing to do) or implausibly large counts (guard against corrupt packets)
    if (count == 0 || count > INPUT_WINDOW_SIZE) return 0;
    // Ensure the claimed number of inputs actually fits in the supplied buffer
    if (size < HEADER_BYTES + count * INPUT_BYTES) return 0;

    const u32 toRead = (count < maxCount) ? count : maxCount;
    u32 o = HEADER_BYTES;  // skip the 4-byte header
    for (u32 i = 0; i < toRead; i++) {
        NetInput& out = outInputs[i];
        out.clientTick = static_cast<u32>(buf[o])
                       | (static_cast<u32>(buf[o+1]) <<  8)
                       | (static_cast<u32>(buf[o+2]) << 16)
                       | (static_cast<u32>(buf[o+3]) << 24);
        o += 4;
        out.ackedSnapshotTick = static_cast<u16>(buf[o])
                              | (static_cast<u16>(buf[o+1]) << 8);
        o += 2;
        out.moveFlags = buf[o++];
        out.weaponId  = buf[o++];
        out.yawQ  = static_cast<u16>(buf[o]) | (static_cast<u16>(buf[o+1]) << 8); o += 2;
        out.pitchQ= static_cast<u16>(buf[o]) | (static_cast<u16>(buf[o+1]) << 8); o += 2;
        out.extFlags  = buf[o++];
        out.skillSlot = buf[o++];
        out.interpDelayMs = buf[o++];   // 0 = unstamped; LagComp::sanitize maps it to the baseline
    }
    return toRead;
}
