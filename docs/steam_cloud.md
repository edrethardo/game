# Steam Cloud (Auto-Cloud) — savegame sync

The game syncs saves/progression/settings via **Steam Auto-Cloud** (no Steamworks SDK linked). The
game just writes user data into the per-user directory `SDL_GetPrefPath(ORG, APP)`; Steam mirrors the
matching files to the cloud on launch/exit. This file records the exact partner-site configuration so
the code and Steamworks stay in agreement.

## Frozen identity — MUST match on both sides

`src/platform/user_paths.cpp` calls `SDL_GetPrefPath("EdRethardo", "DungeonEngine")`, which resolves to:

| OS | Directory | Steam Auto-Cloud Root |
|----|-----------|-----------------------|
| Windows | `%APPDATA%\EdRethardo\DungeonEngine\` (Roaming) | `WinAppDataRoaming` |
| Linux / Steam Deck | `~/.local/share/EdRethardo/DungeonEngine/` | `LinuxXdgDataHome` |
| macOS | `~/Library/Application Support/EdRethardo/DungeonEngine/` | `MacAppSupport` |

**Auto-Cloud Subdirectory (all three roots):** `EdRethardo/DungeonEngine`

⚠️ **Do not change `ORG`/`APP`** (`user_paths.cpp`) after release — they are the on-disk folder AND the
Steamworks subdirectory. Changing them orphans every existing player's saves.

## Steamworks partner-site steps (Part B — no code)

Requires a Steam **App ID** / partner access.

1. **App Admin → Cloud:** enable Steam Cloud. Quota (saves are tiny): **Bytes = 100 MB**, **Files = 256**.
2. **Cloud → Auto-Cloud:** add the three root mappings above, each with Subdirectory
   `EdRethardo/DungeonEngine`.
3. **Path patterns** (per mapping) — sync exactly these:
   - `save_*.dat`
   - `difficulty_unlock.dat`
   - `menagerie.dat`   (pet-collection progress — profile-wide, should roam like unlocks)
   - `controls.json`
   - `audio.json`
   - **NOT** `video.cfg` — fullscreen/display index are machine-specific and must not roam.
4. **Publish** the Steamworks changes (Cloud + Auto-Cloud only go live after publishing).

Proton/Deck note: we ship native Linux/Windows/macOS, so each build uses its native root. If a Windows
build is ever run via Proton, its `%APPDATA%` lives in the Proton prefix and `WinAppDataRoaming` covers it.

## What the code guarantees (save-loss safety)

- **Relocation is migrated, never destructive.** `Platform::migrateLegacyUserData()` (run once in
  `Engine::init`) copies pre-relocation files into the pref dir only when the destination is **absent**,
  searching both the working dir AND the executable dir (`SDL_GetBasePath`) since Steam/launchers don't
  reliably set CWD to the install dir. It never overwrites or deletes existing data.
- **Atomic saves.** `Engine::saveCharacter` writes to `save_NN.dat.tmp` and atomically replaces the slot
  (`Platform::atomicReplace` → POSIX `rename` / Win `MoveFileEx`), checking `ferror` first. A crash /
  power loss / disk-full during a save leaves the previous save intact — the temp is simply not promoted.
- **Format unchanged.** Only the directory changed; `SAVE_VERSION` is untouched and existing saves load.

## Testing

1. Local (no Steam): launch, confirm the log line `User data dir: <path>`; play/save/quit; confirm
   `save_NN.dat` appears under the pref dir; `ffprobe`-free — just `ls` the dir.
2. Cloud round-trip (needs the depot): install via Steam, play → exit → watch the Steam Cloud sync
   status; on a second machine (or after clearing local) confirm `save_*.dat`, `difficulty_unlock.dat`, `menagerie.dat`,
   `controls.json`, `audio.json` re-download and `video.cfg` does **not**.
