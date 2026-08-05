#pragma once
// atomic_file.h — promote a freshly-written temp file over a real one.
//
// Split out of user_paths.cpp so it can be unit-tested: that file pulls in SDL for the pref-path
// lookup, the test binary has no SDL, and so the single primitive every character save depends on
// had no test at all. It then shipped a Switch bug that silently discarded every save after the
// first (see the FAT note below), which is precisely the kind of thing a five-line test catches.

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Platform {

// Replace `dstPath` with `srcPath`. Returns false if the destination could not be updated, in which
// case the ORIGINAL destination is left intact — callers (saveCharacter) treat that as "keep the
// previous save", so this must never leave the destination missing or half-written.
//
// SCOPE OF THE `.bak` GUARANTEE, stated honestly: on the FAT fallback path the old file is moved
// aside before the temp is moved into place, so at every instant one of the two exists on disk. If
// the process dies in that window the data is still there — but NOTHING IN THE GAME READS `.bak`,
// so recovery is manual (rename it back), not automatic. It is a last-resort artefact for a human,
// not a restore feature. Wiring a loader fallback would mean deciding when a stale `.bak` is
// preferable to the real file, which is a bigger question than this function should answer.
inline bool atomicReplace(const char* srcPath, const char* dstPath) {
#ifdef _WIN32
    // std::rename fails if the target exists on Windows; MoveFileEx replaces atomically.
    return MoveFileExA(srcPath, dstPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    // POSIX rename replaces an existing destination atomically, so on desktop this is the whole
    // story and the fallback below never runs.
    if (std::rename(srcPath, dstPath) == 0) return true;

    // ...but the SWITCH writes to a FAT32 SD card through libnx, where renaming ONTO an existing
    // file FAILS — the same semantics Windows has and which the branch above already handles. The
    // effect was silent and specific: the FIRST save of a slot worked (no destination yet) and
    // every save after it failed the replace, so saveCharacter dutifully kept the previous file and
    // the player's progress never updated. "Save and quit didn't overwrite my save" is exactly what
    // that looks like from the couch.
    //
    // FAT offers no atomic replace, so the closest safe thing is to move the old file ASIDE rather
    // than delete it: at every instant either the destination or the .bak exists, so an interrupted
    // save leaves something recoverable. Deleting the destination first — the obvious one-liner —
    // has a window in which the only copy of the save is gone.
    // Bail rather than truncate. snprintf would silently cut an over-long path, and two different
    // slots could then collide on the SAME .bak name — one save's backup overwriting another's is a
    // worse outcome than refusing the promotion, which the caller already handles by keeping the
    // previous file.
    char bak[1024];
    const int need = std::snprintf(bak, sizeof(bak), "%s.bak", dstPath);
    if (need < 0 || static_cast<size_t>(need) >= sizeof(bak)) return false;
    std::remove(bak);                                    // stale leftover from an earlier attempt
    const bool hadDst = (std::rename(dstPath, bak) == 0);
    if (std::rename(srcPath, dstPath) != 0) {
        if (hadDst) std::rename(bak, dstPath);           // put the original back, lose nothing
        return false;
    }
    if (hadDst) std::remove(bak);
    return true;
#endif
}

} // namespace Platform
