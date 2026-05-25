#pragma once

#include "core/types.h"
#include "core/math.h"
#include <cstring>

static constexpr u32 MAX_PACKET_SIZE = 4096;

// Snapshot payloads can exceed one UDP datagram and are sent via ENet's
// unreliable-fragment path (Net::broadcastSnapshot), so they are NOT bounded by
// MAX_PACKET_SIZE. 8 KB holds all 4 players + 128 entities + ~304 projectiles
// (29 fixed + 4*31 + 128*20 = 2713 B; remaining 5479 B / 18 B = 304 projectiles),
// which covers a Frozen-Orb storm during a boss fight. Loads above this are
// priority-dropped by the serializer (projectiles first, nearest-player kept) so
// the packet's declared counts always match the bytes present. Kept modest on
// purpose: 8 KB is ~6 MTU fragments, and a single lost fragment drops the whole
// snapshot — a larger buffer would raise per-snapshot loss under real packet loss.
static constexpr u32 MAX_SNAPSHOT_SIZE = 8192;

struct PacketWriter {
    u8  data[MAX_PACKET_SIZE];
    u32 cursor = 0;

    bool hasSpace(u32 bytes) const { return cursor + bytes <= MAX_PACKET_SIZE; }

    void writeU8(u8 v)   { if (hasSpace(1)) data[cursor++] = v; }
    void writeU16(u16 v) { if (hasSpace(2)) { std::memcpy(data + cursor, &v, 2); cursor += 2; } }
    void writeU32(u32 v) { if (hasSpace(4)) { std::memcpy(data + cursor, &v, 4); cursor += 4; } }
    void writeS16(s16 v) { if (hasSpace(2)) { std::memcpy(data + cursor, &v, 2); cursor += 2; } }
    void writeF32(f32 v) { if (hasSpace(4)) { std::memcpy(data + cursor, &v, 4); cursor += 4; } }

    void writeVec3(Vec3 v) {
        writeF32(v.x); writeF32(v.y); writeF32(v.z);
    }

    void writeBytes(const u8* src, u32 count) {
        if (hasSpace(count)) { std::memcpy(data + cursor, src, count); cursor += count; }
    }
};

struct PacketReader {
    const u8* data;
    u32       size;
    u32       cursor = 0;

    bool hasData(u32 bytes) const { return cursor + bytes <= size; }

    u8 readU8()   { u8  v = 0; if (hasData(1)) { v = data[cursor++]; } return v; }
    u16 readU16() { u16 v = 0; if (hasData(2)) { std::memcpy(&v, data + cursor, 2); cursor += 2; } return v; }
    u32 readU32() { u32 v = 0; if (hasData(4)) { std::memcpy(&v, data + cursor, 4); cursor += 4; } return v; }
    s16 readS16() { s16 v = 0; if (hasData(2)) { std::memcpy(&v, data + cursor, 2); cursor += 2; } return v; }
    f32 readF32() { f32 v = 0; if (hasData(4)) { std::memcpy(&v, data + cursor, 4); cursor += 4; } return v; }

    Vec3 readVec3() {
        Vec3 v;
        v.x = readF32(); v.y = readF32(); v.z = readF32();
        return v;
    }

    void readBytes(u8* dst, u32 count) {
        if (hasData(count)) { std::memcpy(dst, data + cursor, count); cursor += count; }
    }
};

// Quantization helpers
namespace Quantize {
    inline u16 packFloat(f32 value, f32 min, f32 max) {
        f32 t = (value - min) / (max - min);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        return static_cast<u16>(t * 65535.0f + 0.5f);
    }

    inline f32 unpackFloat(u16 packed, f32 min, f32 max) {
        return min + (packed / 65535.0f) * (max - min);
    }

    // Position: [-128, 128] range, ~0.004m precision
    static constexpr f32 POS_MIN = -128.0f;
    static constexpr f32 POS_MAX =  128.0f;

    inline u16 packPos(f32 v)       { return packFloat(v, POS_MIN, POS_MAX); }
    inline f32 unpackPos(u16 v)     { return unpackFloat(v, POS_MIN, POS_MAX); }

    // Velocity: [-30, 30] range
    static constexpr f32 VEL_MIN = -30.0f;
    static constexpr f32 VEL_MAX =  30.0f;

    inline u16 packVel(f32 v)       { return packFloat(v, VEL_MIN, VEL_MAX); }
    inline f32 unpackVel(u16 v)     { return unpackFloat(v, VEL_MIN, VEL_MAX); }

    // Angle: [-PI, PI]
    static constexpr f32 ANGLE_MIN = -3.14159265f;
    static constexpr f32 ANGLE_MAX =  3.14159265f;

    inline u16 packAngle(f32 rad)   { return packFloat(rad, ANGLE_MIN, ANGLE_MAX); }
    inline f32 unpackAngle(u16 v)   { return unpackFloat(v, ANGLE_MIN, ANGLE_MAX); }
}
