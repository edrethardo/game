// UPnP IGD port-mapping helper used by Net::hostServer to auto-open the game's
// UDP port on the user's router so friends on the internet can join without
// manual port-forwarding. Wraps miniupnpc in a tiny, single-purpose API that
// can be linked from any platform — Switch builds compile to a no-op stub via
// the #if defined(UPNP_AVAILABLE) gate in upnp.cpp (the macro is defined in
// src/CMakeLists.txt only for non-Switch targets).
//
// Lifecycle:
//   1. After ENet's host is up, Net::hostServer calls Upnp::tryAddPortMapping(...)
//      with the bound port. Blocking call — typical 1 s wait for SSDP discovery.
//   2. The lobby reads currentExternalIp() / lastError() to display result.
//   3. On Net::shutdown / disconnect, Upnp::removePortMapping() tears the entry
//      down so the router doesn't leak the mapping past the game session.

#pragma once

#include "core/types.h"

namespace Upnp {

// Attempts to discover an IGD on the LAN and request a UDP port-mapping for
// `port` → `port`. Blocking; `discoveryTimeoutMs` caps the SSDP wait window
// (1000 ms is typically enough for a router on the same subnet).
//
// On success:  returns true, fills `externalIp` with the WAN-side address
//              the router reports.
// On failure:  returns false, fills `errorMsg` with a short human-readable
//              cause ("no IGD found", "AddPortMapping failed: <code>", ...).
// Safe to call when no IGD exists — does NOT block longer than the timeout.
// On Switch (no UPnP support) this is a no-op stub that always returns false
// with the error string "UPnP unsupported on this platform".
bool tryAddPortMapping(u16 port,
                       u32 discoveryTimeoutMs,
                       char externalIp[64],
                       char errorMsg[128]);

// Removes the mapping previously installed by tryAddPortMapping. No-op when
// no mapping is currently held. Called from Net::shutdown / disconnect.
void removePortMapping();

// Lobby/UI accessors. Empty string on Switch or when no mapping is held.
const char* currentExternalIp();
const char* lastError();

} // namespace Upnp
