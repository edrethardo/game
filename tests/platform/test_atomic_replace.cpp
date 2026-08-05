// Platform::atomicReplace — the save-promotion primitive.
//
// Every character save is written to a temp file and then promoted over the real slot by this
// function, and `saveCharacter` treats a false return as "keep the previous save". That makes a
// replace failure INVISIBLE: the game keeps running, the old file stays on disk, and the player
// only finds out when they reload and their progress is gone. Which is exactly what happened on
// Switch — libnx writes to a FAT32 SD card where `rename` onto an EXISTING file fails, so the first
// save of a slot worked and every one after it silently did nothing.
//
// The contract these tests pin: promoting over an existing file must SUCCEED and must leave the new
// contents, and a failed promotion must never destroy the file that was already there.

#include <doctest/doctest.h>
#include "platform/user_paths.h"

#include <cstdio>
#include <string>

namespace {

std::string tmpDir() {
    const char* d = std::getenv("TMPDIR");
    return std::string(d && *d ? d : "/tmp");
}

void writeFile(const std::string& path, const char* text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fputs(text, f);
    std::fclose(f);
}

std::string readFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    char buf[256] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    return std::string(buf, n);
}

bool exists(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

} // namespace

TEST_CASE("atomicReplace creates the destination when none exists") {
    const std::string src = tmpDir() + "/de_ar_src_a.tmp";
    const std::string dst = tmpDir() + "/de_ar_dst_a.dat";
    std::remove(dst.c_str());
    writeFile(src, "NEW");

    CHECK(Platform::atomicReplace(src.c_str(), dst.c_str()));
    CHECK(readFile(dst) == "NEW");
    CHECK_FALSE(exists(src));          // the temp is consumed, not copied

    std::remove(dst.c_str());
}

TEST_CASE("atomicReplace OVERWRITES an existing destination") {
    // THE regression test. On a filesystem without rename-over-replace this returned false and left
    // the old contents in place — a save that reports success and changes nothing.
    const std::string src = tmpDir() + "/de_ar_src_b.tmp";
    const std::string dst = tmpDir() + "/de_ar_dst_b.dat";
    writeFile(dst, "OLD-SAVE");
    writeFile(src, "NEW-SAVE");

    CHECK(Platform::atomicReplace(src.c_str(), dst.c_str()));
    CHECK(readFile(dst) == "NEW-SAVE");
    CHECK_FALSE(exists(src));

    std::remove(dst.c_str());
    std::remove((dst + ".bak").c_str());
}

TEST_CASE("repeated replaces keep working — the Switch failure was the SECOND save on") {
    // The bug only showed from the second save of a slot onward, because the first had no
    // destination to collide with. One round trip would have passed happily.
    const std::string src = tmpDir() + "/de_ar_src_c.tmp";
    const std::string dst = tmpDir() + "/de_ar_dst_c.dat";
    std::remove(dst.c_str());

    for (int i = 0; i < 5; i++) {
        const std::string payload = "SAVE-" + std::to_string(i);
        writeFile(src, payload.c_str());
        CAPTURE(i);
        REQUIRE(Platform::atomicReplace(src.c_str(), dst.c_str()));
        CHECK(readFile(dst) == payload);
    }

    std::remove(dst.c_str());
    std::remove((dst + ".bak").c_str());
}

TEST_CASE("a failed replace leaves the existing save intact") {
    // Source does not exist, so the promotion cannot succeed. The destination — the player's actual
    // save — must survive untouched, and no stray .bak may be left behind holding it hostage.
    const std::string dst = tmpDir() + "/de_ar_dst_d.dat";
    writeFile(dst, "PRECIOUS");

    CHECK_FALSE(Platform::atomicReplace((tmpDir() + "/de_ar_does_not_exist.tmp").c_str(),
                                        dst.c_str()));
    CHECK(readFile(dst) == "PRECIOUS");

    std::remove(dst.c_str());
    std::remove((dst + ".bak").c_str());
}
