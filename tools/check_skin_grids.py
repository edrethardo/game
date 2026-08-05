#!/usr/bin/env python3
"""check_skin_grids.py — every voxel mesh's skin must be sized to the mesh's REAL extents.

WHY THIS EXISTS. `add_voxel_model` derives UVs from the `filled` voxel set itself:

    u = (gx - min_gx + 0.5) / grid_w        grid_w = max_gx - min_gx + 1

so the texture is mapped over the voxels that were ACTUALLY emitted, not over the nominal grid the
generator was written against. A skin authored at a convenient 7x16 for a mesh that really occupies
4x6 is stretched across the model, and every band the artist placed — an eye row, a belt, a warning
line — lands somewhere else entirely. Nothing warns: the model renders, the colours are right, they
are simply in the wrong places, and it is only obvious once you know to look.

Eleven of forty-seven pairs were wrong when this check was first written, including a quadruped rat
whose 4x6 mesh wore a 7x16 skin (a 2.7x stretch on both axes) and a bat wing whose gradient ran the
wrong way. Run it after touching any generator:

    python3 tools/check_skin_grids.py

Exits non-zero on any mismatch, so build_assets.py can gate on it.

NOTE ON THE SPY: the probe below MUST accept `uv_overrides` under that exact keyword name. Several
generators pass it by keyword, and a probe with a different parameter name raises TypeError for
precisely those meshes — which, wrapped in a try/except, silently drops them from the audit. That
happened while writing this: a "0 mismatched" all-clear that had quietly checked only 33 of 47.
Nothing here swallows exceptions, for that reason.
"""

import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import gen_mesh   # noqa: E402
import gen_skin   # noqa: E402


def mesh_extents(name):
    """(w, h) of the voxels a generator actually emits, or None if it is not a voxel model."""
    captured = {}
    original = gen_mesh.add_voxel_model

    def spy(mb, filled, voxel_size, offset=(0, 0, 0), uv_overrides=None):
        if filled:
            captured["x"] = (min(p[0] for p in filled), max(p[0] for p in filled))
            captured["y"] = (min(p[1] for p in filled), max(p[1] for p in filled))
        return original(mb, filled, voxel_size, offset, uv_overrides)

    gen_mesh.add_voxel_model = spy
    try:
        gen_mesh.MESH_TYPES[name]["func"]()
    finally:
        gen_mesh.add_voxel_model = original

    if "x" not in captured:
        return None
    return (captured["x"][1] - captured["x"][0] + 1,
            captured["y"][1] - captured["y"][0] + 1)


def shipped_png_dims(png_name):
    """(w, h) of the PNG the ENGINE actually loads, or None if it is not on disk."""
    path = os.path.join(ROOT, "assets", "textures", png_name)
    if not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        header = f.read(24)
    if len(header) < 24:
        return None
    return struct.unpack(">II", header[16:24])


def check_shipped_matches_generator():
    """The GENERATOR being right is not enough — the engine loads the committed PNG.

    Most skins are not in build_assets.py's list at all, so their PNGs are never regenerated and the
    generator is effectively dead code: edit it and the game does not change. That is how `butcher`
    (the floor-5 boss) and `hellhound` came to ship textures that disagreed with their own
    generators, `hellhound`'s being 15x7 against a 6x10 mesh — near enough transposed.

    Divergence is reported as a WARNING rather than a failure: a stale PNG is not automatically
    wrong, and mass-regenerating three dozen enemy skins unreviewed is its own risk. But it must not
    be silent.
    """
    stale = []
    for name, (png, fn) in sorted(gen_skin.SKIN_TYPES.items()):
        dims = shipped_png_dims(png)
        if dims is None:
            continue
        gw, gh, _ = fn()
        if (gw, gh) != dims:
            stale.append((name, (gw, gh), dims))

    if stale:
        print(f"\n  WARNING: {len(stale)} shipped PNG(s) disagree with their generator —")
        print( "           the engine loads the PNG, so editing the generator changes nothing:")
        for name, gen, png in stale:
            print(f"             {name:24s} generator {gen[0]}x{gen[1]:<3d} shipped {png[0]}x{png[1]}")
        print( "           Add them to build_assets.py's `skins` list to bring the two back in step.")
    return len(stale)


def main():
    checked, bad = 0, []
    for name in sorted(gen_skin.SKIN_TYPES):
        if name not in gen_mesh.MESH_TYPES:
            continue                      # skin shared by / renamed for another mesh
        extents = mesh_extents(name)
        if extents is None:
            continue                      # not built from voxels; UVs come from elsewhere
        sw, sh, _ = gen_skin.SKIN_TYPES[name][1]()
        checked += 1
        if extents != (sw, sh):
            bad.append((name, extents, (sw, sh)))

    if bad:
        print(f"=== {len(bad)} of {checked} skins do not match their mesh ===\n")
        print(f"  {'name':24s} {'mesh':>9s} {'skin':>9s}")
        for name, (mw, mh), (sw, sh) in bad:
            print(f"  {name:24s} {mw:4d}x{mh:<4d} {sw:4d}x{sh:<4d}")
        print("\nThe skin is stretched over the model — every band lands in the wrong place.")
        print("Set w/h in the skin function to the MESH column, and re-map the feature rows:")
        print("  px = gx - min_gx,  py = gy - min_gy")
        return 1

    print(f"  OK: all {checked} voxel meshes wear a skin sized to their real extents")
    check_shipped_matches_generator()   # advisory; see the docstring for why it does not fail
    return 0


if __name__ == "__main__":
    sys.exit(main())
