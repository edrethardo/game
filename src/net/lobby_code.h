#pragma once
// lobby_code.h — short shareable Steam lobby codes ("K3M7").
//
// FOUR glyphs, so it can be read out over voice chat without anyone losing their place.
//
// That length is the design constraint, and it decides the whole mechanism: 4 base-32 symbols is
// 20 bits, which obviously CANNOT encode a 64-bit Steam lobby id. So the code is NOT derived from
// the lobby — it is a RANDOM code the host generates and publishes as lobby metadata ("code"), and
// a joiner finds the lobby by asking Steam for lobbies whose "code" field matches (a lobby-list
// string filter). The code is a lookup key, not an encoding.
//
// Consequence worth knowing: a lobby must be SEARCHABLE for a code lookup to find it, so "private"
// here means unlisted-by-our-browser (the lobby publishes private=1 and the browser filters it out),
// not Steam-invisible. Someone talking to the raw Steam API could still enumerate lobbies. For a
// co-op PvE game that is a non-issue; it would not be acceptable for anything competitive.
//
// Collision odds: 32^4 = 1,048,576 codes. With the handful of lobbies live at any one time the
// chance of a clash is negligible, and a clash merely means a joiner lands in the wrong game rather
// than anything corrupt. Not worth a coordination protocol.
//
// Typing is FORGIVING because people retype these from Discord: separators and whitespace are
// ignored, case is ignored, and the ambiguous glyphs are folded (I/L -> 1, O -> 0). The alphabet
// omits I, L, O and U outright, so a generated code never contains them in the first place — the
// folding only rescues a human who typed what they *saw*.
//
// Pure math, no Steamworks: builds and is unit-tested (tests/net/test_lobby_code.cpp) even in the
// Steam-free (itch.io) configuration.

#include "core/types.h"

namespace LobbyCode {

static constexpr u32 CODE_LEN = 4;                // glyphs
static constexpr u32 BUF_SIZE = CODE_LEN + 1;     // + NUL

// Crockford base32 — omits I, L, O, U (ambiguous glyphs / accidental profanity).
static constexpr const char* ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// Generate a code from 20 bits of caller-supplied entropy (e.g. std::rand()). Writes "" if the
// buffer is too small, so the caller always has a printable string.
inline void generate(u32 entropy, char* out, u32 cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (cap < BUF_SIZE) return;
    for (u32 i = 0; i < CODE_LEN; i++) {
        out[i] = ALPHABET[(entropy >> (5u * i)) & 0x1Fu];
    }
    out[CODE_LEN] = '\0';
}

// Value of one code symbol, or -1 if it isn't one. Case-insensitive; folds the omitted glyphs.
inline int charValue(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c == 'I' || c == 'L') c = '1';   // the alphabet has no I/L — a human read a 1 as one of them
    if (c == 'O')             c = '0';   // ...and no O — they read a 0
    for (u32 i = 0; i < 32; i++) {
        if (ALPHABET[i] == c) return static_cast<int>(i);
    }
    return -1;
}

// Fold a hand-typed code into the canonical UPPERCASE form that the host published, which is what we
// hand to Steam as the lobby-list filter value — so it must match byte-for-byte. Ignores '-', ' ',
// '_' and tabs. Returns false (leaving `out` empty) unless the input is exactly CODE_LEN valid
// symbols; a rejected code must never turn into a lobby query that quietly matches the wrong game.
inline bool normalize(const char* in, char* out, u32 cap) {
    if (!out || cap < BUF_SIZE) return false;
    out[0] = '\0';
    if (!in) return false;

    char tmp[BUF_SIZE];
    u32 n = 0;
    for (const char* p = in; *p; ++p) {
        const char c = *p;
        if (c == '-' || c == ' ' || c == '_' || c == '\t') continue;  // separators people add back in
        const int d = charValue(c);
        if (d < 0) return false;            // not a code symbol at all
        if (n >= CODE_LEN) return false;    // too long
        tmp[n++] = ALPHABET[d];             // canonical glyph (folds case + I/L/O)
    }
    if (n != CODE_LEN) return false;        // too short (or empty)

    for (u32 i = 0; i < CODE_LEN; i++) out[i] = tmp[i];
    out[CODE_LEN] = '\0';
    return true;
}

}  // namespace LobbyCode
