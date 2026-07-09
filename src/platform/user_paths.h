#pragma once
// Per-user writable data directory for desktop builds. Saves, progression, and settings live here
// (outside the read-only / non-portable install dir) so Steam Auto-Cloud can sync them: the location
// maps 1:1 to Steam's Auto-Cloud roots (Win %APPDATA% -> WinAppDataRoaming, Linux ~/.local/share ->
// LinuxXdgDataHome, macOS ~/Library/Application Support -> MacAppSupport), all under subdirectory
// "<ORG>/<APP>". On Switch this is a no-op (returns "") — the CWD is the app's writable storage, so
// existing Switch behavior is preserved. See docs: store/steam/steam_cloud.md.
#include "core/types.h"

namespace Platform {

// Absolute per-user data dir WITH a trailing separator (created on first use, cached). Desktop:
// SDL_GetPrefPath(ORG, APP). Switch or failure: "" (callers then fall back to CWD-relative names).
const char* userDataDir();

// Writes "<userDataDir><filename>" into buf and returns buf. On Switch / failure this is just
// <filename> (CWD-relative), identical to the legacy behavior.
const char* userDataPath(const char* filename, char* buf, u32 bufSize);

// Atomically replace dstPath with srcPath (both must be on the same filesystem). Used to make save
// writes crash-safe: serialize to a temp file, then replace — so an interrupted write (crash / power
// loss / disk full) can never truncate or corrupt an existing save. Returns true on success.
bool atomicReplace(const char* srcPath, const char* dstPath);

// One-time, best-effort migration of legacy files into userDataDir(): save_NN.dat and
// difficulty_unlock.dat (from the CWD) and controls.json / audio.json / video.cfg (from
// assets/config/). Copies only when the destination is absent, so it never clobbers newer data.
// No-op on Switch or when userDataDir() is empty. Call once at startup before loading any of them.
void migrateLegacyUserData(u32 maxSaveSlots);

}  // namespace Platform
