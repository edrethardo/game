#!/usr/bin/env python3
"""Generate low-poly 3D meshes as OBJ files for the game engine.

Usage examples:
    python3 tools/gen_mesh.py --type humanoid --height 1.8 --out assets/meshes/skeleton.obj
    python3 tools/gen_mesh.py --type spider --radius 0.6 --out assets/meshes/spider.obj
    python3 tools/gen_mesh.py --type bat --wingspan 1.0 --out assets/meshes/bat.obj
    python3 tools/gen_mesh.py --type pillar --radius 0.3 --height 3.0 --sides 8 --out assets/meshes/pillar.obj
    python3 tools/gen_mesh.py --type chest --width 0.6 --out assets/meshes/chest.obj
    python3 tools/gen_mesh.py --list-types
"""

import argparse
import math
import os
import sys

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _cross(a, b):
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def _normalize(v):
    length = math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2)
    if length < 1e-12:
        return (0.0, 1.0, 0.0)
    return (v[0] / length, v[1] / length, v[2] / length)


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


# ---------------------------------------------------------------------------
# Mesh accumulator
# ---------------------------------------------------------------------------

class MeshBuilder:
    """Collects verts, normals, uvs, and triangle faces."""

    def __init__(self):
        self.verts = []   # list of (x, y, z)
        self.normals = []  # list of (nx, ny, nz)
        self.uvs = []      # list of (u, v)
        self.faces = []    # list of ((vi, ti, ni), (vi, ti, ni), (vi, ti, ni))

    # indices are 0-based internally; converted to 1-based on write.

    def add_vert(self, x, y, z):
        self.verts.append((x, y, z))
        return len(self.verts) - 1

    def add_normal(self, nx, ny, nz):
        self.normals.append((nx, ny, nz))
        return len(self.normals) - 1

    def add_uv(self, u, v):
        self.uvs.append((u, v))
        return len(self.uvs) - 1

    def add_tri(self, v0, v1, v2, t0, t1, t2, n0, n1, n2):
        self.faces.append(((v0, t0, n0), (v1, t1, n1), (v2, t2, n2)))


# ---------------------------------------------------------------------------
# add_box — axis-aligned box with per-face normals (12 triangles)
# ---------------------------------------------------------------------------

def add_box(mb, center, half_extents):
    """Add an axis-aligned box to the MeshBuilder.

    center: (cx, cy, cz)
    half_extents: (hx, hy, hz)
    Generates 8 corners, 6 face normals, simple 0-1 UVs per face, 12 tris.
    """
    cx, cy, cz = center
    hx, hy, hz = half_extents

    # 8 corners
    corners = [
        (cx - hx, cy - hy, cz - hz),  # 0: left  bottom back
        (cx + hx, cy - hy, cz - hz),  # 1: right bottom back
        (cx + hx, cy + hy, cz - hz),  # 2: right top    back
        (cx - hx, cy + hy, cz - hz),  # 3: left  top    back
        (cx - hx, cy - hy, cz + hz),  # 4: left  bottom front
        (cx + hx, cy - hy, cz + hz),  # 5: right bottom front
        (cx + hx, cy + hy, cz + hz),  # 6: right top    front
        (cx - hx, cy + hy, cz + hz),  # 7: left  top    front
    ]

    vi = [mb.add_vert(*c) for c in corners]

    # 6 face normals
    face_defs = [
        # (normal, quad corners in CCW order when viewed from outside)
        ((0, 0, -1), (1, 0, 3, 2)),   # back  (-Z)
        ((0, 0,  1), (4, 5, 6, 7)),   # front (+Z)
        ((-1, 0, 0), (0, 4, 7, 3)),   # left  (-X)
        (( 1, 0, 0), (5, 1, 2, 6)),   # right (+X)
        ((0, -1, 0), (0, 1, 5, 4)),   # bottom(-Y)
        ((0,  1, 0), (3, 7, 6, 2)),   # top   (+Y)
    ]

    # Shared UVs for each face quad: 4 corners
    uv0 = mb.add_uv(0.0, 0.0)
    uv1 = mb.add_uv(1.0, 0.0)
    uv2 = mb.add_uv(1.0, 1.0)
    uv3 = mb.add_uv(0.0, 1.0)

    for normal, (a, b, c, d) in face_defs:
        ni = mb.add_normal(*normal)
        # two triangles per quad
        mb.add_tri(vi[a], vi[b], vi[c], uv0, uv1, uv2, ni, ni, ni)
        mb.add_tri(vi[a], vi[c], vi[d], uv0, uv2, uv3, ni, ni, ni)


# ---------------------------------------------------------------------------
# add_cylinder — N-sided cylinder with caps
# ---------------------------------------------------------------------------

def add_cylinder(mb, base_center, radius, height, sides):
    """N-sided cylinder with top and bottom caps, cylindrical UVs on walls."""
    cx, cy, cz = base_center

    bottom_vis = []
    top_vis = []
    for i in range(sides):
        angle = 2.0 * math.pi * i / sides
        x = cx + radius * math.cos(angle)
        z = cz + radius * math.sin(angle)
        bottom_vis.append(mb.add_vert(x, cy, z))
        top_vis.append(mb.add_vert(x, cy + height, z))

    bot_center = mb.add_vert(cx, cy, cz)
    top_center = mb.add_vert(cx, cy + height, cz)

    bottom_n = mb.add_normal(0, -1, 0)
    top_n = mb.add_normal(0, 1, 0)

    side_normals = []
    for i in range(sides):
        angle = 2.0 * math.pi * (i + 0.5) / sides
        side_normals.append(mb.add_normal(math.cos(angle), 0, math.sin(angle)))

    uv00 = mb.add_uv(0.0, 0.0)
    uv10 = mb.add_uv(1.0, 0.0)
    uv11 = mb.add_uv(1.0, 1.0)
    uv01 = mb.add_uv(0.0, 1.0)
    uv_center = mb.add_uv(0.5, 0.5)

    for i in range(sides):
        j = (i + 1) % sides
        sn = side_normals[i]
        # CCW winding when viewed from outside
        mb.add_tri(bottom_vis[i], top_vis[j], bottom_vis[j],
                   uv00, uv11, uv10, sn, sn, sn)
        mb.add_tri(bottom_vis[i], top_vis[i], top_vis[j],
                   uv00, uv01, uv11, sn, sn, sn)
        mb.add_tri(bot_center, bottom_vis[i], bottom_vis[j],
                   uv_center, uv00, uv10, bottom_n, bottom_n, bottom_n)
        mb.add_tri(top_center, top_vis[j], top_vis[i],
                   uv_center, uv10, uv00, top_n, top_n, top_n)


# ---------------------------------------------------------------------------
# Voxel model builder — Barony-style chunky cube models
# ---------------------------------------------------------------------------

def add_voxel_model(mb, filled, voxel_size, offset=(0, 0, 0),
                    uv_overrides=None):
    """Build optimized mesh from a set of filled voxel positions.

    filled: set of (gx, gy, gz) integer grid positions that are filled
    voxel_size: world-space size of each cube
    offset: world-space offset applied to all voxels
    uv_overrides: optional dict {(gx,gy,gz): (alt_gx, alt_gy)} — remap
        specific voxels to a different skin pixel, e.g. to prevent eye
        color bleeding to back-of-head voxels in the same (gx,gy) column.

    Only emits faces at filled/empty boundaries (internal faces culled).

    UVs map each voxel to a pixel based on (gx, gy). All faces of the same
    voxel share the same color. Simple, stable, agent-friendly.
    """
    ox, oy, oz = offset
    hs = voxel_size * 0.5

    if not filled:
        return
    min_gx = min(p[0] for p in filled)
    max_gx = max(p[0] for p in filled)
    min_gy = min(p[1] for p in filled)
    max_gy = max(p[1] for p in filled)
    grid_w = max_gx - min_gx + 1
    grid_h = max_gy - min_gy + 1
    tex_w = max(grid_w, 1)
    tex_h = max(grid_h, 1)

    # Each entry: (neighbour offset, face normal, quad corners in CCW order from outside)
    face_defs = [
        ((0, 1, 0),  (0, 1, 0),  [(-1,1,-1), (-1,1,1), (1,1,1), (1,1,-1)]),
        ((0,-1, 0),  (0,-1, 0),  [(-1,-1,1), (-1,-1,-1), (1,-1,-1), (1,-1,1)]),
        ((1, 0, 0),  (1, 0, 0),  [(1,-1,-1), (1,1,-1), (1,1,1), (1,-1,1)]),
        ((-1,0, 0),  (-1,0, 0),  [(-1,-1,1), (-1,1,1), (-1,1,-1), (-1,-1,-1)]),
        ((0, 0, 1),  (0, 0, 1),  [(-1,-1,1), (1,-1,1), (1,1,1), (-1,1,1)]),
        ((0, 0,-1),  (0, 0,-1),  [(1,-1,-1), (-1,-1,-1), (-1,1,-1), (1,1,-1)]),
    ]

    normal_cache = {}
    for _, norm, _ in face_defs:
        if norm not in normal_cache:
            normal_cache[norm] = mb.add_normal(*norm)

    for (gx, gy, gz) in filled:
        cx = ox + gx * voxel_size + hs
        cy = oy + gy * voxel_size + hs
        cz = oz + gz * voxel_size + hs

        # UV defaults to (gx, gy) pixel; per-voxel override remaps to a
        # different pixel (e.g. skin color instead of eye color at back of head)
        ugx, ugy = gx, gy
        if uv_overrides and (gx, gy, gz) in uv_overrides:
            ugx, ugy = uv_overrides[(gx, gy, gz)]
        u_center = (ugx - min_gx + 0.5) / tex_w
        v_center = (ugy - min_gy + 0.5) / tex_h
        eps = 0.01 / tex_w
        uv0 = mb.add_uv(u_center - eps, v_center - eps)
        uv1 = mb.add_uv(u_center + eps, v_center - eps)
        uv2 = mb.add_uv(u_center + eps, v_center + eps)
        uv3 = mb.add_uv(u_center - eps, v_center + eps)

        for (dx, dy, dz), norm, corners in face_defs:
            if (gx + dx, gy + dy, gz + dz) in filled:
                continue

            ni = normal_cache[norm]
            vis = []
            for (sx, sy, sz) in corners:
                vis.append(mb.add_vert(cx + sx * hs, cy + sy * hs, cz + sz * hs))

            mb.add_tri(vis[0], vis[1], vis[2], uv0, uv1, uv2, ni, ni, ni)
            mb.add_tri(vis[0], vis[2], vis[3], uv0, uv2, uv3, ni, ni, ni)

    # Store grid info for skin texture generation
    mb.grid_min_x = min_gx
    mb.grid_min_y = min_gy
    mb.grid_w = grid_w
    mb.grid_h = grid_h
    mb.filled = filled


# ---------------------------------------------------------------------------
# write_obj
# ---------------------------------------------------------------------------

def write_obj(path, mb):
    """Write a MeshBuilder to an OBJ file."""
    dirpath = os.path.dirname(path)
    if dirpath:
        os.makedirs(dirpath, exist_ok=True)

    with open(path, "w") as f:
        f.write("# Generated by gen_mesh.py\n")
        f.write(f"# Vertices: {len(mb.verts)}  Normals: {len(mb.normals)}  "
                f"UVs: {len(mb.uvs)}  Faces: {len(mb.faces)}\n\n")

        for x, y, z in mb.verts:
            f.write(f"v {x:.6f} {y:.6f} {z:.6f}\n")
        f.write("\n")

        for nx, ny, nz in mb.normals:
            f.write(f"vn {nx:.6f} {ny:.6f} {nz:.6f}\n")
        f.write("\n")

        for u, v in mb.uvs:
            f.write(f"vt {u:.6f} {v:.6f}\n")
        f.write("\n")

        for tri in mb.faces:
            parts = []
            for vi, ti, ni in tri:
                # OBJ is 1-based
                parts.append(f"{vi+1}/{ti+1}/{ni+1}")
            f.write(f"f {' '.join(parts)}\n")


# ---------------------------------------------------------------------------
# Mesh generators
# ---------------------------------------------------------------------------

def gen_humanoid(height=1.8):
    """Barony-style chunky voxel skeleton. Origin at feet (Y=0).

    Big skull head with eye sockets and jaw, visible rib cage torso,
    bony arms and legs. Cartoony proportions — oversized head.
    """
    mb = MeshBuilder()
    vs = height / 16.0  # voxel size — 16 voxels tall
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Skull (big, cartoony) ---
    # Main skull dome — 5x4x4
    fill_box(-2, 12, -2, 5, 4, 4)
    # Jaw — 3x1x3, slightly narrower
    fill_box(-1, 11, -2, 3, 1, 3)
    # Eye sockets — remove only front layer so the back is colored by skin texture
    filled.discard((-1, 14, -2))  # left eye front
    filled.discard((1, 14, -2))   # right eye front
    # Keep (-1,14,-1) and (1,14,-1) — these are the visible eye voxels
    # Nose — remove only front
    filled.discard((0, 13, -2))
    # Mouth — remove front layer only, back voxels stay for teeth coloring
    filled.discard((0, 11, -2))   # center mouth gap

    # Neck — 1x1x1
    fill_box(0, 10, 0, 1, 1, 1)

    # --- Rib cage (open at front) ---
    # Spine column — 1 voxel wide, 5 tall
    fill_box(0, 5, 1, 1, 5, 1)
    # Rib pairs — 3 rows, leaving front open
    for ry in [6, 7, 9]:
        fill_box(-2, ry, 0, 2, 1, 2)  # left ribs
        fill_box(1, ry, 0, 2, 1, 2)   # right ribs

    # Shoulder bones
    fill_box(-3, 9, 0, 1, 1, 1)
    fill_box(3, 9, 0, 1, 1, 1)

    # --- Arms (bony, 1 voxel thick) ---
    # Upper arms
    fill_box(-3, 7, 0, 1, 2, 1)
    fill_box(3, 7, 0, 1, 2, 1)
    # Lower arms
    fill_box(-3, 4, 0, 1, 3, 1)
    fill_box(3, 4, 0, 1, 3, 1)
    # Hands — small blocks
    fill_box(-3, 3, -1, 1, 1, 2)
    fill_box(3, 3, -1, 1, 1, 2)

    # --- Pelvis ---
    fill_box(-1, 4, 0, 3, 1, 1)

    # --- Legs (bony) ---
    # Thigh bones
    fill_box(-2, 2, 0, 1, 2, 1)
    fill_box(1, 2, 0, 1, 2, 1)
    # Shin bones
    fill_box(-2, 0, 0, 1, 2, 1)
    fill_box(1, 0, 0, 1, 2, 1)
    # Feet — flat 2x1 blocks
    fill_box(-2, 0, -1, 1, 1, 2)
    fill_box(1, 0, -1, 1, 1, 2)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))

    return mb


def gen_spider(radius=0.6):
    """Barony-style chunky voxel spider. Fat body, fangs, jointed legs.
    Legs are part of the body mesh (static). Only mandibles are animated via limb system.
    Origin at center-bottom."""
    mb = MeshBuilder()
    vs = radius / 5.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Fat abdomen
    fill_box(-3, 1, 1, 6, 4, 6)
    for corner in [(-3,4,1),(-3,4,6),(2,4,1),(2,4,6),
                   (-3,1,1),(-3,1,6),(2,1,1),(2,1,6)]:
        filled.discard(corner)
    fill_box(-2, 5, 2, 4, 1, 4)
    # Thorax
    fill_box(-2, 1, -3, 4, 3, 4)
    # Head
    fill_box(-1, 1, -6, 3, 3, 3)
    filled.add((-1, 4, -5)); filled.add((1, 4, -5))
    filled.add((-1, 6, -5)); filled.add((1, 6, -5))
    # Fangs
    filled.add((-1, 0, -7)); filled.add((1, 0, -7))
    filled.add((-1, 0, -6)); filled.add((1, 0, -6))
    # Pedipalps
    filled.add((-1, 1, -7)); filled.add((1, 1, -7))
    # 8 legs with knee joints (static, part of body mesh)
    for sz in [-2, -1, 0, 1]:
        filled.add((-3, 2, sz)); filled.add((-4, 3, sz)); filled.add((-5, 4, sz))
        filled.add((-6, 3, sz)); filled.add((-6, 2, sz)); filled.add((-6, 1, sz))
        filled.add((-7, 0, sz))
        filled.add((3, 2, sz)); filled.add((4, 3, sz)); filled.add((5, 4, sz))
        filled.add((6, 3, sz)); filled.add((6, 2, sz)); filled.add((6, 1, sz))
        filled.add((7, 0, sz))
    ox = -0.5 * vs; oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_spider_leg_pair(radius=0.6):
    """One pair of spider legs matching the original spider style exactly.
    Left side only — mirrored at render time. 2 legs at different Z.
    Shape: short outward climb to knee, then sharp vertical drop to foot.
    Origin at attachment point (gx=0 = body edge)."""
    mb = MeshBuilder()
    vs = radius / 5.0
    filled = set()
    leg_voxels = []

    # Original leg pattern (shifted so attachment is at gx=0):
    #   (0,2) attachment → (-1,3) out+up → (-2,4) KNEE → (-3,3) drop
    #   → (-3,2) → (-3,1) → (-4,0) FOOT
    def add_leg(sz):
        for v in [(0,2,sz), (-1,3,sz), (-2,4,sz),
                  (-3,3,sz), (-3,2,sz), (-3,1,sz), (-4,0,sz)]:
            filled.add(v); leg_voxels.append(v)

    add_leg(-1)  # front leg of pair
    add_leg(1)   # rear leg of pair

    # Remap all voxels to safe dark leg color (avoid eye pixel sampling)
    uv_overrides = {v: (0, 2) for v in leg_voxels}

    ox = -0.5 * vs; oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz), uv_overrides=uv_overrides)
    return mb


def gen_bat(wingspan=1.0):
    """Barony-style chunky voxel bat. Body + head + ears only.

    Wings and claws are handled by the LimbSystem at runtime, so they
    are NOT included in this body mesh to avoid doubling up.
    Origin at center-bottom.
    """
    mb = MeshBuilder()
    vs = wingspan / 16.0  # voxel size — 16 voxels wide
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Body — 3x4x3, chunky fur ball ---
    fill_box(-1, 2, -1, 3, 4, 3)

    # --- Head — 3x3x3, big cartoony head ---
    fill_box(-1, 6, -1, 3, 3, 3)
    # Snout — 1 voxel protruding from front
    filled.add((0, 6, -2))
    filled.add((0, 7, -2))
    # Eyes stay filled — colored bright by the skin texture instead of hollow
    # (hollow sockets are invisible in dark dungeons)
    # Mouth stays filled too — skin texture handles the color

    # --- Big pointy ears ---
    filled.add((-1, 9, 0))
    filled.add((-1, 10, 0))
    filled.add((-1, 11, 0))  # tall ear
    filled.add((1, 9, 0))
    filled.add((1, 10, 0))
    filled.add((1, 11, 0))

    # --- Small shoulder stubs where wings attach (visual anchor) ---
    filled.add((-2, 4, 0))
    filled.add((2, 4, 0))

    # NOTE: Wings and claws are NOT generated here — they are rendered
    # as separate animated limbs by LimbSystem at runtime.

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))

    return mb


def gen_pillar(radius=0.3, height=3.0, sides=8):
    """N-sided cylinder pillar. Origin at base center."""
    mb = MeshBuilder()
    add_cylinder(mb, base_center=(0, 0, 0), radius=radius, height=height, sides=sides)
    return mb


def gen_chest(width=0.6):
    """Simple chest: bottom box + slightly raised lid box."""
    mb = MeshBuilder()

    depth = width * 0.67
    total_h = width * 0.67

    bottom_h = total_h * 0.6
    lid_h = total_h * 0.35
    gap = total_h * 0.02  # small gap between body and lid

    # Bottom box
    add_box(mb,
            center=(0, bottom_h * 0.5, 0),
            half_extents=(width * 0.5, bottom_h * 0.5, depth * 0.5))

    # Lid — slightly wider at the top, offset up with a small gap
    lid_y = bottom_h + gap + lid_h * 0.5
    add_box(mb,
            center=(0, lid_y, 0),
            half_extents=(width * 0.52, lid_h * 0.5, depth * 0.52))

    return mb


def gen_human(height=1.8):
    """Barony-style chunky human NPC. Origin at feet (Y=0).

    Broader build than skeleton, solid face with eyes/mouth indentations,
    thicker limbs. Meant for friendly NPC allies wearing armor.
    """
    mb = MeshBuilder()
    vs = height / 16.0  # voxel size — 16 voxels tall
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Head (round, solid) ---
    fill_box(-2, 13, -2, 5, 3, 4)  # main head block
    fill_box(-1, 12, -1, 3, 1, 3)  # chin
    # Hollow out eyes (front face only)
    filled.discard((-1, 14, -2))
    filled.discard((1, 14, -2))
    # Mouth indent
    filled.discard((0, 12, -2))

    # --- Neck ---
    fill_box(0, 11, 0, 1, 1, 1)

    # --- Torso (broad, solid — like a breastplate) ---
    fill_box(-2, 6, -1, 5, 5, 3)  # main chest
    fill_box(-3, 9, -1, 1, 2, 3)  # left shoulder pad
    fill_box(3, 9, -1, 1, 2, 3)   # right shoulder pad

    # Belt
    fill_box(-2, 5, -1, 5, 1, 3)

    # --- Arms (thick, 2 voxels) ---
    fill_box(-4, 7, 0, 1, 3, 1)   # left upper arm
    fill_box(4, 7, 0, 1, 3, 1)    # right upper arm
    fill_box(-4, 4, 0, 1, 3, 1)   # left lower arm
    fill_box(4, 4, 0, 1, 3, 1)    # right lower arm
    # Hands
    fill_box(-4, 3, -1, 1, 1, 2)
    fill_box(4, 3, -1, 1, 1, 2)

    # --- Legs (thick) ---
    fill_box(-2, 2, -1, 2, 3, 2)  # left thigh
    fill_box(1, 2, -1, 2, 3, 2)   # right thigh
    fill_box(-2, 0, -1, 2, 2, 2)  # left shin
    fill_box(1, 0, -1, 2, 2, 2)   # right shin
    # Boots
    fill_box(-2, 0, -2, 2, 1, 3)
    fill_box(1, 0, -2, 2, 1, 3)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))

    return mb


def gen_cleric(height=1.8):
    """Male cleric NPC. Broad build, hooded head, robes, short blonde hair.

    Origin at feet (Y=0). Heavier torso for priestly armor look.
    """
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # Head (round)
    fill_box(-2, 13, -2, 5, 3, 4)
    fill_box(-1, 12, -1, 3, 1, 3)  # chin
    # Eyes
    filled.discard((-1, 14, -2))
    filled.discard((1, 14, -2))
    # Mouth
    filled.discard((0, 12, -2))
    # Short blonde hair (top of head, slightly wider)
    fill_box(-2, 16, -2, 5, 1, 4)
    fill_box(-2, 15, -2, 5, 1, 1)  # front fringe

    # Hood/collar
    fill_box(-2, 11, -1, 5, 2, 3)

    # Torso (priestly robes — wide, flowing)
    fill_box(-3, 5, -1, 7, 6, 3)
    # Shoulder guards
    fill_box(-4, 9, -1, 1, 2, 3)
    fill_box(4, 9, -1, 1, 2, 3)
    # Holy symbol on chest (small bump)
    filled.add((0, 8, -2))

    # Belt/sash
    fill_box(-3, 4, -1, 7, 1, 3)

    # Arms
    fill_box(-4, 7, 0, 1, 3, 1)
    fill_box(4, 7, 0, 1, 3, 1)
    fill_box(-4, 4, 0, 1, 3, 1)
    fill_box(4, 4, 0, 1, 3, 1)
    # Hands
    fill_box(-4, 3, -1, 1, 1, 2)
    fill_box(4, 3, -1, 1, 1, 2)

    # Legs (hidden under robes — thicker)
    fill_box(-2, 2, -1, 2, 2, 2)
    fill_box(1, 2, -1, 2, 2, 2)
    fill_box(-2, 0, -1, 2, 2, 2)
    fill_box(1, 0, -1, 2, 2, 2)
    # Boots
    fill_box(-2, 0, -2, 2, 1, 3)
    fill_box(1, 0, -2, 2, 1, 3)

    # Remap back-of-head voxels in eye columns to skin color so the eye
    # pixel doesn't bleed through to the back of the head.  gz=-1 is the
    # visible eyeball behind the socket — keep that as eye color.
    uv_fix = {}
    for gz in range(0, 2):      # gz=0 and gz=1
        uv_fix[(-1, 14, gz)] = (0, 14)   # center column = skin pixel
        uv_fix[(1, 14, gz)]  = (0, 14)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz), uv_overrides=uv_fix)
    return mb


def gen_archer(height=1.7):
    """Female archer NPC. Leaner build, ponytail, lighter armor.

    Origin at feet (Y=0). Slimmer than cleric, with a ponytail.
    """
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # Head (slightly smaller, 5 wide to center eye sockets properly)
    fill_box(-2, 13, -2, 5, 3, 4)
    fill_box(-1, 12, -1, 3, 1, 3)  # chin
    # Eyes (green — just the sockets, tint does the color)
    filled.discard((-1, 14, -2))
    filled.discard((1, 14, -2))
    # Mouth
    filled.discard((0, 12, -2))

    # Fox-red/brown ponytail (extends behind head and down)
    fill_box(-2, 15, -2, 5, 2, 4)   # top hair (matches head width)
    fill_box(0, 14, 2, 1, 1, 1)     # ponytail start
    fill_box(0, 13, 2, 1, 1, 1)     # ponytail mid
    fill_box(0, 12, 2, 1, 1, 1)     # ponytail mid
    fill_box(0, 11, 2, 1, 1, 1)     # ponytail end
    fill_box(0, 10, 2, 1, 1, 1)     # ponytail tip

    # Neck
    fill_box(0, 11, 0, 1, 1, 1)

    # Upper torso — narrower waist, wider chest
    fill_box(-2, 8, -1, 4, 3, 3)   # upper chest
    fill_box(-2, 8, -2, 4, 2, 1)   # bust (front protrusion)
    # Light shoulder pads
    fill_box(-3, 9, 0, 1, 1, 1)
    fill_box(2, 9, 0, 1, 1, 1)

    # Narrow waist
    fill_box(-1, 6, -1, 3, 2, 3)

    # Wider hips
    fill_box(-2, 4, -1, 5, 2, 3)
    # Belt with quiver strap
    fill_box(-2, 5, -1, 5, 1, 3)
    # Quiver on back (tall thin box)
    fill_box(1, 6, 2, 1, 5, 1)

    # Arms (slimmer)
    fill_box(-3, 7, 0, 1, 3, 1)
    fill_box(2, 7, 0, 1, 3, 1)
    fill_box(-3, 4, 0, 1, 3, 1)
    fill_box(2, 4, 0, 1, 3, 1)
    # Hands
    fill_box(-3, 3, -1, 1, 1, 2)
    fill_box(2, 3, -1, 1, 1, 2)

    # Legs (shapely — wider thigh, narrower shin)
    fill_box(-2, 2, -1, 2, 2, 2)
    fill_box(1, 2, -1, 2, 2, 2)
    fill_box(-2, 0, 0, 1, 2, 1)
    fill_box(1, 0, 0, 1, 2, 1)
    # Light boots
    fill_box(-2, 0, -1, 1, 1, 2)
    fill_box(1, 0, -1, 1, 1, 2)

    # Remap back-of-head voxels in eye columns to skin so eye color
    # doesn't bleed to the back.  Also remap ponytail voxels at face gy
    # levels to hair color so they don't pick up skin/face texture.
    uv_fix = {}
    for gz in range(0, 2):
        uv_fix[(-1, 14, gz)] = (0, 14)   # skin pixel
        uv_fix[(1, 14, gz)]  = (0, 14)
    for gy in range(10, 15):              # ponytail spans gy 10-14
        uv_fix[(0, gy, 2)] = (0, 16)     # hair pixel (top of head)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz), uv_overrides=uv_fix)
    return mb


def gen_mage(height=1.8):
    """Male mage NPC. Robed spellcaster with a pointed wizard hat.

    Origin at feet (Y=0). Slightly narrower build than cleric, tall hat adds
    height above gy=15. Long robes hide the legs.
    """
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # Head
    fill_box(-2, 13, -2, 5, 3, 4)
    fill_box(-1, 12, -1, 3, 1, 3)  # chin
    # Eye sockets (front face only)
    filled.discard((-1, 14, -2))
    filled.discard((1, 14, -2))
    # Mouth
    filled.discard((0, 12, -2))

    # Pointed wizard hat — wide brim narrows to a tip
    fill_box(-2, 16, -2, 5, 1, 4)  # brim, gy=16, 5 wide
    fill_box(-1, 17, -2, 3, 1, 4)  # middle, gy=17, 3 wide
    fill_box(0, 18, -1, 1, 1, 2)   # pointed tip, gy=18, 1 wide

    # Hood/collar
    fill_box(-2, 11, -1, 5, 2, 3)

    # Torso — narrower robe than cleric (gx -3 to 3, 7 wide)
    fill_box(-3, 5, -1, 7, 6, 3)
    # Slight shoulder guards
    fill_box(-4, 9, 0, 1, 2, 1)
    fill_box(4, 9, 0, 1, 2, 1)

    # Belt/sash
    fill_box(-3, 4, -1, 7, 1, 3)

    # Arms (sleeves, same width as cleric)
    fill_box(-4, 7, 0, 1, 3, 1)
    fill_box(4, 7, 0, 1, 3, 1)
    fill_box(-4, 4, 0, 1, 3, 1)
    fill_box(4, 4, 0, 1, 3, 1)
    # Hands
    fill_box(-4, 3, -1, 1, 1, 2)
    fill_box(4, 3, -1, 1, 1, 2)

    # Legs hidden under robes
    fill_box(-2, 2, -1, 2, 2, 2)
    fill_box(1, 2, -1, 2, 2, 2)
    fill_box(-2, 0, -1, 2, 2, 2)
    fill_box(1, 0, -1, 2, 2, 2)
    # Boots
    fill_box(-2, 0, -2, 2, 1, 3)
    fill_box(1, 0, -2, 2, 1, 3)

    # Remap back-of-head voxels in eye columns so eye pixel doesn't bleed
    # through to the rear faces.
    uv_fix = {}
    for gz in range(0, 2):
        uv_fix[(-1, 14, gz)] = (0, 14)
        uv_fix[(1, 14, gz)]  = (0, 14)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz), uv_overrides=uv_fix)
    return mb


def gen_rogue(height=1.7):
    """Male rogue NPC. Lean hooded figure with cape, lean armor.

    Origin at feet (Y=0). Similar proportions to archer but darker and
    with a hood and a thin back cape.
    """
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # Head
    fill_box(-2, 13, -2, 5, 3, 4)
    fill_box(-1, 12, -1, 3, 1, 3)  # chin
    # Eye sockets (front face only)
    filled.discard((-1, 14, -2))
    filled.discard((1, 14, -2))
    # Mouth
    filled.discard((0, 12, -2))

    # Hood — covers top and wraps down sides of head
    fill_box(-2, 15, -2, 5, 2, 4)  # top of hood
    fill_box(-2, 13, 1, 1, 3, 1)   # left side drape
    fill_box(2, 13, 1, 1, 3, 1)    # right side drape

    # Neck
    fill_box(0, 11, 0, 1, 1, 1)

    # Slim torso
    fill_box(-2, 8, -1, 4, 3, 3)   # upper chest (lean)

    # Narrow waist/belt
    fill_box(-1, 6, -1, 3, 2, 3)
    fill_box(-2, 5, -1, 5, 1, 3)   # belt row

    # Hips
    fill_box(-2, 4, -1, 5, 1, 3)

    # Cape at back — thin voxels along gz=2 from gy=4 to gy=10
    for gy in range(4, 11):
        filled.add((-1, gy, 2))
        filled.add((0, gy, 2))
        filled.add((1, gy, 2))

    # Arms (lean, gx=-3 and gx=2 like archer)
    fill_box(-3, 7, 0, 1, 3, 1)
    fill_box(2, 7, 0, 1, 3, 1)
    fill_box(-3, 4, 0, 1, 3, 1)
    fill_box(2, 4, 0, 1, 3, 1)
    # Hands
    fill_box(-3, 3, -1, 1, 1, 2)
    fill_box(2, 3, -1, 1, 1, 2)

    # Legs
    fill_box(-2, 2, -1, 2, 2, 2)
    fill_box(1, 2, -1, 2, 2, 2)
    fill_box(-2, 0, 0, 1, 2, 1)
    fill_box(1, 0, 0, 1, 2, 1)
    # Boots
    fill_box(-2, 0, -1, 1, 1, 2)
    fill_box(1, 0, -1, 1, 1, 2)

    # Remap back-of-head voxels in eye columns to skin color.
    uv_fix = {}
    for gz in range(0, 2):
        uv_fix[(-1, 14, gz)] = (0, 14)
        uv_fix[(1, 14, gz)]  = (0, 14)
    # Remap cape voxels at face gy levels to avoid picking up face texture.
    for gy in range(4, 11):
        uv_fix[(-1, gy, 2)] = (0, 8)   # leather armor pixel
        uv_fix[(0, gy, 2)]  = (0, 8)
        uv_fix[(1, gy, 2)]  = (0, 8)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz), uv_overrides=uv_fix)
    return mb


def gen_paladin(height=1.85):
    """Male paladin NPC. Heavy plate armor, broad build, flat-top helm.

    Origin at feet (Y=0). Stockier and wider than the cleric — full plate
    coverage with large pauldrons and thick armored limbs. No hair visible.
    """
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # Head (same width as cleric — 5 wide)
    fill_box(-2, 13, -2, 5, 3, 4)
    fill_box(-1, 12, -1, 3, 1, 3)  # chin
    # Eye sockets (front face only — visor opening)
    filled.discard((-1, 14, -2))
    filled.discard((1, 14, -2))
    # Mouth/chin slot
    filled.discard((0, 12, -2))

    # Flat-top great helm — full coverage, no hair
    fill_box(-2, 15, -2, 5, 2, 4)  # helm main block (gy=15-16)
    # Visor slit: remove front row at gy=15 to create a narrow eye opening
    for gx in range(-2, 3):
        filled.discard((gx, 15, -2))

    # Wide torso — heavier plate than cleric (gx -3 to 3, 7 wide)
    fill_box(-3, 5, -1, 7, 6, 3)
    # Large pauldrons (shoulder armor — gx -4 with w=2, gx 3 with w=2)
    fill_box(-4, 9, -1, 2, 2, 3)   # left pauldron
    fill_box(3, 9, -1, 2, 2, 3)    # right pauldron

    # Belt/tassets row
    fill_box(-3, 4, -1, 7, 1, 3)

    # Thick armored arms (2 voxels each side: gx=-4,-5 left, gx=4,5 right,
    # but pauldrons only go to gx=-4 so arms are at gx=-4 only — keep
    # consistent with pauldron width; arms attach below pauldron at gy=7-8)
    fill_box(-4, 7, 0, 1, 2, 1)    # left upper arm
    fill_box(4, 7, 0, 1, 2, 1)     # right upper arm
    fill_box(-4, 4, 0, 1, 3, 1)    # left lower arm
    fill_box(4, 4, 0, 1, 3, 1)     # right lower arm
    # Gauntlets
    fill_box(-4, 3, -1, 1, 1, 2)
    fill_box(4, 3, -1, 1, 1, 2)

    # Thick armored legs
    fill_box(-2, 2, -1, 2, 2, 2)   # left thigh
    fill_box(1, 2, -1, 2, 2, 2)    # right thigh
    fill_box(-2, 0, -1, 2, 2, 2)   # left greave
    fill_box(1, 0, -1, 2, 2, 2)    # right greave
    # Armored sabatons (boots with extra front coverage)
    fill_box(-2, 0, -2, 2, 1, 3)
    fill_box(1, 0, -2, 2, 1, 3)

    # Remap back-of-head voxels at eye-column gy=14 to skin color so eye
    # glow doesn't bleed to the rear helmet faces.  gz=-1 (eyeball behind
    # the visor slot) keeps the eye color; gz=0 and gz=1 are remapped.
    uv_fix = {}
    for gz in range(0, 2):      # gz=0 and gz=1 (behind front face)
        uv_fix[(-1, 14, gz)] = (0, 14)   # center column = skin/neutral pixel
        uv_fix[(1, 14, gz)]  = (0, 14)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz), uv_overrides=uv_fix)
    return mb


def gen_staff(height=1.2):
    """Staff weapon mesh — tall thin rod with crystal tip.

    Origin at base (Y=0). About 1.2 units tall, 0.06 wide shaft with a
    slightly larger crystal orb at the top.
    """
    mb = MeshBuilder()
    # Shaft — thin rod for most of the height
    add_box(mb, center=(0.0, height * 0.85 * 0.5, 0.0),
            half_extents=(0.03, height * 0.85 * 0.5, 0.03))
    # Crystal/orb on top — slightly larger box
    add_box(mb, center=(0.0, height * 0.82 + 0.09, 0.0),
            half_extents=(0.06, 0.09, 0.06))
    return mb


def gen_turret(height=0.4):
    """Combat engineer sentry turret — TF2 level-1 style.

    Tripod base, squat body housing, single barrel on top.
    Origin at feet (Y=0). Compact: about 0.4 units tall.
    """
    mb = MeshBuilder()
    # Tripod base — three stubby legs splayed out
    import math as _m
    for i in range(3):
        angle = i * 2.0 * _m.pi / 3.0
        lx = _m.sin(angle) * 0.12
        lz = _m.cos(angle) * 0.12
        add_box(mb, center=(lx, 0.02, lz),
                half_extents=(0.025, 0.02, 0.025))
    # Central column — short vertical post connecting base to housing
    add_box(mb, center=(0.0, 0.08, 0.0),
            half_extents=(0.04, 0.04, 0.04))
    # Housing body — wider squat box (the "head" of the sentry)
    add_box(mb, center=(0.0, 0.18, 0.0),
            half_extents=(0.08, 0.06, 0.06))
    # Barrel — thin box extending forward from the housing
    add_box(mb, center=(0.0, 0.20, -0.14),
            half_extents=(0.02, 0.02, 0.08))
    return mb


def gen_gargoyle(height=1.6):
    """Gargoyle — stone ambush enemy. Hunched humanoid with stubby wing plates.
    Origin at feet (Y=0). 14 voxels tall, wider and stockier than skeleton."""
    mb = MeshBuilder()
    vs = height / 14.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Head (hunched forward)
    fill_box(-1, 11, -1, 3, 3, 3)
    # Neck
    fill_box(0, 10, 0, 1, 1, 1)
    # Torso (wide, stocky)
    fill_box(-2, 6, -1, 5, 4, 3)
    # Wing plates (stubby, on back)
    fill_box(-3, 8, 1, 2, 3, 2)
    fill_box(2, 8, 1, 2, 3, 2)
    # Arms (thick)
    fill_box(-3, 5, 0, 1, 4, 1)
    fill_box(3, 5, 0, 1, 4, 1)
    fill_box(-4, 4, -1, 1, 3, 1)
    fill_box(4, 4, -1, 1, 3, 1)
    # Hands/claws
    fill_box(-4, 3, -1, 1, 1, 2)
    fill_box(4, 3, -1, 1, 1, 2)
    # Pelvis
    fill_box(-1, 4, 0, 3, 2, 2)
    # Legs (thick)
    fill_box(-2, 2, 0, 2, 3, 1)
    fill_box(1, 2, 0, 2, 3, 1)
    # Feet
    fill_box(-2, 0, -1, 2, 2, 2)
    fill_box(1, 0, -1, 2, 2, 2)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_necromancer(height=2.0):
    """Necromancer — tall hooded skeleton mage. Robed, thin arms.
    Origin at feet (Y=0). 18 voxels tall."""
    mb = MeshBuilder()
    vs = height / 18.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Hood (pointed)
    fill_box(0, 17, 0, 1, 1, 1)
    fill_box(-1, 15, -1, 3, 2, 3)
    # Head inside hood
    fill_box(-1, 13, -1, 3, 2, 2)
    # Neck
    fill_box(0, 12, 0, 1, 1, 1)
    # Shoulders
    fill_box(-3, 11, 0, 7, 1, 2)
    # Upper robe (narrow)
    fill_box(-2, 9, -1, 5, 2, 3)
    # Mid robe (widening)
    fill_box(-2, 6, -1, 5, 3, 3)
    # Lower robe (widest)
    fill_box(-3, 2, -1, 7, 4, 4)
    # Robe hem
    fill_box(-3, 0, -2, 7, 2, 5)
    # Arms (thin, holding staff position)
    fill_box(-3, 8, 0, 1, 3, 1)
    fill_box(3, 8, 0, 1, 3, 1)
    fill_box(-4, 6, -1, 1, 2, 1)
    fill_box(4, 6, -1, 1, 2, 1)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_shaman(height=1.6):
    """Shaman — stocky healer with horned headdress. Broad shoulders.
    Origin at feet (Y=0). 16 voxels tall."""
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Horns
    fill_box(-2, 15, 0, 1, 1, 1)
    fill_box(2, 15, 0, 1, 1, 1)
    # Headdress (wide crown)
    fill_box(-2, 14, -1, 5, 1, 3)
    # Head
    fill_box(-1, 12, -1, 3, 2, 3)
    # Neck
    fill_box(0, 11, 0, 1, 1, 1)
    # Broad shoulders
    fill_box(-3, 9, -1, 7, 2, 3)
    # Torso (wide)
    fill_box(-2, 6, -1, 5, 3, 3)
    # Pelvis
    fill_box(-2, 5, 0, 5, 1, 2)
    # Arms
    fill_box(-3, 6, 0, 1, 4, 1)
    fill_box(3, 6, 0, 1, 4, 1)
    # Hands
    fill_box(-3, 5, -1, 1, 1, 1)
    fill_box(3, 5, -1, 1, 1, 1)
    # Legs (short, thick)
    fill_box(-2, 2, 0, 2, 3, 1)
    fill_box(1, 2, 0, 2, 3, 1)
    # Feet
    fill_box(-2, 0, -1, 2, 2, 2)
    fill_box(1, 0, -1, 2, 2, 2)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_herald(height=2.2):
    """Herald — tall thin skeleton with open rib cage and extended arms.
    Aura-caster enemy. Origin at feet (Y=0). 20 voxels tall."""
    mb = MeshBuilder()
    vs = height / 20.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Skull (angular)
    fill_box(-1, 17, -1, 3, 3, 3)
    # Jaw
    fill_box(-1, 16, -1, 3, 1, 2)
    # Neck (thin)
    fill_box(0, 15, 0, 1, 1, 1)
    # Shoulders (wide, thin)
    fill_box(-4, 14, 0, 9, 1, 2)
    # Upper chest (ribs, open center)
    fill_box(-3, 12, -1, 2, 2, 3)
    fill_box(2, 12, -1, 2, 2, 3)
    # Spine (visible through open chest)
    fill_box(0, 10, 1, 1, 4, 1)
    # Lower ribs
    fill_box(-2, 10, -1, 2, 2, 3)
    fill_box(1, 10, -1, 2, 2, 3)
    # Pelvis
    fill_box(-1, 8, 0, 3, 2, 2)
    # Arms (long, extended outward)
    fill_box(-4, 11, 0, 1, 3, 1)
    fill_box(4, 11, 0, 1, 3, 1)
    fill_box(-5, 9, -1, 1, 3, 1)
    fill_box(5, 9, -1, 1, 3, 1)
    # Clawed hands
    fill_box(-5, 8, -1, 1, 1, 2)
    fill_box(5, 8, -1, 1, 1, 2)
    # Legs (thin)
    fill_box(-1, 4, 0, 1, 4, 1)
    fill_box(1, 4, 0, 1, 4, 1)
    # Feet
    fill_box(-1, 0, -1, 1, 4, 2)
    fill_box(1, 0, -1, 1, 4, 2)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


def gen_sword(length=0.8):
    """Sword weapon — elongated blade + handle.

    Blade extends upward along Y, grip below origin.
    """
    mb = MeshBuilder()
    # Blade — thin wide box extending upward
    add_box(mb, center=(0.0, 0.15, 0.0),
            half_extents=(0.025, 0.3, 0.01))
    # Grip — slightly narrower below blade
    add_box(mb, center=(0.0, -0.2, 0.0),
            half_extents=(0.02, 0.075, 0.02))
    return mb


def gen_dagger(length=0.4):
    """Dagger weapon — short blade + crossguard.

    Shorter and thinner than sword, compact grip.
    """
    mb = MeshBuilder()
    # Blade — shorter than sword
    add_box(mb, center=(0.0, 0.05, 0.0),
            half_extents=(0.02, 0.15, 0.01))
    # Grip
    add_box(mb, center=(0.0, -0.18, 0.0),
            half_extents=(0.0175, 0.06, 0.0175))
    return mb


def gen_axe(length=0.7):
    """Axe weapon — asymmetric axe blade on shaft.

    Wide blade head on left side, butt on right, long haft downward.
    """
    mb = MeshBuilder()
    # Blade head — wide, on left side
    add_box(mb, center=(-0.06, 0.225, 0.0),
            half_extents=(0.06, 0.075, 0.015))
    # Butt — small counterweight on right
    add_box(mb, center=(0.02, 0.22, 0.0),
            half_extents=(0.02, 0.04, 0.02))
    # Haft — long vertical shaft
    add_box(mb, center=(0.0, -0.06, 0.0),
            half_extents=(0.015, 0.24, 0.015))
    return mb


def gen_claymore(length=1.2):
    """Claymore weapon — large two-handed sword.

    Longer blade than sword, wide crossguard, extended grip.
    """
    mb = MeshBuilder()
    # Blade — long and slightly wider than sword
    add_box(mb, center=(0.0, 0.25, 0.0),
            half_extents=(0.03, 0.45, 0.012))
    # Crossguard — wide horizontal bar
    add_box(mb, center=(0.0, -0.05, 0.0),
            half_extents=(0.08, 0.015, 0.015))
    # Grip — long two-handed grip
    add_box(mb, center=(0.0, -0.25, 0.0),
            half_extents=(0.02, 0.12, 0.02))
    return mb


def gen_pistol(length=0.25):
    """Pistol weapon — L-shaped handgun.

    Barrel extends forward along Z, grip hangs below.
    """
    mb = MeshBuilder()
    # Barrel/frame — extends forward along Z
    add_box(mb, center=(0.0, 0.02, 0.05),
            half_extents=(0.02, 0.02, 0.1))
    # Grip — hangs below, angled back
    add_box(mb, center=(0.0, -0.06, -0.05),
            half_extents=(0.02, 0.06, 0.015))
    return mb


def gen_smg(length=0.35):
    """SMG weapon — compact automatic with magazine.

    Longer barrel than pistol, box magazine underneath.
    """
    mb = MeshBuilder()
    # Body/barrel
    add_box(mb, center=(0.0, 0.025, 0.1),
            half_extents=(0.02, 0.025, 0.15))
    # Grip
    add_box(mb, center=(0.0, -0.06, -0.02),
            half_extents=(0.02, 0.06, 0.015))
    # Magazine — box below barrel
    add_box(mb, center=(0.0, -0.04, 0.02),
            half_extents=(0.015, 0.04, 0.01))
    return mb


def gen_carbine(length=0.6):
    """Carbine weapon — long-barreled rifle with stock.

    Extended barrel, buttstock behind, grip below.
    """
    mb = MeshBuilder()
    # Barrel/receiver — long
    add_box(mb, center=(0.0, 0.02, 0.15),
            half_extents=(0.0175, 0.02, 0.225))
    # Stock — extends behind
    add_box(mb, center=(0.0, 0.0, -0.15),
            half_extents=(0.02, 0.025, 0.075))
    # Grip
    add_box(mb, center=(0.0, -0.05, -0.02),
            half_extents=(0.0175, 0.05, 0.015))
    return mb


def gen_revolver(length=0.3):
    """Revolver weapon — barrel + cylinder drum + grip.

    Distinctive cylinder block behind the barrel.
    """
    mb = MeshBuilder()
    # Barrel
    add_box(mb, center=(0.0, 0.02, 0.05),
            half_extents=(0.02, 0.02, 0.075))
    # Cylinder drum — wider block
    add_box(mb, center=(0.0, 0.02, -0.01),
            half_extents=(0.03, 0.03, 0.03))
    # Grip
    add_box(mb, center=(0.0, -0.065, -0.04),
            half_extents=(0.02, 0.065, 0.015))
    return mb


def gen_bow(height=0.8):
    """Bow weapon — curved arc with grip.

    Central grip block, two limbs extending up and down (offset left).
    """
    mb = MeshBuilder()
    # Grip — small central block
    add_box(mb, center=(0.0, 0.0, 0.0),
            half_extents=(0.015, 0.03, 0.015))
    # Upper limb — thin, offset left
    add_box(mb, center=(-0.03, 0.13, 0.0),
            half_extents=(0.01, 0.1, 0.01))
    # Lower limb — thin, offset left
    add_box(mb, center=(-0.03, -0.13, 0.0),
            half_extents=(0.01, 0.1, 0.01))
    return mb


def gen_crossbow(width=0.5):
    """Crossbow weapon — stock with horizontal prod arms.

    Forward-pointing stock, prod arms at the front, bolt channel on top, grip below.
    """
    mb = MeshBuilder()
    # Stock — extends along Z
    add_box(mb, center=(0.0, 0.0, 0.04),
            half_extents=(0.015, 0.015, 0.14))
    # Left prod arm
    add_box(mb, center=(-0.07, 0.02, 0.14),
            half_extents=(0.05, 0.01, 0.02))
    # Right prod arm
    add_box(mb, center=(0.07, 0.02, 0.14),
            half_extents=(0.05, 0.01, 0.02))
    # Bolt channel — thin rail on top
    add_box(mb, center=(0.0, 0.02, 0.06),
            half_extents=(0.008, 0.005, 0.08))
    # Grip
    add_box(mb, center=(0.0, -0.0475, -0.04),
            half_extents=(0.02, 0.0325, 0.02))
    return mb


def gen_throwing_knife(length=0.25):
    """Throwing knife — very thin flat blade + small pommel.

    Extremely thin profile (Z), elongated along Y.
    """
    mb = MeshBuilder()
    # Blade — very thin
    add_box(mb, center=(0.0, 0.03, 0.0),
            half_extents=(0.01, 0.075, 0.0025))
    # Pommel/grip
    add_box(mb, center=(0.0, -0.08, 0.0),
            half_extents=(0.0075, 0.04, 0.0075))
    return mb


def gen_molotov(height=0.25):
    """Molotov cocktail — bottle body + neck.

    Wider bottle body below, narrow neck above.
    """
    mb = MeshBuilder()
    # Bottle body
    add_box(mb, center=(0.0, -0.01, 0.0),
            half_extents=(0.03, 0.06, 0.03))
    # Neck
    add_box(mb, center=(0.0, 0.09, 0.0),
            half_extents=(0.015, 0.03, 0.015))
    return mb


def gen_wand(length=0.35):
    """Wand weapon — thin shaft with crystal tip and pommel.

    Long shaft, gem/crystal at top, small pommel at bottom.
    """
    mb = MeshBuilder()
    # Shaft — long thin rod
    add_box(mb, center=(0.0, -0.025, 0.0),
            half_extents=(0.015, 0.225, 0.015))
    # Crystal tip — wider at top
    add_box(mb, center=(0.0, 0.25, 0.0),
            half_extents=(0.025, 0.05, 0.025))
    # Pommel — small at bottom
    add_box(mb, center=(0.0, -0.25, 0.0),
            half_extents=(0.02, 0.03, 0.02))
    return mb


def gen_web(size=1.0):
    """Flat spider web decoration — thin diamond in XZ plane."""
    mb = MeshBuilder()
    hs = size * 0.5
    add_box(mb, center=(0.0, 0.0, 0.0), half_extents=(hs, 0.02, hs))
    return mb


def gen_shackles(height=0.8):
    """Wall shackles — bar with two hanging chain links and cuffs."""
    mb = MeshBuilder()
    # Horizontal bar
    add_box(mb, center=(0.0, height, 0.0), half_extents=(0.15, 0.015, 0.015))
    # Left/right chain links
    add_box(mb, center=(-0.08, height * 0.7, 0.0), half_extents=(0.01, height * 0.15, 0.01))
    add_box(mb, center=( 0.08, height * 0.7, 0.0), half_extents=(0.01, height * 0.15, 0.01))
    # Left/right cuffs
    add_box(mb, center=(-0.08, height * 0.5, 0.0), half_extents=(0.025, 0.015, 0.02))
    add_box(mb, center=( 0.08, height * 0.5, 0.0), half_extents=(0.025, 0.015, 0.02))
    return mb


def gen_barrel(radius=0.25, height=0.7):
    """Simple barrel — octagonal cylinder with rim bands."""
    mb = MeshBuilder()
    add_cylinder(mb, base_center=(0, 0, 0), radius=radius, height=height, sides=8)
    add_cylinder(mb, base_center=(0, height * 0.85, 0), radius=radius * 1.05, height=height * 0.05, sides=8)
    add_cylinder(mb, base_center=(0, height * 0.05, 0), radius=radius * 1.05, height=height * 0.05, sides=8)
    return mb


def gen_cage(width=0.8, height=1.5):
    """Hanging cage with vertical bars — semi-transparent look."""
    mb = MeshBuilder()
    hw = width * 0.5
    barR = 0.015
    # Top and bottom rings
    add_box(mb, center=(0.0, height, 0.0), half_extents=(hw, 0.02, hw))
    add_box(mb, center=(0.0, 0.0, 0.0), half_extents=(hw, 0.02, hw))
    # 8 vertical bars around perimeter
    for i in range(8):
        angle = i * math.pi * 2.0 / 8.0
        bx = math.sin(angle) * (hw - barR)
        bz = math.cos(angle) * (hw - barR)
        add_box(mb, center=(bx, height * 0.5, bz), half_extents=(barR, height * 0.5, barR))
    # Chain link hanging from top
    add_box(mb, center=(0.0, height + 0.15, 0.0), half_extents=(0.01, 0.15, 0.01))
    return mb


def gen_bones(spread=0.4):
    """Pile of bones on the ground."""
    mb = MeshBuilder()
    add_box(mb, center=(-0.1, 0.02, 0.0), half_extents=(0.15, 0.02, 0.02))
    add_box(mb, center=(0.05, 0.02, 0.08), half_extents=(0.02, 0.02, 0.12))
    add_box(mb, center=(0.0, 0.04, -0.05), half_extents=(0.12, 0.02, 0.02))
    # Skull
    add_box(mb, center=(0.08, 0.06, 0.0), half_extents=(0.04, 0.04, 0.04))
    return mb


def gen_brazier(height=0.6):
    """Standing brazier/fire bowl on a thin post."""
    mb = MeshBuilder()
    add_cylinder(mb, base_center=(0, 0, 0), radius=0.04, height=height * 0.75, sides=6)
    add_cylinder(mb, base_center=(0, 0, 0), radius=0.12, height=0.04, sides=6)
    add_cylinder(mb, base_center=(0, height * 0.7, 0), radius=0.1, height=height * 0.2, sides=6)
    return mb


def gen_butcher(height=2.5):
    """Large demon boss — The Butcher. Massive build, horns, red-tinted.

    Origin at feet (Y=0). Much larger than normal humanoids.
    """
    mb = MeshBuilder()
    vs = height / 20.0  # 20 voxels tall for more detail at large scale
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Massive head ---
    fill_box(-3, 16, -3, 6, 4, 5)
    # Jaw (wide)
    fill_box(-2, 15, -3, 4, 1, 4)
    # Deep eye sockets
    filled.discard((-2, 18, -3))
    filled.discard((1, 18, -3))
    filled.discard((-2, 18, -2))
    filled.discard((1, 18, -2))
    # Mouth snarl
    filled.discard((-1, 15, -3))
    filled.discard((0, 15, -3))

    # Horns (curving up and out)
    fill_box(-4, 19, -1, 1, 2, 1)  # left horn base
    filled.add((-5, 20, -1))        # left horn tip
    fill_box(3, 19, -1, 1, 2, 1)   # right horn base
    filled.add((4, 20, -1))         # right horn tip

    # Neck (thick)
    fill_box(-1, 14, -1, 3, 2, 2)

    # --- Massive torso ---
    fill_box(-4, 8, -2, 8, 6, 4)
    # Barrel chest protrusion
    fill_box(-3, 10, -3, 6, 3, 1)
    # Huge shoulder mounds
    fill_box(-5, 12, -1, 1, 2, 3)
    fill_box(4, 12, -1, 1, 2, 3)

    # Belt / waist
    fill_box(-4, 7, -2, 8, 1, 4)

    # --- Thick arms ---
    fill_box(-5, 9, 0, 1, 4, 1)   # left upper arm
    fill_box(4, 9, 0, 1, 4, 1)    # right upper arm
    fill_box(-5, 5, 0, 1, 4, 1)   # left lower arm
    fill_box(4, 5, 0, 1, 4, 1)    # right lower arm
    # Big fists
    fill_box(-6, 4, -1, 2, 2, 2)
    fill_box(4, 4, -1, 2, 2, 2)

    # --- Powerful legs ---
    fill_box(-3, 3, -1, 3, 4, 3)  # left thigh
    fill_box(1, 3, -1, 3, 4, 3)   # right thigh
    fill_box(-3, 0, -1, 3, 3, 3)  # left shin
    fill_box(1, 0, -1, 3, 3, 3)   # right shin
    # Hooves
    fill_box(-3, 0, -2, 3, 1, 4)
    fill_box(1, 0, -2, 3, 1, 4)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb


# ---------------------------------------------------------------------------
# Type registry
# ---------------------------------------------------------------------------

def gen_skeleton_arm(height=1.8):
    """Skeleton arm — bony arm with fist. Origin near elbow."""
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Shoulder ball
    fill_box(0, 5, 0, 1, 1, 1)
    # Upper arm (2 voxels tall)
    fill_box(0, 3, 0, 1, 2, 1)
    # Lower arm (3 voxels tall)
    fill_box(0, 0, 0, 1, 3, 1)
    # Fist — compact cube, not flat like a foot
    fill_box(0, -1, 0, 1, 1, 1)
    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_skeleton_leg(height=1.8):
    """Skeleton leg — extracted from gen_humanoid. Origin at hip (top).
    Includes thigh, shin, and foot voxels."""
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Thigh (2 voxels tall)
    fill_box(0, 2, 0, 1, 2, 1)
    # Shin (2 voxels tall)
    fill_box(0, 0, 0, 1, 2, 1)
    # Foot
    fill_box(0, 0, -1, 1, 1, 2)
    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_bat_wing(wingspan=1.0):
    """Bat wing — wider membrane shape with multiple voxels for texture detail."""
    mb = MeshBuilder()
    vs = wingspan / 8.0  # 8 voxels wide
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Wing membrane — tapered shape, wider at base narrowing to tip
    fill_box(0, 0, 0, 8, 1, 4)   # main membrane (8 wide, 1 tall, 4 deep)
    fill_box(1, 0, 4, 5, 1, 2)   # inner extension
    fill_box(2, 0, -1, 4, 1, 1)  # front edge
    # Wing finger bones (ridges along the membrane)
    fill_box(0, 1, 1, 1, 1, 1)   # base bone
    fill_box(3, 1, 2, 1, 1, 1)   # mid bone
    fill_box(6, 1, 1, 1, 1, 1)   # tip bone
    ox = -0.5 * vs
    oz = -2.5 * vs  # center depth-wise
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_butcher_arm(height=2.5):
    """Butcher thick demon arm — extracted from gen_butcher. Origin at shoulder."""
    mb = MeshBuilder()
    vs = height / 20.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Upper arm (4 voxels tall, 1 wide)
    fill_box(0, 4, 0, 1, 4, 1)
    # Lower arm (4 voxels tall)
    fill_box(0, 0, 0, 1, 4, 1)
    # Big fist (2x2x2)
    fill_box(-1, -1, -1, 2, 2, 2)
    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_butcher_leg(height=2.5):
    """Butcher thick demon leg — extracted from gen_butcher. Origin at hip."""
    mb = MeshBuilder()
    vs = height / 20.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Thigh (3x4x3)
    fill_box(-1, 3, -1, 3, 4, 3)
    # Shin (3x3x3)
    fill_box(-1, 0, -1, 3, 3, 3)
    # Hoof
    fill_box(-1, 0, -2, 3, 1, 4)
    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_bat_foot():
    """Bat foot/talon — small curved claw shape."""
    mb = MeshBuilder()
    vs = 0.05  # small voxel size
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Ankle joint
    fill_box(0, 2, 0, 1, 1, 1)
    # Three toes spreading forward
    fill_box(-1, 0, -1, 1, 2, 1)  # left toe
    fill_box(0, 0, -2, 1, 2, 1)   # center toe
    fill_box(1, 0, -1, 1, 2, 1)   # right toe
    # Back claw
    fill_box(0, 0, 1, 1, 1, 1)
    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_andariel(height=2.0):
    """Andariel — spider-woman demon boss. Tall, narrow waist, wide chitin shoulders,
    elongated clawed arms, multiple eye sockets. More monster than human.
    Origin at feet (Y=0). Uses 18 voxels tall for detail."""
    mb = MeshBuilder()
    vs = height / 18.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Head (angular, predatory) ---
    fill_box(-2, 15, -2, 5, 3, 4)   # main skull
    fill_box(-1, 14, -2, 3, 1, 3)   # narrow jaw with fangs
    # 4 eye sockets (front face)
    filled.discard((-1, 16, -2))     # upper left eye
    filled.discard((1, 16, -2))      # upper right eye
    filled.discard((-1, 15, -2))     # lower left eye
    filled.discard((1, 15, -2))      # lower right eye
    # Mouth/fang gap
    filled.discard((0, 14, -2))

    # --- Neck (thin) ---
    fill_box(0, 13, 0, 1, 1, 1)

    # --- Upper torso — wide chitin carapace ---
    fill_box(-3, 10, -1, 7, 3, 3)   # broad chest plate
    # Spiked shoulder pauldrons
    fill_box(-4, 11, -1, 1, 2, 3)   # left shoulder spike
    fill_box(4, 11, -1, 1, 2, 3)    # right shoulder spike
    fill_box(-5, 12, 0, 1, 1, 1)    # left spike tip
    fill_box(5, 12, 0, 1, 1, 1)     # right spike tip

    # --- Narrow waist (spider-like constriction) ---
    fill_box(-1, 8, 0, 3, 2, 2)     # thin waist

    # --- Wide abdomen (spider-like) ---
    fill_box(-3, 5, -1, 7, 3, 4)    # bulbous lower body
    fill_box(-2, 4, -1, 5, 1, 3)    # taper

    # --- Long clawed arms ---
    fill_box(-4, 9, 0, 1, 3, 1)     # left upper arm
    fill_box(4, 9, 0, 1, 3, 1)      # right upper arm
    fill_box(-5, 6, 0, 1, 3, 1)     # left lower arm (longer, angled out)
    fill_box(5, 6, 0, 1, 3, 1)      # right lower arm
    # Clawed hands
    fill_box(-5, 5, -1, 1, 1, 1)    # left claw
    fill_box(5, 5, -1, 1, 1, 1)     # right claw

    # --- Legs (thin, insectoid) ---
    fill_box(-2, 2, 0, 1, 2, 1)     # left thigh
    fill_box(2, 2, 0, 1, 2, 1)      # right thigh
    fill_box(-2, 0, 0, 1, 2, 1)     # left shin
    fill_box(2, 0, 0, 1, 2, 1)      # right shin
    # Pointed feet
    fill_box(-2, 0, -1, 1, 1, 2)
    fill_box(2, 0, -1, 1, 1, 2)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_humanoid_torso(height=1.8):
    """Skeleton torso only — arms and legs removed for limb system animation."""
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Skull
    fill_box(-2, 12, -2, 5, 4, 4)
    fill_box(-1, 11, -2, 3, 1, 3)
    filled.discard((-1, 14, -2)); filled.discard((1, 14, -2))
    filled.discard((0, 13, -2)); filled.discard((0, 11, -2))
    # Neck
    fill_box(0, 10, 0, 1, 1, 1)
    # Rib cage
    fill_box(0, 5, 1, 1, 5, 1)
    for ry in [6, 7, 9]:
        fill_box(-2, ry, 0, 2, 1, 2)
        fill_box(1, ry, 0, 2, 1, 2)
    # Shoulder bones (attachment points for arm limbs)
    fill_box(-3, 9, 0, 1, 1, 1)
    fill_box(3, 9, 0, 1, 1, 1)
    # Pelvis (attachment point for leg limbs)
    fill_box(-1, 4, 0, 3, 1, 1)
    # NO arms, NO legs — limb system provides these
    ox = -0.5 * vs; oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_human_torso(height=1.8):
    """Human NPC torso only — arms and legs removed for limb system animation."""
    mb = MeshBuilder()
    vs = height / 16.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Head
    fill_box(-2, 13, -2, 5, 3, 4)
    fill_box(-1, 12, -1, 3, 1, 3)
    filled.discard((-1, 14, -2)); filled.discard((1, 14, -2))
    filled.discard((0, 12, -2))
    # Neck
    fill_box(0, 11, 0, 1, 1, 1)
    # Torso
    fill_box(-2, 6, -1, 5, 5, 3)
    fill_box(-3, 9, -1, 1, 2, 3)  # left shoulder pad
    fill_box(3, 9, -1, 1, 2, 3)   # right shoulder pad
    # Belt
    fill_box(-2, 5, -1, 5, 1, 3)
    # Hip connectors (slim pelvis for leg attachment)
    fill_box(-2, 4, -1, 5, 1, 2)
    # NO arms, NO legs
    ox = -0.5 * vs; oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

def gen_butcher_torso(height=2.5):
    """Butcher boss torso only — arms and legs removed for limb system animation."""
    mb = MeshBuilder()
    vs = height / 20.0
    filled = set()
    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))
    # Head
    fill_box(-3, 16, -3, 6, 4, 5)
    fill_box(-2, 15, -3, 4, 1, 4)
    filled.discard((-2, 18, -3)); filled.discard((1, 18, -3))
    filled.discard((-2, 18, -2)); filled.discard((1, 18, -2))
    filled.discard((-1, 15, -3)); filled.discard((0, 15, -3))
    # Horns
    fill_box(-4, 19, -1, 1, 2, 1); filled.add((-5, 20, -1))
    fill_box(3, 19, -1, 1, 2, 1); filled.add((4, 20, -1))
    # Neck
    fill_box(-1, 14, -1, 3, 2, 2)
    # Massive torso
    fill_box(-4, 8, -2, 8, 6, 4)
    fill_box(-3, 10, -3, 6, 3, 1)
    # Shoulder mounds (attachment points for arm limbs)
    fill_box(-5, 12, -1, 1, 2, 3)
    fill_box(4, 12, -1, 1, 2, 3)
    # Belt / waist
    fill_box(-4, 7, -2, 8, 1, 4)
    # Hip area (attachment for leg limbs)
    fill_box(-3, 6, -1, 7, 1, 3)
    # NO arms, NO legs, NO fists, NO hooves
    ox = -0.5 * vs; oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb

MESH_TYPES = {
    "humanoid": {
        "func": gen_humanoid,
        "desc": "Barony-style voxel humanoid. Params: --height",
        "default_file": "humanoid.obj",
    },
    "humanoid_torso": {
        "func": gen_humanoid_torso,
        "desc": "Skeleton torso only (no arms/legs). Params: --height",
        "default_file": "skeleton.obj",
    },
    "human": {
        "func": gen_human,
        "desc": "Barony-style voxel human NPC. Params: --height",
        "default_file": "human.obj",
    },
    "human_torso": {
        "func": gen_human_torso,
        "desc": "Human NPC torso only (no arms/legs). Params: --height",
        "default_file": "human.obj",
    },
    "butcher": {
        "func": lambda height=2.5: gen_butcher(height),
        "desc": "Large demon boss with horns. Params: --height",
        "default_file": "butcher.obj",
    },
    "butcher_torso": {
        "func": lambda height=2.5: gen_butcher_torso(height),
        "desc": "Butcher torso only (no arms/legs). Params: --height",
        "default_file": "butcher.obj",
    },
    "skeleton_arm": {
        "func": gen_skeleton_arm,
        "desc": "Skeleton arm (upper + lower + hand). Params: --height",
        "default_file": "skeleton_arm.obj",
    },
    "skeleton_leg": {
        "func": gen_skeleton_leg,
        "desc": "Skeleton leg (thigh + shin + foot). Params: --height",
        "default_file": "skeleton_leg.obj",
    },
    "bat_wing": {
        "func": lambda wingspan=1.0: gen_bat_wing(wingspan),
        "desc": "Bat wing membrane shape. Params: --wingspan",
        "default_file": "bat_wing.obj",
    },
    "butcher_arm": {
        "func": lambda height=2.5: gen_butcher_arm(height),
        "desc": "Butcher thick demon arm. Params: --height",
        "default_file": "butcher_arm.obj",
    },
    "butcher_leg": {
        "func": lambda height=2.5: gen_butcher_leg(height),
        "desc": "Butcher thick demon leg. Params: --height",
        "default_file": "butcher_leg.obj",
    },
    "bat_foot": {
        "func": gen_bat_foot,
        "desc": "Bat foot/talon with three toes.",
        "default_file": "bat_foot.obj",
    },
    "spider_leg_pair": {
        "func": lambda radius=0.6: gen_spider_leg_pair(radius),
        "desc": "Spider leg pair (2 legs, left side). Params: --radius",
        "default_file": "spider_leg_pair.obj",
    },
    "andariel": {
        "func": lambda height=2.0: gen_andariel(height),
        "desc": "Andariel spider-woman boss. Params: --height",
        "default_file": "andariel.obj",
    },
    "cleric": {
        "func": gen_cleric,
        "desc": "Male cleric NPC with hood and robes. Params: --height",
        "default_file": "cleric.obj",
    },
    "archer": {
        "func": gen_archer,
        "desc": "Female archer NPC with ponytail and quiver. Params: --height",
        "default_file": "archer.obj",
    },
    "mage": {
        "func": gen_mage,
        "desc": "Male mage NPC with wizard hat and robes. Params: --height",
        "default_file": "mage.obj",
    },
    "rogue": {
        "func": gen_rogue,
        "desc": "Male rogue NPC with hood and cape. Params: --height",
        "default_file": "rogue.obj",
    },
    "paladin": {
        "func": gen_paladin,
        "desc": "Male paladin NPC with heavy plate armor and helm. Params: --height",
        "default_file": "paladin.obj",
    },
    "staff": {
        "func": gen_staff,
        "desc": "Staff weapon — thin rod with crystal tip. Params: --height",
        "default_file": "staff.obj",
    },
    "web": {
        "func": gen_web,
        "desc": "Flat spider web decoration. Params: --radius (used as size)",
        "default_file": "web.obj",
    },
    "shackles": {
        "func": gen_shackles,
        "desc": "Wall shackles with chains and cuffs. Params: --height",
        "default_file": "shackles.obj",
    },
    "barrel": {
        "func": gen_barrel,
        "desc": "Simple barrel with rim bands. Params: --radius --height",
        "default_file": "barrel.obj",
    },
    "cage": {
        "func": gen_cage,
        "desc": "Hanging cage with vertical bars. Params: --width --height",
        "default_file": "cage.obj",
    },
    "bones": {
        "func": gen_bones,
        "desc": "Pile of bones on the ground.",
        "default_file": "bones.obj",
    },
    "brazier": {
        "func": gen_brazier,
        "desc": "Standing brazier/fire bowl. Params: --height",
        "default_file": "brazier.obj",
    },
    "spider": {
        "func": gen_spider,
        "desc": "Barony-style voxel spider. Params: --radius",
        "default_file": "spider.obj",
    },
    "bat": {
        "func": gen_bat,
        "desc": "Barony-style voxel bat. Params: --wingspan",
        "default_file": "bat.obj",
    },
    "pillar": {
        "func": gen_pillar,
        "desc": "N-sided cylinder pillar. Params: --radius --height --sides",
        "default_file": "pillar.obj",
    },
    "chest": {
        "func": gen_chest,
        "desc": "Simple openable chest (~24 tris). Params: --width",
        "default_file": "chest.obj",
    },
    "turret": {
        "func": gen_turret,
        "desc": "Combat engineer sentry turret — tripod + barrel. Params: --height",
        "default_file": "turret.obj",
    },
    "gargoyle": {
        "func": gen_gargoyle,
        "desc": "Gargoyle ambush enemy — hunched stone humanoid. Params: --height",
        "default_file": "gargoyle.obj",
    },
    "necromancer": {
        "func": gen_necromancer,
        "desc": "Necromancer — tall hooded skeleton mage. Params: --height",
        "default_file": "necromancer.obj",
    },
    "shaman": {
        "func": gen_shaman,
        "desc": "Shaman healer — stocky with horned headdress. Params: --height",
        "default_file": "shaman.obj",
    },
    "herald": {
        "func": gen_herald,
        "desc": "Herald aura enemy — tall thin skeleton. Params: --height",
        "default_file": "herald.obj",
    },
    "sword": {
        "func": gen_sword,
        "desc": "Sword weapon — blade + handle. Params: --height (as length)",
        "default_file": "sword.obj",
    },
    "dagger": {
        "func": gen_dagger,
        "desc": "Dagger weapon — short blade + grip. Params: --height (as length)",
        "default_file": "dagger.obj",
    },
    "axe": {
        "func": gen_axe,
        "desc": "Axe weapon — asymmetric blade on shaft. Params: --height (as length)",
        "default_file": "axe.obj",
    },
    "claymore": {
        "func": gen_claymore,
        "desc": "Claymore weapon — large two-handed sword. Params: --height (as length)",
        "default_file": "claymore.obj",
    },
    "pistol": {
        "func": gen_pistol,
        "desc": "Pistol weapon — L-shaped handgun. Params: --height (as length)",
        "default_file": "pistol.obj",
    },
    "smg": {
        "func": gen_smg,
        "desc": "SMG weapon — compact automatic with magazine. Params: --height (as length)",
        "default_file": "smg.obj",
    },
    "carbine": {
        "func": gen_carbine,
        "desc": "Carbine weapon — long-barreled rifle. Params: --height (as length)",
        "default_file": "carbine.obj",
    },
    "revolver": {
        "func": gen_revolver,
        "desc": "Revolver weapon — barrel + cylinder + grip. Params: --height (as length)",
        "default_file": "revolver.obj",
    },
    "bow": {
        "func": gen_bow,
        "desc": "Bow weapon — curved arc with grip. Params: --height",
        "default_file": "bow.obj",
    },
    "crossbow": {
        "func": gen_crossbow,
        "desc": "Crossbow weapon — stock with horizontal prods. Params: --width",
        "default_file": "crossbow.obj",
    },
    "throwing_knife": {
        "func": gen_throwing_knife,
        "desc": "Throwing knife — thin blade + pommel. Params: --height (as length)",
        "default_file": "throwing_knife.obj",
    },
    "molotov": {
        "func": gen_molotov,
        "desc": "Molotov cocktail — bottle + neck. Params: --height",
        "default_file": "molotov.obj",
    },
    "wand": {
        "func": gen_wand,
        "desc": "Wand weapon — shaft + crystal tip. Params: --height (as length)",
        "default_file": "wand.obj",
    },
}


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Generate low-poly 3D meshes as OBJ files.")

    parser.add_argument("--list-types", action="store_true",
                        help="List available mesh types and exit.")
    parser.add_argument("--type", choices=list(MESH_TYPES.keys()),
                        help="Mesh type to generate.")
    parser.add_argument("--out", type=str, default=None,
                        help="Output OBJ file path.")

    # Shared parameters
    parser.add_argument("--height", type=float, default=None,
                        help="Height (humanoid, pillar).")
    parser.add_argument("--radius", type=float, default=None,
                        help="Radius (spider, pillar).")
    parser.add_argument("--wingspan", type=float, default=None,
                        help="Wingspan (bat).")
    parser.add_argument("--width", type=float, default=None,
                        help="Width (chest).")
    parser.add_argument("--sides", type=int, default=None,
                        help="Number of sides (pillar).")

    args = parser.parse_args()

    if args.list_types:
        print("Available mesh types:")
        for name, info in MESH_TYPES.items():
            print(f"  {name:12s} — {info['desc']}")
        return

    if not args.type:
        parser.error("--type is required (or use --list-types)")

    # Determine output path
    # Default: assets/meshes/ relative to the game root (script's parent dir)
    game_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    default_dir = os.path.join(game_root, "assets", "meshes")

    if args.out:
        out_path = args.out
        # If relative, resolve relative to cwd
        if not os.path.isabs(out_path):
            out_path = os.path.abspath(out_path)
    else:
        out_path = os.path.join(default_dir, MESH_TYPES[args.type]["default_file"])

    # Build kwargs for the generator
    kwargs = {}
    mtype = args.type

    if mtype in ("humanoid", "human", "cleric", "archer", "butcher",
                 "mage", "rogue", "paladin", "staff"):
        if args.height is not None:
            kwargs["height"] = args.height
    elif mtype == "spider":
        if args.radius is not None:
            kwargs["radius"] = args.radius
    elif mtype == "bat":
        if args.wingspan is not None:
            kwargs["wingspan"] = args.wingspan
    elif mtype == "pillar":
        if args.radius is not None:
            kwargs["radius"] = args.radius
        if args.height is not None:
            kwargs["height"] = args.height
        if args.sides is not None:
            kwargs["sides"] = args.sides
    elif mtype == "chest":
        if args.width is not None:
            kwargs["width"] = args.width
    elif mtype in ("shackles", "brazier", "turret", "gargoyle", "necromancer", "shaman", "herald"):
        if args.height is not None:
            kwargs["height"] = args.height
    elif mtype == "web":
        if args.radius is not None:
            kwargs["size"] = args.radius
    elif mtype == "barrel":
        if args.radius is not None:
            kwargs["radius"] = args.radius
        if args.height is not None:
            kwargs["height"] = args.height
    elif mtype == "cage":
        if args.width is not None:
            kwargs["width"] = args.width
        if args.height is not None:
            kwargs["height"] = args.height
    elif mtype in ("sword", "dagger", "axe", "claymore", "pistol",
                   "smg", "carbine", "revolver", "throwing_knife", "wand"):
        if args.height is not None:
            kwargs["length"] = args.height
    elif mtype in ("bow", "molotov"):
        if args.height is not None:
            kwargs["height"] = args.height
    elif mtype == "crossbow":
        if args.width is not None:
            kwargs["width"] = args.width
    # bones and brazier use defaults — no special args needed

    mb = MESH_TYPES[mtype]["func"](**kwargs)
    write_obj(out_path, mb)

    tri_count = len(mb.faces)
    vert_count = len(mb.verts)
    print(f"Wrote {out_path}  ({vert_count} verts, {tri_count} tris)")


if __name__ == "__main__":
    main()
