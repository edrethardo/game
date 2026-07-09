// Per-user data directory helper — see user_paths.h. Uses SDL_GetPrefPath so desktop user data
// (saves / progression / settings) lives in the platform-standard, Steam-Auto-Cloud-mappable
// location instead of the install/working directory. ORG/APP are FROZEN: they are the on-disk
// folder name AND the Steamworks Auto-Cloud subdirectory — changing them orphans existing saves.
#include "platform/user_paths.h"
#include "core/log.h"

#include <cstdio>

#ifndef __SWITCH__
#include <SDL.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace Platform {

// MUST match the Steamworks Auto-Cloud "Subdirectory" (EdRethardo/DungeonEngine). Do not change.
static const char* USER_DATA_ORG = "EdRethardo";
static const char* USER_DATA_APP = "DungeonEngine";

const char* userDataDir() {
#ifdef __SWITCH__
    return "";  // CWD is the app's writable storage on Switch — no relocation
#else
    static char s_dir[512] = {0};
    static bool s_resolved = false;
    if (!s_resolved) {
        s_resolved = true;
        // SDL_GetPrefPath creates the dir and returns a newly-allocated string (trailing separator).
        char* pref = SDL_GetPrefPath(USER_DATA_ORG, USER_DATA_APP);
        if (pref) {
            std::snprintf(s_dir, sizeof(s_dir), "%s", pref);
            SDL_free(pref);
            LOG_INFO("User data dir: %s", s_dir);
        } else {
            // Fall back to CWD-relative filenames so the game still runs.
            s_dir[0] = '\0';
            LOG_WARN("SDL_GetPrefPath failed — user data falls back to the working directory");
        }
    }
    return s_dir;
#endif
}

const char* userDataPath(const char* filename, char* buf, u32 bufSize) {
    std::snprintf(buf, bufSize, "%s%s", userDataDir(), filename);
    return buf;
}

bool atomicReplace(const char* srcPath, const char* dstPath) {
#ifdef _WIN32
    // std::rename fails if the target exists on Windows; MoveFileEx replaces atomically.
    return MoveFileExA(srcPath, dstPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(srcPath, dstPath) == 0;  // POSIX rename atomically replaces
#endif
}

#ifndef __SWITCH__
// Byte-for-byte copy src -> dst. Returns true on success.
static bool copyFile(const char* src, const char* dst) {
    FILE* in = std::fopen(src, "rb");
    if (!in) return false;
    FILE* out = std::fopen(dst, "wb");
    if (!out) { std::fclose(in); return false; }
    char chunk[4096];
    size_t n;
    bool ok = true;
    while ((n = std::fread(chunk, 1, sizeof(chunk), in)) > 0) {
        if (std::fwrite(chunk, 1, n, out) != n) { ok = false; break; }
    }
    std::fclose(in);
    std::fclose(out);
    return ok;
}

static bool fileExists(const char* p) {
    FILE* f = std::fopen(p, "rb");
    if (f) { std::fclose(f); return true; }
    return false;
}

// Directory of the running executable (trailing separator), cached. Empty on failure.
static const char* exeDir() {
    static char s_dir[512] = {0};
    static bool s_resolved = false;
    if (!s_resolved) {
        s_resolved = true;
        char* base = SDL_GetBasePath();
        if (base) { std::snprintf(s_dir, sizeof(s_dir), "%s", base); SDL_free(base); }
    }
    return s_dir;
}

// Copy the first existing legacy source into userDataDir/<destName>, but ONLY when the destination
// is absent — migration NEVER overwrites a file already in the user dir, so it cannot destroy
// relocated data. We try the working directory AND the executable's directory because Steam and
// other launchers do not reliably set CWD to the install dir; the pre-relocation build wrote saves
// next to wherever it ran, so checking both avoids orphaning an existing player's saves.
static void migrateInto(const char* destName, const char* legacyRel) {
    char dst[512];
    userDataPath(destName, dst, sizeof(dst));
    if (fileExists(dst)) return;                       // destination already present — leave it
    char src[600];
    std::snprintf(src, sizeof(src), "%s", legacyRel);  // 1) working-directory-relative
    if (fileExists(src) && copyFile(src, dst)) { LOG_INFO("Migrated %s -> %s", src, dst); return; }
    const char* base = exeDir();                        // 2) executable-directory-relative
    if (base[0]) {
        std::snprintf(src, sizeof(src), "%s%s", base, legacyRel);
        if (fileExists(src) && copyFile(src, dst)) { LOG_INFO("Migrated %s -> %s", src, dst); return; }
    }
}
#endif  // !__SWITCH__

void migrateLegacyUserData(u32 maxSaveSlots) {
#ifdef __SWITCH__
    (void)maxSaveSlots;  // Switch already writes to CWD storage — nothing to migrate
#else
    if (userDataDir()[0] == '\0') return;  // no pref dir resolved — nothing to migrate into
    char name[64];
    for (u32 i = 1; i <= maxSaveSlots; i++) {
        std::snprintf(name, sizeof(name), "save_%02u.dat", i);
        migrateInto(name, name);
    }
    migrateInto("difficulty_unlock.dat", "difficulty_unlock.dat");
    migrateInto("controls.json", "assets/config/controls.json");
    migrateInto("audio.json",    "assets/config/audio.json");
    migrateInto("video.cfg",     "assets/config/video.cfg");
#endif
}

}  // namespace Platform
