// tests/net/test_lobby_code.cpp
//
// Unit tests for LobbyCode — the 4-glyph shareable lobby code ("K3M7").
//
// This is the one part of join-by-code that can be verified without two live Steam clients, so it's
// worth pinning hard. normalize() is the load-bearing function: its output is handed to Steam as a
// lobby-list string filter and must match the host's published value BYTE FOR BYTE, so a folding or
// casing bug produces a code that looks perfectly valid and silently finds nothing.

#include <doctest/doctest.h>
#include <cstring>
#include "net/lobby_code.h"

TEST_CASE("LobbyCode: generate produces 4 glyphs from the code alphabet only") {
    for (u32 e = 0; e < 2000; e++) {
        char buf[LobbyCode::BUF_SIZE];
        LobbyCode::generate(e * 2654435761u, buf, sizeof(buf));   // spread the entropy around
        REQUIRE(std::strlen(buf) == LobbyCode::CODE_LEN);
        for (const char* p = buf; *p; ++p) {
            CHECK(std::strchr(LobbyCode::ALPHABET, *p) != nullptr);
            // Never emits the ambiguous glyphs the alphabet deliberately omits.
            CHECK(*p != 'I'); CHECK(*p != 'L'); CHECK(*p != 'O'); CHECK(*p != 'U');
        }
    }
}

TEST_CASE("LobbyCode: a generated code normalizes back to itself") {
    // The round-trip that matters: what the host publishes is what a joiner's normalize() must yield.
    for (u32 e = 0; e < 500; e++) {
        char code[LobbyCode::BUF_SIZE];
        LobbyCode::generate(e * 40503u + 7u, code, sizeof(code));
        char norm[LobbyCode::BUF_SIZE];
        REQUIRE(LobbyCode::normalize(code, norm, sizeof(norm)));
        CHECK(std::strcmp(norm, code) == 0);
    }
}

TEST_CASE("LobbyCode: normalize is forgiving about how a human retypes it") {
    char out[LobbyCode::BUF_SIZE];

    // Lowercase.
    REQUIRE(LobbyCode::normalize("k3m7", out, sizeof(out)));
    CHECK(std::strcmp(out, "K3M7") == 0);

    // Separators and whitespace people sprinkle in.
    REQUIRE(LobbyCode::normalize(" K3-M7 ", out, sizeof(out)));
    CHECK(std::strcmp(out, "K3M7") == 0);
    REQUIRE(LobbyCode::normalize("K_3_M_7", out, sizeof(out)));
    CHECK(std::strcmp(out, "K3M7") == 0);
}

TEST_CASE("LobbyCode: normalize folds the glyphs a reader confuses (I/L -> 1, O -> 0)") {
    char out[LobbyCode::BUF_SIZE];
    // Someone reads "1" as I or L, and "0" as O. All must land on the same canonical code.
    REQUIRE(LobbyCode::normalize("10AB", out, sizeof(out)));
    CHECK(std::strcmp(out, "10AB") == 0);
    REQUIRE(LobbyCode::normalize("IOAB", out, sizeof(out)));
    CHECK(std::strcmp(out, "10AB") == 0);
    REQUIRE(LobbyCode::normalize("LoAb", out, sizeof(out)));
    CHECK(std::strcmp(out, "10AB") == 0);
}

TEST_CASE("LobbyCode: normalize rejects malformed input rather than querying for the wrong game") {
    char out[LobbyCode::BUF_SIZE];

    CHECK_FALSE(LobbyCode::normalize(nullptr, out, sizeof(out)));
    CHECK_FALSE(LobbyCode::normalize("", out, sizeof(out)));        // empty
    CHECK_FALSE(LobbyCode::normalize("K3M", out, sizeof(out)));     // too short
    CHECK_FALSE(LobbyCode::normalize("K3M77", out, sizeof(out)));   // too long
    CHECK_FALSE(LobbyCode::normalize("K3M!", out, sizeof(out)));    // invalid symbol
    CHECK_FALSE(LobbyCode::normalize("K3MU", out, sizeof(out)));    // 'U' isn't in the alphabet

    // A rejected code must leave an EMPTY string — never a half-built one we'd go on to query with.
    CHECK(out[0] == '\0');
}

TEST_CASE("LobbyCode: undersized buffers are refused, never half-written") {
    char small[3] = {'x', 'x', 'x'};
    LobbyCode::generate(12345u, small, sizeof(small));
    CHECK(small[0] == '\0');

    char out[3];
    CHECK_FALSE(LobbyCode::normalize("K3M7", out, sizeof(out)));
}
