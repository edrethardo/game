#pragma once

// lag_comp.h — Pure helpers translating a client's REPORTED interpolation delay into the
// server-side rewind used to reconstruct that client's view of the world.
//
// WHY THIS EXISTS
// ---------------
// The client predicts its own movement by colliding against the *interpolated* entity pool
// (m_renderInterp.entities), which it samples at `now - s_interpDelaySec`. That delay is
// ADAPTIVE: it starts at INTERP_DELAY_SEC (33 ms) and widens with snapshot-arrival jitter,
// up to 150 ms (client.cpp, interp_delay.h).
//
// The server, replaying that same input, has to collide the player against the SAME view of
// the world — otherwise the two disagree about where a moving enemy was, moveAndSlide produces
// a different position, and the mismatch fires the reconcile path (felt as jitter near moving
// enemies). Before this header, three different places each hardcoded their OWN guess at the
// client's delay:
//
//   engine_net.cpp   INTERP_DELAY_TICKS = 2   (33 ms)   — movement rewind
//   engine_combat.cpp INTERP_TICKS      = 3   (50 ms)   — fire rewind
//   client.cpp       s_interpDelaySec   = 33..150 ms    — what the client ACTUALLY used
//
// So the server was structurally incapable of agreeing with the client the moment jitter
// widened the client's buffer past the hardcoded guess. The fix is to stop guessing: the
// client stamps the delay it actually used into every NetInput (`interpDelayMs`), and the
// server rewinds by exactly that. One source of truth, carried on the wire.
//
// The rewind is FRACTIONAL on purpose. The client interpolates continuously between two
// snapshots, so its view rarely lands on an integer tick; snapping the server to the nearest
// stored history tick would reintroduce up to half a tick of enemy motion as error. Callers
// pair `targetTick()` with a lerping history sampler.

#include "core/types.h"
#include "net/net.h"   // NET_TICK_RATE

namespace LagComp {

// These two are the CANONICAL interp-delay bounds — client.h derives INTERP_DELAY_SEC and
// client.cpp derives MAX_INTERP_DELAY_SEC from them, so the delay the client applies and the
// rewind the server trusts can never drift apart. They live here, in the shared contract,
// rather than in client.h precisely because the server depends on them too.

// Baseline (and the fallback for a client that doesn't stamp the field). A zero rewind would
// collide the replay against LIVE enemy poses — the very mismatch this file exists to remove,
// and worse than the old hardcoded guess — so 0 on the wire means "use the baseline".
inline constexpr u8 DEFAULT_INTERP_DELAY_MS = 33;

// Upper bound on how wide the client's jitter buffer may grow. Also the trust ceiling: a
// malicious or corrupt client could otherwise claim a huge delay and rewind enemies far into
// the past, letting it walk through where they used to be. Clamp, don't trust.
inline constexpr u8 MAX_INTERP_DELAY_MS = 150;

// Quantize a delay in seconds to the wire byte (used by the client when stamping an input).
inline u8 toWireMs(f32 delaySec) {
    f32 ms = delaySec * 1000.0f;
    if (ms < 0.0f) ms = 0.0f;
    if (ms > static_cast<f32>(MAX_INTERP_DELAY_MS)) ms = static_cast<f32>(MAX_INTERP_DELAY_MS);
    return static_cast<u8>(ms + 0.5f);
}

// Sanitize a received delay byte: 0 (absent) -> legacy baseline, anything above the client's
// own cap -> clamped.
inline u8 sanitize(u8 interpDelayMs) {
    if (interpDelayMs == 0) return DEFAULT_INTERP_DELAY_MS;
    if (interpDelayMs > MAX_INTERP_DELAY_MS) return MAX_INTERP_DELAY_MS;
    return interpDelayMs;
}

// How many server ticks behind the latest snapshot the client's rendered (and collided-against)
// view sits. Fractional — see the header comment.
inline f32 rewindTicks(u8 interpDelayMs) {
    constexpr f32 TICKS_PER_MS = static_cast<f32>(NET_TICK_RATE) / 1000.0f;   // 0.06
    return static_cast<f32>(sanitize(interpDelayMs)) * TICKS_PER_MS;
}

// The fractional server tick whose world-state the client was looking at when it captured an
// input that acked snapshot `ackedSnapTick`. Floors at 0 — right after a join/floor reset the
// history ring holds only a couple of ticks, and rewinding below 0 has no meaning.
inline f32 targetTick(u32 ackedSnapTick, u8 interpDelayMs) {
    f32 t = static_cast<f32>(ackedSnapTick) - rewindTicks(interpDelayMs);
    return (t < 0.0f) ? 0.0f : t;
}

} // namespace LagComp
