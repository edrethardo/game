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
        mb.add_tri(bottom_vis[i], bottom_vis[j], top_vis[j],
                   uv00, uv10, uv11, sn, sn, sn)
        mb.add_tri(bottom_vis[i], top_vis[j], top_vis[i],
                   uv00, uv11, uv01, sn, sn, sn)
        mb.add_tri(bot_center, bottom_vis[j], bottom_vis[i],
                   uv_center, uv10, uv00, bottom_n, bottom_n, bottom_n)
        mb.add_tri(top_center, top_vis[i], top_vis[j],
                   uv_center, uv00, uv10, top_n, top_n, top_n)


# ---------------------------------------------------------------------------
# Voxel model builder — Barony-style chunky cube models
# ---------------------------------------------------------------------------

def add_voxel_model(mb, filled, voxel_size, offset=(0, 0, 0)):
    """Build optimized mesh from a set of filled voxel positions.

    filled: set of (gx, gy, gz) integer grid positions that are filled
    voxel_size: world-space size of each cube
    offset: world-space offset applied to all voxels

    Only emits faces at filled/empty boundaries (internal faces culled).
    """
    ox, oy, oz = offset
    hs = voxel_size * 0.5

    # Pre-compute shared UVs
    uv0 = mb.add_uv(0.0, 0.0)
    uv1 = mb.add_uv(1.0, 0.0)
    uv2 = mb.add_uv(1.0, 1.0)
    uv3 = mb.add_uv(0.0, 1.0)

    # 6 face definitions: (dx,dy,dz), normal, 4 corner offsets from cell center
    face_defs = [
        # dir,     normal,     corners (CCW from outside)
        ((0, 1, 0),  (0, 1, 0),  [(-1,1,-1), (1,1,-1), (1,1,1), (-1,1,1)]),     # +Y top
        ((0,-1, 0),  (0,-1, 0),  [(-1,-1,1), (1,-1,1), (1,-1,-1), (-1,-1,-1)]),  # -Y bottom
        ((1, 0, 0),  (1, 0, 0),  [(1,-1,-1), (1,1,-1), (1,1,1), (1,-1,1)]),      # +X right
        ((-1,0, 0),  (-1,0, 0),  [(-1,-1,1), (-1,1,1), (-1,1,-1), (-1,-1,-1)]),  # -X left
        ((0, 0, 1),  (0, 0, 1),  [(-1,-1,1), (-1,1,1), (1,1,1), (1,-1,1)]),      # +Z front
        ((0, 0,-1),  (0, 0,-1),  [(1,-1,-1), (1,1,-1), (-1,1,-1), (-1,-1,-1)]),   # -Z back
    ]

    # Cache normals
    normal_cache = {}
    for _, norm, _ in face_defs:
        if norm not in normal_cache:
            normal_cache[norm] = mb.add_normal(*norm)

    for (gx, gy, gz) in filled:
        cx = ox + gx * voxel_size + hs
        cy = oy + gy * voxel_size + hs
        cz = oz + gz * voxel_size + hs

        for (dx, dy, dz), norm, corners in face_defs:
            neighbor = (gx + dx, gy + dy, gz + dz)
            if neighbor in filled:
                continue  # internal face — skip

            ni = normal_cache[norm]
            vis = []
            for (sx, sy, sz) in corners:
                vis.append(mb.add_vert(cx + sx * hs, cy + sy * hs, cz + sz * hs))

            mb.add_tri(vis[0], vis[1], vis[2], uv0, uv1, uv2, ni, ni, ni)
            mb.add_tri(vis[0], vis[2], vis[3], uv0, uv2, uv3, ni, ni, ni)


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
    # Hollow out eye sockets — deep tunnels through the skull (2 voxels deep)
    filled.discard((-1, 14, -2))
    filled.discard((-1, 14, -1))
    filled.discard((1, 14, -2))
    filled.discard((1, 14, -1))
    # Hollow out nose — goes through 2 layers
    filled.discard((0, 13, -2))
    filled.discard((0, 13, -1))
    # Mouth opening — wide gap in the jaw
    filled.discard((-1, 11, -2))
    filled.discard((0, 11, -2))
    filled.discard((1, 11, -2))

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

    Origin at center-bottom.
    """
    mb = MeshBuilder()
    vs = radius / 5.0  # voxel size — finer grid for more detail
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # --- Fat abdomen (rear) — bulbous 6x4x6 ---
    fill_box(-3, 1, 1, 6, 4, 6)
    # Round top corners
    for corner in [(-3,4,1),(-3,4,6),(2,4,1),(2,4,6),
                   (-3,1,1),(-3,1,6),(2,1,1),(2,1,6)]:
        filled.discard(corner)
    # Add a rounded bump on top
    fill_box(-2, 5, 2, 4, 1, 4)

    # --- Thorax (middle) — 4x3x4 ---
    fill_box(-2, 1, -3, 4, 3, 4)

    # --- Head — 3x3x3 with eye bumps ---
    fill_box(-1, 1, -6, 3, 3, 3)
    # Eye bumps on top
    filled.add((-1, 4, -5))
    filled.add((1, 4, -5))

    # --- Fangs — 2 voxels hanging down from head front ---
    filled.add((-1, 0, -7))
    filled.add((1, 0, -7))
    filled.add((-1, 0, -6))
    filled.add((1, 0, -6))

    # --- Pedipalps (small feelers) ---
    filled.add((-1, 1, -7))
    filled.add((1, 1, -7))

    # --- 8 legs with knee joints ---
    # 4 per side, attached to thorax
    leg_attach_z = [-2, -1, 0, 1]

    for sz in leg_attach_z:
        # LEFT leg: out, up to knee, then down to ground
        # Upper segment — goes outward and up
        filled.add((-3, 2, sz))
        filled.add((-4, 3, sz))
        filled.add((-5, 4, sz))  # knee (high point)
        # Lower segment — goes down to ground
        filled.add((-6, 3, sz))
        filled.add((-6, 2, sz))
        filled.add((-6, 1, sz))
        filled.add((-7, 0, sz))  # foot

        # RIGHT leg (mirror)
        filled.add((3, 2, sz))
        filled.add((4, 3, sz))
        filled.add((5, 4, sz))
        filled.add((6, 3, sz))
        filled.add((6, 2, sz))
        filled.add((6, 1, sz))
        filled.add((7, 0, sz))

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))

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
    # Hollow out eyes (deep — remove front AND second layer for see-through)
    filled.discard((-1, 8, -1))
    filled.discard((1, 8, -1))
    filled.discard((-1, 8, 0))
    filled.discard((1, 8, 0))
    # Mouth opening
    filled.discard((0, 6, -1))

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

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
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

    # Head (slightly smaller)
    fill_box(-2, 13, -2, 4, 3, 4)
    fill_box(-1, 12, -1, 2, 1, 3)  # chin (narrower)
    # Eyes (green — just the sockets, tint does the color)
    filled.discard((-1, 14, -2))
    filled.discard((1, 14, -2))
    # Mouth
    filled.discard((0, 12, -2))

    # Fox-red/brown ponytail (extends behind head and down)
    fill_box(-1, 15, -2, 3, 2, 4)   # top hair
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

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
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

MESH_TYPES = {
    "humanoid": {
        "func": gen_humanoid,
        "desc": "Barony-style voxel humanoid. Params: --height",
        "default_file": "humanoid.obj",
    },
    "human": {
        "func": gen_human,
        "desc": "Barony-style voxel human NPC. Params: --height",
        "default_file": "human.obj",
    },
    "butcher": {
        "func": lambda height=2.5: gen_butcher(height),
        "desc": "Large demon boss with horns. Params: --height",
        "default_file": "butcher.obj",
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

    if mtype in ("humanoid", "human", "cleric", "archer", "butcher"):
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

    mb = MESH_TYPES[mtype]["func"](**kwargs)
    write_obj(out_path, mb)

    tri_count = len(mb.faces)
    vert_count = len(mb.verts)
    print(f"Wrote {out_path}  ({vert_count} verts, {tri_count} tris)")


if __name__ == "__main__":
    main()
