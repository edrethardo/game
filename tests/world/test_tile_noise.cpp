// test_tile_noise.cpp — unit tests for world/tile_noise.h, the tiny deterministic value-noise
// used to bake per-tile shade variance and scatter decoration props into the level mesh.
// The key guarantees the level mesher relies on: value() stays in [0,1] (so shade tints never
// blow out or go negative), it's fully deterministic in (x,z,seed) (so host + clients bake
// identical floors — no visual desync), and the seed actually changes the field.
#include "doctest/doctest.h"
#include "world/tile_noise.h"

TEST_CASE("TileNoise hash2 stays in [0,1) and is deterministic") {
    for (s32 x = -20; x <= 20; ++x) {
        for (s32 z = -20; z <= 20; ++z) {
            f32 h = TileNoise::hash2(x, z, 1234u);
            REQUIRE(h >= 0.0f);
            REQUIRE(h <  1.0f);
            // Same inputs must give the same output (called twice).
            REQUIRE(TileNoise::hash2(x, z, 1234u) == doctest::Approx(h));
        }
    }
}

TEST_CASE("TileNoise smoothstep pins its endpoints and stays bounded") {
    REQUIRE(TileNoise::smooth(0.0f) == doctest::Approx(0.0f));
    REQUIRE(TileNoise::smooth(1.0f) == doctest::Approx(1.0f));
    REQUIRE(TileNoise::smooth(0.5f) == doctest::Approx(0.5f));
    for (s32 i = 0; i <= 10; ++i) {
        f32 v = TileNoise::smooth(static_cast<f32>(i) / 10.0f);
        REQUIRE(v >= 0.0f);
        REQUIRE(v <= 1.0f);
    }
}

TEST_CASE("TileNoise value stays in [0,1] across a sweep") {
    // Sample at the fractional frequency the level mesher uses (world coord * 0.35).
    for (s32 ix = 0; ix < 200; ++ix) {
        for (s32 iz = 0; iz < 200; ++iz) {
            f32 x = static_cast<f32>(ix) * 0.35f;
            f32 z = static_cast<f32>(iz) * 0.35f;
            f32 v = TileNoise::value(x, z, 42u);
            REQUIRE(v >= 0.0f);
            REQUIRE(v <= 1.0f);
        }
    }
}

TEST_CASE("TileNoise value is deterministic and seed-sensitive") {
    // Determinism: identical (x,z,seed) → identical result.
    REQUIRE(TileNoise::value(3.7f, 8.1f, 7u) == doctest::Approx(TileNoise::value(3.7f, 8.1f, 7u)));

    // Seed sensitivity: two different seeds must diverge somewhere in the field (otherwise the
    // per-floor seed fold in buildAll would leave every floor looking identical).
    bool differs = false;
    for (s32 i = 0; i < 64 && !differs; ++i) {
        f32 x = static_cast<f32>(i) * 0.35f;
        if (TileNoise::value(x, x, 1u) != doctest::Approx(TileNoise::value(x, x, 2u)))
            differs = true;
    }
    REQUIRE(differs);
}

TEST_CASE("TileNoise value is smooth — neighbours don't jump the full range") {
    // Adjacent samples (a fraction of a cell apart) should be close: the mesher depends on this
    // to get soft shade blobs rather than salt-and-pepper flicker between tiles.
    f32 prev = TileNoise::value(0.0f, 5.0f, 99u);
    for (s32 i = 1; i < 100; ++i) {
        f32 x = static_cast<f32>(i) * 0.05f;   // small step within a noise cell
        f32 cur = TileNoise::value(x, 5.0f, 99u);
        REQUIRE(std::fabs(cur - prev) < 0.5f); // never a hard discontinuity
        prev = cur;
    }
}
